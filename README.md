# win98-xp-virt (working name)

An open-source, cross-platform stack for running Windows 98 and Windows XP as
"native vintage boxes": hardware-accelerated period 3D (Direct3D / Glide /
OpenGL), pixel-accurate CRT-shaded video output, and faithful CD-ROM drive
emulation that works with raw disc dumps — including era copy protection
(SafeDisc, SecuROM, etc.) — of discs you own.

Built on QEMU. Runs on Linux, Windows, and macOS, with Apple Silicon as a
first-class target.

## What exists vs. what we build

| Piece | Status |
|---|---|
| x86 emulation/virtualization | Exists — QEMU (TCG on ARM hosts, KVM/WHPX on x86) |
| Guest 3D acceleration | Exists — qemu-3dfx (integrate, package, test) |
| Win9x guest GPU drivers | Exists — SoftGPU + qemu-3dfx guest wrappers (package) |
| CRT shader ecosystem | Exists — libretro slang shaders via librashader (library, not RetroArch) |
| **Player: in-process QEMU + pixel-accurate CRT-shaded display** | **We build** (Rust, wgpu + librashader) |
| **Companion launcher (machine library, guided creation)** | **We build** (Rust) |
| **Raw CD-ROM backend (cue/bin, subchannel, C2, CD-DA)** | **We build** (Rust "libdisc"; libmirage as reference) |

Authentic-hardware Win98 emulation (real Voodoo, real S3) is 86Box's territory
and explicitly **out of scope** — we don't duplicate that work.

## Design docs

1. [Goals and non-goals](docs/01-goals.md)
2. [Architecture: in-process QEMU, process model, threading](docs/02-architecture.md)
3. [Display pipeline: pixel accuracy, CRT shaders, latency](docs/03-display-pipeline.md)
4. [3D acceleration: qemu-3dfx and guest drivers](docs/04-3d-acceleration.md)
5. [CD-ROM backend: raw images and copy protection](docs/05-cdrom-backend.md)
6. [Guest machines: Win98 and XP reference configs](docs/06-guest-machines.md)
7. [Frontend: machine library, UX, input, audio](docs/07-frontend.md)
8. [Roadmap and milestones](docs/08-roadmap.md)
9. [Reference hardware rig](docs/09-reference-hardware.md)
10. [Decision records](docs/10-decisions.md)

## Building

```sh
git clone --recurse-submodules <repo-url>   # submodules: qemu (gitlab.com, pinned v9.2.4),
cd win98-xp-virt                            #             third_party/qemu-3dfx (github.com)
# already cloned without submodules? → git submodule update --init --depth 1

cargo build --release        # player + libdisc + launcher stub
target/release/player        # M0: native window with test pattern (integer-scaled 4:3)

scripts/prepare-qemu.sh      # overlay qemu-3dfx devices + embed/, patches, sign
scripts/configure-qemu.sh    # configure (uv-managed python — needs uv, ninja, glib, pixman, SDL2)
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so
cargo build --release        # player links libqemu-embed (rpath into build/qemu)

# M1: boot something in-process (firmware path needed until machine bundles land)
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -m 32 \
  -drive file=path/to/floppy.img,format=raw,if=floppy -boot a -vga std -net none
# PLAYER_DUMP=frame.png PLAYER_DUMP_SEQ=150 dumps guest frame #150 and exits (headless check)
# --shader <preset.slangp> (or PLAYER_SHADER=) runs a libretro slang preset, e.g.
#   target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- ...
# PLAYER_DUMP_OUT=out.png dumps the shaded frame (GPU readback) at PLAYER_DUMP_SEQ and exits
# PLAYER_KEYS="120:enter,360:ctrl+g" presses keys/chords at guest frames (headless input test)
# PLAYER_AUDIO_NULL=1 keeps the audio ring without a device and logs QEMU's writes
# audio: the player adds -audiodev embed,id=embed0 automatically; attach e.g.
#   -machine pc,pcspk-audiodev=embed0   or   -device sb16,audiodev=embed0
```

macOS / Apple Silicon specifics: [docs/build-macos.md](docs/build-macos.md).

CI (`.github/workflows/ci.yml`) is currently manual-only — trigger it from the
Actions tab (`workflow_dispatch`).

## License

GPL-2.0. Non-negotiable in practice: the core links QEMU (GPL-2.0)
in-process. Original code is Rust wherever possible (see ADR-004 in
[decision records](docs/10-decisions.md)); C only inside QEMU/qemu-3dfx and
in guest-side era code.
