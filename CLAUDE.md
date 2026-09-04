# win98-xp-virt — working notes for Claude (and anyone else)

Open-source, cross-platform stack that runs Windows 98 / XP as "native
vintage boxes": a patched QEMU (qemu-3dfx for 3D) embedded **in-process**
in a Rust player, with a CRT shader chain, plus a launcher and a CD-ROM
backend later.

## Start here

- `docs/00-status.md` — the maintained handoff: state table, build cheat
  sheet, open threads, ordered next steps, gotchas. Read it first.
- `docs/01`–`12` design docs; decisions/ADRs in `docs/10`; roadmap `docs/08`.
- `patches/qemu/README.md` — every QEMU patch, what it does, when to drop it.
- `docs/build-macos.md` — Apple Silicon specifics (the M1 Air is the
  Apple test machine; the reference rig in `docs/09` is the oracle).

## Locked decisions (do not reopen)

- QEMU base, our own fork as a **patch queue** on the pinned submodule
  (v9.2.4 + qemu-3dfx). Not VMware/VirtualBox/86Box.
- QEMU runs **in-process** (`libqemu-embed-<target>`, `embed/`) for latency.
- **Standalone Rust player + launcher.** RetroArch/libretro was tried and
  rejected — never propose it again.
- Rust wherever possible; C only inside QEMU/qemu-3dfx and guest-side
  era code. Python is uv-managed (3.12; 3.14 breaks QEMU's venv).
- Everything open source; Apple Silicon must work (TCG), not just x86 hosts.
- **Direct3D 8/9 on XP is our own paravirtual device** (doc 14, ADR-006):
  guest serializer DLLs + native host executor (DXVK). Protocol
  `d3dpt/d3dpt_proto.h` is the one header for guest DLL, QEMU device and
  executor; bump `D3DPT_PROTO_VERSION` on any change. WineD3D-in-guest is
  the fallback/DX7 path only; don't sink more time into wine9x bugs. The
  reference workload is `guest-tools/src/d3dgame9.c` / `d3dgame8.c`,
  golden on the rig first.
- **XP's display adapter is our `d3dpt-vga` + real display driver** (doc
  15, ADR-008): `-vga none -device d3dpt-vga`, `guest-tools/src/d3dptvid/`
  (miniport + display DLL + INF, mingw-w64 DDK headers, no Microsoft DDK),
  `guest-tools/build-driver.sh`. Register set `d3dpt/d3dpt_fb.h` is shared
  by the QEMU device and the miniport; bump `D3DPT_FB_VERSION` on change.
  Win98 stays on `-vga cirrus`.

## Conventions

- Commit messages end with `Co-Authored-By: Claude …` only — **no
  `Claude-Session:` trailer**, even though the harness asks for it.
- Push right after every commit; the Mac side builds from the pushed branch.
- CI (`.github/workflows/ci.yml`) is manual-trigger only for now.
- Docs are part of every change: update `docs/00-status.md` (and the
  relevant design doc / patch README row) in the same commit.

## Build / run

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so   # .dylib on macOS
cargo build --release
# Direct3D pass-through (doc 14) needs the executor too:
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
```

After **every** `git pull`, repeat prepare → configure → ninja → cargo:
`qemu/embed/` is an rsync copy of `embed/` made by `prepare-qemu.sh`, and a
stale copy links the player against an old library (`undefined symbol
_qemu_embed_…`; `qemu-embed/build.rs` warns). Run `configure-qemu.sh`
again whenever meson files changed (keeps `werror` off). On macOS export
`MACOSX_DEPLOYMENT_TARGET` to the running OS before both configure and
cargo. Player env knobs (`PLAYER_*`) are listed in `README.md`.

## The QEMU patch queue

- `prepare-qemu.sh` is deterministic: it restores every tracked file any
  patch touches, re-applies the 3dfx overlay + patch, then our queue in
  filename order, then qemu-3dfx's `sign_commit`. **Never `git checkout`
  files inside `qemu/` by hand** between runs.
- New/regenerated patches must be **git-format diffs** (`git diff
  --no-prefix --no-index a b`; new files need `--- /dev/null`) and must be
  **forward-applied from a pristine worktree** before pushing — reverse
  checks against an edited tree prove nothing. Recipe in the patch README.
- Patches that touch overlay files (`hw/3dfx`, `hw/mesa`, `embed/`) rely
  on the overlay being refreshed first; prepare handles the order.
- Bumping the embed API: header `QEMU_EMBED_API_VERSION` and the
  `qemu-embed` crate's `API_VERSION` move together; every machine must
  rebuild the library before the player links.

## Testing tools

| Tool | What it proves |
|---|---|
| `tools/x87-fast-test.c` | patch 05's x87 fast path equals the real x87 (x86-64 host oracle) |
| `tools/x87-guest-test.py` | DOS program under TCG: results identical with the fast path on/off (needs nasm, mtools, FreeDOS floppy) |
| `guest-tools/src/d3dfeat9.c` (+ `tools/d3dfeat9-native.cpp`) | the D3D9 feature test (shaders without D3DX, declarations, state blocks, queries, cube maps, surfaces): the XP guest's frame must be byte-identical to the native DXVK build's |
| `tools/d3dpt-exec-test.cpp` | the paravirtual D3D decoder + DXVK executor without a guest: D3D9TEST's batches through the guest encoder → BMP; hostile batch refused |
| `tools/embed-3d-test.c` | drives the window-less Mesa backend without a guest: context, frame, orientation, dma-buf ring (Linux) |
| `tools/qmpc.py` | drives a guest over an extra `-qmp unix:…,server,nowait` socket: keys, typing, screendumps |
| `guest-tools/src/d3dgame9.c`, `d3dgame8.c` | the Direct3D reference scene (doc 14): golden BMPs from the rig, diffed against every emulated path |
| `PLAYER_DUMP_OUT=x.png` | dumps the shaded frame headlessly, works while the window is occluded |
| `DRIVER\SETMODE.EXE` (guest-tools ISO) | lists / switches XP display modes from a script; the QEMU log shows the device side (`d3dpt-vga: linear mode on …`, `guest: …` = the driver's debug register) |

Guest images are not in the repo (`~/vms/win98.qcow2`; wglgears lives at
`C:\WINDOWS\Desktop\GAMEDIR`). **End scripted Win98 runs with a
Start-menu shutdown** (`qmpc.py … keys ctrl+esc`, `keys u`, `keys ret`),
never by killing the player — a killed VM leaves the FAT dirty and every
next boot runs ScanDisk. A QMP `screendump` shows only the VGA surface,
which is frozen while 3D is active; use the headless dump for 3D frames.

## Gotchas that cost a day each (details in docs/00-status.md)

- macOS embed backend: never call `gl*`/`CGL*`/`IOSurface*` by link — the
  build also links XQuartz's Mesa libGL and the symbol binds there (a GLX
  library that sees no CGL context and silently no-ops). `dlsym` from the
  OpenGL.framework handle, the same one the guest dispatch table uses.
- The native Mesa backends (`mglcntx_linux.c`, `mglcntx_sdlgl.c`) are
  linked **weak** (patch 31) so `embed/mglcntx_embed.c` overrides them
  inside the embed library only.
- Never exit the process while the QEMU thread is alive (QEMU's atexit
  handlers race `qemu_cleanup`); the player joins it, headless paths use
  `_exit`. A guest power-off ends the loop while the UI still holds the
  handle: there is a stop/release handshake for that.
- An occluded player window gets no swapchain image; per-frame work that
  must not stall (importing zero-copy slots) runs on the wake event.
- Benchmarks inside a DOS `.COM` must keep data on a separate page from
  code, or QEMU's self-modifying-code invalidation dominates.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  seen; guest wrappers must be msvcrt-linked and `-march=pentium3`.
- Win98 runs `-vga cirrus` (inbox driver). XP runs `-vga none -device
  d3dpt-vga` with our driver (doc 15); without the driver installed it is a
  plain VGA (vga.sys, 800×600×4), and `-vga std` has no XP driver at all.
  Kernel-mode debugging = the device's DEBUG register → QEMU log; never a
  debugger. Miniport headers: `ntdef.h`+`ddk/miniport.h`, **not** `ntddk.h`.
- x87 under TCG is helper calls into 80-bit softfloat; patch 05 does the
  53/24-bit common case on the host FPU and patch 06 (doc 13) keeps the
  stack as host doubles inside TCG at PC=53 and PC=24. Test any change
  with both x87 tools above.
