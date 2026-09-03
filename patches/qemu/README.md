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
| `10-embed-api` | meson: `shared_library('qemu-embed-<target>')` per softmmu target from the existing static lib + `embed/libqemu_embed.c`, `embedaudio.c`, `embedfx.c`, `mglcntx_embed.c` (+ epoxy when found); ld64 export list (`embed/libqemu_embed.symbols`) because QEMU's plugin `-exported_symbols_list` hides everything else on macOS | upstreamed embed API (aspirational) |
| `20-embed-audio` | register the `embed` audiodev: QAPI enum/union entry, `audio_template.h` per-direction case, **and `audio/audio.c` `audio_create_pdos` CASE** (missing → NULL pdo segfault) | with 10 |
| `30-3dfx-ui-vtable` | the 11 qemu-3dfx UI entry points (`mesa_*`, `glide_*`) dispatch through a `QemuFxUiOps` table (`ui/fxui.c`); SDL registers its implementations at display init, the embed library its window-less provider (`embed/embedfx.c`); no provider = contexts refused / no Glide window, VM keeps running (supersedes the spirit of 04) | upstream qemu-3dfx grows a provider seam |
| `31-mesa-ctx-weak` | `hw/mesa/mglcntx_linux.c` exports weak so `embed/mglcntx_embed.c` (EGL surfaceless + pbuffer as FBO 0, readback on swap) overrides it inside libqemu-embed while qemu-system keeps GLX | a per-consumer backend selection in meson |
| `90-debug-sdl-keydebug` | `QEMU_SDL_KEYDEBUG=1` logs SDL keydown/modifier state (diagnostic) | any time |

Planned: macOS CGL/IOSurface backend (`mglcntx_sdlgl.c` weak like 31),
dma-buf export instead of readback, Glide offscreen path (doc 12).

Regenerating a patch: apply the queue, edit the file(s) in `qemu/`, produce
`diff -u` against a copy of the pre-edit state with `a/`/`b/` paths, then
re-run `prepare-qemu.sh` twice to prove it applies cleanly and is idempotent.
Patches touching overlay files (hw/3dfx, hw/mesa, embed/) must come after
the overlay is refreshed — prepare handles the order.
