# 0. Status and how to resume (updated 2026-09-03)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26.6.2 since 2026-09-03, for KosmicKrisp / ADR-007). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop; Super PI 1M on the Air 9:49 → 6:33. Patch 06 (2026-09-03, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **XP Super PI 1M on the Air 1:57, faster than the rig's real P4 1.7 (2:02)**. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio, QMP over a socketpair (`PLAYER_QMP`, `PLAYER_QMP_EXEC`). **Win98 and XP run in it on the M1 Air** with sound and tablet mouse. |
| 3D | **GL pass-through runs inside the player on Linux** (doc 12 steps 1–2, 2026-09-02): patches 30/31 + `embed/mglcntx_embed.c` (EGL surfaceless pbuffer as FBO 0, `glReadPixels` on swap) + API v4. Win98 wglgears in the player: 420 fps at 800×600 with the readback path, desktop returns on exit. Standalone `-display sdl` still works (500+ fps on the Air). **macOS too** (CGL, no drawable, FBO stand-in; `GL 2.1 Metal / Apple M1`, wglgears in the player on the Air). **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5, 2026-09-03): 575–600 fps wglgears, nothing copied per frame. **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air. Glide: no window, reported cleanly. |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) and, since 2026-09-03, the WineD3D set from JHRobotics/wine9x (Wine 1.7.55 for 9x/XP: per-game D3D8/D3D9/WINED3D DLLs + system-wide switchers) with a D3D9 smoke test (`D3D9TEST.EXE`), the display-mode probe (`MODETEST.EXE`) and the reference workloads `D3DGAME9.EXE` / `D3DGAME8.EXE` (doc 14 P0a) into an ISO. **Rig (P4 + GeForce 6200), 2026-09-03: both run.** First-run fixes: ground triangle winding (top face was culled), shader path now applies the per-cube material (all cubes were one colour), d3dgame8 windowed swaps with COPY_VSYNC so both pace at the refresh rate by default (85 fps on the rig's monitor is vsync, `-novsync` for throughput), console output also goes to `d3dgameN.log`. **Golden captures landed 2026-09-03** (`reference/d3d/rig-2026-09-03/`, diff with `tools/bmpdiff.py`): d3dgame9 frame 300 windowed, fixed function and `-shader`. The rig's log explained the `-shader` oddity: d3dx9_36's HLSL compiler refuses ps_1_1 (X3539), so the cubes ran vs_1_1 + fixed-function pixel stage while the log claimed fixed function. **Rendering is frozen at that build** (the golden set must stay comparable; the rig stays off for now): only the log line naming the shader case and the elapsed-ms summary were fixed, no pixel changes. Mask the HUD bars (wall time) when diffing. d3dgame8 windowed with COPY_VSYNC runs at half refresh (43 fps at 85 Hz) on the GeForce driver: real behaviour, recorded. Must match the host's qemu-3dfx commit. **Win98 and XP (2026-09-03): wglgears and D3D9TEST run in the player on both** (WineD3D needs no Microsoft DX runtime in the guest; XP needs the FXPTL.SYS step first, see gotchas). XP D3D9TEST on the Air: adapter reported as "GeForce 6800" (WineD3D's GL-renderer mapping), x87 PC=24 after CreateDevice, 377–504 fps windowed 640×480. |
| Guests | Win98 SE on the Air: installed, repaired to PCI-bus enumeration (must be an ACPI `SETUP /p j` install or repaired — doc 06/build-macos). XP on the Air: installed, boots in the player in ~30 s (same as the rig, P4 1.7); integer 1.3–2× the rig (7-Zip), x87 FP 21 % on softfloat (Super PI 1M 9:49 vs 2:02), 104 % with patch 06 (1:57) — `reference/benchmarks/`. |
| D3D executor (M4) | **ADR-007 (2026-09-03): DXVK d3d9 native everywhere; macOS over Mesa's KosmicKrisp (needs macOS 26), MoltenVK unsupported** — spike C (`docs/spikes/spike-c-dxvk-native-macos.md`): DXVK master hard-requires five features MoltenVK 1.4.2 lacks. `third_party/dxvk` submodule (3.1.0 d7ac258), patch queue `patches/dxvk/` (01 macOS shim, 02 geometry shaders optional, 03 portability enumeration, 04 headless WSI, 05 fill-mode-non-solid optional; `scripts/prepare-dxvk.sh` + `configure-dxvk.sh` → `build/dxvk`). `libdxvk_d3d9` builds on the Air; `tools/dxvk-d3d9-test.cpp` (off-screen triangle → BMP) runs up to DXVK's refusal on MoltenVK (`shaderCullDistance`), the same binary is the KosmicKrisp acceptance test. **`tools/d3dgame9-native.cpp`** compiles the unmodified reference scene (`guest-tools/src/d3dgame9.c`) as a native program over a 150-line Win32-on-SDL2 shim (`tools/d3dgame-native/win32_sdl.h`) against `libdxvk_d3d9`: same options, log and `-dump` BMPs. **First executor-vs-rig diff, Linux 2026-09-03 (RADV, RX 9060 XT):** the queue builds unchanged on Linux, the off-screen test passes, and the fixed-function frame 300 vs `reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp` (HUD masked) differs in 32.9 % of pixels but only 1089 pixels (0.35 %) beyond a channel tolerance of 8 and 16 pixels beyond 32: edge rasterization and texture-filter noise, visually identical. The `-shader` golden cannot be compared natively (no d3dx9 to compile vs_1_1; the harness falls back to fixed function) and stays untouched with the frozen build. **macOS 26 over KosmicKrisp, same day (Air, LunarG SDK 1.4.357.1, `docs/build-macos.md`):** one more refusal (`fillModeNonSolid`, patch 05: optional, wireframe → solid), then both harnesses pass: 1095 pixels beyond tolerance 8 / 16 beyond 32 (RADV: 1089 / 16), 120 fps windowed; the headless WSI (patch 04) gives the identical BMP, 384 fps. The executor is verified on both platforms. **P1 host side (2026-09-03):** protocol `d3dpt/d3dpt_proto.h` (one header for guest DLL, QEMU device, executor), guest encoder `d3dpt/d3dpt_enc.h`, executor `d3dpt/exec` (C++ over DXVK, dlopened by the device so QEMU stays C; `scripts/build-d3dpt-exec.sh` → `build/d3dpt/libd3dpt_exec.so`), DXVK patch 04 (headless WSI). `tools/d3dpt-exec-test.cpp` replays D3D9TEST through the executor with the guest's own encoder: 120 frames, one batch per frame, 4300 fps with readback at 640×480, oversized draws refused. **P1 host side (2026-09-03):** protocol `d3dpt/d3dpt_proto.h` (one header for guest DLL, QEMU device, executor), guest encoder `d3dpt/d3dpt_enc.h`, executor `d3dpt/exec` (C++ over DXVK, dlopened by the device so QEMU stays C; `scripts/build-d3dpt-exec.sh` → `build/d3dpt/libd3dpt_exec.so`), DXVK patch 04 (headless WSI). `tools/d3dpt-exec-test.cpp` replays D3D9TEST through the executor with the guest's own encoder: 120 frames, one batch per frame, 4300 fps with readback at 640×480, oversized draws refused. **P1 closed 2026-09-03 on Linux: the XP guest's D3D9TEST draws through the device.** QEMU patch 40 + overlay `d3dpt/hw` (SysBus, register page at 0xdfffe000, 64 MiB window at 0xd8000000, executor dlopened on first attach; `embed/embedfx.c` routes frames into the GL frame path), guest `guest-tools/src/d3dpt/d3d9.c` (C COM, 129 methods via a generated vtable, msvcrt, fxlib mapper; ISO folder `D3DPT\`). XP in the player, 640×480 windowed, 3000 frames: **2840 fps on the device vs 1100 fps for WineD3D-in-guest** on the same test and host (RADV); native host replay of the same batches 4300 fps. **P2 closed 2026-09-04:** buffers, textures (all D3D9 formats, mip chains, DXT), surfaces (texture levels, backbuffer, render targets, depth, system-memory), GetRenderTargetData, StretchRect, shaders (bytecode passthrough), scissor; every lockable resource has a guest shadow and Unlock forwards the dirty box. **`D3DGAME9 -frames 600 -dump 300` from XP on the device is byte-identical to the native DXVK frame** and sits at the same 1089 pixels from the rig golden; fullscreen 8888 identical, fullscreen 565 3884 pixels (no rig 565 golden yet), mode switches through ChangeDisplaySettings. 600 frames in 750 ms (windowed 8888) with the dynamic vertex buffer and the render-to-texture pass. **P3 (2026-09-04, protocol v3):** vertex declarations, int/bool constants, queries, state blocks (guest-side: recorded and D3DSBT_ALL/PIXEL/VERTEX), cube textures, DEFAULT-pool offscreen surfaces, ColorFill, StretchRect, UpdateSurface/UpdateTexture (host stages through a system-memory surface when the target is not lockable), clip planes, scissor. The feature test `guest-tools/src/d3dfeat9.c` (hand-assembled vs_1_1/ps_1_1, no D3DX; native build `tools/d3dfeat9-native.cpp`) **is byte-identical between the XP guest and the native DXVK build**, logs included (occlusion count, constant getters). Method gaps show up in `d3dpt.log` as "not implemented". |
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
- d3dpt: `D3DPT_EXEC_LIB` / `D3DPT_DXVK_LIB` point the device and the executor
  at the libraries when not run from the repo root (defaults:
  `build/d3dpt/libd3dpt_exec.so`, `build/dxvk/src/d3d9/libdxvk_d3d9.so.0`
  relative to the cwd, then the bare sonames). The executor sets
  `DXVK_WSI_DRIVER=Headless` itself. A guest process that finds no
  executor sees `D3DPT_STATUS_NO_EXEC` and the DLL refuses to load
  (0xc0000142), same shape as the missing-FXPTL case. Protocol changes
  bump `D3DPT_PROTO_VERSION` in `d3dpt/d3dpt_proto.h`; DLL, executor and
  device all check it. Driving XP from a script: `-cdrom` the ISO,
  `qmpc.py … keys meta_l+r`, `type 'D:\\D3DPT\\D3D9TEST.EXE 3000'`, `keys ret`;
  QMP `system_powerdown` shuts XP down cleanly. Getting files out of XP:
  a raw FAT32 image as `-hdb` (`truncate -s 64M`, `sfdisk` one partition
  at 2048, `mkfs.fat -F 32 --offset 2048`) appears as E: and is read with
  `mcopy -i img@@1048576 ::/path out` — XP writes lazily, so list it a few
  seconds after the program exits. Running EXEs from the CD works, but
  their logs then land in `C:\`; xcopy the folder to E: first.
  Hand-assembling SM1 bytecode: opcode numbers are D3DSIO_* (`m4x4` is 20,
  not 24 = `m3x2`); a wrong opcode compiles fine in DXVK and draws
  nothing — dump the SPIR-V with `DXVK_SHADER_DUMP_PATH` and read it.
  **Rebuild QEMU after a protocol bump** (`prepare-qemu.sh` + ninja): the
  device carries its own copy of the header and refuses a newer DLL
  (0xc0000142 with a `d3dpt.log` version line).
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.

## Next steps, in order

ADR-008 (2026-09-04): a real XP display driver is the long-term shape (M7, staged: framebuffer → DirectDraw DDI → Direct3D DDI) on the same transport and executor; the DLL device stays for Win98 and as the harness. Not started.

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
   2026-09-03~~ (`reference/d3d/rig-2026-09-03/`; rendering frozen at
   that build), then the
   DXVK-native spike (P0b) — ~~decided 2026-09-03~~, ADR-007: DXVK over
   KosmicKrisp on macOS 26. Next: upgrade the Air to macOS 26 + LunarG SDK
   (KosmicKrisp ICD), build `third_party/dxvk` with `patches/dxvk/`, run
   the native off-screen test (`tools/dxvk-d3d9-test.cpp`), then
   `build/d3dgame9-native -frames 600 -dump 300 g9.bmp` and
   `tools/bmpdiff.py reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp g9.bmp --mask 0,368,270,112`
   (the first executor-vs-rig number) — ~~done on Linux 2026-09-03~~
   (0.35 % of pixels beyond tolerance 8, see the state table) ~~and on
   the Air over KosmicKrisp the same evening~~ (1095 pixels, patch 05).
   ~~P1 (transport +
   device)~~ done on Linux 2026-09-03 (XP D3D9TEST on the device, 2.6×
   WineD3D-in-guest). ~~P2~~ closed 2026-09-04: D3DGAME9 from XP is
   byte-identical to the native DXVK frame. ~~P3~~ feature set landed
   2026-09-04 (D3DFEAT9 byte-identical guest vs native). **Next:** a real
   game on the device (doc 04 matrix: needs an XP title installed on the
   image; Max Payne / GTA:VC are the acceptance titles), volume textures,
   swap-chain methods (GetRasterStatus, gamma), DEFAULT-pool surface
   LockRect readback, lost-device protocol; the zero-copy present (DXVK Vulkan interop → dma-buf / IOSurface ring)
   instead of GetRenderTargetData; the macOS build of `libd3dpt_exec`
   (KosmicKrisp); P4 d3d8.dll over the same device. Fresh D3DPT DLL + ISO after every guest
   change: `guest-tools/build-wrappers.sh`; the whole chain without a
   guest: `build/d3dpt-exec-test`.
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
