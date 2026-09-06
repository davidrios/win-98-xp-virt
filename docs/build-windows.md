# Building and packaging for Windows (from Linux)

The Windows build is a **cross build**, done on the Linux machine that
does the rest of the work, and it is the only supported way to produce
Windows artefacts today: nobody here has a Windows box that builds, and
the whole stack — QEMU with a mingw toolchain, Rust for
`x86_64-pc-windows-gnu`, the Direct3D executor — crosses cleanly. The
result is a portable zip you copy to a Windows machine and unpack.

Track: `docs/tracks/m11-windows-host.md`. Names and layout: doc 07.

## The short version

```sh
scripts/win-cross.sh --build      # once: the cross container (~5 min, ~2 GB)
scripts/build-windows.sh          # qemu, rust, exec, guest-tools
scripts/package-windows.sh        # the zip, checked under wine
```

`build/win/package/2ksbox-<version>-windows-x86_64.zip` is the artefact.
Nothing above touches `build/qemu` or `target/release`: a checkout holds a
Linux build and a Windows build side by side, and `scripts/build.sh` and
`scripts/build-windows.sh` never fight over a directory.

## Why a container

QEMU needs glib, pixman, zlib and libepoxy **for the mingw target**.
Arch — this project's Linux host — packages `mingw-w64-gcc` and nothing
else; those four are AUR-only, and growing a mingw sysroot by hand is a
day nobody gets back. Fedora packages all of them, and QEMU's own Windows
CI uses exactly that base
(`qemu/tests/docker/dockerfiles/fedora-win64-cross.docker`), so a QEMU
build failure in here is one upstream would see too.

`packaging/windows/Dockerfile` adds two things to it:

- **rustup with `x86_64-pc-windows-gnu`.** The player links
  `libqemu-embed-i386.dll` and `libdisc` is linked *into* QEMU, so the
  Rust side has to be the same mingw ABI as the C side. (`-msvc` would
  mean a second toolchain and an import-library dance for no gain.)
- **Python 3.13.** Fedora's default is 3.14 and QEMU 9.2's `mkvenv`
  supports 3.8–3.13; `distlib` is installed for it because QEMU's
  fallback to pip's vendored copy is broken on pip ≥ 26 (CLAUDE.md's
  "found no usable distlib"), and the image build is the last moment
  there is a network.

`scripts/win-cross.sh` runs a command in it with the checkout bind-mounted
**at the same absolute path it has on the host**, so meson's absolute
paths keep working from both sides and, with rootless podman's
`--userns=keep-id`, everything it writes comes out owned by you.
`CONTAINER=docker` switches engines; `WIN_CROSS_FEDORA=` picks another
base.

## What each stage produces

| Stage | Output | Notes |
|---|---|---|
| `qemu` | `build/win/qemu/{qemu-system-i386,qemu-img,qemu-io}.exe`, `libqemu-embed-i386.dll` | `configure-qemu.sh --windows`; **WHPX detected and built in** |
| `rust` | `target/x86_64-pc-windows-gnu/release/{launcher,player,discx}.exe` | the embed DLL is found in `build/win/qemu` by `qemu-embed/build.rs` |
| `exec` | `build/win/d3dpt/d3dpt_exec.dll` | the Direct3D decoder + executor (doc 14) |
| `guest` | `guest-tools/out/guest-tools-*.iso` | guest code: host-independent, so only built if absent |

## The package

A Windows package is **one folder**, not a Unix prefix — the executables
at the top, every DLL beside them (which is exactly where the loader
looks, so there is no rpath and no PATH to set), the data directories
under it:

```
2ksbox.exe  2ksbox-player.exe  qemu-img.exe
libqemu-embed-i386.dll  d3dpt_exec.dll  <the mingw runtime>
pc-bios\  guest-tools\  shaders\  doc\
```

`launcher/src/paths.rs` knows both shapes: on Windows the prefix is the
executable's own directory and `pc-bios\` is the marker.

The DLLs beside them are a **closure walked from the import tables** with
`objdump`, not a hand-kept list — start from the four binaries, follow
every import, ship what is in the mingw sysroot and never what is
Windows' own (`kernel32`, `opengl32`, `d3d9`, the `api-ms-win-*` API
sets). Copying a system DLL into the folder is how an app ends up running
only on the machine that built it.

`scripts/package-windows.sh` then **runs the staged package under wine**,
from outside the checkout with an empty environment: the launcher must
answer `--paths` with paths inside the package, and the packaged
`qemu-img.exe` must actually write a qcow2 — which is also what proves
the DLL closure, since it cannot start with one missing. Wine is not the
target and a failure there is investigated rather than believed, but a
package that fails these has not been built correctly for any Windows.

## Acceleration

Windows' hardware acceleration is **WHPX** (the Windows Hypervisor
Platform), and the build has it. The launcher says so rather than saying
KVM: `Accel::Auto` is `whpx:tcg` there, "hardware acceleration required"
is `whpx`, and the wizard's hint asks `WHvGetCapability` — the feature
can be installed and still be off, and on a machine where Hyper-V or WSL2
already took the root partition that is exactly the answer that differs
from a guess. Turn it on with:

```
dism /online /enable-feature /featurename:HypervisorPlatform /all
```

A machine set to a fixed processor speed (doc 06's DOS family) is
emulated regardless, as everywhere else.

## What is not there yet

- **The Mesa/Glide pass-through inside the embed library** —
  `embed/mglcntx_embed.c` has an EGL backend for Linux and a CGL one for
  macOS; Windows needs a WGL one. Without it a Win98 guest's OpenGL is
  refused (`mesapt: no EGL on this host`) and everything else works.
  qemu-3dfx's own WGL backend (`hw/mesa/mglcntx_mingw.c`) is compiled
  into `qemu-system-i386.exe`, which is a different consumer: it has a
  window, and the embed library does not.
- **Zero-copy 3D frames.** The dma-buf ring is Linux and IOSurface is
  macOS; on Windows the executor's frames take the readback path, which
  is what the Direct3D device used everywhere until recently and is
  correct, just a copy per frame.
- **Nothing has run on real Windows yet.** Everything above is built and
  checked from Linux; under wine our `qemu-system-i386.exe` starts a
  machine, answers QMP and quits cleanly, and the packaged launcher and
  `qemu-img.exe` do their jobs — but the player's own window hangs inside
  wine's Vulkan, so the first real run on a Windows PC is still ahead.

## Running it there

Unzip anywhere and run `2ksbox.exe`. The user's own data — machines,
`discs.toml`, shader profiles, a downloaded preset collection — lives in
`%APPDATA%\2ksbox\data`, the same content the Linux build keeps in
`~/.local/share/2ksbox`.
