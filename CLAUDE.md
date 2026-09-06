# win98-xp-virt — working notes for Claude (and anyone else)

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

## Locked decisions (do not reopen)

- QEMU base, our own fork as a **patch queue** on the pinned submodule
  (v9.2.4 + qemu-3dfx). Not VMware/VirtualBox/86Box.
- QEMU runs **in-process** (`libqemu-embed-<target>`, `embed/`) for latency.
- **Standalone Rust player + launcher.** RetroArch/libretro was tried and
  rejected — never propose it again.
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
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so   # .dylib on macOS
cargo build --release
# configure-qemu.sh also builds libdisc (the CD-ROM model) and links it into QEMU (patch 50)
# Direct3D pass-through (doc 14) needs the executor too:
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
```

After **every** `git pull`, repeat prepare → configure → ninja → cargo:
`qemu/embed/` is an rsync copy of `embed/` made by `prepare-qemu.sh`, and a
stale copy links the player against an old library (`undefined symbol
_qemu_embed_…`; `qemu-embed/build.rs` warns). Run `configure-qemu.sh`
again whenever meson files changed (keeps `werror` off). On macOS export
`MACOSX_DEPLOYMENT_TARGET` to the running OS before both configure and
cargo. Player env knobs (`PLAYER_*`) are listed in `README.md`.

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
| `tools/x87-fast-test.c` | patch 05's x87 fast path equals the real x87 (x86-64 host oracle) |
| `target/release/discx` (`cargo build --release -p libdisc`) | the CD-ROM model (doc 17): `selftest <dir>` writes synthetic cue/bin, CCD and ISO images and checks reads, EDC/ECC, Q synthesis and the MMC responders through them (the `libdisc` check in `scripts/test.sh`); `info` / `dump` print what a guest will see (cue, CCD, MDS, ISO); `scan` classifies and L-EC-verifies every sector of a real dump (the bad-sector map: SafeDisc's weak sectors show up here); `repair <image> <outdir>` writes the negative-control copy of a protected dump (every L-EC-failing sector's EDC/ECC regenerated over the dumped user data, nothing else touched, run-out sectors left alone) so a protection check can be watched to *fail*; `subscan` does the same for the stored subchannel (Q CRC failures and whether they cluster, and how often `subq::synthesize` reproduces the disc's own frames); `convert` makes a MODE1/2352 cue/bin (+ WAVE audio tracks) from an ISO |
| `tools/atapi-guest-test.py` | a DOS program drives the ATAPI drive on a cdimage disc by PIO (patch 51): every reply at two byte-count limits identical to `discx dump`, the sense of a bad / audio sector, audio positions; the `atapi-guest` check |
| `tools/xp-cdimage-test.sh <image> <disc> <ref dir>` | XP boots read-only with a `.cue`/`.ccd`/`.mds`/`.iso` as its CD-ROM (the `cdimage` block driver, doc 17), copies the whole disc through cdrom.sys to the scratch FAT and every file is compared with the reference directory (the ISO extracted with `bsdtar` or xorriso); `CDTEST=<CDTEST.EXE>` also plays track 2 through MCI into a wav on the drive's `audiodev` and checks for the 1 kHz tone; the `guest-cdimage` check |
| `GAMEDIR\CDTEST.EXE` (guest-tools ISO; `guest-tools/src/cdtest.c`) | CD audio through MCI in XP / Win98: tracks, play track 2, positions while playing / paused / resumed, `cdtest.log` |
| `tools/x87-guest-test.py` | DOS program under TCG: results identical with the fast path on/off (needs nasm, mtools, FreeDOS floppy) |
| `tools/string-bench.py` | rep movs/stos/scas throughput under TCG, side-by-side for two QEMU binaries (the number behind patch 09) |
| `guest-tools/src/d3dfeat9.c` (+ `tools/d3dfeat9-native.cpp`) | the D3D9 feature test (shaders without D3DX, declarations, state blocks, queries, cube maps, surfaces): the XP guest's frame must be byte-identical to the native DXVK build's |
| `tools/d3dpt-exec-test.cpp` | the paravirtual D3D decoder + DXVK executor without a guest: D3D9TEST's batches through the guest encoder → BMP; hostile batch refused |
| `tools/sse-guest-test.py` | same for the SSE inline path (patch 11, doc 16): every SSE/SSE2 float op over edge-value pairs, `sse-fast=on/off` identical; also runs the SSEBENCH.COM ratio |
| `guest-tools/src/ssebench.c` | `SSEBENCH.EXE`: SSE and x87 math throughput in ns/op, for the rig and the guests (with and without `sse-fast=off` / `x87-fast=off`) |
| `tools/xp-ssebench.sh` | runs `SSEBENCH.EXE` in an XP image headlessly (QMP typing, output via a floppy image), once per `-cpu` config |
| `tools/d3dpt-dp2-test.cpp` | the display driver's records (doc 15 M7c) without a guest: VRAM surfaces, a context, the D3D7TEST scene as DX7 DP2 tokens, readback pixels checked, hostile records refused; its BMP is the oracle for the guest's `D3D7TEST` |
| `tools/embed-3d-test.c` | drives the window-less Mesa backend without a guest: context, frame, orientation, dma-buf ring (Linux) |
| `tools/qmpc.py` | drives a guest over an extra `-qmp unix:…,server,nowait` socket: keys, typing, screendumps |
| `guest-tools/src/d3dgame9.c`, `d3dgame8.c` | the Direct3D reference scene (doc 14): golden BMPs from the rig, diffed against every emulated path |
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
