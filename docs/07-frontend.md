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
- Snapshots UI, disc shelf editing, one-click guest-tools ISO attach, bundle
  import/export.
- UI toolkit: egui or Slint — decide at M6.

The launcher is optional by design: hand-written bundles + the player binary
is a fully supported path.

## Settings taxonomy

- **Per-app:** shader preset library, default hotkeys, telemetry = none.
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
