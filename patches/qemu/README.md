# QEMU patch queue

Applied by `scripts/prepare-qemu.sh` on top of the pinned QEMU submodule
(v9.2.4 — the newest release qemu-3dfx's `00-qemu92x` patch supports) after
the qemu-3dfx overlay (`third_party/qemu-3dfx` → `hw/3dfx`, `hw/mesa`) and
the `embed/` overlay (→ `qemu/embed`). Order = filename order. The script is
deterministic: it restores every tracked file any patch touches to pristine
and re-applies everything on each run, then runs qemu-3dfx's `sign_commit`
(stamps the qemu-3dfx commit; guest wrappers must be built from the same
commit — `guest-tools/build-wrappers.sh` does that).

**Never `git checkout` files inside `qemu/` by hand between prepare runs** and
never rely on "already applied" heuristics — a partial tree once silently
lost the 3dfx meson hunk (symptom: `unknown type 'glidept'`).

| Patch | What / why | Drop when |
|---|---|---|
| `00-3dfx-darwin-contextalpha` | qemu-3dfx Darwin build regression: `GL_CONTEXTALPHA` only defined under `CONFIG_LINUX` but used in shared code | upstream qemu-3dfx fixes it |
| `01-upstream-i386-lss-tb-exit-fix` | backport of QEMU `0f1d6606c28d` (issue 2987): 9.2.4 carries the LSS/IRQ-shadow regression but not the fix → Win98 SE `exception 0D` on first boot after setup under TCG | QEMU ≥ 10.1 |
| `02-mesa-sdlgl-on-darwin` | build `mglcntx_sdlgl.c` (SDL/native OpenGL.framework) instead of GLX on macOS: with a Cocoa SDL window the GLX backend gets an `NSWindow*` as "X11 window" → `BadDrawable`; also defines two missing NV enums | our own M3 backend replaces it |
| `03-sdl-darwin-either-ctrl` | macOS Caps-Lock→Control remap reports `KMOD_RCTRL`; accept either Control for SDL hotkeys | — |
| `04-3dfx-graceful-no-display` | without an SDL display (player: `-display none`) GL activation refused the context cleanly instead of `exit(1)`; Glide still exits | M3 vtable/provider |
| `05-x87-fast` | x87 on the host FPU when the guest runs at 53/24-bit precision with round-to-nearest (Windows default / Direct3D): bit-exact vs. softfloat, falls back for everything else; `-cpu …,x87-fast=off` disables. Tests: `tools/x87-fast-test.c` (host oracle vs. real x87), `tools/x87-guest-test.py` (on/off identical under TCG). Super PI 1M on the M1 Air 9:49 → 6:33 | upstream QEMU grows a floatx80 hardfloat path |
| `06-x87-inline-tcg` | **doc 13**: eight scalar binary64 TCG opcodes (x86-64 VEX+FMA3, aarch64 scalar FP) and a translator mode that keeps the x87 stack as host doubles across instructions at PC=53 or PC=24 (Direct3D) with RC=nearest/PE masked (2-bit TB flag), converting to x80 only at TB exits, before helpers and on faults (unwind repair via a third insn_start word). At PC=24 the shadows hold 24-bit values and every result is rounded through a binary32 conversion (correct for 24-bit operands). Same checks as x87-fast.h; anything else runs the helper sequence out of line and exits the TB, so bit-exact vs softfloat except empty registers after a pop. DOS loop 21.6 (softfloat) / 10.6 (patch 05) / 2.9 ns per op on x86-64; aarch64 bring-up done 2026-09-03 (doc 13 §Bring-up): UMOV element size and constant-into-V-register paths fixed; XP Super PI 1M on the M1 Air 9:49 → 1:57 (rig P4 1.7: 2:02), `x87-fast=off` control at softfloat pace, Win98 boots. Tests: `tools/x87-guest-test.py` (382k lines identical on/off, 7 control words incl. PC=24; bench at both precisions) | upstream float ops in TCG, or a rewrite of the x87 translator upstream |
| `07-upstream-x87-helper-fixes` | backports cf10af6c703d (pseudo-NaN in FPATAN/FYL2X/FYL2XP1 is Invalid + default NaN, not silenced) and 0924d9d3db36 (fcomi/fucomi clear OF/SF/AF); the latter applied to patch 06's inline compare too so fast and slow paths agree. DE / flush-to-zero fixes skipped (need the 10.0 softfloat rework) | base ≥ 11.1 |
| `08-upstream-i386-decoder-fixes` | backports the 2025–26 fixes era code trips: F6/F7 /1 TEST alias, RCL/RCR count modulo for 8/16-bit (immediates too), V86 entry only at CPL 0, real-mode interrupt stack size, TSS T bit, mov to CS / segment 6–7 is #UD, invalid 0F C7 forms | base ≥ 11.1 |
| `09-upstream-i386-rep-string` | backports the 10.0 repeated-string series (14 commits, Paolo Bonzini): REP/REPZ run several iterations per TB with explicit cc_op and RF handling. `tools/string-bench.py`: rep movs/stos 12–16 % faster per element under TCG, scasb unchanged; translate.c closer to 10.x | base ≥ 10.0 |
| `10-embed-api` | meson: `shared_library('qemu-embed-<target>')` per softmmu target from the existing static lib + `embed/libqemu_embed.c`, `embedaudio.c`, `embedfx.c`, `mglcntx_embed.c` (+ epoxy and gbm when found); ld64 export list (`embed/libqemu_embed.symbols`) because QEMU's plugin `-exported_symbols_list` hides everything else on macOS | upstreamed embed API (aspirational) |
| `20-embed-audio` | register the `embed` audiodev: QAPI enum/union entry, `audio_template.h` per-direction case, **and `audio/audio.c` `audio_create_pdos` CASE** (missing → NULL pdo segfault) | with 10 |
| `30-3dfx-ui-vtable` | the 11 qemu-3dfx UI entry points (`mesa_*`, `glide_*`) dispatch through a `QemuFxUiOps` table (`ui/fxui.c`); SDL registers its implementations at display init, the embed library its window-less provider (`embed/embedfx.c`); no provider = contexts refused / no Glide window, VM keeps running (supersedes the spirit of 04) | upstream qemu-3dfx grows a provider seam |
| `31-mesa-ctx-weak` | `hw/mesa/mglcntx_linux.c` (GLX) and `mglcntx_sdlgl.c` (macOS, incl. `dllname`) export weak so `embed/mglcntx_embed.c` overrides them inside libqemu-embed while qemu-system keeps the native backend. Embed backend: Linux = EGL surfaceless + pbuffer as FBO 0; macOS = CGL context without a drawable + an FBO standing in for the default framebuffer; both read FBO 0 back on swap (bring-up) | a per-consumer backend selection in meson |
| `32-mesa-setfunc` | `MesaGLSetFunc(fenum, fn)`: swap one guest-dispatch entry. The macOS embed backend redirects `glBindFramebuffer(…, 0)` to its stand-in FBO | upstream exposes the table |
| `40-d3dpt-device` | instantiate the paravirtual Direct3D device (`hw/d3dpt`, overlaid from `d3dpt/hw` by prepare; doc 14) on the pc machine next to the qemu-3dfx devices and add its meson subdir. The device itself is ours, not a patch: SysBus, register page at 0xdfffe000, 64 MiB RAM window at 0xd8000000, executor library dlopened on first guest attach (`d3dpt_exec_load.c`, shared). The same overlay carries `d3dpt_vga.c`, the `d3dpt-vga` PCI framebuffer adapter for the XP display driver (doc 15): stdvga core + register BAR, 128 MiB VRAM whose top 64 MiB is the Direct3D command window of the driver's DDI (M7c), `-vga none -device d3dpt-vga` | never (our device) |
| `90-debug-sdl-keydebug` | `QEMU_SDL_KEYDEBUG=1` logs SDL keydown/modifier state (diagnostic) | any time |

Planned: dma-buf / IOSurface export instead of readback, Glide offscreen
path (doc 12).

Regenerating a patch: apply the queue, edit the file(s) in `qemu/`, produce
`diff -u` against a copy of the pre-edit state with `a/`/`b/` paths, then
re-run `prepare-qemu.sh` twice to prove it applies cleanly and is idempotent.
Patches touching overlay files (hw/3dfx, hw/mesa, embed/) must come after
the overlay is refreshed — prepare handles the order.
