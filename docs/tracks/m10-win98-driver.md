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
- The 9x half once it exists: the 16-bit display driver, its VxD, its INF
  and its installer, plus whatever the DDK-header vendoring needs.
- `guest-tools/src/setup.c` — the display-driver component for the 98/Me
  role — and `tools/setup-guest-test.sh`'s Win98 expectations.
- Tests: a `tools/win98-driver-test.sh` mirroring `tools/xp-driver-test.sh`,
  and the Win98 side of whatever guest checks land in `scripts/test.sh`.
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

**Step 0 is done** — the 9x driver model is established and written into
doc 19; nothing is built yet. The findings that change the plan:

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
- **New build prerequisite: Open Watcom** for the `.drv` and the `.vxd`
  (mingw cannot make NE or VxD binaries); not installed on this box.
  Header provenance for the 16-bit side is an open item — our rule is no
  Microsoft DDK.

`vmdisp9x` and `vmhal9x` are cloned to `build/ref/` (gitignored) for
reading; nothing from them is vendored.

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
2. **Get Open Watcom building a "hello world" `.drv` + `.vxd`** before
   the split, because it is the one prerequisite that can fail outright:
   install it, build a minimal 16-bit display driver over the DIB engine
   and a dynamic VxD, and get a 98 guest to a desktop on `d3dpt-vga` with
   them — no DirectDraw, no core. That also settles the header-provenance
   question in practice. If this cannot be made to work, the track stops
   here and nothing has been wasted on the split.
3. **Step 1 — the split, XP unchanged.** Carve `core/` out of
   `d3dptdisp.c` per doc 19, thunk NT onto it, and prove it is a
   refactor: `scripts/test.sh all` green, `d3dpt-dp2-test` and `d3d7test`
   against the same golden BMP, `shtest` / `cktest` / `ebtest` / `dxttest`
   at the same case counts, `d3dgame8` still matching the native oracle,
   Moto Racer and Vice City still drawing (`tools/xp-driver-test.sh`,
   `tools/xp-motoracer.sh`, `tools/xp-vicecity.sh`). Land this on its own
   — a 9x bug on top of an unproven refactor is two bugs wearing one
   coat.
4. **Step 2 — the 9x framebuffer driver** (98's M7a): the desktop on the
   adapter, `d3dptvid: adapter found` from the device, no copy inside
   QEMU. Modes come from the INF on 9x, so decide there between an INF
   superset and a mode-list utility (doc 19 §6). Installed by INF from the
   guest-tools ISO; `SETUP.EXE` grows the component and
   `tools/setup-guest-test.sh win98`'s "never offered" check inverts.
5. **Step 3 — the DirectDraw DDI on 9x** (98's M7b): the VRAM heap, the
   flip chain against the frame counter, `DdMapMemory`'s equivalent,
   8 bpp palettized modes. `DDTEST.EXE` and `CDTEST`-style guest probes
   run on 98 as they do on XP.
6. **Step 4 — the Direct3D DDI on 9x** (98's M7c): the core's DP2 walker
   under the 9x HAL. Two decisions land here, both new on 9x (doc 19 §8):
   whether the doorbell is a mapped register page or a VxD ioctl, and how
   the single command window at the top of VRAM is serialised now that
   every process has its own copy of the HAL. `D3D7TEST.EXE`'s frame must match
   `d3dpt-dp2-test`'s BMP exactly, the same oracle XP is held to. Then
   `EBTEST` (the DX3 path, which is most of the 98 matrix), `CKTEST`,
   `DXTTEST`, and `SHTEST` — the DX8 half is in scope on 98 too, step 0
   settled that.
7. **Step 5 — the titles.** The doc 04 Win98 acceptance matrix through
   the driver, against the same titles on the Glide/WineD3D stack: which
   is faster, which is correct, and what the launcher should default to.

## Build / test loop

```sh
guest-tools/build-driver.sh                 # the driver + DRIVER\ ISO (both OSes once step 1 lands)
guest-tools/build-wrappers.sh               # the big guest-tools ISO
scripts/test.sh                             # host stage (~30 s); `all` adds the guest stage
tools/setup-guest-test.sh ~/vms/win98.qcow2 win98    # SETUP in a real 98, headless
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 d3d7   # XP's regression oracle across the split
```

This worktree has its own submodules but no `build/`. It needs
`scripts/build.sh` once (~15 min for QEMU from scratch on the Linux box);
`build/dxvk` may be symlinked from the main checkout rather than rebuilt.

The 9x binaries need a second toolchain: **Open Watcom** (`wcc`, `wcc386`,
`wasm`, `wlink`) for the 16-bit `.drv` and the ring-0 `.vxd`. It is not
installed on this box and mingw cannot stand in for it; the ring-3 HAL DLL,
which is the half that links our core, builds with the
`i686-w64-mingw32` toolchain we already use.

The reference trees read in step 0 are cloned (gitignored, not vendored):

```sh
git clone --depth 1 https://github.com/JHRobotics/vmdisp9x build/ref/vmdisp9x   # the .drv + VxD
git clone --depth 1 https://github.com/JHRobotics/vmhal9x  build/ref/vmhal9x    # the ring-3 DirectDraw/D3D HAL
```

Notes that cost time if forgotten (CLAUDE.md has them, they bite here):

- **Win98 runs under TCG, not KVM** — under KVM the image loses Explorer
  at startup and there is no way to drive the guest.
- **End every scripted Win98 run with a Start-menu shutdown**, never a
  kill: a killed VM leaves the FAT dirty and the next boot runs ScanDisk.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  seen — which is exactly how the adapter arrives.
- Never write the user's own images: run on overlays or on the m10 copies.
- Kernel-mode debugging is the device's DEBUG register into the QEMU log,
  never a debugger.
