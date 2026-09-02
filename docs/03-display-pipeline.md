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
- Instrumentation from day one: dirty→upload→present timestamps in a debug
  HUD, so latency regressions are measured.

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
