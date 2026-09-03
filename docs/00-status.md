# 0. Status and how to resume (updated 2026-09-03)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop; Super PI 1M on the Air 9:49 → 6:33. Patch 06 (2026-09-03, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **XP Super PI 1M on the Air 1:57, faster than the rig's real P4 1.7 (2:02)**. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio, QMP over a socketpair (`PLAYER_QMP`, `PLAYER_QMP_EXEC`). **Win98 and XP run in it on the M1 Air** with sound and tablet mouse. |
| 3D | **GL pass-through runs inside the player on Linux** (doc 12 steps 1–2, 2026-09-02): patches 30/31 + `embed/mglcntx_embed.c` (EGL surfaceless pbuffer as FBO 0, `glReadPixels` on swap) + API v4. Win98 wglgears in the player: 420 fps at 800×600 with the readback path, desktop returns on exit. Standalone `-display sdl` still works (500+ fps on the Air). **macOS too** (CGL, no drawable, FBO stand-in; `GL 2.1 Metal / Apple M1`, wglgears in the player on the Air). **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5, 2026-09-03): 575–600 fps wglgears, nothing copied per frame. **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air. Glide: no window, reported cleanly. |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) into an ISO. Must match the host's qemu-3dfx commit. |
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
  element size, constant into a V register). PC=24 (Direct3D) code
  still takes the patch 05 helpers: the inline variant with binary32
  shadows is the next x87 item. Test any change to
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
   2026-09-03 (XP Super PI 1M 1:57 on the Air, doc 13). Only the Windows
   host remains untested. Next on x87: the PC=24 (Direct3D) inline
   variant with binary32 shadows (doc 13 §Follow-ups).
2. **M3 (doc 12), in progress:** ~~vtable patch → EGL backend with
   readback → Win98 wglgears in the player on Linux → macOS CGL backend →
   wglgears in the player on the Air → dma-buf zero-copy on Linux~~ (done)
   → IOSurface zero-copy on macOS~~ (done) → Glide → fence-based sync
   instead of glFinish.
3. M2 mode table + pixel aspect; curated presets vs. rig CRT photos.
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
