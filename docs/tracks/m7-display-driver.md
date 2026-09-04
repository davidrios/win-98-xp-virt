# Track: M7 — the XP display driver (doc 15, ADR-008)

The handoff for a session that works on the real XP display driver: the
`d3dpt-vga` adapter in QEMU, the miniport / display driver pair, its ISO
folder and its tests. Read `docs/00-status.md` first for the global
picture and the track rules, then this file, then doc 15.

## Scope and files (this track owns them)

- QEMU device: `d3dpt/hw/d3dpt_vga.c`, register set `d3dpt/d3dpt_fb.h`
  (bump `D3DPT_FB_VERSION` on any change; device and miniport check it).
- Guest driver: `guest-tools/src/d3dptvid/` (miniport `d3dptvid.c`, display
  driver `d3dptdisp.c`, `kcrt.c`, INF, `drvinst.c`, `setmode.c`, `ddtest.c`,
  vendored DDK headers `ddk/`), `guest-tools/build-driver.sh` (also run by
  `build-wrappers.sh`, stages `DRIVER\` on the ISO).
- Tests: `tools/xp-driver-test.sh`, `tools/qmpc.py` key map additions.
- Docs: `docs/15-guest-display-driver.md`, this file, the M7 row of the
  state table and the M7 line of "Next steps" in `docs/00-status.md`.
- Shared with the M4 track (rebase first, edit minimally, say so in the
  commit): `d3dpt/d3dpt_proto.h` and `d3dpt/exec/` (M7c reuses the
  executor and will add records), `d3dpt/hw/d3dpt_mm.c` (the executor
  loader moves to a shared helper for M7c), `scripts/test.sh` (a driver
  stage is still to be added), `player/`, `CLAUDE.md`.

## State (2026-09-04)

- **M7a done:** `-vga none -device d3dpt-vga` (+ `-accel kvm -cpu host` on
  Linux), the host's mode table (42 modes) in Display Properties, desktop
  straight from VRAM with no copy inside QEMU, no flash on mode switches,
  unattended install (`DRIVER\DRVINST.EXE -reboot`), clean restart /
  power-down paths.
- **M7b first cut done:** DirectDraw HAL accepted by XP's dxg, surfaces in
  VRAM, real page flips (OFFSET register), cached VRAM mappings; DDTEST:
  640×480×16 exclusive chain 4762 fps, ×32 6383 fps, windowed HEL blit
  305 fps (doc 15 has the table and the bisection findings).
- **M7c (Direct3D DDI): designed in doc 15, not started.**
- Branch history: `worktree-luminous-dancing-cocke` (merged into main
  2026-09-04). New work: branch `track/m7-<topic>` off main.

## Build / run / test

```sh
# after checkout or pull (the device lives in the d3dpt overlay: prepare is mandatory)
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so && cargo build --release
guest-tools/build-driver.sh                     # -> guest-tools/out/d3dpt-driver.iso (DRIVER\ only, fast)
# XP in the player on the driver (install once per image from the ISO first)
target/release/player -- -L $PWD/qemu/pc-bios -accel kvm -cpu host -machine pc -m 512 \
  -hda ~/vms/winxp-m7.qcow2 -cdrom guest-tools/out/d3dpt-driver.iso -vga none -device d3dpt-vga \
  -net none -usb -device usb-tablet
# headless loops (standalone QEMU, KVM, QMP-typed commands, logs pulled through a FAT scratch disk)
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 install   # DRVINST from the ISO, reboot, desktop on the driver
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 ddtest    # DirectDraw: caps, flip chains, windowed blit, fps
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 modes     # SETMODE switches + the mode list
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 cmd 'D:\DRIVER\DDTEST.EXE 800 600 32 300'
```

- Outputs in `build/xp-driver-test/`: screendumps, the guest logs, and
  the QEMU log whose `d3dpt-vga: guest: …` lines are the driver's debug
  output (DEBUG register) and whose `scanout offset` lines are page flips.
- Images (Linux box): `~/vms/winxp-m7.qcow2` is the user's own (driver
  installed; never open it while their player holds it), `winxp-m7c` /
  `winxp-m7d` are scratch copies with the driver installed;
  `~/vms/winxp.qcow2` belongs to the M4 track (cirrus, untouched).
  Reinstall the driver (`install`) after every driver rebuild: XP loads the
  copy in `system32`, not the ISO's.
- Bisection without reinstall: `-device d3dpt-vga,ddflags=N` (doc 15).

## Next steps, in order

1. **M7c — the Direct3D DDI**, design in doc 15 §"M7c": grow BAR 0 to
   128 MiB with the top 64 MiB as the command window (SysBus layout, so
   `d3dpt_enc.h` and `d3dpt_exec_submit` apply unchanged), a DOORBELL
   register, the executor loader shared between `d3dpt_mm.c` and
   `d3dpt_vga.c`, `d3dpt_exec_set_vram`; DX7 caps + `ContextCreate` +
   `DrawPrimitives2` forwarding in the display driver; the DP2 token
   interpreter (non-T&L subset first) and VRAM-resident textures / render
   targets in the executor; `D3D7TEST.EXE`; then FIFA 2000 (doc 00 known
   issues has its history).
2. Add a `driver` stage to `scripts/test.sh` (boot on `d3dpt-vga`, `modes`
   + `ddtest` with expected numbers) once the M4 track's suite structure
   is stable; until then `tools/xp-driver-test.sh` is the check.
3. Small items: `DrvDeriveSurface` (GDI on DirectDraw surfaces), a real
   vblank signal from the player's present, the hardware cursor (needs a
   sprite in the player), the mode table fed from the player (M2), a macOS
   run of the same image, a 2D DirectDraw title.

## Gotchas of this track (details in doc 15)

- Kernel-mode with mingw-w64: miniport headers are `ntdef.h` +
  `ddk/miniport.h` + `ntddvdeo.h` + `ddk/video.h`, never `ntddk.h`; the
  display DLL takes `winddi.h` with the vendored `ddk/ddrawint.h`; GCC calls
  `memcpy`/`memset` even freestanding (`kcrt.c`); `build-driver.sh` checks
  the import lists (videoprt + the few ntoskrnl imports of the cached
  mappings; win32k only for the DLL).
- `EngModifySurface` needs `HOOK_SYNCHRONIZE`; `DDCAPS_GDI` in the caps
  makes dxg drop the HAL; the register BAR must be a kernel mapping.
- XP SP3's Logo dialog ignores every registry policy; DRVINST presses it.
- Keys typed while a full-screen DirectDraw window is up are lost: one
  chained `cmd /k a & b & c` line per test.
- The debugger is the DEBUG register → QEMU log. No WinDbg, no serial KD.
