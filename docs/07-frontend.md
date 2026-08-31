# 7. Frontend: libretro core + companion launcher

Two deliverables (ADR-003): the **core** (what runs the machine, inside
RetroArch) and the **companion launcher** (what manages machines). RetroArch
itself provides video/shaders, audio, input binding, and the on-screen menu —
we don't rebuild any of it.

## The core (in RetroArch)

- **Content model:** the core opens a *machine bundle* (`machine.toml` +
  disks + disc shelf; zipped or a directory). Also accepts a bare disc image
  as content for "boot the default machine with this disc inserted"
  convenience once a default machine exists.
- **Video:** 2D via framebuffer upload with exact guest resolution and
  geometry info (doc 03); 3D via hw-render context when qemu-3dfx is active
  (doc 04). Mode changes → `retro_set_geometry`/av_info updates.
- **Shaders:** users apply RetroArch slang presets natively. We ship our
  curated presets (doc 03) as a shader pack + per-core defaults, including
  the rig-calibrated "Trinitron" preset (doc 09).
- **Disk control:** libretro disk-control interface = the disc shelf.
  Mount/eject/swap raw CD images at runtime, multi-disc installs supported by
  the frontend UI RetroArch users already know.
- **Input:** keyboard passthrough via RetroArch "game focus"; relative mouse
  through libretro mouse; absolute tablet mode as a core option. RetroPad
  mappings for common actions (Esc, Enter, disc swap) as a convenience;
  gamepad-to-DirectInput research unchanged (post-v1).
- **Core options:** machine overrides (RAM within family limits, audio
  device, 3D on/off, grab behavior, aspect handling) — everything else lives
  in the machine bundle.
- **Save states:** initially exposed as in-core QEMU snapshots through core
  options/QMP (guest RAM makes retro_serialize impractical at first); revisit
  proper libretro serialization later.

## The companion launcher (standalone Rust app)

Everything RetroArch's content model handles poorly:

- Machine library with thumbnails and running state.
- **Guided creation:** family (Win98/XP) → name → disk size → install media →
  bundle created from the reference definitions (doc 06); advanced drawer
  edits the TOML. Never a QEMU command line.
- Snapshots UI, disc shelf editing, one-click guest-tools ISO attach,
  bundle import/export.
- Launches RetroArch with core + bundle (detects RetroArch, or guides
  install; optionally drives our own bundled RetroArch on platforms where
  that's cleaner).
- UI toolkit: egui or Slint — decide at M6, not now.

The launcher is optional by design: hand-written bundles + plain RetroArch is
a fully supported path for power users.

## Audio

QEMU audio → ring → libretro audio upload. CD-DA mixes QEMU-side (doc 05), so
it arrives through the same path. Latency inherits RetroArch's audio driver
stack (good) plus our ring (~10–20 ms target); measure in M1.

## Distribution

- Core: our releases (per-platform cdylib + info file), submitted to the
  libretro buildbot/core installer once stable — that step puts us in front
  of RetroArch's entire user base.
- Launcher: signed .app / installer / Flatpak.
- Shader pack: bundled with core releases, plus a repo under `/shaders/`.

## Out of scope for v1

Unchanged (doc 01): shared folders, clipboard sync, USB passthrough,
recording helpers (RetroArch's own recording works day one — a nice side
effect of the core decision).
