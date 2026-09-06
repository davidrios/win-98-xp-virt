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

0. [**Status and how to resume**](docs/00-status.md) — read first
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
11. [M1 embed API design](docs/11-m1-embed-api.md)
12. [M3 window-less GL context provider design](docs/12-m3-context-provider.md)

## Building

```sh
git clone --recurse-submodules <repo-url>   # submodules: qemu (gitlab.com, pinned v9.2.4),
cd win98-xp-virt                            #             third_party/qemu-3dfx (github.com)
# already cloned without submodules? → git submodule update --init --depth 1

cargo build --release        # player + libdisc + launcher stub
target/release/player        # M0: native window with test pattern (integer-scaled 4:3)

scripts/prepare-qemu.sh      # overlay qemu-3dfx devices + embed/, patches, sign
scripts/configure-qemu.sh    # configure (uv-managed python — needs uv, ninja, glib, pixman, SDL2)
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so   # .dylib on macOS
cargo build --release        # player links libqemu-embed (rpath into build/qemu)

# M1: boot something in-process (firmware path needed until machine bundles land)
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -m 32 \
  -drive file=path/to/floppy.img,format=raw,if=floppy -boot a -vga std -net none
# PLAYER_DUMP=frame.png PLAYER_DUMP_SEQ=150 dumps guest frame #150 and exits (headless check)
# --shader <preset.slangp> (or PLAYER_SHADER=) runs a libretro slang preset, e.g.
#   target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- ...
# --shader-params <name=value,...> (or PLAYER_SHADER_PARAMS=) overrides the preset's own
#   parameter defaults by name, e.g. --shader-params BRIGHTBOOST=1.4,GAMMA_INPUT=2.4 — this is
#   what a launcher shader profile (launcher/src/shader_profile.rs) resolves to
# PLAYER_DUMP_OUT=out.png dumps the shaded frame (GPU readback) at PLAYER_DUMP_SEQ and exits
# PLAYER_KEYS="120:enter,360:ctrl+g" presses keys/chords at guest frames (headless input test);
#   each press is held PLAYER_KEYS_HOLD frames (default 6 ≈ 100 ms) — a down+up in one flush is a
#   zero-length press that a game polling the keyboard state never sees
# PLAYER_AUDIO_NULL=1 keeps the audio ring without a device and logs QEMU's writes
# PLAYER_AUDIO_MS=60 (default) is the audio cushion QEMU keeps ahead of the host audio
#   thread: the output latency, and how late QEMU's main loop may run (TCG, the D3D
#   executor) before a gap is heard. `qemu-embed: audio:` lines on stderr count gaps
#   and dropped audio when they happen; raise it if they do, lower it under KVM.
# PLAYER_LATENCY=1 prints publish→present latency percentiles every 240 guest frames
# PLAYER_REFRESH_MS=16 (default) is the guest frame pull interval (QEMU's own default is 30)
# QMP: the player always attaches a control monitor over a socketpair (no socket file).
#   PLAYER_QMP=1 logs every QMP event (SHUTDOWN/RESET/STOP/... are logged regardless)
#   PLAYER_QMP_EXEC='{"execute":"query-status"}' (or a JSON array of requests) runs
#   commands once the guest has drawn its first frame and prints the replies
# Direct3D pass-through (doc 14): the d3dpt device is always present; it loads
#   build/d3dpt/libd3dpt_exec.so (D3DPT_EXEC_LIB) and DXVK (D3DPT_DXVK_LIB) on the
#   guest's first use. Build: scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh &&
#   ninja -C build/dxvk && scripts/build-d3dpt-exec.sh; guest side: D3DPT\ on the ISO.
#   D3DPT_DUMP_DIR=dir D3DPT_DUMP_EVERY=60 makes the executor write every 60th
#   presented frame as dir/frame-NNNNNN.ppm (works with bare qemu-system-i386 too).
#   Guest side: D3DPT_TRACE=1 or a file d3dpt_trace.on next to the DLL writes the
#   creation/lock/upload/present calls to d3d8_trace.log / d3d9_trace.log; a DLL
#   that cannot open the device forwards Direct3DCreateN to the system DLL.
#   While the device is active the player shows the VGA surface again after 1 s
#   without a presented frame if the guest drew on it (a game's error dialog,
#   a DirectShow movie, a crashed process): "[display] no 3D frame for …".
# audio: the player adds -audiodev embed,id=embed0 automatically; attach e.g.
#   -machine pc,pcspk-audiodev=embed0   or   -device sb16,audiodev=embed0
```

macOS / Apple Silicon specifics: [docs/build-macos.md](docs/build-macos.md).

## Packaging (Linux)

The shipped product is named **2ksbox** (ADR-011); `win98-xp-virt` stays
the repository's working name, and so does the user's data directory.

```sh
scripts/package-linux.sh              # build/package/2ksbox-<version>-linux-<arch>.tar.zst
scripts/package-linux.sh --with-shaders   # + the ~80 MB preset collection
```

It stages the launcher, the player, the embed library, our `qemu-img`,
QEMU's firmware and the guest-tools ISO into one relocatable prefix
(doc 07's install layout), checks that the staged launcher resolves all of
them *inside* the package with a scrubbed environment, and rolls a
tarball. The extracted tree runs where it lands — `bin/2ksbox` — and the
`install.sh` inside it copies the tree into a prefix (`~/.local` by
default) and adds a desktop entry (`com._2ksbox.Launcher.desktop`, the
application ID the launcher's window also reports as its `app_id`):

```sh
tar xf 2ksbox-*.tar.zst && cd 2ksbox-*
./install.sh                 # or --prefix /usr/local, or --uninstall
```

The tarball ships no system libraries, so it wants a host much like the
one that built it. The **Flatpak** is the portable answer — it builds
everything from source against `org.freedesktop.Sdk`, so the ~191
libraries come from the runtime:

```sh
scripts/package-flatpak.sh          # build, install --user, smoke check
flatpak run com._2ksbox.Launcher
```

Set `FLATPAK_BUILD_DIR` (and flatpak's own `FLATPAK_USER_DIR`) if the
build tree — a whole QEMU plus a release Rust workspace, ~12 GB — should
not land on your root filesystem.

The Flatpak builds with no network, as Flathub requires: every crate is a
declared source with a checksum in `packaging/flatpak/cargo-sources.json`.
Run `scripts/gen-flatpak-cargo-sources.sh` and commit the result whenever
a dependency changes. `launcher --paths` prints where a given build looks for each
companion — the first thing to ask when something says a file is missing.

CI (`.github/workflows/ci.yml`) is currently manual-only — trigger it from the
Actions tab (`workflow_dispatch`).

## License

GPL-2.0, non-negotiable in practice for everything that links QEMU
(GPL-2.0) in-process: the `player`, `qemu-embed`, and `libdisc`, which is
compiled into QEMU itself.

The **launcher** and the `shader-chain` crate it shares with the player are
**GPL-2.0-or-later** (ADR-009). The launcher links no QEMU code — it spawns
the player as a separate process — and it does link Apache-2.0 crates
(egui's `ab_glyph`, winit's `dpi`, `ring` under `ureq`'s rustls) that GPLv2
cannot take and GPLv3 can.

Original code is Rust wherever possible (see ADR-004 in
[decision records](docs/10-decisions.md)); C only inside QEMU/qemu-3dfx and
in guest-side era code. The GPLv2 text is in [COPYING](COPYING), and every
third-party component is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

**If you package or redistribute the player, read this.** Its dependency
tree contains **Apache-2.0-only** crates — `winit`, `cpal`, `ab_glyph`,
`codespan-reporting` and `rspirv` among them — and Apache-2.0 is
incompatible with GPLv2, which the player is pinned to because it links
QEMU. This is a property of the modern Rust GUI stack rather than a
dependency we chose carelessly (`winit` alone settles it), and removing the
crates individually would change nothing: being clean means dropping wgpu
and librashader, i.e. the CRT shader chain the project exists for. **We
ship player binaries anyway**, with complete source and build scripts, and
the reasoning — including the alternatives measured and rejected — is
ADR-010. If your distribution's policy can't accept that, please open an
issue rather than patching around it; the clean fix (QEMU in its own
process) is designed and costed, not hypothetical.
