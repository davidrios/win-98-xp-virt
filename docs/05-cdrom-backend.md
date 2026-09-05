# 5. CD-ROM backend: raw images and copy protection

## Problem

QEMU's CD-ROM emulation is ISO-shaped: single data track, cooked 2048-byte
sectors. Everything a 1996–2005 game disc actually relies on is thrown away:

- **Multi-track layouts and CD-DA** — Red Book audio tracks played via ATAPI
  audio commands (in-game music for a huge share of Win9x titles).
- **Raw 2352-byte sectors** — mixed-mode discs, Mode 2 forms.
- **Subchannel data (P–W, especially Q)** — read by SecuROM.
- **Deliberate defects** — SafeDisc's unreadable/weak sectors must return
  errors *at the right LBAs*; a drive that reads everything cleanly fails the
  check.
- **Raw TOC / session layout** — multisession and protection fingerprinting.
- **Data position measurement (DPM)** — StarForce-class checks measure sector
  angular position/timing; images carrying DPM data (MDS) can satisfy them.

Goal: mount a raw dump of a disc you own and have the *unmodified* protection
code in the guest pass, because the virtual drive is indistinguishable from a
period drive with that disc inserted. This is preservation-grade drive
emulation — the DRM runs and succeeds; nothing is patched, stripped, or
bypassed, and no-CD/crack functionality is out of scope.

## Implementation

Doc 17 is the implementation spec (model, formats, the C API header, MMC
byte layouts, the `cdimage` block driver, the ATAPI patch, CD-DA, tests);
the ordered plan is `docs/tracks/m5-cdrom-backend.md`. Two refinements of
the design below, decided 2026-09-04: the QEMU side is a *format block
driver* (`-cdrom game.cue` probes to it, medium swap stays QMP
`blockdev-change-medium`) and the MMC response bytes are built in Rust, the
C in `hw/ide/atapi.c` only moves buffers. Copy-protection fidelity for
SafeDisc comes from verifying EDC/ECC on every cooked read exactly as a
drive would, not from annotating bad sectors.

## Prior art

- **CDEmu/libmirage (Linux):** proves the approach end-to-end against real
  protection drivers; libmirage (GPL-2.0+) parses all relevant formats and
  models the disc as tracks/sectors/subchannel with on-the-fly generation of
  whatever a format lacks.
- **86Box/DOSBox-X:** implement cue/bin + CD-DA + some raw commands inside
  their own drive emulation (good reference for ATAPI behavior with real
  Win9x drivers).
- QEMU has **none** of this; that's the gap we fill.

## Design

A new QEMU block driver + ATAPI behavior work, structured for upstreaming:

```
image file (cue/bin, ccd/img/sub, mds/mdf, chd, iso)
   → disc model layer ("libdisc"): sessions, tracks, indices,
     per-sector raw data + subchannel + error/DPM annotations
   → QEMU block driver exposing a "raw optical" interface
   → ATAPI device layer: full MMC command surface against the disc model
```

- **Disc model layer ("libdisc"): implemented in Rust** (ADR-004) as a crate
  with a C API, built as a staticlib linked into the QEMU fork; the ATAPI
  device code in C stays a thin shim over it. libmirage is the *behavioral
  reference* (readable GPL source, format knowledge, protection handling),
  not a dependency — this sidesteps its glib/API-shape friction with QEMU's
  block layer, and format parsing is exactly the workload where Rust pays for
  itself. QEMU upstream's experimental Rust support keeps eventual
  upstreaming of libdisc realistic. The MMC exerciser tests libdisc directly
  as a normal Rust dev-dependency, no QEMU involved.
- Formats priority: **cue/bin → CCD (img+sub) → CHD → MDS/MDF (incl. DPM)**.
  ISO keeps working through the same path.
- Missing data is synthesized like real hardware would produce it: subchannel
  Q generated from the TOC when no .sub file exists, error-free C2 for clean
  sectors, etc. Protection-relevant data is only as good as the dump — docs
  must be explicit that SafeDisc needs a dump that recorded the bad sectors,
  SecuROM needs subchannel, StarForce needs DPM.

### ATAPI/MMC command coverage (the actual work)

Beyond what QEMU has today:

- `READ CD` (0xBE) / `READ CD MSF` (0xB9): all sector types, raw 2352,
  subchannel selection bits, **C2 error pointer reporting**.
- `READ SUB-CHANNEL` (0x42): current position + Q from the model.
- `READ TOC/PMA/ATIP` (0x43): all formats including **raw TOC (format 2)**
  and multisession.
- Audio: `PLAY AUDIO (10/12/MSF)`, `PAUSE/RESUME`, `STOP`, `SCAN`;
  mode page 0x0E (volume/routing). Audio rendered into QEMU's audio backend
  mixed as the "analog" CD output (period-correct), sample-accurate seek.
- Error semantics: defective sectors return the right sense codes
  (medium error / L-EC uncorrectable) with plausible retry timing.
- `GET CONFIGURATION` / mode page 0x2A capabilities that match a period
  CD/DVD-ROM drive profile (protection code sometimes sniffs capabilities).
- Optional (later): coarse seek/read timing model — some checks are timing
  sensitive (DPM especially); start with honest-latency, add a period drive
  timing profile if StarForce-class checks demand it.

### Guest visibility

- Win98 and XP see a bog-standard IDE/ATAPI CD-ROM — inbox drivers, no guest
  software needed. That is the entire point: protection drivers
  (secdrv.sys and friends) run unmodified.
- Frontend UX: mount/eject/swap disc images at runtime (multi-disc installs),
  with a per-machine virtual "disc shelf".

## Acceptance tests (M5 exit criteria)

Using dumps of discs we own, one title per scheme:

| Scheme | Expectation | Result |
|---|---|---|
| Plain mixed-mode + CD-DA | installs; in-game CD audio plays, tracks/seek correct | **PASS** — Age of Empires Gold and Moto Racer both play their CD soundtracks while the game runs, in XP, in the player, from their `.mds` (user, 2026-09-05). Step 6 had proven CD-DA host-side and through MCI; this is the first time a title's *own* audio code drove the path |
| **SafeDisc 2.x** | launch check passes from raw dump with error sectors | **INCONCLUSIVE** (was PASS; downgraded 2026-09-05). FIFA 2002 from `FIFA2002.mds` installs, launches and reaches its menus in XP — **but it does the same from the repaired copy, whose 584 weak sectors all verify** (user). A check that ignores the band's absence is not a check we have evidence of passing. Nothing here shows the error delivery is what satisfied it; see doc 17 §2.6 |
| SafeDisc 1.x | *(the row's premise does not hold)* | **n/a** — 1.x writes no error sectors at all (doc 17 §6.x, measured on The Sims and Rayman 2). It checks the medium another way; a separate expectation has to be written once we know which |
| SecuROM (early + 4.x) | launch check passes from dump with subchannel | not tested. Early needs a `.sub` (replay path exists); 4.x needs DPM, which `mds.rs` ignores — that is the stretch goal below, not this row |
| VOB ProtectCD | launch check passes from a dump carrying both the data and the Q anomaly | **INCONCLUSIVE** (was PASS; downgraded 2026-09-05). The Settlers 3 plays in XP from the `.ccd` and the `.cue`, both discs — **and from the repaired CD01 whose 538-sector band verifies** (user). The earlier cue/ccd A/B is void with it: it concluded the check reads the data anomaly rather than Q, but the control shows it does not read the band at all. See doc 17 §2.6 |
| StarForce (stretch) | documented result with DPM-carrying MDS dump | not started |
| Multisession disc | both sessions visible, TOC correct | not tested |
| Multi-disc title | the guest sees a disc change and the game accepts the new disc | **PASS** — Settlers 3's campaign asked for CD2 and took it (user, 2026-09-05). Unaffected by the protection finding: this row is about the medium change, and the game demonstrably distinguished the two discs |

Plus a synthetic MMC exerciser (host-side unit tests against the disc model,
no guest needed) for command-level regression coverage. The exerciser's
fixtures include **golden ATAPI traces captured from the reference rig's real
drive** while protected titles run their checks (doc 09) — so we verify the
backend against the command sequences protections actually issue. Test dumps
are made from owned discs that verifiably pass on the real machine, giving a
pass/fail oracle for every VM failure.
