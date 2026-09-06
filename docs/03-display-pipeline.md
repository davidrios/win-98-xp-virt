# 3. Display pipeline: pixel accuracy, CRT shaders, latency

The core promise: what the guest's video card outputs is what gets shaded and
shown — at native guest resolution, correct geometry, and with a CRT look good
enough that a 1998 game screenshot is hard to tell from a photo of a Trinitron.
We own this pipeline end to end (ADR-005): winit window, wgpu, librashader.

## Stages

```
guest VGA/SVGA device
  → QEMU DisplaySurface (raw framebuffer + dirty rects, in-process)
  → [render thread] texture upload (dirty-rect aware)
  → mode analysis (resolution, pixel aspect, scanline count)
  → librashader filter chain (slang preset: CRT shader of choice)
  → geometry stage (aspect correction, integer/sharp scaling, overscan)
  → wgpu present (Metal / Vulkan / D3D12)
```

The librashader chain follows RetroArch shader semantics (original-resolution
input, final viewport params, per-pass scaling), so the existing slang
presets — `crt-royale`, `crt-guest-advanced`, `crt-lottes` — work unmodified.
We ship a curated pack with 3–4 defaults ("Trinitron" calibrated against the
reference CRT per doc 09, "shadow mask consumer", "clean sharp", "off") and
accept any `.slangp`.

Implementation status (2026-09-05): the chain runs in `player/src/shader.rs`
— guest texture in, viewport-sized output texture out, then the blit pass;
`PLAYER_DUMP_OUT` reads the shaded texture back for tests. Mode analysis
(rules 2 and 3 below) landed in `player/src/mode.rs`; the rest of M2 —
geometry updates driven off the QEMU surface change rather than the frame,
overscan crop, the curated preset pack, and the mode table the M7 device is
fed from the player — is still open.

## Pixel accuracy rules (all testable)

1. **Never scale before the shader.** The shader input is the exact guest
   framebuffer (640×480, 800×600, 320×200…), never a pre-stretched surface.
2. **Non-square pixels.** Mode 13h and friends: 320×200 is a 4:3 picture with
   1:1.2 pixel aspect. The geometry stage applies pixel aspect from a mode
   table, never width/height ratio. Same for 640×400, 720×400 text mode
   (9-dot characters), 360×240, etc.
3. **Double-scan awareness.** Real VGA double-scans low-res modes (320×200 is
   scanned as 400 lines). Scanline shaders need the *scanline count*, not the
   framebuffer height — set as shader parameters by mode analysis so presets
   draw the right number of scanlines automatically.
4. **Integer scaling by default** for "clean sharp"; CRT presets use free
   scaling at exact 4:3 (5:4 for 1280×1024). Optional overscan crop, off by
   default. (M0 player already does centered integer 4:3 via viewport.)
5. **Mode-change fidelity.** Games and boot flows switch modes constantly;
   mode analysis re-runs on every QEMU surface change; no garbage frames or
   stretched leftovers during the transition.
6. **Color fidelity.** Palettized modes are expanded by guest device
   emulation; the surface is treated as sRGB end-to-end (XRGB8888 uploads as
   BGRA8 with no swizzle); wide-gamut host displays get correct sRGB mapping.

## Mode analysis (rules 2 and 3, landed 2026-09-05)

`player/src/mode.rs` turns a guest framebuffer size into what it meant on a
monitor of the era: the display aspect the picture was meant to fill, the
number of lines the CRT actually scanned, and whether the CRTC double-scanned
to get there. The player re-runs it whenever the surface size changes
(`Gpu::update_mode`) and prints one line per mode change:

```
[display] mode 320x200 VGA 320x200 (mode 13h) — 4:3 picture, pixel aspect 0.833, 400 scanlines (double-scanned)
[shader] mode parameters vga_mode=1 inter=800
```

The table is an exception list, not a lookup. An unlisted size is taken to
have square pixels — right for every SVGA mode and for anything modern a guest
might set, where forcing 4:3 would distort a widescreen one, and identical to
4:3 for every 4:3 mode that has square pixels anyway. What the table must carry
is the modes where the two disagree: the VGA's 200-, 240-, 350- and 400-line
modes, whose pixels are not square. Double-scanning is a rule rather than an
entry — the CRTC sets its bit below ~300 lines. The rest of the entries are
there for their names and to be swept, and are where a correction goes when one
is measured against the reference CRT (doc 09).

**Geometry (rules 2 and 4).** The viewport is the largest rect of the *mode's*
display aspect that fits, with the width following from the aspect and the
height an integer multiple of the mode's **scanlines** — not of its rows. On a
double-scanned mode the two differ, and it is the scanline pitch that has to
come out whole: 320×200 in a 2400-line surface is 6 pixels per scanline
quantised on scanlines and 5.5 quantised on rows. Every whole scale of the
scanlines is a whole scale of the rows too, so this only refines the older
rule. Square-pixel 4:3 modes are unchanged; 320×200 goes from the old 1.6:1
stretch to 4:3, and so do 640×350, 640×400, 720×400 text and the mode X sizes.

**Scanline count (rule 3).** A preset derives its scanline count from the input
texture's height, or guesses from a resolution threshold. Both are wrong here,
so mode analysis states the answer through the preset's own parameters:
`vga_mode` ("VGA Single/Double Scan mode") switches crt-guest-advanced from its
console-oriented interlace guess to the two VGA cases, and `inter` then picks
between them — the preset takes the double-scan branch when `inter` is above
the source height and the single-scan branch when it is at or below. Its
default of 375 is a guess about which side of that line a source falls on;
mode analysis knows, so it says so.

Measured through the real chain, `crt-guest-advanced.slangp`, counting the
scanlines in the frame the preset actually drew (`--mode-sweep`, and
`PLAYER_MODE_PARAMS=0` for the control):

| Mode | Tube | Drawn, mode analysis | Drawn, control |
|---|---|---|---|
| 320×200 | 400 | **400** | 200 |
| 320×240 | 480 | **480** | 240 |
| 640×200 | 400 | **400** | 200 |
| 640×350 | 350 | **350** | 350 |
| 640×400 | 400 | **400** | 200 |
| 720×400 | 400 | **400** | 200 |
| 640×480 | 480 | **480** | 240 |
| 800×600 | 600 | **600** | 300 |
| 1024×768 | 768 | **768** | 384 |

The control is wrong in two different ways at once, which is why both halves of
the fix are needed. Below the 375-line trigger it draws one scanline per guest
row — the double-scan bug, 200 lines where the tube scanned 400. At or above
it, it decides the source is interlaced and draws half the lines of one field:
640×480 at 240, 1024×768 at 384. Only 640×350 (short enough not to be called
interlaced, and genuinely single-scanned) is right by accident.

**The limitation, stated plainly.** This works for presets that expose a
resolution override. crt-lottes and crt-royale derive their scanline period
from `OriginalSize` and have no such parameter, and rule 1 forbids the obvious
workaround of handing them a pre-doubled surface. The player says so once and
leaves those presets at their defaults:

```
[shader] this preset exposes no scanline-count parameter (vga_mode, inter): a double-scanned
mode will be drawn with one scanline per guest row instead of the two per row the tube drew
```

Which way that gets closed — per-mode `.slangp` variants in the curated pack,
or restricting the pack to presets that can be told — is open, and belongs with
the pack itself (M2).

## The mode sweep

`player --mode-sweep <dir>` runs the display path over every mode in the table
plus one unlisted size, with no guest and no QEMU: the geometry test image (a
circle drawn in display space, round on screen only when the pixel aspect is
applied; a block of one-pixel lines; SMPTE bars) is uploaded at each size and
put through the real chain. Each mode is checked for the on-screen aspect, for
fitting the surface, for a whole-pixel scanline pitch, for the parameters
reaching the preset, and — by counting the bright bands down one column of the
frame it just read back — for the scanline count. A PNG per mode lands in
`<dir>`. It is the `mode-sweep` check in `scripts/test.sh`, about 2 s.

Two details that had to be got right for the check to mean anything:

* It renders to a **fixed 3200×2400 surface**, not the window's, and never
  presents or even acquires a swapchain image. What it checks must not depend
  on what the compositor handed us, and an occluded window — a test run behind
  a terminal, which is the normal case — blocks the second acquire forever
  under FIFO.
* The scanline count is taken from the **pitch between the first and last
  band**, not from a repeat period or a band count over the sampled strip:
  the pitch need not be a whole number of pixels, and where it is not, the
  frame's *period* is two scanlines. Below three output pixels per scanline
  the preset has no room for a gap and draws a flat field (one LSB of
  modulation, measured at 1152×864 and above), so the count is not checked
  there — which costs nothing, since those are square-pixel modes whose
  scanline count is their height.

## Latency budget

Target: **≤ 1 host frame added** between guest frame completion and photons at
60 Hz host — measured, not felt.

- In-process handoff: display listener publishes surface + dirty rects;
  render thread uploads immediately. No encode, no copy chains.
- **Present mode:** mailbox where available (newest frame wins), otherwise
  vsync FIFO; `desired_maximum_frame_latency = 1`.
- **Refresh mismatch:** guests run 70 Hz (VGA text/DOS) and 60/75/85 Hz SVGA
  on 60/120/144/ProMotion hosts. We present the newest complete guest frame
  at host vsync, never resample. 120 Hz+ hosts pace 60 Hz content perfectly;
  VRR passthrough is a later nicety.
- **Occlusion:** an occluded/minimized window may get no presents (Wayland
  frame callbacks, macOS occlusion) — the render loop skips frames on
  `Occluded`/`Timeout` and must never let QEMU stall behind it.
- **Publish-driven rendering, acquire before sampling:** the QEMU refresh
  tick wakes the event loop (`EventLoopProxy`); the redraw first acquires
  the swapchain image (blocks until one is free under FIFO), *then* samples
  the newest guest frame, uploads and presents. Sampling before the acquire
  aged every frame by a host vblank whenever the queue was saturated — the
  16 ms tick is slightly faster than 60 Hz, so on Metal it always was
  (≈32 ms publish→present on the M1 Air, 2.5 ms with a 30 ms tick that
  never saturated). Newest frame wins, mailbox semantics on top of FIFO.
- Instrumentation from day one: dirty→upload→present timestamps in a debug
  HUD, so latency regressions are measured. `PLAYER_LATENCY=1` reports
  publish→present-return; with a vsync-throttled present its floor at 60 Hz
  is uniform 0–16.7 ms (p50 ≈ 8, max ≈ 17). Photons follow at the next
  scan-out. Guest-draw→publish (0–`PLAYER_REFRESH_MS`) is before the
  measured window and does not show up in this number.

## Input path (same budget)

- winit event → QEMU input injection directly on the event thread.
- Pointer: absolute (USB tablet) for desktop use; **relative capture mode**
  (PS/2 semantics + host cursor grab) for mouselook — the Win98/XP FPS case is
  the whole point. Hotkey toggles grab.
- Keyboard: full scancode set (Pause/PrtSc correctness); host shortcuts
  suppressed while grabbed.

## 3D and the pipeline

qemu-3dfx renders guest 3D via host OpenGL on QEMU threads. Its output must
reach the wgpu render thread as a texture so it flows through the same
librashader chain (a Voodoo-era game on a CRT shader is the money shot).
Interop per platform: IOSurface (macOS GL↔Metal), dma-buf/external memory
(Linux GL↔Vulkan), shared handles (Windows GL↔D3D12). A texture *copy* is
acceptable; a CPU readback is the failure line. This is Spike A
(docs/spikes/spike-a-macos.md). 2D correctness never depends on the answer.

## Testing

- Golden-image tests: scripted guest boots to known screens; player output
  captured offscreen (wgpu readback in test mode) and compared pixel-exact
  with shaders off; geometry checked with shaders on.
- A test floppy image cycling VGA modes for mode-table coverage.
- **Real-CRT calibration:** default presets and geometry values tuned against
  photographs of the reference rig's CRT showing the same content (doc 09).
