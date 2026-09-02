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

## Spike A, step 1 (hand-run, ~20 min)

Goal: prove qemu-3dfx accelerates a guest on this Mac at all, independent of
our code. Easiest path is the prebuilt arm64 package from
https://github.com/startergo/qemu-3dfx-macos/releases (Homebrew-style
tarball into /opt/homebrew) — or our own build above once it works. Boot a
Win98 image, install the matching guest wrappers (built from the *same*
qemu-3dfx commit as the host binary — for our build, that's
`third_party/qemu-3dfx` at the stamped commit), run a GL/Glide title, and
record the renderer string and rough fps in
`docs/spikes/spike-a-macos.md` → Result.
