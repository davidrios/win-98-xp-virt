# 0. Status and how to resume (updated 2026-09-02)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop. Patch 06 (branch `worktree-x87-inline-tcg`, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **on the Air, XP Super PI 1M 9:49 → 1:57 (2026-09-03), faster than the rig's 2:02**. Not yet merged. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio. **Win98 runs in it on the M1 Air** with sound and tablet mouse. |
| 3D | qemu-3dfx GL pass-through works **standalone** (`qemu-system-i386 -display sdl`, 500+ fps wglgears on the Air). **Not yet in the player** — needs the M3 window-less context provider (doc 12). Under the player a GL app is refused cleanly; a Glide app still exits QEMU (patch 04). |
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
`PLAYER_SHADER` (README). Firmware must be passed with `-L qemu/pc-bios`
until machine bundles exist. Test image: FreeDOS 1.3 floppy
(`build/images/144m/x86BOOT.img`, git-ignored; `tools/x87-guest-test.py`
fetches FD13-FloppyEdition.zip from ibiblio and extracts it).
macOS specifics: `docs/build-macos.md`. x87 tests need `brew install nasm
mtools`; `tools/x87-guest-test.py` downloads the FreeDOS floppy itself.

## Known issues / open threads

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
- x87 under TCG is all helper calls into 80-bit softfloat; patch 05 does
  the 53/24-bit-precision common case on the host FPU. Branch
  `worktree-x87-inline-tcg` (patch 06, doc 13) keeps the x87 stack as host
  doubles across instructions in TCG: 21.6 (softfloat) / 10.6 (patch 05) /
  2.9 ns per op on x86-64. Air bring-up done 2026-09-03 (doc 13 §"Bring-up
  on the Air"): two aarch64 backend paths upstream never runs needed
  fixes (UMOV element size, constant into a V register); with both, XP
  Super PI 1M is 1:57 twice (9:49 on softfloat, `x87-fast=off` control
  confirms) and Win98 boots fine. Ready to merge; main still has patch 05
  only. Test any change to
  it with `tools/x87-fast-test.c` (x86-64 host oracle) and
  `tools/x87-guest-test.py` (on/off identical under TCG; needs nasm,
  mtools, the FreeDOS floppy). Benchmarks inside a .COM must keep data on
  a separate page from code or QEMU's SMC invalidation dominates.
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.

## Next steps, in order

1. M1 close-out: latency DONE (Air: p50 6–10 / p95 15–17 / max 18 ms,
   the 60 Hz vsync-phase floor; Linux identical); XP boot + Super PI DONE
   + 7-Zip DONE (integer 1.3–2× the rig, x87 FP 21 % on softfloat, 104 %
   with patch 06: Super PI 1M 1:57 twice on 2026-09-03; `x87-fast=off`
   control back at softfloat pace; Win98 boots fine). Left: merge patch 06
   to main; QMP over socketpair (doc 11 §QMP).
2. **M3 (pulled forward, doc 12):** `30-3dfx-ui-vtable` patch →
   `embed/mglcntx_embed.c` (EGL pbuffer, compat profile) with readback
   bring-up → dma-buf import into wgpu → macOS CGL/IOSurface → Glide.
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
