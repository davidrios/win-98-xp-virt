# 2. Architecture: in-process QEMU, player + launcher, process model

## Decisions (locked — see doc 10 for rationale)

1. **QEMU runs in-process** with the display (ADR-002).
2. **Standalone Rust player + companion launcher** — no RetroArch/libretro
   (ADR-005 supersedes ADR-003).
3. **Rust where possible**; C only inside QEMU/qemu-3dfx and in guest-side
   code (ADR-004).

## Why in-process

Gaming latency: framebuffer, input, and audio must not cross a process
boundary. Linking QEMU as a library (UTM-style embed patches) gives zero-copy
framebuffer access (display listener → GPU texture), direct input injection,
and one clock domain for pacing. Consequences accepted: one VM per hosting
process (QEMU global state), a maintained QEMU fork (needed anyway for
qemu-3dfx + the CD backend), GPL-2.0 for everything that links it.

## Component map

```
┌────────────────────────── player process (Rust) ─────────────────────┐
│  ┌──────────── QEMU fork (C, linked as lib) ──────────────────────┐  │
│  │  TCG (ARM hosts) / KVM / WHPX                                  │  │
│  │  qemu-3dfx device + host GL translation                        │  │
│  │  ATAPI raw-CD device ──► libdisc (Rust, staticlib, C API)      │  │
│  │  display listener ─┐   input inject ◄─┐   audio backend ─┐     │  │
│  └────────────────────┼──────────────────┼──────────────────┼─────┘  │
│           libqemu_embed.h (bindgen)      │                  │        │
│  ┌─────────────────────┼─────────────────┼──────────────────┼─────┐  │
│  │  triple-buffered fb handoff           │   cpal low-latency out │  │
│  │  mode analysis → geometry/shader params                        │  │
│  │  librashader-wgpu chain (CRT presets)                          │  │
│  │  geometry stage (aspect, integer scale) → wgpu present         │  │
│  │  winit events ────────────────────────┘   egui overlay         │  │
│  │  QMP-JSON over in-memory pipe (snapshots, media, status)       │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘

┌────────────── launcher process (Rust, separate app) ─────────────────┐
│  machine library, guided creation, snapshots UI, disc shelf,         │
│  guest-tools ISO attach → spawns one player per running machine      │
└──────────────────────────────────────────────────────────────────────┘
```

- **Machine bundle** = directory with `machine.toml`, disk images, disc
  shelf references. The launcher creates/edits bundles; the player runs one.
  Power users can hand-write bundles and skip the launcher.
- One VM per player process; the launcher isolates the library UI from a
  crashed guest and allows several machines at once.

## The embed boundary: `libqemu_embed.h`

Small stable C API on the QEMU fork, written upstream-style:

- lifecycle: create/configure (from machine bundle), run, pause, reset,
  shutdown;
- display: register listener → surface + dirty rects callback (2D); a
  GPU-texture handoff for 3D output (see docs 03/04);
- input: keyboard scancodes, relative + absolute pointer injection;
- audio: pull callback into caller-provided ring;
- media: disc mount/eject (drives libdisc), floppy;
- control: QMP-JSON channel over in-memory pipe for everything else.

Rust bindings are hand-written in the `qemu-embed` crate (the API is ours
and small; `qemu_embed_api_version()` guards drift — no libclang build
dependency). The player never reaches past this header.

## Language policy (ADR-004 summary)

Rust: player, launcher, **libdisc** (CD disc model + format parsers, built
as a staticlib with a C API consumed by the QEMU ATAPI device), MMC
exerciser, tooling. C: patches inside QEMU (embed API, ATAPI glue — thin,
calls into libdisc), qemu-3dfx host code. Era C for guest-side
wrappers/drivers (Rust cannot target Win9x/XP). QEMU upstream accepts
experimental Rust device code since 9.2; libdisc's clean C API keeps
upstreaming realistic.

## Graphics stack

wgpu (Metal on macOS, Vulkan/D3D12 on Linux/Windows) with **librashader's
wgpu runtime** for the RetroArch-format slang shader ecosystem. Versions are
coupled: librashader pins a wgpu major (0.12 → wgpu 30); the workspace
follows librashader's pin, not the newest wgpu.

## Threading model

- **QEMU main loop + vCPU threads** — QEMU-managed (MTTCG on ARM hosts).
- **Render thread** — wgpu, librashader, present. The display listener
  (QEMU main loop) only publishes "surface updated + dirty rect" into a
  triple-buffered handoff; the render thread uploads and draws. QEMU never
  blocks on vsync; a slow host frame repeats the last guest frame.
- **Audio thread** — real-time; pulls from a lock-free ring fed by QEMU's
  audio backend (~20–40 ms initially, tune down).
- **Event thread** — winit; forwards input to QEMU without waiting.

Rule: no QEMU-owned thread waits on the GPU; no render/audio thread takes a
QEMU lock.

## Acceleration per platform

| Host | Win98/XP (x86 guests) | Notes |
|---|---|---|
| Linux x86_64 | KVM | performance reference |
| Windows x86_64 | WHPX (TCG fallback) | WHPX quirks with old guests: test |
| macOS Apple Silicon | TCG (MTTCG) | Win98 easy; XP benchmarked at M1 |
| macOS/Linux x86_64 | HVF / KVM | secondary, should Just Work |

macOS JIT for TCG needs the `com.apple.security.cs.allow-jit` entitlement on
the player bundle — copy UTM's / qemu-3dfx-macos's approach.

## Repo layout

```
/player/              Rust: the running-machine window (wgpu, librashader, embed bindings)
/launcher/            Rust: companion app
/libdisc/             Rust: CD disc model, format parsers, C API, exerciser
/qemu/                submodule: upstream QEMU pinned to qemu-3dfx cadence (v9.2.4)
/third_party/qemu-3dfx  submodule: 3dfx/Mesa pass-through overlay + patch + wrappers
/patches/qemu/        our patch queue (embed API, ATAPI device)
/scripts/             prepare-qemu.sh, configure-qemu.sh
/guest-tools/         driver ISO build (Rust tooling + era binaries)
/shaders/             curated slang presets
/reference/           rig captures: CRT photos, ATAPI traces, benchmarks
/docs/                these documents
```

## Open questions (tracked, not blocking)

- qemu-3dfx host GL output → wgpu texture: GL/Metal interop on macOS
  (IOSurface), GL/Vulkan on Linux (dma-buf / external memory), GL/D3D12 on
  Windows — Spike A.
- In-proc QMP: JSON over in-memory pipe (cheapest, reuses tooling) vs. direct
  C API — start with JSON.
- QEMU version cadence pinned by qemu-3dfx (9.2.x today).
