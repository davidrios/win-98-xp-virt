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
| `exec` | `build/win/d3dpt/d3dpt_exec.dll`, `build/win/wgl-probe.exe` | the Direct3D decoder + executor (doc 14), and the offscreen-GL diagnostic |
| `guest` | `guest-tools/out/guest-tools-*.iso` | guest code: host-independent, so only built if absent |

## The package

A Windows package is **one folder**, not a Unix prefix — the executables
at the top, every DLL beside them (which is exactly where the loader
looks, so there is no rpath and no PATH to set), the data directories
under it:

```
2ksbox.exe  2ksbox-player.exe  qemu-img.exe
libqemu-embed-i386.dll  d3dpt_exec.dll  <the mingw runtime>
pc-bios\  guest-tools\  shaders\  tools\  doc\
```

`launcher-core/src/paths.rs` knows both shapes: on Windows the prefix is
the executable's own directory and `pc-bios\` is the marker.

Both front ends are **windowed** programs there
(`windows_subsystem = "windows"`): a console-subsystem binary opens a
black terminal the moment someone double-clicks it and keeps it for the
session. `launcher-core/src/console.rs` pays that back on both sides —
`--paths` and friends borrow the console they were launched from when
there is one, and every subprocess the launcher starts (the player,
`qemu-img`) is given none, so nothing flashes. With no console to inherit
the player's output would be lost, which is exactly when it matters, so
it goes to `%APPDATA%\2ksbox\data\player.log` instead.

The DLLs beside them are a **closure walked from the import tables** with
`objdump`, not a hand-kept list — start from the four binaries, follow
every import, ship what is in the mingw sysroot and never what is
Windows' own (`kernel32`, `opengl32`, `d3d9`, the `api-ms-win-*` API
sets). Copying a system DLL into the folder is how an app ends up running
only on the machine that built it.

An import table does not name what a DLL **loads at run time**, and one
of ours does: Fedora's `mingw64-SDL2` is *sdl2-compat*, an `SDL2.dll`
that `LoadLibrary`s `SDL3.dll`. The first package therefore died on a
real Windows PC with "Failed loading SDL3 library". So a second pass
searches every staged binary for the name of any DLL that exists in the
sysroot and is not staged yet, and ships that too — deliberately broader
than the import tables, so the next runtime load is caught by the pass
instead of by a user. (It is why `SDL3.dll`, `libEGL.dll` and
`libGLESv2.dll` are in the folder; QEMU's SDL front end is the reason SDL
is there at all, and the qemu-3dfx patch makes it mandatory.)

`scripts/package-windows.sh` then **runs the staged package under wine**,
from outside the checkout with an empty environment: the launcher must
answer `--paths` with paths inside the package, and the packaged
`qemu-img.exe` must actually write a qcow2 — which is also what proves
the DLL closure, since it cannot start with one missing. Wine is not the
target and a failure there is investigated rather than believed, but a
package that fails these has not been built correctly for any Windows.

## OpenGL for a Win98 guest

A Win98 guest's 3D is qemu-3dfx's Mesa pass-through, and it needs a GL
context **inside the embed library**, where there is no window.
`embed/mglcntx_embed.c` gets one from EGL on Linux and CGL on macOS; on
Windows it uses WGL with a `WGL_ARB_pbuffer` standing in for the window,
which makes it the closest of the three to the Linux backend — macOS has
to fake a default framebuffer with an FBO because CGL pbuffers are gone,
while Windows has a real offscreen drawable.

There is still one window: a 1×1 popup, created and never shown. WGL has
no way to reach a device's pixel formats or its extension entry points
without a window and a context on it, and `wglCreatePbufferARB` itself
takes a DC to say which device to create the pbuffer on. Nothing is ever
drawn to it.

qemu-3dfx's own WGL backend (`hw/mesa/mglcntx_mingw.c`) stays where it is
for `qemu-system-i386.exe`, which *does* have a window. Linux and macOS
arrange that with weak symbols (patch 31); **Windows cannot** — a COFF
weak external is not an ELF weak definition, and marking those entry
points weak leaves `qemu-system-i386.exe` with undefined references
rather than a fallback. So that file is split instead: its WGL backend
behind `MESAGL_WGL_BACKEND`, its platform-independent helpers behind the
negation, and patch 10's meson hunk compiles it a second time — backend
half only — into the emulators alone. The static library both consumers
share ends up with the helpers and no backend at all, which is what the
weak marks were for. `strings` on the two artefacts is the check: the
emulator has the WGL backend's messages, the DLL the window-less one's,
and neither has the other's.

**`tools\wgl-probe.exe`, shipped in the package, is the diagnostic.** It
performs that whole sequence with no QEMU involved and prints where it
stops. Run it first on a Windows machine whose Win98 guest gets no 3D:
the answer is in its output rather than inside a VM. Under wine on the
Linux build host it passes on an AMD card (Mesa 26.2) — green clear, red
quad, right way up — which is how the sequence has been verified at all;
a real Windows driver is still ahead.

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
