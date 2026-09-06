# 7. Frontend: player + companion launcher

Two Rust apps (ADR-005): the **player** runs one machine in one window; the
**launcher** manages the library. Together they feel like a UTM-style app.

## Player

- Opens a machine bundle (`machine.toml` + disks + disc shelf); boots QEMU
  in-process; renders through the doc 03 pipeline.
- **Window:** the shaded display fills it; aspect-correct with black bars;
  borderless fullscreen; optional CRT bezel later (cute, not core).
- **Overlay UI (egui)** on hotkey/hover: pause, snapshot, disc swap, shader
  preset picker, grab indicator, latency HUD (debug builds).
- **Input:** grab model per doc 03 — absolute tablet for desktop mousing,
  relative PS/2 grab for games, hotkey toggle (default Ctrl+Alt+G,
  rebindable), auto-grab-on-click option. Keyboard passthrough while grabbed.
  Gamepads → DirectInput research post-v1.
- **Audio:** QEMU audio → lock-free ring → cpal on CoreAudio / WASAPI /
  PipeWire. Start ~30 ms end-to-end, instrument, tighten. CD-DA mixes
  QEMU-side (doc 05).
- **Media:** runtime disc mount/eject/swap from the disc shelf (multi-disc
  installs), floppy images.
- **Snapshots:** QEMU internal snapshots via in-proc QMP, surfaced in the
  overlay and the launcher.

## Launcher

- Machine library grid with last-frame thumbnails, family badge, running
  state; spawns a player per machine.
- **Guided creation:** family (Win98/XP/DOS) → name → memory → processor →
  acceleration →
  networking → disk size → install media → bundle from the reference
  definitions (doc 06). Advanced drawer edits the TOML. Never a QEMU
  command line.
- **Memory and acceleration** are the two machine settings worth exposing
  next to the family, and the same form edits them on an existing
  machine. Memory offers the family's own default and is bounded by
  doc 06 (Win98 stops at its 512 MB ceiling — more and it will not boot).
  Acceleration is Automatic / KVM-required / Emulation, written into the
  bundle as `accel`: *Automatic* becomes QEMU's own `accel=kvm:tcg`
  fallback rather than anything the launcher probes, so it cannot be
  wrong at spawn time; the form says separately whether this host has
  KVM, since "Automatic" otherwise means something invisible.
  **The default is per family: Win98 is emulated, XP is automatic.** KVM
  runs a guest at host speed, and doc 06's `pentium3` model does not
  protect Win9x from its own fast-CPU bugs — it is the speed that trips
  them — while emulation is also the path docs 13 and 16's x87/SSE fast
  paths exist for, i.e. the configuration Win98 is actually tuned and
  tested on here. A bundle with no `accel` field follows its family
  rather than a fixed default, so nothing written before the field
  existed silently changes how it runs.
- **The processor** is a combo of named machines, not a number
  (`cpu_speed` in the bundle, `bundle::CpuSpeed`): *Unthrottled* down
  through *Pentium 133*, *486DX2-66*, *386DX-33*, *286-12*. Nobody knows
  how many instructions per second their DOS game wants, but "it needs a
  486" is written on the box — and DOS-era software times itself against
  the CPU it finds, so this is the field that decides whether a game is
  playable at all (doc 06 has the measurements). It is offered for every
  family, because a Win98 machine runs DOS games in a DOS box too;
  only the DOS family defaults to a throttled one. The form says, next
  to the combo, the two things that follow: that this is what makes an
  era game work, and that choosing a processor makes the machine
  emulated — QEMU's `-icount` cannot run under KVM, so
  `effective_accel()` returns TCG whenever one is chosen rather than
  letting the machine fail to start.
- **A floppy and a boot order** (`floppy`, `boot`): doc 06 lists a floppy
  on the Win98 machine and this document lists floppy images among the
  media the launcher handles, but until 2026-09-06 no bundle could
  express either. *Boot from* is Automatic / Hard disk / Floppy / CD;
  Automatic emits no `-boot` at all, which is what every bundle written
  before the field did and what the wizard's "boot the installer from
  the CD because the new disk is blank" case relies on.
- **Networking** is one checkbox (`network` in the bundle). It follows
  the family for a new machine — on for Win98 and XP, **off for DOS**,
  which reaches a network only through a packet driver the user installs
  by hand — until someone touches the box, exactly like memory and the
  processor. An existing bundle with no `network` field is still on
  whatever its family: turning a card off under a machine that has been
  running with one is a hardware change, not a default. The checkbox is:
  the machine either has doc 06's per-family NIC on QEMU's user-mode NAT
  — outbound through the host, nothing on the network able to reach the
  guest — or it has no adapter at all, so Windows never sees a card, asks
  for its driver or waits on a network at boot. Off emits `-nic none`,
  because QEMU otherwise supplies a NIC of its own when the command line
  asks for none; and XP's PCI devices carry the explicit addresses their
  order already gave them, so the NIC's absence doesn't slide the sound
  card into its slot and make an installed guest re-detect hardware. An
  absent `network` field means on, as every bundle written before it ran.
- **Shader presets come with the launcher or are downloaded by it:** a
  source checkout has the `third_party/slang-shaders` submodule, and a
  machine without one (no `--recurse-submodules`, or a packaged build)
  gets a "Download presets" button in the profile manager instead of a
  preset picker that opens on nothing — upstream's tarball, unpacked into
  the data directory beside the machine and profile libraries, never into
  `third_party/`. `LAUNCHER_SHADERS_DIR` overrides where they live. An
  empty preset field's "Browse…" opens there, since a `.slangp` is never
  somewhere a person would navigate to by hand.
- **Disc shelf:** the user's disc images, labelled, shared by every machine
  (`discs.toml` beside the machine and shader-profile libraries) — a rip is a
  property of the person, not of the machine that installed it first. A
  machine keeps only which disc is in its drive at boot; the rest are
  swapped in while it runs. One-click guest-tools ISO attach.
- **The shelf from inside the guest** (`CDSHELF`, guest-tools ISO): the
  same shelf, listed and swapped from a program running in the guest — a
  disc-2 prompt in a game is answered without leaving it. It is a
  *window* on Windows (pick a disc, press Insert) and a key-per-disc menu
  in a DOS box, because a disc swap is something a player does mid-game,
  not a command line they retype; both also take verbs for scripting. The channel is a
  vendor ATAPI command on the machine's own CD-ROM drive (opcode 0xD0,
  patch 52, protocol `cdshelf/cdshelf_proto.h`), because that drive is the
  one thing DOS, Win98 and XP can all send a raw command to — PIO, ASPI
  and SPTI respectively — and its firmware is ours. No new device, no
  guest driver, and a machine started without a shelf answers ILLEGAL
  REQUEST, which the program reports as "this drive has no shelf". The
  launcher publishes the shelf to a flat file beside the machine's monitor
  socket at spawn and on every edit, so a disc added while the guest runs
  appears in its next listing.
- Snapshots UI, bundle import/export.
- UI toolkit: **egui/eframe** (decided at M6, 2026-09-04 — see
  `docs/tracks/m6-launcher.md`). MIT/Apache-2.0 fits the project's
  GPL-2.0-only + open-source stance better than Slint's non-GPLv3 tiers;
  its default features are wgpu-backed already, unifying with the
  player's `wgpu`/`winit` pins in `Cargo.lock`.

The launcher is optional by design: hand-written bundles + the player binary
is a fully supported path.

### How the launcher reaches a running machine (decided at M6, 2026-09-05)

Snapshots and disc swaps on a machine that is already up need the guest's
monitor, and the player's own one lives on a socketpair inside its process
(doc 11). Rather than give either binary an IPC surface, **the launcher adds
`-qmp unix:<runtime dir>/…,server,nowait` to the arguments it spawns the
player with and speaks QMP to that socket itself** — QEMU allows several
monitors, everything after `--` is passed through to QEMU unchanged, and this
is the same shape `tools/qmpc.py` already uses to drive a guest. A bundle run
straight through `player` by hand has no such socket, which is exactly the
"optional launcher" path above. The socket is derived from the bundle
directory and lives in an owner-only directory (a QMP monitor is complete
control of the machine). Unix sockets only, so live control is Linux/macOS;
Windows needs a named pipe or a loopback port, settled with packaging.

A machine that *isn't* running has no monitor, so the launcher goes at the
qcow2 with `qemu-img snapshot` instead — the same snapshots `savevm`/`loadvm`
write. Both are refused in the other's mode: `qemu-img` writing to an image
QEMU has open corrupts it.

## Settings taxonomy

- **Per-app:** shader preset library, the disc shelf, default hotkeys,
  telemetry = none.
- **Per-machine:** everything in the bundle (hardware, RAM, media, preset
  override, grab behavior).
- Bundles live in a plain, documented directory layout the user can back up.

## Platform packaging

- macOS: signed .app, JIT entitlement, notarized; Apple Silicon native.
- Windows: installer + portable zip; WHPX detection with visible
  "acceleration: …" indicator and TCG fallback.
- Linux: Flatpak primary (bundles our patched QEMU cleanly) + distro builds.

### The names (ADR-011, 2026-09-05; amended 2026-09-06)

The product is **2ksbox** (`2ksbox.com`), and the application ID is
**`com._2ksbox.Launcher`** — the desktop entry's filename, the icon's
name, the Wayland `app_id` matching the two, and the future Flatpak /
AppStream ID. The leading digit is escaped because no segment of such a
name may start with one (`flatpak build-init` rejects `com.2ksbox.…`).
Since 2026-09-06 nothing is called `win98-xp-virt` any more: the
repository is `davidrios/2ksbox`, the docs say 2ksbox, and the user's data
directory is `~/.local/share/2ksbox` — moved once, on the first run that
looks for it (`launcher/src/paths.rs::data_dir`).

### The install layout (decided at M6 step 6, 2026-09-05)

Everything the launcher reaches for used to be found in the checkout it
was *built* from — the player next to it in `target/`, `qemu-img` in
`build/qemu`, the firmware in `qemu/pc-bios`, the guest-tools ISO in
`guest-tools/out`, the presets in `third_party/`. An installed copy has
none of those, so there is now a second layout, and the launcher decides
which one it is in by looking at its own executable
(`launcher/src/paths.rs`): `<exe dir>/..` containing
`share/2ksbox` means installed.

```
<prefix>/bin/2ksbox                            the launcher
<prefix>/bin/2ksbox-player                     the player
<prefix>/lib/2ksbox/libqemu-embed-i386.so
<prefix>/libexec/2ksbox/qemu-img               ours, patched — kept off PATH
<prefix>/share/2ksbox/pc-bios/                 QEMU firmware (the player's -L)
<prefix>/share/2ksbox/guest-tools/             the guest-tools ISO
<prefix>/share/2ksbox/shaders/                 presets, when a package ships them
<prefix>/share/2ksbox/desktop/                 .desktop + icon + metainfo, for install.sh
<prefix>/share/doc/2ksbox/                     COPYING, notices, README
```

Three rules hold it together:

- **Everything is relative to the executable**, so an extracted tarball
  works where it was extracted and needs no install step at all. The
  player's own `libqemu-embed` is found the same way, through an
  `$ORIGIN/../lib/2ksbox` rpath (`@loader_path` on macOS) that is
  deliberately ordered *before* the absolute build-directory one, so a
  binary copied out of a developer's `target/` is genuinely self-contained
  once packaged instead of quietly loading the library from their build.
- **It is one layout or the other, never a mixture.** An installed
  launcher answers only with its own prefix, even for a file the package
  left out; falling back to a checkout would let a broken package pass on
  the machine that built it and fail everywhere else. `LAUNCHER_*`
  environment overrides still win over both.
- `qemu-img` is ours (patch 50's `cdimage` driver is compiled into it),
  so it goes in `libexec/` where it can neither shadow nor be shadowed by
  the system's own.

`scripts/package-linux.sh` stages exactly this, checks it by asking the
staged launcher itself with a scrubbed environment (`--paths`, and a real
machine created and translated to a command line), and rolls a tarball;
`packaging/linux/install.sh`, shipped inside it, copies the tree into a
prefix and writes the desktop entry with absolute paths. The launcher's
window carries the same identity — `app_id` = `com._2ksbox.Launcher`,
matching the desktop file's own name, plus the icon itself for X11 and
Windows.

The AppStream metadata (`com._2ksbox.Launcher.metainfo.xml`, installed
into `share/metainfo`) goes with it: a software centre needs it, and
Flathub requires it. Its content rating is a deliberately **empty** OARS
block — that field rates 2ksbox itself, which has no chat, no purchasing
and nothing user-to-user, and the Windows software someone runs in a
guest is their own content, the same reading RetroArch and other
emulators apply. `appstreamcli validate --no-net` runs on every package
and fails it on errors only, since the one outstanding warning (no
screenshots) needs somewhere to host them.

Still open: the Flatpak (its ID and metadata are settled by ADR-011; what
is left is the manifest, hosted screenshots and a `flatpak-builder`), the
macOS .app and the Windows installer, and with the latter Windows live control (a named pipe or a
loopback port in place of the Unix monitor socket above).

## The Qt port (a spike, `launcher-qt/`)

Built 2026-09-06 to answer "how would this go in Qt" with something that
runs rather than an argument. `launcher-qt/` is a **second, complete
front end** over the same launcher: the machine grid, the wizard, the
disc shelf, snapshots and the shader profile manager with its live
preview, on Qt 6.11 through **cxx-qt 0.10** (KDAB's Rust↔Qt bridge,
MIT/Apache-2.0), with the views in QML.

It is deliberately **not** in the workspace: `launcher-qt/Cargo.toml`
declares its own `[workspace]`, so `cargo build` at the repository root
never starts needing Qt 6 development files on the Mac, in CI or in the
Flatpak. Build it from its own directory; that is the whole build
command, no CMake (`cxx-qt-build` finds Qt through `qmake6` and drives
`moc`/`qmltyperegistrar` itself).

### What the exercise actually measured

**The launcher's logic is not egui's, and that is now proven by the
compiler.** Ten of `launcher/src/`'s seventeen modules — `bundle`,
`control`, `disc_library`, `library`, `paths`, `player`,
`shader_library`, `shader_profile`, `shader_source`, `snapshots`, 1,943
lines — touch no toolkit type, and the Qt build re-includes them
*verbatim* through `#[path]` module declarations rather than copying
them. They compiled unchanged. Against that, 3,208 lines of egui became
2,924 lines of Rust bridge plus 1,678 lines of QML.

(Nine and 1,516 when the port was built. Two of the shared modules were
reaching across the boundary they were supposed to prove: `disc_library`
named `filepicker::Filter` for its `DISC_FILTER`, and `control` used the
`Snapshot` type out of a file that also held an egui window, which is why
the port had to *copy* that file's free half. Both were fixed on
2026-09-06 — the filter is a plain `(label, extensions)` pair the shelf
owns, and `snapshots.rs` is now the free half alone with the window in
`snapshots_ui.rs` — so the copy and the Qt crate's `filepicker` module
are both gone.)

That "more lines, not fewer" is the honest headline, and it splits into
three unequal parts:

- **The bridge is real work egui doesn't need.** Every window's state
  becomes a `QObject` with `#[qproperty]`/`#[qinvokable]`, and a
  `QAbstractListModel` for every list. egui reads a `Vec` in the same
  frame it draws it; Qt needs the model to *say* the list changed.
- **The views got shorter and much flatter.** `qml/` describes what is
  on screen; there is no `if ui.button(…).clicked()` interleaving
  layout, event handling and business logic in one expression.
- **Two places came out structurally better.** The shader editor *is*
  the parameter list model, so "the checkbox and the slider disagree
  about which parameter they belong to" stops being expressible (the
  egui version keeps `params` and `overrides` in lockstep by hand). And
  every field with a consequence goes through a named invokable
  (`chooseFamily`, `chooseRam`, …), which makes the "…_chosen" rule —
  memory follows the family until someone picks a number — impossible
  to forget in a new widget.

### Windows, not floating panels

The secondary screens are **real top-level windows** (`Window` with
`Qt.Dialog`), not in-window popups. That is the platform doing work the
egui build has to do itself: they move, resize, stack and close the way
every other window on the desktop does, and the shader editor no longer
needs the "Fullscreen" toggle the egui version grew — the user drags the
window as wide as they like and the preview grows with it.

It also forced one honest simplification. The egui shader manager is one
window with two modes (list / editor) that resizes itself between them;
the Qt version is **two windows**, because a real window's size cannot be
reliably changed once the window manager has mapped it — bound or
assigned, the request is the WM's to ignore, and here it was ignored on
the height, leaving the editor's preview squashed into a strip. Two
windows with fixed initial sizes is both the fix and the better shape.

### What Qt gives for free

- **File dialogs.** `QtQuick.Dialogs`' `FileDialog` is the XDG portal on
  Linux, NSOpenPanel on macOS, `IFileDialog` on Windows — the same three
  backends the egui build reaches through `rfd`. That dependency, and 94
  lines of `filepicker.rs`, drop to a type alias and a `format!`.
- **Headless rendering.** `QT_QPA_PLATFORM=offscreen` plus
  `Item.grabToImage()` replaces `main.rs`'s ~150 lines of "build an
  `egui::Context`, feed it synthetic pointer events, tessellate, paint
  into an off-screen texture, dump it". Qt separates "render" from "have
  a window"; egui does not.
- **Timers say what they are.** The snapshot job poll and the
  player-exit reap are `Timer { interval: … }` in QML, running only when
  there is something to watch, where the egui build does both at the top
  of every frame because it has a frame anyway.

### What Qt costs

- **The live shader preview.** eframe hands egui a `wgpu::Device` and the
  preview borrows it — the rendered texture reaches the widget by id,
  zero copy. Qt Quick renders through QRhi and cxx-qt exposes no handle
  to it, so `launcher-qt` opens a *second*, windowless wgpu device
  (~40 MB of VRAM and another driver context) and the frame reaches QML
  through a **CPU readback written to a temp BMP**: ~3 ms readback plus
  ~4 ms write per frame at 1280×960, on every slider drag. Doing it
  properly means a `QQuickRhiItem` subclass in C++ importing the Vulkan
  image — a real project, not a spike. **This is the one place the Qt
  build is meaningfully worse, and it is fixable, in C++.**
- **A runtime to ship.** The Qt binary is *smaller* — 24.4 MB release
  against egui's 39.8 MB — because Qt is a shared library and egui,
  wgpu and winit are statically linked into the launcher. But it then
  needs ~25 MB of `libQt6{Core,Gui,Qml,Network,DBus}` plus the ~13 MB
  QtQuick QML plugin tree present on the machine, where the egui build
  needs nothing beyond the system's Vulkan and windowing libraries.
  That is a packaging question, not a size question: on Linux it means
  the Flatpak would move from `org.freedesktop.Sdk` to `org.kde.Platform`
  (which ships Qt), and the AppImage/macOS/Windows builds would each
  have to carry Qt themselves.
- **Build time.** A clean release build is 4m08s here, against the egui
  launcher's own (the C++ generation, `moc` and `qmltyperegistrar` steps
  are additive, and every QML file recompiles the resource).
- **C++ in the loop.** No C++ is written here, but `cargo build` now runs
  a C++ compiler, `moc` and `qmltyperegistrar`, and the binary links
  Qt 6 — LGPLv3, which is why `launcher-qt` is `GPL-2.0-or-later` like
  `launcher` (ADR-009's reasoning carries over unchanged; GPL-2.0-only
  could not take LGPLv3).

### Four traps, each of which cost real time

1. **cxx-qt's generated property setter skips the notify when the value
   already matches** — it compares first, to avoid binding loops. So
   writing `rust_mut().open = true` and then "publishing" it with
   `set_open(true)` emits *nothing*, and QML keeps showing the old
   value. Every window in the port opened once and then stopped
   reacting. The fix is a discipline, written into each bridge's header:
   build the new state as a *value* and hand it to an `apply` that
   writes every property through its setter while the struct still holds
   the old one.
2. **`grabToImage` only works on an item the QML engine created.** It
   starts with `qmlEngine(this)`, and a window's own `contentItem`,
   `Overlay.overlay` and a `Popup`'s default `contentItem` are all made
   in C++ — all three refuse, silently. The screenshot path therefore
   grabs an item each window declares itself. A *whole-window* headless
   shot, frame and all, would need a small C++ shim calling
   `QQuickWindow::grabWindow()`.
3. **`property var` holding a QObject gives QML no metadata**, so
   `editor.open` in a binding is read once and never re-evaluated. Use
   the registered type (`property ShaderEditor editor`) — free, since
   `#[qml_element]` already registers it.
4. **A `Window`'s size cannot be changed after the window manager has
   mapped it** — not by a binding (which the WM's own resize breaks for
   good) and not by assignment (which it may simply ignore, as it did
   here for the height but not the width). A window that wants two very
   different sizes should be two windows.

### The numbers

| | egui (`launcher/`) | Qt (`launcher-qt/`) |
|---|---|---|
| toolkit-free Rust, shared | 1,943 lines | the same 1,943, `#[path]`-included |
| front-end code | 3,208 lines Rust | 2,924 Rust + 1,678 QML |
| dependencies beyond the shared set | `eframe`, `egui`, `rfd` | `cxx-qt`, `cxx-qt-lib`, system Qt 6 |
| release binary | 39.8 MB, self-contained | 24.4 MB, **plus ~38 MB of Qt runtime** |
| shader preview | zero-copy texture id | CPU readback → temp BMP → `Image` |
| file dialog | `rfd` (portal / NSOpenPanel / IFileDialog) | Qt's own, same three backends |
| headless frame | ~150 lines of synthetic-input plumbing | `QT_QPA_PLATFORM=offscreen` + 4 lines of QML |
| secondary screens | floating panels inside the one window | real top-level windows |

**Equivalence check:** `launcher-qt --preview-shader` and `launcher
--preview-shader` render the same preset and image to **byte-identical**
PNGs, so the shared `shader-chain` path is provably the same on both.

**The `#[path]` arrangement survived its first real test.** Rebasing the
spike onto `main` picked up 61 commits, including a `shader-chain` that
had grown `parameter`/`has_parameter` and split `dump_texture` into
`read_texture` + `write_png` — and the Qt crate built with no edit. The
split then let the Qt preview delete its own 58-line readback and call
`shader_chain::read_texture`, so row strides and BGRA handling live in
one place for the player and both launchers.

### The recommendation

**Nothing here justifies switching, and nothing here rules Qt out.** The
egui build stays: it is finished, it is pure Rust, it is one `cargo
build` on every platform, and the shader preview — the launcher's one
genuinely graphical feature — is a texture id there and a CPU round-trip
here. What the spike does settle is that the *cost of the option* is
known: the logic half is portable as-is, the port is a few thousand
lines of view code, and the only hard part has a named fix. Keep
`launcher-qt/` as a reference point; revisit if the launcher ever needs
something egui is bad at — native menus, accessibility, or a Windows
build that has to look like Windows.

## Out of scope for v1

Shared folders/drag-drop, clipboard sync, USB passthrough, multi-monitor
guests, recording/streaming helpers (recording pairs naturally with the
shader pipeline — first post-v1 candidate).
