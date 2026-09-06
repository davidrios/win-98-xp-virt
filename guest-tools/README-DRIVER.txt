2ksbox paravirtual display adapter driver (d3dpt-vga), Windows 2000/XP.

Boot XP with:  -vga none -device d3dpt-vga
Install (as Administrator):  DRVINST.EXE -reboot
  (or the guest-tools ISO's SETUP.EXE, which runs the same installer;
   or Device Manager > Video Controller (VGA Compatible) > Update Driver
   > Install from a specific location > this folder)
Then Display Properties offers the host's mode table (640x480 ..
1600x1200, 8/16/32 bpp, 60/75/85 Hz; 8 bpp is palettized); the desktop
lives in the adapter's VRAM and the player shows it without copies.

The driver:
  D3DPTVID.SYS   video miniport
  D3DPTDISP.DLL  display driver
  D3DPTVID.INF   the INF the two above are installed from
  DRVINST.EXE    scripted installer: sets the driver-signing policy to
                 ignore, UpdateDriverForPlugAndPlayDevices, optional
                 reboot with -reboot

Its test programs, which only mean anything with this driver installed:
  SETMODE.EXE    lists the modes; SETMODE 1024 768 32 85 switches and
                 saves
  DDTEST.EXE     DirectDraw 7: HAL caps, exclusive flip chain,
                 Lock/Blt/Flip, fps, and at 8 bpp a palette on the
                 primary rotated every frame. DDTEST [w h bpp] [frames]
                 [-windowed]; log in ddtest.log
  D3D7TEST.EXE   Direct3D 7 through the HAL: device enumeration, Z
                 buffer, texture, the reference scene, fps; d3d7test.log
                 and d3d7test.bmp
  DITEST.EXE     DirectInput keyboard under load: DITEST [seconds]
                 [busy-ms] [-window] [-nonexcl]; what DirectInput,
                 GetAsyncKeyState and WM_KEYDOWN each see of the keys
  DXTTEST.EXE    Direct3D 8 texture formats: CheckDeviceFormat,
                 CreateTexture, Lock, a textured quad for every format x
                 pool, CreateImageSurface; every HRESULT in dxttest.log
                 and the driver's surface lines in the QEMU log
  SHTEST.EXE     vertex / pixel shaders 1.x through d3d8.dll with
                 hardware vertex processing: vs 1.1 through a
                 declaration and its constants, from user memory and
                 from a vertex + index buffer, a declaration-only
                 shader, D3DVSD_CONST, ps 1.1 with a constant and with a
                 texture, the FVF path again; every draw read back and
                 compared, shtest.log ends with "shtest: N cases, M
                 failed"
  CKTEST.EXE     Direct3D 7: an 8-bit palettized texture with its own
                 palette, SetEntries changing it, a colour-keyed R5G6B5
                 texture with COLORKEYENABLE on and off; every draw read
                 back, cktest.log ends with "cktest: N cases, M failed"
  EBTEST.EXE     the DirectX 3 path a 1997 title takes: IDirect3D v1 on
                 the back buffer, the viewport's Clear through a
                 background material, execute buffers with
                 PROCESSVERTICES_COPY / _TRANSFORM and D3DOP_TRIANGLE,
                 textures loaded with IDirect3DTexture::Load and bound by
                 TEXTUREHANDLE, a colour-keyed one; every case read back,
                 ebtest.log ends with "ebtest: N cases, M failed"
