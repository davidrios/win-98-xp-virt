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

## Practical notes

- Keep a `reference/` directory in the repo: CRT photo sets (with capture
  settings), ATAPI trace fixtures, benchmark results, real-hardware
  screenshots — versioned alongside the tests that consume them.
- Capture sessions to schedule: (1) CRT photo set early in M2 (shader
  calibration), (2) ATAPI traces + disc dumps before M5, (3) 3D screenshot
  set + benchmarks during M3/M4.
- The rig stays stock — it is an oracle, not a dev machine.
