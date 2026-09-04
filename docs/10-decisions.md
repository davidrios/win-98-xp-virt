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

## ADR-007: The host executor is DXVK on every platform; macOS runs it over KosmicKrisp (2026-09-03)

**Decision.** The paravirtual Direct3D device's host executor (ADR-006,
doc 14) is DXVK's d3d9, built natively as a library next to QEMU, on Linux
and macOS alike. On macOS the Vulkan implementation under it is **Mesa's
KosmicKrisp** (LunarG's Vulkan-on-Metal-4 driver, Vulkan 1.4 conformant on
Apple Silicon, prebuilt in the LunarG macOS SDK), which requires **macOS 26**
— the Air gets upgraded. MoltenVK is not a supported configuration.
DXVK is carried as a submodule (`third_party/dxvk`) plus a patch queue
(`patches/dxvk/`), the same discipline as the QEMU fork.

**Why.** Spike C (`docs/spikes/spike-c-dxvk-native-macos.md`): DXVK
master refuses MoltenVK 1.4.2 outright — five hard-required features are
missing (geometry shaders, cull distance, depth-clip-enable,
robustBufferAccess2, nullDescriptor), and the two robustness ones are
unimplementable without shader-side bounds checks Metal lacks (MoltenVK
issue open since 2025-02). KosmicKrisp advertises all of them except
geometry shaders, which d3d9 never uses: a one-line "required → optional"
patch. DXVK's core compiles on macOS; only its Linux-only Windows shim
needs ~30 lines.

**Alternatives rejected.** *Implement the features in MoltenVK*: harder than
patching DXVK for the same result. *DXVK-on-MoltenVK with a dummy-resource
patch queue* (the Gcenx DXVK-macOS pattern): viable as a bridge but fights
DXVK's descriptor-heap design on every rebase; not pursued since the OS
upgrade is accepted. *A native D3D9-on-Metal executor inspired by DXVK*:
a rewrite of ~40 k lines plus years of fidelity work, and a second executor
to keep bug-for-bug equal with Linux; last resort only. *wgpu translator*:
same magnitude.

**Consequences.** Minimum macOS for the player's Direct3D path is 26
(Tahoe); GL pass-through and everything else keep working on 15. The
player links DXVK through a C shim; a window-less WSI backend replaces
SDL2 (the device renders off-screen into the existing IOSurface / dma-buf
frame path). KosmicKrisp is young: Metal 4 workarounds for M1/M2 on macOS
26 live in the driver (fixed in 27); the rig goldens (`reference/d3d`)
are the acceptance test for both drivers. **Verified 2026-09-03** on the
Air (macOS 26.6.2, SDK 1.4.357.1): one more required feature had to be made
optional (`fillModeNonSolid`, patch 05), then the reference frame matches
the rig as closely as on RADV (spike C, "Verified on KosmicKrisp").

## ADR-008: A real guest display driver is the long-term shape; staged after the DLL device (2026-09-04)

**Decision.** The paravirtual Direct3D device keeps ADR-006's shape today
(guest `d3d9.dll` / `d3d8.dll` serializers → `d3dpt` device → DXVK
executor), and we commit to growing a **real Windows display driver** on
top of the same transport and executor, in stages, as milestone M7 (doc
08). The driver replaces the DLL copies per game folder, replaces the
emulated Cirrus as the guest's display adapter, and is the only road to
Vista/7/10 acceleration should we ever want it. Nothing below the guest
side changes: the shared window, the doorbell, the record protocol
(`d3dpt/d3dpt_proto.h`) and `libd3dpt_exec` are the driver's back end as
they are the DLL's.

**Why.** After P1/P2 (2026-09-03/04) the DLL device works and matches the
rig, so the question is no longer "can the host run D3D9 for the guest"
but "how do games reach it". The DLL answer has structural limits:
- **Per-game installs.** A D3D9.DLL must sit next to every EXE (system-wide
  replacement fights Windows File Protection on XP), games that resolve
  `system32\d3d9.dll` by path, load through DirectDraw 7 in the same
  process, or check DirectX versions do not see it. On the rig the same
  folder-level fiddling already bit the reference workloads.
- **We re-implement the runtime.** State shadowing, managed pool, lost
  device, state blocks, software vertex processing: every semantic of
  Microsoft's d3d9.dll has to be re-done in our DLL. With a display driver
  Microsoft's runtime stays in the guest and talks to us through the
  **Direct3D DDI**, which on 2000/XP is *already a serialized command
  stream*: `D3dDrawPrimitives2` hands the driver DP2 token buffers (render
  states, texture stage states, draws, shader creation and constants —
  `d3dhal.h`, 32 opcodes), surfaces come through the DirectDraw DDI
  (`DdCreateSurface`, `DdLock`, `DdBlt`), GDI through `winddi.h`. That is
  our record stream with Microsoft doing the validation.
- **The desktop becomes ours.** A display driver owns the framebuffer and
  the mode list: any resolution and refresh the CRT shader wants (M2's
  mode table and pixel aspect), no Cirrus limits (XP has no driver for
  QEMU's standard VGA), DirectDraw and Direct3D 7 for free, a zero-copy
  2D path through the shared window. This is what VMware (SVGA II +
  SVGA3D) and VirtualBox (their XPDM/WDDM additions) do; both prove the
  shape and both show the cost.

**Alternatives kept in their place.** *The DLL device* stays: it is the
Win98 path regardless (9x has no comparable driver model worth targeting),
it is the debugging harness for the executor, and it ships now. *WineD3D
in the guest* stays the DX7 fallback until the driver's DirectDraw DDI
exists. *Vista/7/10 (WDDM: kernel miniport under dxgkrnl plus user-mode
D3D9/D3D10/D3D11 UMDs)* is explicitly **not** in scope for v1; the
decision only keeps the door open by choosing the transport that leads
there. A D3D11 UMD over our device is the size of VBoxDX, a multi-year
team effort.

**Staged plan (M7).**
1. **M7a — framebuffer driver (2D).** A video miniport (`video.h`, loaded by
   videoprt.sys) that exposes our shared window as the frame buffer and a
   mode list we control, plus a display driver DLL (`winddi.h`) that does
   GDI in software on that buffer (the `framebuf`/`vga` shape; ReactOS
   builds the same kind with GCC, ~3 k lines). QEMU side: a paravirtual
   framebuffer register set on the `d3dpt` device (mode, pitch, dirty
   rectangles, vsync) and the player showing it through the existing frame
   path. Exit: XP desktop at 1024×768@85 on the driver, zero-copy, no
   Cirrus, ScanDisk-clean shutdowns, a mode table the CRT presets pick
   from. This step pays for itself (M2) and teaches the kernel-side
   workflow: BSOD debugging over serial with WinDbg or the ReactOS tools,
   a `.inf` install, driver signing not required on XP.
2. **M7b — DirectDraw DDI.** `DdCreateSurface`/`DdLock`/`DdBlt`/`DdFlip` on
   the same driver → surfaces in the device's handle table, blits by the
   executor (DXVK `StretchRect`/`UpdateSurface`), overlays refused. Exit:
   DirectX 5–7 titles (FIFA 2000, doc 00) without WineD3D.
3. **M7c — Direct3D DDI.** `D3dContextCreate`, `D3dDrawPrimitives2`: the DP2
   token consumer that translates into our records (mostly 1:1), texture
   and shader DDIs, caps from the executor. Exit: the doc 04 matrix
   through Microsoft's own d3d8/d3d9 with no DLL in the game folder; the
   DLL device becomes the 9x path only.
4. **M7d — later, if ever:** WDDM for Vista+ (out of scope for v1).

**Toolchain and licensing.** mingw-w64 ships the DDI headers (`ddk/winddi.h`,
`ddk/video.h`, `d3dhal.h`) under its permissive terms; kernel-mode display
DLLs and miniports are plain PE files with no CRT, buildable with the
same cross toolchain as the wrappers (the build script's msvcrt/ISA
checks extend to them). ReactOS (GPL-2.0) is the reference for building
such drivers with GCC and for DDI behaviour; Wine has no display-driver
side. Nothing from Microsoft's DDK ships.

**Consequences.** Doc 08 gains M7 after M4; doc 14 P5's "proper PnP driver
instead of MAPMEM" becomes M7a. ADR-006 stands. The record protocol must
stay driver-neutral: no guest-DLL-only assumptions (handles are guest
chosen, resources are host objects, nothing in it knows about COM). The
executor grows the DDI-shaped operations M7b/c need (blits between
surfaces, DP2 semantics such as per-primitive vertex buffers) when those
stages start, not before.
