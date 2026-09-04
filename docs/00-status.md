# 0. Status and how to resume (updated 2026-09-04, late)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Tracks (pick one per session)

Work runs as parallel tracks, one session each, so the handoffs stay
separate. Each track has its own doc with scope, owned files, state,
build/test loop and ordered next steps:

| Track | Doc | Owns | Next |
|---|---|---|---|
| **M4 — paravirtual Direct3D device** (DLL path, executor, tests) | `docs/tracks/m4-d3d-device.md` | `d3dpt/exec`, `d3dpt/hw/d3dpt_mm.c`, `guest-tools/src/d3dpt/`, `scripts/test.sh`, doc 14 | a real game on the device |
| **M7 — XP display driver** (`d3dpt-vga`, miniport + display DLL, DirectDraw/Direct3D DDI) | `docs/tracks/m7-display-driver.md` | `d3dpt/hw/d3dpt_vga.c`, `d3dpt/d3dpt_fb.h`, `guest-tools/src/d3dptvid/`, `tools/xp-driver-test.sh`, doc 15 | M7c, the Direct3D DDI |
| Everything else (M3 Glide/fences, M2, M5, M6, macOS bring-up, x87) | this doc's "Next steps" | — | as listed |

Rules: branch `track/<name>-<topic>` off `main`, rebase on `main` before
pushing, merge to `main` when green. Shared files (`d3dpt/d3dpt_proto.h`,
`d3dpt/exec/`, `scripts/test.sh`, `player/`, `CLAUDE.md`, this doc) are
edited minimally and the commit message says which track. In this doc a
track edits only its own state-table row, its "Next steps" line and this
table; everything else about the track lives in its track doc. The Mac
side pulls `main`.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26.6.2 since 2026-09-03, for KosmicKrisp / ADR-007). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop; Super PI 1M on the Air 9:49 → 6:33. Patch 06 (2026-09-03, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **XP Super PI 1M on the Air 1:57, faster than the rig's real P4 1.7 (2:02)**. Patches 07/08 (2026-09-04): upstream x87 helper fixes (pseudo-NaN transcendentals, fcomi flags) and seven decoder / segment fixes from the 2025–26 fuzzing work, hand-rebased onto 05/06; suite green under KVM and TCG. Patch 09 (same day): the 10.0 repeated-string series, 12–16 % on rep movs/stos per `tools/string-bench.py` (DOS microbench, compares two QEMU binaries side by side). Upstream survey the same day: QEMU 10.x/11.x add nothing we need (11 dropped 32-bit *hosts*, not the i386 target; qemu-3dfx has no 10.x patch), staying on 9.2.4 is a decision. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio, QMP over a socketpair (`PLAYER_QMP`, `PLAYER_QMP_EXEC`). **Win98 and XP run in it on the M1 Air** with sound and tablet mouse. |
| 3D | **GL pass-through runs inside the player on Linux** (doc 12 steps 1–2, 2026-09-02): patches 30/31 + `embed/mglcntx_embed.c` (EGL surfaceless pbuffer as FBO 0, `glReadPixels` on swap) + API v4. Win98 wglgears in the player: 420 fps at 800×600 with the readback path, desktop returns on exit. Standalone `-display sdl` still works (500+ fps on the Air). **macOS too** (CGL, no drawable, FBO stand-in; `GL 2.1 Metal / Apple M1`, wglgears in the player on the Air). **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5, 2026-09-03): 575–600 fps wglgears, nothing copied per frame. **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air. Glide: no window, reported cleanly. |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) and, since 2026-09-03, the WineD3D set from JHRobotics/wine9x (Wine 1.7.55 for 9x/XP: per-game D3D8/D3D9/WINED3D DLLs + system-wide switchers) with a D3D9 smoke test (`D3D9TEST.EXE`), the display-mode probe (`MODETEST.EXE`) and the reference workloads `D3DGAME9.EXE` / `D3DGAME8.EXE` (doc 14 P0a) into an ISO. **Rig (P4 + GeForce 6200), 2026-09-03: both run.** First-run fixes: ground triangle winding (top face was culled), shader path now applies the per-cube material (all cubes were one colour), d3dgame8 windowed swaps with COPY_VSYNC so both pace at the refresh rate by default (85 fps on the rig's monitor is vsync, `-novsync` for throughput), console output also goes to `d3dgameN.log`. **Golden captures landed 2026-09-03** (`reference/d3d/rig-2026-09-03/`, diff with `tools/bmpdiff.py`): d3dgame9 frame 300 windowed, fixed function and `-shader`. The rig's log explained the `-shader` oddity: d3dx9_36's HLSL compiler refuses ps_1_1 (X3539), so the cubes ran vs_1_1 + fixed-function pixel stage while the log claimed fixed function. **Rendering is frozen at that build** (the golden set must stay comparable; the rig stays off for now): only the log line naming the shader case and the elapsed-ms summary were fixed, no pixel changes. Mask the HUD bars (wall time) when diffing. d3dgame8 windowed with COPY_VSYNC runs at half refresh (43 fps at 85 Hz) on the GeForce driver: real behaviour, recorded. Must match the host's qemu-3dfx commit. **Win98 and XP (2026-09-03): wglgears and D3D9TEST run in the player on both** (WineD3D needs no Microsoft DX runtime in the guest; XP needs the FXPTL.SYS step first, see gotchas). XP D3D9TEST on the Air: adapter reported as "GeForce 6800" (WineD3D's GL-renderer mapping), x87 PC=24 after CreateDevice, 377–504 fps windowed 640×480. |
| Guests | Images live outside the repo: `~/vms/win98.qcow2` and `~/vms/winxp.qcow2` on both machines (the XP image was copied to the Linux box 2026-09-03; it has FXPTL.SYS installed, no d3dx9, no games yet), plus `~/vms/scratch.img` on Linux (64 MB FAT32, seen as E:, for files out of the guest). Win98 SE on the Air: installed, repaired to PCI-bus enumeration (must be an ACPI `SETUP /p j` install or repaired — doc 06/build-macos). XP on the Air: installed, boots in the player in ~30 s (same as the rig, P4 1.7); integer 1.3–2× the rig (7-Zip), x87 FP 21 % on softfloat (Super PI 1M 9:49 vs 2:02), 104 % with patch 06 (1:57) — `reference/benchmarks/`. |
| Direct3D device (M4) | **Works end to end on Linux, P0–P4 closed 2026-09-03/04** (doc 14 has the per-milestone detail and numbers). Executor: DXVK d3d9 native (ADR-007), `third_party/dxvk` + `patches/dxvk/` (01 macOS shim, 02/05 optional features, 03 portability, 04 headless WSI), verified on RADV and on the Air over KosmicKrisp (macOS 26). Transport: SysBus device `d3dpt/hw` (QEMU patch 40; register page 0xdfffe000, 64 MiB window 0xd8000000), protocol `d3dpt/d3dpt_proto.h` **v4**, decoder+executor `d3dpt/exec` → `build/d3dpt/libd3dpt_exec.so` dlopened by the device, frames through the GL frame path (`embed/embedfx.c`). Guest: `guest-tools/src/d3dpt/` — `d3d9.c` (+`d3d9_res.h`, `d3d9_p3.h`: resources, surfaces, shaders, declarations, queries, guest-side state blocks, cube maps) and `d3d8.c` (D3D8 wrappers in the same TU); vtables generated from mingw's headers (`gen_vtbl.py`, `gen_vtbl8.py`); ISO folder `D3DPT\` with D3D9.DLL, D3D8.DLL and the test EXEs. **Acceptance so far:** XP D3DGAME9 and D3DGAME8 frames on the device are byte-identical to the native DXVK build and 1089 pixels (tolerance 8) from the rig golden; D3DFEAT9 (hand-assembled SM1.1, no D3DX) byte-identical guest vs native including query results; D3D9TEST 2840 fps vs 1100 on WineD3D-in-guest. Open: no real game run yet (needs a title on the XP image), volume textures, swap-chain objects, GetFrontBuffer, lockable DEFAULT surfaces, lost-device protocol, zero-copy present (readback via GetRenderTargetData today), macOS build of `libd3dpt_exec`, decoder thread (everything runs on the vCPU thread under the BQL). |
| XP display driver (M7, doc 15) | **M7a landed 2026-09-04, M7b (DirectDraw DDI) first cut the same evening; track doc `docs/tracks/m7-display-driver.md`.** `d3dpt-vga` PCI adapter (`d3dpt/hw/d3dpt_vga.c`: QEMU's stdvga core + a register BAR, `d3dpt/d3dpt_fb.h`; `-vga none -device d3dpt-vga`) and the driver pair `guest-tools/src/d3dptvid/` (video miniport, display driver, INF, `DRVINST.EXE` unattended installer, `SETMODE.EXE`, `DDTEST.EXE`), built by `guest-tools/build-driver.sh` with mingw-w64's DDK headers + ReactOS' public-domain `ddrawint.h` (`DRIVER\` on the guest-tools ISO). XP desktop from the host's mode table (42 modes, 1024×768×32@85 etc.) straight out of VRAM with no copy inside QEMU, no flash on mode switches, KVM verified. DirectDraw: HAL accepted by dxg, surfaces in VRAM, real page flips through the OFFSET register, cached VRAM mappings (miniport maps VRAM itself); DDTEST 640×480×16 flip chain 4762 fps, ×32 6383 fps, windowed HEL blit 305 fps. Findings: `EngModifySurface` needs `HOOK_SYNCHRONIZE`, `DDCAPS_GDI` makes dxg drop the HAL, XP SP3's Logo dialog ignores every registry policy. `tools/xp-driver-test.sh` runs the guest loops headless. Not yet: M7c (Direct3D DDI, designed in doc 15), hardware cursor, mode table from the player (M2), real vblank, macOS run, a DX5–7 game. |
| Tests | Integration / e2e only (CLAUDE.md policy, 2026-09-04): `scripts/test.sh all` runs the host tools (x87 oracle, embed Mesa backend, decoder + executor, the native DXVK reference scene within budget of the rig golden, the native feature test) and the guest stage (DOS x87 battery under TCG; XP headless on the D3D device from a `snapshot=on` view of `~/vms/winxp.qcow2` with a fresh scratch FAT disk and `RUN.BAT`, driven over QMP: D3DGAME9 / D3DGAME8 pixel-identical to the native frame outside the HUD, D3DFEAT9 byte-identical with the same query lines). 12 checks, ~2 min on the Linux box under KVM, all green at 2026-09-04. Local only by decision (2026-09-04): CI stays off the suite, it needs the images and a GPU. |
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
# Direct3D device: build the executor once, then the ISO after every guest change
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
scripts/test.sh          # the regression suite, host stage (~30 s); `all` adds XP + DOS guests (~2 min)
tools/string-bench.py --qemu old/qemu-system-i386 --qemu build/qemu/qemu-system-i386   # rep movs/stos/scas ns per element, A/B
build/d3dpt-exec-test x.bmp 120 60                 # host-only check of decoder + executor
guest-tools/build-wrappers.sh                      # ISO with D3DPT\ (D3D9.DLL, D3D8.DLL, tests)
# XP test loop (Linux; -accel kvm -cpu host is fine, TCG identical): scratch FAT disk as E:
# for files out of the guest (creation recipe in the gotchas), CD as D:
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 512 -hda ~/vms/winxp.qcow2 \
  -hdb ~/vms/scratch.img -cdrom guest-tools/out/guest-tools-3dfx-d00e858.iso -vga cirrus -net none \
  -usb -device usb-tablet -qmp unix:/tmp/qmp.sock,server,nowait
tools/qmpc.py /tmp/qmp.sock keys meta_l+r; tools/qmpc.py /tmp/qmp.sock type 'cmd /c xcopy D:\D3DPT E:\D3DPT\ /I /Y'; tools/qmpc.py /tmp/qmp.sock keys ret
tools/qmpc.py /tmp/qmp.sock keys meta_l+r; tools/qmpc.py /tmp/qmp.sock type 'E:\D3DPT\D3DGAME9.EXE -frames 600 -dump 300 E:\OUT\G9.BMP'; tools/qmpc.py /tmp/qmp.sock keys ret
tools/qmpc.py /tmp/qmp.sock json '{"execute":"system_powerdown"}'   # clean XP shutdown
mcopy -i ~/vms/scratch.img@@1048576 ::/OUT/G9.BMP g9.bmp && tools/bmpdiff.py reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp g9.bmp --mask 0,368,270,112
# host log: qemu-system-i386: info: d3dpt: … (device, executor, and every guest DLL log line)
# XP on our display driver (M7 track, doc 15): -vga none -device d3dpt-vga instead of -vga cirrus,
# driver installed once per image from the ISO's DRIVER\ (DRVINST.EXE -reboot); headless loops:
guest-tools/build-driver.sh && tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 ddtest
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
- XP paints the whole screen white around a Cirrus mode switch (a D3D
  title going fullscreen and back: 640×480 white, then 800×600 white for
  ~0.6 s while the desktop repaints; seen on the VGA surface of a bare
  `qemu-system-i386` too, so it is guest-drawn, not ours). Since
  2026-09-04 the player publishes black instead of any uniform
  single-colour frame within 1.5 s of a real mode switch
  (`qemu_vm.rs`, `SWITCH_GRACE`); the log counts them
  (`[display] N transitional frame(s) after the switch shown black`).
  On the way in nothing shows because the VGA surface is frozen while
  3D is active. The M7 driver path never had it (miniport zeroes VRAM).
- Win98: after wglgears / D3D9TEST exit, the mouse stops working in the
  guest (2026-09-03, untriaged: wrapper hook/cursor state vs the
  player's tablet? check whether keyboard still works and whether a
  second launch restores it).
- XP has no driver for `-vga std` (Bochs VBE): basic 640×480×16. The M4
  test loop runs XP with `-vga cirrus` (inbox GD5446 driver); the M7 track
  replaces it with `-vga none -device d3dpt-vga` + our driver (doc 15),
  which is where XP is headed.
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
  Guest-side debugging: the DLL's log lines reach the host log in order
  with the device's own lines (`qemu-system-i386: info: d3dpt: guest: …`),
  which is the only reliable channel when the guest freezes (files on the
  scratch disk stay in the guest's write cache). A process that ends
  without `DLL_PROCESS_DETACH` in that log was terminated or crashed at
  exit; exit-path bisection with `_cexit()` + `ExitProcess()` vs
  `return 0` found the stack smash above. mingw's d3d8 headers are
  `#pragma pack(4)` on i386, d3d9's are not: never hand-copy a D3D8 struct
  without the pack. Swapping the ISO under a running guest:
  QMP `blockdev-change-medium` on `ide1-cd0`.
  Hand-assembling SM1 bytecode: opcode numbers are D3DSIO_* (`m4x4` is 20,
  not 24 = `m3x2`); a wrong opcode compiles fine in DXVK and draws
  nothing — dump the SPIR-V with `DXVK_SHADER_DUMP_PATH` and read it.
  **Rebuild QEMU after a protocol bump** (`prepare-qemu.sh` + ninja): the
  device carries its own copy of the header and refuses a newer DLL
  (0xc0000142 with a `d3dpt.log` version line).
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.
- KVM on Linux (`-accel kvm -cpu host`) works for XP with every device of
  ours and is far faster than TCG; the x87 patches are TCG-only. TCG stays
  the Apple Silicon path and `scripts/test.sh` accepts both.
- Kernel-mode drivers with mingw-w64 (M7 track, doc 15): no `ntddk.h` in a
  miniport, `winddi.h` needs the vendored `ddrawint.h`, GCC emits
  `memcpy`/`memset` calls even freestanding; the debugger is a device
  register echoed to the QEMU log. `grim` hangs inside the agent sandbox:
  use QMP `screendump` on a standalone `-display none` run.
- Keys typed while a full-screen DirectDraw window is up go to that window
  and are lost: chain guest commands with `&` on one `cmd /k` line
  (`qmpc.py type` knows `& ( ) , ; = ' " * % + ! > < |`), copy logs to the
  FAT scratch disk at the end. Swap the CD under a running guest with QMP
  `blockdev-change-medium` (device `ide1-cd0`).

## Next steps, in order

Per-track order lives in the track docs: **M4** → `docs/tracks/m4-d3d-device.md`
(a real game on the device, present/pacing, the Air build, the x87
real-world number); **M7** → `docs/tracks/m7-display-driver.md` (M7c the
Direct3D DDI, a driver stage in `scripts/test.sh`, cursor / vblank / mode
table). ADR-008 (2026-09-04): the M7 driver is the long-term XP shape; the
M4 DLL device stays for Win98 and as the executor's harness. Below, the
items nobody owns yet:

1. **M3 (doc 12):** Glide offscreen path, fence-based sync instead of
   glFinish. Both untouched since 2026-09-03.
2. **M2** mode table + pixel aspect: on XP it is the M7 device's mode table
   fed from the player (M7 track item); Win98 / the CRT presets still need
   the player side.
3. **Player:** a hardware-cursor sprite (the M7 driver and cirrus both
   define cursors the player ignores today), the vblank signal for guests.
4. M5 libdisc; M6 launcher.

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
