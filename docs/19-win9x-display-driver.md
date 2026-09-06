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
  w9x/      d3dpthal.c    the ring-3 HAL DLL: the DDHAL / D3DHAL callbacks,
                          thunked onto the same core (mingw, like NT's)
            d3dptmini.c   the 16-bit .drv: GDI over the DIB engine, modes,
                          palette, cursor, the DCICOMMAND escapes (Watcom)
            d3dptvxd.c    the mini-VDD: the adapter, VRAM, the DOS boxes (Watcom)
            d3dptvid.inf
  ddk/      vendored headers (per OS as needed)
```

Only `w9x/d3dpthal.c` links the core; the other two 9x binaries never see
it. And because the core is linked into a **kernel-mode** DLL on NT and a
**user-mode** one on 9x, it may call no operating-system service at all —
every one it needs (map VRAM, read a tick, allocate, write the debug
register) arrives through the per-OS layer. That constraint is what makes
the core portable, and it is close to true already: `d3dptdisp.c`'s core
three quarters calls almost nothing but the encoder.

**The refactor is a refactor.** It changes no behaviour on XP, and the
proof is that the M7 suite is byte-identical across it: `d3dpt-dp2-test`
and `d3d7test` against the same golden BMP, `shtest` / `cktest` /
`ebtest` / `dxttest` with the same case counts, `d3dgame8` still matching
the native oracle, Moto Racer and Vice City still drawing. Split first,
land it green, then start on 9x — never both at once, or a 9x bug and a
refactor bug are indistinguishable.

## What 9x does differently (step 0, answered 2026-09-06)

Answered by reading JHRobotics' **`vmdisp9x`** (the Win9x display
minidriver behind SoftGPU, itself derived from Michal Necasek's VirtualBox
minidriver, with Philip Kelley's `boxv9x` for QEMU's `-vga std`) and its
companion **`vmhal9x`** (the DirectDraw/Direct3D HAL). Both are cloned to
`build/ref/` (gitignored) — read, not vendored. Line references below are
to those trees at the commits cloned on 2026-09-06.

### 1. A 9x display driver is three binaries, not two

| | XP (what we have) | Win98/Me |
|---|---|---|
| GDI | `d3dptdisp.dll`, kernel, `Drv*` DDI, loaded by win32k | a **16-bit NE `.drv`** exporting the Win3.x-style DDI by ordinal (`BitBlt.1 … ValidateMode.700`), ring 3 |
| drawing | GDI draws into the engine bitmap = VRAM | every drawing export **jumps straight to the DIB Engine** (`DIBENG.DLL`); the driver keeps only `Enable`/`ReEnable`/`Disable`, `Control`, palette, cursor, `ValidateMode` |
| hardware | the video miniport `d3dptvid.sys` (`videoprt`) | a ring-0 **mini-VDD `.vxd`** (`system win_vxd dynamic`) that owns the adapter, maps VRAM, and arbitrates the DOS boxes |
| DirectDraw / D3D HAL | **in the kernel display driver**, called by `dxg.sys` | a **ring-3 32-bit DLL** loaded into the *game's* process by `ddraw.dll` |

The last row is the structural surprise and it is good news: on 9x the
part of the driver that carries our DP2 walker and surface table is an
ordinary user-mode Win32 DLL. The 16-bit `.drv` and the VxD never see the
core at all.

### 2. How the 32-bit HAL is published from a 16-bit driver

The 16-bit driver answers GDI's `Control` (escape) `DCICOMMAND` (0x0C03)
with four sub-commands (`vmdisp9x/control.c`, `dddrv.c`):

- `DDNEWCALLBACKFNS` hands the driver DirectDraw's `lpSetInfo`;
- `DDGET32BITDRIVERNAME` returns a `DD32BITDRIVERDATA` — **a DLL file
  name, an entry-point name (`"DriverInit"`) and a 32-bit context value**.
  DirectDraw loads that DLL into the calling process and calls the entry;
- `DDCREATEDRIVEROBJECT` builds `DDHALINFO` and calls `lpSetInfo`;
- `DDVERSIONINFO` reports `DD_RUNTIME_VERSION`.

The context value is the **linear address of a structure shared between
the two halves** (`VMDAHAL_t`: a far pointer for the 16-bit side, a flat
pointer for the DLL — 9x maps everything above 2 GiB into every process,
so one linear address is valid everywhere). `DriverInit` in the DLL fills
that structure's `cb32` table with its own 32-bit callbacks; the 16-bit
side then copies them into `DDHAL_DDCALLBACKS` / `DDHAL_DDSURFACECALLBACKS`
and sets the matching `DDHAL_CB32_*` flags. There is no registry entry
and no signing: whoever the `.drv` names is what gets loaded.

### 3. The DDI structures: identical calls, rearranged objects

This is the fact the whole split hinges on, and it is now checked rather
than hoped for.

**The per-call data structures are field-for-field identical.** NT's
`DD_CREATESURFACEDATA` and 9x's `DDHAL_CREATESURFACEDATA` have the same
six members in the same order; so does the one that matters most —
`D3DNTHAL_DRAWPRIMITIVES2DATA` and `D3DHAL_DRAWPRIMITIVES2DATA` are the
same fourteen fields in the same order, down to the
`lpDDVertex`/`lpVertices` union and `dwVertexSize`/`ddrval`. Only the
*names of the pointer types* differ (`PDD_SURFACE_LOCAL` against
`LPDDRAWI_DDRAWSURFACE_LCL`). Calling convention is `__stdcall` on both.

**The DirectDraw object structures are not.** NT's `DD_SURFACE_LOCAL` is
ten members; 9x's `DDRAWI_DDRAWSURFACE_LCL` is twenty-six, in a different
order, with `lpSurfMore` first instead of `lpGbl`. Every field our driver
reads exists on both sides under the same name — `lpGbl->fpVidMem`,
`lpGbl->lPitch`/`dwLinearSize`, `lpGbl->ddpfSurface`,
`lpSurfMore->dwSurfaceHandle`, `lpSurfMore->dwMipMapCount`, `ddsCaps`,
`dwFlags`, `ddckCKSrcBlt` — at different offsets, and
`lpGbl->wWidth`/`wHeight` are `WORD` on 9x against `DWORD` on NT. So the
neutral descriptor of "The split" is required, not a precaution, and
filling it is a mechanical per-OS accessor rather than a translation.

**The DP2 token stream is the same stream.** 9x's `d3dhal.h` and our
`d3dnthal.h` agree on every opcode value we handle (`TEXBLT` 38,
`CLIPPEDTRIANGLEFAN` 58, `DRAWPRIMITIVE2` 59, `BUFFERBLT` 64,
`SETVERTEXSHADERFUNC` 76 …), and `vmhal9x` sees the full DX8 set in a
real 98 guest — stream sources, vertex/index buffers, shader create/set,
palettes, state sets. The walker — the expensive three quarters of
`d3dptdisp.c` — is portable as it stands.

### 4. The DDI does reach DirectX 8 on 9x

`GetDriverInfo2` works exactly as on NT: same `GUID_GetDriverInfo2`
(aliased to `GUID_DDStereoMode`), same `D3DGDI2_MAGIC`, same
`D3DGDI2_TYPE_GETD3DCAPS8` / `GETFORMATCOUNT` / `GETFORMAT` / `DXVERSION`
sub-types, same `D3DCAPS8`. `vmhal9x` announces it by setting
`DDHALINFO_GETDRIVERINFO2` in the flags the 16-bit half passes on, and
its README claims "current DDI is 8, this means support up to DX9
programs and games". So the DX8 DDI (hardware T&L, the token rewrite,
vs/ps 1.x) is not XP-only and the whole of M7c is in scope for 98.

One difference to expect: 9x's runtime still offers the pre-DP2 HAL
entries (`RenderState`, `RenderPrimitive`, `DrawOnePrimitive`,
`TextureCreate`) that NT dropped, and `vmhal9x` implements them. Whether
a driver claiming DDI 8 can leave them out — as ours would — is the one
question of this section that only a guest can answer.

### 5. Caps rules are *not* the same as NT's

- `DDCAPS_GDI` is **normal on 9x** ("the hardware is shared with GDI")
  and `vmdisp9x` sets it. On NT it makes dxg drop the whole HAL (doc 15).
  Two OSes, opposite answers, same bit — a per-OS caps table, not a
  shared one.
- On 9x a HAL callback may return `DDHAL_DRIVER_NOTHANDLED` and
  DirectDraw's HEL takes over; on NT a declined `DdBlt` reaches the
  application as `E_NOTIMPL` (doc 15, "Blit caps and the HEL"). So 9x can
  claim `DDCAPS_BLT` cheaply — though `vmhal9x` implements blits in
  software anyway rather than leave them to the HEL.

### 6. Modes come from the registry, not from the adapter

On NT our miniport enumerates the host's mode table and `DrvGetModes`
reports it. On 9x the mode list is **written into the registry by the
INF** (`HKR,"MODES\<bpp>\<w>,<h>"`), the driver only validates and sets
them; `vmdisp9x` ships a `vesamode.exe` and a tray applet to rewrite the
list from the adapter afterwards. Consequence for us: either the INF
carries a superset of the host table, or we ship the equivalent of that
utility. M2's "the mode table comes from the player" needs an answer on
98 that it does not need on XP.

### 7. Installation

A plain `Class=DISPLAY`, `signature="$CHICAGO$"` INF matched on
`PCI\VEN_1234&DEV_3D00`, with `HKR,DEFAULT,drv,,<name>.drv`,
`HKR,DEFAULT,minivdd,,<name>.vxd`, `HKR,,DevLoader,,*vdd` and the mode
list. Selected in Device Manager or Display Settings; no `DRVINST.EXE`
equivalent needed.

### 8. Ring 3 reaches the hardware through the VxD

`vmhal9x` opens the VxD with `CreateFileA("\\\\.\\<name>.vxd")` and drives
it with `DeviceIoControl` (its `OP_FBHDA_*` ops), and reads VRAM through a
linear address the VxD published in the shared structure. For us that
gives two options for the doorbell, and the choice belongs to step 4:

- **map the register page into the process** (it is one 4 KiB page,
  `D3DPT_FB_REGS_SIZE`) and let the HAL write `DOORBELL` directly — no
  ring transition per batch, the same cost profile as XP; or
- **an ioctl per submission** through the VxD — slower, but the VxD can
  then serialise submissions.

Serialisation is the real question behind that choice, and it is new:
on NT the command window has exactly one writer because the HAL lives in
the kernel behind dxg. On 9x every process with a Direct3D device has its
own copy of the HAL and they would share one window at the top of VRAM.

### 9. Toolchain: two compilers, and a header-provenance question

- The 16-bit `.drv` and the ring-0 `.vxd` need **Open Watcom** (`wcc`,
  `wcc386`, `wasm`, `wlink`; `system windows dll` and `system win_vxd
  dynamic`), plus a small `fixlink`-style post-pass to fix the NE/VxD
  header flags wlink leaves wrong. mingw-w64 cannot produce either
  format. Open Watcom is not installed on this box — a new build
  prerequisite, and one `scripts/build.sh` must treat the way it already
  treats a missing mingw: skip the artefact and say which one is behind.
- The ring-3 HAL DLL — **the one that links our core** — builds with the
  `i686-w64-mingw32` toolchain we already use; `vmhal9x` does exactly
  that, freestanding with its own tiny CRT, which is also how our NT
  driver is built. mingw-w64 already ships `ddrawi.h`, `d3dhal.h` and
  `dmemmgr.h`, so the ring-3 side needs almost no vendored headers.
- The 16-bit side is the gap: `gdidefs.h`, `dibeng.h`, `minivdd.h`,
  `valmode.h` are Windows 98 DDK headers that `vmdisp9x` vendors. Our
  rule is "no Microsoft DDK" (doc 15), so their provenance has to be
  settled before we copy anything — as does linking the `.drv` without
  Watcom's `clibs.lib` (our NT driver already builds `-nostdlib` with a
  30-line `kcrt.c`, and the VxD link in `vmdisp9x` already uses
  `option nodefaultlibs`).
- `dibeng.lib` is not a Microsoft file: it is generated by `wlib` from a
  text import list, so the DIB Engine can be imported without a DDK.

### 10. What this means for the split

The core stays freestanding C with no operating-system calls at all —
because on NT it is linked into a kernel-mode DLL and on 9x into a
user-mode one, and nothing may assume either. Every OS service it needs
(map VRAM, read a tick, allocate, write the debug register) arrives
through the per-OS layer, and every DirectDraw object arrives as the
neutral descriptor. The 9x side is then three binaries of which only one
— the ring-3 HAL DLL — links the core.

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

### 11. The mini-VDD is not optional (measured 2026-09-06)

Step 0 read this as "a ring-0 VxD for the 32-bit work and DOS-box VGA
arbitration", i.e. something a first cut could leave out. It cannot. The
first `.drv`-only driver was built and run in a real Win98 guest, and the
adapter's PCI base addresses tell the story:

```
BARs  bios   BAR0: prefetchable memory at 0xf0000000  BAR1: memory at 0xfebf0000
BARs  30s    BAR0: (not mapped)                       BAR1: (not mapped)
```

SeaBIOS maps both BARs at POST; **Windows 98 unmaps them within the first
half-minute of boot** and never puts them back. Nothing claimed the
device's resources, so the Configuration Manager took them away — and a
16-bit driver that then reads the BARs out of PCI config space finds
zeros, fails, and GDI falls back to VGA (a 16-colour desktop, which is
how the run reports itself). Claiming those resources is the ring-0
half's job on 9x, which is exactly why `vmdisp9x` says it moved "most
calls to the 32-bit mini-VDD driver": the mini-VDD registers with the
main VDD (`VDD_REGISTER_DISPLAY_DRIVER_INFO`) and hands the 16-bit driver
a mapped frame buffer rather than letting it map anything itself.

So the 9x work has a fixed order that the XP work did not: **the VxD comes
before the display driver can do anything at all.** What the `.drv`-only
attempt did establish, and what carries over unchanged:

- Open Watcom builds a 16-bit NE display driver on Linux, and the binary
  checks out: module `DISPLAY`, the ordinal export table, imports from
  `KERNEL` and `DIBENG` only, no C runtime (§9's licence question
  answered — nothing OWPL-licensed enters the binary).
- **A display driver must carry `oembin` resources** (`config.bin`,
  `colortab.bin`, `fonts.bin`, `fonts120.bin`): the machine metrics, the
  Control Panel's colour table and the three system LOGFONTs live inside
  the `.drv`, and GDI needs them. They are small fixed structures from the
  Windows 3.1 DDK, written as C and linked to raw binaries.
- **The INF path works with no clicks.** With `d3dpt9x.inf` and the driver
  in `C:\WINDOWS\INF`, Win98's PnP matches `PCI\VEN_1234&DEV_3D00`,
  installs silently, asks to restart, and writes `drv=d3dpt9x.drv` into
  `Services\Class\Display\0000` — the registry is correct, `SYSTEM.INI`
  keeps `display.drv=pnpdrvr.drv` and puts the device description in
  `[boot.description]`, which is what a real driver's install looks like.
- **`SYSTEM.INI` is not a shortcut.** Setting `[boot] display.drv=` by
  hand and skipping the INF does not work: Windows rewrites the line when
  it re-detects the adapter.
- **Two debug channels, and the driver wants both.** The adapter's DEBUG
  register cannot say anything before the register page is mapped, which
  is where the interesting failures are, so every line also goes to port
  0xE9 (`-debugcon file:…`). In this run both were silent, which is
  consistent with the BARs being gone.

`tools/win98-driver-test.sh` is the harness: it stages the driver into a
raw copy of the image (never the user's qcow2), boots on the adapter, and
prints the BARs at three points, the screen's colour count and both debug
channels.
