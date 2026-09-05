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
  move (bounded at 50 ms), `DdGetBltStatus` / `DdGetFlipStatus` = always
  done. Not hooked: Lock, Unlock, Blt, CreateSurface, DestroySurface.
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
  primary, 0x10 = add `DDCAPS_GDI`. That is how the caps were bisected
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
- `FRAMES` is a refresh counter, not a vblank: `DdWaitForVerticalBlank`
  waits for the next player refresh pull (16 ms), not for a display's
  vertical blank. A real per-frame signal from the player's present is
  the follow-up; `DDFLIP_WAIT` today never throttles (flips are instant).
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
  palettized modes since 2026-09-04 evening (Diablo plays). Left:
  `DrvDeriveSurface`, the vblank signal, StarCraft / Age of Empires /
  Caesar 3 as further 8 bpp titles.
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
Result: the launcher, the main menu and the tutorial level (Max in the
alley, textures, lightmaps, decals, snow, HUD, matching the M4 device's
frame of the same scene) render at 800×600×16, ~290 frames/s under KVM
`-cpu pentium3` with 18 DP2 calls and ~230 draws per frame, no
unsupported token, no refused record; screendumps in
`build/xp-driver-test/mp-hal10/`. What it took, and what it taught:

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

