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
- **The host's 3D** is stated, not chosen (ADR-013): under the
  acceleration row a Windows machine gets one line saying what this host
  will give the guest's Direct3D. It needs a Vulkan 1.3 device, because
  that is what its DXVK executor needs; below that bar 3D goes through
  OpenGL with WineD3D in the guest instead, and the line says how to
  install it. There is no picker because there is no decision here: the
  host settles it, and the only failure worth preventing is finding out
  after the machine exists. A **software** Vulkan driver counts as
  available — DXVK will use one — but is the single case drawn as a
  warning rather than a note, because it works and disappoints: it says
  to expect it to be very slow and that WineD3D may well beat it, both
  being worth trying. A host with no Vulkan at all is a plain note;
  nothing is wrong and every machine still runs. DOS machines get no
  line. The sentence is the shared form's (`graphics_note()`), like every
  other note under a row, so the egui build, the Qt build and the C ABI
  cannot drift; `launcher --host-check` is the same answer in full, for a
  support question or a script (`launcher-core/src/host_gpu.rs`; the
  `host-check` check in `scripts/test.sh`).
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
- **Our own emulator fast paths are seven checkboxes**, behind a
  disclosure headed "Emulation optimizations — 6 of 7 on" so a machine
  with one turned off says so while the section is closed. Each is one of
  the QEMU patches this project maintains (`patches/qemu/README.md`) with
  the off switch that patch already carried: `x87-fast`, `sse-fast`,
  `simd-fast` and `rep-fast` are guest-CPU properties, `smc-same-value`,
  `inline-lookup` and `pinned-regs` are properties of the TCG accelerator
  itself. **They are exposed because the switch is the oracle.** Every
  one of them replaces simulated arithmetic with the host's own, so when
  a guest computes the wrong number or a game stops drawing, one run with
  one checkbox clear says whether a fast path did it — the alternative is
  bisecting a patch queue against a Windows install. The section says the
  measured gain under each switch, and says above them that on a machine
  headed for KVM they do nothing at all, because there is then no
  emulator in the path to have a fast path.
  Everything that has shipped is on; `pinned-regs` is off, because the
  patch itself is off by default while that work is in progress.
  **Only the difference is stored** (`[optimizations]` in the bundle, keyed
  by the QEMU property name): a machine that has changed nothing writes no
  table and produces the command line it always produced, and an
  optimization added to the patch queue later arrives on in every bundle
  that already exists. An entry a newer launcher wrote survives a load and
  a save in an older one, since the table is keyed by name and not by a
  Rust enum.
  One consequence reaches the command line: the accelerator is now spelled
  `-accel kvm -accel tcg,…` rather than `-machine accel=kvm:tcg`, because
  the accelerator-side properties need somewhere to live and QEMU refuses
  the two spellings together. It is the same code path — two `-accel`
  options are tried in order and the first that initializes wins, which is
  exactly what `kvm:tcg` did.
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
- **The preview moves when the preset does** (fixed 2026-09-06): plenty of
  presets do not draw the same picture every frame — an interlaced CRT
  puts up alternate fields, a phosphor afterglow decays over several,
  an NTSC signal shimmers, the flicker of a TV is the whole effect — and
  the editor's preview, which renders when something is clicked and not
  otherwise, showed one frozen frame of all of it. So the core says which
  presets those are and how often it wants drawing
  (`preview::Preview::frame_interval`, `None` for a preset that stands
  still), and each front end obeys in its own idiom: egui asks for a
  repaint after the interval, QML runs a `Timer` at it. The frame number
  the shader is given comes from a **clock at `FRAME_RATE` (60/s)**, not
  from a count of renders, so the effect runs at the speed it would in
  the player even on the Qt path, which reads every frame back to the CPU
  and cannot always keep up — it drops frames rather than running slow.
  Which presets animate is `shader_chain::preset_is_animated`: it reads
  the preprocessed pass sources for a *use* of `FrameCount` (the
  `params.FrameCount` member access, since 1131 of the slang-shaders tree
  declare the uniform and only 271 read it) or of a history / feedback
  texture, and errs towards animating — a preview that redraws a picture
  that never changes costs a redraw, the other error is the bug itself.
  The headless verbs pin one frame (`PREVIEW_FRAME`, default 0) so that
  their PNGs stay reproducible.
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
  player's `wgpu`/`winit` pins in `Cargo.lock`. Since 2026-09-06 there
  is a **second, maintained front end** on Qt 6 / QML (`launcher-qt/`),
  and everything either of them decides lives in `launcher-core/` —
  "Two front ends, one core" below.

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
  override, grab behavior, the emulator's own fast paths). The fast paths
  are per machine and not per app on purpose: turning one off is a
  diagnosis of *one guest* — the game that draws wrong — and it must not
  slow down every other machine in the library while that lasts.
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

## Two front ends, one core

The launcher is **two maintained front ends over one library** (decided
2026-09-06): `launcher/` on egui/eframe and `launcher-qt/` on Qt 6 / QML
through cxx-qt, both views over `launcher-core/`. The Qt build began as a
costed spike — "how would this go in Qt", answered with something that
runs rather than an argument — and is now kept as a peer.

### What is in the core, and why all of it

`launcher-core/` is **everything the launcher does that is not drawing**,
and the line is drawn deliberately far into what usually counts as UI:

- the data — `bundle` (`machine.toml`), `library`, `disc_library`,
  `shader_profile` / `shader_library` / `shader_source`, `paths`;
- the machinery — `player` (spawning one, and `qemu-img`), `control`
  (QMP to a running machine), `snapshots`, `preview` (the shader chain on
  a still image);
- and **the windows' own behaviour**: `machines`, `wizard`, `shelf`,
  `snaps`, `editor`, one model per window, plus `browse` for the one
  file-dialog decision that is not a dialog and `cli` for every debug
  verb that needs no toolkit.

That last group is the part worth arguing about, and the argument is
settled by what happened without it. When the two builds each held their
own copy of a window's state machine, they drifted, in ways nobody
noticed until the models were merged:

- the Qt wizard had **no processor, floppy or boot-order field at all**,
  so a DOS machine created there came out unthrottled — which is the one
  setting that decides whether a DOS game is playable;
- its networking checkbox **did not follow the family**, so it disagreed
  with `Machine::reference` about a new DOS machine;
- the sentence under that checkbox said `Windows won't see a card` where
  egui's said `the guest won't see a card`, on machines that may not run
  Windows at all;
- and saving a *new* shader profile **dropped the parameter overrides**
  on the egui side (`create(…).map(|_| ())`) and kept them on the Qt
  side. One of those two was a bug for a year of nobody looking.

None of those is expressible now. A front end reads `ram_note()`,
`accel_note()`, `network_notes()` and prints them; it fills a combo box
from `Family::ALL`/`CpuSpeed::ALL` and their `label()`s rather than
retyping the strings; and a field with a *consequence* has no setter at
all, only `choose_*`, which is what applies the rule that memory, the
accelerator, the processor and the NIC follow the family until someone
picks one.

### What each front end still owns

Everything that is genuinely the toolkit's, and nothing else:

| | egui (`launcher/`) | Qt (`launcher-qt/`) |
|---|---|---|
| the file dialog | `rfd` — egui has none | `QtQuick.Dialogs`, declarative |
| when to redraw | every frame; the model is read inline | a `Timer` per thing being watched, off when idle |
| "the list changed" | no such concept; redraw | `beginResetModel` / `dataChanged` |
| a destructive restore | the row's button becomes "Discard current state?" | a dialog |
| the preview frame | a texture id, zero copy | CPU readback → temp BMP → `Image` |
| secondary screens | floating panels inside the one window | real top-level windows |
| a headless frame | ~150 lines of synthetic-input plumbing | `QT_QPA_PLATFORM=offscreen` + `grabToImage` |

Two of those are real differences in kind rather than in spelling. The
**shader preview** is where Qt is meaningfully worse: eframe hands egui a
live `wgpu::Device` and the preview borrows it, so the rendered texture
reaches the widget by id; Qt Quick renders through QRhi and cxx-qt
exposes no handle to it, so `launcher-qt` opens a *second*, windowless
wgpu device (~40 MB of VRAM and another driver context) and the frame
reaches QML through a CPU readback written to a temp BMP — ~3 ms
readback plus ~4 ms write per 1280x960 frame, on every slider drag. (BMP,
not PNG: ~4 ms against ~90 ms.) Doing it properly means a `QQuickRhiItem`
subclass in C++ importing the Vulkan image. **This is the one place the
Qt build is worse, and it is fixable, in C++.** The other, in Qt's
favour, is that a **`Timer` says its interval out loud and stops when
there is nothing to watch**, where the egui build polls the snapshot job
and reaps exited players at the top of every frame because it has a
frame anyway.

The **windows-not-panels** difference forced one honest simplification.
The egui shader manager is one window with two modes (list / editor) that
resizes itself between them; the Qt version is **two windows**, because a
real window's size cannot be reliably changed once the window manager has
mapped it — bound or assigned, the request is the WM's to ignore, and
here it was ignored on the height, leaving the editor's preview squashed
into a strip. Two windows with fixed initial sizes is both the fix and
the better shape.

### The numbers

Measured 2026-09-06, after the split; the figures in brackets are what
they were when the Qt build was a spike with ten `#[path]`-included
files.

| | lines |
|---|---|
| `launcher-core/`, shared by both | **4,435** (1,943) |
| `launcher/` — egui views only | **1,735** (3,208) |
| `launcher-qt/src/` — Qt bridges only | **2,132** (2,924) |
| `launcher-qt/qml/` | **1,772** (1,678) |
| dependencies beyond the shared set | `eframe`, `egui`, `rfd`, `image` (the icon) / `cxx-qt`, `cxx-qt-lib`, system Qt 6 |
| release binary | 39.8 MB, self-contained / 24.4 MB **plus ~38 MB of Qt runtime** |

The two front ends together lost 2,171 lines; the core gained 2,469 of
new shared modules on top of the 1,966 that merely moved. It is
**not** a net saving in lines and it was never going to be: what was
duplicated is now written once, documented once, and given an API
(`ram_note()`, `choose_family()`) where it used to be a field poked
inline. The saving is that there is one place to change any of it.

The Qt binary being *smaller* is not a size win: egui, wgpu and winit are
statically linked into the egui build, while Qt is a shared library, so
the Qt build then needs ~25 MB of `libQt6{Core,Gui,Qml,Network,DBus}`
plus the ~13 MB QtQuick QML plugin tree present on the machine. That is a
packaging question: on Linux the Flatpak would move from
`org.freedesktop.Sdk` to `org.kde.Platform` (which ships Qt), and the
AppImage/macOS/Windows builds would each have to carry Qt themselves.
`launcher-qt` is not in the root workspace (`Cargo.toml` declares its
own) precisely so that a plain `cargo build` never starts needing Qt 6
development files on the Mac, in CI or in the Flatpak; the
`launcher-core` path dependency crosses that boundary without dragging Qt
back the other way. Build it from its own directory — that is the whole
build command, no CMake.

### Proving they agree

Four checks, all of which run without a GUI click:

- **`--preview-shader` on both binaries renders byte-identical PNGs.**
  It is the same code now (`launcher_core::preview` on a headless
  device), so this is a check that the two builds really are linking the
  one implementation.
- **The preview's animation is one decision, checked once.** The
  `preview-anim` check in `scripts/test.sh` renders a still preset at two
  frame numbers (one picture, and reported still) and an interlaced one
  at two frame numbers (two pictures, and reported animated) through the
  shared verb, so a front end cannot quietly stop redrawing and a
  detector regression cannot go unnoticed.
- **The same debug verbs, from the same code.** `launcher_core::cli`
  answers `--paths`, `--discs`, `--snapshots`, `--wizard-new`,
  `--wizard-edit`, `--boot-disc`, `--insert-disc`, `--print-args` and the
  rest for both binaries, where the Qt build used to reimplement two of
  them and lack the other twenty. (`--pick-file` is the one exception: it
  pops `rfd`'s dialog, and Qt's is declarative. The `--diag-*` screenshot
  verbs are each toolkit's own, for the same reason.)
- **A machine created through each front end's real window is the same
  machine.** `--diag-wizard-frame` drives the egui form headlessly;
  `LAUNCHER_QT_SCREEN=create LAUNCHER_QT_ARG=dos:<name>` drives the QML
  one under `QT_QPA_PLATFORM=offscreen`. The two `machine.toml`s differ
  only in the name and the disk path — `family`, `ram_mb = 64`,
  `accel = "tcg"`, `network = false`, `boot`, `cpu_speed = "486dx2-66"`
  all match. Before the split, four of those six were wrong on the Qt
  side or absent.

**The `#[path]` arrangement it replaced survived its own first test** —
rebasing the spike onto `main` picked up 61 commits, including a
`shader-chain` that had grown `parameter`/`has_parameter` and split
`dump_texture`, and the Qt crate built with no edit. It was still the
wrong shape: it proved the *file formats* were portable and left every
window's state machine written twice, which is exactly where the
divergences above came from.

## A third front end: `launcher-core` as a library

Because the core is a real library and not a pile of modules two binaries
happen to include, a front end in another language is a view over it too.
`launcher-capi/` is the C ABI that makes that concrete — a native macOS
app in Swift is the case it was shaped for, since Swift imports a C
header directly with no bridge crate, but anything that speaks C works.

- `launcher-capi/include/launcher_core.h` is the header, hand-written and
  kept beside the code.
- Each window is an **opaque handle** (`lc_wizard_new` / `lc_wizard_free`
  …) and rows are addressed by index, one field at a time — which is not
  a compromise for C: it is exactly how the Qt build's
  `QAbstractListModel::data` already reads them.
- Strings out are owned by the caller (`lc_string_free`) and are never
  `NULL` for "empty", so `NULL` means only "no such row".
- Nothing blocks on a guest: the two long operations keep their polls
  (`lc_snapshots_poll` while `lc_snapshots_job_pending`,
  `lc_editor_preset_state` while a download runs).
- It adds **no behaviour**. Every function is a thin wrapper, so a third
  front end gets the same wizard rules, the same "`qemu-img` only when
  the machine is stopped", the same "keep only the parameters the user
  actually overrode".

`launcher-capi` is a workspace member but **not a default one** — it
builds a `cdylib` and a `staticlib` of the whole launcher, which nobody
needs unless they are building such a front end. `cargo build -p
launcher-capi`.

`launcher-capi/examples/smoke.c` is a third front end in the smallest
possible form, and it is a *test*: `scripts/test.sh host`'s `capi` check
builds it and runs it against a scratch library, creating a DOS machine
through the shared wizard and checking the answers (64 MB, a period
processor, emulated, no network card), then the disc shelf, the library
and the profile editor. A rename or a changed default in a model fails
there as well as in the two GUIs.

What a third front end still owes is what the other two own: a file
dialog, when to redraw, how to confirm a destructive restore, and how to
show a preview frame (`lc_editor_read_frame` hands over RGB8).

### The recommendation, revisited

The 2026-09-06 spike's finding was "nothing here justifies switching, and
nothing here rules Qt out". Keeping both is the answer to a different
question: **what does maintaining a second front end cost, once the
launcher's logic is not the first one's?** About 3,900 lines of view code
for the Qt build, no behaviour, and a build that is off the default path
— and in exchange, four real divergences got found and closed, and the
door to a native macOS front end is a C header rather than a rewrite.

### Four Qt traps, each of which cost real time

1. **cxx-qt's generated property setter skips the notify when the value
   already matches** — it compares first, to avoid binding loops. So
   writing `rust_mut().open = true` and then "publishing" it with
   `set_open(true)` emits *nothing*, and QML keeps showing the old
   value. Every window in the port opened once and then stopped
   reacting. Keeping the state in a core model *beside* the properties
   is what closes this by construction: a `publish` reads the model and
   writes every property through its setter, and the property fields are
   never assigned anywhere else.
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

## Out of scope for v1

Shared folders/drag-drop, clipboard sync, USB passthrough, multi-monitor
guests, recording/streaming helpers (recording pairs naturally with the
shader pipeline — first post-v1 candidate).
