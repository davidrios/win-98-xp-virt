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
  backend, still to write), `player/src/qmp.rs`,
  `launcher/src/paths.rs` + `player.rs` + `wizard.rs` + `bundle.rs`
  (the layout and the WHPX naming), `d3dpt/hw/d3dpt_exec_load.c`,
  `d3dpt/exec/*.cpp`, `libdisc/src/bin/discx.rs`.
- Docs: `docs/build-windows.md`, this file, the M11 rows of
  `docs/00-status.md` and doc 08.
- Shared with other tracks (rebase first, edit minimally, say so in the
  commit): `patches/qemu/13-perfmap-darwin.patch` and
  `50-cdimage-block-driver.patch`, `scripts/configure-qemu.sh`,
  `qemu-embed/build.rs`.

## State (2026-09-06)

The whole stack cross-builds and packages. `scripts/build-windows.sh`
then `scripts/package-windows.sh` produce a ~92 MB zip holding
`2ksbox.exe`, `2ksbox-player.exe`, `qemu-img.exe`,
`libqemu-embed-i386.dll` (all 19 embed API entry points exported),
`d3dpt_exec.dll`, the mingw runtime closure, `pc-bios\` and the
guest-tools ISO. **WHPX is detected and built in.**

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

Nothing has run on real Windows yet. That is the next thing, and it needs
the user's PC.

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

1. **Run it on the user's Windows PC.** A machine created in the wizard,
   booted, with WHPX. Everything below is guesswork until this happens.
2. **A WGL backend for `embed/mglcntx_embed.c`** so a Win98 guest gets
   OpenGL (M3's Mesa pass-through) inside the embed library. The file
   already has the seam: ~15 `plat_*` functions, and the macOS branch is
   the closer model of the two (a hidden window for the context, an FBO
   standing in for the default framebuffer, `MesaGLSetFunc` redirecting
   `glBindFramebuffer(…, 0)` to it — patch 32). The shared WGL layer at
   the top of the file *defines* `PIXELFORMATDESCRIPTOR` and friends
   because Linux and macOS have no `windows.h`; on Windows those
   typedefs must give way to the real ones.
3. **The installer.** Doc 07 wants an installer as well as the portable
   zip. QEMU's own `mingw32-nsis` recipe is in the cross image's reach.
4. **Zero-copy frames** through a DXGI shared handle, the Windows answer
   to the dma-buf ring and IOSurface.
5. **A second Windows check that boots a guest**, once (1) says what
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
- The launcher's Windows data directory is `%APPDATA%\2ksbox\data`
  (`directories`' own convention), not `%APPDATA%\2ksbox`.
