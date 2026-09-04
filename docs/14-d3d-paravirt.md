# 14. Paravirtual Direct3D device for XP (ADR-006, 2026-09-03)

Direct3D 8/9 calls leave the guest as a command stream and are executed by
native host code. Guest-side WineD3D (doc 04 fallback, guest-tools ISO)
stays for DirectDraw / Direct3D ≤7 and as the comparison baseline.

## Why a device and not a better WineD3D

Under TCG every guest instruction costs ~10–20 host instructions. WineD3D
in the guest spends most of a frame translating D3D state into GL state
(shader generation, state tables, resource tracking) *before* anything
crosses to the host, and then crosses once per GL call. A serializer crosses
once per D3D call with almost no guest-side work, and the translation runs
natively. The same reasoning made qemu-3dfx pass GL and Glide through
instead of emulating a GPU.

## Shape

```
guest (XP)                                  host (QEMU process, embed lib)
 game.exe                                   d3dpt device (hw/d3dpt/)
   └ d3d9.dll  (ours, C, LGPL parts)  ──FIFO──▶  decoder / resource mirror
   └ d3d8.dll  (d3d8to9-style over d3d9)          └ executor: DXVK d3d9 (C++ behind a C shim)
 shared memory: cmd ring + data pages             └ Vulkan → MoltenVK (macOS) / native (Linux, Windows)
 FXPTL.SYS (qemu-3dfx MAPMEM) maps the device     present → embed_fx_frame / zero-copy ring (doc 12)
```

- **Transport:** the qemu-3dfx model — a PCI device with an MMIO doorbell
  page and a guest-physical shared area (command ring, argument data, bulk
  pages for Lock/Unlock uploads). The guest maps it through the FXPTL.SYS
  `\\.\MAPMEM` ioctl already installed for the GL wrapper (doc 00 gotcha);
  a proper PnP driver is a later polish item. Batched: the guest writes
  commands until a sync point (Present, Lock readback, GetRenderTargetData,
  queries, device creation) and rings the doorbell once.
- **Guest `d3d9.dll`:** COM objects for IDirect3D9 / Device / Swapchain /
  the resource interfaces. Each method is either *forward* (append
  opcode + args), *shadow* (state the app reads back — GetRenderState,
  GetTransform, caps — answered from a guest-side copy so no round trip),
  or *sync* (Present, Lock/Unlock, queries). Resource contents move through
  the bulk pages: Lock returns a guest buffer, Unlock copies the dirty
  box. Shader bytecode (SM1–3) passes through untouched; DXVK consumes it.
  `CreateDevice` sets the x87 control word to PC=24 unless
  `D3DCREATE_FPU_PRESERVE`, like native (that is what QEMU's inline x87
  mode 2 is for, doc 13). Code and behaviour may come from current Wine
  (LGPL; ADR-006).
- **Guest `d3d8.dll`:** D3D8 over our d3d9, the d3d8to9 approach (BSD-2):
  interface mapping, caps translation, SM1.1 passthrough.
- **Host decoder:** one thread per device (like mesapt), owns a mirror of
  handles → DXVK objects, validates arguments (a hostile guest must not
  crash the host), executes through DXVK's `IDirect3D9` natively. DXVK's
  d3d9 is a complete D3D9 implementation with a Windows-free build (the
  former dxvk-native, upstream since 2.0) that needs a WSI shim; we give it
  an off-screen swapchain whose backbuffer we read/blit into the existing
  frame path (IOSurface ring on macOS, dma-buf on Linux). MoltenVK on macOS
  is the one platform-specific risk (below).
- **Present:** the device presents explicitly at `Present`, once per frame,
  into `embed_fx_frame` / the zero-copy ring — none of the front-buffer
  flush heuristics the GL path needed.
- **Fallback:** the `-device d3dpt` off, the guest DLLs absent → the game
  loads Microsoft's d3d9 (software/no HAL) or WineD3D from the game folder,
  as today. Both stacks can coexist on one machine.

## Milestones (P = paravirt)

- **P0a — Reference workload, golden on the rig:** `D3DGAME9.EXE` and
  `D3DGAME8.EXE` (`guest-tools/src/d3dgame9.c`, `d3dgame8.c`, shared
  `d3dgame.h`; on the guest-tools ISO): a small deterministic game-like
  scene that exercises what era titles do — textured lit indexed cubes,
  a per-frame dynamic vertex buffer (software animation), additive alpha
  particles from DrawPrimitiveUP, a render-to-texture "monitor", DXT1 /
  565 / 8888 textures with mipmaps, fixed-function lights and materials,
  an optional SM1.1/2.0 shader path when D3DX is present, windowed and
  exclusive fullscreen with mode changes, vsync on/off, keyboard camera.
  `-frames N` runs a fixed-step deterministic sequence; `-dump N file.bmp`
  writes frame N as a BMP through GetRenderTargetData / CopyRects. It must
  run perfectly on the reference rig (P4 + GeForce 6200, doc 09) and its
  BMPs are the golden images every later layer is diffed against: WineD3D
  in the guest today, the device tomorrow. No game, no crack, no disc.
  **Done 2026-09-03:** both run flawlessly on the rig; the first golden set
  (d3dgame9 frame 300 windowed, fixed function and vs_1_1) with logs is in
  `reference/d3d/rig-2026-09-03/` (README there lists the caveats of that
  build: HUD bars are wall time, mask them; ps_1_1 refused by d3dx9_36's
  HLSL compiler so `-shader` is vs_1_1 + fixed pixel stage). Rendering is
  frozen at that build until a new golden set exists. `tools/bmpdiff.py`
  compares candidates against them.
- **P0b — Spike (decides the executor):** DXVK d3d9 native on macOS over
  MoltenVK and on Linux: clear + textured triangle + a SM2 shader, off-screen,
  read back. Measure. If MoltenVK cannot run DXVK's d3d9 for D3D9-era
  features (D3D9 needs Vulkan 1.1 + a few extensions DXVK lists), the
  executor becomes host WineD3D-over-GL or a wgpu translator; the guest side
  is unchanged either way.
- **P1 — Transport + device:** `hw/d3dpt` in the QEMU queue (patch 40),
  guest `d3d9.dll` with `Direct3DCreate9`, adapter identifier/caps from the
  host, `CreateDevice`, `Clear`, `Present` → the D3D9TEST triangle
  (guest-tools) shows in the player. Per-call and per-frame cost measured
  against WineD3D-in-guest on the same test.
- **P2 — Resources + fixed function:** vertex/index buffers, textures
  (all D3D9-era formats incl. DXT and palettized via conversion), Lock/Unlock,
  render/texture/sampler states, transforms, lights, DrawPrimitive*/UP
  variants, render targets, depth/stencil, device reset and lost-device
  protocol.
- **P3 — Shaders + queries:** SM1–3 vertex/pixel shaders, constants,
  occlusion/event queries, StretchRect, swap-chain variants, multi-head
  ignored. Acceptance: Max Payne (D3D8 via P4 stub), GTA:VC (D3D9), the
  doc 04 matrix.
- **P4 — D3D8:** `d3d8.dll` over d3d9.
- **P5 — later:** DirectDraw/D3D7 layer over the device (or keep WineD3D
  for DX7 titles), Win98 guest (the same DLLs are 9x-compatible if built
  msvcrt / no-CRT like wine9x), proper PnP driver instead of MAPMEM.

## Reference workloads and conformance (what we test the device with)

- **Ours:** `D3DGAME9` / `D3DGAME8` (P0a) — small, deterministic,
  instrumented as we like, golden BMPs from the rig.
- **Wine's d3d8/d3d9 test suites** (`dlls/d3d9/tests/*.c`, LGPL): thousands
  of API and pixel-readback tests written to *pass on real Windows*; they
  build with mingw and run on XP. Run them on the rig for the pass list,
  then against the device: the conformance suite we don't have to write.
- **Irrlicht** (zlib) ships Direct3D 8 and 9 renderers with sample apps and
  real content (meshes, lightmaps, particles, shaders); builds with mingw,
  runs on XP. Good for "engine-shaped" traffic and easy to instrument.
- **Commercial titles** (doc 04 matrix: Max Payne, GTA:VC) stay the
  acceptance bar, last. The Quake/Duke ports are OpenGL and already
  covered by the qemu-3dfx pass-through; useful for the GL side only.

## Risks

- **MoltenVK feature gaps** for DXVK's d3d9 (P0 decides). Known gaps have
  shrunk each year; D3D9-era workloads need less than D3D11.
- **FIFO cost under TCG:** each MMIO doorbell is a TCG exit; batching per
  frame keeps it to a few per frame. qemu-3dfx's numbers (500+ fps wglgears
  on the Air) bound the transport.
- **Lock-heavy games** (per-frame dynamic vertex buffers): bulk pages plus
  DXVK's own upload path; measure in P2 with FIFA-style software-skinned
  titles.
- **Scope creep from DX7:** kept out; WineD3D covers it.
- **C++ in the QEMU tree** (DXVK): built as a separate static library with a
  C shim, linked into the embed library only.

## Where the FIFA 2000 investigation stopped (2026-09-03)

Parked, for the record (doc 00 has the timeline): with the wine9x
WineD3D + our two fixes the game runs a match through DirectDraw/D3D6 and
the GL pass-through; the pitch texture renders as noise bands (dynamic
surfaces mapped every frame through the PBO path, or a 16-bit/palettized
upload with the wrong stride — not resolved), the screen flickers (the
front-buffer present fires on every glFlush, not once per frame), and
DirectInput stops after the match's mode switch (foreground window). The
nine "program error: out of range indirect offset (+65)" lines on the host
are wined3d's own ARB offset-limit probe and are harmless.
