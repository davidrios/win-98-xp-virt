# 0. Status and how to resume (updated 2026-09-04, late)

Read this first in a new session. Decisions: doc 10. Plan: doc 08.

## Tracks (pick one per session)

Work runs as parallel tracks, one session each, so the handoffs stay
separate. Each track has its own doc with scope, owned files, state,
build/test loop and ordered next steps:

| Track | Doc | Owns | Next |
|---|---|---|---|
| **M4 — paravirtual Direct3D device** (DLL path, executor, tests) | `docs/tracks/m4-d3d-device.md` | `d3dpt/exec`, `d3dpt/hw/d3dpt_mm.c`, `guest-tools/src/d3dpt/`, `scripts/test.sh`, doc 14 | a real game on the device |
| **M7 — XP display driver** (`d3dpt-vga`, miniport + display DLL, DirectDraw/Direct3D DDI) | `docs/tracks/m7-display-driver.md` | `d3dpt/hw/d3dpt_vga.c`, `d3dpt/hw/d3dpt_exec_load.[ch]`, `d3dpt/d3dpt_fb.h`, `d3dpt/exec/d3dpt_exec_ddi.cpp`, `guest-tools/src/d3dptvid/`, `tools/xp-driver-test.sh`, `tools/xp-fifa2000.bat`, `tools/xp-fifa-match.sh`, `tools/xp-diablo.sh`, `tools/d3dpt-dp2-test.cpp`, doc 15 | **the DX8 DDI landed 2026-09-05** (`D3DCAPS8`, hardware T&L, the DX8 token stream rewritten by the driver, TEXBLT, state sets, render-to-texture; D3DGAME8 through XP's own d3d8.dll with hardware vertex processing); Max Payne's clipped fans fixed the same day (they are stream-0 draws into the runtime's own clip buffer; the DP2 vertex buffer is a dummy under d3d8.dll) — the alley renders complete with hardware T&L; DXT textures fixed the same night (`DdCreateSurface` sizes compressed surfaces for dxg's heap; `DRIVER\DXTTEST.EXE` probes every format × pool); **vertex / pixel shaders 1.x landed the same night (protocol v7):** vs 1.1 / ps 1.4 claimed, the shader tokens forwarded, shaders kept per context on the host (DX8 declaration → d3d9, declaration-only = fixed function, constants), every function validated against a vs/ps 1.x opcode table because DXVK asserts on garbage; `DRIVER\SHTEST.EXE` + `xp-driver-test.sh shtest` is the guest check; **palettized textures + colour keying the same night (v8):** P8 textures through the DP2 palette tokens, `DDCAPS_COLORKEY` with a never-called `Blt` callback (dxg's rule, found by bisection) → `DdSetColorKey` → the host keys texels to alpha 0 and forces the alpha test (`CKTEST.EXE` / `xp-driver-test.sh cktest`); the same probe found `DdFlip` re-registering the flip chain as if a flip swapped memory (it swaps roles on NT) — fixed; **the DirectX 3 path the same day (2026-09-05 afternoon):** Moto Racer takes the HAL with v8 and drew nothing through it — it is an execute-buffer title (`IDirect3DDevice::Execute`), a path never exercised; `DRIVER\EBTEST.EXE` + `xp-driver-test.sh ebtest` reproduce it, the `d3dim.dll` disassembly explained it (doc 15 "Execute buffers"): `dwMaxVertexCount` 65535 made every `Execute` fail with `E_OUTOFMEMORY` (now 2048), and the UNCLIPPED path is a pass-through of the raw instruction stream that the driver must partly consume and partly *bounce* back with `D3DERR_COMMAND_UNPARSED`; the DX5 texture render states mapped in the executor; EBTEST 5/5, **Moto Racer plays** (`tools/xp-motoracer.sh`: name screen, showroom, the race with keyed palms, 120 fps; not yet by hand). **open first:** a title with shaders, more streams; `ZBIAS` still dropped; more streams / cube maps as the next title demands, more 8 bpp titles (StarCraft, AoE) — the FIFA keyboard is confirmed fixed and the flip chain has a vertical blank (both 2026-09-05) |
| **M8 — CPU fast paths in TCG** (x87 shadows, SSE inline, TCG float opcodes) | `docs/tracks/m8-tcg-fastpaths.md` | `patches/qemu/05`, `06`, `11`, `12`, `tools/x87-*`, `tools/sse-guest-test.py`, `guest-tools/src/ssebench.c`, docs 13 and 16 | **merged to `main` 2026-09-04** (the SSE/SIMD patches re-sequenced after the upstream backports as `11-sse-inline-tcg` / `12-simd-inline-tcg`, `scripts/test.sh all` green on x86-64; XP `SSEBENCH.EXE` on the x86-64 box clamp+cmp 43 % of the rig vs the Air's 34 %). Air build + `scripts/test.sh all` on the merged `main` green the same day (aarch64 over the re-sequenced queue, both batteries identical). Next: item 2 of the track doc — a real Direct3D workload with and without `*-fast=off` |
| **M5 — CD-ROM backend** (libdisc, `cdimage` block driver, ATAPI/MMC, CD-DA) | `docs/tracks/m5-cdrom-backend.md` | `libdisc/`, `patches/qemu/50..59`, `tools/atapi-guest-test.py`, `guest-tools/src/cdtest.c`, docs 05 and 17 | steps 1–6 landed 2026-09-04 (model, cue/bin + CCD + MDS + ISO, EDC/ECC, Q, MMC responders, C API, `discx`, the `cdimage` block driver + patch 50, patch 51 = ATAPI + CD-DA from the model, DOS + XP guest tests incl. the tone through MCI into a wav, real dumps copied and scanned clean); next: Win98 CD Player by ear, a SafeDisc / SecuROM dump (steps 7–8), M5f with M6 |
| Everything else (M3 Glide/fences, M2, M6, macOS bring-up) | this doc's "Next steps" | — | as listed |

Rules: branch `track/<name>-<topic>` off `main`, rebase on `main` before
pushing, merge to `main` when green. Shared files (`d3dpt/d3dpt_proto.h`,
`d3dpt/exec/`, `scripts/test.sh`, `player/`, `CLAUDE.md`, this doc) are
edited minimally and the commit message says which track. In this doc a
track edits only its own state-table row, its "Next steps" line and this
table; everything else about the track lives in its track doc. The Mac
side pulls `main`.

## Where things stand

| Area | State |
|---|---|
| QEMU fork | v9.2.4 + qemu-3dfx (d00e858) + our queue (`patches/qemu/README.md`). Builds on Linux x86_64 (Arch) and macOS Apple Silicon (M1 Air, macOS 26.6.2 since 2026-09-03, for KosmicKrisp / ADR-007). Windows untested. Patch 05 (2026-09-02): x87 on the host FPU at 53/24-bit precision, bit-exact vs softfloat (host oracle + in-guest on/off test), 2.2× on an x86-64 host loop; Super PI 1M on the Air 9:49 → 6:33. Patch 06 (2026-09-03, doc 13): x87 stack as host doubles in TCG, 7.4× vs softfloat on x86-64; **XP Super PI 1M on the Air 1:57, faster than the rig's real P4 1.7 (2:02)**. Patches 07/08 (2026-09-04): upstream x87 helper fixes (pseudo-NaN transcendentals, fcomi flags) and seven decoder / segment fixes from the 2025–26 fuzzing work, hand-rebased onto 05/06; suite green under KVM and TCG. Patch 09 (same day): the 10.0 repeated-string series, 12–16 % on rep movs/stos per `tools/string-bench.py` (DOS microbench, compares two QEMU binaries side by side). Upstream survey the same day: QEMU 10.x/11.x add nothing we need (11 dropped 32-bit *hosts*, not the i386 target; qemu-3dfx has no 10.x patch), staying on 9.2.4 is a decision. Patch 11 (2026-09-04, doc 16; `11-sse-inline-tcg`, numbered 07 on the track branch before the merge): SSE/SSE2 float inline on the host FPU, packed ops on the vector unit (new TCG vector float opcodes), scalar in general registers; 333,875-line on/off guest test identical on the Air; register-only bench packed 12×, scalar 3.6× over the helpers; **SSEBENCH.EXE in XP on the Air: SSE kernels 3.2–7.4×, x87 kernels 10–12× (`reference/benchmarks/README.md`)**; rig run 2026-09-04: checks bit-identical to the real P4, Air at 97–109 % of it on four of the nine kernels, 61 % packed transform, 34 % clamp+cmp, 18 % x87 C transform. **x86-64 host, 2026-09-04, clamp+cmp:** `minps`/`maxps`/`cmpps` had no native TCG vector op (unlike `fadd_vec` etc), so were synthesized from a 16-18-op total-order key transform just to feed `cmp_vec` a monotonic float ordering; new `fmin_vec`/`fmax_vec`/`fcmp_vec` opcodes map straight onto native `VMINPS`/`VMAXPS`/`VCMPPS` on x86-64 (same ISA as the guest, so −0/+0 tie-break and NaN-operand-selection come for free, only a 6-op NaN-presence check needed for the guard), aarch64 keeps the key-transform path (macros `0`, unvalidated). `tools/sse-guest-test.py` packed chain 8.1× → 10.0×; new isolated `SSEBENCHC` kernel 6.5× over the helper; 546,425 lines still bit-identical. Patch 12 (2026-09-04, `12-simd-inline-tcg`, was 08): MMX/SSE integer and permutation ops inline (`simd-fast`), 546,425-line on/off test identical, MMX chain 4× (aarch64). x86-64 host run (2026-09-04, same day): both guest batteries bit-identical; `simd_psadbw`'s gvec-vs-register-vec regression fixed, `pmulhw`/`pack*` given native x86-64 vector ops (`mulsh_vec`/`muluh_vec`, `ssnarrow_vec`/`usnarrow_vec`), MMX chain 1.4× → 1.7×; then packuswb's `env->sses_scratch` combine (a store-to-load-forwarding stall) replaced with `dup_i64_vec` + `bitsel_vec` (portable stock TCG ops, no new opcodes), MMX chain 1.7× → **2.1×**, still 546,425 lines bit-identical. aarch64 side validated on the Air the same day: both batteries bit-identical, register-only bench packed 13.8×, scalar 3.4×, MMX 3.6×, clamp+cmp 8.7× on the key-transform fallback. XP `SSEBENCH.EXE` on the x86-64 box (Ryzen 7 5700X, 2026-09-04): clamp+cmp 3.68 ns/op = 43 % of the rig (Air 34 %), scalar chain 116 %, convert 130 %, MMX blend 122 %, C normalize 99 %, normalize 89 %, packed xform 49 %, C xform 16 %; checks identical to the rig's. |
| Player (Rust, `player/`) | Boots a machine in-process via `libqemu-embed-<target>`; wgpu presentation, librashader CRT chain, keyboard/mouse, audio, QMP over a socketpair (`PLAYER_QMP`, `PLAYER_QMP_EXEC`). Audio (2026-09-04): the embed audiodev keeps a 60 ms cushion (`PLAYER_AUDIO_MS`) ahead of the host audio thread and pins the guest's audio clock to wall time, dropping a stall's backlog instead of queuing it; `qemu-embed: audio:` stderr lines count gaps and drops. Before, a 10 ms cushion with no catch-up: every main-loop stall under TCG was a gap plus permanent extra latency inside the guest. **Win98 and XP run in it on the M1 Air** with sound and tablet mouse. |
| 3D | **GL pass-through runs inside the player on Linux** (doc 12 steps 1–2, 2026-09-02): patches 30/31 + `embed/mglcntx_embed.c` (EGL surfaceless pbuffer as FBO 0, `glReadPixels` on swap) + API v4. Win98 wglgears in the player: 420 fps at 800×600 with the readback path, desktop returns on exit. Standalone `-display sdl` still works (500+ fps on the Air). **macOS too** (CGL, no drawable, FBO stand-in; `GL 2.1 Metal / Apple M1`, wglgears in the player on the Air). **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5, 2026-09-03): 575–600 fps wglgears, nothing copied per frame. **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air. Glide: no window, reported cleanly. |
| Guest tools | `guest-tools/build-wrappers.sh` builds the qemu-3dfx guest wrappers (msvcrt-linked, `-march=pentium3`, wglgears test EXE) and, since 2026-09-03, the WineD3D set from JHRobotics/wine9x (Wine 1.7.55 for 9x/XP: per-game D3D8/D3D9/WINED3D DLLs + system-wide switchers) with a D3D9 smoke test (`D3D9TEST.EXE`), the display-mode probe (`MODETEST.EXE`) and the reference workloads `D3DGAME9.EXE` / `D3DGAME8.EXE` (doc 14 P0a) into an ISO. **Rig (P4 + GeForce 6200), 2026-09-03: both run.** First-run fixes: ground triangle winding (top face was culled), shader path now applies the per-cube material (all cubes were one colour), d3dgame8 windowed swaps with COPY_VSYNC so both pace at the refresh rate by default (85 fps on the rig's monitor is vsync, `-novsync` for throughput), console output also goes to `d3dgameN.log`. **Golden captures landed 2026-09-03** (`reference/d3d/rig-2026-09-03/`, diff with `tools/bmpdiff.py`): d3dgame9 frame 300 windowed, fixed function and `-shader`. The rig's log explained the `-shader` oddity: d3dx9_36's HLSL compiler refuses ps_1_1 (X3539), so the cubes ran vs_1_1 + fixed-function pixel stage while the log claimed fixed function. **Rendering is frozen at that build** (the golden set must stay comparable; the rig stays off for now): only the log line naming the shader case and the elapsed-ms summary were fixed, no pixel changes. Mask the HUD bars (wall time) when diffing. d3dgame8 windowed with COPY_VSYNC runs at half refresh (43 fps at 85 Hz) on the GeForce driver: real behaviour, recorded. Must match the host's qemu-3dfx commit. **Win98 and XP (2026-09-03): wglgears and D3D9TEST run in the player on both** (WineD3D needs no Microsoft DX runtime in the guest; XP needs the FXPTL.SYS step first, see gotchas). XP D3D9TEST on the Air: adapter reported as "GeForce 6800" (WineD3D's GL-renderer mapping), x87 PC=24 after CreateDevice, 377–504 fps windowed 640×480. |
| Guests | Images live outside the repo: `~/vms/win98.qcow2` and `~/vms/winxp.qcow2` on both machines (the XP image was copied to the Linux box 2026-09-03; it has FXPTL.SYS installed, no d3dx9, no games yet), plus `~/vms/scratch.img` on Linux (64 MB FAT32, seen as E:, for files out of the guest). Win98 SE on the Air: installed, repaired to PCI-bus enumeration (must be an ACPI `SETUP /p j` install or repaired — doc 06/build-macos). XP on the Air: installed, boots in the player in ~30 s (same as the rig, P4 1.7); integer 1.3–2× the rig (7-Zip), x87 FP 21 % on softfloat (Super PI 1M 9:49 vs 2:02), 104 % with patch 06 (1:57) — `reference/benchmarks/`. |
| Direct3D device (M4) | **Works end to end on Linux, P0–P4 closed 2026-09-03/04** (doc 14 has the per-milestone detail and numbers). Executor: DXVK d3d9 native (ADR-007), `third_party/dxvk` + `patches/dxvk/` (01 macOS shim, 02/05 optional features, 03 portability, 04 headless WSI), verified on RADV and on the Air over KosmicKrisp (macOS 26). Transport: SysBus device `d3dpt/hw` (QEMU patch 40; register page 0xdfffe000, 64 MiB window 0xd8000000), protocol `d3dpt/d3dpt_proto.h` **v4**, decoder+executor `d3dpt/exec` → `build/d3dpt/libd3dpt_exec.so` dlopened by the device, frames through the GL frame path (`embed/embedfx.c`). Guest: `guest-tools/src/d3dpt/` — `d3d9.c` (+`d3d9_res.h`, `d3d9_p3.h`: resources, surfaces, shaders, declarations, queries, guest-side state blocks, cube maps) and `d3d8.c` (D3D8 wrappers in the same TU); vtables generated from mingw's headers (`gen_vtbl.py`, `gen_vtbl8.py`); ISO folder `D3DPT\` with D3D9.DLL, D3D8.DLL and the test EXEs. **Acceptance so far:** XP D3DGAME9 and D3DGAME8 frames on the device are byte-identical to the native DXVK build and 1089 pixels (tolerance 8) from the rig golden; D3DFEAT9 (hand-assembled SM1.1, no D3DX) byte-identical guest vs native including query results; D3D9TEST 2840 fps vs 1100 on WineD3D-in-guest. **First real games (user, 2026-09-04):** Max Payne (D3D8) starts, its resolution list had 16-bit entries only, and it freezes on the loading screen when a level starts (diagnosed 2026-09-04, see **Max Payne campaign crash** below — the game's own heap corruption on the cracked level data, not our stack). GTA Vice City refused with "cannot find enough available video memory". Fixed the same day: (1) Vice City asks DirectDraw 7 `GetAvailableVidMem`, not Direct3D, and XP's Cirrus driver answers 4 MB → `D3DPT\DDRAW.DLL`, a shim next to the EXE that forwards to the system ddraw.dll and reports 256 MB (`DDVMTEST.EXE` shows what such a check sees; `scripts/test.sh` runs it); (2) the Cirrus driver lists 24-bit modes and no 32-bit ones (`MODETEST`: 32 bpp = BADMODE), and the DLL mapped only 32 → X8R8G8B8, so EnumAdapterModes was 16-bit only; 24-bit now counts as 8888 (the switch already retried at 24 bpp), list cached and de-duplicated; (3) GetRasterStatus (60 Hz sweep from the performance counter), SetGammaRamp / GetGammaRamp (remembered, not applied) implemented in both DLLs instead of E_NOTIMPL. Gotcha: a 64-bit `%` in the DLL pulled in `libgcc_s_dw2-1.dll` and the EXE would not start; DLLs now link `-static-libgcc`. **2026-09-04 evening, from the first player log:** Max Payne in 32-bit crashed at start ("requires a DirectX 8 compatible display adapter") — it asks for a D3DFMT_D32 auto depth buffer, which DXVK's D3D9 refuses outright (`d3d9_format.cpp`: D32/D15S1/D24X4S4 "unsupported everywhere") while our `format_ok` advertises it → CreateDevice came back D3DERR_NOTAVAILABLE. Fixed host-side: `depth_norm()` in the executor maps D32→D24X8 and D15S1/D24X4S4→D24S8 in `fill_pp` and CREATE_DEPTH_STENCIL (one spot for both guest DLLs; the guest still answers GetDesc with the asked-for format), and `d3dpt-exec-test` now requests D32 auto depth so the case is regression-tested on every run. Vice City with the shim now dies silently before even Direct3DCreate8 (two attach/detach pairs in the log, no dialog) → the shim's OutputDebugString lines were invisible; it now also appends every call to `d3dpt_ddraw.log` next to the EXE (attach with EXE name, CreateEx wrapped-or-not, GetAvailableVidMem, EnumDisplayModes with mode count, SetCooperativeLevel/SetDisplayMode/CreateSurface/GetDeviceIdentifier with hr), and the d3d8/d3d9 attach log line now names the process. The silent Vice City exit was then explained: the user had deleted `D3D8.DLL` from the game folder (VC is D3D8 — all RenderWare GTAs through VC; San Andreas is the D3D9 one), so it ran on stock d3d8 over Cirrus. **The two "freezes", diagnosed headless the same night (`tools/xp-game-test.sh`, new):** neither was a hang. Both games were sitting in a **message box behind their fullscreen window** while the player showed only 3D frames (once a device existed the VGA surface was hidden, and a process that dies without `DLL_PROCESS_DETACH` never released it): the vCPU idled in HLT, QMP answered, Dr. Watson attached to the game (`drwtsn32 -p`) showed the main thread inside `MessageBoxA`. **Max Payne:** "JPEG Error — Corrupt JPEG data: 19 extraneous bytes before marker 0xd0", then "bad Huffman code", from `grphmfc.dll` while loading the level's textures. Not the disc (a 70 MB level archive copied inside the guest is byte-identical to the ISO), not memory corruption (XP's full page heap on the EXE: no fault), and gone with **`-cpu pentium3`** under KVM or TCG: the tutorial level loads and plays (Max in the alley, HUD, weapon — `build/xp-game-test/mp-p3/frames`). The game's CPUID-dispatched JPEG decoder mis-decodes under `-cpu host` (family 25). **Vice City:** its own handler's box, "Unhandled exception c0000005 at address 00000001": under page heap the fault moved to `gta-vc.exe` reading a freed `IDirect3DSurface8` (`GetDesc` on the render-target surface it had fetched and released earlier, on RwRasterCreate for the camera). Our D3D8 wrappers were one fresh object per `Get*` call and died with the game's last `Release`; real D3D8 objects owned by the device / a texture keep their identity and live on at ref 0. Fixed in `d3d8.c` (`w8_new`: one wrapper per object, kept until the object is freed) and `d3d9_res.h` (texture level surfaces persist at ref 0 while the texture lives, `res_addref` retakes the texture reference); with it **Vice City reaches its main menu on the device** (`build/xp-game-test/vc-fixed/frames`: the Vice City logo and Start Game / Options / Quit at 41k presents), the menu's background texture showing as grey noise — the next stub, 8-bit palettized textures (`SetPaletteEntries` / `SetCurrentTexturePalette`, RenderWare's P8 rasters). Also found: the game's process had loaded **both** our `D3D8.DLL` and `D3D9.DLL` (something in it asks for d3d9), and the second refused to load because the device was busy — the DLL now forwards `Direct3DCreateN` to the system DLL when it cannot open the device (`d3dpt: forwarding to C:\WINDOWS\system32\d3d9.dll`), logging the module list. **Player:** while 3D is active the VGA surface is shown again after 1 s without a presented frame when the guest drew on it (`[display] no 3D frame for 1000 ms …`); `embedfx.c` no longer sets QEMU's passthrough for the D3D device so the VGA keeps rendering. **New diagnostics:** `D3DPT_DUMP_DIR` (executor frame dumps from bare QEMU), the DLL call trace (`D3DPT_TRACE=1` / `d3dpt_trace.on` → `d3d8_trace.log`, generated wrappers in the vtable headers, args logged), the DLL forwarding fallback, and the tool itself (screendumps, frames, keys, Dr. Watson stacks, page heap, `CPU=`). Gotcha: modern mingw's `psapi.h` maps to `K32*` kernel32 exports → XP's loader blocks the process in a hard-error dialog before `DllMain`; `PSAPI_VERSION 1`. **Max Payne campaign crash (diagnosed 2026-09-04, later):** the tutorial plays but New Game → any campaign level (Fugitive first) crashes on the loading screen with the game's own XP error box (a heap corruption), and it is **not** our stack. Two signatures, both entirely in game code with no `d3dpt`/DXVK frames: without page heap a wild jump to `eip=0x53414d41` (ASCII "AMAS" where an `X_LevelDBLevel` vtable pointer belongs); with full page heap a break in `RtlFreeHeap` ← MFC42 free ← the level-init function `MaxPayne+0x4fd72` (strings "Level init"/"Global AI object"/"MP_GM_AINETINIT"/"Enemy creation"). Ruled out as ours by varying every axis: reproduces under KVM `-cpu pentium3` (real host CPU, so not our TCG x87/SSE fast paths); reproduces with `x_level1.ras` copied byte-identical (123,196,327 B) to the guest's local disk (not QEMU CD/disk emulation, not our device streaming); reproduces at 256 MB and 1 GB RAM; the tutorial (local `x_data.ras`, same device) plays through. Conclusion: the level-1 data from the cracked DINO-BYTES ISO (SafeDisc; `secdrv.sys`/`drvmgt.dll` on the disc) corrupts the game's heap during level init. Open next: try a legit / differently-cracked `x_level1.ras` for a final verdict. Tool notes: `xp-game-test.sh` CDS discs get XP letters in **reverse** of CDS order (first CD → E:, second → D:); the image's `cd.ini` holds the level path (`E:\disk1\Levels`) and the game shows its own "insert the Max Payne CD" box (not a crash) when that path lacks `Disk1\Levels`; new `PRE_CMD=` env runs a batch command in the game dir before the EXE (used to stage a level archive local / rewrite `cd.ini`). Open: volume textures, swap-chain objects, GetFrontBuffer, lockable DEFAULT surfaces, lost-device protocol, zero-copy present (readback via GetRenderTargetData today), macOS build of `libd3dpt_exec`, decoder thread (everything runs on the vCPU thread under the BQL). |
| XP display driver (M7, doc 15) | **M7a landed 2026-09-04, M7b (DirectDraw DDI) first cut the same evening; track doc `docs/tracks/m7-display-driver.md`.** `d3dpt-vga` PCI adapter (`d3dpt/hw/d3dpt_vga.c`: QEMU's stdvga core + a register BAR, `d3dpt/d3dpt_fb.h`; `-vga none -device d3dpt-vga`) and the driver pair `guest-tools/src/d3dptvid/` (video miniport, display driver, INF, `DRVINST.EXE` unattended installer, `SETMODE.EXE`, `DDTEST.EXE`), built by `guest-tools/build-driver.sh` with mingw-w64's DDK headers + ReactOS' public-domain `ddrawint.h` (`DRIVER\` on the guest-tools ISO). XP desktop from the host's mode table (42 modes, 1024×768×32@85 etc.) straight out of VRAM with no copy inside QEMU, no flash on mode switches, KVM verified. DirectDraw: HAL accepted by dxg, surfaces in VRAM, real page flips through the OFFSET register, cached VRAM mappings (miniport maps VRAM itself); DDTEST 640×480×16 flip chain 4762 fps, ×32 6383 fps, windowed HEL blit 305 fps (throughput: since the vertical blank below a flip chain runs at 60 fps, `DDFLAGS=32768` measures throughput again). Findings: `EngModifySurface` needs `HOOK_SYNCHRONIZE`, `DDCAPS_GDI` makes dxg drop the HAL, XP SP3's Logo dialog ignores every registry policy. `tools/xp-driver-test.sh` runs the guest loops headless. **M7c (Direct3D DDI) first cut 2026-09-04:** the DX7 non-T&L HAL on the doc 14 executor — VRAM 128 MiB with a 64 MiB command window on top (register set v2), surfaces mirrored from VRAM by handle, the DP2 token interpreter (`d3dpt/exec/d3dpt_exec_ddi.cpp`), readback into VRAM at EndScene / Lock / Flip; `D3D7TEST` (HAL device, Z, texture, the reference scene) at 2400 fps, its frame pixel-identical to the host-side `d3dpt-dp2-test`. **FIFA 2000 runs on the HAL unmodified (2026-09-04, headless, `tools/xp-fifa2000.bat`):** its DX6 Thrash renderer through DrawPrimitives2 — intro, title screen, attract-mode match at 800×600 with textures, kits, crowd and HUD; no unsupported token, no refused record, no colour keying asked for. Played by hand the same day: clean and smooth under KVM; the keyboard dead in the match under TCG — traced (doc 15) to the game's non-exclusive DirectInput keyboard, fed on XP by a hook that its match loop never services (Windows sees every key, the device reports none, KVM or TCG headless); fixed by `D3DPT\DINPUT.DLL` next to the EXE (a forwarding shim that merges `GetAsyncKeyState` into the keyboard state and logs the game's DirectInput use). **User-confirmed 2026-09-05 by A/B on a Linux TCG run** (keys with the DLL next to the EXE, dead keyboard again with it moved away: the shim is the variable) — and scoped the same day: the user's everyday setup is this Linux host run natively (KVM) on `-vga none -device d3dpt-vga`, where they report **no input issues and no custom DLLs anywhere** (stock XP on the driver, no WineD3D renames, no shim next to any EXE), so the unpumped-hook symptom is TCG-only and `DINPUT.DLL` is medicine for the Apple Silicon path, not the normal one. Decided with it (doc 15): the merge stays a per-game side-by-side DLL — `system32` fights Windows File Protection and cannot hold a shim of the same name, `AppInit_DLLs` would inject it into every process, and the merge is only correct where we have watched the game want it — with deployment becoming the launcher's job (M6). The shim is now silent by default (the fix alone); `D3DPT_DINPUT_LOG=1` restores the log and the sampler thread, whose 248 keys per 5 ms are not free under TCG. Tools from the hunt: `DRIVER\DITEST.EXE`, the embed library's input-queue statistics, `PLAYER_KEYS_HOLD`, the executor's `frames/s` line, `xp-driver-test.sh` `bat` / `GAME_ISO` / `SHOTS` / `SHOT_KEYS`, `qmpc.py click`. **8 bpp palettized modes (2026-09-04 night, register set v3): Diablo plays** — device PALETTE block + indexed shadow, palette-driven miniport modes, GDI palette management (`DrvSetPalette`), `DDPF_PALETTEINDEXED8`; the XP runtime wants `dwPalCaps` = 0 with no palette callbacks (palettes reach the driver through GDI) and Direct3D offered in every mode, else the HAL degrades to `DDCAPS_NOHARDWARE` (both found in the `dxg.sys` / `ddraw.dll` disassembly). `DDTEST 640 480 8` animates a palette at 1200 fps; `tools/xp-diablo.sh install\|play` gets Diablo into Tristram headless with screendumps. **The flip chain has a vertical blank (2026-09-05):** Moto Racer played at several times its speed because `DdFlip` never blocked and `DdGetFlipStatus` always said "done" — a 1997 racer is paced by its flip chain, and ours had no pace. `FRAMES` is now a clock (periods of the mode's `HZ` off the host clock, not the display client's pull, so a headless run paces like the player), `DdFlip` / `DdGetFlipStatus` hold the second flip of a double-buffered chain until it moves, bounded at 50 ms; `DDTEST`'s three exclusive chains and `D3D7TEST` all run at 60 fps (`DDFLAGS=32768` = `DDF_NO_VSYNC` restores the old numbers to the frame), the windowed `Blt` path is untouched as on real hardware, and the device prints `N page flips in 5.0 s` so a title's real frame rate — or its absence, meaning it blits — is visible in the log. **Max Payne with no wrapper DLL (2026-09-05, `tools/xp-maxpayne.bat`):** XP's d3d8.dll takes a driver without `D3DCAPS8` as a DX7 driver (software vertex processing, DX7 tokens) and the launcher, menu and tutorial level render on the HAL at ~290 fps; it exposed two executor bugs — `TRIANGLEFAN_IMM` / `LINELIST_IMM` have their payload and the next token DWORD-aligned by *offset* (the DX8 runtime emits them at offset 2 mod 4; the stream desynchronised into garbage tokens, and a half fix that aligned only the end drew one garbage fan per frame: black bands across the alley, found by the trace's per-draw render-target snapshots) and a garbage light index made DXVK throw `std::bad_alloc` that cannot be caught (DXVK's own static unwinder → abort): indices are validated before DXVK now, the host test covers both; plus the DP2 frame trace (`D3DPT_DP2_TRACE`: state snapshot, tokens, vertices, texture and per-draw target dumps), `D3DPT_DDI_REREAD`, `D3DPT_DDI_NOFOG`, the driver's surface registration log. The alley matches the M4 device's frame; `ZBIAS` (47) is still dropped. **The DirectX 8 DDI (2026-09-05, later):** `GetDriverInfo2` with `D3DCAPS8` and the DX8 format list, hardware T&L claimed in both caps sets (the executor's fixed-function mapping does the work), the DX8 token stream rewritten in the driver into self-contained draws (protocol v6 `D3DPT_DP2_DRAW8`: the runtime's vertex / index buffers are guest system memory), TEXBLT done in the driver, the DX8 state kept per context between calls, state sets as d3d9 state blocks, render-to-texture, MULTIPLYTRANSFORM, DXT pitches; D3DGAME8 runs through XP's own d3d8.dll with hardware vertex processing (~575 fps, render-to-texture and all; `xp-driver-test.sh d3dgame8` diffs it against the native oracle) FIFA / D3D7TEST keep working, Max Payne renders on it too since the clipped fans were fixed (2026-09-05, later: `CLIPPEDTRIANGLEFAN` offsets count into stream 0, which the runtime rebinds to its own clip buffer before the tokens; the DP2 call's vertex buffer under d3d8.dll is a 10 × 32-byte dummy — read from there the fans were heap garbage, the nearest walls and ground black); findings by disassembly: the HAL-info flag that unlocks the queries, `dwActualSize` checked against the inner header, no `CLIPTLVERTS` (the host does not clip TL vertices), FOURCC surfaces need the HAL info's list, DX8 state persists across calls by handle. DXT textures on this path failed in dxg's heap (a FOURCC format has no bit count, the driver had no `CreateSurface` to size it; fixed 2026-09-05 with `DdCreateSurface` + `DDHAL_PLEASEALLOC_BLOCKSIZE`, found with the new `DRIVER\DXTTEST.EXE`). **Shaders 1.x (2026-09-05 night, protocol v7):** `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)` in the caps, the CREATE / SET / DELETE / CONST tokens pass through the driver, the executor keeps them per context (the `D3DVSD_*` declaration → a d3d9 declaration with `dcl`s prepended to the function, declaration-only shaders as the fixed function on that layout, `D3DVSD_CONST` loaded at set time, a DRAW8 under a shader carries the handle) and validates every function first — DXVK's compiler *asserts* on an unknown opcode (an abort: QEMU would die), found by the host test's hostile case; `SHTEST.EXE` / `xp-driver-test.sh shtest` verifies it through XP's own d3d8.dll. **Palettized textures and colour keying (2026-09-05 night, protocol v8):** the two caps Moto Racer 1997 wanted — P8 in both format lists, `TRANSPARENCY` / `ALPHAPALETTE`, `DDCAPS_COLORKEY` + `DDCKEYCAPS_SRCBLT` with a `SetColorKey` and a never-called `Blt` callback (dxg drops the HAL for the caps without a Blt callback, and without the caps user-mode ddraw never hands a texture's key down — four CKTEST runs, doc 15), the key sent by `DdSetColorKey` and re-checked off dxg's surface at texture bind → `VRAM_COLORKEY`; the executor takes palettes from the DP2 `SETPALETTE` / `UPDATEPALETTE` tokens, expands P8 and keyed textures to A8R8G8B8 (key = alpha 0), forces the alpha test under `COLORKEYENABLE` and overrides stage 0's alpha op when the app's ignores the texture alpha (the DX7 runtime's `TEXTUREMAPBLEND` emulation does that for every keyed 16-bit texture), and re-uploads dirty bound textures before each draw; `CKTEST.EXE` / `xp-driver-test.sh cktest` verifies it through the DX7 API. **Found by CKTEST's second case:** on NT a flip exchanges the two surfaces' roles, not their memory (the handles keep their VRAM, dxg moves the PRIMARYSURFACE caps and re-issues `CreateSurfaceEx`), and `DdFlip` had re-registered them as if the memory had swapped since the first M7c cut — the host rendered into the displayed buffer every other frame; fixed (doc 15 "A flip does not move memory"). **Moto Racer on it (2026-09-05, `tools/xp-motoracer.sh`):** it takes the HAL now and turned out to be a DirectX 3 title — execute buffers through XP's `d3dim.dll`, a path with two breakages of its own, both found with the new `DRIVER\EBTEST.EXE` probe and the runtime's disassembly (doc 15 "Execute buffers — the DirectX 3 path"): `hwCaps.dwMaxVertexCount` 65535 sized the runtime's TL vertex buffer over its own 65535-vertex limit, so every `Execute` failed with `E_OUTOFMEMORY` before a token was built (2048 now, `ddflags=0x40000` the repro); and the UNCLIPPED `Execute` is a pass-through of the execute buffer's raw `D3DOP_` instructions (`D3DHALDP2_EXECUTEBUFFER`) in which the driver consumes POINT / LINE / TRIANGLE / STATERENDER / SPAN / EXIT and must *bounce* the rest — `PROCESSVERTICES` first — with `D3DERR_COMMAND_UNPARSED` + `dwErrorOffset` so the runtime executes them and calls again (skipping them leaves the TL buffer empty); the DX5 texture render states (`TEXTUREHANDLE`, `TEXTUREMAPBLEND`, filters, address) arrive verbatim on this path and the executor maps them onto stage 0 now. EBTEST passes 5/5 through `d3dim.dll` (and on the RGB control); `tools/d3dpt-dp2-test.cpp` covers the executor half. **Moto Racer plays** (`tools/xp-motoracer.sh install|play`: the 3D name screen, the showroom bike, the Speed Bay race with the colour-keyed palms and the HUD at 120 fps under KVM; ~175 one-triangle draws a frame — batching is the follow-up; not yet by hand). Not yet: a title that uses shaders, more than one stream, hardware cursor, mode table from the player (M2), a present signal in phase with the player's swapchain, macOS run, StarCraft / AoE on the 8 bpp path. |
| Tests | Integration / e2e only (CLAUDE.md policy, 2026-09-04): `scripts/test.sh all` runs the host tools (x87 oracle, embed Mesa backend, decoder + executor, the native DXVK reference scene within budget of the rig golden, the native feature test) and the guest stage (DOS x87 battery under TCG; XP headless on the D3D device from a `snapshot=on` view of `~/vms/winxp.qcow2` with a fresh scratch FAT disk and `RUN.BAT`, driven over QMP: D3DGAME9 / D3DGAME8 pixel-identical to the native frame outside the HUD, D3DFEAT9 byte-identical with the same query lines). 13 checks, ~2 min on the Linux box under KVM, all green at 2026-09-04. Local only by decision (2026-09-04): CI stays off the suite, it needs the images and a GPU. |
| CD backend (M5, libdisc) | **Track opened 2026-09-04** (`docs/tracks/m5-cdrom-backend.md`, branch `track/m5-cdrom`), spec in doc 17: a `cdimage` QEMU format block driver over libdisc's C API (cooked view through the block layer, raw model for atapi.c), MMC responders in Rust so the host exerciser (`discx`) tests the exact bytes, L-EC verified on cooked reads (the SafeDisc signal comes from the drive model, no bad-sector lists), CD-DA through an `audiodev` on `ide-cd`. **Step 1 landed the same evening:** the disc model (sessions, tracks, indices, extents), cue/bin (`BINARY`/`MOTOROLA`/`WAVE`, PREGAP/POSTGAP, multi-FILE) and plain ISO parsers, raw ⇄ cooked synthesis with EDC/RSPC parity (verified on every cooked read, never corrected), Q-channel synthesis with MCN/ISRC frames and CRC-16, and `discx` (`selftest` writes `mixed.cue/.bin/.ccd/.img/.sub`, `cooked.cue`, `plain.iso`; `dump`, `info`, `convert iso → cue/bin + WAVE audio tracks`). The EDC/ECC generator was checked against Neill Corlett's `ecm` 1.03 as an independent oracle (it strips only sectors whose parity it can regenerate: 2000 of 2000, 1999 with one byte flipped). `scripts/test.sh` runs `discx selftest` as the `libdisc` check. **Step 2 landed the same night:** the MMC responders (`mmc.rs`: READ TOC formats 0/1/2, READ SUB-CHANNEL 1/2/3, READ DISC INFORMATION, the READ CD length table with the MMC-3 contiguity rule and the per-sector fill incl. C2 and the three subchannel forms) and the C API (`libdisc/libdisc.h` v1, `capi.rs`, every body under `catch_unwind`); `discx selftest` now goes through the `extern "C"` functions only (the boundary QEMU will use) and adds `toc`, `read-cd-length` (60 CDB combinations), `read-cd-fill`, `panic-safety` (corrupt cues, NULL handles, short buffers, probe scores). **Step 3 (CCD reader) the same night:** `.ccd` + `.img` + optional `.sub` (replayed verbatim, synthesized past a truncated file's end), every `[Entry]` kept for READ TOC format 2, multisession from the `Session=` fields, `DataTracksScrambled=1` refused; the `ccd` check proves TOCs, sub-channel replies, raw / cooked sectors and sub-channel bytes identical across `mixed.cue`, `mixed.ccd` and `cooked.cue`. **Step 4 (the `cdimage` block driver) the same night:** `libdisc/qemu/cdimage.[ch]` overlaid into `block/` + `include/block/` by prepare, patch `50-cdimage-block-driver` (meson option `libdisc_dir`, `CONFIG_CDIMAGE`), `configure-qemu.sh` builds the crate and passes the option; `-cdrom x.cue` / `x.ccd` probe to `cdimage` (a plain `.iso` stays on `raw`), `qemu-img info` reports lead-out × 2048, the data track dd'd through the block layer equals the ISO, audio and L-EC-failing sectors are `-EIO`, writes refused, no Rust `std` symbol exported from `libqemu-embed-i386.so` (the 17 `libdisc_*` and `cdimage_disc` are; harmless). **XP boots with the converted guest-tools disc as `-cdrom gt.cue` under KVM and copies all 49 files through cdrom.sys byte-identical to the ISO** (`tools/xp-cdimage-test.sh`, 46 s). `scripts/test.sh`: `cdimage` (host) and `guest-cdimage` (guest stage) checks. **Step 5 (patch `51-atapi-disc-model`) the same night:** atapi.c serves reads (PIO synchronously, DMA by chunks through a bottom half), READ CD / READ CD MSF over the full MMC-3 table, READ TOC 0/1/2, READ SUB-CHANNEL, READ DISC INFORMATION, GET CONFIGURATION / mode pages 2A and 0E as a CD-ROM drive, and tracks a CD-DA position (75 sectors/s of virtual time; no sound yet) for PLAY / PAUSE / RESUME / STOP; a plain ISO keeps QEMU's path byte for byte. `tools/atapi-guest-test.py` (DOS, PIO on the secondary channel): **142 replies at byte-count limits 512 and 65534 identical to `discx dump`**, sense 03/11/05 on the flipped sector, 05/64/00 on audio, audio positions advance / hold / complete; the XP copy test passes on the patch-51 path (cdrom.sys, DMA). **Real dumps (the user's `/mnt/data2/david/Downloads/oldstuff` on the Linux box, 2026-09-04):** MDS/MDF brought forward from M5e (`mds.rs`: tracks from index 1 for `length` sectors at `start_offset`, the pregap not in the file — verified against a RAW+SUB dump's own Q frames); `discx scan` walks a whole image: **0 L-EC failures over five discs** (Death Rally, Blood 1 = Mode 2 form 1 + 8 audio, Duke Atomic, AOE Gold and Moto Racer MDS with 14 / 12 audio tracks), the only failures being the 149 audio-format sectors at the end of Fire Fight's data track (what a drive fails too); a data-track sector without a sync pattern (a dump tool's zero filler) now fails L-EC. The AOE Gold dump has no bad sectors: not a SafeDisc disc after all; **a SafeDisc / SecuROM dump is still wanted**. **Step 6 (CD-DA) the same night:** `-device ide-cd,audiodev=<id>` opens a 44100 Hz stereo voice; PLAY / PAUSE / RESUME / STOP feed the audio sectors through mode page 0E's routing and volume, MODE SELECT(10) sets the page (a data-out packet command: its end-transfer function is registered in core.c's table, which otherwise aborts QEMU on an unknown one); `CDTEST.EXE` (`guest-tools/src/cdtest.c`, MCI) plays track 2 and logs positions; `tools/xp-cdimage-test.sh` with `CDTEST=` records the drive's audiodev into a wav whose loudest second must be the 1 kHz tone. XP copies the real Blood disc 1 (Mode 2 form 1 + 8 audio tracks) and the AOE Gold MDS through cdrom.sys byte-identical. Next: Win98's CD Player by ear in the player (`-drive if=none,id=cd0,media=cdrom,file=x.cue -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0`), a protected dump for steps 7–8, the player's disc shelf (M5f with M6). |
| Launcher | stub (M6). |

## Build / run cheat sheet

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so     # .dylib on macOS
cargo build --release
# After every git pull: repeat prepare → configure → ninja → cargo. qemu/embed/
# is a COPY of embed/ (prepare-qemu.sh rsyncs it); a stale copy links the
# player against an old dylib ("undefined symbol _qemu_embed_..."). build.rs
# warns when the copy differs. macOS: export MACOSX_DEPLOYMENT_TARGET (same
# value configure-qemu.sh printed) before cargo too, or ld warns about
# "built for newer macOS version" on every C++ dep and libqemu.
# Win98 in the player (macOS shown; Linux identical, drop coreaudio bits)
target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 256 -hda ~/vms/win98.qcow2 \
  -vga cirrus -net none -usb -device usb-tablet -device sb16,audiodev=embed0
# Direct3D device: build the executor once, then the ISO after every guest change
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
scripts/test.sh          # the regression suite, host stage (~30 s); `all` adds XP + DOS guests (~2 min)
target/release/discx convert game.iso build/test/disc/game.cue --audio a.wav   # cue/bin from an ISO (+ audio tracks)
build/qemu/qemu-img info build/test/disc/game.cue   # "file format: cdimage"; -cdrom game.cue probes to it (doc 17)
tools/xp-cdimage-test.sh ~/vms/winxp.qcow2 build/test/disc/game.cue <dir with the ISO's files>   # XP copies the disc, hashes
target/release/discx scan game.cue                 # every sector classified + L-EC verified: the bad-sector map of a dump
CDIMAGE_TRACE=1 build/qemu/qemu-system-i386 … -cdrom game.cue   # every ATAPI packet, reply and sense on stderr
python3 tools/atapi-guest-test.py                  # DOS ATAPI battery vs discx dump (the guest stage's atapi-guest)
tools/string-bench.py --qemu old/qemu-system-i386 --qemu build/qemu/qemu-system-i386   # rep movs/stos/scas ns per element, A/B
build/d3dpt-exec-test x.bmp 120 60                 # host-only check of decoder + executor
build/d3dpt-dp2-test x.bmp                          # the display driver's records (M7c) without a guest
guest-tools/build-wrappers.sh                      # ISO with D3DPT\ (D3D9.DLL, D3D8.DLL, tests)
# XP test loop (Linux; -accel kvm -cpu host is fine, TCG identical): scratch FAT disk as E:
# for files out of the guest (creation recipe in the gotchas), CD as D:
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 512 -hda ~/vms/winxp.qcow2 \
  -hdb ~/vms/scratch.img -cdrom guest-tools/out/guest-tools-3dfx-d00e858.iso -vga cirrus -net none \
  -usb -device usb-tablet -qmp unix:/tmp/qmp.sock,server,nowait
tools/qmpc.py /tmp/qmp.sock keys meta_l+r; tools/qmpc.py /tmp/qmp.sock type 'cmd /c xcopy D:\D3DPT E:\D3DPT\ /I /Y'; tools/qmpc.py /tmp/qmp.sock keys ret
tools/qmpc.py /tmp/qmp.sock keys meta_l+r; tools/qmpc.py /tmp/qmp.sock type 'E:\D3DPT\D3DGAME9.EXE -frames 600 -dump 300 E:\OUT\G9.BMP'; tools/qmpc.py /tmp/qmp.sock keys ret
tools/qmpc.py /tmp/qmp.sock json '{"execute":"system_powerdown"}'   # clean XP shutdown
mcopy -i ~/vms/scratch.img@@1048576 ::/OUT/G9.BMP g9.bmp && tools/bmpdiff.py reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp g9.bmp --mask 0,368,270,112
# host log: qemu-system-i386: info: d3dpt: … (device, executor, and every guest DLL log line)
# a raw disc with CD audio in the player (doc 17): the explicit drive form instead of -cdrom, audiodev = the player's
#   -drive if=none,id=cd0,media=cdrom,file=game.cue -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0
# XP on our display driver (M7 track, doc 15): -vga none -device d3dpt-vga instead of -vga cirrus,
# driver installed once per image from the ISO's DRIVER\ (DRVINST.EXE -reboot); headless loops:
guest-tools/build-driver.sh && tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 ddtest   # or d3d7
```
Player env knobs: `PLAYER_DUMP`, `PLAYER_DUMP_OUT`, `PLAYER_DUMP_SEQ`,
`PLAYER_KEYS`, `PLAYER_AUDIO_NULL`, `PLAYER_LATENCY`, `PLAYER_REFRESH_MS`,
`PLAYER_SHADER`, `PLAYER_QMP`, `PLAYER_QMP_EXEC` (README). Firmware must be passed with `-L qemu/pc-bios`
until machine bundles exist. Test image: FreeDOS 1.3 floppy
(`build/images/144m/x86BOOT.img`, git-ignored; `tools/x87-guest-test.py`
fetches FD13-FloppyEdition.zip from ibiblio and extracts it).
macOS specifics: `docs/build-macos.md`. x87 tests need `brew install nasm
mtools`; `tools/x87-guest-test.py` downloads the FreeDOS floppy itself.

## Known issues / open threads

- 3D sync is `glFinish` before every hand-off (both platforms); a shared
  fence would let the vCPU continue while the blit drains. Glide (doc 12
  §5) is the last M3 item.

- Warm reboot of Win98 freezes on the Air (cold start works; Linux reboot
  paths verified fine). Untriaged: needs `-monitor stdio` → `info registers`
  / `info pic`, `-machine pc,hpet=off` test, and a stock-QEMU comparison.
- SDL standalone on macOS: 3D presentation janky unless the mouse moves
  (`SDL_GL_SwapWindow` from the vCPU thread; try `mesagl.cfg`
  `DispTimerMS,16`). Not relevant once M3 lands.
- Display Properties in Win98 under TCG faults RUNDLL32 (upstream 1964).
- On 2000/XP the qemu-3dfx OPENGL32.DLL maps the device through
  `\\.\MAPMEM` = FXPTL.SYS in its DllMain and returns FALSE without it,
  so every GL/D3D EXE "crashes at startup" (0xc0000142). The ISO's
  WIN2KXP step (FXPTL.SYS + INSTDRV.EXE as admin, reboot) is required for
  OpenGL and WineD3D, not only Glide. Hit and resolved 2026-09-03.
- FIFA 2000 (DX7, XP): with the WineD3D DDRAW.DLL it died in
  SetCooperativeLevel before the menu (2026-09-03). Read from the disk image
  (qemu-img convert → hdiutil attach → Dr Watson + Wine logs, the logs need
  the flushing debug build): wined3d asked cirrus for 800×600×32 because it
  maps the 24-bit desktop to B8G8R8X8, the driver refused, Wine 1.7.55
  crashed in the init_3d error path. Fixed in `patches/wine9x/01` (verified:
  the game runs, sound plays). Next symptom, white screen: wined3d logs show
  `glDrawBuffer` → GL_INVALID_OPERATION, and ddraw presents the primary
  surface by drawing into GL_FRONT + glFlush, never SwapBuffers; our embed
  backend's FBO stand-in has no front buffer and only presented on swaps.
  Fixed 2026-09-03 in `embed/mglcntx_embed.c` (macOS section): GL_FRONT/
  GL_BACK on framebuffer 0 → GL_COLOR_ATTACHMENT0, glFlush/glFinish present
  while the front buffer is selected. Not yet re-tested. Linux (EGL pbuffer)
  has the same swap-only presentation and will need the flush path too. The
  host also reports an ARB program failing to assemble ("out of range
  indirect offset +65", 9× per run): unexplained, may matter later. The
  stock software renderer also crashed once at match start with Microsoft's
  DDraw (NULL surface in softdrawz.dll), so the game may have a second,
  unrelated problem on this XP. **Parked 2026-09-03 (ADR-006):** the match
  renders (flush present + mode follow), but the pitch texture is noise
  bands, the screen flickers (present per glFlush) and DirectInput dies at
  the mode switch; the host's "program error +65" lines are wined3d's own
  ARB offset-limit probe, harmless. Direct3D 8/9 on XP moves to our
  paravirtual device (doc 14); WineD3D stays the DX7 fallback.
- Player: keys held in the guest are lifted on focus loss (2026-09-03):
  Cmd+Tab delivered the Windows-key press to the player and its release to
  the next app, leaving the guest with Win held down.
- XP paints the whole screen white around a Cirrus mode switch (a D3D
  title going fullscreen and back: 640×480 white, then 800×600 white for
  ~0.6 s while the desktop repaints; seen on the VGA surface of a bare
  `qemu-system-i386` too, so it is guest-drawn, not ours). Since
  2026-09-04 the player publishes black instead of any uniform
  single-colour frame within 1.5 s of a real mode switch
  (`qemu_vm.rs`, `SWITCH_GRACE`); the log counts them
  (`[display] N transitional frame(s) after the switch shown black`).
  On the way in nothing shows because the VGA surface is frozen while
  3D is active. The M7 driver path never had it (miniport zeroes VRAM).
- Win98: after wglgears / D3D9TEST exit, the mouse stops working in the
  guest (2026-09-03, untriaged: wrapper hook/cursor state vs the
  player's tablet? check whether keyboard still works and whether a
  second launch restores it).
- XP has no driver for `-vga std` (Bochs VBE): basic 640×480×16. The M4
  test loop runs XP with `-vga cirrus` (inbox GD5446 driver); the M7 track
  replaces it with `-vga none -device d3dpt-vga` + our driver (doc 15),
  which is where XP is headed.
- Pixel aspect / mode table not implemented (720×400 shows 9:5) — M2.
- `enable_cache` for librashader off (needs `Features::PIPELINE_CACHE`).
- `prepare-qemu.sh` must be followed by `configure-qemu.sh` when meson
  files change; the script keeps `werror` off and unchanged mtimes stable.
- x87 under TCG was all helper calls into 80-bit softfloat; patch 05 does
  the 53/24-bit-precision common case on the host FPU, and patch 06
  (doc 13, merged 2026-09-03) keeps the x87 stack as host doubles across
  instructions in TCG at PC=53: 21.6 (softfloat) / 10.6 (patch 05) /
  2.9 ns per op on x86-64; XP Super PI 1M on the Air 9:49 → 6:33 → 1:57
  (rig: 2:02), `x87-fast=off` control at softfloat pace, Win98 boots.
  Two aarch64 backend paths upstream never runs needed fixes (UMOV
  element size, constant into a V register). PC=24 (Direct3D) is inline
  too since 2026-09-03 (mode 2: same double shadows holding 24-bit
  values, results rounded through binary32; guest test identical, DOS
  loop 6.0× softfloat on the Air vs 10.4× at PC=53). Not yet checked in
  a D3D title. Test any change to
  it with `tools/x87-fast-test.c` (x86-64 host oracle) and
  `tools/x87-guest-test.py` (on/off identical under TCG; needs nasm,
  mtools, the FreeDOS floppy). Benchmarks inside a .COM must keep data on
  a separate page from code or QEMU's SMC invalidation dominates.
- SSE under TCG was a helper call per instruction (hardfloat inside, but
  a call and a lane loop). Patch 07 (doc 16, 2026-09-04) inlines the
  common cases when MXCSR admits the host FPU (RC nearest, no FTZ/DAZ,
  all masked) *and* PE is already sticky: then the host can raise nothing
  but PE, so one classification check per result replaces the residual
  the x87 path needs; anything else takes the helper out of line.
  Packed ops run on the vector unit (fadd_vec etc., vector checks, one
  lane-mask branch through env), scalar ops in general registers.
  Register-only bench on the Air: packed 1.2 ns/op vs 14 (12×), scalar
  2.7 vs 10 (3.6×); memory-operand loops see less (the TLB lookup
  dominates either way; the `dmb` barriers TCG emits for max_cpus > 1
  measured free on the M1). `-cpu …,sse-fast=off` is the control. Test
  any change with `tools/sse-guest-test.py` (on/off identical, incl.
  SSE2 via `+sse2`, the hand-over case and a mixed x87/SSE block) and
  re-run the x87 test: the slow blocks are shared. The hand-over exit
  after the first inexact helper must be emitted at the end of the
  instruction (after the register write-back) — the test caught
  `cvttss2si` losing EAX when it was emitted inside the gen function.
- Driving a Windows guest headlessly on Linux: pass
  `-qmp unix:/path,server,nowait` to the player (extra monitor), then
  `screendump` / `send-key` from a script; the QMP screendump shows the VGA
  surface only (frozen while 3D is active) — grab the player window with
  `grim` to see 3D frames. Win98 image copy: `~/vms/win98.qcow2`; wglgears
  at `C:\WINDOWS\Desktop\GAMEDIR`.
- Scripted guest runs: `tools/qmpc.py <sock> keys|type|screendump|json`
  against `-qmp unix:…,server,nowait`. Shut Win98 down from inside
  (`keys ctrl+esc`, `keys u`, `keys ret`) instead of killing the player —
  a killed VM leaves the FAT dirty and every next boot runs ScanDisk.
  `PLAYER_DUMP_OUT` dumps the shaded frame even when the window is
  occluded (compositor screenshots are useless then).
- macOS embed backend: never call `gl*`/`CGL*` by link — the QEMU build
  links XQuartz's Mesa libGL too and the symbol binds there (GLX library,
  no CGL context → silent no-ops, NULL renderer). `dlsym` on the
  OpenGL.framework handle, the same one the dispatch table uses.
- The Mesa backend (`MGL*`) runs on the vCPU thread under the BQL and can
  be driven without a guest right after `qemu_embed_new` (BQL held):
  `tools/embed-3d-test.c`. Order: `InitMesaGL` → `MGLTmpContext` →
  Choose/SetPixelFormat → `MGLCreateContext(MESAGL_MAGIC)` →
  `MGLMakeCurrent(MESAGL_MAGIC, 0)` → draw → `MGLSwapBuffers`.
- d3dpt: `D3DPT_EXEC_LIB` / `D3DPT_DXVK_LIB` point the device and the executor
  at the libraries when not run from the repo root (defaults:
  `build/d3dpt/libd3dpt_exec.so`, `build/dxvk/src/d3d9/libdxvk_d3d9.so.0`
  relative to the cwd, then the bare sonames). The executor sets
  `DXVK_WSI_DRIVER=Headless` itself. A guest process that finds no
  executor sees `D3DPT_STATUS_NO_EXEC` and the DLL refuses to load
  (0xc0000142), same shape as the missing-FXPTL case. Protocol changes
  bump `D3DPT_PROTO_VERSION` in `d3dpt/d3dpt_proto.h`; DLL, executor and
  device all check it. Driving XP from a script: `-cdrom` the ISO,
  `qmpc.py … keys meta_l+r`, `type 'D:\\D3DPT\\D3D9TEST.EXE 3000'`, `keys ret`;
  QMP `system_powerdown` shuts XP down cleanly. Getting files out of XP:
  a raw FAT32 image as `-hdb` (`truncate -s 64M`, `sfdisk` one partition
  at 2048, `mkfs.fat -F 32 --offset 2048`) appears as E: and is read with
  `mcopy -i img@@1048576 ::/path out` — XP writes lazily, so list it a few
  seconds after the program exits. Running EXEs from the CD works, but
  their logs then land in `C:\`; xcopy the folder to E: first.
  Guest-side debugging: the DLL's log lines reach the host log in order
  with the device's own lines (`qemu-system-i386: info: d3dpt: guest: …`),
  which is the only reliable channel when the guest freezes (files on the
  scratch disk stay in the guest's write cache). A process that ends
  without `DLL_PROCESS_DETACH` in that log was terminated or crashed at
  exit; exit-path bisection with `_cexit()` + `ExitProcess()` vs
  `return 0` found the stack smash above. mingw's d3d8 headers are
  `#pragma pack(4)` on i386, d3d9's are not: never hand-copy a D3D8 struct
  without the pack. Swapping the ISO under a running guest:
  QMP `blockdev-change-medium` on `ide1-cd0`.
  Hand-assembling SM1 bytecode: opcode numbers are D3DSIO_* (`m4x4` is 20,
  not 24 = `m3x2`); a wrong opcode compiles fine in DXVK and draws
  nothing — dump the SPIR-V with `DXVK_SHADER_DUMP_PATH` and read it.
  **Rebuild QEMU after a protocol bump** (`prepare-qemu.sh` + ninja): the
  device carries its own copy of the header and refuses a newer DLL
  (0xc0000142 with a `d3dpt.log` version line).
- Embed API bump (header `QEMU_EMBED_API_VERSION` + `qemu-embed` crate
  `API_VERSION`) ⇒ every machine must re-run prepare + ninja the dylib
  before `cargo build`, or the link fails on the new symbol.
- KVM on Linux (`-accel kvm -cpu host`) works for XP with every device of
  ours and is far faster than TCG; the x87 patches are TCG-only. TCG stays
  the Apple Silicon path and `scripts/test.sh` accepts both.
- Kernel-mode drivers with mingw-w64 (M7 track, doc 15): no `ntddk.h` in a
  miniport, `winddi.h` needs the vendored `ddrawint.h`, GCC emits
  `memcpy`/`memset` calls even freestanding; the debugger is a device
  register echoed to the QEMU log. `grim` hangs inside the agent sandbox:
  use QMP `screendump` on a standalone `-display none` run.
- Keys typed while a full-screen DirectDraw window is up go to that window
  and are lost: chain guest commands with `&` on one `cmd /k` line
  (`qmpc.py type` knows `& ( ) , ; = ' " * % + ! > < |`), copy logs to the
  FAT scratch disk at the end. Swap the CD under a running guest with QMP
  `blockdev-change-medium` (device `ide1-cd0`).

## Next steps, in order

Per-track order lives in the track docs: **M4** → `docs/tracks/m4-d3d-device.md`
(a real game on the device, present/pacing, the Air build, the x87
real-world number); **M7** → `docs/tracks/m7-display-driver.md` (FIFA 2000 plays on the
M7c HAL with the `DINPUT.DLL` keyboard fix confirmed under TCG, the DirectX 8 DDI with hardware T&L (D3DGAME8 and Max Payne through XP's own d3d8.dll), Diablo on
the new 8 bpp modes; next colour keying / T&L / DX8 tokens, more 8 bpp
titles, a driver stage in `scripts/test.sh`, cursor / mode table; the flip chain's vertical blank landed 2026-09-05). ADR-008 (2026-09-04): the M7 driver is the long-term XP shape; the
M4 DLL device stays for Win98 and as the executor's harness. Below, the
items nobody owns yet:

1. **M3 (doc 12):** Glide offscreen path, fence-based sync instead of
   glFinish. Both untouched since 2026-09-03.
2. **M2** mode table + pixel aspect: on XP it is the M7 device's mode table
   fed from the player (M7 track item); Win98 / the CRT presets still need
   the player side.
3. **Player:** a hardware-cursor sprite (the M7 driver and cirrus both
   define cursors the player ignores today), the vblank signal for guests.
4. **M5** → `docs/tracks/m5-cdrom-backend.md` (steps 1–6 done 2026-09-04; step 7 needs a protected dump; M5e MDS done early, CHD open); M6 launcher.
5. x87 / SSE: the **M8** track (`docs/tracks/m8-tcg-fastpaths.md`).

## Gotchas learned (don't relearn)

- **A game that runs far too fast is usually presenting, not timing.** A
  title of the era paces itself by its flip chain, so a `Flip` that never
  blocks is a missing frame limiter, not a clock bug (Moto Racer, 2026-09-05).
  The QEMU log's `d3dpt-vga: N page flips in 5.0 s` line is the guest's real
  frame rate; no line at all means the game blits to the primary instead of
  flipping, which nothing in the display path can pace — that one is the
  guest CPU. `DDFLAGS=32768` turns the vertical blank off for the A/B.
- Host toolchains: pinned 9.2.x needs `--disable-werror` (+ native-file
  strip), `-fPIC` + `b_staticpic` for the shared lib, uv-managed Python 3.12
  (3.14 breaks mkvenv), `MACOSX_DEPLOYMENT_TARGET` = running OS.
- macOS link: `qemu_default_main` must exist (cocoa.m); plugin export list
  hides symbols → our ld64 list; XQuartz + SDL2 required to build.
- Guest audio that gets laggier the longer XP runs, worse under load, was the
  embed audiodev pacing (2026-09-04, `embed/embedaudio.c` header): the mixer
  wrote at most one 10 ms tick per tick and never caught up after a stall, so
  every late main-loop tick under TCG stayed queued in the guest's DMA
  buffers. The cushion + wall-clock drop design replaced it.
- `build-wrappers.sh` is `set -e` and writes the ISO last: a failing stage
  leaves the previous ISO in place, so an ISO older than the sources means a
  stage died, not that the change is missing. Homebrew's mingw is a symlink
  in `/opt/homebrew/bin`, so `build-driver.sh` finds the DDK headers through
  `-print-sysroot` (2026-09-04: the Air's ISO had been missing `DRIVER\` and
  the `D3DPT\DDRAW.DLL` / `DINPUT.DLL` shims for that reason).
- Guest wrappers: modern mingw-w64 links the UCRT (Win9x has none) and
  qemu-3dfx compiles `-march=x86-64-v2`; the script forces msvcrt +
  pentium3 and refuses anything else.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  detected; repair path in build-macos.md.
- Caps-Lock→Control on macOS reports as right Ctrl to SDL (patch 03).
- Never `exit()` the process while the QEMU thread is alive: QEMU registers
  atexit handlers (`audio_cleanup`, exit notifiers) that then race
  `qemu_cleanup` → `assertion failed: mutex->initialized` on macOS. The
  player joins the QEMU thread after the event loop; headless dump paths use
  `_exit`. The other direction too: a guest power-off returns from
  `qemu_main_loop` while the UI thread still holds the handle — the QEMU
  thread flags `stopped`, wakes the loop, and waits for `release()` before
  `qemu_embed_destroy` (which frees the input mutex → same assert).
