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

## M1 — Architecture validation  ✅ (2026-09-02; Windows host untested)

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
  (7-Zip), x87 FP 21 % (Super PI 1M 9:49 vs 2:02), 31 % after patch 05's
  host-FPU fast path (6:33) — `reference/benchmarks/README.md`.
- ~~QMP over socketpair (snapshots/media).~~ Done 2026-09-02: `player/src/qmp.rs`
  — socketpair, `-chardev socket,fd=N -mon mode=control`, id-matched
  synchronous `execute`, event queue drained on the UI thread;
  `PLAYER_QMP=1` logs every event, `PLAYER_QMP_EXEC='<json>'` runs
  commands after the first guest frame (verified: query-version/status/
  block, error classes, RTC_CHANGE events on FreeDOS).
- Windows host untested throughout (stays open; not blocking M3).

## M3 progress (2026-09-02)

GL pass-through renders inside the player on Linux **and macOS**: qemu-3dfx
UI seam behind a vtable (patch 30), window-less backends in the embed
library (patch 31/32 + `embed/mglcntx_embed.c`: EGL surfaceless pbuffer on
Linux, CGL + FBO stand-in on macOS), embed API v4. Win98 wglgears through
the player: 420–450 fps at 800×600 on Linux, `GL 2.1 Metal / Apple M1` on
the Air with the readback path; **zero-copy on Linux** (dma-buf ring →
Vulkan import, API v5): 575–600 fps; **zero-copy on macOS** (IOSurface ring
→ Metal, API v6) verified on the Air. Remaining: Glide (doc 12).

## M2 — Pixel accuracy + input polish

- ~~Mode analysis table (doc 03): pixel aspect, double-scan shader params, text
  modes~~ ✅ 2026-09-05 (`player/src/mode.rs`, doc 03 "Mode analysis"): the
  geometry stage takes the display aspect from the table, and the scanline
  count reaches the preset through `vga_mode` / `inter` — 320×200 draws its
  400 scanlines instead of 200, 640×480 its 480 instead of the 240 the
  preset's interlace guess gave it. `player --mode-sweep` is the check
  (`scripts/test.sh`), `PLAYER_MODE_PARAMS=0` the control. Left: presets with
  no resolution override (crt-lottes, crt-royale) cannot be told — see doc 03.
- Geometry updates driven off the QEMU surface change; golden-image tests;
  overscan crop; curated preset pack v1 calibrated against rig CRT photos.
- Relative-mouse grab, fullscreen, hotkeys, overlay basics.
- **Exit:** mode-sweep test passes; a DOS game under Win98 looks right and
  mouselook feels right.

## M3 — 3D for Win98  (design: doc 12; pulled forward, starts after M1 closes)

- 3D output through the librashader chain per Spike A; guest tools ISO build
  (SoftGPU + matching wrappers + AC'97/net drivers).
- **Exit:** Win98 acceptance titles (doc 04 matrix) accelerated on all three
  platforms, CRT-shaded.

## M4 — XP + 3D: the paravirtual Direct3D device (doc 14, ADR-006)

- P0 spike: DXVK d3d9 native over MoltenVK / Vulkan off-screen — decides
  the host executor. P1 transport + device + D3D9TEST triangle. P2
  resources and fixed function. P3 shaders and queries. P4 D3D8 over d3d9.
- WineD3D-in-guest (guest-tools ISO) stays the fallback and the DX7 path
  (ADR-013: and stays it after M10, for hosts below Vulkan 1.3);
  FIFA 2000 findings parked in doc 14.
- XP reference machine tuned; real-GPU screenshot diffs against the rig's
  GeForce 6200.
- **Exit:** XP acceptance titles (Max Payne, GTA:VC) accelerated through the
  device on all three platforms; Apple Silicon results documented honestly.

## M7 — A real guest display driver (ADR-008; after M4, interleaves with M5/M6)

- ~~M7a framebuffer driver~~ ✅ 2026-09-04 (doc 15): `d3dpt-vga` PCI
  adapter (stdvga core + register BAR) + `d3dptvid.sys` / `d3dptdisp.dll`
  built with mingw-w64; XP desktop at 1024×768×32@85 from the host's mode
  table, no copy inside QEMU, installed by `DRIVER\DRVINST.EXE`. Cirrus
  replaced for XP; M2's mode table plugs into the device's table.
- M7b DirectDraw DDI on the same driver (DX5–7 titles without WineD3D).
- M7c Direct3D DDI: DP2 tokens → device records; Microsoft's d3d8/d3d9
  stay in the guest, no DLL in the game folder. The DLL device (M4)
  remains the Win98 path.
- **Exit:** the doc 04 matrix through the driver on XP; M4's per-game DLL
  install no longer needed on XP.

## M5 — CD-ROM backend (track opened 2026-09-04: `docs/tracks/m5-cdrom-backend.md`, spec doc 17)

- M5a libdisc (Rust): cue/bin + ISO model, EDC/ECC, Q synthesis, C API,
  the `discx` exerciser; `cdimage` QEMU block driver; ATAPI patch: raw
  sector reads, READ CD / READ CD MSF, subchannel, raw TOC; DOS ATAPI
  guest test.
- M5b CD-DA playback (`audiodev` on `ide-cd`), mode pages, runtime disc
  swap over QMP, player command lines.
- M5c CCD + `.sub` replay, a SecuROM title from an owned dump; golden
  ATAPI traces from the rig as fixtures (doc 09).
- M5d SafeDisc on the L-EC path with a real dump.
- M5e CHD, MDS/DPM, timing profile as needed; disc shelf with M6.
- **Exit:** doc 05 acceptance table green for CD-DA, SafeDisc, SecuROM rows.

## M6 — Launcher, packaging, release

- Launcher (machine library, guided creation, snapshots, disc shelf); signed
  builds / Flatpak; shader pack release; docs site from these documents.
- **Exit:** a stranger can go from install → playing a disc dump of a 1999
  game with a CRT shader in under an hour, on any of the three platforms.

## M9 — TCG on Apple Silicon (track opened 2026-09-05: `docs/tracks/m9-tcg-aarch64.md`)

- Profile first: XP idle / 7-Zip / a game under TCG on the Air, the vCPU
  thread split into generated code, helpers, softmmu, translation; the hot
  guest instructions' host code classified (`tools/tcg-profile.sh`,
  `tools/tcg-hot.py`).
- Then the optimization the data picks (candidates: flags in NZCV in the
  aarch64 backend, barrier elision on one vCPU, cheaper TLB lookups),
  with M8's on/off-oracle methodology. First results (2026-09-05, from
  the Moto Racer profile): patch 15 (TB invalidation: the vAPIC ROM page
  storm at every interrupt, per-page code ranges, no jump-cache flush per
  TB) and patch 16 (a 4096-entry floor for the dynamic TLB, which XP's
  context-switch flushes had shrunk to 64–256 entries) — the game's vCPU
  from 14 % to 57 % generated code.
- The structural option — TCG's output inside a Hypervisor.framework VM
  with the x86 page tables mirrored in stage 1 — measured by
  `tools/hvf-el1/` (2026-09-05): feasible, ~45 KLOC of the vCPU core
  built freestanding + cputlb rewritten as a fault-driven mirror, 4–8
  weeks; decided after the working-set measurement (track doc).
- Patches 17–20 (2026-09-05): the REP fast path, same-value SMC stores
  (Moto Racer's race 5×), the RCU/TLS hot paths, the inline jump-cache
  probe (7-Zip +12 %). Patch 21, doc 18: the register file in x20–x28
  across chained TBs — built, 7-Zip decompress +15 %, off by default
  until two open items close (track doc).
- **Exit:** a measured, reproducible gain on the game workload with both
  guest batteries identical and `scripts/test.sh all` green.

## Post-v1 candidates

Recording/streaming, gamepads / DirectInput, CRT bezel packs, VRR pacing,
suspend/resume, upstreaming campaign (libdisc, embed API).

## Standing risks

| Risk | Watch/mitigation |
|---|---|
| Spike A: GL→wgpu interop on macOS (IOSurface) | fallback: texture copy via CPU-free path, ANGLE/Zink under qemu-3dfx (doc 04); last resort 3D bypasses the shader chain on macOS |
| qemu-3dfx maintenance/version coupling | pin QEMU to its cadence; wrappers built from our fork |
| Apple GL deprecation | ANGLE/Zink escape hatch (doc 04) |
| XP-on-TCG too slow for late-era titles | measured in M1 vs. a P4 1.7: integer 1.3–2× faster, x87 FP 31 % with patch 05 (Pentium III class; 21 % before). FP-heavy late-era titles are out of scope for Apple Silicon claims; early-XP/late-98 titles need per-title validation in M4 |
| librashader/wgpu version coupling | follow librashader's wgpu pin |
| Fork drift from upstream QEMU | patch-queue discipline; upstream-first style |
| Protection checks needing dump features users' rips lack | loud UX messaging about required dump formats (doc 05) |
