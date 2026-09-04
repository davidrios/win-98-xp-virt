# 15. A real XP display driver (ADR-008, M7)

The long-term guest side of doc 14: instead of DLL copies per game
folder, a Windows display driver pair that owns the adapter, and later
speaks the DirectDraw and Direct3D DDIs into the same transport and
executor. Staged: **M7a framebuffer (landed 2026-09-04) → M7b DirectDraw
DDI (first cut landed the same day: VRAM surfaces, page flips, cached
mappings) → M7c Direct3D DDI**. ADR-008 (doc 10) has the why; this doc
is the how and the state.

## Shape (M7a)

```
guest (XP)                                          host (QEMU process)
 win32k.sys (GDI)                                    hw/d3dpt/d3dpt_vga.c  "d3dpt-vga"
   └ d3dptdisp.dll  display driver (winddi.h)          ├ PCI 1234:3d00, class VGA, stdvga ROM
       EngCreateBitmap over the mapped VRAM            ├ BAR 0 VRAM 32 MiB  ─── DisplaySurface points here
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
  not ship, so `guest-tools/src/d3dptvid/ddk-stubs/` declares the seven
  opaque types `winddi.h` names. M7b needs the real DirectDraw DDI
  definitions (from the public documentation).
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
  `ddtest.log` + `ddtest.bmp`. Results 2026-09-04 (KVM, RADV host):

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
  (M7c). Palettized (8 bpp) modes and surfaces, overlays, FourCC and
  different-format surfaces are refused. Not yet exercised: `GetDC` on a
  DirectDraw surface (`DrvDeriveSurface` absent, DirectDraw falls back),
  a real DX5–7 game.
- Mode table from the player (M2): a `modes=` device property or an embed
  API call that replaces the static table, pixel-aspect flags per entry.
- Multi-monitor, 8 bpp palettized modes, DPMS/power states, hot-unplug:
  not planned.
- Win2000 untested (same DDI version; should work).

## M7b / M7c pointers

- **M7b DirectDraw DDI:** landed in its first cut (above). Left: a DX5–7
  title on it (2D titles run now; 3D ones need M7c), `DrvDeriveSurface`,
  the vblank signal.
- **M7c Direct3D DDI — design (2026-09-04, not started):**
  - *Transport:* BAR 0 grows to 128 MiB; the top 64 MiB is the command
    window in the SysBus device's exact layout (`d3dpt_proto.h`: header
    page, records, return area at 48 MiB), so `d3dpt_enc.h` and
    `d3dpt_exec_submit` work unchanged; the DirectDraw heap ends below it.
    A DOORBELL register on BAR 1 submits the window; the executor is the
    same `libd3dpt_exec` (the loader moves out of `d3dpt_mm.c` into a
    shared helper). New executor call `d3dpt_exec_set_vram(ptr, size)`.
  - *Surfaces stay in guest VRAM.* `DdCreateSurfaceEx` (through
    `GUID_Miscellaneous2Callbacks`) registers every texture / render
    target / Z surface with a `VRAM_SURFACE` record (handle, VRAM offset,
    w, h, pitch, format, caps); the executor creates the DXVK object and
    reads texels straight from the VRAM pointer (no guest copy); `DdLock`
    / `DdUnlock` on a texture send `VRAM_DIRTY`. Rendering goes to a host
    render target; a `READBACK` record (at `DdFlip`, and at `DdLock` of a
    render target) copies it back into the guest's VRAM surface (host
    memcpy), so flips, HEL blits and screenshots see the frame. Presenting
    the host frame straight through the 3D frame path is the later
    optimisation.
  - *Commands:* `D3dDrawPrimitives2` copies the DP2 command buffer and the
    vertex data into the window as one `DP2` record (context handle,
    vertex type, lengths) and rings the doorbell; the executor interprets
    the tokens on DXVK d3d9. First cut = the DX7 non-T&L HAL: the runtime
    transforms and lights in software and sends `XYZRHW` vertices, so the
    executor only needs RENDERSTATE, TEXTURESTAGESTATE, VIEWPORTINFO,
    WINFO, ZRANGE, SETRENDERTARGET, CLEAR, the (indexed) list / strip /
    fan tokens → `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP`. Claiming
    T&L (SETTRANSFORM / SETLIGHT / SETMATERIAL, 1:1 on d3d9) and the DX8/9
    tokens (streams, shaders, `D3DCAPS8` through `GUID_D3DCaps`) come after.
  - *Caps:* `lpD3DGlobalDriverData` (`D3DNTHALDEVICEDESC_V1` + the texture
    format list), `lpD3DHALCallbacks` (`ContextCreate` → `CREATE_DEVICE`
    sized like the render target, `ContextDestroy`), `GUID_D3DCallbacks3`
    (`DrawPrimitives2`, `Clear2`, `ValidateTextureStageState`),
    `GUID_D3DExtendedCaps`, `GUID_ZPixelFormats`.
  - *Test:* a `D3D7TEST.EXE` (IDirect3D7 HAL device, clear + textured
    TLVERTEX triangle, BMP dump) next to DDTEST; then FIFA 2000 (doc 00).
  Exit: doc 04 matrix with no DLL in the game folder.
