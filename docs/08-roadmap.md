# 8. Roadmap and milestones

Ordered so the riskiest bets are validated first (in-process embedding,
3D-output interop into wgpu, Apple Silicon TCG performance). Each milestone
ends in something runnable.

Reference-rig capture sessions (doc 09) slot in as inputs: rig benchmarks
before M1 (baseline for the Apple Silicon XP verdict), CRT photo set before
M2, ATAPI traces and disc dumps before M5, real-GPU screenshots during M3/M4.

## M0 — Foundation  ✅ (2026-09-02)

- Repo scaffold per doc 02; QEMU submodule pinned v9.2.4 (qemu-3dfx cadence);
  qemu-3dfx submodule; `prepare-qemu.sh` (overlay + patch + sign) and
  `configure-qemu.sh` (uv-managed Python); patched QEMU builds and runs with
  glidept/glidelfb/mesapt regions live — verified on Linux x86_64 (Arch) and
  **macOS Apple Silicon (M1 Air)**. Windows untested.
- Rust workspace: `player` (winit + wgpu 30 window, XRGB8888 test pattern
  with integer 4:3 viewport, mailbox present), `libdisc` (MSF/LBA + types),
  `launcher` stub. CI (manual trigger) for Linux/Windows/macOS-arm64.
- Detour: a libretro core was built and validated in RetroArch, then dropped
  (ADR-005).

## M1 — Architecture validation

- `10-embed-api` patch: `libqemu_embed.h`, QEMU built as a linkable library;
  bindgen bindings; player boots FreeDOS then Win98 in-process with the
  display listener feeding the wgpu texture.
- librashader-wgpu chain with one CRT preset; keyboard/mouse injection; cpal
  audio; latency HUD (dirty→upload→present).
- XP boots on Apple Silicon and is benchmarked against the rig baseline.
- **Spike A (parallel, on the M1 Air + Linux):** qemu-3dfx host GL output →
  wgpu texture interop per platform (docs/spikes/spike-a-macos.md).
- **Exit:** Win98 desktop, CRT-shaded, mouse/keyboard/audio working on all
  three platforms; added display latency ≤ 1 host frame measured.

## M2 — Pixel accuracy + input polish

- Mode analysis table (doc 03): pixel aspect, double-scan shader params, text
  modes; geometry updates on mode change; golden-image tests; curated preset
  pack v1 calibrated against rig CRT photos.
- Relative-mouse grab, fullscreen, hotkeys, overlay basics.
- **Exit:** mode-sweep test passes; a DOS game under Win98 looks right and
  mouselook feels right.

## M3 — 3D for Win98

- 3D output through the librashader chain per Spike A; guest tools ISO build
  (SoftGPU + matching wrappers + AC'97/net drivers).
- **Exit:** Win98 acceptance titles (doc 04 matrix) accelerated on all three
  platforms, CRT-shaded.

## M4 — XP + 3D

- XP reference machine tuned; XP guest tools; real-GPU screenshot diffs
  against the rig's GeForce 6200.
- **Exit:** XP acceptance titles pass per matrix; Apple Silicon results
  documented honestly.

## M5 — CD-ROM backend

- libdisc (Rust): cue/bin + CCD first; ATAPI command coverage incl.
  C2/subchannel/raw TOC; CD-DA playback; runtime disc swap UX.
- Golden ATAPI traces from the rig as exerciser fixtures (doc 09).
- Then CHD, MDS/DPM, timing profile as needed.
- **Exit:** doc 05 acceptance table green for CD-DA, SafeDisc, SecuROM rows.

## M6 — Launcher, packaging, release

- Launcher (machine library, guided creation, snapshots, disc shelf); signed
  builds / Flatpak; shader pack release; docs site from these documents.
- **Exit:** a stranger can go from install → playing a disc dump of a 1999
  game with a CRT shader in under an hour, on any of the three platforms.

## Post-v1 candidates

Recording/streaming, gamepads / DirectInput, CRT bezel packs, VRR pacing,
suspend/resume, upstreaming campaign (libdisc, embed API).

## Standing risks

| Risk | Watch/mitigation |
|---|---|
| Spike A: GL→wgpu interop on macOS (IOSurface) | fallback: texture copy via CPU-free path, ANGLE/Zink under qemu-3dfx (doc 04); last resort 3D bypasses the shader chain on macOS |
| qemu-3dfx maintenance/version coupling | pin QEMU to its cadence; wrappers built from our fork |
| Apple GL deprecation | ANGLE/Zink escape hatch (doc 04) |
| XP-on-TCG too slow for late-era titles | measured in M1 vs. rig baseline; scope claims to data |
| librashader/wgpu version coupling | follow librashader's wgpu pin |
| Fork drift from upstream QEMU | patch-queue discipline; upstream-first style |
| Protection checks needing dump features users' rips lack | loud UX messaging about required dump formats (doc 05) |
