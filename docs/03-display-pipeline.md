# 3. Display: pixel accuracy, CRT shaders, latency

The core promise: what the guest's video card outputs is what gets shaded and
shown — at native guest resolution, correct geometry, and with a CRT look good
enough that a 1998 game screenshot is hard to tell from a photo of a Trinitron.

Since ADR-003, presentation is split: **RetroArch owns** the shader chain
(native slang presets), scaling/viewport, vsync/frame pacing, and the window.
**Our core owns** everything that makes RetroArch's output *correct*: raw
framebuffer delivery, mode analysis, geometry reporting, and the handoff
timing. RetroArch is only as accurate as what the core tells it.

## Stages

```
guest VGA/SVGA device
  → QEMU DisplaySurface (raw framebuffer + dirty rects, in-process)
  → [core] triple-buffered handoff; upload in retro_run
  → [core] mode analysis → retro geometry/av_info + shader params
  → [RetroArch] slang shader chain (curated CRT presets)
  → [RetroArch] viewport scaling, vsync, present
```

We ship a curated preset pack with per-core defaults — "Trinitron"
(rig-calibrated, doc 09), "shadow mask consumer", "clean sharp", "off" —
instead of pointing users at the full overwhelming libretro shader tree; any
`.slangp` still works, it's stock RetroArch.

## Pixel accuracy rules (core responsibilities, all testable)

1. **Never scale before the shader.** The core hands RetroArch the exact
   guest framebuffer (640×480, 800×600, 320×200…), never a pre-stretched
   surface.
2. **Non-square pixels.** Mode 13h and friends: 320×200 is a 4:3 picture with
   1:1.2 pixel aspect. The core reports aspect from a mode table via
   `retro_game_geometry`, not width/height ratio. Same for 640×400, 720×400
   text mode, 360×240, etc.
3. **Double-scan awareness.** Real VGA double-scans low-res modes (320×200 is
   scanned as 400 lines). Scanline shaders need the *scanline count*, not the
   framebuffer height — exposed as shader parameters set by mode analysis so
   presets draw the right number of scanlines automatically.
4. **Integer scaling by default** for "clean sharp" (RetroArch integer-scale
   option in our defaults); CRT presets use free scaling at exact 4:3 (5:4
   for 1280×1024). Optional overscan crop, off by default.
5. **Mode-change fidelity.** Games and boot flows switch modes constantly;
   mode analysis re-runs on every QEMU surface change and pushes geometry
   updates without garbage frames or stretched leftovers.
6. **Color fidelity.** Palettized modes are expanded by guest device
   emulation; the core delivers sRGB and verifies no host colorspace
   mangling end-to-end.

## Latency budget

Target unchanged: **≤ 1 host frame added** between guest frame completion and
photons at 60 Hz host, measured not felt.

- In-process handoff: display listener publishes surface + dirty rects; the
  core uploads the newest complete frame each `retro_run`. QEMU never blocks
  on RetroArch; a slow frontend frame repeats the last guest frame.
- **Refresh mismatch:** guests run 70 Hz (VGA text/DOS) and 60/75/85 Hz SVGA.
  The core reports actual guest refresh in av_info; RetroArch's pacing
  handles the rest (and handles it best on 120 Hz+/VRR hosts). We do not
  resample the guest.
- RetroArch-side settings ship in our defaults: max swapchain images / frame
  delay tuned per platform once measured.
- Instrumentation from day one: dirty→upload→present timestamps in a debug
  overlay (core option), so latency regressions are caught in CI-adjacent
  manual runs.

## Input path (same budget)

- RetroArch "game focus" mode gives us raw keyboard + suppressed host
  shortcuts; the core maps libretro keyboard events to guest scancodes
  (full set, Pause/PrtSc correctness).
- Pointer: libretro relative mouse → PS/2 injection for mouselook (the point
  of the project); absolute-tablet core option for desktop mousing. Grab
  toggle follows RetroArch conventions (game focus hotkey).

## 3D and the pipeline

qemu-3dfx renders via host GL. In RetroArch this maps to a libretro
**hw-render context**: the guest's 3D output lands in the context's FBO and —
critically — **goes through the same slang chain**, so Voodoo-era games get
the CRT treatment (the money shot). Risks and the macOS story live in doc 04;
the M0 spike validates exactly this. 2D correctness never depends on the 3D
answer: framebuffer path and hw-render path are independent.

## Testing

- Golden-image tests: scripted guest boots to known screens; core output
  captured headless (libretro test harness) and compared pixel-exact with
  shaders off; geometry checked with shaders on (aspect within tolerance).
- A test floppy image cycling VGA modes for mode-table coverage.
- **Real-CRT calibration:** default presets and geometry values tuned against
  photographs of the reference rig's CRT showing the same content (doc 09).
