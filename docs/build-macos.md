# Building and testing on macOS (Apple Silicon)

Everything below runs natively on arm64. Tested target: M1 MacBook Air.

## One-time setup

```sh
xcode-select --install                       # Apple clang + git
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config glib pixman sdl2 gnu-sed uv
brew install --cask xquartz                  # log out/in once after installing
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh      # Rust toolchain
```

Why each of the odd ones:
- **XQuartz** — qemu-3dfx's Mesa pass-through uses its GLX backend on
  macOS (it dlopens `/opt/X11/lib/libGL.dylib` at runtime); the patched
  `meson.build` hardcodes `-I/opt/X11/include` and links
  `-L/opt/X11/lib -lX11 -lXxf86vm -lGL -framework OpenGL`. Without it:
  `GL/glcorearb.h not found`, then link errors, then no 3D at runtime.
- **sdl2** — the patch makes SDL2 mandatory
  (`error('Featuring qemu-3dfx required SDL2')`).
- **gnu-sed** — `sign_commit` uses GNU `sed -i` syntax;
  `scripts/prepare-qemu.sh` puts Homebrew's `gsed` first on PATH when present.
- The Khronos `GL/glcorearb.h` is additionally vendored in
  `third_party/khronos` and put on the include path by
  `scripts/configure-qemu.sh`, so the header itself never depends on
  XQuartz's Mesa headers version.

## Clone

```sh
git clone --recurse-submodules --shallow-submodules git@github.com:davidrios/win-98-xp-virt.git
cd win-98-xp-virt
```

## Rust side (player, libdisc, launcher) — ~1 min

```sh
cargo build --release
cargo test --workspace
target/release/player          # window with the test pattern, rendered via wgpu → Metal
```

What to look for: color bars with a 1-px white border, a white line sweeping
down (one pass ≈ 8 s), sharp edges (nearest sampling, integer-scaled 4:3),
no tearing. Resize the window: bars stay 4:3 and pixel-aligned.

## QEMU side (patched with qemu-3dfx) — ~10–15 min on an M1 Air

```sh
scripts/prepare-qemu.sh        # overlay hw/3dfx + hw/mesa, apply 3dfx patch + our queue, sign_commit
scripts/configure-qemu.sh      # uv-managed Python 3.12, --disable-werror
ninja -C build/qemu qemu-system-i386 qemu-system-x86_64
```

`configure-qemu.sh` sets `MACOSX_DEPLOYMENT_TARGET` to the running macOS
version (unless you preset it). Without that, the 15.4+ SDK's availability
annotations (`strchrnul is only available on macOS 15.4 or newer`) produce a
wall of `-Wunguarded-availability-new` warnings, because QEMU's configure
detects the function and uses it unguarded. Local builds therefore target the
machine they're built on; release packaging will choose its own floor.

Order matters: if you re-run `prepare-qemu.sh` later (e.g. after pulling a
patch-queue change), run `configure-qemu.sh` again before `ninja` — a
refreshed overlay can make ninja regenerate the build with default options
(notably `werror` back on).

Smoke tests:

```sh
build/qemu/qemu-system-i386 --version                     # 9.2.4
printf 'info mtree\nquit\n' | build/qemu/qemu-system-i386 -machine pc -display none \
    -monitor stdio -net none 2>/dev/null | grep -E 'glidept|glidelfb|glideshm|mesapt'
# expect the four pass-through MMIO regions
build/qemu/qemu-system-i386 -machine pc -cpu max -m 256 -display cocoa   # BIOS screen in a window
```

Notes:
- **`-cpu max`** is what qemu-3dfx recommends for TCG on Apple Silicon
  (x86-64-v2 feature level); our machine definitions will pin `pentium2` /
  `pentium3` models for guest compatibility — both are TCG-only here.
- TCG needs JIT. Locally built, ad-hoc-signed binaries can `MAP_JIT` fine;
  the `com.apple.security.cs.allow-jit` entitlement only matters once we ship
  a hardened, notarized `.app` (doc 07).
- If configure complains about Python, it means uv isn't on PATH — the
  script never uses the system interpreter.

## Spike A, step 1: Win98 + guest wrappers (hand-run)

Goal: prove qemu-3dfx accelerates a guest on this Mac. You need your own
Win98 SE install ISO; everything else comes from the repo.

```sh
# guest wrappers, built from the exact qemu-3dfx commit the host is signed with
brew install mingw-w64 xorriso && guest-tools/build-wrappers.sh
#   → guest-tools/out/guest-tools-3dfx-<rev>.iso   (or one built on Linux — same commit)

# 1. install Win98 (cirrus = in-box high-color driver, no extra guest driver needed)
qemu-img create -f qcow2 ~/vms/win98.qcow2 4G
build/qemu/qemu-system-i386 -machine pc -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom ~/isos/Win98SE.iso -boot d \
  -vga cirrus -display sdl -net none \
  -audiodev coreaudio,id=snd -device sb16,audiodev=snd
# 2. after install, boot with the guest-tools ISO attached
build/qemu/qemu-system-i386 -machine pc -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom guest-tools/out/guest-tools-3dfx-*.iso \
  -vga cirrus -display sdl -net none \
  -audiodev coreaudio,id=snd -device sb16,audiodev=snd
```

**`-display sdl` is mandatory for 3D.** qemu-3dfx creates its host GL
context on QEMU's SDL2 window: `sdl_display_valid()` in the patched
`ui/sdl2.c` exits with `mesapt: invalid sdl display` otherwise, and
`-display sdl,gl=on` is rejected too. Cocoa is fine for 2D-only sessions.

**Why 3D died with `X Error … BadDrawable, Major opcode 129 (Apple-DRI)`
(seen on Sequoia 15.7):** upstream's macOS path is GLX-on-XQuartz. On a
Cocoa SDL window, `ui/sdl2.c` passes the `NSWindow*` to the Mesa backend,
which the GLX backend uses as an X11 window id — XQuartz rejects it.
Upstream needs an X11-capable SDL2 on XQuartz (not Homebrew's). Our queue
patch `02-mesa-sdlgl-on-darwin` builds qemu-3dfx's SDL/native-OpenGL
backend (`mglcntx_sdlgl.c`) on macOS instead: the context is created with
`SDL_GL_CreateContext` on the Cocoa window, Apple's `OpenGL.framework` is
loaded, no X server at runtime. (XQuartz stays a *build-time* link
dependency until the link flags are patched.) Rebuild after `git pull`:
prepare → configure → ninja.

**Mouse/keyboard grab under `-display sdl`:** hotkey is left-Ctrl +
left-Option + G (SDL `KMOD_LCTRL|KMOD_LALT`; the right-hand keys don't
count). Alternatives: `-display sdl,grab-mod=lshift-lctrl-lalt`
(Shift+Ctrl+Option+G) or `grab-mod=rctrl`. While a 3D title runs
fullscreen, qemu-3dfx's own window logic (`fxui_grab`) holds the grab.

In the guest: copy `D:\WIN9X\*` to `C:\WINDOWS\SYSTEM`, copy `D:\GAMEDIR\*`
to a folder like `C:\GLTEST`, reboot, run `C:\GLTEST\WGLGEARS.EXE`.
Accelerated = a smooth gears window and a Mesa/host renderer string (not
"GDI Generic"); qemu-3dfx also prints context messages on the host
terminal (`mesapt: DLL loaded`, `glcntx: …`). For Glide, any Glide title
works once `GLIDE2X.DLL` is in `SYSTEM`; for OpenGL games drop
`OPENGL32.DLL` next to the game EXE (Quake 2 is the classic check).

Known trap (fixed in our queue since 2026-09-02): stock QEMU 9.2.4 TCG
blue-screens Win98 SE with `exception 0D` on the first boot after setup
(upstream issue 2987); `prepare-qemu.sh` applies the upstream fix.

Notes: `-cpu pentium3` is the compatibility-safe choice for Win9x under
TCG and the floor our wrappers are built for (SSE1); qemu-3dfx's own
`-cpu max` advice exists because *their* wrappers target x86-64-v2. Keep
RAM ≤ 512 MB (Win9x VCache limit). Warm reboot currently freezes on the
Air (under investigation — shut down and cold start instead). Record
renderer string + fps under "Result" in `docs/spikes/spike-a-macos.md`.
