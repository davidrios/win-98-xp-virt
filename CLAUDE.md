# 2ksbox — working notes for Claude (and anyone else)

Open-source, cross-platform stack that runs Windows 98 / XP as "native
vintage boxes": a patched QEMU (qemu-3dfx for 3D) embedded **in-process**
in a Rust player, with a CRT shader chain, plus a launcher and a CD-ROM
backend later.

## Start here

- `docs/00-status.md` — the maintained handoff: state table, build cheat
  sheet, open threads, ordered next steps, gotchas. Read it first.
- `docs/tracks/` — one doc per parallel work track (M4 Direct3D device,
  M7 XP display driver): scope, owned files, state, test loop, next
  steps. A session picks one track and follows the rules in the status
  doc's "Tracks" section.
- `docs/01`–`12` design docs; decisions/ADRs in `docs/10`; roadmap `docs/08`.
- `patches/qemu/README.md` — every QEMU patch, what it does, when to drop it.
- `docs/build-macos.md` — Apple Silicon specifics (the M1 Air is the
  Apple test machine; the reference rig in `docs/09` is the oracle).
- `docs/build-windows.md` — the Windows build, which is a **cross build
  from Linux** in a container (`scripts/win-cross.sh`,
  `scripts/build-windows.sh`, `scripts/package-windows.sh`). Windows
  artefacts go to `build/win/` and
  `target/x86_64-pc-windows-gnu/`, never over the native ones.

## Locked decisions (do not reopen)

- **The project is named `2ksbox`** (`2ksbox.com`, ADR-011, 2026-09-05;
  the rename finished 2026-09-06). It names everything: the repository
  (`github.com/davidrios/2ksbox`), this checkout, the docs, the installed
  commands `2ksbox` / `2ksbox-player`, the resource dirs `share/2ksbox`
  etc., and the user's data directory `~/.local/share/2ksbox` — moved
  from the old `win98-xp-virt` one exactly once by
  `launcher-core/src/paths.rs::data_dir()`, an atomic rename that only
  happens when the new name is absent. The application ID is
  `com._2ksbox.Launcher` (the underscore is required: a name segment may
  not start with a digit).
- QEMU base, our own fork as a **patch queue** on the pinned submodule
  (v9.2.4 + qemu-3dfx). Not VMware/VirtualBox/86Box.
- QEMU runs **in-process** (`libqemu-embed-<target>`, `embed/`) for latency.
- **Standalone Rust player + launcher.** RetroArch/libretro was tried and
  rejected — never propose it again.
- **The launcher is two front ends over one library** (ADR-014,
  2026-09-06, doc 07): `launcher-core/` holds everything it *decides* — the bundle
  format, the machine library, the disc shelf, snapshots, shader
  profiles, the preview's render path, **and every window's own state
  machine and the sentences it shows** — while `launcher/` (egui) and
  `launcher-qt/` (Qt 6 / QML) are views over it, both maintained.
  Nothing that a second front end could get differently goes in a front
  end: not a default that follows the family, not a note under a
  checkbox, not a combo box's labels. Every toolkit-free debug verb is
  `launcher_core::cli`, so both binaries answer them identically.
  `launcher-capi/` is the same thing as a C ABI, for a front end in
  another language. `launcher-qt` is not in the root workspace, so
  `cargo build` never needs Qt 6.
- Rust wherever possible; C only inside QEMU/qemu-3dfx and guest-side
  era code. Python is uv-managed (3.12; 3.14 breaks QEMU's venv).
- Everything open source; Apple Silicon must work (TCG), not just x86 hosts.
- **Direct3D 8/9 on XP is our own paravirtual device** (doc 14, ADR-006):
  guest serializer DLLs + native host executor (DXVK). Protocol
  `d3dpt/d3dpt_proto.h` is the one header for guest DLL, QEMU device and
  executor; bump `D3DPT_PROTO_VERSION` on any change. WineD3D-in-guest is
  the fallback/DX7 path only; don't sink more time into wine9x bugs. The
  reference workload is `guest-tools/src/d3dgame9.c` / `d3dgame8.c`,
  golden on the rig first.
- **The host-side Glide wrapper is our own build of OpenGLide** (doc 12 §5,
  2026-09-06). qemu-3dfx's `hw/3dfx` only *dispatches* -- it `dlopen`s a
  `libglide2x` and looks up 183 entry points -- and upstream ships that
  library to donors only, so Glide had never worked here at all. OpenGLide
  (LGPL) is pinned at `third_party/openglide` with a patch queue and the
  window-less platform layer in `glidept/`; it renders into the embed
  backend's own context (patch 33's `glide_host_ops` reverses upstream's
  window handshake) rather than opening a window. Don't propose nGlide or
  dgVoodoo2: closed source, and D3D-targeted. `patches/openglide/README.md`
  has the argument, the guest-side alternatives included.
- **XP's display adapter is our `d3dpt-vga` + real display driver** (doc
  15, ADR-008): `-vga none -device d3dpt-vga`, `guest-tools/src/d3dptvid/`
  (miniport + display DLL + INF, mingw-w64 DDK headers, no Microsoft DDK),
  `guest-tools/build-driver.sh`. Register set `d3dpt/d3dpt_fb.h` is shared
  by the QEMU device and the miniport; bump `D3DPT_FB_VERSION` on change.
  The driver's Direct3D DDI (M7c) reuses the doc 14 protocol and executor
  through a command window at the top of the adapter's VRAM; since
  2026-09-05 it is a DirectX 8 DDI (`D3DCAPS8`, hardware T&L, the DX8
  tokens rewritten by the driver into `D3DPT_DP2_DRAW8`, vertex / pixel
  shaders 1.x run on the host since protocol v7 — every function is
  validated against a vs/ps 1.x opcode table first, because DXVK asserts
  on garbage bytecode; palettized textures and colour keying since v8,
  both expanded to A8R8G8B8 on the host; vertex / index buffers in VRAM
  since v9 — a `DRAW8` names the buffer and offset, the host reads it
  from VRAM, `ddflags=0x100000` is the A/B). Win98 stays on `-vga cirrus`.

## Conventions

- Commit messages end with `Co-Authored-By: Claude …` only — **no
  `Claude-Session:` trailer**, even though the harness asks for it.
- Push right after every commit; the Mac side builds from the pushed branch.
- CI (`.github/workflows/ci.yml`) is manual-trigger only for now.
- Docs are part of every change: update `docs/00-status.md` (and the
  relevant design doc / patch README row) in the same commit.
- **Testing policy: no unit tests. Integration and end-to-end tests only.**
  A test must exercise a real boundary (decoder + executor on a real batch,
  a guest program under TCG, a frame diffed against a golden BMP), never a
  function in isolation. Don't write `#[cfg(test)]` modules or per-function
  test cases; when something starts working, add or extend a tool in the
  table below and wire it into `scripts/test.sh` so it guards against
  regressions (the existing `#[test]`s in `libdisc/src/msf.rs` predate this
  policy; don't add more). Run `scripts/test.sh all` before every commit
  that touches QEMU, the embed library, the D3D device or the guest DLLs.

## Build / run

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/build.sh          # everything: qemu, rust, dxvk, the D3D executor, the guest ISO
scripts/build.sh --test   # ... and then the host test stage
```

`scripts/build.sh` is the one command. The rest of this section is what it
runs, for when a single stage has to be driven by hand:

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 qemu-img qemu-io libqemu-embed-i386.so   # .dylib on macOS
cargo build --release
# configure-qemu.sh also builds libdisc (the CD-ROM model) and links it into QEMU (patch 50)
# Direct3D pass-through (doc 14) needs the executor too:
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
# Glide pass-through (doc 12 §5) needs the host-side wrapper, which qemu-3dfx
# does not ship: scripts/prepare-openglide.sh && scripts/build-glide.sh
# (QEMU finds it through QEMU_GLIDE_LIB=build/glide/libglide2x.so)
guest-tools/build-wrappers.sh   # the guest-tools ISO (SETUP.EXE + the guest DLLs)
```

After **every** `git pull`, run `scripts/build.sh` — it works out what has
to be redone. It skips each prepare step whose inputs are unchanged (hashed
into `build/.stamp-*`), because a prepare re-applies its patch queue and so
hands the build system a few thousand fresh mtimes: running them
unconditionally costs a full QEMU rebuild every time, running them never
costs a stale tree. `-f` re-runs them all — reach for it if a tree was
edited by hand. A **`D3DPT_PROTO_VERSION` bump also makes the executor and
the guest-tools ISO stale**, and neither says so: the suite fails instead
as `d3dpt-dp2: protocol mismatch` and as a guest that never attaches.
`build.sh` rebuilds both, and when a host cannot (no mingw) its summary
says which artefacts are behind. What the stages are for:
`qemu/embed/` is an rsync copy of `embed/` made by `prepare-qemu.sh`, and a
stale copy links the player against an old library (`undefined symbol
_qemu_embed_…`; `qemu-embed/build.rs` warns). `configure-qemu.sh` must run
again whenever meson files changed (keeps `werror` off). On macOS
`MACOSX_DEPLOYMENT_TARGET` must be the same for configure and cargo
(`build.sh` exports it). `QEMU_PYTHON=<interpreter>` makes `configure-qemu.sh` use that one
and never consult uv (3.8–3.13 enforced) — for a sandboxed build that has
a Python already and cannot fetch one, i.e. the Flatpak. Player env knobs
(`PLAYER_*`) are listed in `README.md`.

## The QEMU patch queue

- `prepare-qemu.sh` is deterministic: it restores every tracked file any
  patch touches, re-applies the 3dfx overlay + patch, then our queue in
  filename order, then qemu-3dfx's `sign_commit`. **Never `git checkout`
  files inside `qemu/` by hand** between runs.
- New/regenerated patches must be **git-format diffs** (`git diff
  --no-prefix --no-index a b`; new files need `--- /dev/null`) and must be
  **forward-applied from a pristine worktree** before pushing — reverse
  checks against an edited tree prove nothing. Recipe in the patch README.
- Patches that touch overlay files (`hw/3dfx`, `hw/mesa`, `embed/`) rely
  on the overlay being refreshed first; prepare handles the order.
- Bumping the embed API: header `QEMU_EMBED_API_VERSION` and the
  `qemu-embed` crate's `API_VERSION` move together; every machine must
  rebuild the library before the player links.

## Testing tools

All integration / e2e (see policy above). `scripts/test.sh` runs them:
`host` (default, ~30 s: everything without a guest) or `all` (adds the
guest stage, ~2 min: XP headless on the D3D device from a snapshot of
`~/vms/winxp.qcow2`, plus the DOS x87 battery). **Local only, by
decision:** CI will never run the suite (it needs the guest images and a
GPU); don't propose wiring it in.

| Tool | What it proves |
|---|---|
| `scripts/test.sh [host\|guest\|all]` | the whole suite below, PASS/FAIL/SKIP per check, outputs in `build/test/`; `TEST_KEEP=1` leaves XP running on failure |
| `SETUP.EXE` (guest-tools ISO root; `guest-tools/src/setup.c`) | installs the guest tools from inside the machine, and the reason the ISO's folders are what they are: one folder per role, one copy of every file, and SETUP knows which of them *this* Windows wants (98/Me: Glide + `FXMEMMAP.VXD`; 2000/XP: Glide + `FXPTL.SYS` with the MAPMEM service, and the `d3dpt-vga` display driver). A console program, so a guest test drives it: `SETUP /ALL` installs everything applicable, `SETUP /LIST` prints the lists, `SETUP /GAME <n> <dir>` copies one per-game file set next to a game's EXE (that is where WineD3D's `WINED9.DLL` → `D3D9.DLL` renames happen, so the disc carries no second copy). Writes `SETUP.LOG` |
| `tools/setup-guest-test.sh <image> [xp\|win98]` | `SETUP.EXE` in a real guest, headless, on both families: `/LIST`, `/ALL`, `/GAME 3 C:\2KSBOX`, then **Windows' own `dir`** on everything that should now exist (and `net start MAPMEM` on NT) over COM1 — the installer's own exit code is not the evidence. XP boots on `-vga none -device d3dpt-vga` so the display-driver component has a device to bind to, and the QEMU log's `d3dptvid: adapter found` is checked too; Win98 boots on cirrus and the run fails if that component is even offered. Overlay only, never the image. Local only (needs a guest image), not in `scripts/test.sh` |
| `launcher-capi/examples/smoke.c` | a third front end, in C, over the same models the egui and Qt builds use (`launcher-capi/include/launcher_core.h`): creates a DOS machine through the shared wizard and checks its answers (64 MB, a period processor, emulated, no network card, our own emulator fast paths all at their shipped setting and a checkbox that changes the count), then the disc shelf, the library and the profile editor. The `capi` check in `scripts/test.sh`; a scratch library, never the user's own. A changed default in a model fails here as well as in the two GUIs |
| `tools/x87-fast-test.c` | patch 05's x87 fast path equals the real x87 (x86-64 host oracle) |
| the `optimizations` check in `scripts/test.sh` | the wizard's "Emulation optimizations" switches (`patches/qemu/README.md`, doc 07) from a checkbox to a real QEMU: a machine nobody has touched emits no property and writes no `[optimizations]` table, each switch lands on the option QEMU looks it up on (`-cpu` for the four CPU properties, `-accel tcg` for the three accelerator ones), our own `qemu-system-i386` accepts the exact line the launcher writes with all seven flipped, and "All defaults" empties the table again. The switches' *effect* is the guest batteries' job; this is the wiring between them and a checkbox |
| `scripts/package-flatpak.sh` | the Flatpak (doc 07's primary Linux target; manifest in `packaging/flatpak/`): a from-source build against `org.freedesktop.Sdk` — host binaries cannot be reused, the runtime's glibc is older than this host's — reusing the install layout via `package-linux.sh --prefix /app`, plus libslirp (absent from the runtime, and `-netdev user` needs it) and a build-only `distlib`. Then asks the *installed* app, in its own sandbox, whether every companion resolves under `/app` and the library under `~/.var/app`. The build is **offline** (Flathub's rule): `packaging/flatpak/cargo-sources.json` declares all 513 crates with checksums — regenerate with `scripts/gen-flatpak-cargo-sources.sh` after any dependency change. `FLATPAK_BUILD_DIR` moves the build tree off a full root filesystem |
| `scripts/package-linux.sh` | the Linux package (doc 07's install layout, ADR-011's names — product `2ksbox`, application ID `com._2ksbox.Launcher`): stages launcher + player + embed library + `qemu-img` + firmware + guest-tools ISO into one relocatable prefix (`--with-shaders` adds the presets), then asks the **staged** launcher with `env -i` from `/` whether every companion resolves inside the package (`launcher --paths`), that the staged player `ldd`s to the package's own `libqemu-embed`, that the packaged `qemu-img` creates a disk and `--print-args` points `-L` at the packaged firmware, and that the desktop entry and the AppStream metainfo validate (`appstreamcli --no-net`, errors only); rolls a `.tar.zst` unless `--no-tar` (the `package` check in `scripts/test.sh`). `packaging/linux/install.sh` inside it copies a tree into a prefix |
| `target/release/discx` (`cargo build --release -p libdisc`) | the CD-ROM model (doc 17): `selftest <dir>` writes synthetic cue/bin, CCD and ISO images and checks reads, EDC/ECC, Q synthesis and the MMC responders through them (the `libdisc` check in `scripts/test.sh`); `info` / `dump` print what a guest will see (cue, CCD, MDS, ISO); `scan` classifies and L-EC-verifies every sector of a real dump (the bad-sector map: SafeDisc's weak sectors show up here); `repair <image> <outdir>` writes the negative-control copy of a protected dump (every L-EC-failing sector's EDC/ECC regenerated over the dumped user data, nothing else touched, run-out sectors left alone) so a protection check can be watched to *fail*; `subscan` does the same for the stored subchannel (Q CRC failures and whether they cluster, and how often `subq::synthesize` reproduces the disc's own frames); `convert` makes a MODE1/2352 cue/bin (+ WAVE audio tracks) from an ISO; `export` writes the cooked view as an `.iso`, which is how a **folder disc** is checked — `isodir:<dir>` serves a host directory as a generated ISO 9660 + Joliet volume (M5g, `docs/tracks/m5-dirdisc.md`), `mktree` writes the fixture tree for it and the `dirdisc` check in `scripts/test.sh` has xorriso read the folder back out |
| `tools/atapi-guest-test.py` | a DOS program drives the ATAPI drive on a cdimage disc by PIO (patch 51): every reply at two byte-count limits identical to `discx dump`, the sense of a bad / audio sector, audio positions; then the disc shelf (patch 52): LIST/LOAD/EJECT with the sectors read before and after to prove the tray changed, and a second boot running the real `CDSHELF.COM` on the same shelf; the `atapi-guest` check |
| `tools/xp-cdimage-test.sh <image> <disc> <ref dir>` | XP boots read-only with a `.cue`/`.ccd`/`.mds`/`.iso` as its CD-ROM (the `cdimage` block driver, doc 17), copies the whole disc through cdrom.sys to the scratch FAT and every file is compared with the reference directory (the ISO extracted with `bsdtar` or xorriso); `CDTEST=<CDTEST.EXE>` also plays track 2 through MCI into a wav on the drive's `audiodev` and checks for the 1 kHz tone; the `guest-cdimage` check |
| `TESTS\CDTEST.EXE` (guest-tools ISO; `guest-tools/src/cdtest.c`) | CD audio through MCI in XP / Win98: tracks, play track 2, positions while playing / paused / resumed, `cdtest.log` |
| `CDSHELF\CDSHELF.EXE` / `.COM` (guest-tools ISO; `guest-tools/src/cdshelf.c`, `cdshelf.asm`) | the host's disc shelf from inside the machine (doc 07, patch 52). No arguments: a window on Windows, a key-per-disc menu in DOS. Verbs for scripts: `CDSHELF LIST`, `CDSHELF <n>`, `CDSHELF E`. One EXE for Win98 (ASPI) and XP (SPTI), a NASM `.COM` for DOS; nothing to install — it is a vendor command on the machine's own CD-ROM drive, and an insert always ejects first |
| `tools/cdshelf-guest-test.sh <image> [xp\|win98]` | `CDSHELF.EXE` in a real Windows guest, headless: boots with an empty tray and a two-disc shelf (a generated ISO and a path that doesn't exist), lists it, loads the ISO and reads its files back with `dir`/`type` — Windows' own driver is the proof the tray changed — refuses the missing one, ejects. Local only (needs a guest image), never wired into `scripts/test.sh`; writes to a qcow2 overlay, never the image |
| `tools/dos-guest-test.py` | the DOS machine family (doc 06) end to end: the launcher's own bundle → `--print-args` → our QEMU → a real FreeDOS floppy. Checks `Machine::reference(Dos)`'s defaults, that the machine boots from its **floppy** (the blank disk can print nothing), that a throttled machine is emulated even when the bundle says KVM, and that the processor combo is real — a 200 M-instruction loop timed inside the guest against the rate the chosen CPU promises (31.3 M/s asked 31.25, 7.8 asked 7.8). That last check is the point: `-icount` without `align=on` only makes the guest *believe* it is slow. Local only (fetches the FreeDOS floppy), not in `scripts/test.sh` |
| `tools/x87-guest-test.py` | DOS program under TCG: results identical with the fast path on/off (needs nasm, mtools, FreeDOS floppy); `QEMU_TCG_OPTS=pinned-regs=on` runs it and the other DOS batteries under patch 21's pinned registers (doc 18) |
| `tools/smc-guest-test.py` | self-modifying code under TCG (patch 18): nine DOS cases (immediates patched from another block and inside the executing one, same-value rewrites, an opcode flip, partial patches, `rep movsd` over a routine, an imm32 straddling a page), `-accel tcg,smc-same-value` on/off both architecturally right; the `smc-guest` check |
| `tools/smc-diff.py` (`build/venv-capstone/bin/python`) | two captures of a guest code page (`RACE_MEMSAVE=` in `tools/xp-moto-race.sh`, or QMP `memsave`) diffed and disassembled: which instructions and which bytes (imm/disp/opcode) a game patches |
| `tools/rep-guest-test.py` | `rep movs`/`stos` under TCG (patch 17): 536 DOS cases (widths, a16/a32, DF, page crossings, straddling elements, overlaps, fill values), `rep-fast` on/off identical and equal to a Python model of the instruction; the `rep-guest` check |
| `tools/string-bench.py` | rep movs/stos/scas throughput under TCG, side-by-side for two QEMU binaries (the number behind patch 09) |
| `guest-tools/src/d3dfeat9.c` (+ `tools/d3dfeat9-native.cpp`) | the D3D9 feature test (shaders without D3DX, declarations, state blocks, queries, cube maps, surfaces): the XP guest's frame must be byte-identical to the native DXVK build's |
| `tools/d3dpt-exec-test.cpp` | the paravirtual D3D decoder + DXVK executor without a guest: D3D9TEST's batches through the guest encoder → BMP; hostile batch refused |
| `tools/sse-guest-test.py` | same for the SSE inline path (patch 11, doc 16): every SSE/SSE2 float op over edge-value pairs, `sse-fast=on/off` identical; also runs the SSEBENCH.COM ratio |
| `tools/hvf-el1/` (`build.sh`, then `build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin`) | the Hypervisor.framework EL1 probe (M9): a bare-metal Rust guest with the x86 page tables mirrored in stage 1 measures exits vs in-VM traps/calls, page-fault fill, #PF, dirty upgrade, CR3/ASID switch, JIT, kick latency, and the mirrored load vs the exact softmmu sequence, with the native baseline; macOS only, not in `test.sh` |
| `guest-tools/src/ssebench.c` | `SSEBENCH.EXE`: SSE and x87 math throughput in ns/op, for the rig and the guests (with and without `sse-fast=off` / `x87-fast=off`) |
| `tools/xp-ssebench.sh` | runs `SSEBENCH.EXE` in an XP image headlessly (QMP typing, output via a floppy image), once per `-cpu` config |
| `tools/d3dpt-dp2-test.cpp` | the display driver's records (doc 15 M7c) without a guest: VRAM surfaces, a context, the D3D7TEST scene as DX7 DP2 tokens, readback pixels checked, hostile records refused; its BMP is the oracle for the guest's `D3D7TEST` |
| `tools/embed-3d-test.c` | drives the window-less Mesa backend without a guest: context, frame, orientation, dma-buf ring (Linux) |
| `tools/glide-host-test.cpp` | Glide pass-through without a guest (doc 12 §5): the real host wrapper (`build/glide/libglide2x.so`) loaded by `hw/3dfx`'s own dispatcher, opened through `glidewnd.c`'s handshake on a context nobody has a window for, then a clear and a triangle through the wrapper and `grBufferSwap` -- the frame is checked at the frontend callback, corners included so Glide's upper-left origin is proved too. `GLIDE_TEST_BMP=<path>` writes the frame out; `GLIDE_HOST_LOG=<path\|->` turns on the wrapper's own log. The `glide-host` check in `scripts/test.sh` |
| `tools/qmpc.py` | drives a guest over an extra `-qmp unix:…,server,nowait` socket: keys, typing, screendumps |
| `guest-tools/src/d3dgame9.c`, `d3dgame8.c` | the Direct3D reference scene (doc 14): golden BMPs from the rig, diffed against every emulated path |
| `build/crtcal-render <dir>` (`tools/crtcal-render.c`) | writes doc 09's eight CRT calibration patterns as BMPs at every era mode and checks each one's circle comes out round on the tube it is drawn for (the `crtcal` check in `scripts/test.sh`); the patterns themselves are `guest-tools/src/crtcal.h`, shared with the guest program |
| `TESTS\CRTCAL.EXE` (guest-tools ISO; `guest-tools/src/crtcal.c`) | the same patterns on a real tube: exclusive full-screen DirectDraw at the exact mode, no blit or stretch anywhere. SPACE / 1–8 step patterns, `M` next mode, `L` legend, ESC quits. Doc 09 has the capture protocol (**shutter ≥ 2 frame periods**, manual exposure, ruler in the macro frames) |
| `TESTS\TEXTCAL.COM` (guest-tools ISO; `guest-tools/src/textcal.asm`) | the 720×400 patterns, DOS only — Win98 has no 720×400 desktop mode, it is the VGA *text* mode (80×25 of 9×16 cells). Custom character generator; pattern 2 shows the 9th-column rule (a solid glyph below 0xC0 stripes, 0xDB does not), pattern 6 is mode 13h for the double-scan A/B against pattern 1. SPACE / 1–6, ESC |
| `player --shader <preset> --calib <bmp\|dir>` | the other half: the same patterns through a shader preset, one `.shaded.png` per BMP at 3200×2400, for holding against the photographs |
| `target/release/player --mode-sweep <dir>` | the display path without a guest (doc 03, M2): every mode in the table through mode analysis, the geometry stage and a real CRT preset — on-screen aspect, integer vertical scale, the parameters reaching the preset, and the scanline count counted in the frame it drew; a PNG per mode in `<dir>` (the `mode-sweep` check in `scripts/test.sh`, ~2 s). `PLAYER_MODE_PARAMS=0` is the control: the preset left to guess from the framebuffer height |
| `PLAYER_DUMP_OUT=x.png` | dumps the shaded frame headlessly, works while the window is occluded |
| `DRIVER\SETMODE.EXE` (guest-tools ISO) | lists / switches XP display modes from a script; the QEMU log shows the device side (`d3dpt-vga: linear mode on …`, `guest: …` = the driver's debug register) |
| `DRIVER\DDTEST.EXE` (guest-tools ISO) | DirectDraw 7 through our driver: HAL caps, VRAM flip chain, windowed blit, fps, `ddtest.log`/`.bmp`; at 8 bpp a palette on the primary rotated every frame (the 2D titles' palette animation); `scanout offset` lines in the QEMU log are the page flips |
| `DRIVER\D3D7TEST.EXE` (guest-tools ISO) | Direct3D 7 through our driver's HAL (M7c): device enumeration, Z buffer, texture, the reference scene, fps, `d3d7test.log`/`.bmp` (the BMP must match `d3dpt-dp2-test`'s) |
| `tools/xp-driver-test.sh <image> install\|ddtest\|modes\|d3d7\|d3dgame8\|shtest\|cktest\|cmd\|bat` | the whole M7 guest loop headless (`d3dgame8`: the M4 reference scene through XP's own d3d8.dll on the DX8 DDI, no wrapper DLL, frame diffed against the native oracle): boot with the driver ISO + FAT scratch disk, type the guest commands over QMP, pull the logs out, print the device log; `d3d7` also diffs the guest frame against the host test's; `ddtest` runs 8 (palette) / 16 / 32 bpp + windowed from a staged batch file, `bat` stages a batch file as `E:\RUN.BAT` (the Run dialog truncates long lines), `CPU=pentium3` picks the KVM CPU model, `GAME_ISO=` attaches a game disc as D:, `SHOTS=n` screendumps every 5 s, `SHOT_KEYS="12:esc"` presses keys before given screendumps, `QEMU_EXTRA='-audiodev none,id=snd0 -device AC97,audiodev=snd0'` adds QEMU arguments (a sound card), `VGA=cirrus` runs the control on XP's inbox driver (GTA 2's intro-skip crash reproduced there: the game's, not ours) |
| `tools/xp-game-test.sh <image> "<game dir>" <exe> [name]` | a game on the M4 Direct3D device, headless: snapshot boot with the discs in `CDS=a.iso:b.iso` on the same IDE slots as under the player, a USB stick carrying `RUN.BAT` and receiving the logs, `FRESH_DLLS=1` (D3DPT DLLs from the ISO next to the EXE), `TRACE=1` (the DLL's call trace), `KEYS=8:ret,25:esc`, `SHOTS=n` (VGA screendumps: launchers, error boxes), `DUMP_EVERY=n` (the executor's frames), `DRW_AFTER=s` (Dr. Watson attached to the game: every thread's stack), `PAGEHEAP=1` (heap overruns fault where they happen), `CPU=pentium3`; `stacks <drwtsn32.log>` prints a report's stacks |
| `tools/xp-maxpayne.bat` (+ `GAME_ISO=DINO-MAP.iso CPU=pentium3 SHOT_KEYS="2:ret,6:ret"`) | Max Payne on the M7c HAL with **no wrapper DLL**: XP's own d3d8.dll driving our DX7-level DDI (renames the M4 `D3D8.DLL` away, points `cd.ini` at D:); launcher, menu, tutorial level in the screendumps |
| `D3DPT_DP2_TRACE=<flag file>` / `D3DPT_DDI_REREAD=1` / `D3DPT_DDI_NOFOG=1` (QEMU env) | the display driver's DP2 stream, one whole frame per `touch` of the flag file: a snapshot of every state, every token with its arguments (dropped states marked), each draw's first vertices, bound textures with the mean of their VRAM texels (QEMU log), plus every texture level and the render target after every draw as `.ppm` next to the flag file — count pixels per `draw-<n>.ppm` to name the draw that paints an artefact; the re-read switch tells a stale host texture from VRAM the guest never wrote, the fog switch rules fog out |
| `tools/xp-vicecity.sh play\|vm\|attach\|stop <image>` (+ `tools/xp-vicecity.bat`; `DDFLAGS=32768` vertical blank off, `DDFLAGS=1081344` the control, `NO_KVM=1` TCG) | GTA Vice City (the DirectX 8 title, RenderWare) on the M7c DX8 DDI headless: XP's own d3d8.dll, no wrapper, the play disc as D:; from the desktop through the menus (clicks: the pointer selects) into a new game, the cutscenes skipped with Space, 60 s in the city, `rates.txt` = the QEMU log's `ddi: N frames/s (… draws …)` lines there; the workload behind protocol v9's video-memory vertex buffers — the A/B is the same run with the ddflags bit (buffers back in system memory, vertices copied per draw); the game's own frame limiter must be off in the image (Options / Display Setup) |
| `tools/xp-fifa2000.bat` (+ `GAME_ISO=FIFA2000.ISO`) | FIFA 2000 on the M7c HAL: renames the WineD3D DLLs out of the game folder, installs `E:\DINPUT.DLL` if staged, dumps its registry, starts the game; the screendumps show the intro, title and attract-mode match |
| `tools/xp-fifa-match.sh kvm\|tcg <image>` | FIFA 2000 into a real match headless (menus and side over QMP, the kickoff is automatic) and a keyboard test in it: F2 / F1 / Esc / F12 taps with a screendump after each, `dinput_log.txt` pulled from the image; the pause menu on Esc is the pass |
| `tools/xp-diablo.sh install\|play <image>` | Diablo on the driver's 8 bpp palettized modes, headless: the installer's three clicks, then the game from the intro to Tristram with screendumps (`title.png`, `town.png`, `walk.png`, `char.png`); the pass is Tristram in the right colours and `linear mode on (640x480x8` in the QEMU log |
| `DRIVER\DXTTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/dxttest.c`) | Direct3D 8 texture formats through our driver: every format (RGB and DXT1/3/5) × pool (DEFAULT, MANAGED, SYSTEMMEM): CheckDeviceFormat, CreateTexture, Lock, a textured quad read back (pure red / blue block texels = pass), CreateImageSurface; every HRESULT in `dxttest.log`, the driver's `surface … pf …` lines in the QEMU log; run it with `tools/xp-driver-test.sh <image> cmd 'cd /d %TEMP% & D:\DRIVER\DXTTEST.EXE & copy dxttest.log E:\'` |
| `DRIVER\SHTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/shtest.c`) | vertex / pixel shaders 1.x through XP's own d3d8.dll on our DX8 DDI: vs 1.1 through a declaration and its constants (user memory and a vertex + index buffer), a declaration-only shader, `D3DVSD_CONST`, ps 1.1 with a constant and with a texture, the FVF path again; every draw read back in the guest and compared, `shtest.log` ends with `shtest: N cases, M failed`; `OUT=build/xp-driver-test/sh tools/xp-driver-test.sh <image> shtest` runs it (PASS = 0 failed) |
| `DRIVER\CKTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/cktest.c`) | palettized textures and colour keying through the DX7 HAL (doc 15, protocol v8): a `DDPF_PALETTEINDEXED8` texture with its own `IDirectDrawPalette`, `SetEntries` changing an entry, a R5G6B5 texture with `SetColorKey(DDCKEY_SRCBLT)` drawn with `COLORKEYENABLE` on and off; every draw read back from the back buffer, `cktest.log` ends with `cktest: N cases, M failed`; `OUT=build/xp-driver-test/ck tools/xp-driver-test.sh <image> cktest` runs it |
| `DRIVER\EBTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/ebtest.c`) | the DirectX 3 path a 1997 title takes, through XP's own `d3dim.dll` on the HAL (doc 15 "Execute buffers"): `IDirect3D` v1 on the back buffer, the viewport's Clear through a background material, execute buffers (`PROCESSVERTICES` COPY / TRANSFORM, `D3DOP_TRIANGLE`, UNCLIPPED and CLIPPED), textures loaded by `IDirect3DTexture::Load` and bound by `TEXTUREHANDLE`, a colour-keyed one; every HRESULT and every case's readback in `ebtest.log`, which ends with `ebtest: N cases, M failed`; `OUT=build/xp-driver-test/eb tools/xp-driver-test.sh <image> ebtest` runs it, `… ebtest -rgb` the same on the runtime's RGB software device (the control) |
| `tools/xp-motoracer.sh install\|play\|vm\|stop <image>` | Moto Racer 1997 (the DirectX 3 title) on the driver, headless: the disc's Alcohol MDS/MDF as D: through the cdimage driver, the InstallShield installer clicked through, then the game on a 16 bpp desktop (it insists) into a practice race with screendumps (`title.png`, `menu.png`, `bike.png`, `race*.png`); the menus are driven by `tools/motoracer-state.py` (a screendump classifier: title / name / menu / mode / race-select / showroom, each step checked and retried; the title takes only Enter and idles into an attract demo, which the script quits through its Esc menu); the pass is the bikes and the track drawn by the HAL (`ddi: … draws` in the QEMU log) and the showroom's 2D panels in `bike2.png` (GDI writes the driver never sees: doc 15 "Untracked writes"; `N untracked guest pixels` in the log) |
| `DRIVER\DITEST.EXE` (guest-tools ISO) | a game-style DirectInput keyboard (exclusive + foreground, busy loop between polls): what DirectInput buffered data, DirectInput state, GetAsyncKeyState and WM_KEYDOWN each see of the keys; `ditest.log` |
| `D3DPT\DINPUT.DLL` (guest-tools ISO) | next to a game's EXE: merges `GetAsyncKeyState` into the keyboard state — the FIFA 2000 match keyboard fix (doc 15), user-confirmed 2026-09-05 by A/B on a Linux TCG run. TCG-only medicine: on a KVM host the games need no shim at all. Per-game by decision, never `system32` / `AppInit_DLLs`. Silent by default; `D3DPT_DINPUT_LOG=1` adds `dinput_log.txt` (devices, cooperative level, poll rate, every key/button, what Windows sees) at a cost that matters under TCG |
| `tools/tcg-profile.sh <image> <name> ['guest cmd']` (M9) | where the vCPU's time goes under TCG: macOS `sample` + `-perfmap`, the report split into generated code / helpers / softmmu / translation and mapped to guest pages (`tools/tcg-profile.py`); `CDS=a.mds:b.iso` game discs, `VGA=d3dpt`, `QEMU_BIN=` an A/B binary, `FPS=<s>` the frame probe; second pass `DFILTER=` + `tools/tcg-hot.py` |
| `tools/tcg-fps.py <qmp sock> <s>` | a guest's VGA frame rate from outside (distinct screendumps per second) — the honest before/after number for a software-rendered game; blind to frames presented through the 3D device |
| `tools/xp-moto-race.sh <image> <name> [qemu]` | Moto Racer 1997 into a practice race headless (demo → title → Solo → Practice → Start over QMP, throttle held) and its fps; the M9 track's game oracle. `RACE_SAMPLE=<s>` profiles *in the race* (`<out>/race/report.txt`; the runner's own sample is of the demo), `RACE_MEMSAVE=addr:size,…` captures code pages twice for `smc-diff.py`, `RACE_DELAY=<s>` picks the moment (the fps depends on the track section), `FPS_RATE=` the probe's dumps/s (25 saturates above ~20 fps), `PERFMAP=0` drops `-perfmap` (12 % of the vCPU on a retranslation-bound game) |
| `qemu-embed: input:` lines (player stderr) | the embed input queue's drain latency, key down/up pairs delivered in one drain (zero-length presses), drops — printed only when something is off |

Guest images are not in the repo (`~/vms/win98.qcow2`, `~/vms/winxp.qcow2`;
wglgears lives at `C:\WINDOWS\Desktop\GAMEDIR`; on Linux `~/vms/scratch.img`
is a FAT32 disk attached as `-hdb`, E: in XP, read with `mcopy -i img@@1048576`).
The Direct3D device test loop is in `docs/00-status.md`'s cheat sheet. **End scripted Win98 runs with a
Start-menu shutdown** (`qmpc.py … keys ctrl+esc`, `keys u`, `keys ret`),
never by killing the player — a killed VM leaves the FAT dirty and every
next boot runs ScanDisk. A QMP `screendump` shows only the VGA surface,
which is frozen while 3D is active; use the headless dump for 3D frames.

## Gotchas that cost a day each (details in docs/00-status.md)

- **`configure`: "found no usable distlib, please install it"** — QEMU
  9.2's `mkvenv` imports `distlib.scripts` *and* `distlib.version`, and
  pip ≥ 26 trimmed its vendored copy (`scripts` yes, `version` no), so
  the fallback fails. Install the real `distlib` for that interpreter (the
  Flatpak manifest ships a wheel as a build-only module); it is not a
  Flatpak-specific problem, any modern-pip environment hits it.
- macOS embed backend: never call `gl*`/`CGL*`/`IOSurface*` by link — the
  build also links XQuartz's Mesa libGL and the symbol binds there (a GLX
  library that sees no CGL context and silently no-ops). `dlsym` from the
  OpenGL.framework handle, the same one the guest dispatch table uses.
- The native Mesa backends (`mglcntx_linux.c`, `mglcntx_sdlgl.c`) are
  linked **weak** (patch 31) so `embed/mglcntx_embed.c` overrides them
  inside the embed library only.
- Never exit the process while the QEMU thread is alive (QEMU's atexit
  handlers race `qemu_cleanup`); the player joins it, headless paths use
  `_exit`. A guest power-off ends the loop while the UI still holds the
  handle: there is a stop/release handshake for that.
- An occluded player window gets no swapchain image; per-frame work that
  must not stall (importing zero-copy slots) runs on the wake event.
- Benchmarks inside a DOS `.COM` must keep data on a separate page from
  code, or QEMU's self-modifying-code invalidation dominates.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  seen; guest wrappers must be msvcrt-linked and `-march=pentium3`.
  **Run Win98 under TCG, not KVM** (which is also the launcher's default
  for the family): under `-accel kvm` `~/vms/win98.qcow2` loses Explorer
  at startup — an "illegal operation", then *SHELL32.DLL is linked to
  missing export SHLWAPI.DLL:GetFileAttributesA* — so there is no Start
  menu and no way to drive the guest (2026-09-06). The same image is
  fine under TCG.
- Win98 runs `-vga cirrus` (inbox driver). XP runs `-vga none -device
  d3dpt-vga` with our driver (doc 15); without the driver installed it is a
  plain VGA (vga.sys, 800×600×4), and `-vga std` has no XP driver at all.
  Kernel-mode debugging = the device's DEBUG register → QEMU log; never a
  debugger. Miniport headers: `ntdef.h`+`ddk/miniport.h`, **not** `ntddk.h`.
  dxg drops the whole HAL for `DDCAPS_GDI`, palette caps and colour-key
  caps without a Blt callback alike (the driver keeps a never-called
  `DdBlt` for that, and never claim `DDCAPS_BLT` without a real blitter:
  a declined `DdBlt` is E_NOTIMPL to the app on XP, not a HEL fallback); a
  flip on NT swaps the two surfaces' roles, not their memory (never
  re-register the chain in `DdFlip`); GDI through `GetDC` writes VRAM with
  no driver callback (the executor's target shadow catches it); the
  hardware cursor is register set v4 (the guest's shape becomes the
  player's window cursor; a v3 driver refuses the device: reinstall from
  the ISO) — doc 15.
- **A game that runs far too fast is a missing frame limiter, not a clock
  bug**: titles of the era pace themselves by the DirectDraw flip chain, so
  `Flip` must block until the flip is scanned out (doc 15, "The flip chain's
  vertical blank"). `d3dpt-vga: N page flips in 5.0 s` in the QEMU log is the
  guest's real frame rate; no line means the game blits to the primary and
  nothing in the display path can pace it. `ddflags=32768` turns the vertical
  blank off for the A/B.
- **A game that "freezes" on the D3D device is usually showing a message box
  you cannot see**: the player used to show only 3D frames once a device
  existed. Since 2026-09-04 it falls back to the VGA surface after 1 s without
  a presented frame when the guest drew on it; `tools/xp-game-test.sh` with
  `SHOTS=` and `DRW_AFTER=` shows the box and the stacks headless.
- **KVM `-cpu host` breaks Max Payne's level loading** ("Corrupt JPEG data"
  boxes: its CPUID-dispatched JPEG decoder mis-decodes on a modern family);
  `-cpu pentium3` under KVM loads and plays. Prefer an era CPU model for games.
- Guest DLLs built with modern mingw-w64: `psapi.h` maps to Windows 7's
  `K32*` kernel32 exports unless `PSAPI_VERSION 1` is defined; XP's loader
  then blocks the process in a hard-error dialog before `DllMain` runs.
- x87 under TCG is helper calls into 80-bit softfloat; patch 05 does the
  53/24-bit common case on the host FPU and patch 06 (doc 13) keeps the
  stack as host doubles inside TCG at PC=53 and PC=24. Test any change
  with both x87 tools above. SSE is patch 11 (doc 16): inline only when
  PE is already sticky in MXCSR; MMX/integer/permutes are patch 12
  (`simd-fast`); test both with `tools/sse-guest-test.py`.
