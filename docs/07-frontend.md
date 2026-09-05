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
- **Guided creation:** family (Win98/XP) → name → disk size → install media →
  bundle from the reference definitions (doc 06). Advanced drawer edits the
  TOML. Never a QEMU command line.
- **Disc shelf:** the user's disc images, labelled, shared by every machine
  (`discs.toml` beside the machine and shader-profile libraries) — a rip is a
  property of the person, not of the machine that installed it first. A
  machine keeps only which disc is in its drive at boot; the rest are
  swapped in while it runs. One-click guest-tools ISO attach.
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

## Out of scope for v1

Shared folders/drag-drop, clipboard sync, USB passthrough, multi-monitor
guests, recording/streaming helpers (recording pairs naturally with the
shader pipeline — first post-v1 candidate).
