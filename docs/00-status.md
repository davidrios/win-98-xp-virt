# 0. Status and how to resume (updated 2026-09-03)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop; Super PI 1M on the Air 9:49 → 6:33. Patch 06 (2026-09-03, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **XP Super PI 1M on the Air 1:57, faster than the rig's real P4 1.7 (2:02)**. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio, QMP over a socketpair (`PLAYER_QMP`, `PLAYER_QMP_EXEC`). **Win98 and XP run in it on the M1 Air** with sound and tablet mouse. |
| 3D | **GL pass-through runs inside the player on Linux** (doc 12 steps 1–2, 2026-09-02): patches 30/31 + `embed/mglcntx_embed.c` (EGL surfaceless pbuffer as FBO 0, `glReadPixels` on swap) + API v4. Win98 wglgears in the player: 420 fps at 800×600 with the readback path, desktop returns on exit. Standalone `-display sdl` still works (500+ fps on the Air). **macOS too** (CGL, no drawable, FBO stand-in; `GL 2.1 Metal / Apple M1`, wglgears in the player on the Air). **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5, 2026-09-03): 575–600 fps wglgears, nothing copied per frame. **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air. Glide: no window, reported cleanly. |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) and, since 2026-09-03, the WineD3D set from JHRobotics/wine9x (Wine 1.7.55 for 9x/XP: per-game D3D8/D3D9/WINED3D DLLs + system-wide switchers) with a D3D9 smoke test (`D3D9TEST.EXE`), the display-mode probe (`MODETEST.EXE`) and the reference workloads `D3DGAME9.EXE` / `D3DGAME8.EXE` (doc 14 P0a) into an ISO. **Rig (P4 + GeForce 6200), 2026-09-03: both run.** First-run fixes: ground triangle winding (top face was culled), shader path now applies the per-cube material (all cubes were one colour), d3dgame8 windowed swaps with COPY_VSYNC so both pace at the refresh rate by default (85 fps on the rig's monitor is vsync, `-novsync` for throughput), console output also goes to `d3dgameN.log`. **Golden captures landed 2026-09-03** (`reference/d3d/rig-2026-09-03/`, diff with `tools/bmpdiff.py`): d3dgame9 frame 300 windowed, fixed function and `-shader`. The rig's log explained the `-shader` oddity: d3dx9_36's HLSL compiler refuses ps_1_1 (X3539), so the cubes ran vs_1_1 + fixed-function pixel stage while the log claimed fixed function. Fixed afterwards (ps_1_1 assembled via D3DXAssembleShader, honest log, HUD bars deterministic in `-frames` mode, elapsed-ms summary fixed) — those fixes have not been run on the rig; the rig stays off for now. d3dgame8 windowed with COPY_VSYNC runs at half refresh (43 fps at 85 Hz) on the GeForce driver: real behaviour, recorded. Must match the host's qemu-3dfx commit. **Win98 and XP (2026-09-03): wglgears and D3D9TEST run in the player on both** (WineD3D needs no Microsoft DX runtime in the guest; XP needs the FXPTL.SYS step first, see gotchas). XP D3D9TEST on the Air: adapter reported as "GeForce 6800" (WineD3D's GL-renderer mapping), x87 PC=24 after CreateDevice, 377–504 fps windowed 640×480. |
| Guests | Win98 SE on the Air: installed, repaired to PCI-bus enumeration (must be an ACPI `SETUP /p j` install or repaired — doc 06/build-macos). XP on the Air: installed, boots in the player in ~30 s (same as the rig, P4 1.7); integer 1.3–2× the rig (7-Zip), x87 FP 21 % on softfloat (Super PI 1M 9:49 vs 2:02), 104 % with patch 06 (1:57) — `reference/benchmarks/`. |
| CD backend (libdisc) | vocabulary types + MSF/LBA only (M5). |
| Launcher | stub (M6). |

## Build / run cheat sheet

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so     # .dylib on macOS
cargo build --release
# After every git pull: repeat prepare → configure → ninja → cargo. qemu/embed/
# is a COPY of embed/ (prepare-qemu.sh rsyncs it); a stale copy links the
# player against an old dylib ("undefined symbol _qemu_embed_..."). build.rs
# warns when the copy differs. macOS: export MACOSX_DEPLOYMENT_TARGET (same
# value configure-qemu.sh printed) before cargo too, or ld warns about
# "built for newer macOS version" on every C++ dep and libqemu.
# Win98 in the player (macOS shown; Linux identical, drop coreaudio bits)
target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 256 -hda ~/vms/win98.qcow2 \
  -vga cirrus -net none -usb -device usb-tablet -device sb16,audiodev=embed0
```
Player env knobs: `PLAYER_DUMP`, `PLAYER_DUMP_OUT`, `PLAYER_DUMP_SEQ`,
`PLAYER_KEYS`, `PLAYER_AUDIO_NULL`, `PLAYER_LATENCY`, `PLAYER_REFRESH_MS`,
`PLAYER_SHADER`, `PLAYER_QMP`, `PLAYER_QMP_EXEC` (README). Firmware must be passed with `-L qemu/pc-bios`
until machine bundles exist. Test image: FreeDOS 1.3 floppy
(`build/images/144m/x86BOOT.img`, git-ignored; `tools/x87-guest-test.py`
fetches FD13-FloppyEdition.zip from ibiblio and extracts it).
macOS specifics: `docs/build-macos.md`. x87 tests need `brew install nasm
mtools`; `tools/x87-guest-test.py` downloads the FreeDOS floppy itself.

## Known issues / open threads

- 3D sync is `glFinish` before every hand-off (both platforms); a shared
  fence would let the vCPU continue while the blit drains. Glide (doc 12
  §5) is the last M3 item.

- Warm reboot of Win98 freezes on the Air (cold start works; Linux reboot
  paths verified fine). Untriaged: needs `-monitor stdio` → `info registers`
  / `info pic`, `-machine pc,hpet=off` test, and a stock-QEMU comparison.
- SDL standalone on macOS: 3D presentation janky unless the mouse moves
  (`SDL_GL_SwapWindow` from the vCPU thread; try `mesagl.cfg`
  `DispTimerMS,16`). Not relevant once M3 lands.
- Display Properties in Win98 under TCG faults RUNDLL32 (upstream 1964).
- On 2000/XP the qemu-3dfx OPENGL32.DLL maps the device through
  `\\.\MAPMEM` = FXPTL.SYS in its DllMain and returns FALSE without it,
  so every GL/D3D EXE "crashes at startup" (0xc0000142). The ISO's
  WIN2KXP step (FXPTL.SYS + INSTDRV.EXE as admin, reboot) is required for
  OpenGL and WineD3D, not only Glide. Hit and resolved 2026-09-03.
- FIFA 2000 (DX7, XP): with the WineD3D DDRAW.DLL it died in
  SetCooperativeLevel before the menu (2026-09-03). Read from the disk image
  (qemu-img convert → hdiutil attach → Dr Watson + Wine logs, the logs need
  the flushing debug build): wined3d asked cirrus for 800×600×32 because it
  maps the 24-bit desktop to B8G8R8X8, the driver refused, Wine 1.7.55
  crashed in the init_3d error path. Fixed in `patches/wine9x/01` (verified:
  the game runs, sound plays). Next symptom, white screen: wined3d logs show
  `glDrawBuffer` → GL_INVALID_OPERATION, and ddraw presents the primary
  surface by drawing into GL_FRONT + glFlush, never SwapBuffers; our embed
  backend's FBO stand-in has no front buffer and only presented on swaps.
  Fixed 2026-09-03 in `embed/mglcntx_embed.c` (macOS section): GL_FRONT/
  GL_BACK on framebuffer 0 → GL_COLOR_ATTACHMENT0, glFlush/glFinish present
  while the front buffer is selected. Not yet re-tested. Linux (EGL pbuffer)
  has the same swap-only presentation and will need the flush path too. The
  host also reports an ARB program failing to assemble ("out of range
  indirect offset +65", 9× per run): unexplained, may matter later. The
  stock software renderer also crashed once at match start with Microsoft's
  DDraw (NULL surface in softdrawz.dll), so the game may have a second,
  unrelated problem on this XP. **Parked 2026-09-03 (ADR-006):** the match
  renders (flush present + mode follow), but the pitch texture is noise
  bands, the screen flickers (present per glFlush) and DirectInput dies at
  the mode switch; the host's "program error +65" lines are wined3d's own
  ARB offset-limit probe, harmless. Direct3D 8/9 on XP moves to our
  paravirtual device (doc 14); WineD3D stays the DX7 fallback.
- Player: keys held in the guest are lifted on focus loss (2026-09-03):
  Cmd+Tab delivered the Windows-key press to the player and its release to
  the next app, leaving the guest with Win held down.
- Win98: after wglgears / D3D9TEST exit, the mouse stops working in the
  guest (2026-09-03, untriaged: wrapper hook/cursor state vs the
  player's tablet? check whether keyboard still works and whether a
  second launch restores it).
- XP has no driver for `-vga std` (Bochs VBE): basic 640×480×16. Run XP
  with `-vga cirrus` like Win98 (inbox GD5446 driver, PnP picks it up on
  the next boot). Docs 04/06 said otherwise until 2026-09-03.
- Pixel aspect / mode table not implemented (720×400 shows 9:5) — M2.
- `enable_cache` for librashader off (needs `Features::PIPELINE_CACHE`).
- `prepare-qemu.sh` must be followed by `configure-qemu.sh` when meson
  files change; the script keeps `werror` off and unchanged mtimes stable.
- x87 under TCG was all helper calls into 80-bit softfloat; patch 05 does
  the 53/24-bit-precision common case on the host FPU, and patch 06
  (doc 13, merged 2026-09-03) keeps the x87 stack as host doubles across
  instructions in TCG at PC=53: 21.6 (softfloat) / 10.6 (patch 05) /
  2.9 ns per op on x86-64; XP Super PI 1M on the Air 9:49 → 6:33 → 1:57
  (rig: 2:02), `x87-fast=off` control at softfloat pace, Win98 boots.
  Two aarch64 backend paths upstream never runs needed fixes (UMOV
  element size, constant into a V register). PC=24 (Direct3D) is inline
  too since 2026-09-03 (mode 2: same double shadows holding 24-bit
  values, results rounded through binary32; guest test identical, DOS
  loop 6.0× softfloat on the Air vs 10.4× at PC=53). Not yet checked in
  a D3D title. Test any change to
  it with `tools/x87-fast-test.c` (x86-64 host oracle) and
  `tools/x87-guest-test.py` (on/off identical under TCG; needs nasm,
  mtools, the FreeDOS floppy). Benchmarks inside a .COM must keep data on
  a separate page from code or QEMU's SMC invalidation dominates.
- Driving a Windows guest headlessly on Linux: pass
  `-qmp unix:/path,server,nowait` to the player (extra monitor), then
  `screendump` / `send-key` from a script; the QMP screendump shows the VGA
  surface only (frozen while 3D is active) — grab the player window with
  `grim` to see 3D frames. Win98 image copy: `~/vms/win98.qcow2`; wglgears
  at `C:\WINDOWS\Desktop\GAMEDIR`.
- Scripted guest runs: `tools/qmpc.py <sock> keys|type|screendump|json`
  against `-qmp unix:…,server,nowait`. Shut Win98 down from inside
  (`keys ctrl+esc`, `keys u`, `keys ret`) instead of killing the player —
  a killed VM leaves the FAT dirty and every next boot runs ScanDisk.
  `PLAYER_DUMP_OUT` dumps the shaded frame even when the window is
  occluded (compositor screenshots are useless then).
- macOS embed backend: never call `gl*`/`CGL*` by link — the QEMU build
  links XQuartz's Mesa libGL too and the symbol binds there (GLX library,
  no CGL context → silent no-ops, NULL renderer). `dlsym` on the
  OpenGL.framework handle, the same one the dispatch table uses.
- The Mesa backend (`MGL*`) runs on the vCPU thread under the BQL and can
  be driven without a guest right after `qemu_embed_new` (BQL held):
  `tools/embed-3d-test.c`. Order: `InitMesaGL` → `MGLTmpContext` →
  Choose/SetPixelFormat → `MGLCreateContext(MESAGL_MAGIC)` →
  `MGLMakeCurrent(MESAGL_MAGIC, 0)` → draw → `MGLSwapBuffers`.
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.

## Next steps, in order

1. ~~M1~~ closed 2026-09-02 (latency at the vsync floor, XP benchmarked,
   patch 05 x87 fast path, QMP over socketpair). Patch 06 merged
   2026-09-03 (XP Super PI 1M 1:57 on the Air, doc 13); PC=24 inline
   mode added the same day. Only the Windows host remains untested.
   D3D9TEST runs on XP (2026-09-03) and reports PC=24, so Direct3D on
   XP does run in inline mode 2. Next on x87: a Direct3D title in XP
   with and without `-cpu pentium3,x87-fast=off` for the real-world
   number. It has to be a D3D8/9 title: Wine 1.7.55's ddraw does not
   implement DDSCL_FPUSETUP ("unhandled, harmless"), so DirectX 5–7
   games through WineD3D stay at PC=53 unless the game sets the control
   word itself (candidate DDRAW.DLL patch: setup_fpu() on FPUSETUP
   without FPUPRESERVE, as native does). FIFA 2000 (DX7, SafeDisc,
   D3D/Glide/software EXEs) tests the DDraw path and Glide, not mode 2.
2. **M4 — paravirtual Direct3D device (doc 14, ADR-006, decided
   2026-09-03).** P0 first: the reference workload `D3DGAME9.EXE` /
   `D3DGAME8.EXE` (guest-tools/src) must run perfectly on the rig
   (P4 + GeForce 6200) and produce golden screenshots (`-dump`) — ~~done
   2026-09-03~~ (`reference/d3d/rig-2026-09-03/`; a re-capture with the
   fixed build is wanted whenever the rig is next on), then the
   DXVK-native spike on the Air (MoltenVK) and Linux (P0b, in progress
   2026-09-03); those two decide the host executor before any guest DLL
   is written.
3. **M3 (doc 12), in progress:** ~~vtable patch → EGL backend with
   readback → Win98 wglgears in the player on Linux → macOS CGL backend →
   wglgears in the player on the Air → dma-buf zero-copy on Linux~~ (done)
   → IOSurface zero-copy on macOS~~ (done) → Glide → fence-based sync
   instead of glFinish.
4. M2 mode table + pixel aspect; curated presets vs. rig CRT photos.
5. M5 libdisc; M6 launcher.

## Gotchas learned (don't relearn)

- Host toolchains: pinned 9.2.x needs `--disable-werror` (+ native-file
  strip), `-fPIC` + `b_staticpic` for the shared lib, uv-managed Python 3.12
  (3.14 breaks mkvenv), `MACOSX_DEPLOYMENT_TARGET` = running OS.
- macOS link: `qemu_default_main` must exist (cocoa.m); plugin export list
  hides symbols → our ld64 list; XQuartz + SDL2 required to build.
- Guest wrappers: modern mingw-w64 links the UCRT (Win9x has none) and
  qemu-3dfx compiles `-march=x86-64-v2`; the script forces msvcrt +
  pentium3 and refuses anything else.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  detected; repair path in build-macos.md.
- Caps-Lock→Control on macOS reports as right Ctrl to SDL (patch 03).
- Never `exit()` the process while the QEMU thread is alive: QEMU registers
  atexit handlers (`audio_cleanup`, exit notifiers) that then race
  `qemu_cleanup` → `assertion failed: mutex->initialized` on macOS. The
  player joins the QEMU thread after the event loop; headless dump paths use
  `_exit`. The other direction too: a guest power-off returns from
  `qemu_main_loop` while the UI thread still holds the handle — the QEMU
  thread flags `stopped`, wakes the loop, and waits for `release()` before
  `qemu_embed_destroy` (which frees the input mutex → same assert).
