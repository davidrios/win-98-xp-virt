# 2. Architecture: in-process QEMU, libretro core, process model

## Decisions (locked — see doc 10 for rationale)

1. **QEMU runs in-process** with whatever presents the display (ADR-002).
2. **The primary frontend is a libretro core** running in RetroArch; a
   companion launcher app handles machine management (ADR-003).
3. **Rust where possible**; C only inside QEMU/qemu-3dfx and in guest-side
   code (ADR-004).

## Component map

```
┌────────────────── RetroArch process ─────────────────────┐
│  RetroArch: video (slang CRT shaders), audio, input,     │
│  disk control UI, save states, config                    │
│            ▲ libretro API                                │
│  ┌─────────┴─────────── our core (cdylib) ────────────┐  │
│  │  core shell (Rust): retro_* impl, geometry/mode    │  │
│  │  reporting, input mapping, disk-control glue       │  │
│  │        ▲ libqemu_embed.h (bindgen)                 │  │
│  │  ┌─────┴──────── QEMU fork (C, linked) ─────────┐  │  │
│  │  │  TCG / KVM / WHPX                            │  │  │
│  │  │  qemu-3dfx device + host GL translation      │  │  │
│  │  │  ATAPI raw-CD device ──► libdisc (Rust)      │  │  │
│  │  │  display listener / input inject / audio     │  │  │
│  │  └──────────────────────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘

┌────────── companion launcher (Rust, separate app) ───────┐
│  machine library, guided creation, snapshots UI,         │
│  disc shelf, guest-tools ISO attach                      │
│  → writes machine bundles the core loads as "content";   │
│    can launch RetroArch with core+content                │
└──────────────────────────────────────────────────────────┘
```

- "Content" for the core = a **machine bundle**: a directory/`.zip` with
  `machine.toml`, disk images, and disc-shelf references. The launcher
  creates and edits bundles; RetroArch just opens them. Power users can
  hand-write bundles and skip the launcher entirely.
- One VM per RetroArch process (QEMU global state) — matches RetroArch's
  one-core-per-process model, so the constraint costs nothing here.
- **Optional future standalone player:** the embed API is frontend-agnostic;
  a wgpu+librashader player (previous design, preserved in git history) can
  return if RetroArch friction demands it. Not scheduled.

## The embed boundary: `libqemu_embed.h`

Small stable C API on the QEMU fork, written upstream-style:

- lifecycle: create/configure (from machine bundle), run, pause, reset,
  shutdown;
- display: register listener → surface + dirty rects callback (2D), and a
  hw-render handoff for 3D (see doc 03/04);
- input: keyboard scancodes, relative + absolute pointer injection;
- audio: pull callback into caller-provided ring;
- media: disc mount/eject (drives libdisc), floppy;
- control: QMP-JSON channel over in-memory pipe for everything else
  (snapshots, status), so we reuse QEMU tooling semantics.

Rust bindings via bindgen; the core shell never reaches past this header.

## Language policy (ADR-004 summary)

Rust: core shell, embed bindings, launcher, **libdisc** (CD disc model +
format parsers, built as a staticlib with a C API consumed by the QEMU ATAPI
device), MMC exerciser, all tooling. C: patches inside QEMU (embed API,
ATAPI glue — thin, calls into libdisc), qemu-3dfx host code. Era C for
guest-side wrappers/drivers (Rust cannot target Win9x/XP).

QEMU upstream has accepted experimental Rust device code since 9.2; libdisc
having a clean C API keeps both integration and eventual upstreaming
realistic.

## Threading model

- **QEMU main loop + vCPU threads** — QEMU-managed (MTTCG on ARM hosts).
- **retro_run cadence** — RetroArch calls the core once per frame; the core
  publishes the newest complete guest frame (triple-buffered handoff from the
  display listener). QEMU never blocks on RetroArch's vsync; RetroArch never
  waits on QEMU — a missed guest frame repeats the last one.
- **Audio** — QEMU audio backend → lock-free ring → core's audio upload in
  retro_run (or async audio callback where the frontend supports it).
- Rule unchanged: no QEMU-owned thread waits on the GPU; no render/audio path
  takes a QEMU lock.

## Acceleration per platform

| Host | Win98/XP (x86 guests) | Notes |
|---|---|---|
| Linux x86_64 | KVM | performance reference |
| Windows x86_64 | WHPX (TCG fallback) | WHPX quirks with old guests: test |
| macOS Apple Silicon | TCG (MTTCG) | Win98 easy; XP benchmarked at M1 |
| macOS/Linux x86_64 | HVF / KVM | secondary, should Just Work |

JIT entitlement on macOS applies to the RetroArch app bundle — RetroArch
already ships JIT-entitled builds for its own cores; verify ours rides along.

## Repo layout (target)

```
/core/                Rust: libretro core shell + embed bindings
/libdisc/             Rust: CD disc model, format parsers, C API, exerciser
/launcher/            Rust: companion app
/qemu/                submodule: upstream QEMU pinned to qemu-3dfx cadence
/patches/qemu/        patch queue: embed API, ATAPI device, 3dfx rebase
/guest-tools/         driver ISO build (Rust tooling + era binaries)
/shaders/             curated slang presets (consumed by RetroArch directly)
/reference/           rig captures: CRT photos, ATAPI traces, benchmarks
/docs/                these documents
```

CI builds patched QEMU + core + launcher on all three platforms from day one;
Apple Silicon runner mandatory. CI also runs the core headless under a
libretro test harness for smoke tests.

## Open questions (tracked, not blocking)

- rust-libretro crate vs. our own thin `retro_*` shim (API is small; decide
  in M0 by reading the crates' hw-render support).
- qemu-3dfx GL inside libretro hw-render on macOS (M0 spike — the plan's
  riskiest integration; see docs 03/04).
- Save states: QEMU snapshot streams via retro_serialize are large (guest RAM
  ~0.5–1 GB); likely ship with in-core QEMU snapshots (via QMP) instead of
  libretro serialization initially.
- qemu-3dfx rebase cadence pins our QEMU version (unchanged from before).
