# 4. 3D acceleration: qemu-3dfx and guest drivers

## Strategy

We do not write a GPU emulator. Guest 3D comes from **API pass-through**:
guest-side wrapper libraries intercept Glide/OpenGL calls and forward them to
the host GPU via a QEMU device. That is the qemu-3dfx approach, and it is the
only open-source path that covers both Win98 and XP with host acceleration on
all our platforms, including Apple Silicon (qemu-3dfx has documented Apple
Silicon support and third-party ARM64 builds exist).

Direct3D — the API the user actually asked for — is covered indirectly:
D3D8/9 calls go through WineD3D-derived wrappers in the guest that translate
to OpenGL, which then passes through to the host. For Win9x, SoftGPU packages
this stack (DirectDraw/D3D → Mesa/WineD3D components) as an installable
driver.

## The pieces

| Layer | Win98 | Windows XP |
|---|---|---|
| Glide (2x/3x) | qemu-3dfx guest stubs → host translation | same (fewer titles care) |
| OpenGL | qemu-3dfx MESA GL pass-through | same |
| Direct3D 5–7 | SoftGPU (DDraw/D3D → wrapper stack) | rarely needed; wrappers exist |
| Direct3D 8/9 | WineD3D-based wrapper DLLs → GL pass-through | WineD3D-based wrapper DLLs → GL pass-through |
| 2D/desktop | SoftGPU display driver (QEMU std VGA path) | XP inbox VGA/vendor-neutral driver |

Known qemu-3dfx constraints we design around:

- **Version coupling:** the guest wrappers and the patched QEMU must be built
  from matching commits (the project enforces a commit-hash signature). Our
  guest-tools ISO (P2) is therefore *generated per build* of our QEMU fork,
  never a random downloaded binary.
- **QEMU version coupling:** the 3dfx patches track specific QEMU releases;
  our fork pins whatever upstream version the patches support.
- **Host GL on macOS:** pass-through lands on Apple's OpenGL framework
  (deprecated but present, GL 4.1 core / 2.1 compat). Long-term risk. Escape
  hatches if Apple ever removes GL: ANGLE (GL ES on Metal) or a Zink-style
  layer. Tracked, not solved now.
- **Windowing:** 3dfx-era games love exclusive fullscreen and mode changes;
  interaction with our render pipeline needs explicit testing (see doc 03,
  "3D and the pipeline").
- **libretro hw-render (ADR-003):** guest 3D must land in a libretro
  hw-render GL context so it flows through RetroArch's shader chain. This is
  the plan's riskiest integration — especially on macOS, where RetroArch is
  Metal-first — and is validated by Spike A in M0 before anything depends on
  it. Fallbacks in the roadmap risk table.

## Fallbacks and alternatives (documented, not primary)

- **SoftGPU software rendering** (llvmpipe-style in guest): always works, no
  host GPU needed; on Apple Silicon under TCG it is slow — acceptable for 2D
  desktop + light 3D only. This is the zero-config default before the user
  installs the accelerated stack.
- **86Box** for titles that demand a *real* emulated Voodoo (early Glide
  titles with driver-level tricks): out of scope for us; we document the
  recommendation.
- Watch list: virgl/venus will never target 9x/XP guests; any future
  community D3D9→native paravirt device would be adopted if it materializes.

## Guest tools ISO (P2)

One ISO per guest family, built by `/guest-tools/` scripts, containing:

- **Win98:** SoftGPU release (pinned), qemu-3dfx wrappers built from our fork
  commit, audio driver (AC97), network driver, USB/tablet support notes,
  unattended-friendly installer (batch/INF) where possible.
- **XP:** qemu-3dfx wrappers + D3D8/9 wrapper DLL set, AC97/HDA driver,
  network driver, tablet driver.
- A tiny in-guest `verify.exe`/batch that reports which APIs are accelerated
  (renderer strings, triangle smoke test) — our acceptance test hook.

## Acceptance matrix (M3/M4 exit criteria)

A small fixed game/benchmark set per API, run on all three host platforms:

- Win98: Quake 2 (GL), Unreal (Glide), Forsaken or Incoming (D3D6),
  3DMark99/2001SE.
- XP: Quake 3 (GL), Max Payne (D3D8), Half-Life 2 or GTA:VC (D3D9 — stretch),
  3DMark2001SE/03.

Pass = renders correctly at playable framerate with acceleration verifiably
active (renderer string ≠ software).
