# 15. A real XP display driver (ADR-008, M7)

The long-term guest side of doc 14: instead of DLL copies per game
folder, a Windows display driver pair that owns the adapter, and later
speaks the DirectDraw and Direct3D DDIs into the same transport and
executor. Staged: **M7a framebuffer (this doc, landed 2026-09-04) →
M7b DirectDraw DDI → M7c Direct3D DDI**. ADR-008 (doc 10) has the why;
this doc is the how and the state.

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
- `FRAMES` is a refresh counter, not a vblank; DirectDraw's `WaitForVerticalBlank`
  (M7b) needs a real per-frame signal (the player's present timing) here.
- Mode table from the player (M2): a `modes=` device property or an embed
  API call that replaces the static table, pixel-aspect flags per entry.
- Multi-monitor, 8 bpp palettized modes, DPMS/power states, hot-unplug:
  not planned.
- Win2000 untested (same DDI version; should work).

## M7b / M7c pointers

- **M7b DirectDraw DDI:** `DrvEnableDirectDraw` / `DrvGetDirectDrawInfo`
  (`DD_HALINFO`, VIDEOMEMORY heaps in VRAM), `DdCreateSurface`,
  `DdLock/Unlock`, `DdBlt`, `DdFlip`, `DdWaitForVerticalBlank`, palettes
  refused. Surfaces live in VRAM (the guest's own memory: system-memory
  surfaces need no transport at all); blits go to the executor as records
  through a doorbell on BAR 1 — the `d3dpt` SysBus device's protocol
  (`d3dpt_proto.h`) reused, with the window being VRAM itself. Exit:
  DirectX 5–7 titles without WineD3D.
- **M7c Direct3D DDI:** `D3dContextCreate`, `D3dDrawPrimitives2` (DP2
  token stream → our records), `D3dValidateTextureStageState`,
  `DdCreateSurfaceEx`; caps from the executor. Microsoft's d3d8/d3d9 stay
  in the guest. Exit: doc 04 matrix with no DLL in the game folder.
