# Track: M11 — the Windows host (build, package, run)

The handoff for a session working on Windows as a *host*: cross-building
the whole stack from Linux, packaging it, and closing the gaps a Windows
host has that Linux and macOS do not. Read `docs/00-status.md` first for
the global picture and the track rules, then this file, then
`docs/build-windows.md`, which is the prose version of the build.

Opened 2026-09-06 on `track/m11-windows-host` (worktree
`.claude/worktrees/m11-windows-host`), because the user wants to see what
2ksbox is like on Windows and the only machine that builds is this Linux
one. Windows had been "untested" since M1 (doc 08).

## Scope and files (this track owns them)

- The cross toolchain: `packaging/windows/Dockerfile`,
  `scripts/win-cross.sh`.
- The build: `scripts/build-windows.sh`, the `--windows` mode of
  `scripts/configure-qemu.sh` and of `scripts/build-d3dpt-exec.sh`.
- The package: `scripts/package-windows.sh`.
- Windows branches of shared code: `embed/mglcntx_embed.c` (the WGL
  backend) and `tools/wgl-probe.c`, `launcher-core/src/console.rs`,
  `player/src/qmp.rs`,
  `launcher/src/paths.rs` + `player.rs` + `wizard.rs` + `bundle.rs`
  (the layout and the WHPX naming), `d3dpt/hw/d3dpt_exec_load.c`,
  `d3dpt/exec/*.cpp`, `libdisc/src/bin/discx.rs`.
- Docs: `docs/build-windows.md`, this file, the M11 rows of
  `docs/00-status.md` and doc 08.
- Shared with other tracks (rebase first, edit minimally, say so in the
  commit): `patches/qemu/13-perfmap-darwin.patch`,
  `31-mesa-ctx-weak.patch` and `50-cdimage-block-driver.patch`,
  `scripts/configure-qemu.sh`, `qemu-embed/build.rs`, and the shared
  layer of `embed/mglcntx_embed.c` (M3's file).

## State (2026-09-06)

The whole stack cross-builds and packages. `scripts/build-windows.sh`
then `scripts/package-windows.sh` produce a ~92 MB zip holding
`2ksbox.exe`, `2ksbox-player.exe`, `qemu-img.exe`,
`libqemu-embed-i386.dll` (all 20 embed API entry points exported),
`d3dpt_exec.dll`, the mingw runtime closure, `pc-bios\`, the guest-tools
ISO and `tools\wgl-probe.exe`. **WHPX is detected and built in**, and the
embed library has a WGL backend, so a Win98 guest's OpenGL has somewhere
to go.

What has actually been *run*, all under wine on the build host:

- `qemu-system-i386.exe -version` prints 9.2.4 with the qemu-3dfx
  signature; `-M pc -accel tcg -m 64 -display none -qmp stdio` starts the
  machine, negotiates QMP, answers and quits cleanly.
- The staged launcher answers `--paths` with every companion inside the
  package, from outside the checkout with an empty environment, and its
  `--wizard-new` runs the packaged `qemu-img.exe` to create a real
  qcow2 — which is also the DLL closure's proof.
- The player's own window does **not** get that far: it initialises
  Vulkan through wine's winevulkan (radv, "not a conformant
  implementation") and then hangs before the first frame. Not chased:
  wine is not the target, and the answer costs less on a real Windows
  machine than in a wine debugger.

### First run on real Windows (2026-09-06)

The user ran the package on their PC. It starts, and two things were
wrong — both of them things only a real Windows could show:

1. **A terminal window.** The launcher was a console-subsystem binary, so
   double-clicking it opened a black console and kept it for the session.
   Both front ends are `windows_subsystem = "windows"` now, with
   `launcher-core/src/console.rs` paying back what that costs: the debug
   verbs borrow the console they were launched from when there is one
   (`AttachConsole(ATTACH_PARENT_PROCESS)`, and only when nothing has
   already handed us a stdout — a redirection or a test harness's pipe
   must be left alone), and every subprocess the launcher starts is given
   `CREATE_NO_WINDOW`, so `qemu-img` no longer flashes a console per call
   and the player no longer sits behind one. Verified under wine both
   ways: `package-windows.sh`'s `--paths` check still reads its pipe, and
   `2ksbox.exe --host-check` from a `.bat` under `cmd` prints into that
   console.
2. **"Failed loading SDL3 library" on Play.** Fedora's `mingw64-SDL2` is
   *sdl2-compat*: an `SDL2.dll` that `LoadLibrary`s `SDL3.dll`. The DLL
   closure walks import tables, and a runtime load is not in one, so
   SDL3 was never shipped and the player could not start on a machine
   without its own. `package-windows.sh` now has a second pass over the
   staged binaries' strings for sysroot DLL names that are not staged
   yet (`SDL3.dll`, and ANGLE's `libEGL`/`libGLESv2` behind it).

A windowless launcher has nowhere to put the player's output, which is
precisely when a start-up failure needs reading, so `player::spawn`
redirects it to `%APPDATA%\2ksbox\data\player.log` in that case —
appended, with a header per run.

Still open from that first run: whether a machine actually boots, and
what WHPX does with it.

### The QMP monitor's `fd=` on Windows (2026-09-06)

The next run got as far as starting a machine and QEMU refused the
command line: `-chardev socket,id=qmp0,fd=7368: File descriptor '7368' is
not a socket`. 7368 is a `SOCKET` handle, and `fd=` on Windows is not one.
Every socket call in QEMU's Windows build is an `os-win32.h` wrapper
(`#define getsockopt qemu_getsockopt_wrap`) whose first line is
`_get_osfhandle(fd)`, so the number has to be a **C-runtime descriptor**
— and `util/qemu-sockets.c`'s `fd_is_socket()` is the getsockopt that
fails on a raw handle.

`_open_osfhandle()` makes that descriptor, but *whose* table it lands in
depends on which CRT the module linking it uses, and the player cannot
know that about the library it loads (both are msvcrt.dll today; nothing
enforces it). So the handle crosses the boundary as a handle and the
library converts: **`qemu_embed_socket_to_fd()`, embed API v7**. On Unix
an fd is already an fd and it returns the value unchanged, so
`player/src/qmp.rs` has one `into_raw` shape on both platforms.

Anyone pulling this must rebuild the embed library before the player
links (the API version moved).

### Both front ends (2026-09-06)

`scripts/package-windows.sh --qt` rolls a second, complete package whose
`2ksbox.exe` is `launcher-qt`, so the two can be unzipped side by side on
one machine and compared. Qt crosses better than expected: Fedora has
`mingw64-qt6-*` to link against, a native Qt of the same version for the
tools that run here, and a `x86_64-w64-mingw32-qmake-qt6` whose `-query`
splits `QT_INSTALL_*` (target) from `QT_HOST_*` (host) exactly the way
cxx-qt's cargo-only build wants. Two things had to be said:

- `CXX_QT_AUTORCC_OPTIONS=--no-zstd` (a supported cxx-qt env var, found
  after nearly wrapping `rcc` by hand): the host rcc has zstd and the
  mingw Qt6Core does not, so the default algorithm produces a resource
  that asks the target for `qResourceFeatureZstd()` and will not link.
- Qt deployment by hand, since Fedora has no cross `windeployqt`: the
  plugin directories, the QML module trees, and a `qt.conf` so both
  resolve relative to the executable. The DLL closure had to grow roots:
  a platform plugin or a QML module's DLL sits in a subdirectory and
  imports half of Qt, and nothing above it names either — it now walks
  every binary anywhere in the package.

**Open: the Qt binary does not start under wine.** It faults on a call to
address 0 before `main` prints anything, with either subsystem, and with
no display attached wine logs a window-creation attempt first. The egui
binary in the same folder, with the same DLLs, answers `--paths` fine, so
the package is not the problem, and the backtrace is one unwalkable frame
at address 0 (which is what a jump through a null thunk looks like). Two
candidates, in order: cxx-qt's whole-archive static initialisers (the QML
type registration that runs before `main`, which would break on real
Windows too), and wine's own Qt 6 support. The next real Windows run
decides which — the packaging checks report rather than fail for `--qt`
so the artefact exists to try.

### OpenGL in the embed library

Written the same day: `embed/mglcntx_embed.c` grew a Windows branch using
WGL with a `WGL_ARB_pbuffer` as the drawable — the Linux backend's shape
(the pbuffer is FBO 0, the swap is a readback of its back buffer), not
the macOS one's FBO stand-in, because Windows has a real offscreen
drawable and macOS does not. One 1×1 window is created and never shown,
because WGL cannot reach a device's pixel formats or its ARB entry points
without one. Everything goes through epoxy, which the embed library
already links.

Three things this needed elsewhere:

- **The weak-symbol trick does not work on Windows.** Marking
  `hw/mesa/mglcntx_mingw.c`'s entry points weak the way patch 31 does for
  GLX and SDL links the DLL fine and then leaves `qemu-system-i386.exe`
  with `undefined reference to MGLCreateContext` — a COFF weak external
  is not an ELF weak definition, and ld.bfd does not fall back to the
  aliased body (the object's own calls to its own weak symbols are
  undefined too). The file is split instead: its WGL backend behind
  `MESAGL_WGL_BACKEND`, its platform-independent helpers behind the
  negation, and patch 10's meson hunk compiles it once more — backend
  half only — into the emulators alone, so the archive both consumers
  share holds the helpers and no backend. `strings` says it worked: the
  emulator carries the WGL backend's messages and the DLL the
  window-less one's, neither the other's.
- The shared WGL layer at the top of `mglcntx_embed.c` *defines*
  `PIXELFORMATDESCRIPTOR`, `WORD`, `DWORD`, `BYTE` because Linux and
  macOS have no `windows.h`; on Windows those give way to the real ones.
- `HPBUFFERARB` in that layer is a record of what the guest asked for,
  not a handle, and on Windows the name belongs to `wglext.h` — it is
  now `FakePBuffer`, which is what it always was.

`tools/wgl-probe.c` is the test: the same sequence without QEMU, drawing
the frame `tools/embed-3d-test.c` draws and checking the same three
pixels. It passes under wine on this host (Mesa 26.2, RX 9060 XT), which
is what says the sequence itself is right; it ships in the package as
`tools\wgl-probe.exe` so the same question can be asked on the machine
where a guest's 3D actually fails.

### The six things that were in the way

1. **`tcg/perf.c`** — patch 13 compiles it on every host, and half of it
   (jitdump) needs `mmap` and `flockfile`. The perfmap half is plain
   stdio and stays; `-jitdump` on Windows says it is unavailable.
2. **libdisc's link libraries** are per-platform and meson forbids a
   chained ternary, so patch 50's `link_args` became an `if/elif`
   (`-lkernel32 -lntdll -luserenv -lws2_32 -ldbghelp`, from
   `rustc --print native-static-libs`).
3. **`d3dpt_exec_load.c`** dlopens the executor: `LoadLibrary` /
   `GetProcAddress` behind three macros.
4. **The player's QMP monitor is a socketpair**, which Windows has not
   got. A loopback pair stands in, and the accepted connection is proved
   to be our own (peer address == our connecting socket's own address)
   before the monitor is handed to it — otherwise a local process that
   raced for the port would be handed a QMP monitor. QEMU's `fd=` is a
   plain integer on both platforms.
5. **The executor** compiled almost unchanged against mingw's own
   `<windows.h>` / `<d3d9.h>` instead of DXVK's stand-ins for them (x86-64
   has one calling convention, so the COM ABI is the same). It loads
   plain `d3d9.dll` at run time: the system implementation, or a DXVK
   build dropped next to the player, which the loader prefers because it
   searches the executable's directory first. `access()` became a
   `fopen` probe and `%zu` needs `__USE_MINGW_ANSI_STDIO`.
6. **The package layout.** A Unix prefix's `bin`/`lib`/`libexec`/`share`
   split is wrong on Windows, where the loader wants the DLLs beside the
   exe and the user wants one folder. `paths.rs` now knows both shapes;
   the strings at the call sites did not change.

## Build / test loop

```sh
scripts/win-cross.sh --build          # once
scripts/build-windows.sh              # qemu, rust, exec, guest
scripts/package-windows.sh            # zip + the wine checks
scripts/build-windows.sh rust         # the inner loop while changing Rust
```

`scripts/test.sh` is unchanged and stays a Linux suite: it needs guest
images and a GPU, and now a Windows host too. The Windows evidence is
`package-windows.sh`'s own checks.

## Next steps, in order

1. **Try both packages on the user's Windows PC**, the Qt one first —
   if it faults there too, it is cxx-qt's static initialisers and not
   wine, and the fix is ours.
2. **Boot a machine on the user's Windows PC.** The launcher runs there
   and Play now starts the player; what a guest does under WHPX is the
   next unknown.
3. **A Win98 guest with 3D on real Windows.** The WGL backend is written
   and its sequence passes under wine (`tools/wgl-probe.exe`), but no
   guest has used it.
4. **The installer.** Doc 07 wants an installer as well as the portable
   zip. QEMU's own `mingw32-nsis` recipe is in the cross image's reach.
5. **Zero-copy frames** through a DXGI shared handle, the Windows answer
   to the dma-buf ring and IOSurface.
6. **A second Windows check that boots a guest**, once (1) says what
   actually happens. The shape to aim for is `xp-driver-test.sh`'s: drive
   the machine over QMP, pull the artefacts out, diff a frame.

## Gotchas found here

- Fedora's default Python breaks QEMU's `mkvenv` (3.14 vs 3.8–3.13); the
  image pins 3.13 and installs a real `distlib` for it.
- `scripts/win-cross.sh` forwards a **whitelist** of environment
  variables into the container. A knob that seems to be ignored is
  probably not on it.
- A `wine` command whose output goes through a pipe can look like a hang;
  redirect to a file.
- A kept copy of the cross image's DLLs (`build/win/sysroot-bin`) is a
  snapshot of an older image: the first `--qt` package took one from
  before Qt was installed and shipped a Qt launcher with no `Qt6Core.dll`.
  Both sysroot copies are refreshed on every packaging run now.
- Running the packaged binaries under wine with a display attached puts
  wine's crash dialog **on the user's desktop** when one of them faults.
  Unset `DISPLAY`/`WAYLAND_DISPLAY` for anything that might crash.
- The launcher's Windows data directory is `%APPDATA%\2ksbox\data`
  (`directories`' own convention), not `%APPDATA%\2ksbox`.
- A DLL loaded with `LoadLibrary` is not in any import table, so a
  closure walked from those alone is not a closure. Fedora's SDL2 is
  sdl2-compat and loads SDL3 that way; `package-windows.sh` has a
  strings-based second pass for the next one.
- `windows_subsystem = "windows"` is what stops the terminal window, and
  it costs a console for the debug verbs and a place to put a child's
  output. Both are in `launcher-core/src/console.rs`; neither is
  optional once the attribute is set.
