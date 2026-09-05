# 15. A real XP display driver (ADR-008, M7)

The long-term guest side of doc 14: instead of DLL copies per game
folder, a Windows display driver pair that owns the adapter, and later
speaks the DirectDraw and Direct3D DDIs into the same transport and
executor. Staged: **M7a framebuffer (landed 2026-09-04) → M7b DirectDraw
DDI (first cut landed the same day: VRAM surfaces, page flips, cached
mappings) → M7c Direct3D DDI (first cut 2026-09-04: the DX7 non-T&L HAL
on the doc 14 executor, D3D7TEST's frame identical to the host-side
test's)**. ADR-008 (doc 10) has the why; this doc is the how and the state.

## Shape (M7a)

```
guest (XP)                                          host (QEMU process)
 win32k.sys (GDI)                                    hw/d3dpt/d3dpt_vga.c  "d3dpt-vga"
   └ d3dptdisp.dll  display driver (winddi.h)          ├ PCI 1234:3d00, class VGA, stdvga ROM
       EngCreateBitmap over the mapped VRAM            ├ BAR 0 VRAM 128 MiB ─── DisplaySurface points here
                                                       │   (top 64 MiB: the M7c command window)
       GDI draws every pixel itself                    ├ BAR 1 registers (d3dpt/d3dpt_fb.h)
 videoprt.sys                                          │   mode table, WIDTH/HEIGHT/BPP/PITCH/OFFSET,
   └ d3dptvid.sys   video miniport (video.h)           │   ENABLE, FRAMES, DEBUG (char → QEMU log)
       finds the PCI device, maps BAR 1,               └ VGA core (hw/display/vga.c) while ENABLE = 0:
       reads the host's mode table, sets modes            BIOS text, vga.sys before install, BSODs
 vga.sys / VGA BIOS until the driver is installed    embed listener: same surface pointer → player
```

- **The adapter is a real VGA.** QEMU's standard VGA core with the Bochs
  VBE ports, so SeaBIOS' `vgabios-stdvga.bin` boots it (the ROM's default
  path takes BAR 0 as the LFB for any vendor id), XP's inbox `vga.sys`
  runs the desktop at 640×480 or 800×600 4 bpp before our driver exists,
  and the blue screen / shutdown text still work because `HwResetHw`
  returns FALSE and videoprt's int10 puts the core back into mode 3.
  Class code 03.00, vendor/device 1234:3d00 (the QEMU/Bochs pseudo vendor,
  our id), matched by the INF as `PCI\VEN_1234&DEV_3D00`.
- **The register BAR is the paravirtual part** (`d3dpt/d3dpt_fb.h`, one
  header for the QEMU device and the miniport). The host owns the **mode
  table**: the miniport reads MODE_COUNT and walks MODE_SEL → MODE_W/H/
  BPP/HZ, so what XP's Display Properties offers is decided in the player
  (M2's mode table and pixel aspect plug in here; today a static list of
  7 sizes × {60, 75, 85} Hz × {16, 32} bpp = 42 modes). A mode switch
  writes WIDTH/HEIGHT/BPP/PITCH/OFFSET and ENABLE = 1; ENABLE = 0 hands
  the console back to the VGA core. DEBUG takes one character per write
  and the device prints lines to the QEMU log (`d3dpt-vga: guest: …`):
  the only "debugger" we have for kernel code, and enough so far.
- **No copy inside QEMU.** While ENABLE is set the device's
  `DisplaySurface` is created *over the VRAM bytes* at the guest's
  offset/pitch (`qemu_create_displaysurface_from`), dirty lines come from
  the memory dirty log (`DIRTY_MEMORY_VGA`, the same mechanism
  `bochs-display` uses), and the embed listener hands that pointer plus
  the dirty rectangles to the player. 16 bpp modes are converted per dirty
  span into an x8r8g8b8 shadow (the embed listener takes one format);
  32 bpp is the untouched framebuffer. The player's upload of the dirty
  rectangle into its texture is the one copy left (zero-copy to the GPU
  from guest RAM is not possible with a host-visible-only import; M3's
  dma-buf ring is for 3D frames the host renders).
- **The display driver is the DDK "framebuf" shape.** `DrvEnablePDEV`
  picks the miniport mode matching the DEVMODE, fills GDIINFO/DEVINFO for
  16/32 bpp bitfield surfaces, `DrvEnableSurface` sets the mode, maps
  VRAM (`IOCTL_VIDEO_MAP_VIDEO_MEMORY`) and hands GDI an engine bitmap over
  it with no hooks: GDI draws every pixel in software, straight into
  guest VRAM, and simulates the pointer. `DrvAssertMode(FALSE)` resets to
  VGA for full-screen consoles and the logon desktop switch. The register
  page reaches the display driver through
  `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` (debug now; M7b/c records later).
- **Install** is `DRIVER\DRVINST.EXE [-reboot]` on the guest-tools ISO:
  newdev's `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE` for
  the hardware id (what `devcon update` does), after setting the
  driver-signing policy to ignore. No DDK, no devcon. Or Device Manager →
  Video Controller (VGA Compatible) → Update Driver → the folder.
  `SETMODE.EXE [w h bpp [hz]]` lists / switches modes for scripts.

## Building kernel-mode PE files with GCC

`guest-tools/build-driver.sh` (also run by `build-wrappers.sh`, which
stages the result as `DRIVER\` on the ISO). mingw-w64 ships the DDK headers
and import libraries under its permissive licence; nothing from Microsoft's
DDK is used. What it took:

- `-nostdlib -shared -ffreestanding -fno-stack-protector
  -mno-stack-arg-probe -fno-asynchronous-unwind-tables`, subsystem native,
  image base 0x10000, OS/subsystem version 5.1, entry `_DriverEntry@8`
  (miniport) / `_DrvEnableDriver@12` (display DLL; exported undecorated
  through a .def with `--kill-at`). `-lgcc` for helpers.
- The miniport links **only** `libvideoprt.a`, the display DLL **only**
  `libwin32k.a`; the build script fails if any other import appears.
- GCC emits `memcpy`/`memset` calls for struct copies even freestanding
  and neither port driver exports them: `kcrt.c` carries byte loops,
  compiled with `-fno-tree-loop-distribute-patterns` so they are not
  turned back into calls to themselves.
- Header sets are not mixable: the miniport takes `ntdef.h`, `dderror.h`,
  `devioctl.h`, `ddk/miniport.h`, `ntddvdeo.h`, `ddk/video.h` (**not**
  `ntddk.h`, which conflicts with `miniport.h`; with `_WIN32_WINNT=0x0501`
  mingw's `wdm.h` does not even compile, so leave the default). The display
  DLL takes `windef.h`, `wingdi.h`, `winddi.h`, `devioctl.h`, `ntddvdeo.h`;
  mingw 14's `winddi.h` includes `ddrawint.h`/`d3dnthal.h` which mingw does
  not ship, so `guest-tools/src/d3dptvid/ddk/` vendors ReactOS' public-domain
  `ddrawint.h` (+ `dvp.h`) and a **self-contained `d3dnthal.h`**: the DDK's
  version includes `d3dtypes.h`/`d3dcaps.h` and through them the user-mode
  `windows.h`, so the few Direct3D types the DDI structures use (caps
  structs, D3DRECT, the DP2 command header) are spelled out in it with the
  DDK's layouts. The DP2 token layouts themselves only the host interprets
  (`d3dpt/exec/d3dpt_exec_ddi.cpp`).
- The same `-march=pentium3` floor and ISA/UCRT checks as the wrappers.

## State (2026-09-04, Linux host, XP SP3 guest)

- `-vga none -device d3dpt-vga`: SeaBIOS text, XP boots to the desktop on
  `vga.sys` (800×600×4, Found New Hardware wizard for the unknown VGA).
- `DRVINST.EXE`: the INF matches, XP's Logo-test dialog appears once
  (unsigned driver; "Continue anyway"; the installer sets the policy
  values but SP3 still asked — see below), "installed", the miniport is
  loaded immediately (`adapter found`, 42 modes in the QEMU log). After a
  reboot the desktop is ours: XP picks 640×480×32@60 first (no saved
  mode), `SETMODE 1024 768 32 85` switches (`ChangeDisplaySettings = 0`),
  800×600×16@75 too (the 16 bpp shadow path), `SETMODE` lists 42 modes
  plus XP's two 4 bpp VgaSave entries. Every switch is `reset device` →
  `set mode N` → `linear mode on (WxHxBPP pitch P …)` in the log.
  Clean power-down (QMP `system_powerdown`) and the desktop come back at
  the saved mode.
- Player (Linux, RADV): the same image boots in `target/release/player`
  with `-vga none -device d3dpt-vga`; the log shows the listener following
  the driver (`[display] switch 1024x768 stride 4096` right after
  `linear mode on`), `shutdown -r` writes ENABLE = 0 (`linear mode off`)
  before the guest reset so the BIOS text and the XP boot logo show on the
  VGA core, the second boot comes back at the saved 1024×768×32@85, and
  QMP `system_powerdown` ends the run with the player exiting cleanly.

- **KVM** (2026-09-04, the user's run and mine): `-accel kvm -cpu host`
  with the same device and driver works and is far faster than TCG; the
  x87 patches are TCG-only and irrelevant there. Linux hosts should run XP
  under KVM; TCG stays the Apple Silicon path.
- **Mode-switch flash fixed** (2026-09-04): a switch is RESET → SET_MODE a
  few ms apart, and the VGA core was shown in between (text mode / stale
  VGA memory in the player), then the new mode came up with the old
  desktop bytes at the new pitch. Now the device holds the last linear
  frame for 15 refreshes after ENABLE goes 0 before handing the console to
  the VGA core (a real return to VGA is delayed by ~250 ms, nothing else),
  and the miniport zeroes the new mode's frame buffer unless
  `VIDEO_MODE_NO_ZERO_MEMORY` (it maps all of VRAM in kernel space for
  that). Player log of a switch is now one `[display] switch` line.

## M7b — the DirectDraw DDI (2026-09-04)

What Microsoft's `ddraw.dll` → `dxg.sys` sees behind the display driver
(`d3dptdisp.c`, bottom half; the DDI header is ReactOS' public-domain
`ddrawint.h`, vendored in `guest-tools/src/d3dptvid/ddk/`):

- **The primary is a device surface GDI still draws on.** `EngCreateDeviceSurface`
  + `EngModifySurface(hsurf, hdev, HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY,
  dhsurf, pvScan0 = VRAM, pitch)` with a no-op `DrvSynchronizeSurface`.
  `HOOK_SYNCHRONIZE` is not optional: without it win32k refuses the
  `EngModifySurface` (the driver tries the variants in order and logs the
  one that took; the M7a engine bitmap is the fallback, desktop only).
- **VRAM behind the primary is one linear heap** (`DrvGetDirectDrawInfo`:
  `VIDMEM_ISLINEAR`, `fpStart` = primary size rounded to 4 KiB, `fpEnd` =
  end of BAR 0, offsets relative to the frame buffer). dxg's heap manager
  places every DirectDraw surface in it; the driver never allocates.
- **The caps dxg accepts:** `dwCaps = 0` (no blit caps: DirectDraw's HEL
  blits on the mapped VRAM, which is RAM here), `dwCaps2 = WIDESURFACES`,
  `ddsCaps = PRIMARYSURFACE | OFFSCREENPLAIN | FLIP | FRONTBUFFER | BACKBUFFER`,
  `DDHALINFO_GETDRIVERINFOSET` + a `GetDriverInfo` that answers
  `GUID_NTCallbacks` (SetExclusiveMode, FlipToGDISurface) and refuses the
  rest. **`DDCAPS_GDI` in `dwCaps` makes dxg drop the HAL** (enable →
  immediate disable, `DDCAPS_NOHARDWARE`, system-memory surfaces); found
  by bisection with the `ddflags` knob below, kept as `DDF_GDI_CAP` for
  the record.
- **Callbacks:** `DdMapMemory` (VRAM into the game's process through the
  miniport's `IOCTL_VIDEO_SHARE_VIDEO_MEMORY`), `DdCanCreateSurface`
  (display format only), `DdFlip` = one write of the target surface's
  VRAM offset into the device's OFFSET register (a real page flip: the
  console surface moves to the other buffer, nothing is copied),
  `DdWaitForVerticalBlank` = wait for the device's FRAMES counter to
  move (bounded at 50 ms), `DdGetBltStatus` = always done,
  `DdGetFlipStatus` = the vertical blank below. Not hooked: Lock, Unlock,
  Blt, CreateSurface, DestroySurface.
- **Mappings are cached, not write-combined.** videoprt's
  `VideoPortMapMemory` maps frame buffers uncached or write-combined
  (right for a PCI aperture, wrong for RAM): reads from such a mapping run
  at tens of MB/s, and GDI scrolls, DirectDraw's HEL copies and every
  `Lock` read do exactly that. The miniport therefore maps VRAM itself:
  `MmMapIoSpace(MmCached)` for the kernel view GDI draws through, and a
  cached view of `\Device\PhysicalMemory` (`ZwMapViewOfSection`, what
  videoprt does inside minus `PAGE_NOCACHE`) for DirectDraw's user-mode
  `Lock`. Those are the miniport's only ntoskrnl imports; the register BAR
  stays a videoprt (uncached) mapping. QEMU reads guest RAM coherently, so
  a cached guest mapping is correct under KVM and TCG alike.
- **`-device d3dpt-vga,ddflags=N`** (register `DDFLAGS`, read by the
  display driver) switches behaviours without a driver reinstall:
  1 = no GetDriverInfo, 4 = no surface callbacks, 8 = engine-bitmap
  primary, 0x10 = add `DDCAPS_GDI`, 0x8000 = no vertical blank (flips
  complete instantly, the M7b behaviour: throughput runs). That is how the caps were bisected
  in one afternoon: one boot per variant, `DDTEST` and the QEMU log tell.
- **Test:** `DRIVER\DDTEST.EXE [w h bpp] [frames] [-windowed]` (guest-tools
  ISO): caps, exclusive flip chain (Lock/Unlock pattern + `Blt` colour fill
  + `Flip`) or a windowed offscreen surface blitted to the primary, fps,
  `ddtest.log` + `ddtest.bmp`. `tools/xp-driver-test.sh <image> ddtest`
  runs the set headless and pulls the logs out. Results 2026-09-04 (KVM,
  RADV host):

  | case | before (write-combined) | cached mappings |
  |---|---|---|
  | 640×480×16 exclusive, 300 flips | 3846 fps | 4762 fps |
  | 640×480×32 exclusive, 300 flips | 6383 fps | 6383 fps |
  | 640×480×32 windowed, offscreen VRAM → primary Blt (HEL) | 29.9 fps | 305 fps |

  Those are throughput numbers, from before the flip chain had a vertical
  blank: since 2026-09-05 a flip chain runs at the mode's refresh rate and
  `DDTEST` reports 60 fps for all three exclusive cases. Add
  `DDFLAGS=32768` to measure throughput again (the numbers above come back
  to the frame).

  Every surface reports `VIDEOMEMORY | LOCALVIDMEM`, the QEMU log shows
  `scanout offset 0 -> 614400 -> 0 …` per flip, the picture (checkerboard
  + moving bar) is right in the screendump. The uncached numbers before
  write-combining were 31 fps for the 16 bpp chain.

## Open items

- The Logo dialog: none of the registry policy values silence it on XP
  SP3 (the stored policy is hash-protected; only the Control Panel can
  change it). `DRVINST.EXE` therefore runs a watcher thread that finds
  the dialog (created by setupapi inside DRVINST's own process) and presses
  its first push button, "Continue Anyway". `alt+c` is the manual fallback.
- The two `640×480×4 / 800×600×4 @ 1 Hz` entries in the mode list are
  vga.sys' (VgaSave stays registered as the VGA-compatible device); harmless,
  a `VgaCompatible = 1` INF variant would hide them but also makes our
  driver the one bootvid uses. Leave.
- No `DrvSetPointerShape`: GDI's software pointer draws into VRAM, so the
  cursor is part of the framebuffer (the player hides its own cursor over
  the image, as with cirrus). A hardware cursor needs the player to
  composite a sprite (it ignores QEMU's `on_cursor` today) plus define /
  move registers here; deferred, the software pointer is correct and the
  dirty rectangles it causes are small.
- `FRAMES` is a clock, not the player's present: it counts periods of the
  mode's `HZ` since `ENABLE`. A game therefore gets the refresh rate it
  asked for whatever the player is doing, but the two are not in phase —
  a present signal from the player's own swapchain is the follow-up, and
  the only way to make the guest's frames and the host's line up exactly.
- DirectDraw: `DdBlt` is not hooked, so blits are the HEL's user-mode
  copies on cached VRAM (fast enough for 2D); a host-side blit through
  the executor only makes sense once surfaces can live on the host GPU
  (M7c). Overlays, FourCC and different-format surfaces are refused
  (8 bpp palettized modes: see the section below). Not yet exercised:
  `GetDC` on a DirectDraw surface (`DrvDeriveSurface` absent, DirectDraw
  falls back).
- Mode table from the player (M2): a `modes=` device property or an embed
  API call that replaces the static table, pixel-aspect flags per entry.
- Multi-monitor, DPMS/power states, hot-unplug: not planned.
- Win2000 untested (same DDI version; should work).

## The flip chain's vertical blank (2026-09-05)

Moto Racer 1997 on the driver played at several times its intended speed.
It is not a timing bug in the guest: the game paces itself by its flip
chain, as nearly every title of the era does, and our chain had no pace.
`DdFlip` wrote the OFFSET register and returned, `DdGetFlipStatus` said
"done" without looking, so `Flip` never blocked and `DDTEST`'s 640×480×16
chain ran at 4762 fps. On a real card the second `Flip` of a
double-buffered chain cannot be set up until the first has been scanned
out, and that wait — not a timer in the game — is what holds a 1997 racer
at 60 frames a second.

Two halves:

- **The device (`d3dpt_vga.c`, `FRAMES`).** The counter used to be
  incremented by `gfx_update`, i.e. by whoever was pulling frames: the
  player at `PLAYER_REFRESH_MS` (16 ms), a headless run never (there is no
  display client, so `graphic_hw_update` is never called — a game under
  `xp-game-test.sh` would have been capped at the 50 ms bail-out, 20 fps).
  It is now derived from the host clock: periods of the mode's `HZ` since
  `ENABLE`, 60 Hz if the guest left `HZ` unset. Same register, same
  contract (a monotonic counter ticking at the display's rate), so no
  `D3DPT_FB_VERSION` bump and an older driver still works against it.
- **The driver (`d3dptdisp.c`).** `DdFlip` remembers `FRAMES` at the flip;
  until it moves the flip is in the air. A second `DdFlip` in that window
  waits (`DDFLIP_WAIT`) or returns `DDERR_WASSTILLDRAWING`, and
  `DdGetFlipStatus` answers `DDERR_WASSTILLDRAWING` for both `DDGFS_CANFLIP`
  and `DDGFS_ISFLIPDONE` — the runtime spins there for `DDFLIP_WAIT`.
  Bounded at 50 ms like `wait_frame`, so a device that stops counting
  cannot stop the guest with it. `wait_frame` now pauses between two polls:
  the register read and XP's performance counter are both exits, and a
  16 ms wait used to cost tens of thousands of them.

Measured (KVM, RADV host, `tools/xp-driver-test.sh <image> ddtest`):

| case | before | with the vertical blank |
|---|---|---|
| 640×480×8 exclusive flip chain (palette every frame) | 1132 fps | 60.0 fps |
| 640×480×16 exclusive flip chain | 3846 fps | 60.0 fps |
| 640×480×32 exclusive flip chain | 4762 fps | 60.4 fps |
| 640×480×32 windowed, offscreen → primary `Blt` (HEL) | 346 fps | 346 fps |
| `D3D7TEST` 640×480×32, 300 frames | 2400–2700 fps | 60.2 fps, frame still byte-identical to `d3dpt-dp2-test` |

The windowed case is unchanged on purpose: a blit to the primary is not a
flip and a real card does not throttle it either — a game that presents
that way and never calls `WaitForVerticalBlank` runs as fast as the CPU
allows on real hardware too. `DDFLAGS=32768` (`DDF_NO_VSYNC`) restores the
old behaviour to the frame, which is both the A/B for a suspect title and
how the throughput numbers above are still measured.

The device prints the guest's real frame rate every 5 s while flips are
happening (`d3dpt-vga: 299 page flips in 5.0 s (59.6/s)`), driven by the
flips themselves so a headless run reports the same as the player. **No
line at all while a game runs means it blits to the primary instead of
flipping** — the vertical blank cannot pace that game, and if it is too
fast the cause is the guest CPU, not the display path.

## When a title falls back to its software renderer

Moto Racer draws through its software rasterizer on the driver while FIFA
2000 gets the HAL. The two differ by two years: FIFA's 1999 DX6 renderer
asks for RGB textures and alpha blending, which the HAL offers; a 1997
title asks for what a Voodoo of the day had. Two gaps stand out in
`d3d_caps_init`:

- **No palettized textures.** `d3d_texformats` offers 32/16-bit RGB and
  DXT1/3/5 — no `DDPF_PALETTEINDEXED8`. 8-bit palettized textures with a
  palette per texture are how nearly every 1997 3D title stores its art
  (it is what fits in 4 MB of texture memory), and `DdCanCreateSurface`
  refuses the format outright, so the game cannot make its textures and
  drops to software.
- **No colour keying.** `dpcTriCaps.dwTextureCaps` has no
  `D3DPTEXTURECAPS_TRANSPARENCY`, and no surface-level colour key reaches
  the executor. 1997 titles cut out sprites and foliage with a colour key
  rather than an alpha channel; a game that requires it will not take a
  HAL that does not claim it.

Both landed the same night (the next section, protocol v8): the HAL offers
palettized textures and colour keying now. Whether Moto Racer takes the
HAL with them is the outstanding check (the user's box). The diagnostic
stays: `DdCanCreateSurface` prints the first eight formats it refuses —

    d3dpt-vga: guest: d3dptdisp: refused pixel format, flags 0x00000020 fourcc 0x00000000 bits 0x00000008 …

`flags` bit 5 (`DDPF_PALETTEINDEXED8`) with 8 bits is the palettized-texture
case. Read it together with the context line: `d3dptdisp: d3d context 1 …`
means the game did take the HAL, and no context line at all means it never
got that far. Neither has been seen on a Moto Racer run yet — that log is
the next thing to collect.

## Palettized textures and colour keying (2026-09-05, protocol v8)

Both gaps above are closed. What a 1997 title now gets from the HAL:

- **The caps.** The DX7 texture format list has a tenth entry,
  `DDPF_RGB | DDPF_PALETTEINDEXED8` at 8 bits, and the DX8 list
  `D3DFMT_P8`; `dpcTriCaps.dwTextureCaps` carries
  `D3DPTEXTURECAPS_TRANSPARENCY` and `ALPHAPALETTE` (masked out of
  `D3DCAPS8.TextureCaps`, where bit 3 means nothing); the DirectDraw caps
  carry `DDCAPS_COLORKEY` with `dwCKeyCaps = DDCKEYCAPS_SRCBLT`, and the
  surface callbacks a `SetColorKey` **and a `Blt`** — the four
  CKTEST runs that settled the shape (2026-09-05):
  1. caps + `SetColorKey` callback, no `Blt`: dxg drops the whole HAL
     (`GetCaps` answers `DDCAPS_NOHARDWARE`, no Direct3D device — the same
     post-enable validation as `DDCAPS_GDI` and palette caps;
     `ddflags=0x20000` is the repro);
  2. no caps: the HAL stays and `SetColorKey(DDCKEY_SRCBLT)` succeeds,
     but user-mode ddraw keeps the key to itself — dxg's `DD_SURFACE_LOCAL`
     shows no `DDRAWISURF_HASCKEYSRCBLT`, `DdSetColorKey` is never called;
  3. caps + `DDCAPS_BLT` + a `DdBlt` that declines: the HAL stays, dxg
     calls `DdSetColorKey` *and* records the key in the surface, and
     DDTEST's chains and windowed blits are unchanged with `DdBlt` never
     called;
  4. caps + the `DdBlt` callback **without** `DDCAPS_BLT`: the same, and
     nothing can ever be routed to `DdBlt` — this is the driver's shape.
  So dxg's rule is "colour-key caps need a Blt callback"; the HEL keeps
  doing every blit, keyed ones included. `ddflags=0x10000`
  (`DDF_NO_CKEY`) withdraws the D3D caps, the DirectDraw caps, the P8
  format and the key check together.
- **Two ways the key arrives**, both kept: `DdSetColorKey` sends
  `D3DPT_OP_VRAM_COLORKEY` (handle, low, high, on/off) as the app sets
  it, and the DP2 walk, on every `TEXTURESTAGESTATE` that binds a
  texture (state 0, pass 1), reads the surface's `DDRAWISURF_HASCKEYSRCBLT`
  / `ddckCKSrcBlt` off the `DD_SURFACE_LOCAL` the surface table remembers
  (valid until `DestroySurface`) and sends the record when it differs
  from what the host was told — as the DDK's sample drivers read it —
  which covers a key set before the surface was mirrored. The record goes
  into the batch ahead of the DP2 record, so the host has the key before
  the draw.
- **A flip does not move memory — found on the way.** The first case
  passed and the second read back black: CKTEST's per-case frame dumps
  and a flip / lock trace showed dxg's flip model on NT. `DdFlip` gets
  `lpSurfCurr` (front) and `lpSurfTarg` (back); the driver scans out
  `Targ`'s memory. Afterwards dxg does *not* exchange the two objects'
  `fpVidMem`: the handles keep their VRAM and the *roles* move — the
  `DDSCAPS_PRIMARYSURFACE` bit goes to the object now displayed, the
  application's `back` pointer means the other object from then on, and
  dxg tells the driver with a `CreateSurfaceEx` pair (same offsets,
  swapped caps). The M7c first cut re-registered `Targ` at `Curr`'s
  offset and vice versa in `DdFlip`, so from the second frame of every
  flip chain the host rendered and read back into the *displayed*
  buffer on alternate frames; the runtime's `SETRENDERTARGET` alternates
  the handle (2, 1, 2, 1 …) as the roles alternate, and with the stale
  offsets the app's `Lock` of its back buffer found nothing (black), or
  the previous frame. `D3D7TEST`'s golden compare never caught it (every
  frame identical, the last one read back before its flip), and a
  spinning scene looks the same either way. `DdFlip` re-registers
  nothing now.
- **Palettes never touch the driver.** A texture's palette reaches the
  host inside the DP2 stream: `SETPALETTE` (palette handle, flags with
  `DDRAWIPAL_ALPHA` 0x2000 when the entries' `peFlags` are alpha, surface
  handle) binds a palette to a surface and `UPDATEPALETTE` (palette
  handle, start, count, `PALETTEENTRY`s) fills it — the same two tokens
  DX7 and DX8 use (`SetPaletteEntries` / `SetCurrentTexturePalette` in
  DX8), and the executor answered "palettes are not supported" to them
  until now. `dwPalCaps` stays 0 and there are still no palette
  callbacks (the 8 bpp section: dxg drops the HAL otherwise).
- **The colour key goes by a record**, `D3DPT_OP_VRAM_COLORKEY` (handle,
  low, high, flags), sent by the texture-bind check above. The key values
  are the surface's own pixel values (0xf81f for magenta in R5G6B5, an
  index for P8), a range inclusive.
- **The host expands both to A8R8G8B8.** DXVK has no P8, and a key needs
  an alpha channel, so a P8 or keyed texture's host object is created in
  A8R8G8B8 (`host_format`) and `upload_texture` converts texel by texel:
  the palette's colour (alpha from `peFlags` when the palette is an alpha
  one, a grey ramp when no palette was set), the 16-bit formats decoded,
  alpha 0 for a texel whose raw value is inside the key range. A palette
  update marks every texture using it dirty; a key set or cleared
  releases the host object when its format changes (`refresh_object`).
  Since a bound texture can change under the runtime's nose (a palette
  edit, a `Lock` of a bound texture), every draw first re-uploads the
  bound stages whose surface is dirty (`pre_draw`) — the runtime re-sends
  `TEXTUREMAP` only on a `SetTexture`.
- **Keying is alpha 0 plus the alpha test, with one override.** Render
  state 41 (`COLORKEYENABLE`) is a host state now. While it is on and the
  texture at stage 0 has a key, the alpha test is forced on
  (`GREATEREQUAL 1`) unless the app runs its own (`ALPHATESTENABLE`),
  and — the part that matters for 1997 titles — stage 0's alpha op is
  made `SELECTARG1 TEXTURE` when the app's alpha pipeline does not read
  the texture alpha: the DX7 runtime's `TEXTUREMAPBLEND` emulation sets
  `ALPHAOP = SELECTARG2 DIFFUSE` for any texture format without alpha,
  which is every keyed R5G6B5 / P8 texture, and an alpha test cannot see
  a key the alpha pipeline threw away. The app's alpha test states and
  stage-0 alpha op come back the moment the key stops applying (state 41
  off, another texture bound, the key removed); an app change to those
  states while overridden is recorded and the override re-applied. The
  cost of the override: a title that colour-keys *and* blends by the
  diffuse alpha in the same draw loses the fade (it blends by the
  texture's alpha, 255 everywhere but the key) — the DX7 SDK's own
  `TEXTUREMAPBLEND` semantics make that combination rare.
- Tests: `tools/d3dpt-dp2-test.cpp` — a P8 texture through a palette
  (four entries, four cells), `UPDATEPALETTE` changing an entry under a
  bound texture, a keyed R5G6B5 checker with state 41 on (the keyed cell
  is the clear colour) and off (blue again), the app's own alpha test
  winning over the key, the key removed, `UPDATEPALETTE` beyond 256
  entries refused, `SETPALETTE` on an unknown surface ignored.
  `DRIVER\CKTEST.EXE` (`guest-tools/src/d3dptvid/cktest.c`) does it on XP
  through the DX7 API: a `DDPF_PALETTEINDEXED8` texture with an
  `IDirectDrawPalette`, `SetEntries` turning an entry blue, a R5G6B5
  texture with `SetColorKey(DDCKEY_SRCBLT, magenta)` drawn with
  `COLORKEYENABLE` on and off, every draw read back from the back buffer;
  `tools/xp-driver-test.sh <image> cktest` runs it and greps `cktest.log`
  for "0 failed".

## 8 bpp palettized modes (2026-09-04, register set v3)

The late-90s 2D titles (Diablo, StarCraft, Age of Empires, Caesar 3) set
640×480×8 through DirectDraw and animate the palette; with only 16/32 bpp
modes in the table `SetDisplayMode` failed and each stopped at its "could
not set display mode" box. Now every size is also offered at 8 bpp (63
modes), across the four layers:

- **Device** (`d3dpt_fb.h` v3, `d3dpt_vga.c`): `BPP = 8`, a 256-entry
  `PALETTE` register block (x8r8g8b8 per entry, 0x400..0x7fc of the
  register page, `CAP_BPP8`). The scanout is a pixman `c8` view of VRAM
  through a `pixman_indexed_t` filled from the registers, converted into
  the x8r8g8b8 shadow per dirty span exactly like the 16 bpp path; a
  palette write marks the palette dirty and the next refresh repaints the
  whole frame (a palette change recolours every pixel). Each entry write
  is one MMIO exit: a full 256-entry animation costs ≈0.5 ms under KVM
  (DDTEST's 8 bpp chain with a `SetEntries` every frame runs at 1200 fps
  against 3800–6400 for the RGB chains) — fine for a game's 20–30 palette
  updates a second; a RAM-backed palette page is the optimisation if a
  title ever needs more.
- **Miniport**: 8 bpp mode entries carry `VIDEO_MODE_PALETTE_DRIVEN |
  VIDEO_MODE_MANAGED_PALETTE`, 8 bits per gun, no masks;
  `IOCTL_VIDEO_SET_COLOR_REGISTERS` writes the CLUT into the block.
- **Display driver, GDI:** `GCAPS_PALMANAGED | GCAPS_COLOR_DITHER`, a
  `PAL_INDEXED` 256-entry default palette (the 20 system colours at 0–9
  and 246–255, a 6×6×6 cube, 20 greys), `BMF_8BPP` surfaces, `ulNumColors
  = 20`, `ulNumPalReg = 256`, `HT_FORMAT_8BPP`, and `DrvSetPalette`
  (`PALOBJ_cGetColors` → the PALETTE registers directly, the IOCTL only
  while the register page is unmapped). XP's desktop at 800×600×8 comes up
  in the classic theme with the wallpaper dithered through the system
  palette, as on any palette-managed adapter.
- **Display driver, DirectDraw:** `ddpfDisplay = DDPF_RGB |
  DDPF_PALETTEINDEXED8`, 8 bits, no masks. Two runtime findings, both
  from the disassembly of the image's `dxg.sys` / `ddraw.dll`
  (`build/xp-driver-test/pal-*`, DDTEST at 8 bpp answering
  `DDERR_UNSUPPORTEDMODE` from `CreateSurface` until both were in):
  1. **`dwPalCaps` must stay 0 and there must be no palette callbacks.**
     dxg's post-enable validation (the function that also rejects
     `DDCAPS_GDI`, `DDCAPS_BANKSWITCHED`, `DDSCAPS_MODEX`…) fails the HAL
     outright when `ddCaps.dwPalCaps != 0`; the probes stop after
     `GUID_MotionCompCallbacks`, never reaching `GUID_GetHeapAlignment`.
     On NT the primary's palette is GDI's: `IDirectDrawPalette::SetEntries`
     on the primary's palette lands in `DrvSetPalette`, so the
     `DdCreatePalette` / `DdSetEntries` / `DdSetPalette` callbacks of the
     DDK header are dead on XP. `DDPCAPS_8BIT | DDPCAPS_ALLOW256` palettes
     work without any of it.
  2. **The HAL's Direct3D must not disappear between modes.** The first
     8 bpp cut hid Direct3D (`d3d_init` refused palettized PDEVs); dxg then
     completed every probe but `ddraw.dll`'s object rebuild after
     `SetDisplayMode` still failed (`GetCaps` afterwards showed
     `DDCAPS_NOHARDWARE`, `dwModeIndex = DDUNSUPPORTEDMODE`). With
     `-device d3dpt-vga,ddflags=0x20` (no Direct3D in any mode) the 8 bpp
     chain worked at once, so it is the change of shape across the mode
     switch that the runtime rejects, not 8 bpp. Direct3D is therefore
     offered at 8 bpp too; the runtime refuses `CreateDevice` on a
     palettized primary by itself.
- **Test:** `DDTEST 640 480 8 300` creates a `DDPCAPS_8BIT | ALLOW256`
  palette of four ramps on the primary, draws diagonal bands through all
  256 indices, fills the bar with index 255 and rotates the palette by one
  entry per frame with `SetEntries`; `ddtest.bmp` is written through the
  palette. `tools/xp-driver-test.sh <image> ddtest` now runs 8, 16, 32 and
  windowed from a staged `E:\RUN.BAT` (the chain outgrew the Run dialog:
  the earlier truncated line was why `dd32.log` went missing once) and
  pulls `dd8.log` / `dd8.bmp`; a QMP screendump during the 8 bpp run shows
  the ramps through the device's conversion
  (`build/xp-driver-test/pal-shot/cmd-02.png`).
- **Diablo (1.00, the retail disc) plays.** `tools/xp-diablo.sh install
  <image>` runs Blizzard's installer (three clicks, the game starts by
  itself), `play` runs `C:\Diablo\Diablo.exe`: the Blizzard North and
  intro videos, the title menu with its palette-cycled flames, a new
  Warrior into Tristram, a click to walk, the character sheet, all at
  640×480×8 through the palette; the QMP screendumps (`title.png`,
  `town.png`, `walk.png`, `char.png` in `build/xp-driver-test/diablo-play/`)
  are the device's conversion and show the right colours. The whole run
  takes 2 minutes under KVM. Quirks: the game's menus ignore QMP clicks
  (keyboard instead), the installer's "DirectX 2.0 cannot be detected
  (sound)" warning is the missing sound card, alt+F4 exits the game.
  Not tested: the dungeon levels (the light radius is palette work too),
  a sound card, TCG timing.

## M7b / M7c pointers

- **M7b DirectDraw DDI:** landed in its first cut (above); 8 bpp
  palettized modes since 2026-09-04 evening (Diablo plays); the flip
  chain's vertical blank since 2026-09-05 (below). Left:
  `DrvDeriveSurface`, a present signal in phase with the player's
  swapchain, StarCraft / Age of Empires / Caesar 3 as further 8 bpp
  titles.
- **M7c Direct3D DDI: first cut landed (see the section below); FIFA 2000
  runs on it out of the box (intro, title, attract-mode match).** Left:
  a user-driven match (input, menus, frame rate), claiming T&L, DX8
  tokens / `GUID_D3DCaps` for `D3DCAPS8`, colour keying, render-to-texture,
  state sets. Exit: doc 04 matrix with no DLL in the game folder.

## M7c — the Direct3D DDI (2026-09-04, first cut)

The DX7 HAL behind the display driver, on the doc 14 protocol and executor.
Nothing new was invented for the transport: the adapter's VRAM grew to
128 MiB and its top 64 MiB is a command window in exactly the SysBus
device's layout (`d3dpt_proto.h`: header page, records, return area), so
the guest encoder `d3dpt_enc.h` and `d3dpt_exec_submit` work unchanged;
the DOORBELL register submits the window, D3D_STATUS says whether the
host has an executor, CMD_OFFSET where the window is (register set
version 2, `d3dpt_fb.h`). The executor library is dlopened once per
process (`d3dpt/hw/d3dpt_exec_load.c`, shared with the SysBus device);
each device has its own instance.

```
guest (XP)                                      host
 ddraw.dll (DX7 runtime: T&L in software)        d3dpt-vga: VRAM [heap | 64 MiB window]
   └ dxg.sys ── DDI ──> d3dptdisp.dll              DOORBELL ──> libd3dpt_exec (d3dpt_exec_ddi.cpp)
        CreateSurfaceEx → VRAM_SURFACE record        surface handle → DXVK texture / render target
        ContextCreate  → CTX_CREATE                  d3d9 device, SetRenderTarget + depth
        DrawPrimitives2 → DP2 record (tokens+verts)  token interpreter → DrawPrimitiveUP etc.
        SceneCapture END / Lock / Flip → READBACK    GetRenderTargetData → memcpy into VRAM (+dirty)
        Unlock → VRAM_DIRTY                          texels re-read from VRAM before the next draw
```

- **Surfaces stay in guest VRAM.** dxg's heap allocates every DirectDraw
  surface below the window as before; `DdCreateSurfaceEx`
  (`GUID_Miscellaneous2Callbacks`) registers each one by its
  `dwSurfaceHandle` with VRAM offset, size, pitch, D3DFORMAT (translated
  from the DDPIXELFORMAT) and caps; mip chains send the attached levels'
  offsets. The host creates the DXVK object lazily: a MANAGED texture whose
  levels are filled from the VRAM pointer, a render target, or a depth
  surface (D16/D24X8/D24S8, with fallbacks). `DdUnlock` on a texture or a
  target sends `VRAM_DIRTY` and the host re-reads the texels before the
  next use; `DdDestroySurface` (`NOTHANDLED`, dxg frees the block) sends
  `VRAM_RELEASE`.
- **Rendering goes to a host render target; READBACK brings it back.**
  A context (`D3dContextCreate`) is a render-target + Z pair; the d3d9
  device is created on the first one (backbuffer unused) and the context's
  targets are set on it. The frame is copied into the render target's VRAM
  at `SceneCapture END` (EndScene), at `DdLock` of a target and in `DdFlip`
  before the OFFSET write, so flips, HEL blits, GDI and screenshots see it;
  the copy is skipped when nothing was drawn since (`S_FALSE`). After a
  flip dxg exchanges the two surfaces' VRAM, so `DdFlip` re-registers both
  with swapped offsets (the host keeps the objects, marks them dirty). A
  full `Clear` of the target skips the upload of stale VRAM content; a
  partial one or a draw onto a HEL-blitted background uploads it first
  (sysmem staging → default-pool surface → StretchRect).
- **DrawPrimitives2** copies the runtime's command buffer and the vertex
  buffer (`D3DHALDP2_USERMEMVERTICES` is a user pointer, read directly in
  the caller's context) into one `DP2` record and rings the doorbell; the
  render-state array the runtime keeps next to the stream (`lpdwRStates`)
  is mirrored from the RENDERSTATE tokens by the driver. The host
  interprets the tokens on `IDirect3DDevice9`: RENDERSTATE (DX7 states
  with a d3d9 twin pass through; the DX5/6 ones — texture handle, stipple,
  ROP, colour key — are dropped, once logged), TEXTURESTAGESTATE
  (TEXTUREMAP = surface handle → `SetTexture`; address / filter / LOD
  states → sampler states with the DX7→d3d9 filter renumbering; the rest
  1:1), VIEWPORTINFO + ZRANGE → the viewport (re-applied after every
  target change, d3d9 resets it), SETRENDERTARGET, CLEAR, the list / strip
  / fan tokens and the `_IMM` variants → `DrawPrimitiveUP`, the indexed
  ones → `DrawIndexedPrimitiveUP` over the touched vertex range (the
  `…2` variants add their start vertex), SETMATERIAL / SETLIGHT /
  SETTRANSFORM (WORLD renumbered) → their d3d9 calls for the day T&L is
  claimed, WINFO / SETPRIORITY / SETTEXLOD / CREATELIGHT accepted and
  ignored, STATESET / TEXBLT / palettes logged and skipped. Malformed
  streams answer `D3DERR_COMMAND_UNPARSED` with the offending offset
  (`dwErrorOffset`); out-of-range vertex references skip the primitive.
- **Caps:** `lpD3DGlobalDriverData` (V1 desc: FLOATTLVERTEX, DRAWPRIMITIVES2
  + 2EX, HWRASTERIZATION, TEXTUREVIDEOMEMORY, no HWTRANSFORMANDLIGHT;
  tri/line caps: Z, all blends and compares, Gouraud + specular, fog,
  point/linear/mip filters, wrap/mirror/clamp/border; 9 texture formats:
  X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, DXT1/3/5),
  `lpD3DHALCallbacks` (ContextCreate/Destroy/DestroyAll, SceneCapture),
  `GUID_D3DCallbacks3` (Clear2, ValidateTextureStageState = 1 pass,
  DrawPrimitives2), `GUID_D3DCallbacks2` (SetRenderTarget),
  `GUID_D3DExtendedCaps` (4096² textures, 8 stages, all texture ops,
  stencil), `GUID_ZPixelFormats` (D16, D24X8, D24S8), `DDCAPS_3D` +
  `DDSCAPS_3DDEVICE|TEXTURE|ZBUFFER|MIPMAP`. `DdCanCreateSurface` accepts
  exactly the formats the host mirrors. `-device d3dpt-vga,ddflags=0x20`
  turns Direct3D off (the M7b DirectDraw-only HAL) without a reinstall;
  0x40 / 0x80 / 0x100 / 0x200 / 0x400 / 0x800 drop the 3D caps bits, every
  D3D GetDriverInfo answer, the D3D buffer callbacks, Callbacks3, Misc2 and
  ParseUnknownCommand one at a time (the bisection knobs below).
- **The finding of the day: `GetDriverState` is mandatory.** With the
  first cut XP's `ddraw.dll` reported `DDCAPS_NOHARDWARE` and no 3D at
  all: the whole HAL dropped, as with `DDCAPS_GDI` in M7b. The `ddflags`
  bisection (five runs) pinned it on the `GUID_Miscellaneous2Callbacks`
  answer, and the disassembly of `ddraw.dll` (pulled out of the image
  with `qemu-img convert` + `7z`) shows why: after validating that table
  the runtime checks `lpD3DGlobalDriverData->hwCaps.dwDevCaps &
  (D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT)` and, if
  set, requires `GetDriverState` to be non-NULL, else the object creation
  fails and the HEL-only object is built. Neither the DDK docs nor the
  samples say so. The same pass showed the user-mode GetDriverInfo probe
  (a mangled `GUID_DDStereoMode` the driver must *refuse*), that
  `dxg.sys` queries Callbacks3 and the ParseUnknownCommand pointer at
  enable time and drops Callbacks3 when the latter is refused, and that
  every answer must not exceed `dwExpectedSize` (guard words after the
  buffer are checked). `DdGetDriverState` answers DD_OK with the buffer
  untouched. The second trap of the day was cheaper: `CreateDevice` failed
  with `DDERR_INVALIDPIXELFORMAT` at 32 bpp but worked at 16 bpp, because
  the `DDBD_*` bit-depth flags count *down* (`DDBD_16` 0x400, `DDBD_24`
  0x200, `DDBD_32` 0x100) and the self-contained header had them the
  other way; `dwDeviceRenderBitDepth` is what the runtime checks the
  render target against.
- **Tests.** `tools/d3dpt-dp2-test.cpp` (host stage of `scripts/test.sh`)
  registers a render target, a Z buffer and a texture in a malloc'ed
  VRAM, opens a context and sends the D3D7TEST scene as the DP2 tokens
  the runtime would emit (cyan triangle behind a wrapped checkerboard
  quad, Gouraud fan in front, half-transparent strip: Z test, texture
  wrap, alpha blend), reads back and checks pixels, then feeds hostile
  records (surface beyond VRAM, a record lying about its length, a
  truncated stream, out-of-range vertices, a released texture) and expects
  each refused without killing the executor. `DRIVER\D3D7TEST.EXE`
  draws the same scene through `IDirect3DDevice7` on XP;
  `tools/xp-driver-test.sh <image> d3d7` runs it headless and diffs its
  BMP against the host test's frame.
- **What is not there yet:** T&L (the runtime transforms; claiming
  HWTRANSFORMANDLIGHT is a caps + `D3DCAPS8` change, the tokens are
  mapped), DX8's tokens (streams, shaders, `GUID_D3DCaps`), colour keying
  (`D3DRENDERSTATE_COLORKEYENABLE` is dropped; the plan is key → alpha at
  upload plus alpha test), render-to-texture (a texture with 3DDEVICE
  becomes a render target the host cannot sample), state sets, driver-
  managed textures (TEXBLT), palettized textures, more than one context
  sharing device state, presenting the host frame straight through the
  player's 3D path instead of the readback copy (every frame is a
  1.2 MB `GetRenderTargetData` + memcpy at 640×480×32 today).

Results 2026-09-04 (KVM, RADV host, XP SP3, `tools/xp-driver-test.sh
~/vms/winxp-m7c.qcow2 d3d7`): `D3D7TEST 640 480 32 300` enumerates the
"Direct3D HAL" device (devcaps 0x8ae51, textures 1..4096, 8 stages),
creates the flip chain + 16-bit Z + a 64×64 A8R8G8B8 texture, renders
the scene through DrawPrimitives2 (61 DP2 calls for the run: the runtime
batches every draw of a frame into one call) and reports

| case | result |
|---|---|
| 640×480×32, 300 frames, 4 draws + clear each | 2400–2700 fps |
| 640×480×16, 60 frames | 1277 fps (first run of the session, includes the device creation) |
| the back buffer read through `Lock` after `EndScene` | byte-identical to `build/d3dpt-dp2-test`'s frame (0 of 307200 pixels differ) |

Per frame that is one 1.2 MB readback (`GetRenderTargetData` +
memcpy) plus the page flip; the executor logs the DX7-only render
states the runtime sends (4, 10, 30, 33, 47: texture perspective, line
pattern, ZVISIBLE, stippled alpha, ZBIAS) once and drops them.

### FIFA 2000 on the HAL (2026-09-04)

The first real DX7-era title, the one that was parked on WineD3D (doc 00
known issues, doc 14): with the WineD3D DLLs renamed out of the game
folder (`tools/xp-fifa2000.bat`, run by `GAME_ISO=FIFA2000.ISO SHOTS=24
tools/xp-driver-test.sh ~/vms/winxp-m7f.qcow2 bat tools/xp-fifa2000.bat`)
the game's own DirectX renderer (`THRASH\dx6z.dll`, registry `Thrash
Driver = dx`, `Hardware Acceleration = 1`, `Thrash Resolution = 800x600`)
runs on the HAL **unmodified**: the EA intro at 640×480×16 (19 DP2 calls,
page flips), the title screen, then the attract-mode match at 800×600×16
with textured players, kits, crowd, pitch and the HUD, all through
`DrawPrimitives2` on DXVK and the readback into VRAM (screendumps in
`build/xp-driver-test/fifa/cmd-*.png`). The match does not page-flip (4
`scanout offset` lines for the whole run): the game blits its back buffer
to the primary, so every frame is READBACK + a driver-side blit. Executor
log for the run: the DX5/6 render states dropped once (1–6, 10–13, 17,
18, 21, 30–33, 39, 40, 43–47, 49: texture handle / address / filter /
map-blend, ROP, plane mask, stipple, ZBIAS…), DXVK's own "unhandled
render state 26 / 62 / 128" (dither, an unknown, WRAP0), no colour keying
requested, no unsupported token, no refused record. Nothing crashed;
the run ends with a clean power-down while the match is still playing.
Not yet measured: the frame rate (the player shows it), input (the
headless loop never touches the game), a full user-driven match with the
menus, the 640×480 in-game resolution.

Played by hand the same day: graphics clean, smooth under KVM. The user
found **the keyboard dead in the match under TCG** (`-cpu pentium3`, no
KVM) and working under KVM. What the investigation established
(2026-09-04, all on this Linux box, headless runs driven over QMP with
`tools/xp-driver-test.sh`, plus the real player with `PLAYER_KEYS`):

- Not the emulator's input path. `DRIVER\DITEST.EXE` (a game-style
  DirectInput keyboard, exclusive + foreground, with a busy loop between
  polls) sees every key under TCG, in bare QEMU and through the player's
  own queue; the embed library's new `qemu-embed: input:` statistics show
  no drain latency over 20 ms and no key down/up pair delivered in one
  drain. Not XP's `LowLevelHooksTimeout` (5000 ms changes nothing). Not
  the frame rate: the match renders at the game's own 30 fps cap under
  TCG here (`d3dpt-vga: ddi: 30.0 frames/s` in the log).
- The game itself. `D3DPT\DINPUT.DLL` next to the EXE (a forwarding shim
  that logs the game's DirectInput use, `dinput_log.txt`) shows FIFA's
  keyboard device is `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND`, polled with
  `GetDeviceState` 30 times a second, no errors — and in the match it
  reports **no key at all**, KVM or TCG, while a sampler thread in the
  same process sees every key through `GetAsyncKeyState`. The front end
  (title screen, side selection) works through the same device. On XP a
  non-exclusive DirectInput keyboard is fed by a low-level hook that runs
  on the thread which created the device, only while that thread services
  its message queue; FIFA's match loop does not pump. Why the user's KVM
  session got through is not settled (likely the loop's idle time on a
  fast CPU); the headless KVM runs did not.
- The fix is in the shim: every key `GetAsyncKeyState` reports pressed is
  set in the keyboard state handed back (DIK from the scan code, the
  extended keys mapped by hand), logged once per key when DirectInput's
  own state lacked it. With `DINPUT.DLL` in the game folder the match
  takes 100 ms taps (F2 camera, Esc pause, F12 exit) under KVM and TCG
  alike. The user-facing recipe: copy `D3DPT\DINPUT.DLL` next to
  `fifa2000.exe`; `tools/xp-fifa2000.bat` does it from `E:\DINPUT.DLL`.
  **Confirmed by the user 2026-09-05, both directions:** on a TCG run on
  this Linux box the match takes keys with `DINPUT.DLL` next to the EXE, and
  moving the DLL away brings the dead keyboard straight back. That is the
  causal check the headless harness alone could not give — the shim is the
  variable, not the driver, the CPU model or the run.
- **How much this matters, from the same day:** the user's everyday setup is
  the Linux host run natively (KVM) with `-vga none -device d3dpt-vga`, and
  there they have **no input issues and no custom DLLs anywhere** — no
  WineD3D set renamed out of a game folder, no shim next to any EXE, a stock
  XP on the driver. So the unpumped-hook symptom is a **TCG-only** one, as
  the KVM/TCG split above already suggested: the game's match loop does pump,
  rarely, and only a guest slow enough to stretch the gaps lets the hook fall
  behind. `DINPUT.DLL` is therefore medicine for the Apple Silicon path (TCG
  is the only x86 accelerator there) and for `xp-fifa-match.sh tcg` — not
  something in the normal path on a KVM host, which is one more reason for
  the per-game placement decided below.

**Where the merge lives: next to the game, never system-wide** (decided
2026-09-05, after the confirmation). The tempting alternatives are both
wrong here:

- Replacing `system32\dinput.dll` fights Windows File Protection on XP SP3
  (SFC restores it from `dllcache`), and a forwarding shim cannot carry the
  same name as the DLL it forwards to in the same directory — the original
  would have to be renamed, which breaks SFC and any repair install.
- `AppInit_DLLs` loads the shim into every GUI process on the system,
  including explorer and every installer, to fix one game's match loop.

And the merge is not a neutral improvement: `GetAsyncKeyState` is
system-wide, so a `DISCL_FOREGROUND` device that has correctly gone quiet
(the game is not in front, or is not acquired) would start reporting keys
again, and an application that reads buffered data alongside `GetDeviceState`
would see the two disagree. That is a lie we are happy to tell FIFA's match
loop, having watched it, and not one to tell every process on the guest.
So the shim stays a per-game, side-by-side DLL — the era-correct mechanism,
reversible by deleting one file — and *deploying* it becomes the launcher's
job (M6): a per-game compat list in the machine bundle that stages shim DLLs
next to the EXE, the same shape `D3DPT\DDRAW.DLL` already needs.

Making it shippable (same day): the shim now has two modes. The default is
the fix and nothing else — silent, no `dinput_log.txt`, no sampler thread.
`D3DPT_DINPUT_LOG=1` in the environment restores the full diagnostic build
described above. The observation was the expensive half: the sampler polls
248 virtual keys every 5 ms and every log line is flushed, which is real
money under TCG and not something to leave running in a game folder for a
fix that is 20 lines of merge. `tools/xp-fifa2000.bat` turns the log on when
it finds an `E:\DILOG` marker, which `tools/xp-fifa-match.sh` stages because
it greps the log for its regression check.
- FIFA's own quirks met on the way: its front-end menus need a mouse
  button held ≈1 s (a 100 ms click is ignored; the QMP `click` in
  `qmpc.py` is too short, hold the button by hand in `input-send-event`);
  the intro video can be skipped with Esc; the kickoff starts by itself
  after ≈1 minute; F1–F4 cameras, Esc pause, F12 exit are the readme's
  in-match keys. `tools/xp-fifa-match.sh kvm|tcg <image>` does the whole
  thing headless (menus, side, kickoff, the tap test with screendumps,
  `dinput_log.txt` pulled from the image): the regression check for
  "keys in a real DX7 game" on the HAL.

### Max Payne on the HAL: XP's own d3d8.dll on a DX7 driver (2026-09-05)

The first DX8 title with **no wrapper DLL in the game folder**
(`tools/xp-maxpayne.bat` renames the M4 track's `D3D8.DLL` away; run by
`GAME_ISO=DINO-MAP.iso CPU=pentium3 SHOTS=30 SHOT_KEYS="2:ret,6:ret"
tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 bat tools/xp-maxpayne.bat`).
XP's d3d8.dll treats a driver without a `D3DCAPS8` answer as a
"DirectX 7 driver": it does the vertex processing itself and feeds the
DX7 token set (`INDEXEDTRIANGLELIST2`, `TRIANGLEFAN_IMM`, the DX7 render
and stage states) through DrawPrimitives2 — the same HAL FIFA runs on.
Result (on the DX7 face of the driver — since the DX8 DDI below landed,
`ddflags=0x2000` gives d3d8.dll that face again): the launcher, the main
menu and the tutorial level (Max in the alley, textures, lightmaps,
decals, snow, HUD, matching the M4 device's frame of the same scene)
render at 800×600×16, ~290 frames/s under KVM `-cpu pentium3` with 18
DP2 calls and ~230 draws per frame, no unsupported token, no refused
record; screendumps in `build/xp-driver-test/mp-hal10/`. What it took,
and what it taught:

- **`TRIANGLEFAN_IMM` / `LINELIST_IMM`: the payload after the 4-byte
  header is DWORD-aligned, and so is the next token.** The first runs
  desynchronised (garbage tokens 86, 115, 255…, "truncated" errors, the
  runtime re-submitting from `dwErrorOffset`) exactly after every
  inline-vertex token that started at offset 2 mod 4 — which happens
  after an `INDEXEDTRIANGLELIST2` with an even primitive count (2 + 6n
  bytes), a sequence the DX7 runtime never produced for D3D7TEST or FIFA.
  The DX8 runtime's legacy path does it every frame. Aligning only the
  token's *end* made the stream parse but still read the edge flags and
  vertices two bytes early: the per-draw snapshots of the frame trace
  (below) showed one fan per frame with positions of 10¹¹ and colours
  that were the top halves of floats — the **black bands across the
  alley** (a garbage triangle clipped to the screen), which no state,
  texture or fog experiment had explained. The rule (the DDK's perm3
  sample does the same): after the `D3DHAL_DP2COMMAND`, round the
  offset up to 4, then the `D3DHAL_DP2TRIANGLEFAN_IMM` and the vertices;
  round up again for the next command. `tools/d3dpt-dp2-test.cpp` draws
  its fan as an inline token at such an offset with the runtime's
  padding (0xcc bytes, so a parser that reads them as data fails). The
  executor also logs the token history (`ddi: dp2: tokens before it
  (offset:op x count): …`) with every first failure of a kind, which is
  how the pattern showed.
- **DXVK's exceptions abort QEMU; validate before calling.** The garbage
  streams reached `LightEnable` with an index of ~2³² and DXVK grows its
  light array to the index: `std::bad_alloc`. It cannot be caught in the
  executor — DXVK carries its own statically linked unwinder, the system
  `__gxx_personality_v0` is handed its context and `abort()`s (two
  core dumps, `_Unwind_Resume` in `libdxvk_d3d9.so.0` under
  `LightEnable.cold`). The interpreter now drops light indices ≥ 1024 and
  transform ids outside VIEW / PROJECTION / TEXTURE0–7 / WORLD0–3 (DXVK
  indexes an array with them), and `d3dpt_exec_submit` still wraps each
  record in a `try` for the executor's own allocations. The host test
  sends both and expects the draw to go on; on the old library the test
  process aborts (exit 134), which is the reproduction.
- **A per-frame DP2 trace, armed by a file.** `D3DPT_DP2_TRACE=<path>`
  in QEMU's environment: `touch <path>` when the screendump shows the
  scene in question; the executor arms, and from the next frame boundary
  (a readback) to the one after it logs every token with its arguments —
  a snapshot of every render / stage state seen so far at the start
  (most are set once per scene), then render states (marked when
  dropped), stage states, each bound texture's size / format / levels /
  caps and the mean of its VRAM texels, every draw with its first three
  vertices (position, colours, both uv sets), clears, targets, viewports
  — and writes next to the flag file every bound texture's levels
  (`tex-<handle>-l<n>.ppm` + `-a.pgm` for alpha) and the render target
  after every draw (`draw-<n>.ppm`), then removes the file. That last
  part is what found the garbage fan: a script that counts black pixels
  per snapshot names the draw, and its vertex lines in the log name the
  bug. `D3DPT_DDI_REREAD=1` re-reads every texture from VRAM at every
  bind (a stale host copy vs VRAM the guest never wrote);
  `D3DPT_DDI_NOFOG=1` forces FOGENABLE off. Both were used to rule
  things out here. The driver now logs every surface it registers
  (`d3dptdisp: surface <handle> caps … w h fmt at …`, `sysmem, skipped`
  / `no format, skipped`), so an unknown handle in the executor's log can
  be looked up: the `render target handle 3 unknown` line at start is
  the third buffer of the game's flip chain, set as a target once before
  its `CreateSurfaceEx` (harmless).
- The DX7 states the DX8 runtime sets on a legacy driver and the executor
  drops once: 4, 10, 30, 33, 40, 47 (TEXTUREPERSPECTIVE, LINEPATTERN,
  ZVISIBLE, STIPPLEDALPHA, EDGEANTIALIAS, ZBIAS — the last one matters
  for decals and has a d3d9 twin, DEPTHBIAS; the alley sets it to 0).
  What the traced frame looks like, for the next title: 230 draws, sky as
  38 opaque fans of one 512×256 texture, world geometry as `ADD(lightmap,
  diffuse)` on stage 0 (uv set 1) then `MODULATE(material)` on stage 1,
  decals and sprites blended SRCALPHA / INVSRCALPHA with A8R8G8B8
  textures (alpha test on for some), fog on with table mode NONE and
  the factor in the specular alpha, all textures 16- or 32-bit RGB (no
  DXT, no palettes).

## M7c — the DirectX 8 DDI (2026-09-05)

The driver is a DirectX 8 driver to d3d8.dll now: `D3DCAPS8` with hardware
transform and lighting, the DX8 token stream, render-to-texture, state
sets. D3DGAME8 (the M4 track's DX8 reference scene) runs through XP's own
d3d8.dll on it with **hardware vertex processing** — cubes, lit ground,
the render-to-texture panel, the index-buffer grid, the particles — at
~575 fps under KVM, with no wrapper DLL near it
(`tools/xp-driver-test.sh <image> d3dgame8`). FIFA and D3D7TEST keep
working; Max Payne renders on it except its clipped fans (the
"clipper" bullet, open). What the DDI is made of:

- **`GetDriverInfo2`.** With `DDHALINFO_GETDRIVERINFO2` in the HAL info
  the runtime sends `GUID_DDStereoMode` queries whose data starts with a
  `DD_GETDRIVERINFO2DATA` header (`dwMagic` = `D3DGDI2_MAGIC`); the
  driver answers `DXVERSION` (the runtime says 0x802), `GETD3DCAPS8`
  (the 212-byte `D3DCAPS8`), `GETFORMATCOUNT` / `GETFORMAT` (the DX8
  format list: `DDPF_D3DFORMAT` entries with the `D3DFORMAT` in `dwFourCC`
  and the operations in the `dwRBitMask` slot — texture, display mode
  with 3D acceleration, offscreen / same-format render target, Z-stencil
  for the twelve formats the host mirrors) and refuses the rest (0x18 is
  asked too). A real stereo query, without the magic, is refused as
  before. **Two findings from `d3d8.dll`'s disassembly:** without the
  HAL-info flag the runtime never asks and stays on the DX7 path (which
  is why the first run reported hardware vertex processing and no
  render-target textures — the T&L claim in the DX7 caps was enough for
  that); and the runtime checks `dwActualSize` against the size *inside*
  the GDI2 header while leaving the outer `dwExpectedSize` at the
  previous query's 24 bytes — an answer clamped to the outer size makes
  it drop the driver altogether (`GetDeviceCaps` fails, `CreateDevice`
  answers `D3DERR_NOTAVAILABLE`). `pf_format` reads the D3DFORMAT-coded
  pixel formats the DX8 runtime creates surfaces with.
- **The caps.** `D3DCAPS8`: the DX7 caps in DX8 form plus
  `HWTRANSFORMANDLIGHT` (also claimed in the DX7 `D3DDEVICEDESC`, with
  the transform / lighting caps, the extended caps' lights, clip planes,
  blend matrices and vertex-processing caps: the executor maps
  SETTRANSFORM / MULTIPLYTRANSFORM / SETLIGHT / SETMATERIAL and the
  lighting states onto DXVK's fixed-function pipeline, so the runtime
  hands us untransformed vertices; `ddflags=0x1000` withdraws the claim),
  `PUREDEVICE`, one stream, 16-bit indices, vertex / pixel shaders
  `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)` since the shader section
  below (0.0 in the first cut), 4096² textures, 8
  stages, no cube or volume maps, and **no `D3DPMISCCAPS_CLIPTLVERTS`**:
  with it the runtime stops clipping pre-transformed vertices and hands
  the driver polygons that cross the camera plane, which the host
  rasterizes as garbage (Max Payne transforms on the CPU even on a T&L
  device — every draw of its frame is an RHW format — and its alley
  walls came out as flat panels at wrong depths until the claim went;
  the DX7 runtime always clipped them). `ddflags=0x2000` keeps the DX7
  face (no GDI2 flag).
- **The tokens.** The runtime's DX8 draws name vertex and index buffers
  by surface handle — buffers in guest system memory the host cannot
  see. The driver keeps a table of every surface dxg reports (VRAM and
  system memory alike, with each level's address and pitch, buffers with
  their linear size) and walks the stream twice in `D3dDrawPrimitives2`:
  SETVERTEXSHADER (an FVF: shader handles have bit 0 set), SETSTREAMSOURCE
  / SETSTREAMSOURCEUM / SETINDICES become driver state, each DRAWPRIMITIVE
  (2) / DRAWINDEXEDPRIMITIVE(2) / CLIPPEDTRIANGLEFAN becomes a
  self-contained `D3DPT_DP2_DRAW8` token (`d3dpt_proto.h` v6) carrying
  the primitive, the FVF, the vertex range and the indices (relative to
  the runtime's MinIndex), TEXBLT is done in the driver (system-memory
  texture → VRAM, every level, then VRAM_DIRTY; the DX8 runtime loads
  managed textures that way instead of blitting), shader tokens, patches,
  volume / buffer blits and dirty rects are dropped by size, the DX7
  tokens pass through (the inline-vertex ones re-padded for their new
  offset). The first pass measures and does the blits, the second writes
  the record. **That state persists between calls:** the runtime sends
  SETVERTEXSHADER / SETSTREAMSOURCE / SETINDICES only on change, so the
  vertex format and the bindings live in the context (`D3DCTX`) — as
  handles, resolved from the table at every call, because a `Lock` with
  DISCARD gives a buffer new memory and dxg reports it with another
  `CreateSurfaceEx` (the log's `at 0x00000000` lines are the old memory
  going); the first D3DGAME8 run lost every `DrawPrimitiveUP` for that
  (they arrive in their own calls, "dx8 draws skipped … fvf 0"). A
  user-memory stream holds `dwVertexLength` vertices of the token's
  stride, not of `dwVertexSize`. The executor draws a
  DRAW8 with `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` after
  `SetFVF`, and for the DX8 tokens that do reach it knows the sizes and
  drops them with a note.
- **State sets** (`STATESET`): BEGIN / END record into a d3d9 state block
  (a predefined type is `CreateStateBlock`), EXECUTE applies it, CAPTURE
  refreshes it, DELETE releases it. **Render-to-texture:** a texture with
  3DDEVICE caps is a default-pool render-target texture whose level 0 is
  the target; uploads go through the staging path, readback as for any
  target. **MULTIPLYTRANSFORM** and the d3d8-only render states (153,
  164, 172, 173 dropped; the DX8 numbering of the rest is d3d9's).
- **Compressed textures need a `DdCreateSurface` that sizes them.** dxg
  sizes a video-memory surface from its pixel format's bit count before
  it takes it from the heap; a FOURCC format has no bit count, so the
  request was for zero bytes and its one `DDERR_OUTOFVIDEOMEMORY` site
  answered — `CreateTexture(DXT1)` in `D3DPOOL_DEFAULT` failed with
  `D3DERR_OUTOFVIDEOMEMORY`, while `MANAGED` / `SYSTEMMEM` succeeded (the
  runtime's own system-memory copy: a surface with *no* pixel format,
  the compressed bytes as a 128×16 or 256×16 "display-format" image) and
  the video-memory copy then failed silently at the first draw, so the
  runtime kept the previous texture bound (D3DGAME8's particles as its
  gradient). The driver had no `CreateSurface` callback at all. It has
  one now: for a `DDPF_FOURCC` DXT surface it sets `dwBlockSizeX` = the
  linear size, `dwBlockSizeY` = 1, `fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE`
  (dxg allocates that many bytes from the linear heap), `dwLinearSize`
  in the `lPitch` union (which is why dxg's "pitch" of a DXT surface is
  its linear size: the driver put it there) and `DDSD_LINEARSIZE` on the
  description; everything else returns `DDHAL_DRIVER_NOTHANDLED`
  untouched. Managed textures are filled by the runtime through Lock /
  Unlock (Unlock marks the VRAM dirty), not TEXBLT. Also: DirectDraw
  creates a FOURCC surface only when the code is in the driver's FOURCC
  list (`DrvGetDirectDrawInfo`'s `pdwFourCC`, two-call protocol) — the
  DX8 runtime never reads the codes (it passes a null pointer in all
  three `DdQueryDirectDrawObject` calls), the kernel does; and a
  FOURCC-style entry in the GDI2 format list makes `CreateTexture` fail
  outright — d3d8.dll matches formats against `dwFourCC` under
  `DDPF_D3DFORMAT` only. `DRIVER\DXTTEST.EXE` is the probe that found
  it: every format × pool through CheckDeviceFormat, CreateTexture,
  Lock, a textured quad read back (pure red / blue block texels are the
  pass), CreateImageSurface; every HRESULT in `dxttest.log`.
- **The runtime's clipped fans are stream-0 draws; the DP2 vertex buffer
  is a dummy under d3d8.dll.** With `CLIPTLVERTS` withdrawn the runtime
  clips pre-transformed triangles itself and emits them as
  `CLIPPEDTRIANGLEFAN` tokens (58: FirstVertexOffset, dwEdgeFlags,
  PrimitiveCount). Where the vertices are was settled in `d3d8.dll`'s
  disassembly (2026-09-05): the DX8 DDI layer keeps two internal
  "TL streams" (a 44-byte object each: an `IDirect3DVertexBuffer8` it
  creates itself in `D3DPOOL_DEFAULT` with `D3DUSAGE_DYNAMIC`, a stride,
  the bytes used, the base of the current batch), one for the software
  pipeline's output and one for the clipper. `DrawClippedPrim` locks the
  clip stream (`D3DLOCK_NOOVERWRITE`, or `DISCARD` when it wraps), copies
  the fan's vertices in at the *current* FVF's stride, and writes the
  stream's batch base into `FirstVertexOffset`; before that, when the
  clip stream is not the current stream 0, it emits **`SETSTREAMSOURCE`
  (49) for stream 0 with the clip buffer's handle and stride**
  (`SETSTREAMSOURCEUM` if the buffer had no driver handle) and flags the
  application's stream to be re-set on the next draw. So a fan's offset
  is a byte offset into stream 0 as bound at that moment — a vertex
  buffer the driver knows by handle, with its size as the bound. The
  DP2 call's own vertex buffer never carries anything on the DX8 path:
  the DDI layer's init fills `dwFlags` 0x9, `lpVertices` = a 10 × 32-byte
  dummy, `dwVertexLength` 10, `dwVertexSize` 32 once and only
  `DrawPrimitiveUP` swaps its user pointer in temporarily
  (`SETSTREAMSOURCEUM`). The driver first read the fans at `lpVertices
  + FirstVertexOffset` (as the DX7 runtime's fans are laid out) and got
  the dummy's neighbours: heap garbage, zeros mostly — Max Payne's
  nearest walls and ground black. Reading them from stream 0 fixed it;
  the user-memory buffer's bound is the declared `dwVertexLength ×
  dwVertexSize` again.
- Tests: `tools/d3dpt-dp2-test.cpp` sends DRAW8 tokens (an indexed quad
  with a MinIndex of 10, a fan), a recorded state set executed, captured
  and deleted, a DRAW8 with an index beyond its vertices (skipped) and
  one lying about its stride (refused);
  `tools/xp-driver-test.sh <image> d3dgame8` boots the guest-tools ISO,
  copies `D3DGAME8.EXE` out alone and diffs its frame against the native
  d3d9 oracle of `scripts/test.sh` (HUD masked).
- Not there: more than one stream, video-memory
  buffers (`D3DDEVCAPS_HWVERTEXBUFFER`), cube and volume textures, N-
  and RT-patches, ZBIAS → DEPTHBIAS, palettized textures. DXT textures on this path were fixed
  on 2026-09-05 (the compressed-textures bullet above); vertex and pixel
  shaders 1.x the same night (the section below).

### Vertex and pixel shaders 1.x on the DX8 DDI (2026-09-05, protocol v7)

The caps say `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)` now (96 vertex
constants, `MaxPixelShaderValue` 8; `ddflags=0x4000` withdraws both), so
d3d8.dll creates every shader through the driver: with hardware vertex
processing the runtime validates the declaration and the function against
the caps and emits `CREATEVERTEXSHADER` (handle, declaration bytes,
function bytes, then both), `SETVERTEXSHADER` with that handle (an FVF has
bit 0 clear, a handle bit 0 set), `SETVERTEXSHADERCONST` (register, count,
float4s), `DELETEVERTEXSHADER`, and the pixel-shader four
(`CREATEPIXELSHADER` is handle + function). The design keeps the driver
out of it:

- **Driver.** The seven shader tokens pass through the DP2 walk unchanged
  (they were dropped by size before). A `SETVERTEXSHADER` value is kept as
  the context's vertex format whatever it is, and a `D3DPT_DP2_DRAW8`
  under a shader carries the handle in its `fvf` field with the stream's
  stride: the driver copies `nverts × stride` bytes as before and only
  the host, which has the declaration, knows what a vertex is (the
  `fvf_stride > stride` check is skipped under a shader; the "1 shader"
  skip reason is gone).
- **Executor** (`d3dpt_exec_ddi.cpp`). Shaders live per context (the
  runtime's handles are per device), by handle. `CREATEVERTEXSHADER`
  converts the `D3DVSD_*` declaration to `D3DVERTEXELEMENT9`s (`STREAM`,
  `REG` with its register number naming the usage by the DX8 convention
  — 0 position, 3 normal, 5 diffuse, 7.. texcoords — and its type
  numbered as `D3DDECLTYPE_*`, `SKIP`, `CONST` runs remembered, tessellator
  and `EXT` tokens skipped) into a d3d9 vertex declaration, remembers the
  bytes it reads of a stream-0 vertex and the streams it touches, and,
  when there is a function, validates it and puts one `dcl_usage vN` per
  input register in front (d3d9 wants them; DX8 shaders have none) before
  `CreateVertexShader`. A declaration without a function is the fixed
  function on that layout (`SetVertexDeclaration` + `SetVertexShader(NULL)`
  — a `D3DVSD_REG(D3DVSDE_DIFFUSE, …)` before the position, which no FVF
  can express, works). `SETVERTEXSHADER` applies the declaration, the
  function and the `D3DVSD_CONST` runs (DX8 loads them when the shader is
  set); an FVF restores `SetVertexShader(NULL)` + `SetFVF`. The constants
  go to `Set*ShaderConstantF` with the register range checked (DXVK
  indexes arrays with them). A DRAW8 under a shader is skipped, with one
  log line, when the handle is unknown, the declaration reads a stream
  other than 0 (one stream is claimed), or it reads more than the stride
  carries; otherwise it is the same `DrawPrimitiveUP` /
  `DrawIndexedPrimitiveUP` with the declaration bound.
- **The bytecode is validated before DXVK sees it.** The d3d9 half hands
  guest bytecode straight to DXVK, and the hostile case of the host test
  showed why that is not enough here: on a stream with an unknown opcode
  DXVK's compiler logs `No layout known for opcode` and then *asserts*
  (`sm3_parser.h: getDst … m_operands[0u].getInfo().kind == eDstReg`) — an
  abort, not an exception, QEMU dies with it. `sm1_valid` walks the
  tokens (bit 31 = a parameter, else an instruction; `DEF` carries four
  raw floats, `COMMENT` its length, `PHASE` only in ps 1.4) against a
  table of the vs 1.x / ps 1.x opcodes with their operand counts, and
  checks every register against the stage's file sizes. Anything else is
  refused with a log line and the handle stays unknown (its draws are
  skipped). The DX8 runtime validates every shader itself before the
  token, so a real guest never gets there.
- Tests: `tools/d3dpt-dp2-test.cpp` — vs 1.1 through a declaration
  (`oD0 = v5 * c0`: red, then green by the constant alone), a
  declaration-only shader with the colour before the position, a
  `D3DVSD_CONST` in the declaration, ps 1.1 `r0 = c0` and off again,
  ps 1.1 `tex t0` / `mul r0, t0, v0` on XYZRHW vertices, then the
  hostile set (a function without END, one with unknown opcodes, one
  missing its operands, a register off the file, a constant as the
  destination, a declaration reading stream 1, one wider than the
  stride, an unknown handle, constants at c300 / c250, a
  `CREATEVERTEXSHADER` lying about its declaration size →
  `D3DERR_COMMAND_UNPARSED`), and the FVF path after all that.
  `DRIVER\SHTEST.EXE` (`guest-tools/src/d3dptvid/shtest.c`) is the same
  through XP's own d3d8.dll — hand-assembled shaders (no D3DX in mingw),
  every draw read back through `CopyRects` and compared; also the vertex +
  index buffer path (`DrawIndexedPrimitive` under a shader); `tools/xp-driver-test.sh
  <image> shtest` runs it and greps `shtest.log` for "0 failed" (9 cases
  pass on `winxp-m7g`, 2026-09-05). One thing the guest run taught: a
  declaration-only shader must list its registers in FVF order —
  d3d8.dll answers `D3DERR_INVALIDCALL` to the colour-before-position
  layout itself, so the driver never sees such a declaration (the host
  test keeps the case because the executor handles it anyway).

