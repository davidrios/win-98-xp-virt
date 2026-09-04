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

## State (2026-09-04)

P0–P4 closed: the device, both DLLs and the three test programs are
byte-exact against the native DXVK build on Linux (details and numbers in
the M4 row of `docs/00-status.md` and in doc 14). `scripts/test.sh all`
is green (13 checks, ~2 min under KVM). First games tried by the user on
2026-09-04 (status doc, M4 row, has the detail): Vice City's DirectDraw
video-memory check is answered by the new `D3DPT\DDRAW.DLL` shim, the
16-bit-only mode list came from the Cirrus driver's 24-bit modes and is
fixed, GetRasterStatus and the gamma ramp are implemented; Max Payne
still freezes on level load and needs its host log.

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

1. **A real game on the device.** Max Payne (D3D8) and GTA: Vice City
   (D3D8) are installed on the user's XP image; copy `D3DPT\D3D8.DLL`
   (plus `DDRAW.DLL` for Vice City) next to the EXE (never together with
   WineD3D's), run, read the host log: every unimplemented method prints `not implemented` once
   (`D3DPT_STUB`). Known stubs a game may hit: volume textures, swap-chain
   objects, GetFrontBuffer, ProcessVertices, LockRect on DEFAULT-pool
   surfaces, the lost-device protocol, palettes.
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
