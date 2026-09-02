# 1. Goals and non-goals

## Vision

A Windows 98 or Windows XP machine that feels like sitting at the real thing
around 1998–2005: games install from your own disc dumps (copy protection and
all), Direct3D and Glide titles run accelerated, and the picture on screen
looks like a shadow-mask CRT fed by a VGA card — not a blurry stretched
rectangle in a window.

## Goals

1. **Cross-platform, open source.** Linux, Windows, macOS. Apple Silicon is a
   hard requirement and a first-class target, not a port. Everything in the
   stack must be open source (this ruled out VMware; VirtualBox was ruled out
   on capability — no 3D for pre-Win7 guests since 6.1).
2. **Real guest 3D.** Direct3D (via wrappers), Glide, and OpenGL working in
   the guest with host-GPU acceleration, for both Win98 and XP.
3. **Pixel-accurate, period-accurate video.** The raw guest framebuffer is
   captured before any scaling, presented with correct aspect (including
   non-square-pixel modes like 320×200), integer/sharp scaling, and a
   high-quality CRT shader chain (libretro-format "slang" shaders via
   librashader on wgpu).
4. **Faithful CD-ROM drive emulation.** Raw dump formats (cue/bin, CCD, MDS,
   CHD) mount as a virtual drive that behaves like period hardware: CD-DA
   audio tracks, subchannel data, C2 error behavior, raw TOC. Era copy
   protection running inside the guest passes its checks because the drive is
   faithful — nothing is patched or bypassed.
5. **Low latency.** This is for games. QEMU runs in-process with the frontend;
   the display, input, and audio paths are designed for minimal added latency
   (see doc 03).
6. **UTM-style UX.** A machine library, guided machine creation for the two
   guest families, sane defaults, one-click driver/tools media. Nobody should
   need to hand-write a 40-flag QEMU command line. Delivered as a standalone
   player (one machine per window) plus a companion launcher (library and
   creation) — see docs 02/07.

## Non-goals

- **Cycle-accurate vintage hardware emulation.** That is 86Box/PCem, and they
  do it well. We target "fast machine of the era" behavior, not
  chip-accurate timing.
- **Guests other than Win9x/ME and Win2k/XP.** Nothing newer, nothing weirder
  (at least until the four pillars are solid).
- **General-purpose VM manager.** We are not competing with virt-manager or
  UTM for Linux/modern-Windows guests.
- **Bypassing or stripping DRM.** The CD pillar makes protected originals
  *work* by being faithful; it is a preservation-grade drive emulation, not a
  crack. No-CD patches, key generators, activation workarounds are out of
  scope. Users supply their own install media, licenses, and dumps.
- **Networking-era features** (shared folders beyond basics, clipboard
  integration, etc.) — nice-to-haves later, not pillars.

## The four pillars

| # | Pillar | Novelty |
|---|---|---|
| P1 | QEMU + qemu-3dfx builds and machine configs for Win98/XP on all three platforms | Integration |
| P2 | Guest-side driver/tools packaging (SoftGPU, 3dfx wrappers, audio/net drivers) | Integration |
| P3 | Player: in-process embed + pixel-accurate CRT-shaded display pipeline (wgpu + librashader), plus companion launcher | **Original work** |
| P4 | Raw CD-ROM ATAPI backend for QEMU (Rust "libdisc") | **Original work** |

P3 and P4 are the contributions that don't exist anywhere today and are
designed to be upstreamable / reusable by the wider retro-VM community.
(A RetroArch-core variant of P3 was tried and dropped — ADR-003/ADR-005.)
