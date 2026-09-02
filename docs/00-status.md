# 0. Status and how to resume (updated 2026-09-02)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, Sequoia 15.7). Windows untested. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio. **Win98 runs in it on the M1 Air** with sound and tablet mouse. |
| 3D | qemu-3dfx GL pass-through works **standalone** (`qemu-system-i386 -display sdl`, 500+ fps wglgears on the Air). **Not yet in the player** — needs the M3 window-less context provider (doc 12). Under the player a GL app is refused cleanly; a Glide app still exits QEMU (patch 04). |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) into an ISO. Must match the host's qemu-3dfx commit. |
| Guests | Win98 SE on the Air: installed, repaired to PCI-bus enumeration (must be an ACPI `SETUP /p j` install or repaired — doc 06/build-macos). XP install in progress on the Air. |
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
(`build/images/144m/x86BOOT.img`, git-ignored; re-download from ibiblio).
macOS specifics: `docs/build-macos.md`.

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
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.

## Next steps, in order

1. M1 close-out: latency number + XP benchmark from the Air; QMP over
   socketpair (doc 11 §QMP).
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
