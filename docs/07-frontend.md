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
- **Guided creation:** family (Win98/XP) → name → memory → acceleration →
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
- **Networking** is one checkbox (`network` in the bundle, default on):
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

### The names (ADR-011, 2026-09-05)

The product is **2ksbox** (`2ksbox.com`), and the application ID is
**`com._2ksbox.Launcher`** — the desktop entry's filename, the icon's
name, the Wayland `app_id` matching the two, and the future Flatpak /
AppStream ID. The leading digit is escaped because no segment of such a
name may start with one (`flatpak build-init` rejects `com.2ksbox.…`).
`win98-xp-virt` remains the repository's working name, the docs' name and
the user's data directory; only the packaged identity has moved so far.

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

## Out of scope for v1

Shared folders/drag-drop, clipboard sync, USB passthrough, multi-monitor
guests, recording/streaming helpers (recording pairs naturally with the
shader pipeline — first post-v1 candidate).
