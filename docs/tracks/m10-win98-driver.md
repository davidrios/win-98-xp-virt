# Track: M10 — the native Win98 display driver (doc 19, ADR-012)

The handoff for a session that gives Win98/Me the driver XP already has:
first the split of the XP driver into an OS-independent core plus a thin
per-OS layer, then the 9x layer on top of that core. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then doc 19, then doc 15 (which is the core's actual
specification — every behaviour the core has to keep is described there).

Branch `track/m10-win98-driver`, worktree
`.claude/worktrees/m10-win98-driver` (opened 2026-09-06, off `main` at
`8a0cfce`; submodules initialised, nothing built there yet).

## Scope and files (this track owns them)

- The guest driver source: `guest-tools/src/d3dptvid/` and
  `guest-tools/build-driver.sh`. **This is the M7 track's tree**; M7's
  work is all on `main` (its branch was fully merged), so there is no
  live conflict, but any M7 session that reopens must rebase on this
  split rather than edit around it.
- The 9x half, which exists now:
  - `guest-tools/src/d3dptvid/w9x/` — `d3dpt9x.c` (the 16-bit DIB Engine
    display driver), `dibthunk.asm` (its drawing exports, all jumps into
    the Engine), `res/` (the `oembin` resource blobs GDI requires),
    `d3dptvxd.c` (the ring-0 mini-VDD), `d3dpt9x.h` / `d3dpt9v.h` (what
    the two halves share), `d3dpt9x.inf`;
  - `guest-tools/src/d3dptvid/ddk9x/` — the vendored 9x interface headers
    (MIT, provenance in its README); `guest-tools/build-driver9x.sh`.
- `guest-tools/src/setup.c` — the display-driver component for the 98/Me
  role — and `tools/setup-guest-test.sh`'s Win98 expectations.
- Tests: `tools/win98-driver-test.sh` (the 9x counterpart of
  `tools/xp-driver-test.sh`), and the Win98 side of whatever guest checks
  land in `scripts/test.sh`. Not wired into `scripts/test.sh` — it needs a
  guest image, like the other guest harnesses.
- Docs: `docs/19-win9x-display-driver.md`, this file, ADR-012 in doc 10,
  the M10 row of the state table and the M10 line of "Next steps" in
  `docs/00-status.md`.
- Shared with other tracks (rebase first, edit minimally, say which track
  in the commit): `d3dpt/d3dpt_fb.h` and `d3dpt/hw/d3dpt_vga.c` (M7's —
  the adapter should need *no* change for 9x; if it does, that is a
  finding worth writing down), `d3dpt/d3dpt_proto.h` and `d3dpt/exec/`
  (M4/M7's — the 9x driver speaks the existing protocol, a bump would
  mean 9x needs something XP does not), `guest-tools/build-wrappers.sh`,
  `scripts/test.sh`, `launcher/` (the Win98 reference machine's `-vga`),
  `CLAUDE.md`.

## State (2026-09-06)

**Steps 0–4 are done: the toolchain, the `.drv`, the mini-VDD, and — since
2026-09-06 — the driver actually running. GDI loads it, it claims the
adapter through the mini-VDD and sets the mode (`d3dpt-vga: linear mode on
(640x480x32 pitch 2560 offset 0)`), its own `d3dpt9x:` lines arrive through
the DEBUG register as the XP driver's do, and the DIB Engine's software
cursor is drawn into guest VRAM at the right place and scale. The open item
is that the shell never appears: a black desktop, the wait cursor, and a
640×20 strip of garbage at the top of the frame buffer, unchanged between
150 s and 200 s of boot (doc 19 §15).** The findings that shape the plan:

- A 9x display driver is **three binaries**: a 16-bit `.drv` whose drawing
  exports all jump to the DIB Engine, a ring-0 mini-VDD `.vxd`, and — the
  surprise — the DirectDraw/Direct3D HAL as a **ring-3 32-bit DLL** loaded
  into the game's own process. Only that last one links our core, and it
  builds with the `i686-w64-mingw32` toolchain we already have.
- **The per-call DDI structures are field-for-field identical to NT's**
  (`D3DHAL_DRAWPRIMITIVES2DATA` = `D3DNTHAL_DRAWPRIMITIVES2DATA`, same
  fourteen fields, same order, `__stdcall` both), and the DP2 opcode
  values agree. The walker is portable as it stands.
- **The DirectDraw object structures are not**: `DDRAWI_DDRAWSURFACE_LCL`
  has the same fields as `DD_SURFACE_LOCAL` under the same names at
  different offsets (and 16-bit `wWidth`/`wHeight`). The neutral
  descriptor is required, and filling it is mechanical.
- **DDI 8 works on 9x** — same `GetDriverInfo2`, same `D3DGDI2_*`, same
  `D3DCAPS8` — so all of M7c is in scope for 98.
- **Caps rules differ**: `DDCAPS_GDI` is normal on 9x and fatal on NT; a
  9x callback may decline and fall back to the HEL, an NT one may not. The
  caps table is per-OS, not shared.
- **Modes come from the INF/registry on 9x**, not from the adapter.
- **Open Watcom is the second toolchain** for the `.drv` and the `.vxd`
  (mingw can make neither format). Settled 2026-09-06: Open Watcom v2
  ships Linux-hosted binaries, unpacked from the Last-CI-build release's
  `ow-snapshot.tar.xz` into `~/.local/opt/open-watcom` — no sudo, nothing
  on the system path, `WATCOM=` points the build at it.
- **Header provenance settled**: `vmdisp9x`'s `ddk/` carries no Microsoft
  code or copyright (interface descriptions written from the published
  documentation, MIT), so they are vendored under
  `guest-tools/src/d3dptvid/ddk9x/` with the notice, the same posture as
  the XP driver's `ddk/`. Nothing OWPL-licensed enters the binary either:
  the driver links no C runtime.
- **A `.drv` must carry `oembin` resources** — `config.bin`,
  `colortab.bin`, `fonts.bin`, `fonts120.bin`: the machine metrics, the
  Control Panel colour table and the three system LOGFONTs live inside
  the driver and GDI needs them (`w9x/res/`).
- **PnP installs it with no clicks**: the INF in `C:\WINDOWS\INF` matches
  `PCI\VEN_1234&DEV_3D00`, installs silently and asks to restart.
  Editing `SYSTEM.INI` by hand instead does not work — Windows rewrites
  the line when it re-detects the adapter.
- **Three silent failures, two now caught by the build**
  (doc 19 §13, §14). GDI refused the `.drv` because wlink dropped an empty
  `_TEXT`/`FAR_DATA` segment — the one the `__based(__segname("_TEXT"))
  *pText` idiom creates — and left a relocation naming it; the VMM refused
  the VxD, again, when a `static const` array was emitted ahead of the DDB;
  and the adapter answered zeros because a 16-bit `*(DWORD __far *)` is
  two word accesses, which `d3dpt-vga`'s register BAR
  (`valid.min_access_size = 4`) drops on the floor. None of the three
  produced a message anywhere.
- **The adapter needed no change for 9x**, which is what this track hoped
  for: the 16-bit half changed instead, to 32-bit register accesses written
  out by hand.

`vmdisp9x` and `vmhal9x` are cloned to `build/ref/` (gitignored) for
reading. Nothing of their *code* is vendored; their `ddk/` headers are,
under `ddk9x/`, for the reason in the bullet above.

**Where a new session starts.** Everything is committed and pushed; the
open item is doc 19 §15 — the driver runs and draws, but Windows never
reaches the desktop. The two leads, in order: the 640×20 strip of garbage
at the top of the frame buffer (nothing the driver draws belongs there, so
whatever writes it is writing where it should not), and what `Enable`
answers with — the `GDIINFO` and the `deviceBitmap` handed to the DIB
Engine, held against `vmdisp9x`'s `enable.c` field by field. Rule out the
dull explanation first: a 32 bpp desktop painted in software under TCG may
simply be slower than the harness waits.

Do not re-derive the three silent refusals; they are written up in doc 19
§13 and §14 and two of them are now build-time checks.

What the track starts from:

- XP's driver is complete through the DX8 DDI and is the thing being
  generalised: miniport + display driver, DirectDraw DDI, Direct3D DDI
  (DX3 execute buffers through DX8 with hardware T&L and vs/ps 1.x),
  protocol v9, all on `main`. Doc 15 and `docs/tracks/m7-display-driver.md`
  have the detail and the hard-won rules (dxg's caps rules, the flip
  role-swap, untracked GDI writes, the cursor register set).
- Win98 today: `-vga cirrus` with the inbox driver, 3D through the
  qemu-3dfx Glide wrappers and WineD3D DLLs per game folder. That path
  stays and is the control to measure against.
- `~/vms/win98.qcow2` is the guest image (ACPI install, TCG only — under
  KVM Explorer dies at startup). `tools/setup-guest-test.sh <image> win98`
  is the existing headless Win98 harness to build on.

## Next steps, in order

1. ~~**Step 0 — establish the 9x driver model**~~ **done 2026-09-06**,
   from `vmdisp9x` and `vmhal9x`; the answers are doc 19's "What 9x does
   differently" and the State section above. One question of that section
   is left for a guest to answer rather than a source tree: whether a
   driver claiming DDI 8 may leave out the pre-DP2 HAL entries
   (`RenderState`, `RenderPrimitive`, `DrawOnePrimitive`, `TextureCreate`)
   that NT dropped and `vmhal9x` still implements.
2. ~~**Get Open Watcom building a "hello world" `.drv`**~~ **done
   2026-09-06**: `guest-tools/build-driver9x.sh` builds `d3dpt9x.drv`
   (module `DISPLAY`, the ordinal exports, `oembin` resources, imports
   `KERNEL` and `DIBENG` only, no CRT), the INF installs it through PnP,
   and `tools/win98-driver-test.sh` runs the whole thing headless. The
   toolchain risk is gone.
3. ~~**The mini-VDD**~~ **done 2026-09-06** (doc 19 §12): `d3dpt9v.vxd`
   loads, claims the adapter, maps VRAM and the register page, checks the
   register set, and installs itself in the main VDD's dispatch table —
   and the BARs now survive the whole boot, which was the §11 blocker.
   Three `wlink` facts had to be found first (LE objects at base 0 and
   executable; the DDB at offset 0 of the *code* object, which needs
   everything in one CODE-class segment; a 32-bit entry-table bundle);
   they are in doc 19 §12 because a VxD the VMM dislikes is simply never
   loaded, silently.
4. ~~**Make GDI load `d3dpt9x.drv`**~~ **done 2026-09-06** (doc 19 §13,
   §14): a dangling NE relocation into a segment wlink had dropped, then a
   ring-0-only mapping, then 16-bit accesses to a register BAR that takes
   only 32-bit ones. `tools/win98-driver-test.sh` now prints the driver's
   own `d3dpt9x:` lines and the device's `linear mode on (640x480x32 …)`.
5. **Get to a desktop** — the open blocker (doc 19 §15). The driver draws,
   but the shell never appears: a black screen, the wait cursor and a
   640×20 strip of garbage at the top of VRAM. Where to look, in order:
   whether the boot is merely slow at 32 bpp under TCG; what writes that
   strip; and `Enable`'s `GDIINFO` / `deviceBitmap` against `vmdisp9x`'s
   `enable.c`. The pass is a Win98 desktop the harness can drive — and the
   ACPI power-button shutdown succeeding, which is the same thing said
   another way.
6. **Step 1 — the split, XP unchanged.** Carve `core/` out of
   `d3dptdisp.c` per doc 19, thunk NT onto it, and prove it is a
   refactor: `scripts/test.sh all` green, `d3dpt-dp2-test` and `d3d7test`
   against the same golden BMP, `shtest` / `cktest` / `ebtest` / `dxttest`
   at the same case counts, `d3dgame8` still matching the native oracle,
   Moto Racer and Vice City still drawing (`tools/xp-driver-test.sh`,
   `tools/xp-motoracer.sh`, `tools/xp-vicecity.sh`). Land this on its own
   — a 9x bug on top of an unproven refactor is two bugs wearing one
   coat.
7. **Step 2 — the 9x framebuffer driver** (98's M7a): the desktop on the
   adapter, `d3dptvid: adapter found` from the device, no copy inside
   QEMU. Modes come from the INF on 9x, so decide there between an INF
   superset and a mode-list utility (doc 19 §6). Installed by INF from the
   guest-tools ISO; `SETUP.EXE` grows the component and
   `tools/setup-guest-test.sh win98`'s "never offered" check inverts.
8. **Step 3 — the DirectDraw DDI on 9x** (98's M7b): the VRAM heap, the
   flip chain against the frame counter, `DdMapMemory`'s equivalent,
   8 bpp palettized modes. `DDTEST.EXE` and `CDTEST`-style guest probes
   run on 98 as they do on XP.
9. **Step 4 — the Direct3D DDI on 9x** (98's M7c): the core's DP2 walker
   under the 9x HAL. Two decisions land here, both new on 9x (doc 19 §8):
   whether the doorbell is a mapped register page or a VxD ioctl, and how
   the single command window at the top of VRAM is serialised now that
   every process has its own copy of the HAL. `D3D7TEST.EXE`'s frame must match
   `d3dpt-dp2-test`'s BMP exactly, the same oracle XP is held to. Then
   `EBTEST` (the DX3 path, which is most of the 98 matrix), `CKTEST`,
   `DXTTEST`, and `SHTEST` — the DX8 half is in scope on 98 too, step 0
   settled that.
10. **Step 5 — the titles.** The doc 04 Win98 acceptance matrix through
   the driver, against the same titles on the Glide/WineD3D stack: which
   is faster, which is correct, and what the launcher should default to.

## Build / test loop

The 9x half, which is what this track is actually building right now:

```sh
# QEMU comes from the main checkout: this worktree has no build/ and does
# not need one until the split (step 5) touches XP.
export QEMU_BIN=$HOME/work/2ksbox/build/qemu/qemu-system-i386
export QEMU_IMG=$HOME/work/2ksbox/build/qemu/qemu-img

WATCOM=$HOME/.local/opt/open-watcom guest-tools/build-driver9x.sh   # d3dpt9x.drv + d3dpt9v.vxd + INF
tools/win98-driver-test.sh ~/vms/win98.qcow2 install                # fresh raw copy, PnP installs, reboot prompt
tools/win98-driver-test.sh ~/vms/win98.qcow2 boot                   # every run after that (~4 min)
```

`install` throws the scratch image away and converts a fresh raw from the
user's qcow2, so it is also the reset button — and the way out of safe
mode. `boot` re-stages only the two
binaries, which is what an edit-build-test cycle wants. `BOOT_WAIT=190`
buys more time on a slow run; `OUT=` moves the outputs.

**The scratch image (`build/w98/win98-m10.raw`) is hand-edited** and a new
session should know what is in it, or run `install` to start clean:
`MSDOS.SYS` has `BootLog=1` and `Logo=0`; `SYSTEM.INI` has `[386Enh]
device=C:\WINDOWS\SYSTEM\D3DPT9V.VXD` (loading the mini-VDD explicitly,
which is how it was first proven — the `minivdd=` registry path has not
been tested on its own since) and `[boot] display.drv=d3dpt9x.drv` (naming
the display driver directly, part of ruling out the selection path in doc
19 §13).

The XP side, for when the split lands:

```sh
guest-tools/build-driver.sh                          # the XP driver + DRIVER\ ISO
scripts/test.sh                                      # host stage (~30 s); `all` adds the guest stage
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 d3d7   # XP's regression oracle across the split
```

That needs a built `build/qemu` in this worktree — `scripts/build.sh` once,
~15 min from scratch on the Linux box; `build/dxvk` may be symlinked from
the main checkout rather than rebuilt.

### The second toolchain

The 16-bit `.drv` and the ring-0 `.vxd` need **Open Watcom** (`wcc`,
`wcc386`, `wasm`, `wlink`); mingw can make neither format, though the
ring-3 HAL DLL — the half that will link our core — builds with the
`i686-w64-mingw32` toolchain we already use. Installed on the Linux box at
`~/.local/opt/open-watcom`, no sudo and nothing on the system path:

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

`build-driver9x.sh` takes `WATCOM=` and says where to get it when missing.
It also carries the post-link fixes both formats need — doc 19 §12 for the
VxD's three, and the NE's expected-Windows-version and zero local heap —
because `wlink` gets them wrong and nothing downstream complains.

### The reference trees

Read, never vendored; gitignored under `build/ref/`:

```sh
git clone --depth 1 https://github.com/JHRobotics/vmdisp9x build/ref/vmdisp9x  # the .drv + VxD
git clone --depth 1 https://github.com/JHRobotics/vmhal9x  build/ref/vmhal9x   # the ring-3 DirectDraw/D3D HAL
curl -L -o build/ref/fixlink.c https://raw.githubusercontent.com/JHRobotics/fixlink/master/fixlink.c
```

`fixlink.c` is the one that says what `wlink`'s VxD output gets wrong; its
`fix_wlink_vxd` is 40 lines and was worth reading in full.

## Traps

From CLAUDE.md, and they bite here:

- **Win98 runs under TCG, not KVM** — under KVM the image loses Explorer
  at startup and there is no way to drive the guest.
- **End every scripted Win98 run with a Start-menu shutdown**, never a
  kill: a killed VM leaves the FAT dirty and the next boot runs ScanDisk.
  `win98-driver-test.sh` does this; a modal dialog can swallow it.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  seen — which is exactly how the adapter arrives.
- Never write the user's own images: `win98-driver-test.sh` works on a raw
  copy, because mtools cannot write into a qcow2 and there is no in-guest
  shell to drive before the display works.

Learned here, each at the cost of a boot or three:

- **A VxD the VMM dislikes is simply not loaded**: no `BOOTLOG.TXT` entry,
  nothing on any debug channel, no error. Doc 19 §12 has the three
  reasons; suspect the linker before the code.
- **`BootLog=1` in `MSDOS.SYS` did not produce a fresh `BOOTLOG.TXT`** on
  this image. Check the file's date (`mdir -a`) before believing a word of
  it — a four-day-old log sent this session after the wrong thing.
- **Edit `SYSTEM.INI` in binary or not at all.** Python's text mode strips
  its CRLFs on read and the rewrite ate a section header, which then looks
  exactly like Windows having rejected the setting.
- **Ring-3 port 0xE9 output has never been seen from this guest**, so the
  display driver's own debug channel is unproven; in ring 0 it works
  (the VxD's lines arrive). Until the `.drv` runs, the mini-VDD's log is
  the only reliable witness for it — `DriverInit` calls the VxD for
  exactly that reason.
- `-debugcon file:x` was verified end to end by pointing it at 0x402 and
  watching SeaBIOS write 4 KB to it; the plumbing is not the suspect.
