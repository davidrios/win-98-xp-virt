# 19. A native Win98 display driver (ADR-012, M10)

XP has a real display driver for our `d3dpt-vga` adapter: a video
miniport, a display driver DLL, a DirectDraw DDI and a Direct3D DDI that
turns the runtime's DP2 token stream into `d3dpt` protocol records the
host executor runs on DXVK (doc 15). Win98 has none of that — it boots
`-vga cirrus` with the inbox driver and gets its 3D through the
qemu-3dfx Glide wrappers and the WineD3D DLLs in a game's folder.

This track gives Win98/Me the same driver, on the same adapter, over the
same protocol — and, because two drivers doing the same job in two source
trees is how both rot, it first **splits the XP driver into an
OS-independent core plus a thin per-OS layer** and rebuilds XP's driver
on that core before the 9x one is written.

Read doc 15 first: everything it says about the adapter, the register
set, the flip chain, the DP2 stream, palettized textures, colour keying,
execute buffers and the DX8 DDI is what the core *is*. This doc is only
about the split and about what 9x does differently.

## Why a native driver instead of the wrapper stack

- The Glide/WineD3D path needs per-game files in per-game folders. The
  driver needs nothing next to a game: XP's own `ddraw.dll` / `d3dim.dll`
  / `d3d8.dll` drive it, and the same is true of 98's.
- It is the only way a 9x title gets an accelerated *desktop* as well —
  modes from our table, page flips that are register writes, no copy
  inside QEMU (doc 15, "Shape").
- The DirectX 3–7 titles that matter on 98 are exactly the ones M7
  already made work on XP through the HAL (execute buffers, colour keys,
  palettized textures, 8 bpp modes). That work is spent; on 9x it should
  cost the per-OS layer and nothing else.
- Everything shipped by the M4 track (the paravirtual device, the guest
  DLLs) stays as it is. This does not replace it, and the DLL path
  remains the fallback wherever the driver is not installed.

## The split

Today the driver is two C files against the NT DDI: `d3dptvid.c` (the
video miniport, 555 lines) and `d3dptdisp.c` (3 708 lines) — the GDI
`Drv*` entry points, the DirectDraw callbacks dxg asks for, the Direct3D
DDI, the surface table, the caps tables and the DP2 walker, all in one
translation unit. Roughly three quarters of it never mentions a fact
about NT.

**OS-independent (the core).** Everything that is about *our* adapter and
*our* protocol:

- the DP2 walker — tokens to `D3DPT_DP2_*` records, stream bindings, the
  DX8 rewrite, `TEXBLT`, `BUFFERBLT`, the vs/ps 1.x validation, the body
  sizing table (`walk*`, ~800 lines, the single most expensive piece);
- the surface table: handles, mip levels, formats and their row/pitch
  arithmetic (including DXT), VRAM offsets, registration, dirty ranges,
  the colour-key and palette bookkeeping;
- the caps: `DDCAPS`, the pixel-format lists, `D3DCAPS7`, `D3DCAPS8`,
  the `ddflags` bisection knobs;
- contexts, render targets, `Clear2`, readback, scene capture;
- the flip chain: the offset register, the frame counter, the wait, the
  timeout that keeps a stalled refresh from hanging a game;
- the heap layout (primary, DirectDraw heap, cursor, command window) and
  the encoder in `d3dpt_enc.h`, the doorbell, the debug log through the
  DEBUG register.

**Per-OS (the layer).** Everything that is about the operating system:

- entry points and packaging: NT is a `win32k`-loaded kernel DLL plus a
  `videoprt` miniport; 9x is a 16-bit display driver with a ring-0 VxD
  beside it (see "What 9x does differently");
- kernel services: memory, VRAM and register mapping, the IOCTL to the
  miniport, a monotonic tick for the flip timeout;
- the DirectDraw/Direct3D structures the OS hands us — NT's
  `DD_SURFACE_LOCAL` / `D3DNTHAL_*` against 9x's `DDRAWI_DDRAWSURFACE_LCL`
  / `D3DHAL_*`. **The core must never see either.** The layer fills a
  neutral descriptor (`d3dpt_surf_desc`: memory, pitch, width, height,
  pixel format, caps, colour key, next mip level) and the core works on
  that. If the two layouts turn out to agree field for field on
  everything we read — they are related structures, NT's were derived
  from 9x's — the accessors collapse to inlines and nothing is lost;
  if they do not, one struct changing is one file changing.
- GDI: NT's `Drv*` DDI against 9x's `.drv` DDI over the DIB engine.

Proposed layout (names to settle when the first file moves):

```
guest-tools/src/d3dptvid/
  core/     d3dpt_core.h  the core's types and the hooks it calls back into
            core_dp2.c    the DP2 walker
            core_surf.c   surfaces, formats, registration, keys, palettes
            core_caps.c   the caps and format tables
            core_ctx.c    contexts, targets, clear, readback
            core_flip.c   the flip chain and the vertical blank
  nt/       d3dptdisp.c   Drv* + the dxg callbacks, thunked onto the core
            d3dptvid.c    the video miniport
            d3dptdisp.def, d3dptvid.inf
  w9x/      (the 16-bit driver, the VxD, the DDHAL/D3DHAL thunks)
  ddk/      vendored headers (per OS as needed)
```

**The refactor is a refactor.** It changes no behaviour on XP, and the
proof is that the M7 suite is byte-identical across it: `d3dpt-dp2-test`
and `d3d7test` against the same golden BMP, `shtest` / `cktest` /
`ebtest` / `dxttest` with the same case counts, `d3dgame8` still matching
the native oracle, Moto Racer and Vice City still drawing. Split first,
land it green, then start on 9x — never both at once, or a 9x bug and a
refactor bug are indistinguishable.

## What 9x does differently (to be established, step 0)

This is the part nobody in this repository has written before, so the
first thing the track does is establish it against a working reference
rather than from memory. The reference is **JHRobotics' `vmdisp9x`** (the
Win9x display minidriver behind SoftGPU, which already drives QEMU/VMware
adapters and carries a DirectDraw HAL) — we do not vendor it; we read it
to answer:

- **The driver's shape.** A 16-bit display driver (`.drv`) that leaves
  the drawing to the DIB engine and owns mode setting and the hardware,
  plus a ring-0 VxD for the 32-bit work and DOS-box VGA arbitration.
  What exactly must be 16-bit, what may be flat, and how the two halves
  call each other.
- **How the DirectDraw HAL is published** from a driver whose GDI half is
  16-bit, and where the 32-bit callbacks live.
- **Whether the DDHAL / D3DHAL structures match NT's** for the fields the
  core reads (this decides how thin the 9x layer is).
- **How far the DDI goes on 9x.** DirectX 9.0c installs on 98; era
  drivers shipped DX8-level DDIs. If `GetDriverInfo2` and the DX8 token
  rewrite work the same, the whole DX8 DDI comes along for free; if 9x
  tops out at the DX7 HAL, the DX3–7 half is still the 98 title matrix.
- **Installation.** An INF the same way, and the display adapter is
  chosen in Device Manager rather than by `DRVINST.EXE`.
- **The 9x traps we already know of:** Win98 must be an ACPI install for
  PCI hot-adds to be seen, Win98 runs under TCG here (KVM loses Explorer),
  and a scripted run ends with a Start-menu shutdown (CLAUDE.md).

## What else moves when this lands

- `-vga cirrus` stops being the Win98 answer: the launcher's Win98
  reference machine gets `-vga none -device d3dpt-vga`, and CLAUDE.md's
  "Win98 stays on `-vga cirrus`" line goes.
- `SETUP.EXE` grows a display-driver component for the 98/Me role. Today
  `tools/setup-guest-test.sh win98` *fails the run if that component is
  even offered*; that check inverts, and the Win98 boot in it moves onto
  the adapter.
- The Win98 acceptance titles (doc 04) get a second path to be measured
  on: the driver against the Glide/WineD3D stack, same image, same host.
