# 10. Decision records

Short ADR-style log. Newest last. Docs elsewhere reflect the current state;
this file preserves the *why*.

## ADR-001: QEMU as the base (2026-08-31)

Only open-source option covering Linux/Windows/macOS incl. Apple Silicon with
a working guest-3D path for both Win98 and XP (qemu-3dfx). VMware rejected
(closed source, no ARM mac). VirtualBox rejected (3D for pre-Win7 guests
removed in 6.1+). 86Box out of scope (authentic-hardware emulation already
exists; we don't duplicate it).

## ADR-002: QEMU runs in-process with the display (2026-08-31)

Gaming latency requirement: framebuffer, input, and audio must not cross a
process boundary. UTM-style embed patches. Consequence: one VM per hosting
process; GPL-2.0 for anything linking QEMU.

## ADR-003: Primary frontend is a libretro core; custom app demoted to companion (2026-08-31)

A RetroArch core replaces most of the planned custom display pipeline and
player: native slang CRT shaders, mature frame pacing, input capture, audio,
disk-control API for CD swapping, cross-platform packaging, existing
user base. Precedent: DOSBox Pure (Win98-in-RetroArch works and has demand).
The `libqemu_embed.h` layer is frontend-agnostic, so a standalone player
remains possible later at low cost; the **companion launcher** (machine
creation, snapshots, disc shelf, guest-tools media) covers what RetroArch's
content model does badly. Accepted risks: qemu-3dfx GL inside libretro
hw-render contexts on macOS (spike scheduled in M0), large save states,
living with libretro API constraints.

## ADR-004: Rust where possible (2026-08-31)

All original code is Rust unless a hard boundary forces C:

| Component | Language | Why |
|---|---|---|
| libretro core shell (cdylib) | **Rust** | thin FFI over small API |
| embed-API bindings | **Rust** (bindgen) | consumer side |
| companion launcher | **Rust** | greenfield GUI |
| CD disc-model crate + format parsers ("libdisc") | **Rust** | greenfield, parser-heavy = Rust sweet spot; QEMU upstream now accepts Rust (experimental since 9.2), so upstreaming is plausible |
| MMC exerciser, tooling, CI | **Rust** | greenfield |
| QEMU patches: embed API, ATAPI device glue | C | inside QEMU; thin shims calling libdisc |
| qemu-3dfx host patches | C | third-party, version-coupled |
| Guest-side wrappers/drivers | C (era toolchains) | Rust cannot target Win9x/XP |

Consequence for doc 05: disc model is implemented in Rust; libmirage becomes
a behavioral reference (readable GPL source, format documentation), not a
linked dependency — this also drops its glib dependency problem.

## ADR-005: Standalone player is the frontend; RetroArch/libretro dropped (2026-09-02)

Supersedes ADR-003. After hands-on time with RetroArch the user judged it too
buggy to build on ("the thing is so full of bugs it's not even worth it").
The frontend returns to the original design: a **standalone Rust player**
(winit + wgpu + librashader + egui + cpal) with in-process QEMU (ADR-002
unchanged), plus the **launcher** for machine management — the two-process
model from the first architecture draft. Consequences:

- We own the whole presentation pipeline again (doc 03 fully in scope): CRT
  shaders via librashader's wgpu runtime (Metal on macOS), frame pacing,
  input grab, audio. More work than the core route, but no dependence on
  RetroArch's quality or API constraints.
- qemu-3dfx integration target becomes "guest GL output → wgpu texture"
  (Spike A retargeted: docs/spikes/spike-a-macos.md) instead of a libretro
  hw-render context. No libretro hw-render risk on macOS anymore; the
  GL↔Metal interop question remains and is the spike.
- Spike B (libretro crate choice) is void. The M0 libretro core validated in
  RetroArch is deleted; its test pattern lives on in `player/`.
- The embed API stays frontend-agnostic on principle; a libretro shell could
  be re-added by a third party, we won't maintain one.

## ADR-006: Direct3D 8/9 for XP through our own paravirtual device (2026-09-03)

**Decision.** Direct3D 8/9 acceleration for the XP guest is a paravirtual
Direct3D device of our own: thin guest `d3d9.dll`/`d3d8.dll` that serialize
the API into a shared-memory command stream, and a host-side executor that
runs the same D3D9 semantics natively (DXVK's d3d9 over Vulkan, MoltenVK on
macOS). Design in doc 14. Guest-side WineD3D (JHRobotics' wine9x build on
the guest-tools ISO) stays as the fallback and as the only path for
DirectDraw / Direct3D ≤7 until the device grows a ddraw layer.

**Why now.** Two days with WineD3D-in-guest on XP (FIFA 2000, doc 00):

- Every fix so far was in a 2015 fork with no upstream: 24-bit desktop
  modes (`patches/wine9x/01`), front-buffer presentation on our FBO stand-in
  (`embed/mglcntx_embed.c`), and the open ones (palettized/dynamic texture
  corruption, per-flush flicker, DirectInput focus after the mode switch).
  Each is diagnosable, none is the last.
- The structural cost is unfixable there: WineD3D does all state tracking
  and D3D→GL translation *inside the emulated x86 guest*, at TCG speed,
  and then ships GL calls through the FIFO one by one. A paravirtual device
  moves that translation to native host code; the guest only serializes.
  On Apple Silicon that is the difference between "works" and "plays".
- Doc 04 already listed "a community D3D9 paravirt device, if it
  materializes" as the adoption target. None has (VirtualBox's WDDM/DX
  path is Vista+, VMware's SVGA3D is closed and Win7+). We build it.

**Alternatives rejected.** *Proton*: a different product (Windows games on a
Linux host, no Windows guest, no macOS) — its components, not its shape, are
relevant. *DXVK in the guest*: needs a Vulkan driver in XP; none exists.
*Keep fixing WineD3D 1.7.55*: see above; it remains the fallback.

**Licensing.** Wine is LGPL-2.1-or-later: copying Direct3D behaviour *and
code* from current Wine (d3d9/d3d8 COM plumbing, d3d8→d3d9 mapping, format
tables, tests) into the guest DLLs and the host executor is allowed; those
components carry the LGPL, ship with source, keep their headers. DXVK is
zlib. d3d8to9 is BSD-2. QEMU is GPL-2.0 and qemu-3dfx GPL-2.0; all of these
combine (LGPL/zlib/BSD code inside a GPL-2.0 program is fine). Nothing from
Microsoft's SDK ships: headers come from mingw-w64 (public domain / ZPL).

**Consequences.** New milestone in doc 08 (M4 becomes the device). The
qemu-3dfx FIFO/MMIO model, the FXPTL/MAPMEM guest driver, the embed frame
path (IOSurface / dma-buf) and the player's 3D layer are reused as-is; the
new pieces are the guest DLLs, a `d3dpt` device in QEMU, and a host executor
linking DXVK. C++ enters the host side (DXVK) behind a C shim; guest DLLs
are C. Apple Silicon depends on DXVK-over-MoltenVK being good enough for
D3D9-era feature levels: spike first (doc 14 P0), fallback is a host-side
WineD3D or a wgpu backend later. The x87 work (patch 06, doc 13) is
unaffected: games still set PC=24 through our d3d9.dll's CreateDevice.
