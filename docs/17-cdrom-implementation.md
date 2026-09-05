# 17. CD-ROM backend: implementation spec (M5, doc 05)

Doc 05 says *what* the drive emulation must do and why. This document says
*how*, precisely enough that a session can implement it step by step
without re-deriving decisions. The ordered plan, with acceptance criteria
per step, is the track doc `docs/tracks/m5-cdrom-backend.md`. Read both
before writing code. Everything here was checked against the pinned QEMU
tree (v9.2.4) on 2026-09-04; line numbers are approximate, function names
are exact.

## 1. Shape

```
image on disk            libdisc (Rust crate, staticlib, C API in libdisc/libdisc.h)
 .cue+.bin / .ccd+.img+.sub    disc model: sessions, tracks, indices, per-sector kind,
 / .iso / (.mds+.mdf, .chd)    raw ⇄ cooked synthesis, EDC/ECC, subchannel Q, MMC responders
        │                                  │
        ▼                                  ▼
 qemu/block/cdimage.c  ── a QEMU *format* block driver (C, ~300 lines):
   cooked 2048-byte view through bdrv_co_preadv (qemu-img, SeaBIOS El Torito,
   blockdev-change-medium, eject, snapshots all keep working), plus
   cdimage_disc(bs) → the libdisc handle for whoever wants the raw model
        │
        ▼
 qemu/hw/ide/atapi.c ── patch 51: every command asks `cdimage_disc(blk_bs(s->blk))`;
   NULL → today's code path, byte for byte (plain ISO on the `raw` driver);
   non-NULL → sector reads, TOC, subchannel, capabilities, CD-DA from the model
        │
        ▼
 guest: an ordinary IDE/ATAPI CD-ROM (inbox drivers, protection drivers unmodified)
```

Decisions this fixes (all consistent with doc 05 / ADR-004; do not reopen):

1. **Integration is a block driver, not a device property.** `-cdrom game.cue`
   probes to `cdimage`; `-drive file=game.cue,format=cdimage,media=cdrom`
   is the explicit form. The drive stays QEMU's `ide-cd`. Medium swap is QMP
   `blockdev-change-medium` as today (`device: ide1-cd0`).
2. **The MMC response bytes are computed in Rust**, not in C. atapi.c copies
   buffers and drives the IDE state machine; libdisc builds TOCs, subchannel
   replies and READ CD sector layouts. This is what makes the host-side
   exerciser meaningful: it tests the exact bytes the guest will see.
3. **Reads are synchronous** (`pread` on immutable files inside libdisc).
   Disc images are ≤ 900 MB and the guest reads at most 128 KiB per command;
   no AIO in M5. If profiling ever shows stalls, mmap the payload files
   inside libdisc — the C API does not change.
4. **Copy-protection fidelity comes from modelling the drive, not from
   lists of bad sectors.** A raw dump of a SafeDisc disc contains sectors
   whose EDC/ECC are wrong on purpose. We verify L-EC on every cooked read of
   a Mode 1 / Mode 2 form 1 sector and fail exactly like a drive would
   (MEDIUM ERROR, L-EC uncorrectable). No annotation file, nothing patched,
   nothing bypassed.
5. **Plain `.iso` stays on QEMU's `raw` driver by default** (probe score 0 for
   ISO), so the existing behaviour is bit-identical and stays the regression
   baseline. `format=cdimage` on an `.iso` is allowed and the exerciser
   compares the two paths.
6. **Subchannel is deinterleaved (P..W, 12 bytes each) inside libdisc**, the
   CloneCD `.sub` layout; the MMC "raw" (interleaved) form is produced only
   inside the READ CD responder.

## 2. The libdisc crate

`libdisc/` is a workspace member already (`crate-type = ["rlib",
"staticlib"]`, no dependencies — keep it that way, no crates.io deps; the
staticlib is linked into QEMU). Layout after M5a:

| File | Contents |
|---|---|
| `src/lib.rs` | the model types below, `open(path) -> Result<Disc>` dispatching on extension + content |
| `src/msf.rs` | exists: `Msf` ⇄ LBA (LBA 0 = MSF 00:02:00) |
| `src/cue.rs` | cue sheet parser → model (§2.3) |
| `src/ccd.rs` | CloneCD `.ccd` parser → model incl. raw TOC entries (§2.4) |
| `src/iso.rs` | single Mode 1 track over a cooked image |
| `src/sector.rs` | raw ⇄ cooked: sync, header, subheader, EDC placement, `SectorKind` detection from a raw sector |
| `src/ecc.rs` | EDC (CRC-32) and RSPC P/Q parity, `verify_mode1`, `verify_mode2f1` (§2.5) |
| `src/subq.rs` | Q-channel synthesis + CRC-16 (§2.6), P-channel |
| `src/mmc.rs` | responders: READ TOC formats 0/1/2, READ SUB-CHANNEL, READ CD sector layout, READ DISC INFORMATION (§4) |
| `src/capi.rs` | `#[no_mangle] extern "C"` functions matching `libdisc/libdisc.h` exactly (§3) |
| `src/bin/discx.rs` | the exerciser (§6.1): synthetic images, self-test through the C API, `dump`, `convert` |
| `qemu/cdimage.c`, `qemu/cdimage.h` | the QEMU block driver, overlaid into the tree by `prepare-qemu.sh` (§5) |
| `libdisc.h` | the one C header (§3), overlaid next to it |

### 2.1 Model

Replace the M0 placeholder types in `lib.rs` (nothing uses them yet):

```rust
pub struct Disc {
    pub sessions: Vec<Session>,        // 1-based session numbers, ascending
    pub mcn: Option<[u8; 13]>,         // CATALOG
    pub raw_toc: Option<Vec<TocEntry>>, // CCD [Entry] records verbatim; None = synthesize
    files: Vec<Payload>,               // opened files (bin/img/sub/wav), pread-able
}
pub struct Session { pub number: u8, pub tracks: Vec<Track>, pub leadout_lba: i32 }
pub struct Track {
    pub number: u8,                    // 1..=99
    pub mode: TrackMode,               // Audio, Mode1, Mode2 (form per sector), Mode2Form1, Mode2Form2
    pub control: u8,                   // Q control nibble: audio 0x0 (|1 pre-emphasis, |2 DCP), data 0x4
    pub isrc: Option<[u8; 12]>,
    pub indices: Vec<(u8, i32)>,       // (index number, absolute LBA of its first sector), index 1 always present
    pub start_lba: i32,                // == indices[index 1]
    pub end_lba: i32,                  // exclusive: next track's index 0/1 start, or the session lead-out
    pub extents: Vec<Extent>,          // contiguous coverage of [first index .. end_lba)
}
pub struct Extent {
    pub lba: i32, pub count: u32,
    pub source: Source,                // File{file, offset, stride, layout} | Silence | ZeroData
    pub sub: Option<SubSource>,        // File{file, offset} (96 B/sector, deinterleaved) or None = synthesize
}
pub enum Layout { Cooked2048, Mode2_2336, Raw2352, Raw2352Sub96 /* .bin with interleaved sub appended */ }
pub enum SectorKind { Audio, Mode1, Mode2Form1, Mode2Form2, Mode2Formless, Gap }
pub struct SectorInfo { pub kind: SectorKind, pub track: u8, pub index: u8, pub lec_ok: Option<bool> /* data sectors only */ }
```

Rules:
- LBAs are `i32`; the program area starts at LBA 0 (MSF 00:02:00). The
  disc's readable range is `[0, last session lead-out)`. Anything else is
  `Err(Range)`; libdisc never invents lead-in data.
- `start_lba` of track 1 is 0 for every format we read (the mandatory
  150-sector pregap of track 1 is *not* in the file and is never addressed).
- The lead-out LBA of the last session is what `sector_count()` returns
  and what READ CAPACITY / TOC point A2 use.
- Sector reads go through one function: `Disc::read_raw(lba) -> [u8; 2352]`
  (stored raw, or synthesized from cooked per §2.5) and
  `Disc::read_sub(lba) -> [u8; 96]` (stored, or synthesized per §2.6).
  `read_cooked(lba)` = raw → kind detection → L-EC verify → user data, or
  `Err(Medium)` / `Err(Mode)` (audio / gap).

### 2.2 Byte conventions (get these right once)

- **Header MSF and Q-channel times are BCD** (`0x21` = 21). MMC TOC /
  subchannel *replies* carry binary MSF or 32-bit big-endian LBA.
- Absolute MSF in a sector header = MSF of `lba + 150`.
- CD-DA samples: 16-bit little-endian, left then right, 588 stereo frames
  per sector (2352 bytes). `FILE … MOTOROLA` in a cue is big-endian: swap.
  `FILE … WAVE`: parse the RIFF chunks, use the `data` chunk offset (not a
  fixed 44), require 44100 Hz / 16-bit / stereo, else refuse the image.
- Raw sectors in dumps are **descrambled** already (drives descramble); we
  never scramble or descramble.
- Sync pattern: `00 FF FF FF FF FF FF FF FF FF FF 00`. Header: `MIN SEC
  FRAME MODE` (BCD, BCD, BCD, 1 or 2). Mode 2 subheader: 8 bytes at 16..24
  (form 2 = bit 5 of byte 18 (`submode`) set).

### 2.3 Cue sheet parsing (`cue.rs`)

Grammar to accept (case-insensitive keywords, quoted or bare filenames,
`REM` lines ignored, CRLF or LF):

```
CATALOG 1234567890123
FILE "name" BINARY|MOTOROLA|WAVE
  TRACK nn AUDIO|MODE1/2048|MODE1/2352|MODE2/2336|MODE2/2352|CDI/2336|CDI/2352
    FLAGS [DCP] [4CH] [PRE] [SCMS]
    ISRC XXXXXXXXXXXX
    PREGAP mm:ss:ff
    INDEX 00 mm:ss:ff
    INDEX 01 mm:ss:ff
    INDEX nn mm:ss:ff
    POSTGAP mm:ss:ff
```

Semantics (these are where cue parsers go wrong):
- `INDEX` times are **relative to the start of the current FILE**, in
  MSF without the 150 offset (`00:00:00` = the file's first sector).
  Absolute LBA = (running LBA at file start) + index frames.
- A track's sectors in the file run from its lowest index (00 if present,
  else 01) to the next track's lowest index in the same file, or to the end
  of the file. The file's sector count = file size ÷ stride (2048 / 2336 /
  2352; a size not divisible by the stride is an error naming the file).
- `PREGAP` = that many sectors **not in the file**, synthesized before index
  01 (audio: digital silence; data: zero-filled sectors of the track's
  mode with valid EDC/ECC). They belong to the track as index 00. `POSTGAP`
  = synthesized after the last sector. Absolute LBAs advance across them.
- Multiple `FILE` lines: files are concatenated in order; the running LBA
  carries over.
- Track 1 must start at absolute LBA 0 after the above (a cue whose track 1
  has `INDEX 00 00:00:00` and `INDEX 01 00:02:00` is the common
  "pregap in file" case: index 00 covers LBA 0..149, index 01 starts at 150;
  accept it as is).
- Control nibble: AUDIO → 0x0, plus 0x1 if `PRE`, 0x2 if `DCP`; data → 0x4.
  `4CH` → 0x8 (never seen in practice; carry it anyway).
- Payload file resolution: relative to the cue's directory; if not found,
  try case-insensitively (dumps made on Windows), then error naming the
  path tried.
- The session lead-out LBA = end of the last track. Cue sheets are
  single-session (`REM SESSION` extensions: ignore in M5a, note in the
  error message if a second session is declared).

### 2.4 CloneCD parsing (`ccd.rs`)

`.ccd` is an INI file. Sections and keys to read (others ignored):

```
[CloneCD]  Version=3
[Disc]     TocEntries=n  Sessions=n  DataTracksScrambled=0  CDTextLength=0  CATALOG=…
[Session n] PreGapMode=n PreGapSubC=n
[Entry n]  Session= Point= ADR= Control= TrackNo= AMin= ASec= AFrame= ALBA= Zero= PMin= PSec= PFrame= PLBA=
[TRACK n]  MODE=0|1|2  INDEX 0=lba  INDEX 1=lba  ISRC=…
```

- `.img` next to it: 2352 bytes per sector from LBA 0, all tracks; `.sub`
  next to it (optional): 96 bytes per sector, **deinterleaved** P..W.
  `DataTracksScrambled=1` images exist (rare): refuse with a clear error.
- Tracks come from `[Entry]` records with `Point` 1..99 (`PLBA` = index 1
  start, `Control` = the nibble), `[TRACK n]` gives `INDEX 0` where present
  and the mode. Session lead-outs from `Point=0xA2` (`PLBA`). Multisession
  works out of the box: one `Session` per distinct `Session=` value.
- **Keep every `[Entry]` verbatim in `Disc::raw_toc`** (session, point,
  adr, control, min/sec/frame, pmin/psec/pframe). READ TOC format 2 replays
  them (§4.1); that is the fidelity SecuROM-era checks want.

### 2.4b MDS/MDF (`mds.rs`, brought forward from M5e on 2026-09-04)

Alcohol 120% / Daemon Tools: `.mds` header (`MEDIA DESCRIPTOR`, version
1.x), session blocks at the offset in header byte 0x50 (24 bytes each:
start, end, number, block counts, first/last track, tracks offset), track
blocks of 80 bytes (mode `0xA9` audio / `0xAA` Mode 1 / `0xAB` Mode 2 /
`0xAC`/`0xAD` form 1/2, subchannel flag `0x08` = 96 bytes interleaved
appended, ADR/control, point, MSF, PMSF, extra-block offset, sector size
2048/2352/2448, start sector, 64-bit start offset, footer offset with the
`.mdf` name), extra blocks (pregap, length). Every block with a point is
kept as a raw TOC entry. **Layout rule, checked against a RAW+SUB dump's
own Q frames:** the `.mdf` holds each track from `start_sector` (index 1)
for `length` sectors at `start_offset`; the `pregap` sectors are *not* in
the file and are synthesized (silence / zero data) as the track's index 0;
track 1's pregap of 150 is the lead-in pause and is never addressed. DPM
blocks (header byte 0x54) are ignored until a title needs timing.

### 2.5 Raw ⇄ cooked and L-EC (`sector.rs`, `ecc.rs`)

Raw Mode 1 sector (2352 bytes): `sync[12] header[4] data[2048] edc[4]
zero[8] p_parity[172] q_parity[104]`. Mode 2 form 1: `sync header
subheader[8] data[2048] edc[4] p[172] q[104]`. Mode 2 form 2: `sync header
subheader data[2324] edc[4]` (EDC optional, may be zero).

**EDC**: CRC-32 with polynomial x^32+x^31+x^16+x^15+x^2+x+1, LSB-first
(table built from reflected `0xD8018001`), initial value 0, no final XOR,
stored little-endian. Coverage: Mode 1 bytes 0..2064 (sync+header+data);
form 1 bytes 16..2072 (subheader+data); form 2 bytes 16..2348.

**ECC (RSPC)**: over GF(2^8) with the primitive polynomial `0x11D`. Operate
on the 2064-byte area `header+data+edc+zero` (bytes 12..2076 of the sector;
for Mode 2 the header is treated as four zero bytes — compute with the
header temporarily zeroed, then restore). Use the standard two-pass loop
(the form Neill Corlett's ECM tool uses, public domain; libmirage does the
same): each pass produces parity for one "major" of size `major_count`
over `minor_count` bytes, written as two bytes at `dst[major]` and
`dst[major + major_count]`:

| Pass | major_count | minor_count | major_mult | minor_inc | writes to |
|---|---|---|---|---|---|
| P | 86 | 24 | 2 | 86 | sector[2076..2248] |
| Q | 52 | 43 | 86 | 88 | sector[2248..2352] |

Inner loop for one major `m`: `index = (m >> 1) * major_mult + (m & 1)`,
`ecc_a = ecc_b = 0`; repeat `minor_count` times: `t = src[index]`;
`index += minor_inc; if index >= size (2064 + 172 for Q) index -= size`;
`ecc_a ^= t; ecc_b ^= t; ecc_a = gf_mul2(ecc_a)` (multiply by α with the
0x11D reduction); after the loop `ecc_a = gf_div(ecc_a ^ ecc_b) ` using
the standard `ecc_b_lut` (inverse of α³ ⊕ 1 table); store
`dst[m] = ecc_a`, `dst[m + major_count] = ecc_a ^ ecc_b`. Build the two
256-entry tables once (`ecc_f_lut[i] = 2·i in GF`, `ecc_b_lut[i]` such that
`ecc_b_lut[ecc_f_lut[i] ^ i] = i`). The Q pass reads the P parity too
(source size 2064 + 172).

`verify_mode1(raw)`: recompute EDC and both parities into scratch and
compare with the stored bytes. Result `Ok`, `EdcMismatch`, `EccMismatch`
or `NoSync` (no sync pattern / wrong mode byte in a data track: an
audio-format tail, or the zero filler a dump tool writes for an
unreadable sector — an all-zero sector's EDC and parity are zero and would
verify otherwise; C2 reports every byte bad). **No correction is attempted**: a dump made by a drive
already holds what that drive read; a mismatch means "this sector fails
L-EC on a real drive", which is exactly the SafeDisc signal. Cooked reads
of a mismatching sector return `Err(Medium)`. Raw reads return the bytes.
C2 pointers (READ CD with C2 requested): set the bit for every byte whose
recomputed parity disagrees — approximate, good enough for checks that
only count errors; refine when a dump with recorded C2 data exists.

Synthesis (cooked image → raw request): sync + BCD header (mode 1 or 2)
+ data + EDC + zero + P + Q, computed once per sector on demand (a
2 KiB-per-sector cache is unnecessary; the ECC costs ~10 µs).

### 2.6 Subchannel synthesis (`subq.rs`)

When no `.sub` data exists, `read_sub(lba)` returns 96 bytes: P (12
bytes) all `0xFF` inside index 00 of any track and for the last 150
sectors before a track's index 01 (that is the pause flag as real discs
carry it), else `0x00`; Q (12 bytes) as below; R..W zero.

**Synthesis of an undeclared pregap is a guess, and it must stay one**
(measured 2026-09-05 with `discx subscan` against four real dumps). Where a
descriptor gives a track no index 00, the disc underneath may be any of three
things, and nothing in the descriptor tells them apart — the MDS `pregap`
field is 0 for every such track on both Alcohol dumps we have:

| Disc | P before the next track | Q there |
|---|---|---|
| AoE Gold | `0xFF` (pause) | previous track, **index 01**, counting up |
| Moto Racer | `0xFF` (pause) | next track, **index 00**, counting down to 0 over 149 sectors |
| Settlers 3 | `0x00` (no pause) | previous track, index 01, counting up |

The same caveat covers **MCN frame placement**: we interleave ADR 2 at
`lba % 100 == 98` when the disc has a catalog number, and Rayman 2 — the one
disc here that carries MCN frames at all, 3,307 of them — puts them
elsewhere, which is 99.0 % agreement rather than 100 %. Where a disc chooses
to put its ADR 2 and 3 frames is a mastering decision no descriptor records.

We synthesize the first of those: P pauses for the 150 sectors before a
track's index 01, Q keeps counting up in the previous track at index 01.
That reproduces AoE's own subchannel **exactly** — 277,626 of 277,626 ADR 1
frames — and costs ~0.7 % of frames on a Moto-Racer-shaped disc. Changing it
to the second convention was tried and reverted: it trades 1,633 wrong frames
on one disc for 1,866 and 1,650 on two others.

**The residual does not reach a game.** AoE Gold and Moto Racer are the two
discs the conventions differ on, and both play their CD soundtracks in-game in
XP, in the player, from their `.mds` (user, 2026-09-05) — so the ~0.7 % of Q
frames we synthesize wrongly on a Moto-Racer-shaped disc is below what a
title's own audio code looks at. That is what makes the guess safe to keep,
not merely cheapest to keep.

**What a protection actually reads: measured, not assumed** (2026-09-05).
Settlers 3 CD01 is dumped as both a `.ccd` (with `.sub`) and a `.cue`
(without), and its ProtectCD band is corrupt in the data *and* in the Q
relative timing. The game plays from **both**, on both discs (user). The two
images deliver an identical data anomaly — `discx scan` gives the same 697
failing LBAs for each — and differ only in subchannel: the `.ccd` replays the
disc's own Q, the `.cue` synthesizes regular, CRC-correct Q that has no
anomaly in it at all.

**The conclusion drawn from that pair — "the check reads the data anomaly,
not Q" — is void.** It assumed the check reads *something*, and the negative
control below shows it does not read the band at all. What the cue/ccd pair
actually established is narrower and still useful: subchannel replay versus
synthesis makes no difference to this title. The remaining honest statement
about Q is the one from the pregap result — nothing we have met so far is
known to read it.

### 2.6b The negative control, and what it took away (2026-09-05)

Three schemes appeared to pass and not one had been seen to *fail*, so every
conclusion rested on inference from a game that started. `discx repair` builds
the disc that separates the two readings: a copy in which every L-EC-failing
sector verifies, the user data left exactly as dumped, the difference confined
to offsets 2064–2351 of exactly those sectors (EDC, reserved gap, ECC) — one
variable, verified by a byte-for-byte diff against the original.

`clean/fifa2002/FIFA2002.mds` (584 sectors repaired, 0 failures on a rescan)
and `clean/settlers3/CD01.cue` (547 repaired, only the 150 sync-less run-out
sectors left, which a real drive fails too).

**Both titles run from their clean copies** (user, 2026-09-05). FIFA 2002
launches and reaches its menus; Settlers 3 plays. The protections did not
refuse a disc they should have refused.

So the SafeDisc 2.x and ProtectCD acceptance rows are **inconclusive**, not
passing, and doc 05 says so. Nothing measured so far shows that our error
delivery is what satisfied either check — a check that ran and was satisfied
and a check that never ran look identical from outside, and the control was
the only thing that could tell them apart. Two readings remain, and one
common cause is likelier than two coincidences:

1. **Neither check runs in these installs.** The wrapped EXE never
   authenticates (a full install path that skips it, or a binary that is not
   actually SafeDisc-wrapped).
2. **Both checks bail out permissively before reaching the disc.** Protections
   of the era skip authentication rather than risk a false positive when they
   cannot get the low-level access they want — no SPTI/ASPI, a drive that does
   not identify as they expect, `secdrv.sys` not loaded. Our drive would then
   never be asked, and every disc would "pass".

**Empty drive, run 2026-09-05: FIFA 2002 asks for the CD.** So reading 1 is
out for that title — *a* disc check runs, and it is not satisfied by an empty
drive. Be careful how much that buys: "asks for the CD" is exactly what a
volume-label or data-file presence check does, and the repaired disc satisfies
it, so all it establishes is that something looks for a disc with the right
files on it. It does **not** establish that SafeDisc's authentication runs. The
three observations together — original disc runs, repaired disc runs, no disc
refuses — are equally consistent with a presence check that always ran and an
authentication that never did.

**Both binaries are genuinely wrapped** (offline, `7z` reads the qcow2
directly — no conversion needed): `fifa2002.exe` has the `stxt371` and
`stxt774` sections and the `BoG_` marker of SafeDisc 2, `S3.EXE` has
ProtectCD's `.ficken` section. A cracked or unwrapped binary is not the
explanation.

**The ATAPI trace, 2026-09-05.** `tools/xp-game-test.sh` with
`QEMU_EXTRA="-trace ide_atapi_cmd_packet -trace ide_atapi_cmd_error -D log"`
over a FIFA 2002 launch from the original `.mds` (KVM, `-cpu pentium3`):

- SafeDisc's probe is unmistakable and is **not** an error-pattern check. It
  reads **LBA 800, then one pseudo-random single sector, and repeats** — 22
  such pairs in one launch, the probes scattered over ~1300–9900, every one a
  single-sector `READ(10)`.
- **Not one of the 584 corrupt sectors is ever read.** 0 of 506 reads in the
  whole session touch a band LBA. The check is measuring something about
  *reading* — timing is the obvious candidate for an anchor-then-seek pattern
  — and not the L-EC failures we spent the design on.

That is why a repaired disc passes: the repaired sectors are never looked at.

**And the check does fail — we have finally seen it.** In this configuration
FIFA 2002 refuses: *"Por favor insira o CD FIFA 2002"*, reproducibly, with and
without QEMU's default empty CD-ROM drive (which the first run showed
answering 263 `NOT READY / MEDIUM NOT PRESENT`; removing it with `-nodefaults`
changed nothing). So the authentication is live and environment-sensitive —
the negative control we could not get from the disc contents arrived from the
harness instead. The player passes with the same disc, so the difference is
the player's path versus bare QEMU, not the bytes on the medium.

**The passing launch, traced in the player** (`-trace ide_atapi_cmd_packet`
works through the player's QEMU args; FIFA 2002 reached its intro, `d3dpt-vga`
logging page flips, screendump confirms). Four runs on one image and one disc:

| Run | CD on | Display | Outcome | Reads | Multi-sector | Max LBA | Band sectors read |
|---|---|---|---|---|---|---|---|
| bare QEMU | ide0 slave | cirrus | *"insira o CD"* | 488 | 1 | 13272 | **0** |
| bare QEMU `-nodefaults` | ide0 slave | cirrus | *"insira o CD"* | 506 | 1 | 13272 | **0** |
| bare QEMU | **ide.1** | cirrus | no dialog, then crash | 505 | — | — | **0** |
| **player** | **ide.1** | d3dpt-vga | **runs** | 781 | **281** | **250230** | **0** |

The passing run is unmistakable in the trace: 281 multi-sector reads out to LBA
250230, 9069 sectors in all — the game loading its content after the check let
it through, where every failing run stops at 13272 with essentially only
single-sector reads. It does 13 anchor-and-probe pairs on the way.

**And it still never reads a corrupt sector: 0 of 781.** So the conclusion
holds from the inside, not merely by inference from a repaired disc passing.
FIFA 2002's SafeDisc 2 authentication completes without once looking at the
584-sector band. The band is not what this title's check reads, and the
`repair` control was reporting the truth.

**A harness finding worth its own line:** the check *rejects* when the CD
shares the boot disk's IDE channel (`-drive media=cdrom` lands at index 1 =
ide0 slave) and passes when the CD is on `ide.1`, which is where the player
puts it. One variable between runs 2 and 3, same display, same disc. An
anchor-then-probe pattern is consistent with a timing measurement, and sharing
a channel with the boot disk is exactly what would perturb one — but that is a
hypothesis, not a measurement. What is established is the correlation, and
that `tools/xp-game-test.sh`'s default CD placement makes a protected title
fail for reasons that have nothing to do with the disc image.

**A real defect found on the way** (not yet fixed): `GET CONFIGURATION` with
RT=10b (request one feature) answers `ILLEGAL REQUEST / INVALID FIELD IN CDB`
for every feature we do not implement — including **0x1e, CD Read**, which
every CD drive must report. MMC requires a valid header with an empty feature
list instead. `MODE SENSE(10)` page 0x1b is refused the same way. Windows
issues both; neither is known to be what SafeDisc trips on, but both are wrong.

Until the passing trace exists, the drive model's error delivery is proven
only host-side and by `atapi-guest-test.py` — real evidence, just not evidence
about protections.

The lesson meanwhile is that anything
which actually reads subchannel wants a dump that *carries* it (CCD `.sub`,
MDS 2448), where `read_sub` replays the bytes verbatim and none of this
applies.

Q, ADR 1 (position), the layout every sector has:

```
byte 0  control<<4 | adr(1)
byte 1  TNO   BCD track number (0xAA in lead-out; not addressable here)
byte 2  INDEX BCD (00 in pregap)
byte 3-5  MIN SEC FRAME  BCD, track-relative; in index 00 it counts DOWN (to 00:00:01 at the last pregap sector on both real discs measured, not to zero)
byte 6  ZERO (0x00)
byte 7-9  AMIN ASEC AFRAME  BCD absolute (lba + 150)
byte 10-11 CRC-16 big-endian: CCITT polynomial 0x1021, init 0x0000, over bytes 0..10, result inverted (~crc)
```

Real discs interleave ADR 2 (MCN) and ADR 3 (ISRC) frames roughly once
per 100 sectors. When the model has an MCN / ISRC, emit ADR 2 at `lba % 100
== 98` and ADR 3 at `lba % 100 == 99` (MCN: 13 BCD digits packed high
nibble first in bytes 1..7, byte 8 zero, AFRAME in byte 9; ISRC: 12
characters 6-bit packed per Red Book in bytes 1..8, AFRAME in byte 9).
Protection code that fingerprints subchannel does so from a `.sub` dump,
which is replayed verbatim; synthesis only has to be *plausible*.

When a `.sub` file exists, it wins for every sector it covers; sectors
past its end (a truncated `.sub`) are synthesized.

## 3. The C API (`libdisc/libdisc.h`)

One header for the crate, the block driver and atapi.c, like
`d3dpt/d3dpt_proto.h`. Hand-written (no cbindgen dependency). Bump
`LIBDISC_API_VERSION` on any change; `cdimage_open` refuses a mismatch.
Every function is thread-safe on the same handle (immutable files; no
per-handle mutable state — the audio position lives in atapi.c, not here).

```c
#ifndef LIBDISC_H
#define LIBDISC_H
#include <stddef.h>
#include <stdint.h>
#define LIBDISC_API_VERSION 1

typedef struct libdisc libdisc;                /* opaque */

enum {
    LIBDISC_OK      =  0,
    LIBDISC_ERANGE  = -1,   /* LBA outside [0, lead-out)               → sense 05/21/00 */
    LIBDISC_EMEDIUM = -2,   /* L-EC failed on a cooked read            → sense 03/11/05 */
    LIBDISC_EMODE   = -3,   /* wrong sector kind for the request       → sense 05/64/00 */
    LIBDISC_EINVAL  = -4,   /* bad parameter / CDB field               → sense 05/24/00 */
    LIBDISC_EIO     = -5,   /* host file read failed                   → sense 04/xx: report and fail */
};

/* sector kinds, as LibdiscSectorInfo.kind */
enum { LIBDISC_KIND_AUDIO = 0, LIBDISC_KIND_MODE1 = 1, LIBDISC_KIND_MODE2F1 = 2,
       LIBDISC_KIND_MODE2F2 = 3, LIBDISC_KIND_MODE2 = 4, LIBDISC_KIND_GAP = 5 };

typedef struct LibdiscSectorInfo {
    uint8_t kind;        /* LIBDISC_KIND_* */
    uint8_t track;       /* 1..99 */
    uint8_t index;       /* 0..99 */
    uint8_t lec;         /* data kinds: 1 ok, 0 fails; audio/gap: 1 */
} LibdiscSectorInfo;

typedef struct LibdiscTrackInfo {
    uint8_t number, session, control, mode;   /* mode = LIBDISC_KIND_* of the track */
    int32_t start_lba;   /* index 1 */
    int32_t pregap_lba;  /* index 0 start, == start_lba when there is none */
    int32_t end_lba;     /* exclusive */
} LibdiscTrackInfo;

uint32_t libdisc_api_version(void);
/* 0..100: how sure libdisc is that `filename` (first `len` bytes in `head`) is an image it reads.
   .cue → 100 when the text parses as a cue sheet; .ccd → 100 on "[CloneCD]"; .iso → 0 (stays on raw) */
int      libdisc_probe(const uint8_t *head, size_t len, const char *filename);
/* opens the image and every payload file; on failure returns NULL and writes a message into err */
libdisc *libdisc_open(const char *path, char *err, size_t errlen);
void     libdisc_close(libdisc *d);

uint32_t libdisc_sector_count(const libdisc *d);          /* lead-out LBA of the last session */
uint8_t  libdisc_session_count(const libdisc *d);
uint8_t  libdisc_track_count(const libdisc *d);           /* across sessions */
int      libdisc_track_info(const libdisc *d, uint8_t track, LibdiscTrackInfo *out); /* ERANGE if no such track */
int      libdisc_sector_info(const libdisc *d, uint32_t lba, LibdiscSectorInfo *out);

int      libdisc_read_cooked(const libdisc *d, uint32_t lba, uint8_t out[2048]);  /* L-EC verified user data */
int      libdisc_read_raw(const libdisc *d, uint32_t lba, uint8_t out[2352]);
int      libdisc_read_sub(const libdisc *d, uint32_t lba, uint8_t out[96]);      /* deinterleaved P..W */

/* MMC responders: write the reply into out (capacity cap), return its length or a LIBDISC_E* code.
   The caller truncates to the CDB's allocation length. */
int libdisc_mmc_read_toc(const libdisc *d, uint8_t format, int msf, uint8_t start, uint8_t *out, size_t cap);
int libdisc_mmc_read_subchannel(const libdisc *d, uint32_t pos_lba, int msf, int subq, uint8_t format,
                                uint8_t track, uint8_t audio_status, uint8_t *out, size_t cap);
int libdisc_mmc_read_disc_information(const libdisc *d, uint8_t *out, size_t cap);
/* READ CD (BE) / READ CD MSF (B9): bytes per sector for this CDB, or LIBDISC_EINVAL for an illegal
   combination; then one call per sector fills exactly that many bytes */
int libdisc_mmc_read_cd_length(uint8_t expected_type, uint8_t byte9, uint8_t byte10);
int libdisc_mmc_read_cd_sector(const libdisc *d, uint32_t lba, uint8_t expected_type, uint8_t byte9,
                               uint8_t byte10, uint8_t *out, size_t cap);
#endif
```

(The two structs were first named `libdisc_sector_info` / `libdisc_track_info`,
which C refuses because the functions of the same name exist; renamed
2026-09-04.)

Rust side: `capi.rs` holds `#[no_mangle] pub extern "C" fn libdisc_open(…)`
etc.; `libdisc` is `Box<Disc>` cast to a pointer; `err` is filled with a
NUL-terminated ASCII message. Never panic across the boundary: wrap bodies
in `std::panic::catch_unwind` and return `LIBDISC_EIO`.

## 4. MMC responder byte layouts (`mmc.rs`)

Reference: MMC-3 (T10 1363-D), which is what Win9x/XP's `cdrom.sys` and
period protection drivers assume. All multi-byte fields big-endian unless
stated. Times: `msf=1` → `0, M, S, F` binary; `msf=0` → 32-bit LBA.

### 4.1 READ TOC/PMA/ATIP (0x43)

Header: `data length (2 bytes, excludes itself)`, then per format:

- **Format 0** (TOC): `first track, last track`, then one 8-byte descriptor
  per track from `start` (0 or 1 = all; 0xAA = lead-out only; a start above
  the last track → EINVAL): `reserved, ADR<<4|control (0x14 data / 0x10
  audio), track number, reserved, time (4 bytes)`; last descriptor: track
  0xAA with control 0x14 if the last track is data else 0x10, time = the
  last session's lead-out. Multisession discs list every track of every
  session.
- **Format 1** (multisession): `first session, last session`, one
  descriptor for the first track of the last session (same 8-byte shape).
- **Format 2** (raw TOC): `first session, last session`, then 11-byte
  descriptors: `session, ADR<<4|control, TNO (0), POINT, MIN, SEC, FRAME
  (running time, 0 for A0..A2), ZERO, PMIN, PSEC, PFRAME`. Per session, in
  this order: `A0` (PMIN = first track, PSEC = disc type: 0x00 CD-DA/CD-ROM,
  0x10 CD-I, 0x20 CD-ROM XA — use 0x20 when the first track of the session
  is Mode 2, else 0x00), `A1` (PMIN = last track), `A2` (lead-out MSF),
  then each track (POINT = track, P-time = index 1 start, MSF absolute).
  Multisession adds a `B0` descriptor after the last session's `A2` with
  ADR 5 (next writable start / max lead-out; use the last lead-out + 150
  and 0x4C:0x2C:0x00 as real drives do for closed discs). **Fields are MSF
  regardless of the `msf` bit** (real drives). When `Disc::raw_toc` is
  present (CCD), emit its entries verbatim instead.
- Formats 3 (ATIP), 4 (CD-TEXT), 5 → EINVAL.
- `cdrom_read_toc` / `cdrom_read_toc_raw` in `hw/block/cdrom.c` stay for
  the no-disc path; format 0 on a single-track ISO through libdisc
  produces the same layout (exerciser check) with one deliberate
  difference: QEMU's lead-out descriptor says control `0x16`, ours `0x14`
  (what a real drive reports for a data disc; decided 2026-09-04).

### 4.2 READ SUB-CHANNEL (0x42)

`out[0] = 0, out[1] = audio_status` (0x00 not supported, 0x11 playing,
0x12 paused, 0x13 completed, 0x14 error, 0x15 no status), `out[2..4] =
data length`. `subq=0` → 4-byte header only. Formats:

- **1 (current position)**: `format 1, ADR<<4|control, track, index,
  absolute time (4), relative time (4)` from the Q of `pos_lba` (relative
  time counts down inside index 0).
- **2 (MCN)**: `format 2, 0,0,0, MCVal<<7, MCN 13 ASCII digits + NUL`
  (all zero if none).
- **3 (ISRC)**: `format 3, ADR<<4|control, track, 0, TCVal<<7, ISRC 12
  ASCII + NUL + 0` for `track`.

### 4.3 READ CD (0xBE) and READ CD MSF (0xB9)

CDB fields: expected sector type = `(byte1 >> 2) & 7` (0 any, 1 CD-DA, 2
Mode 1, 3 Mode 2 formless, 4 Mode 2 form 1, 5 Mode 2 form 2); byte 9:
`sync 0x80, header 0x60 (01 header, 10 subheader, 11 both), user data
0x10, EDC/ECC 0x08, C2 0x06 (01 = 294 bytes C2 bits, 10 = 296 bytes C2 +
block error byte + pad)`; byte 10 & 7: subchannel `0 none, 1 raw 96
(interleaved), 2 Q 16 bytes (formatted), 4 R-W 96`.

`libdisc_mmc_read_cd_length` returns the sum of the selected fields for
the expected type (the MMC-3 table 358..362; e.g. Mode 1 with `0xF8` = 2352,
`0x10` = 2048, `0x78` = 2352; CD-DA any combination = 2352; Mode 1 with
`0x30` = 2048 + 4 header + 8 subheader positions per table); C2 adds 294
or 296; subchannel adds 96 or 16. Illegal: header/EDC bits without user
data on audio... follow the table; EINVAL for combinations it marks
illegal, and EMODE when the sector at `lba` is not of the expected type
(type 0 accepts anything). The per-sector fill copies the selected pieces
in order: sync, header, subheader, user data, EDC/ECC, C2, subchannel
(interleave P..W into the 96-byte MMC form for `1`, Q formatted as the
16-byte "subchannel Q" form for `2`).

MSF form: start MSF inclusive, end MSF exclusive, both → LBA via
`Msf::to_lba`.

### 4.4 READ DISC INFORMATION (0x51)

Standard 34-byte reply: `length 32, disc status 0x0E (complete, non-erasable,
last session complete = 0x02 | 0x0C)`, first track 1, number of sessions
(LSB byte 4, MSB byte 9 = 0), first/last track in the last session, lead-in
and lead-out start times `0xFFFFFFFF` (not available), disc type from the
A0 rule above. The existing `cmd_read_disc_information` stays for the
no-disc path.

## 5. The QEMU side

### 5.1 `block/cdimage.c` (in the repo: `libdisc/qemu/cdimage.c`)

Model it on `block/bochs.c` (format driver, read-only). Members:

```c
typedef struct BDRVCdimageState { libdisc *disc; uint32_t sectors; } BDRVCdimageState;
static int cdimage_probe(const uint8_t *buf, int buf_size, const char *filename)
    { return libdisc_probe(buf, buf_size, filename); }
static int cdimage_open(BlockDriverState *bs, QDict *options, int flags, Error **errp)
    /* bdrv_open_file_child(NULL, options, "file", bs, errp) as bochs does (the .cue itself becomes
       bs->file: tiny, read-only, harmless), then bdrv_apply_auto_read_only (refuse RDWR),
       libdisc_api_version() == LIBDISC_API_VERSION or error, path = bs->file->bs->exact_filename
       (fall back to bs->exact_filename), libdisc_open, sectors = libdisc_sector_count */
static int64_t coroutine_fn cdimage_co_getlength(BlockDriverState *bs) { return (int64_t)sectors * 2048; }
static void cdimage_refresh_limits(...) { bs->bl.request_alignment = 2048; }
static int coroutine_fn cdimage_co_preadv(bs, offset, bytes, qiov, flags)
    /* offset/bytes are 2048-aligned (alignment above); per sector libdisc_read_cooked into a
       2048-byte stack buffer, qemu_iovec_from_buf; EMEDIUM/EMODE/ERANGE → -EIO */
static void cdimage_close(bs) { libdisc_close(disc); }
libdisc *cdimage_disc(BlockDriverState *bs)   /* exported, declared in include/block/cdimage.h */
    { bs = bdrv_skip_filters(bs); return bs && bs->drv == &bdrv_cdimage ? ((BDRVCdimageState*)bs->opaque)->disc : NULL; }
static BlockDriver bdrv_cdimage = { .format_name = "cdimage", .instance_size = …, .bdrv_probe, .bdrv_open,
    .bdrv_child_perm = bdrv_default_perms, .bdrv_refresh_limits, .bdrv_co_preadv, .bdrv_co_getlength,
    .bdrv_close, .is_format = true, .supports_backing = false };
block_init(bdrv_cdimage_init);
```

`cdimage_disc` is called by atapi.c under the BQL from the vCPU thread;
`bs->drv` and `opaque` are stable while the medium is inserted, and a
medium change replaces `blk_bs(s->blk)`, so re-fetch the handle on every
command and never cache it in IDEState.

### 5.2 Build: patch `50-cdimage-block-driver`

- `meson_options.txt`: `option('libdisc_dir', type: 'string', value: '',
  description: 'directory holding liblibdisc.a (the CD image block driver)')`.
- `meson.build` (next to the other optional libs): `libdisc = not_found;
  if get_option('libdisc_dir') != '' ; libdisc = cc.find_library('libdisc',
  dirs: [get_option('libdisc_dir')], required: true, has_headers: [])`
  … then `libdisc = declare_dependency(dependencies: [libdisc, threads]
  + (host_os == 'linux' ? [cc.find_library('dl'), cc.find_library('m')] :
  []))`; `config_host_data.set('CONFIG_CDIMAGE', libdisc.found())`;
  summary line.
- `block/meson.build`: `block_ss.add(when: libdisc, if_true: files('cdimage.c'))`.
- `include/block/cdimage.h` (overlaid) declares `cdimage_disc` and is the
  only thing atapi.c includes besides `libdisc.h`; both guarded by
  `CONFIG_CDIMAGE` in atapi.c (`#ifdef` around the disc paths so the tree
  still builds without the library).
- `scripts/configure-qemu.sh`: run `cargo build --release -p libdisc`
  first (the crate has no QEMU dependency, so no cycle with the player)
  and pass `-Dlibdisc_dir="$ROOT/target/release"`. QEMU's `configure`
  forwards unknown `-D` options to meson. `prepare-qemu.sh`: overlay
  `libdisc/qemu/cdimage.c → qemu/block/`, `libdisc/qemu/cdimage.h →
  qemu/include/block/`, `libdisc/libdisc.h → qemu/include/block/` (rsync
  -c, same as the d3dpt overlay lines), *before* the patch loop.
- The staticlib gets linked into `qemu-system-i386` and into
  `libqemu-embed-i386.{so,dylib}` (both link `block_ss`). The player is
  Rust too, so two copies of `std` exist in one process. On macOS the
  export list (`embed/libqemu_embed.symbols`) hides libdisc's symbols
  already. On Linux check `nm -D build/qemu/libqemu-embed-i386.so | grep
  -c ' T _ZN3std'`; if non-zero add `-Wl,--exclude-libs,liblibdisc.a` to
  the shared library's `link_args` in patch 10 (the copies would merely
  interpose identical code, but keep it clean).
- Rust ≥ 1.63 is what QEMU's own meson checks want if `rust` is ever
  enabled; we do not enable QEMU's Rust support — cargo builds the crate
  outside meson.

### 5.3 ATAPI: patch `51-atapi-disc-model`

Files: `hw/ide/atapi.c`, `hw/ide/ide-internal.h` (new ASC values: 0x11
UNRECOVERED READ ERROR, 0x64 ILLEGAL MODE FOR THIS TRACK, 0x00/0x11
`ASCQ` support = extend `ide_atapi_cmd_error` with an ascq parameter
defaulting to 0 through a new `ide_atapi_cmd_error_ascq`), `include/hw/ide/
ide-dev.h` (new IDEState fields, **not** added to `vmstate_ide_drive`:
migration with a disc attached is unsupported, say so in the README row),
`hw/ide/ide-dev.c` (an `audiodev` property on `ide-cd`, §5.4).

New IDEState fields:

```c
/* disc-model reads (patch 51) */
bool     atapi_disc_read;      /* the current transfer is served by libdisc, not blk */
uint8_t  atapi_rc_type, atapi_rc_b9, atapi_rc_b10;  /* READ CD parameters, cooked read = type 0, b9 0x10, b10 0 */
uint32_t atapi_last_lba;       /* for READ SUB-CHANNEL when not playing */
/* CD-DA (patch 51, §5.4) */
uint8_t  atapi_audio_status;   /* 0x11 playing, 0x12 paused, 0x13 completed, 0x14 error, 0x15 none */
uint32_t atapi_play_lba, atapi_play_end;
uint8_t  atapi_audio_port[4];  /* mode page 0x0E: channel selection per port */
uint8_t  atapi_audio_vol[4];
QEMUSoundCard *atapi_card; SWVoiceOut *atapi_voice; QEMUTimer *atapi_play_timer;
```

Command dispatch: at the top of `ide_atapi_cmd` fetch `libdisc *disc =
atapi_disc(s)` once per command (`s->blk ? cdimage_disc(blk_bs(s->blk)) :
NULL`) and pass it to handlers via a static `s->atapi_disc_cur` field
valid for that call only — or simpler, each handler calls `atapi_disc(s)`
itself. Add table entries: `0x42 READ SUB-CHANNEL (CHECK_READY)`, `0x45
PLAY AUDIO(10)`, `0x47 PLAY AUDIO MSF`, `0x48 PLAY AUDIO TRACK/INDEX`,
`0x4B PAUSE/RESUME`, `0x4E STOP PLAY/SCAN`, `0x55 MODE SELECT(10)`, `0xA5
PLAY AUDIO(12)`, `0xB9 READ CD MSF`, all `CHECK_READY` (audio ones
`NONDATA` too). With no disc attached, the new audio commands succeed as
no-ops that leave `audio_status = 0x15` (period Linux/Windows behaviour on
a data-only drive is close enough), READ SUB-CHANNEL reports 0x15 with a
zero position, READ CD MSF behaves like READ CD.

**The transfer path.** Today `ide_atapi_cmd_read_pio` / `_dma` assume
`cd_sector_size` ∈ {2048, 2352} and fetch through `blk`. The disc path
generalises `cd_sector_size` to any value `libdisc_mmc_read_cd_length`
returns (≤ 2352 + 296 + 96 = 2744; `io_buffer_total_len` is 131076 bytes,
so at least 47 sectors fit per DMA chunk):

- `ide_atapi_cmd_read(s, lba, n, size)`: unchanged signature; the callers
  set `s->atapi_disc_read = (disc != NULL)` and the three READ CD
  parameter fields first (a cooked READ(10)/(12) sets type 0, b9 `0x10`,
  b10 0 — so through libdisc it is `read_cooked`, L-EC verified).
- PIO: in `ide_atapi_cmd_reply_end`, the "see if a new sector must be
  read" block calls `cd_read_sector(s)`. Make `cd_read_sector` return **1**
  when it filled the sector synchronously (disc path: one
  `libdisc_mmc_read_cd_sector` into `s->io_buffer`, then `s->lba++`,
  `s->io_buffer_index = 0`) and 0 when it started an async block read (the
  existing path). The loop then reads: `ret < 0 → ide_atapi_io_error;
  return`, `ret == 0 → return` (callback resumes), `ret == 1 → continue the
  loop`. Never call `ide_atapi_cmd_reply_end` recursively from the disc
  path (a 128 KiB transfer would recurse 64 deep). `cd_read_sector_sync`
  gets the same disc branch.
- DMA: write `ide_atapi_disc_read_dma_cb(void *opaque, int ret)` mirroring
  `ide_atapi_cmd_read_dma_cb`: if `io_buffer_size > 0` push the chunk with
  `s->bus->dma->ops->rw_buf(s->bus->dma, 1)` and advance
  `packet_transfer_size`; when nothing is left set `READY_STAT | SEEK_STAT`,
  the IO|CD interrupt reason, raise the IRQ, `ide_set_inactive`; otherwise
  fill the next chunk (`n = min(remaining sectors, io_buffer_total_len /
  cd_sector_size)`) synchronously via libdisc and re-enter through
  `replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
  ide_atapi_disc_read_dma_cb, s)` (a bottom half keeps the stack flat and
  the completion asynchronous, which every era driver copes with).
  `s->bus->dma->aiocb` stays NULL on this path (nothing to cancel;
  `ide_dma_restart` must not be reached with a disc transfer pending —
  guard `ide_atapi_dma_restart`).
- Errors from libdisc mid-transfer: `ide_atapi_cmd_error` with the sense
  from the table in §3 (`EMEDIUM` → `MEDIUM_ERROR 0x03`, ASC 0x11, ASCQ
  0x05), the transfer ends, no partial data delivered beyond the sectors
  already transferred (matches drives: the failing sector aborts the
  command). `ide_atapi_io_error` (block errno path) is unchanged.
- `cmd_read` (READ 10/12): before the transfer, `libdisc_sector_info`
  over `[lba, lba+n)`; any audio/gap sector → sense 05/64/00.
- `cmd_read_cd`: replace the two-case switch with the length function;
  keep `validate_bcl`; the range check against `s->nb_sectors >> 2` stays
  (the block driver's length is the lead-out, so it is right).
- `cmd_read_toc_pma_atip`, new `cmd_read_subchannel`,
  `cmd_read_disc_information`: disc → libdisc responder into `buf`, then
  `ide_atapi_cmd_reply(s, len, max_len)`; negative → the sense table.
- `cmd_get_configuration` with a disc: current profile `MMC_PROFILE_CD_ROM`
  and only that profile listed (a CD-ROM drive, not a DVD-ROM), plus
  features 0x001E (CD read: C2 + CD-Text bits) and 0x0103 (CD external
  audio play). Without a disc: unchanged.
- `cmd_mode_sense` page 0x2A with a disc: byte 10 `0x03` (reads CD-R/RW,
  no DVD), byte 12 `0x71` (unchanged: audio play, mode 2 form 1/2,
  multisession), byte 13 `0x7F` (CD-DA commands, stream accurate, R-W
  supported, R-W deinterleaved, C2 pointers, ISRC, UPC), byte 14 as today,
  byte 15 `0x03` (separate volume and mute), max/current speed 8467 (48×),
  256 volume levels, 128 KiB buffer. Page 0x0E reports
  `atapi_audio_port/vol`. `cmd_mode_select` (0x55) parses page 0x0E only
  (mode parameter header 8 bytes then the page), updates those fields,
  ignores other pages, errors on malformed lengths (05/26/00).
- INQUIRY: product string from `s->drive_model_str` (16 chars) so that
  `-device ide-cd,model=…` names a period drive; default unchanged
  (`QEMU DVD-ROM`). Check the `model` property exists on `ide-cd`
  (`hw/ide/ide-dev.c` `ide_dev_properties` vs `ide_cd_properties`); if it
  is hard-disk-only, add it to the CD list.
- `ide_cd_change_cb` (core.c): also stop audio and reset
  `atapi_audio_status` to 0x15, `atapi_last_lba` to 0.

### 5.4 CD-DA

Properties: `DEFINE_AUDIO_PROPERTIES(IDEDrive, card)` on `ide-cd` (as
`hw/audio/sb16.c` does; the card struct lives in `IDEDrive`, a pointer is
handed to the IDEState). If `audiodev` is unset, no voice is opened and
playback is timer-driven only (position advances at 75 sectors per second
so polling games complete; nothing is heard). With it set:
`AUD_register_card("ide-cd", card, errp)` at realize, `AUD_open_out(card,
NULL, "cd-audio", s, cd_audio_callback, &as)` with `as = {44100, 2,
AUDIO_FORMAT_S16, endianness 0}`; `AUD_set_active_out(voice, 1)` on play,
0 on pause/stop/complete.

`cd_audio_callback(opaque, avail)`: while `avail >= 2352` and
`play_lba < play_end`: `libdisc_read_raw(play_lba)` (an audio sector; a
data sector inside a play range ends playback with status 0x14), apply
page 0x0E routing (port 0 = left output: take channel per
`atapi_audio_port[0]` bit mask (1 = L, 2 = R, 3 = both/mix) scaled by
`vol[0]/255`; port 1 = right likewise), `AUD_write`, `play_lba++`;
when `play_lba == play_end`: status 0x13, deactivate. Without a voice, a
`QEMUTimer` at 1000/75 ms per sector advances `play_lba`.

Commands: PLAY AUDIO(10) `start LBA (4), length (2)`; PLAY AUDIO(12)
`start (4), length (4)`; PLAY AUDIO MSF `start MSF bytes 3..5, end MSF
6..8` (end exclusive; `FF:FF:FF` = to the end of the disc); PLAY AUDIO
TRACK/INDEX `start track/index bytes 4,5, end 7,8` (map through
`libdisc_track_info`); PAUSE/RESUME byte 8 bit 0 (0 pause → 0x12, 1
resume → 0x11); STOP PLAY/SCAN → 0x15 (and `play_lba = 0`). A play range
starting on a data sector → 05/64/00. `start == end` → status 0x13
immediately. READ SUB-CHANNEL position: `play_lba` while 0x11/0x12/0x13,
else `atapi_last_lba` (updated by every successful read command).

The player passes an audiodev already (`embed0`, see the cheat sheet in
doc 00); once the property exists the cheat-sheet lines change from
`-cdrom x` to `-drive if=none,id=cd0,media=cdrom,format=cdimage,file=x
-device ide-cd,bus=ide.1,drive=cd0,audiodev=embed0` (QMP device name for
swaps becomes `cd0`'s device: use `-device ide-cd,id=ide1-cd0,…` to keep
the name the docs use).

## 6. Tests (policy: integration / e2e only, `scripts/test.sh`)

### 6.1 Host: `discx` (Rust binary in `libdisc/src/bin/discx.rs`)

Built by `cargo build --release -p libdisc` (a `[[bin]]`; no extra deps).
Subcommands:

- `discx selftest <outdir>` — writes synthetic images into `<outdir>`
  and checks them **through the C API in `capi.rs`** (call the `extern
  "C"` functions from Rust, the same boundary `cdimage.c` uses):
  1. `mixed.cue/.bin`: track 1 MODE1/2352 (2000 sectors of deterministic
     pseudo-random data with computed EDC/ECC), track 2 AUDIO with `INDEX
     00` in the file (150 sectors) + 3000 sectors of a 1 kHz tone, track 3
     AUDIO with `PREGAP 00:02:00` + 1500 sectors; `CATALOG`, an `ISRC`.
  2. `mixed.ccd/.img/.sub`: the same disc as CloneCD with synthesized
     `.sub` and `[Entry]` records.
  3. `cooked.cue/.bin`: track 1 as MODE1/2048 (the same user data) +
     the audio tracks.
  4. `plain.iso`: the 2000 data sectors only.
  Checks (each prints PASS/FAIL with a name; exit 1 on any FAIL):
  - `toc0/1/2`: READ TOC formats 0 (msf 0 and 1), 1, 2 are byte-identical
    across 1, 2 and 3; format 0 on 4 equals `cdrom_read_toc`'s layout
    for a single track (hand-coded expected bytes).
  - `raw-synth`: `libdisc_read_raw` of every track-1 sector of 3 equals
    1's stored sector (synthesis = the real thing); `read_cooked` of 1
    equals 3.
  - `subq-synth`: `read_sub` of 1 (synthesized) equals 2's `.sub`
    (stored) for 200 sampled sectors including both index-0 regions;
    CRC-16 verified on every Q.
  - `lec`: flip one byte in one sector of 1's `.bin` (a copy):
    `read_cooked` → `LIBDISC_EMEDIUM`, `read_raw` → the flipped bytes,
    `read_cd_sector` with C2 requested has ≥ 1 bit set, and the
    unmodified neighbours are fine.
  - `edges`: LBA 0, track boundaries, `lead-out − 1` OK, lead-out and
    `0xFFFFFFFF` → ERANGE; READ CD with expected type 2 on an audio sector
    → EMODE; every `libdisc_mmc_read_cd_length` combination from the
    MMC-3 table (hand-coded expected lengths, ~20 entries).
  - `msf`: `Msf` round trips at the same points as the old `#[test]`s
    (then delete those tests from `msf.rs`: policy).
- `discx dump <image> <what> [args]` — prints hex of a responder's
  bytes (`toc 0 1`, `subq 1000`, `readcd 16 0 0xf8 1`): the oracle the
  guest test compares against.
- `discx repair <image> <outdir>` — a copy of the image in which every
  sector whose L-EC fails now verifies: **the negative control** (§2.6). It
  copies each payload and the descriptor by basename, so the copy opens under
  the same name in `<outdir>`, and rewrites nothing but the EDC, the reserved
  gap and the ECC of the failing sectors. The user data is left exactly as
  dumped — SafeDisc's `0x55` fill stays `0x55` — because the disc has to
  differ from the original in the *one* signal a protection reads and in
  nothing else. Sectors with no sync pattern are left alone: those are
  run-out, a real drive fails them too, and repairing them would add a second
  variable. Checked by selftest's `repair`, which corrupts a sector, repairs
  the image and requires that the sector read cleanly, that its user data is
  still the corrupted data, and that no byte outside the parity fields moved.
- `discx convert <in.iso> <out.cue>` — cooked ISO → MODE1/2352 cue/bin
  with synthesized EDC/ECC and, with `--audio tone.wav …`, appended audio
  tracks: the way to make guest test discs from the guest-tools ISO.

`scripts/test.sh` host stage: `run_check libdisc libdisc.log
target/release/discx selftest build/test/disc`, and `run_check cdimage
cdimage.log build/qemu/qemu-img info --output=json build/test/disc/mixed.cue`
followed by a grep for `"format": "cdimage"` and the expected virtual
size (`sector_count × 2048`) — the block driver is probed and reports
through the block layer.

### 6.2 Guest, DOS: `tools/atapi-guest-test.py`

Same harness as `tools/x87-guest-test.py` (NASM `.COM`, FreeDOS floppy
fetched once, `-serial file:`, `-cpu pentium3`): the program talks to the
secondary IDE channel (0x170/0x376; `-cdrom` = master) directly, sends
PACKET (0xA0) commands by PIO and hex-dumps every reply and every REQUEST
SENSE to COM1. Command list: INQUIRY, READ TOC formats 0 (msf 0/1), 1, 2,
READ CD `0xF8`/subch 0, 1, 2 at LBA 0, 16, the last sector of track 1 and
the first of track 2, READ(10) of the flipped sector (expect CHECK
CONDITION, sense 03/11/05) and of its neighbours, READ SUB-CHANNEL format
1/2/3, PLAY AUDIO MSF of 2 seconds of track 2 then READ SUB-CHANNEL twice
100 ms apart (position must advance), PAUSE, STOP, GET CONFIGURATION, MODE
SENSE 0x2A and 0x0E, MODE SELECT 0x0E then MODE SENSE 0x0E back. Run the
data commands with byte-count limits 512 and 65534 (BCL splitting in
`ide_atapi_cmd_reply_end`). The Python side builds the disc with `discx
selftest`, boots `qemu-system-i386 -cdrom build/test/disc/mixed.cue
-audiodev none,id=a0` and compares every dumped reply with `discx dump`
of the same request: the guest must see exactly what libdisc computed.
Wired into the guest stage as `atapi-guest`.

### 6.3 Guest, Windows

- XP and Win98 (`scripts/test.sh` guest stage, and by hand on the Air):
  `discx convert` the newest guest-tools ISO to `guest-tools.cue`, boot
  with it on `format=cdimage`, copy `D:\D3DPT\*.EXE` to the scratch disk
  (XP: `RUN.BAT` addition; Win98: by hand), hash on the host against the
  ISO's files. Proves cooked reads through the real OS driver, DMA
  included.
- `guest-tools/src/cdtest.c` (`GAMEDIR\CDTEST.EXE`, msvcrt-linked like the
  rest): `mciSendString("open cdaudio alias cd")`, `status cd number of
  tracks`, `play cd from 2 to 3` (TMSF), poll `status cd position` for
  3 s, print everything to `cdtest.log`. Run on the mixed test disc with
  `-audiodev wav,id=embed0,path=build/test/cd.wav` (headless) or the
  player's audio (by ear): the wav must contain the 1 kHz tone (Python:
  RMS over the middle second, dominant frequency by a naive DFT).
- Reference material (doc 09): ATAPI traces from the rig's real drive
  while a protected title checks its disc — a Windows XP SPTI logger is
  the tool to write when M5c starts (a filter driver is out of scope);
  dumps of owned discs with CloneCD (subchannel) for SecuROM and with a
  raw-capable tool for SafeDisc.

### 6.x Which dumps carry a protection signal (measured 2026-09-05)

Checked with `discx scan` over six real dumps; the details and the per-disc
table are in the M5 track doc. Two rules came out of it:

- **The SafeDisc *version* decides whether an L-EC band exists, not the
  dumping tool.** SafeDisc 2.x writes deliberately corrupt sectors near the
  start of the data track (Age of Mythology 580 from LBA 825, FIFA 2002 584
  from LBA 811); SafeDisc 1.x writes none at all (The Sims, Rayman 2, both
  0 failures over a complete disc) and checks the medium some other way. A
  1.x title is not an L-EC fixture however it was dumped.
- **DiscImageCreator / redump sets do carry the 2.x band.** `/sf` replaces
  each bad sector's user data with `0x55` and leaves the header and the
  wrong parity alone ("N unmatch sector is replaced at 0x55 except header"
  in `*.img_EccEdc.txt`), so the sector still fails L-EC and still reaches
  the guest as `-EIO`. FIFA 2002 exists here as both a DIC set and an
  Alcohol set of the same disc, dumped four years apart: inside the band
  they are identical, 584 sectors either way. Outside it they differ — DIC
  logs 64 sectors it could not descramble where Alcohol silently filled
  them, so DIC is the more truthful record of a damaged copy.

A useful shape to remember when judging a new dump: real protection is a
**band** (hundreds of sectors against hundreds of thousands clean). Whole-
disc failure is a format problem, not a protection one — most likely a
scrambled image (§2.x, `ccd.rs` refuses `DataTracksScrambled=1`, but a bare
`.scm` from DIC's pipeline carries no flag to catch it by).

## 7. Milestones (detail and order in the track doc)

| Step | Delivers | Proof |
|---|---|---|
| M5a | cue/bin + ISO model, EDC/ECC, Q synthesis, C API, `discx`, `cdimage` driver, patch 50/51 data path + TOC + READ CD + subchannel | `discx selftest`, `qemu-img info`, `atapi-guest`, XP/Win98 copy from a converted ISO |
| M5b | CD-DA (play/pause/stop/position, page 0x0E/0x2A, `audiodev`), player cheat-sheet lines, swap over QMP | `CDTEST.EXE` tone in the wav, Win98 CD Player by ear |
| M5c | CCD + `.sub` replay, raw TOC verbatim, a SecuROM title from an owned dump | the title's launch check passes on the Air and the Linux box |
| M5d | SafeDisc: L-EC path against a real dump, `secdrv.sys` running | launch check passes; `atapi-guest`'s flipped-sector case is the regression guard |
| M5e | MDS/MDF (+ DPM data), CHD, a seek/read timing profile if StarForce needs it | documented result per doc 05's table |
| M5f | disc shelf / swap in the player and launcher (with M6) | — |
