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
  `d3d7test.c`, `ditest.c`, `dxttest.c`, `shtest.c`, `cktest.c`, `ebtest.c`, vendored DDK headers `ddk/` incl. the self-contained
  `d3dnthal.h`), `guest-tools/build-driver.sh` (also run by
  `build-wrappers.sh`, stages `DRIVER\` on the ISO).
- Executor, M7c's half: `d3dpt/exec/d3dpt_exec_ddi.cpp` (the display
  driver's records: VRAM surfaces, contexts, the DP2 interpreter,
  readback) and `d3dpt/exec/d3dpt_exec_int.h` (state shared with the d3d9
  half); `tools/d3dpt-dp2-test.cpp`.
- Tests: `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` + `tools/xp-fifa2000.bat`,
  `tools/xp-diablo.sh`, `tools/xp-motoracer.sh`, `tools/qmpc.py` key map additions.
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
  state and logs the game's DirectInput use. **Confirmed by the user 2026-09-05 by A/B on a
  Linux TCG run: keys with the DLL in place, dead keyboard again with it
  moved away.** Decided with it (doc 15): the merge stays a per-game,
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
- **Max Payne on the HAL with no wrapper DLL (2026-09-05, branch
  `track/m7-fifa`):** XP's own d3d8.dll on our DX7-level DDI (no
  `D3DCAPS8` answer = "DirectX 7 driver" to the DX8 runtime: software
  vertex processing, the DX7 token set). Launcher, menu and the tutorial
  level render (`tools/xp-maxpayne.bat`, `build/xp-driver-test/mp-hal3/`,
  ~290 frames/s under KVM `-cpu pentium3`). Two executor bugs came out
  (doc 15 "Max Payne on the HAL"): the inline-vertex tokens' payload and
  the next token are DWORD-aligned by *offset* (the DX8 runtime puts
  them at offset 2 mod 4 after an `INDEXEDTRIANGLELIST2`; the stream
  desynchronised, and once the end was aligned the vertices were still
  read 2 bytes early — the "black bands" across the alley), and a
  garbage light index made DXVK throw `std::bad_alloc` through its own
  statically linked unwinder — uncatchable, QEMU aborted; light indices
  and transform ids are now validated before DXVK sees them. New
  diagnostics: the token history on the first failure of a kind,
  `D3DPT_DP2_TRACE=<flag file>` (one whole frame: a state snapshot, every
  token with arguments, each draw's first vertices, every bound texture's
  levels and the render target after every draw as image files),
  `D3DPT_DDI_REREAD=1`, `D3DPT_DDI_NOFOG=1`, the driver's per-surface
  registration lines. `tools/d3dpt-dp2-test.cpp` covers the misaligned
  fan with the runtime's padding and the wild light / transform (the old
  library fails both: parse error, then abort). `winxp-m7g` carries this
  driver build (surface log lines); `cd.ini` in it points at D: now (the
  batch rewrites it). Not played by hand yet; ZBIAS (47) maps to
  DEPTHBIAS since 2026-09-05 evening (DXVK's d3d8 scale, −1/65535 a
  step; the alley sets it to 0).
- **The DirectX 8 DDI (2026-09-05, branch `track/m7-fifa`):** `GetDriverInfo2`
  (`D3DCAPS8`, the DX8 format list), hardware T&L claimed (DX7 and DX8
  caps; `ddflags=0x1000` withdraws it, `0x2000` keeps the DX7 face), the
  DX8 token stream rewritten by the driver into self-contained draws
  (`D3DPT_DP2_DRAW8`, protocol v6, v7 since the shaders: the runtime's buffers are guest memory),
  TEXBLT in the driver, per-context DX8 state, state sets as d3d9 state
  blocks, render-to-texture, MULTIPLYTRANSFORM, DXT pitches. D3DGAME8
  through XP's own d3d8.dll with hardware vertex processing at ~575 fps
  (`tools/xp-driver-test.sh <image> d3dgame8`), doc 15 "M7c — the DirectX 8
  DDI" for the d3d8.dll findings (the HAL-info flag; `dwActualSize`
  against the inner header; no `CLIPTLVERTS`; the FOURCC list; the
  clipper's fans). No shaders yet (VS/PS 0.0). **Max Payne on this path
  (2026-09-05, later that day):** the clipped fans were read from the
  DP2 vertex buffer, which is a 10 × 32-byte dummy under d3d8.dll; the
  runtime binds its clip buffer as stream 0 (`SETSTREAMSOURCE`) before
  the `CLIPPEDTRIANGLEFAN` tokens and their offsets count into that
  stream (doc 15, found in the disassembly). Read from stream 0 the
  alley renders complete on the DX8 DDI (`build/xp-driver-test/mp-fan/`).
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
- **The flip chain has a vertical blank (2026-09-05).** The user reported
  Moto Racer 1997 playing at several times its speed on the driver. Cause:
  `DdFlip` wrote the OFFSET register and returned, `DdGetFlipStatus` always
  said "done", so `Flip` never blocked — and a 1997 racer is paced by its
  flip chain, not by a clock. Fixed on both sides (doc 15 "The flip chain's
  vertical blank"): `FRAMES` is now periods of the mode's `HZ` off the host
  clock rather than the display client's pull (so a headless run paces like
  the player instead of falling back on the 50 ms bail-out at 20 fps), and
  the driver holds the second flip of a double-buffered chain until it
  moves. `DDTEST`'s three exclusive chains and `D3D7TEST` all report 60 fps
  (the D3D7 frame is still byte-identical to `d3dpt-dp2-test`'s), the
  windowed `Blt` path is untouched at 346 fps as on real hardware, and
  `DDFLAGS=32768` (`DDF_NO_VSYNC`) restores the old throughput numbers to the
  frame. The device logs `N page flips in 5.0 s` while flips happen: that is
  a title's real frame rate, and no line at all means it blits to the
  primary, which no vertical blank can pace. No `D3DPT_FB_VERSION` bump (the
  register's contract is unchanged), so an installed v3 driver keeps working
  against the new device — it just needs a reinstall from the ISO to get the
  pacing. Not verified on a real title yet: **Moto Racer itself is the
  outstanding check** (the user's box, `~/vms/winxp-m7` after a driver
  reinstall).
- **Moto Racer 1997 takes the HAL with protocol v8 — and is a DirectX 3
  title (2026-09-05, `tools/xp-motoracer.sh`).** Headless with its
  Alcohol MDS/MDF as D: (the cdimage driver) on `winxp-m7g`: the game
  wants a 16 bpp desktop (`SETMODE 800 600 16` first), creates a Direct3D
  context, 256×256 textures with colour keys and palettes, flips at 60/s —
  and drew nothing through it: 0 draws in minutes, the menus' bikes and the
  race's track missing while the game's own 2D (panels, panorama, HUD) is
  there. It draws through `IDirect3DDevice::Execute` (execute buffers,
  texture handles), which XP's `d3dim.dll` emulates on our DP2 driver, and
  that path had never been exercised. `DRIVER\EBTEST.EXE`
  (`guest-tools/src/d3dptvid/ebtest.c`, `xp-driver-test.sh <image> ebtest`,
  `-rgb` = the same on the runtime's RGB device as the control) reproduces
  it without the game; doc 15 "Execute buffers — the DirectX 3 path" has
  the findings: (1) `dwMaxVertexCount` 65535 made every `Execute` fail with
  `E_OUTOFMEMORY` before any token (the runtime sizes its TL vertex buffer
  from the cap plus a page and refuses > 65535 vertices — found in the
  d3dim.dll disassembly; the cap is 4096 now, `ddflags=0x40000` is the
  repro); (2) the legacy path is a pass-through: an UNCLIPPED `Execute`
  hands the execute buffer's own instruction stream to `DrawPrimitives2`
  (`D3DHALDP2_EXECUTEBUFFER`), the driver consumes POINT / LINE /
  TRIANGLE / STATERENDER / SPAN / EXIT and *bounces* everything else
  (`PROCESSVERTICES` first of all) with `D3DERR_COMMAND_UNPARSED` +
  `dwErrorOffset` so the runtime executes it and calls again — skipping
  it left the TL vertex buffer empty; (3) the DX5 texture render states
  (`TEXTUREHANDLE`, `TEXTUREMAPBLEND`, filters, address) arrive verbatim
  on this path and the executor maps them onto stage 0 now. EBTEST passes
  5/5 on `winxp-m7g`; `tools/d3dpt-dp2-test.cpp` covers the executor half.
  **Moto Racer plays** (`tools/xp-motoracer.sh install|play`, `moto4/`:
  name screen, showroom bike, the Speed Bay race with the palms keyed
  out, 120 frames/s under KVM, ~175 draws a frame one triangle each — a
  batching follow-up). **Played by hand by the user (2026-09-05 evening,
  the player on `winxp-m7g`, the shadow executor): "works great", and
  fast under TCG too** — the frame is the host GPU's, the guest only
  builds execute buffers. The user's `winxp-m7` still needs the driver
  reinstalled from the ISO.
- **Untracked writes: the showroom's 2D panels (2026-09-05, evening).**
  The bike-selection screen's header, arrows, features panel and
  Start / Back were missing in *every* run, not one: the game draws them
  with GDI through `GetDC` on the back buffer, which dxg serves without
  any driver callback (no `DrvDeriveSurface`), so no `VRAM_DIRTY` ever
  came and the executor's readback overwrote them each frame. The
  executor now keeps a shadow of each render target's VRAM and compares
  it before the frame's first draw (an unannounced write is uploaded)
  and at the readback (pixels written since the draws are kept over the
  host frame, and the target refreshed from VRAM before the next draw)
  — doc 15 "Untracked writes"; `tools/d3dpt-dp2-test.cpp` covers both
  paths. The panels and the race HUD's text show
  (`build/xp-driver-test/moto9/bike2.png`, `race1.png`); the device log
  counts the pixels (`N untracked guest pixels`). Found on the way: the
  one-triangle draws cannot be batched (a `TEXTUREHANDLE` before every
  draw, painter's order with Z off — doc 15), the item is closed; and
  `xp-motoracer.sh play` had never reached the race on its own (the
  title takes only the keyboard and runs into an attract demo when
  idle, the name screen's Enter presses whichever letter the cursor was
  on) — it drives the menus by `tools/motoracer-state.py` now, a
  screendump classifier with a retry per screen.
- **The hardware cursor (2026-09-05 evening, register set v4).** The
  user's report from hand play: the mouse cursor flickers. GDI's software
  pointer is painted into the GDI primary — one buffer of a flip chain,
  so under a 60 Hz title it shows every other frame, and on the desktop
  the scanout catches its erase. Now `DrvSetPointerShape` /
  `DrvMovePointer` write the pointer (mono AND/XOR, colour through a
  32 bpp engine bitmap, `SPS_ALPHA`) as a8r8g8b8 into the 16 KiB above
  the DirectDraw heap and the CURSOR registers; the device hands it to
  QEMU's console (`dpy_cursor_define` / `dpy_mouse_set`); the player,
  which already received `on_cursor` / `on_mouse_set` from the embed
  library and ignored them, shows the guest's shape as the host window's
  cursor over the image (the USB tablet puts the host pointer where the
  guest's is: no compositing, no latency; hidden when the guest hides it,
  hidden as before for a guest without a hardware cursor). Pointers over
  64×64 stay software. v4 refuses a v3 driver: every image needs
  `install` from the ISO (`winxp-m7g` done; the user's `winxp-m7`
  pending), QEMU rebuilt (prepare → ninja). Doc 15 "The hardware cursor".
  Without the tablet (a relative mouse, the grab) the player composites
  the sprite into the frame at the guest's position instead — the first
  cut lacked that and showed no cursor at all in the game. **User-confirmed
  the same evening: steady desktop cursor, present in Moto Racer's menus.**
- **GTA Vice City plays on the DX8 DDI (user, 2026-09-05 evening).** The
  first real DirectX 8 title through XP's own d3d8.dll on our driver
  (`FLT-VCA` / `FLT-VCB` in the oldstuff folder, a May 2003 build):
  installed and played by hand under TCG (`-cpu pentium3`, no KVM),
  800×600×32 with a D24S8 depth buffer, "everything ok, water effects
  and all". The log (`/tmp/player3.log`) has no unknown token, no
  dropped state and no `dx8 draws skipped` line; the city runs 400–600
  draws a frame over 3–4 DrawPrimitives2 calls at 20–30 frames/s, the
  menus at 60. Slow-ish under TCG at 800×600: the cost is the guest CPU
  (the game, the DX8 runtime, the driver copying every draw's vertices
  into the window) — KVM first, then `D3DDEVCAPS_HWVERTEXBUFFER` (vertex
  buffers in VRAM, written once, a handle and a range per draw) is the
  driver-side win.
- **Tried and dropped the same evening: blit / stretch caps** for FIFA
  2000's 320×240 intro videos — `DDCAPS_BLT | BLTSTRETCH | …` with a
  declining `DdBlt` broke DDTEST's colour fill (on XP `NOTHANDLED` is
  E_NOTIMPL to the app, not a HEL fallback) and the game never blits its
  movies anyway (doc 15 "Blit caps and the HEL"). Open: where the user
  saw them full-screen.
- **GTA 2 (DirectX 6, 640×480×16) glitched on its first run and crashed
  on its second (user, 2026-09-05 evening) — three faults, all fixed**
  (doc 15 "A DirectX 6 title's flip chain"): its flip chain reaches
  `CreateSurfaceEx` as the primary alone, so the back buffer (handle 2)
  was never registered and the host drew every frame into the front
  buffer while the flips alternated the scanout; the Direct3D context
  table sat in the PDEV, which the game's mode switch back at exit
  replaced before dxg's `ContextDestroyAll`, so the context leaked on the
  host and the next run's `CTX_CREATE` of the same handle was refused
  (`E_FAIL` from `CreateDevice`); and its menu text drew as white boxes
  because the DX6 runtime passes `TEXTUREMAPBLEND` through untranslated
  and the executor had mapped it once, before any texture was bound. The
  driver walks the attach list, keeps its contexts in a global table and
  re-registers a target the host knows at another offset before every
  readback; the host replaces a context re-created under an open handle
  and re-maps the legacy blend per texture. Verified headless on an
  overlay of `winxp-m7g` (three launches in one boot, the menu with its
  text); **the driver must be reinstalled from the ISO in the user's
  image**. Enter during the Bink intro movie crashes the game on XP's
  inbox cirrus driver just the same (`VGA=cirrus` control): the game's,
  not ours (doc 15). Not yet: played past the menu.
- Branch history: `worktree-luminous-dancing-cocke` (merged into main
  2026-09-04), `track/m7-d3d-ddi` (M7c, merged into main 2026-09-04),
  `track/m7-fifa` (FIFA on the HAL + the keyboard fix, merged into main
  2026-09-04 evening; the 8 bpp work continues on it). New work: branch
  `track/m7-<topic>` off main.
- **Resuming here (2026-09-05, after the DX8 DDI session):** pull main,
  then prepare → configure → ninja (protocol v8 is checked by the
  device), `scripts/build-d3dpt-exec.sh`, `guest-tools/build-driver.sh`,
  `guest-tools/build-wrappers.sh` (the ISO's DLLs speak v8),
  `cargo build --release`. `winxp-m7g` carries the last driver build
  (`install` it again after any driver change). State of the DX8 DDI:
  1. **Works:** D3DGAME8 through XP's own d3d8.dll with hardware vertex
     processing, render-to-texture and all (`tools/xp-driver-test.sh
     ~/vms/winxp-m7g.qcow2 d3dgame8`, ~575 fps); its frame differs from
     the native oracle only in the particles, which use the gradient
     texture because the DXT1 one never reaches the driver (open item 3).
     FIFA 2000 (attract-mode match) and D3D7TEST (frame == host frame)
     are unchanged on the new driver. `scripts/test.sh host` green.
  2. **Done (2026-09-05): Max Payne's clipped fans.** Their offsets are
     byte offsets into stream 0, which the runtime rebinds to its own
     clip buffer before the tokens (doc 15 "M7c — the DirectX 8 DDI",
     the clipper bullet). The driver reads them from `w->vb` now; the
     tutorial alley renders complete with hardware T&L
     (`build/xp-driver-test/mp-fan/cmd-*.png`). Not played by hand yet.
  3. **Done (2026-09-05): DXT textures on the DX8 path.** dxg sized the
     video-memory surface from the (absent) bit count of a FOURCC format
     and asked its heap for zero bytes; the new `DdCreateSurface` hands
     it the block size (doc 15, the compressed-textures bullet).
     `DRIVER\DXTTEST.EXE` (new) is the probe and the check: every format
     × pool, readback of a textured quad. D3DGAME8's particles now use
     the DXT1 disc like the native oracle.
  4. **Done (2026-09-05 night, protocol v7): vertex and pixel shaders
     1.x.** The caps claim vs 1.1 / ps 1.4, the driver forwards the seven
     shader tokens and a DRAW8 under a shader carries the handle, the
     executor keeps the shaders per context (DX8 declaration → d3d9
     declaration + prepended `dcl`s, declaration-only = fixed function,
     `D3DVSD_CONST`, the constants) and validates every function against
     a vs 1.x / ps 1.x opcode table first — DXVK *asserts* on an unknown
     opcode (an abort, QEMU dies), found by the host test's hostile case
     (doc 15 "Vertex and pixel shaders 1.x"). `tools/d3dpt-dp2-test.cpp`
     covers the whole set; `DRIVER\SHTEST.EXE` + `xp-driver-test.sh
     <image> shtest` is the guest check through XP's own d3d8.dll
     (9 cases, 0 failed on `winxp-m7g` under KVM; one finding on the
     way: d3d8.dll refuses a declaration-only shader whose registers are
     not in FVF order with `D3DERR_INVALIDCALL` before the driver sees
     it). A protocol bump: QEMU (prepare → ninja), the
     executor, the driver ISO and the guest-tools ISO all rebuilt;
     `winxp-m7g` has the v7 driver installed.
  5. **Done (2026-09-05 night, protocol v8): palettized textures and
     colour keying**, the two caps Moto Racer 1997 wants (main's
     diagnosis). P8 in both format lists, `TRANSPARENCY` / `ALPHAPALETTE`
     in the texture caps, `DDCAPS_COLORKEY` + `DDCKEYCAPS_SRCBLT` with a
     `SetColorKey` *and* a `Blt` surface callback (dxg drops the HAL for
     the caps without a Blt callback; without the caps user-mode ddraw
     never hands the key down; no `DDCAPS_BLT`, so the Blt is never
     called — doc 15 has the four runs), the key sent by `DdSetColorKey`
     and re-checked off dxg's `DD_SURFACE_LOCAL` when a
     `TEXTURESTAGESTATE` binds the texture → `D3DPT_OP_VRAM_COLORKEY`;
     the executor
     takes the palettes from the DP2 `SETPALETTE` / `UPDATEPALETTE`
     tokens, expands P8 and keyed textures to A8R8G8B8 (key = alpha 0),
     forces the alpha test while render state 41 is on and overrides
     stage 0's alpha op when the app's ignores the texture alpha (the
     DX7 runtime's `TEXTUREMAPBLEND` emulation does exactly that for a
     keyed 16-bit texture), and re-uploads bound textures whose palette
     or VRAM changed before each draw (doc 15 "Palettized textures and
     colour keying"). `tools/d3dpt-dp2-test.cpp` covers it;
     `DRIVER\CKTEST.EXE` + `xp-driver-test.sh <image> cktest` is the guest
     check (4 cases, 0 failed on `winxp-m7g` under KVM; with it D3D7TEST's
     frame still equals the host test's, SHTEST 9/9, Diablo into Tristram
     in the right colours, FIFA 2000's attract-mode match as before —
     one observation from that run: with the vertical blank the match
     *flips* at ~24/s (`119 page flips in 5.0 s`, 121 readbacks, ~145
     DP2 calls per frame, each a synchronous doorbell round trip), so
     the per-call round trip is what bounds a title that batches
     little; it used to blit). `ddflags=0x10000` withdraws the caps for
     an A/B. **Found on the way, in `DdFlip`:** on NT a flip exchanges the
     two surfaces' *roles*, not their memory (handles keep their VRAM,
     the PRIMARYSURFACE caps move, dxg re-issues `CreateSurfaceEx` for
     both), and the M7c first cut re-registered them as if the memory had
     swapped — the host rendered into the displayed buffer every other
     frame since 2026-09-04 without any test noticing (doc 15 "A flip
     does not move memory"). Fixed: `DdFlip` re-registers nothing. Moto
     Racer itself is still the outstanding check (the user's box: does it
     take the HAL now, and does it look right).
  6. **Done (2026-09-05 afternoon): the DirectX 3 execute-buffer path.**
     Moto Racer took the HAL with v8 and drew nothing; `EBTEST` and the
     `d3dim.dll` disassembly found `dwMaxVertexCount` (65535 → every
     `Execute` E_OUTOFMEMORY; 2048 now) and the pass-through protocol (the
     raw `D3DOP_` stream under `D3DHALDP2_EXECUTEBUFFER`: consume 1 / 2 /
     3 / 8 / 13 / 11, *bounce* the rest with `D3DERR_COMMAND_UNPARSED` +
     `dwErrorOffset`), plus the DX5 texture render states mapped in the
     executor. EBTEST 5/5, Moto Racer races (doc 15 "Execute buffers — the
     DirectX 3 path"). **Both follow-ups closed the same evening:** the
     showroom's 2D panels were GDI writes through `GetDC` the driver
     never sees (the executor's target shadow catches them now, doc 15
     "Untracked writes"), and the one-triangle draws are one texture
     switch each in painter's order — nothing to batch. Played by hand
     by the user the same evening: works great, fast under TCG as well.
     (The attract demo's Esc menu looked like "EXT
     DEMO / UIT DEMO / C NTINUE DEMO" in the screendumps: at 3× the
     letters are there, drawn in a darker red as the hotkeys — the
     game's own style, not a rendering fault.)
  7. Then the small things the
     runs showed: D3DGAME8's frame still differs from the native oracle
     along the checker texture's texel edges only (2026-09-05 night:
     8.7 k pixels beyond tolerance 8, channel difference ≤ 43, the
     particles now identical — a mip / filter selection nuance on the DX8
     DDI, `build/xp-driver-test/g8-dxt/g8-diff.bmp`); `render target handle 3 unknown` once at start
     (harmless), the two textures D3DGAME8 re-registers every frame
     (kept by the executor, cheap), the `dp2 vertices at` debug lines
     (first four calls only).
- The tools of this session: `D3DPT_DP2_TRACE` (a whole frame with
  states, vertices, texture and per-draw target dumps),
  `D3DPT_DDI_REREAD`, `D3DPT_DDI_NOFOG`, the driver's surface log and
  `dx8 draws skipped … why …` lines (bits: 1 shader, 2 no fvf, 4 no
  stream, 8 stride < fvf, 16 vertex range, 32 index range, 64 prim; the
  first skipped draw's parameters follow), `tools/xp-driver-test.sh
  d3dgame8`, `tools/xp-maxpayne.bat`, and the M7c DX8 section of doc 15
  with the six d3d8.dll / dxg findings.

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
tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 d3dgame8  # D3DGAME8 through XP's own d3d8.dll on the DX8 DDI, diffed against build/test/g9-native.bmp
OUT=build/xp-driver-test/sh tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 shtest   # SHTEST: shaders 1.x through d3d8.dll, "0 failed" in shtest.log is the pass
OUT=build/xp-driver-test/ck tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 cktest   # CKTEST: a P8 texture + palette, a colour-keyed texture through the DX7 HAL
# (OUT= relative under build/: the QMP socket path must stay under 108 characters, and the worktree path is long)
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 cmd 'D:\DRIVER\DDTEST.EXE 800 600 32 300'
# a game: its disc as D: (the driver ISO moves to F:), a batch file staged as E:\RUN.BAT, a screendump every 5 s
GAME_ISO=/mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO SHOTS=24 tools/xp-driver-test.sh ~/vms/winxp-m7f.qcow2 bat tools/xp-fifa2000.bat
# Max Payne through XP's own d3d8.dll on the DX7-level HAL (no wrapper DLL): launcher, menu, tutorial; ~4 min
GAME_ISO="/mnt/data2/david/Downloads/oldstuff/Max Payne/DINO-MAP.iso" CPU=pentium3 SHOTS=30 SHOT_KEYS="2:ret,6:ret" \
  tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 bat tools/xp-maxpayne.bat
# one frame of its DP2 stream in the QEMU log: export D3DPT_DP2_TRACE=$PWD/build/xp-driver-test/trace.flag before the
# run and `touch` that file when the screendump shows the scene (D3DPT_DDI_REREAD=1 re-reads every texture at every bind)
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
  `0x20` = Direct3D off, `0x1000` no T&L, `0x2000` no DX8 face, `0x10000`
  no colour keying, `0x40000` `dwMaxVertexCount` 65535 again (the DX3
  E_OUTOFMEMORY repro), `0x80000` never call the runtime's
  `D3DParseUnknownCommand`. The QEMU log's `d3dpt-vga: ddi: …` lines are the
  executor's (unsupported states / tokens, once each), `batch N: error` a
  refused record, `d3dptdisp: dp2 0x…` a DrawPrimitives2 the host failed.

## Next steps, in order

1. **M7c, the rest:** FIFA 2000 plays, keyboard included — the user
   confirmed `D3DPT\DINPUT.DLL` fixes it (2026-09-05), and the shim stays
   per-game by decision (doc 15). Max Payne runs through XP's d3d8.dll on the DX7-level DDI
   (tutorial level clean, now on the DX8 DDI with hardware T&L); play it
   by hand (ZBIAS → DEPTHBIAS landed 2026-09-05 evening). Shaders 1.x landed
   2026-09-05 (protocol v7; a title that uses them is the next check —
   a DX8 game with vs 1.1 / ps 1.1 paths). Moto Racer 1997 ran its
   software rasterizer (main, 2026-09-05) for want of palettized (P8)
   textures and colour keying (`D3DPTEXTURECAPS_TRANSPARENCY`); both
   landed the same night (protocol v8), and the DirectX 3 execute-buffer
   path the next afternoon — **Moto Racer races on the HAL headless**
   (`tools/xp-motoracer.sh play`), and **played by hand by the user the
   same evening on `winxp-m7g`: works great, fast even without KVM** (the
   showroom's 2D panels fixed first, doc 15 "Untracked writes"); the
   user's own `winxp-m7` still needs the driver reinstalled from the ISO.
   **GTA Vice City, the first DX8 title, plays (user, the same evening;
   nothing refused, 400–600 draws a frame, 20–30 fps under TCG).** Next
   for it: a KVM number, then video-memory vertex buffers
   (`D3DDEVCAPS_HWVERTEXBUFFER`) so a draw stops copying its vertices
   through the window. Then what the next title asks
   for first among: **more than one stream** (the driver copies stream 0
   only; a multi-stream declaration's draws are skipped with a log line —
   the DRAW8 token would carry one blob per stream), cube / volume textures, presenting the
   host frame through the player's 3D path instead of the per-frame
   readback copy. A validator for SM2/3 bytecode on the d3d9 half (the
   M4 track's `d3dpt_exec.cpp` hands guest bytecode straight to DXVK,
   which asserts on garbage — see doc 15's shader section) is worth the
   same treatment.
2. Add a `driver` stage to `scripts/test.sh` (boot on `d3dpt-vga`, `modes`
   + `ddtest` with expected numbers) once the M4 track's suite structure
   is stable; until then `tools/xp-driver-test.sh` is the check.
3. Small items: `DrvDeriveSurface` (GDI on DirectDraw surfaces; an
   optimisation now, the executor's target shadow keeps GDI's writes), a
   real vblank signal from the player's present, a real blitter behind
   `DDCAPS_BLT` if a title ever needs the caps (doc 15 "Blit caps and the
   HEL": a declined `DdBlt` is E_NOTIMPL to the app on XP), the mode
   table fed from the player (M2), a macOS
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
  `DDCAPS_NOHARDWARE` (doc 15 has the disassembly trail). The same for
  `DDCAPS_COLORKEY` / `dwCKeyCaps` *without a Blt callback* (CKTEST
  bisection): the driver keeps a `DdBlt` that is never called (no
  `DDCAPS_BLT`) so dxg accepts the caps, and without the caps user-mode
  ddraw never hands a texture's colour key down at all.
- A flip on NT exchanges the two surfaces' roles, not their memory: each
  handle keeps its VRAM, the PRIMARYSURFACE caps move, dxg re-issues
  `CreateSurfaceEx` for both. Never re-register the chain in `DdFlip`.
- The debugger is the DEBUG register → QEMU log. No WinDbg, no serial KD.
- The DX8 runtime asks its `GetDriverInfo2` questions only with
  `DDHALINFO_GETDRIVERINFO2` in the HAL info, and checks `dwActualSize`
  against the size inside the GDI2 header (the outer one is stale). DX8
  device state (vertex format, streams, indices) persists across
  DrawPrimitives2 calls, by handle (buffers move on a DISCARD lock).
  dxg's `lPitch` of a DXT surface is its linear size, and a FOURCC
  surface needs its code in `DrvGetDirectDrawInfo`'s FOURCC list. Never
  claim `D3DPMISCCAPS_CLIPTLVERTS`: the host does not clip
  pre-transformed vertices; the runtime's clipped fans are stream-0
  draws (it rebinds stream 0 to its own clip buffer first), and the DP2
  call's vertex buffer is a dummy under d3d8.dll. A protocol bump (`D3DPT_PROTO_VERSION`) means QEMU (prepare →
  ninja), the executor and the guest-tools ISO all rebuilt before any
  guest run.
- The executor must never let DXVK throw: its exceptions abort QEMU
  (DXVK's own static unwinder vs the system personality routine), a
  `try` in the executor does not help. Validate every index / count
  before the call — and every shader's bytecode (`sm1_valid`): DXVK's
  compiler asserts on an unknown opcode instead of failing the create. `pgrep -f '<image>'` matches the shell loop that
  contains the pattern — use `pgrep -a qemu-system` to see guests.
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
