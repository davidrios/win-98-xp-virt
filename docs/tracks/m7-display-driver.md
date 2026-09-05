# Track: M7 — the XP display driver (doc 15, ADR-008)

The handoff for a session that works on the real XP display driver: the
`d3dpt-vga` adapter in QEMU, the miniport / display driver pair, its ISO
folder and its tests. Read `docs/00-status.md` first for the global
picture and the track rules, then this file, then doc 15.

## Scope and files (this track owns them)

- QEMU device: `d3dpt/hw/d3dpt_vga.c`, register set `d3dpt/d3dpt_fb.h`
  (bump `D3DPT_FB_VERSION` on any change; device and miniport check it),
  the shared executor loader `d3dpt/hw/d3dpt_exec_load.[ch]`.
- Guest driver: `guest-tools/src/d3dptvid/` (miniport `d3dptvid.c`, display
  driver `d3dptdisp.c`, `kcrt.c`, INF, `drvinst.c`, `setmode.c`, `ddtest.c`,
  `d3d7test.c`, vendored DDK headers `ddk/` incl. the self-contained
  `d3dnthal.h`), `guest-tools/build-driver.sh` (also run by
  `build-wrappers.sh`, stages `DRIVER\` on the ISO).
- Executor, M7c's half: `d3dpt/exec/d3dpt_exec_ddi.cpp` (the display
  driver's records: VRAM surfaces, contexts, the DP2 interpreter,
  readback) and `d3dpt/exec/d3dpt_exec_int.h` (state shared with the d3d9
  half); `tools/d3dpt-dp2-test.cpp`.
- Tests: `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` + `tools/xp-fifa2000.bat`,
  `tools/xp-diablo.sh`, `tools/qmpc.py` key map additions.
- Docs: `docs/15-guest-display-driver.md`, this file, the M7 row of the
  state table and the M7 line of "Next steps" in `docs/00-status.md`.
- Shared with the M4 track (rebase first, edit minimally, say so in the
  commit): `d3dpt/d3dpt_proto.h` (the M7c records live in it; bump the
  version), `d3dpt/exec/d3dpt_exec.cpp` + `d3dpt_exec.h`, `d3dpt/hw/d3dpt_mm.c`,
  `scripts/test.sh` (the `d3dpt-dp2` host check is M7's; a driver guest
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
- **M7c first cut done (2026-09-04, branch `track/m7-d3d-ddi`):** the
  DX7 non-T&L HAL. VRAM 128 MiB with the top 64 MiB as the command window
  (register set v2: CMD_OFFSET, DOORBELL, D3D_STATUS), the executor shared
  with the SysBus device, surfaces mirrored from VRAM by handle, contexts,
  the DP2 token interpreter on DXVK, readback into VRAM at EndScene / Lock
  / Flip. `D3D7TEST` on XP (KVM): HAL device enumerated, Z buffer, texture,
  the reference scene at 640×480×32 renders at 2400 fps and its
  frame is byte-identical to the host-side
  `d3dpt-dp2-test` frame of the same DP2 tokens.
- **FIFA 2000 renders on the HAL (2026-09-04, headless):** the game's
  DX6 Thrash renderer (`THRASH\dx6z.dll`) unmodified, with the WineD3D
  DLLs renamed out of its folder: intro at 640×480×16, title screen,
  attract-mode match at 800×600×16 with textures, kits, crowd, HUD
  (doc 15 "FIFA 2000 on the HAL"; screendumps in
  `build/xp-driver-test/fifa/`). No unsupported token, no refused record,
  no colour keying requested; the match blits instead of flipping. The
  image is `~/vms/winxp-m7f.qcow2` (a copy of the user's `winxp-m7` with
  the M7c driver reinstalled and the DLLs renamed).
- **Played by hand (user, 2026-09-04): graphics clean, smooth; the
  keyboard dead in the match under TCG.** Root cause and fix in doc 15
  ("FIFA 2000 on the HAL"): the game's non-exclusive DirectInput keyboard
  never updates in the match because its thread stops pumping messages;
  `D3DPT\DINPUT.DLL` next to the EXE merges `GetAsyncKeyState` into the
  state and logs the game's DirectInput use. **The user confirmed the fix
  2026-09-05.** Decided with it (doc 15): the merge stays a per-game,
  side-by-side DLL — not `system32`, not `AppInit_DLLs` — and the launcher
  (M6) gets the job of staging it; the shim is now silent by default, with
  the log and the sampler thread behind `D3DPT_DINPUT_LOG=1`. Tools that came out of it:
  `DRIVER\DITEST.EXE`, the `qemu-embed: input:` statistics in the embed
  library, `PLAYER_KEYS_HOLD`, the executor's `frames/s` line,
  `xp-driver-test.sh`'s `bat` / `GAME_ISO` / `SHOTS` / `SHOT_KEYS`,
  `qmpc.py click`.
- **8 bpp palettized modes (2026-09-04 night, register set v3): Diablo
  plays.** Device PALETTE block + c8 shadow, miniport palette-driven modes
  + `SET_COLOR_REGISTERS`, GDI `GCAPS_PALMANAGED` / `DrvSetPalette`,
  DirectDraw `DDPF_PALETTEINDEXED8`; two XP runtime rules found by
  disassembly (doc 15 "8 bpp palettized modes"): `dwPalCaps` must be 0
  and no palette callbacks (dxg drops the HAL otherwise; palettes go
  through GDI on NT), and the HAL must keep offering Direct3D in every
  mode (ddraw.dll fails a mode switch to a PDEV without it). `DDTEST 640
  480 8` (palette rotated every frame, 1200 fps), `tools/xp-diablo.sh
  install|play <image>` (installer, intro, menus, Tristram, screendumps).
  `winxp-m7g` has Diablo installed at `C:\Diablo`; it and the user's
  `winxp-m7` carry the v3 driver (installed 2026-09-05 00:50); `m7f` still
  has v2, which refuses the v3 device: reinstall from the ISO first.
- **The driver is the user's daily driver, on a stock guest (2026-09-05).**
  They run the Linux host natively (KVM) with `-vga none -device d3dpt-vga`
  on `~/vms/winxp-m7` (v3 driver) and report it working great, with **no
  input issues and no custom DLLs anywhere** — no WineD3D set to rename out
  of a game folder, no `D3DPT\DINPUT.DLL`, nothing next to any EXE. That is
  the bar the driver now meets: an unmodified XP talking to the adapter
  through our miniport and display DLL, and unmodified games on the HAL.
  It also scopes the keyboard shim: the dead match keyboard is a **TCG-only**
  symptom (doc 15 — the game's unpumped DirectInput hook only loses the race
  when the guest is slow enough), so `DINPUT.DLL` is medicine for the Apple
  Silicon path and for `xp-fifa-match.sh tcg`, not something in anyone's
  normal path on a KVM host. One more reason it stays per-game rather than
  system-wide.
- Branch history: `worktree-luminous-dancing-cocke` (merged into main
  2026-09-04), `track/m7-d3d-ddi` (M7c, merged into main 2026-09-04),
  `track/m7-fifa` (FIFA on the HAL + the keyboard fix, merged into main
  2026-09-04 evening; the 8 bpp work continues on it). New work: branch
  `track/m7-<topic>` off main.
- **Resuming here (2026-09-04 evening):** pull main, then prepare →
  configure → ninja (the embed library carries the input statistics),
  `scripts/build-d3dpt-exec.sh` (the frames/s line; main also has the M4
  track's executor changes of the same day), `guest-tools/build-driver.sh`
  (DITEST) and `guest-tools/build-wrappers.sh` (the DINPUT shim into
  `guest-tools/out/iso/D3DPT/`), `cargo build --release`. Images on this
  box: `winxp-m7f` and `winxp-m7g` (copies of the user's `winxp-m7`, M7c
  driver, WineD3D DLLs renamed, `DINPUT.DLL` already in the FIFA folder,
  `m7g` also has `LowLevelHooksTimeout` = 5000 which proved irrelevant);
  `tools/xp-fifa-match.sh kvm ~/vms/winxp-m7g.qcow2` is the check that the
  match takes keys (pause menu on Esc). The keyboard question is answered
  — see the daily-driver bullet below; M7c item 1's keyboard half is
  closed and the next item is whatever the next title asks for.

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
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 d3d7      # D3D7TEST: the DX7 HAL scene, diffed against build/d3dpt-dp2-test's frame
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 cmd 'D:\DRIVER\DDTEST.EXE 800 600 32 300'
# a game: its disc as D: (the driver ISO moves to F:), a batch file staged as E:\RUN.BAT, a screendump every 5 s
GAME_ISO=/mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO SHOTS=24 tools/xp-driver-test.sh ~/vms/winxp-m7f.qcow2 bat tools/xp-fifa2000.bat
# a real match, driven over QMP (menus, side, kickoff) and a keyboard test in it (F2 / Esc / F12 taps + screendumps),
# dinput_log.txt pulled from the image at the end; ~6 min under kvm, ~9 under tcg
tools/xp-fifa-match.sh kvm ~/vms/winxp-m7g.qcow2      # or tcg
# Diablo, the 8 bpp palettized title: installer (once per image), then the game into Tristram with screendumps (~2 min)
tools/xp-diablo.sh install ~/vms/winxp-m7g.qcow2 && tools/xp-diablo.sh play ~/vms/winxp-m7g.qcow2
# the same game in the player, by hand (the image above: driver reinstalled, WineD3D DLLs renamed)
target/release/player -- -L $PWD/qemu/pc-bios -accel kvm -cpu host -machine pc -m 512 \
  -hda ~/vms/winxp-m7f.qcow2 -cdrom /mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO -vga none -device d3dpt-vga \
  -net none -usb -device usb-tablet 2>&1 | tee fifa-player.log     # then Start menu → EA SPORTS → FIFA 2000
build/d3dpt-dp2-test x.bmp                              # the same scene through the executor without a guest (host stage of scripts/test.sh)
```

- The executor: `scripts/build-d3dpt-exec.sh` after touching `d3dpt/exec/`
  (both `.cpp` files); the device dlopens `build/d3dpt/libd3dpt_exec.so`
  (`D3DPT_EXEC_LIB`) and DXVK from `build/dxvk` (`D3DPT_DXVK_LIB`). A
  worktree without `build/dxvk` can symlink the main checkout's.

- Outputs in `build/xp-driver-test/`: screendumps, the guest logs, and
  the QEMU log whose `d3dpt-vga: guest: …` lines are the driver's debug
  output (DEBUG register) and whose `scanout offset` lines are page flips.
- Images (Linux box): `~/vms/winxp-m7.qcow2` is the user's own (driver
  installed; never open it while their player holds it), `winxp-m7c` /
  `winxp-m7d` are scratch copies with the driver installed;
  `~/vms/winxp.qcow2` belongs to the M4 track (cirrus, untouched).
  Reinstall the driver (`install`) after every driver rebuild: XP loads the
  copy in `system32`, not the ISO's.
- Bisection without reinstall: `-device d3dpt-vga,ddflags=N` (doc 15);
  `0x20` = Direct3D off. The QEMU log's `d3dpt-vga: ddi: …` lines are the
  executor's (unsupported states / tokens, once each), `batch N: error` a
  refused record, `d3dptdisp: dp2 0x…` a DrawPrimitives2 the host failed.

## Next steps, in order

1. **M7c, the rest:** FIFA 2000 plays, keyboard included — the user
   confirmed `D3DPT\DINPUT.DLL` fixes it (2026-09-05), and the shim stays
   per-game by decision (doc 15). Next: what it or the next title asks for
   first among: colour
   keying (key → alpha at upload + alpha test), claiming T&L
   (`D3DDEVCAPS_HWTRANSFORMANDLIGHT`, the tokens are already mapped),
   render-to-texture, state sets, the DX8 tokens + `GUID_D3DCaps`
   (`D3DCAPS8`) for DX8 games without the DLL, presenting the host frame
   through the player's 3D path instead of the per-frame readback copy.
2. Add a `driver` stage to `scripts/test.sh` (boot on `d3dpt-vga`, `modes`
   + `ddtest` with expected numbers) once the M4 track's suite structure
   is stable; until then `tools/xp-driver-test.sh` is the check.
3. Small items: `DrvDeriveSurface` (GDI on DirectDraw surfaces), a real
   vblank signal from the player's present, the hardware cursor (needs a
   sprite in the player), the mode table fed from the player (M2), a macOS
   run of the same image, more 8 bpp titles (StarCraft, Age of Empires,
   Caesar 3: install + a `tools/xp-<game>.sh` each; Diablo's dungeon
   levels), a RAM-backed palette page if a title animates the palette
   faster than the MMIO writes allow.

## Gotchas of this track (details in doc 15)

- Kernel-mode with mingw-w64: miniport headers are `ntdef.h` +
  `ddk/miniport.h` + `ntddvdeo.h` + `ddk/video.h`, never `ntddk.h`; the
  display DLL takes `winddi.h` with the vendored `ddk/ddrawint.h`; GCC calls
  `memcpy`/`memset` even freestanding (`kcrt.c`); `build-driver.sh` checks
  the import lists (videoprt + the few ntoskrnl imports of the cached
  mappings; win32k only for the DLL).
- `EngModifySurface` needs `HOOK_SYNCHRONIZE`; `DDCAPS_GDI` in the caps
  makes dxg drop the HAL; the register BAR must be a kernel mapping.
- Direct3D: a device with `DRAWPRIMITIVES2EX` caps must answer
  `GUID_Miscellaneous2Callbacks` *with* `GetDriverState`, or ddraw.dll
  builds a HEL-only object (`DDCAPS_NOHARDWARE`, no error anywhere);
  `DDBD_32` is 0x100 (the DDBD flags count down); `ddraw.dll` and
  `dxg.sys` can be pulled out of the image (`qemu-img convert` + `7z x`)
  and disassembled with `i686-w64-mingw32-objdump` when the DDK docs run
  out (doc 15 has the two findings).
- XP SP3's Logo dialog ignores every registry policy; DRVINST presses it.
- Keys typed while a full-screen DirectDraw window is up are lost: one
  chained `cmd /k a & b & c` line per test — and the Run dialog truncates
  long lines silently (a `copy … E:\dd32.log` became `E:\dd`): anything
  longer than a short chain goes through a staged `E:\RUN.BAT`.
- Palettized DirectDraw on XP: `dwPalCaps` 0, no palette callbacks, and
  Direct3D offered in every mode, or the HAL silently degrades to
  `DDCAPS_NOHARDWARE` (doc 15 has the disassembly trail).
- The debugger is the DEBUG register → QEMU log. No WinDbg, no serial KD.
- Two harness bugs found 2026-09-05 while re-checking the keyboard fix, both
  of which make a *tool* failure look like a *driver* failure:
  `xp-fifa-match.sh` looked for the shim as `dinput.dll` while
  `build-wrappers.sh` upper-cases the whole staged tree for 8.3, so after any
  real build it staged nothing (`set -u`, no `-e`: silently) and the match
  ignored every key; and its wait for the game's mode switch was an unbounded
  `until`, so a QEMU that never started span for ever. Both fixed: the shim is
  looked up under either name and its absence is a loud warning, and the wait
  is bounded and prints `qemu.log` when it gives up.
- Only one process may hold a guest image: QEMU takes a write lock and the
  second one dies with `Failed to get "write" lock`. With tracks running in
  parallel, check `pgrep -af qemu-system-i386` for the image before starting a
  headless run — `winxp-m7g` in particular is shared by the FIFA and Max Payne
  loops.
