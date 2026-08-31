# 8. Roadmap and milestones

Ordered so the riskiest bets are validated first (libretro hw-render 3D on
macOS, in-process embedding, Apple Silicon TCG performance). Each milestone
ends in something runnable in RetroArch.

Reference-rig capture sessions (doc 09) slot in as inputs: rig benchmarks
before M1 (baseline for the Apple Silicon XP verdict), CRT photo set before
M2, ATAPI traces and disc dumps before M5, real-GPU screenshots during M3/M4.

## M0 — Foundation + the two spikes

- Repo scaffold per doc 02; QEMU submodule pinned to the qemu-3dfx-supported
  release; patch-queue tooling; Rust workspace (core, libdisc, launcher
  stubs).
- CI: patched QEMU + workspace on Linux x86_64, Windows x86_64, macOS arm64;
  headless libretro harness smoke test.
- **Spike A (go/no-go):** qemu-3dfx host GL rendering inside a libretro
  hw-render context, including macOS/Apple Silicon under RetroArch. Result
  recorded in doc 10.
- **Spike B:** rust-libretro crate choice vs. own `retro_*` shim (hw-render
  support decides).
- **Exit:** a "hello world" core boots FreeDOS in-process and shows frames in
  RetroArch on all three platforms; Spike A verdict written down.

## M1 — Core validation

- Embed API (`libqemu_embed.h`) fleshed out; display handoff, keyboard/mouse
  via game focus, audio; machine-bundle loading.
- Win98 boots and is usable in RetroArch; XP boots on Apple Silicon and is
  benchmarked against the rig baseline (doc 09).
- Latency instrumentation (dirty→upload→present overlay).
- **Exit:** Win98 desktop, CRT-shaded via stock RetroArch presets,
  mouse/keyboard/audio working, all three platforms; added display latency
  ≤ 1 host frame measured.

## M2 — Pixel accuracy + input polish

- Mode analysis table (doc 03): pixel aspect, double-scan shader params, text
  modes; geometry updates on mode change; golden-image tests; curated preset
  pack v1 calibrated against rig CRT photos.
- Relative-mouse polish, core options, disk-control skeleton (ISO swap).
- **Exit:** mode-sweep test passes; a DOS game under Win98 looks right and
  mouselook feels right.

## M3 — 3D for Win98

- qemu-3dfx patches in the fork; hw-render integration per Spike A; guest
  tools ISO build (SoftGPU + matching wrappers + AC'97/net drivers).
- 3D output through the slang chain (CRT-shaded Glide).
- **Exit:** Win98 acceptance titles (doc 04 matrix) accelerated on all three
  platforms, in RetroArch.

## M4 — XP + 3D

- XP reference machine tuned; XP guest tools (wrappers, D3D8/9 DLLs,
  drivers); real-GPU screenshot diffs against the rig's GeForce 6200.
- **Exit:** XP acceptance titles pass per matrix; Apple Silicon results
  documented honestly.

## M5 — CD-ROM backend

- libdisc (Rust): cue/bin + CCD first; ATAPI command coverage incl.
  C2/subchannel/raw TOC; CD-DA playback; disk-control runtime swap complete.
- Golden ATAPI traces from the rig as exerciser fixtures (doc 09).
- Then CHD, MDS/DPM, timing profile as needed.
- **Exit:** doc 05 acceptance table green for CD-DA, SafeDisc, SecuROM rows.

## M6 — Launcher, packaging, release

- Companion launcher (machine library, guided creation, snapshots, disc
  shelf); signed builds; core submitted toward the libretro core installer;
  shader pack release; docs site from these documents.
- **Exit:** a stranger can go from install → playing a disc dump of a 1999
  game with a CRT shader in under an hour, on any of the three platforms.

## Post-v1 candidates

libretro save states (retro_serialize for big-RAM guests), gamepads /
DirectInput, standalone wgpu player (if RetroArch friction warrants —
ADR-003), CRT bezel packs, upstreaming campaign (libdisc, embed API).

## Standing risks

| Risk | Watch/mitigation |
|---|---|
| Spike A fails on macOS (GL hw-render under Metal-era RetroArch) | fallbacks: GL video driver requirement on macOS, offscreen GL + copy, or ANGLE/Zink (doc 04); worst case revives standalone player for 3D |
| qemu-3dfx maintenance/version coupling | pin QEMU to its cadence; wrappers built from our fork |
| XP-on-TCG too slow for late-era titles | measured in M1 vs. rig baseline; scope claims to data |
| libretro API constraints bite (refresh, states, disc UX) | ADR-003 hybrid keeps the standalone-player exit open |
| Fork drift from upstream QEMU | patch-queue discipline; upstream-first style |
| Protection checks needing dump features users' rips lack | loud UX messaging about required dump formats (doc 05) |
