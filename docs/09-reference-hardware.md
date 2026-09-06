# 9. Reference hardware rig

We have access to a real period machine: **Pentium 4, GeForce 6200, dual-boot
Windows 98 / Windows XP, CRT monitor, real optical drive.** This is the
ground truth the emulated stack is judged against. Each pillar gets concrete
comparisons instead of guesses.

The monitor is a **Samsung SyncMaster 753DFX**: 17" (≈16" viewable, ≈320×240 mm
of picture), Samsung DynaFlat — flat glass, and a **delta dot-trio shadow mask
at ≈0.20 mm horizontal pitch**, not an aperture grille. That matters for the
preset pack: this tube has no damper wires and no vertical stripe, so a
Trinitron-style grille preset is the wrong default for it (doc 03's pack names
are corrected accordingly). `shaders/syncmaster-753dfx.slangp` approximates it
from that geometry; the photo set below is what would turn the approximation
into a calibration. **Unverified:** the dot pitch and the viewable width are
recalled from the model's class, not read off the monitor, and everything
derived scales with them — measure the picture width and check the pitch in
the manual before treating the numbers as fixtures.

## What it validates, per pillar

### Display pipeline (doc 03) — the biggest win

- **CRT look calibration:** photograph the CRT (tripod, fixed exposure,
  macro shots of the mask/scanlines plus full-screen shots) showing known
  test content — DOS text mode, Win98 desktop at 640×480/800×600/1024×768,
  320×200 game content — and tune our default shader presets against the
  photos. "Period accurate" becomes side-by-side, not vibes.
- **Geometry truth:** confirm real-world aspect for the tricky modes
  (320×200 filling a 4:3 tube, 720×400 text, double-scan appearance of
  low-res modes) against our mode table's pixel-aspect and scanline-count
  values.
- **Motion/refresh feel:** 70 Hz DOS content and 60/75/85 Hz SVGA on the real
  tube as a reference for judging our frame pacing on modern displays.

### CD-ROM backend (doc 05)

- **Golden ATAPI traces:** run a logging tool on the real machine (or the
  same drive in a modern box) to capture MMC command/response/sense sequences
  while protected titles (SafeDisc, SecuROM) perform their disc checks. Those
  traces become fixtures for the synthetic MMC exerciser — we can verify our
  virtual drive answers the *actual command sequence* the protection issues,
  not just our reading of the spec.
- **Known-good dumps:** dump the owned discs on hardware we control, with
  subchannel/error data, so acceptance-test inputs are traceable to a real
  disc that verifiably passes its check on the real machine.
- **Behavior A/B:** any title that fails in the VM but passes on the rig is a
  backend bug by definition — an oracle most emulation projects never have.

### 3D acceleration (doc 04)

- **Rendering correctness:** screenshot the acceptance-matrix titles on the
  GeForce 6200 (native D3D8/9 and OpenGL) and diff against our
  WineD3D-wrapper output. Wrapper bugs (fog, alpha test, texture stage
  weirdness) show up as visible deltas against real-hardware captures.
- Note the 6200 is a 2004 DX9 card: it is the right oracle for the XP era and
  late-Win98 D3D titles; early Glide-era output has no Voodoo oracle here
  (acceptable — 86Box and community references cover that).

### Guest machines & performance (doc 06)

- **Honest baselines:** benchmark the rig (3DMark99/2001SE/03, game timedemos)
  so in-app performance expectations are phrased as "vs. a real P4 +
  GeForce 6200" with actual numbers. Especially valuable for calibrating the
  XP-on-Apple-Silicon TCG verdict in M1: "X% of the reference P4" is a
  meaningful, testable claim.
- **Driver/OS behavior reference:** install-flow quirks, control-panel
  behavior, CD autorun etc. checked against real Win98/XP when a VM behavior
  looks suspicious.

## The CRT photo set (what to shoot, and how)

The patterns are `guest-tools/src/crtcal.h`, one definition compiled into both
sides: `GAMEDIR\CRTCAL.EXE` on the guest-tools ISO puts them on the tube at the
exact mode through an exclusive full-screen DirectDraw primary — no blit, no
stretch, because a scaler is what would otherwise be measured — and
`tools/crtcal-render` writes the same pixels as BMPs, which
`player --shader <preset> --calib <dir>` runs through the preset. One
photograph, one shaded frame, side by side; adjust the preset; repeat.

On the rig: `CRTCAL.EXE [w h [bpp]]`, then SPACE / 1–8 to step patterns, `M`
for the next mode, `L` to take the legend away, ESC to quit.

| # | Pattern | What it settles | The shot |
|---|---|---|---|
| 1 | `grid` | does the mode fill 4:3, how much falls off each edge, is the geometry linear | whole screen, straight on, lens level with the middle of the tube |
| 2 | `scanlines` | the beam's vertical profile, and **how many scanlines the tube really draws** | macro on a band centre; one whole-screen frame too |
| 3 | `mask` | mask kind, pitch in mm, the stagger | macro, as close as the lens focuses, **with a ruler in the frame** |
| 4 | `bloom` | how much the beam widens as it brightens | macro across the stack, one exposure for all rows |
| 5 | `sharp` | horizontal spot size, where the video bandwidth gives out | macro on the bar bands, repeated at every mode |
| 6 | `halation` | how far light spreads into black | whole screen, dark room, fixed exposure, unchanged between shots |
| 7 | `gamma` | the tube's gamma, against a dithered reference | whole screen, straight on; slightly defocused is right |
| 8 | `colour` | phosphor primaries and colour temperature | whole screen, fixed white balance (daylight), never auto |

The two that pay for the trip are **2 at 320×200** — it is the direct answer to
whether the tube really draws 400 scanlines for a 200-line mode, which doc 03
rule 3 asserts and the shader is now told — and **3 with a ruler**, which turns
the ≈0.20 mm dot pitch above from a recollection into a measurement.

### Getting the shot

- **Shutter ≥ 2 frame periods.** A CRT is only ever lit where the beam is; a
  fast shutter photographs a band, not a picture. At 85 Hz use 1/30 s or
  slower, and never a flash. This is the one mistake that ruins a whole set.
- **Manual everything.** Fixed exposure, fixed white balance (daylight), fixed
  ISO, manual focus. The halation and bloom patterns are only comparable if
  the exposure did not move between them; write it down.
- **Tripod, straight on, dark room.** For `grid` the lens goes level with the
  centre of the tube — off-axis makes a linear picture look like pincushion.
- **A ruler in the macro frames**, taped flat to the glass, in focus with the
  phosphors. Without a scale a mask photo says the mask's shape but not its
  pitch, and the pitch is the number everything else is derived from.
- **RAW if the camera has it**, and record the monitor's OSD settings —
  brightness, contrast, colour temperature preset, and whether moiré reduction
  is on (it must be **off**: it defocuses the beam on purpose).
- **Let it warm up** twenty minutes; a cold tube has not settled its geometry.
- One whole-screen frame per pattern per mode, plus the macros. `reference/`
  in the repo is where they go, with the capture settings in the filename or a
  sidecar note.

Doc 03's "Mode analysis" section says which shader parameters each of these
feeds, and `shaders/syncmaster-753dfx.slangp` says which of its values are
derived from the tube's geometry and which are waiting on these photographs.

## Practical notes

- Keep a `reference/` directory in the repo: CRT photo sets (with capture
  settings), ATAPI trace fixtures, benchmark results, real-hardware
  screenshots — versioned alongside the tests that consume them.
- Capture sessions to schedule: (1) CRT photo set early in M2 (shader
  calibration), (2) ATAPI traces + disc dumps before M5, (3) 3D screenshot
  set + benchmarks during M3/M4.
- The rig stays stock — it is an oracle, not a dev machine.
