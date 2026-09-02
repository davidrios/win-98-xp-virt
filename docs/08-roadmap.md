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

## M1 — Architecture validation  (essentially done, 2026-09-02)

Done, all through the in-process embed path:
- `10-embed-api` builds `libqemu-embed-<target>` from QEMU's meson; shim
  `embed/libqemu_embed.{h,c}` (lifecycle on one thread, display listener,
  input via bottom-half, VM control, audio ring, refresh interval; API v3);
  `qemu-embed` crate (hand-written FFI, rpath via `links` metadata).
- Display: FreeDOS and **Windows 98** render in the player — Linux and the
  M1 Air (dylib). sRGB-correct on macOS swapchains.
- Input: keyboard (scripted `PLAYER_KEYS` verified in FreeDOS; typing in
  Win98 on the Air), USB-tablet absolute mouse (never grabs), PS/2 relative
  grab with Ctrl+Alt+G release.
- Audio: `embed` audiodev → SPSC ring → cpal; verified (ring fills on
  FreeDOS `echo ^G`; Win98 plays sounds on the Air).
- librashader chain: `--shader <preset.slangp>` (submodule
  `third_party/slang-shaders`); verified with crt-lottes via GPU readback.
- Latency instrumentation (`PLAYER_LATENCY=1`), 16 ms refresh pull.
  **Measured on the M1 Air (Win98, crt-lottes, 2026-09-02):** publish→present
  p50 6–10 ms, p95 15–17 ms, max 18 ms — the vsync-phase floor at 60 Hz;
  Linux/Wayland reads the same. Meets the ≤ 1 host frame budget (doc 03).
- Spike A step 1: qemu-3dfx GL pass-through at 500+ fps on the Air
  (standalone `-display sdl`, SDL/native-OpenGL backend, no XQuartz at
  runtime).

Open before calling M1 closed:
- ~~XP boot + TCG benchmark on the Air against the rig baseline (doc 09).~~
  Done 2026-09-02: XP boots in the player (sound, tablet, clean power-off),
  ~30 s to desktop on both; vs. the rig's P4 1.7: integer 1.3–2× faster
  (7-Zip), x87 FP 21 % (Super PI 1M 9:49 vs 2:02) —
  `reference/benchmarks/README.md`.
- QMP over socketpair (snapshots/media) — not started; design in doc 11.
- Windows host untested throughout.

## M2 — Pixel accuracy + input polish

- Mode analysis table (doc 03): pixel aspect, double-scan shader params, text
  modes; geometry updates on mode change; golden-image tests; curated preset
  pack v1 calibrated against rig CRT photos.
- Relative-mouse grab, fullscreen, hotkeys, overlay basics.
- **Exit:** mode-sweep test passes; a DOS game under Win98 looks right and
  mouselook feels right.

## M3 — 3D for Win98  (design: doc 12; pulled forward, starts after M1 closes)

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
| XP-on-TCG too slow for late-era titles | measured in M1 vs. a P4 1.7: integer 1.3–2× faster, x87 FP 21 % (Pentium II class). FP-heavy late-era titles are out of scope for Apple Silicon claims; early-XP/late-98 titles need per-title validation in M4 |
| librashader/wgpu version coupling | follow librashader's wgpu pin |
| Fork drift from upstream QEMU | patch-queue discipline; upstream-first style |
| Protection checks needing dump features users' rips lack | loud UX messaging about required dump formats (doc 05) |
