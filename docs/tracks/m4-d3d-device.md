# Track: M4 — the paravirtual Direct3D device (doc 14, ADR-006/007)

The handoff for a session that works on the DLL-based Direct3D 8/9 path:
the SysBus `d3dpt` device, the decoder / executor over DXVK, the guest
`d3d9.dll` / `d3d8.dll`, their reference workloads and the regression
suite. Read `docs/00-status.md` first for the global picture and the
track rules, then this file, then doc 14.

## Scope and files (this track owns them)

- Protocol and host side: `d3dpt/d3dpt_proto.h` (bump
  `D3DPT_PROTO_VERSION` on any wire change, then prepare + ninja QEMU),
  `d3dpt/d3dpt_enc.h`, `d3dpt/exec/` (`libd3dpt_exec`),
  `d3dpt/hw/d3dpt_mm.c` (+ QEMU patch 40), `embed/embedfx.c` presenter.
- Guest DLLs: `guest-tools/src/d3dpt/` (`d3d9.c`, `d3d8.c`, headers,
  vtable generators), the test programs `guest-tools/src/d3d9test.c`,
  `d3dfeat9.c`, `d3dgame9.c`, `d3dgame8.c`, and `guest-tools/build-wrappers.sh`
  (the big ISO; it also runs the M7 track's `build-driver.sh`).
- DXVK: `third_party/dxvk` + `patches/dxvk/`, `scripts/*dxvk*`,
  `scripts/build-d3dpt-exec.sh`.
- Tests: `scripts/test.sh` (host + guest stages), `tools/d3dpt-exec-test.cpp`,
  `tools/d3dgame9-native.cpp`, `tools/d3dfeat9-native.cpp`, `tools/bmpdiff.py`,
  the goldens in `reference/d3d/`.
- Docs: `docs/14-d3d-paravirt.md`, this file, the M4 and Tests rows of the
  state table and the M4 lines of "Next steps" in `docs/00-status.md`.
- Shared with the M7 track (rebase first, edit minimally, say so in the
  commit): `d3dpt/d3dpt_proto.h` and `d3dpt/exec/` (M7c adds records and a
  VRAM pointer), `d3dpt/hw/d3dpt_mm.c` (the executor loader becomes a
  shared helper), `scripts/test.sh`, `player/`, `CLAUDE.md`.

## State (2026-09-04, night)

P0–P4 closed: the device, both DLLs and the three test programs are
byte-exact against the native DXVK build on Linux (details and numbers in
the M4 row of `docs/00-status.md` and in doc 14). `scripts/test.sh all`
is green (13 checks, ~2 min under KVM). First games tried by the user on
2026-09-04 (status doc, M4 row, has the detail): Vice City's DirectDraw
video-memory check is answered by the new `D3DPT\DDRAW.DLL` shim, the
16-bit-only mode list came from the Cirrus driver's 24-bit modes and is
fixed, GetRasterStatus and the gamma ramp are implemented. From the first
player log (same day, evening): Max Payne's 32-bit "requires a DirectX 8
compatible display adapter" was its D3DFMT_D32 auto depth buffer meeting
DXVK's refusal of D32 — fixed by `depth_norm()` in the executor
(D32→D24X8, D15S1/D24X4S4→D24S8; `d3dpt-exec-test` now requests D32 so
it stays covered). Vice City's silent exit was then explained by the
user: they had deleted `D3D8.DLL` from the game folder believing VC is a
D3D9 title — it is D3D8 (all RenderWare GTAs through VC; San Andreas is
the D3D9 one), so the game ran on XP's stock d3d8 over Cirrus, exactly
the environment it cannot start in. The retest (same evening) gave "Max Payne
opens in 32-bit, same freeze during load; Vice City freezes the PC
showing the desktop". Both diagnosed headless the same night (details in
the M4 row of `docs/00-status.md`): neither was a hang — both games were
in a **message box behind their fullscreen window** that the player
could not show. Max Payne's box is a corrupt-JPEG error from its
CPUID-dispatched decoder under KVM `-cpu host`; with `-cpu pentium3`
(KVM or TCG) the tutorial level loads and plays on the device. Vice
City's box was a real bug of ours: the D3D8 wrapper objects had no
identity and died with the game's last Release, while real D3D8 keeps
device-/texture-owned objects alive at ref 0 — fixed (`w8_new`,
`surf_Release`/`res_addref`). Plus: the DLL forwards to the system DLL
when it cannot open the device (Vice City's process loads both d3d8 and
d3d9), the player shows the VGA surface again when 3D frames stop and
the guest draws (dialogs, movies, dead processes), and the diagnostics
below exist now. `scripts/test.sh all` green (14 checks).

## Diagnosing a game (the loop that found the above)

```sh
D=/path/to/discs
CDS="$D/DINO-MAP.iso:$D/FLT-VCB.iso01.iso:guest-tools/out/guest-tools-3dfx-d00e858.iso" \
  FRESH_DLLS=1 TRACE=1 SHOTS=10 DUMP_EVERY=60 DRW_AFTER=90 CPU=pentium3 \
  tools/xp-game-test.sh ~/vms/winxp-m7.qcow2 'C:\Arquivos de programas\Max Payne' MaxPayne.exe
```

Discs go on the same IDE slots as under the player (letters match), the
stick gets `RUN.BAT` and receives the game folder's logs, Dr. Watson's
report and minidump. `SHOTS` catches launchers and error boxes on the VGA
surface, `DUMP_EVERY` the frames the game presents, `KEYS=8:ret,25:esc`
drives menus, `DRW_AFTER` gives every thread's stack, `PAGEHEAP=1` makes a
heap overrun fault where it happens, `stacks <drwtsn32.log>` re-reads a
report. In the player: `D3DPT_DUMP_DIR`/`D3DPT_DUMP_EVERY` for frames,
`d3dpt_trace.on` next to the EXE for the DLL's call trace.

## Build / run / test

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so && cargo build --release
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
guest-tools/build-wrappers.sh                      # the ISO with D3DPT\ (rebuild after every guest change)
scripts/test.sh                                    # host stage (~30 s); `all` adds XP + DOS (~2 min)
```

The XP loop by hand (player + QMP, `-vga cirrus`, scratch FAT disk as E:,
ISO as D:, `d3dpt: guest: …` host log lines) is in the cheat sheet of
`docs/00-status.md`. Image: `~/vms/winxp.qcow2` (FXPTL.SYS installed, no
games yet); `scripts/test.sh` boots a `snapshot=on` view of it.

## Next steps, in order

1. **The games, by hand in the player** (`-cpu pentium3` under KVM, the
   fresh ISO's `D3DPT\*.DLL` next to both EXEs): Max Payne's tutorial
   level plays headless — is it playable (input, fps, sound)? Vice City
   reaches its main menu with the wrapper fix (`build/xp-game-test/vc-fixed/frames`)
   but the menu background is grey noise: **palettized (P8) textures are
   stubs** (`SetPaletteEntries` / `SetCurrentTexturePalette`; DXVK's D3D9
   has no P8 — expand to A8R8G8B8 on upload in the guest DLL, re-upload on
   palette change), that is the next thing to build. Every unimplemented
   method still prints `not implemented` once (`D3DPT_STUB`); Max Payne
   hit `GetFrontBuffer`. Known stubs: volume textures, swap-chain objects,
   GetFrontBuffer, ProcessVertices, LockRect on DEFAULT-pool surfaces, the
   lost-device protocol, palettes. Also worth a look: which CPU model to
   recommend (an era family that keeps the host's speed; `pentium3` works).
2. **Performance shape when a game asks for it:** zero-copy present (DXVK
   Vulkan interop → dma-buf / IOSurface ring instead of
   GetRenderTargetData), Present pacing against the player's vsync, a
   decoder thread off the vCPU. Measure first with `PLAYER_LATENCY=1`.
3. **On the Air:** build `libd3dpt_exec` there (KosmicKrisp) and run the
   same XP tests; expected byte-identical to Linux for the fixed-function
   scene.
4. **x87 real-world number:** a D3D8/9 title with and without
   `-cpu pentium3,x87-fast=off` (doc 13).

Long term this path stays the Win98 path and the executor's harness; on
XP the M7 track's driver replaces the per-game DLLs (ADR-008).
