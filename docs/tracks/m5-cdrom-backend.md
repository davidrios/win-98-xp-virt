# Track: M5 — the CD-ROM backend (docs 05 and 17)

The handoff for a session that works on the raw optical drive emulation:
the `libdisc` Rust crate (disc model, image formats, MMC responders), the
`cdimage` QEMU block driver over its C API, the ATAPI patch that serves
TOC / raw sectors / subchannel / CD-DA from the model, and their tests.
Read `docs/00-status.md` first for the global picture and the track
rules, then this file, then doc 17 (the implementation spec: every byte
layout, function name and QEMU hook is there — this file is the *plan*,
doc 17 is the *spec*; do not re-derive what doc 17 fixes). Doc 05 is the
problem statement and acceptance table. Branch: `track/m5-cdrom` (opened
2026-09-04 off `main`).

## Scope and files (this track owns them)

- `libdisc/` entirely: `Cargo.toml`, `src/` (model, `cue.rs`, `ccd.rs`,
  `iso.rs`, `sector.rs`, `ecc.rs`, `subq.rs`, `mmc.rs`, `capi.rs`,
  `bin/discx.rs`), the C header `libdisc/libdisc.h` (bump
  `LIBDISC_API_VERSION` on any change), the QEMU block driver sources
  `libdisc/qemu/cdimage.c` + `cdimage.h` (overlaid into the QEMU tree by
  `scripts/prepare-qemu.sh`, like `d3dpt/hw`).
- QEMU patches `patches/qemu/50-cdimage-block-driver.patch` (meson option,
  block meson line, `CONFIG_CDIMAGE`) and `51-atapi-disc-model.patch`
  (`hw/ide/atapi.c`, `ide-internal.h`, `include/hw/ide/ide-dev.h`,
  `hw/ide/ide-dev.c`, `hw/ide/core.c`), their rows in
  `patches/qemu/README.md`. Numbers 50–59 are reserved for this track.
- Tests: `tools/atapi-guest-test.py` (DOS ATAPI exerciser),
  `guest-tools/src/cdtest.c` (`CDTEST.EXE`, MCI CD audio in the guest),
  the `libdisc` / `cdimage` / `atapi-guest` checks in `scripts/test.sh`.
- Docs: doc 05, doc 17, this file, the M5 row of the state table and the
  M5 line of "Next steps" in `docs/00-status.md`, the M5 section of doc 08.
- Shared (rebase first, edit minimally, say which track in the commit):
  `scripts/prepare-qemu.sh` and `scripts/configure-qemu.sh` (the overlay
  and the `-Dlibdisc_dir` line), `scripts/test.sh`, `patches/qemu/10-embed-api.patch`
  (only if the Linux `--exclude-libs` line is needed, doc 17 §5.2),
  `guest-tools/build-wrappers.sh` (adds `CDTEST.EXE`), `player/` (the
  `-drive`/`-device ide-cd,audiodev=` lines in M5b), `CLAUDE.md` (the
  testing-tools table), `docs/00-status.md` outside the M5 row.

## State (2026-09-05: the protected dumps arrived, and one passed)

- **Doc 05's plain mixed-mode + CD-DA row: PASS.** Age of Empires Gold and
  Moto Racer both play their CD soundtracks while the game runs — XP, in the
  player, from their `.mds` (user, 2026-09-05). Step 6 had proven CD-DA
  host-side and through MCI; this is the first time a title's own audio code
  drove PLAY / position / routing, over a real 14- and 12-audio-track disc.
  It also settles the pregap question below.

- **VOB ProtectCD: PASS, and it is the first CCD run in a guest.** The
  Settlers 3 plays from `CD01.ccd` and `CD02.ccd` in XP — tutorial from CD1,
  campaign from CD2, which it asks for and accepts (user, 2026-09-05). Every
  earlier guest pass was an `.mds`; this is the `.sub`-carrying CCD path
  (`read_sub` replaying bytes verbatim) under a real protection.
  **And the A/B came back the same day: it reads the data, not Q.** The game
  also plays from `CD01.cue` / `CD02.cue`, which carry no `.sub` (user).
  `discx scan` gives both images the same 697 failing LBAs, so the data
  anomaly is delivered identically; they differ only in subchannel, replayed
  verbatim from the CCD and synthesized — regular, CRC-correct, anomaly-free —
  from the cue. **VOB ProtectCD's check therefore reads the data anomaly and
  is satisfied by synthesized subchannel**, which also means a dump without
  `.sub` is a good source for a ProtectCD title. Doc 05's row premise ("a
  dump carrying both") is wrong for this scheme.
  **The gap this leaves is a negative control:** three schemes now pass and we
  have never watched a check *fail*, so a pass is inference from a game that
  started. Until one is seen to refuse a disc it should refuse, none of the
  three passes is fully nailed down.
  **The control discs are built** (`discx repair`, new, 2026-09-05; outside
  the repo at `oldstuff/clean/`): `fifa2002/FIFA2002.mds` (584 sectors
  repaired, scans with 0 failures) and `settlers3/CD01.cue` (547 repaired,
  only the 150 sync-less run-out sectors left, which a real drive fails too).
  Each was diffed against its original byte for byte — the difference is
  confined to offsets 2064–2351 of exactly those sectors, i.e. the EDC, the
  reserved gap and the ECC; sync, header and all 2048 user bytes identical, so
  the pair differs in one variable and the 0x55 fill is still 0x55. **Run each
  title from its clean copy: both must now refuse.** For Settlers the launch
  check is on CD1, so only CD01 is repaired — the campaign's CD2 has no band
  at all and the original is used for it.
  Scans, for the record: CD01 has 697 L-EC failures — the 538-sector band at
  195539–196076, **nine scattered singles past it** (196654, 196823, 197060,
  197160, 197219, 197424, 197584, 198370, 198977, EDC wrong as well), and 150
  sync-less sectors at 219692–219841 which are the ordinary run-out before
  track 02. **CD02 carries no band at all** (its only 150 failures are the
  same benign run-out at 234254–234403): the protection is on disc 1 only,
  and disc 2 is a plain mixed-mode disc with 12 audio tracks.

- **Step 8, first title: PASS.** FIFA 2002 installed from `FIFA2002.mds`,
  launched and navigated its menus in XP (user, 2026-09-05). SafeDisc 2.x's
  check runs at launch, so reaching the menus means the wrapped EXE and
  `secdrv.sys` read the 584-sector band through `cdimage` → patch 51 →
  libdisc and got the errors they expect. Doc 05's SafeDisc 2.x row is green
  on real protected media. A match was not reached; suspected display path,
  not the disc — diagnose with `tools/xp-game-test.sh` (`SHOTS=`, `DRW_AFTER=`)
  before assuming, since an invisible message box is the documented
  failure shape.
  **Use the `.mds`, not the `.cue`:** the DIC bin carries 64 undescrambled
  sectors *outside* the band (LBA 135084–135086, 161089, 223875, 224045)
  which the driver correctly answers `-EIO`, breaking an install for reasons
  unrelated to the protection. The Alcohol dump read those sectors cleanly
  (real bytes, valid EDC) and carries the identical 584-sector band. Keep
  the `.cue` as the verification fixture — it is the one whose bad-sector
  list provably equals the dumper's own log — and the `.mds` as the image to
  run.

## State (2026-09-05: the protected dumps arrived)

- **Step 7 has its material.** Real dumps in
  `/mnt/data2/david/Downloads/oldstuff` (not in the repo), checked with
  `discx info` / `scan` / `subscan`:

  | Dump | Protection, from the disc | Signal in the dump |
  |---|---|---|
  | `fifa2002/` (DIC **and** Alcohol sets of one disc) | SafeDisc 2.x (`00000001.TMP`, `00000002.TMP`, `DRVMGT.DLL`, `SECDRV.SYS`) | **intact in both**, and they agree: 584 sectors from LBA 811, identical in each — see the acceptance note below |
  | `AOM_D1.ccd` | SafeDisc 2.x (`00000001.TMP`, `DRVMGT.DLL`, `SECDRV.SYS`) | **intact** — 580 weak sectors between LBA 825 and 12000, valid sync+header, 0x55 fill, wrong EDC |
  | `AOM_D2.ccd` | none (second disc) | clean |
  | `the settlers 3/CD01.ccd` | VOB ProtectCD (`.ficken` section in `S3.EXE`) | **intact** — 538 sectors 195539–196076 corrupt in the data *and* in the Q relative timing, over otherwise flawless subchannel |
  | `The Sims (PT-BR) (CCD)` | SafeDisc 1.x (`CLCD16/32.DLL`, `DPLAYERX.DLL`, `SIMS.ICD`) | none to find — see the version rule below |
  | `rayman2/` (DIC/redump submission) | SafeDisc 1.1x–1.3x (submission info names every file) | none to find; the best-documented dump here (full `.sub`, C2, DAT hashes verified) |

  **The acceptance criterion is met on FIFA 2002.** Doc 17 asks that the
  dumper's own log of bad sectors be exactly the LBAs where
  `sector_info.lec == 0`. Three independent sources agree on the same 584:
  DiscImageCreator's `FIFA2002.img_EccEdc.txt` ("584 unmatch sector is
  replaced at 0x55 except header"), our scan of its `.bin`, and our scan of
  an Alcohol dump of the same disc made four years later. Outside the
  protection band the two dumps differ — DIC reports 64 sectors it could not
  descramble (LBA 135084–135086, 161089, 223875, 224045) where Alcohol
  reports none, i.e. DIC records read damage honestly and Alcohol fills it
  silently. Neither is inside the band.

  **The SafeDisc version decides whether there is a band at all**, not the
  dumping tool (measured on four discs): 2.x writes deliberately corrupt
  sectors (AoM 580, FIFA 2002 584), 1.x does not (The Sims, Rayman 2, both
  0) and checks the disc another way. So a 1.x title cannot serve as an
  L-EC fixture no matter how it was dumped, and **redump / DiscImageCreator
  sets are perfectly good sources for 2.x titles** — `/sf` fills the bad
  sectors with 0x55 and leaves the parity wrong, so the failure survives.
  The Sims and Rayman are the fixtures for the *other* case: protection
  files present, nothing for `scan` to find, which is what
  `cd-dump-verify.sh` must report as "this dump cannot test the check".

  AoM disc 1 already exercises the path end to end with no code change:
  `qemu-img convert -f cdimage AOM_D1.ccd` fails `Input/output error`
  because the block driver refuses the sectors whose L-EC does not verify.
  Settlers CD01 is the widest of them — it needs the `.sub` replay path as
  well as the L-EC one, and it is mixed-mode with 12 audio tracks.

- **`discx subscan` (new)** walks every sector's stored subchannel: Q CRC
  failures with their clustering, kind/track/ADR distribution, whether the
  bytes would verify un-deinterleaved, and how often `subq::synthesize`
  reproduces the disc's own frames. Verdict on the Alcohol dumps: 1.9 % and
  0.18 % bad CRC, 99.7 % isolated single sectors, no run longer than 2, not
  one frame valid in the raw form — **drive noise, not our layout**.
  Subchannel is delivered with no error correction; the Settlers CloneCD
  dump has 0 bad frames in 344,876.

- **Fixed: MDS mode `0xEC` read as audio.** NFS Porsche Unleashed's v1.3
  MDS came out as one 281,279-sector CD-DA track — no L-EC verified
  anywhere, unmountable in a guest. It is Mode 2 XA (every sector header
  says mode 2, the TOC control says 4); `0xEC` is Alcohol's mixed mode 2.
  Now `mode2 form1` throughout, and an MDS whose mode byte contradicts its
  TOC control bits is refused outright rather than silently misread.

- **Not fixed, by decision: the undeclared-pregap guess** (doc 17 §2.6).
  Synthesizing Q where the descriptor declares no index 00 is a choice
  between three conventions real discs use, and no descriptor field
  distinguishes them (`pregap` is 0 for every such track in both MDS
  files). Ours reproduces AoE Gold exactly — 277,626 of 277,626 ADR 1
  frames — and misses ~0.7 % on a Moto-Racer-shaped disc. Switching to
  Moto's convention was implemented, measured and reverted: it fixed 1,633
  frames on one disc and broke 1,866 and 1,650 on two others. Anything that
  really reads subchannel wants a dump that carries it.
  **And the residual does not reach a game:** AoE Gold and Moto Racer are the
  very two discs the conventions differ on, and both play their CD audio
  in-game (above), so the ~0.7 % of Q frames we get wrong on a
  Moto-Racer-shaped disc is below what a title's own audio code looks at. The
  guess is safe to keep, not just cheapest to keep.

## State (2026-09-04, evening)

- **Step 6 done** (same commit as step 5): the voice in patch 51
  (`ide-dev.h` `QEMUSoundCard card` on `IDEDevice`, `ide-dev.c`
  `DEFINE_AUDIO_PROPERTIES` + `AUD_register_card` at realize when
  `audiodev=` is set, `ide_atapi_audio_init` opens `cd-audio` at 44100 Hz
  S16 stereo; `atapi_audio_cb` writes one routed sector at a time, carries
  a partial write, flips to 0x13 at the end and 0x14 on a data sector or a
  lost medium; without a voice the timer/clock position of step 5 stays),
  MODE SELECT(10) PIO and DMA, `guest-tools/src/cdtest.c` (+
  `build-wrappers.sh` line), `CDTEST=` in `tools/xp-cdimage-test.sh`,
  `scripts/test.sh` builds `CDTEST.EXE` with mingw for the
  `guest-cdimage` check, the DOS test's data-out op (MODE SELECT page 0E
  then MODE SENSE readback, a short list → 05/1A/00). **Gotcha:** a new
  PIO end-transfer function must be added to core.c's `ide_is_pio_out`
  (and `transfer_end_table`) or QEMU aborts on the first data word; a
  static function in atapi.c cannot be, hence the exported
  `ide_atapi_data_out_done`. **Gotcha:** meson does not track
  `liblibdisc.a`: after a libdisc change run `scripts/build-libdisc.sh`
  (cargo + relink), a plain ninja keeps the old code in the binaries (an
  MDS "not a disc image libdisc reads" from qemu-img was that).
  **Gotcha:** `pkill -f <pattern>` matches the shell that runs it when
  the pattern is on its own command line (memory note; bitten again).
  **From the XP trace:** cdrom.sys probes GET CONFIGURATION per feature
  (starting feature 001E, 001F, 0020…, 0103 with RT 0) — stock QEMU (and
  the no-disc path) answer 05/24/00 to any start but 0; the disc path now
  lists the features from the starting one on (RT 2: that one alone,
  header only when unsupported). XP's MCI `play cd from 2` is MODE SELECT
  page 0E, PAUSE, SEEK, PAUSE, PLAY AUDIO MSF (track start → lead-out),
  PAUSE, RESUME; an MCI `resume` after the play completed re-issues the
  whole sequence, so `CDTEST.EXE`'s wav holds the track twice (expected).
  One of three CD-DA runs lost `cdtest.log` after the polling loop
  (CDTEST.EXE exited silently, no crash dialog on this image); the reruns
  passed — watch for it.
- **Step 5 done** (commit "M5 step 5"): patch `51-atapi-disc-model`
  (945 lines; `atapi.c`, `ide-internal.h`, `core.c`, `ide-dev.h`; README
  row), `tools/atapi-guest-test.py` (wired as `atapi-guest`), `discx
  scan`, `mds.rs` (M5e's MDS/MDF brought forward: real dumps exist).
  Findings: the DMA path fills whole chunks from the model and re-enters
  through `replay_bh_schedule_oneshot_event` (stack flat, completion
  asynchronous, XP's cdrom.sys copies 49 files by DMA); the PIO loop's
  disc branch fills synchronously and never recurses; READ CD requests
  without the EDC/ECC field verify L-EC (so READ(10) through the same
  fill fails a bad sector with 03/11/05) while raw requests deliver the
  bytes; the audio position is computed from the virtual clock (no
  per-sector timer), one timer flips to "completed"; QEMU's `-cdrom`
  path lands on the secondary master, so the DOS program owns 0x170 with
  nIEN set and polls. Real dumps on the Linux box:
  `/mnt/data2/david/Downloads/oldstuff` — cue/bin (Death Rally, Blood 1+2,
  Duke Atomic, Fire Fight, Vice City "FLT"), MDS/MDF RAW+SUB (AOE Gold,
  Moto Racer, 14 / 12 audio tracks); every cue and mds opens, `scan`
  finds 0 L-EC failures except Fire Fight's 149 audio-format sectors at
  the end of its data track. **None of them is a protected disc**
  (AOE Gold 1999 has no SafeDisc bad sectors); step 7 still needs one.
  MDS layout rule (checked against the AOE dump's own Q frames): the
  `.mdf` holds each track from `start_sector` (index 1) for `length`
  sectors at `start_offset`; the `pregap` sectors are not in the file
  (like a cue `PREGAP`); track 1's pregap 150 is the lead-in pause.
  A data-track sector without a sync pattern (an all-zero filler) is an
  L-EC failure (`Lec::NoSync`, C2 all set): an all-zero sector's EDC and
  parity are zero and would otherwise verify.
- **Step 4 done** (commit "M5 step 4"): `libdisc/qemu/cdimage.c` (~170
  lines, modelled on `block/bochs.c`: `bdrv_apply_auto_read_only`,
  `bdrv_open_file_child`, path from `bs->file->bs->exact_filename`,
  `request_alignment` 2048, `bdrv_co_preadv` = `libdisc_read_cooked` per
  sector, `cdimage_disc()` under `GRAPH_RDLOCK_GUARD_MAINLOOP`),
  `cdimage.h`, patch `50-cdimage-block-driver` (README row), the overlay
  lines in `prepare-qemu.sh`, `cargo build -p libdisc` + `-Dlibdisc_dir`
  in `configure-qemu.sh`. Acceptance all met on the Linux box: `qemu-img
  info` → `cdimage`, 6800 × 2048 for the three selftest images and `raw`
  for `plain.iso`; `qemu-img dd` of the data track == `plain.iso`; audio /
  flipped sectors → `-EIO`; write refused; `nm -D libqemu-embed-i386.so |
  grep -c ' T _ZN3std'` = **0** on Linux (no `--exclude-libs` needed; the
  17 `libdisc_*` functions and `cdimage_disc` are exported, harmless);
  **XP with `-cdrom build/test/disc/gt.cue` (the guest-tools ISO converted
  + a tone track) lists `D:\` as GUESTTOOLS and copies all 49 files
  byte-identical** (`tools/xp-cdimage-test.sh`, KVM, 46 s). `scripts/test.sh`
  gained `cdimage` (host: qemu-img / qemu-io on the selftest images) and
  `guest-cdimage` (guest stage, the XP copy). Header fix on the way: C
  refuses a typedef named like a function, so the structs are
  `LibdiscSectorInfo` / `LibdiscTrackInfo` (doc 17 §3 updated). Gotcha
  for shell checks: `cmd | grep -q` under `set -o pipefail` fails when the
  producer gets SIGPIPE — capture then match.
- **Step 3 done** (commit "M5 step 3"): `ccd.rs`. Tracks from the
  `[Entry]` records with ADR 1 and Point 1..99 (`PLBA` = index 1,
  `Control` = the nibble), modes and `INDEX n=` from `[TRACK n]`, one
  `Session` per distinct `Session=`, lead-out from `Point=0xa2`; the
  `.img` is addressed as `lba × 2352` (inter-session gaps are in the file,
  as libmirage assumes), the `.sub` as `lba × 96` and only while it covers
  the sector (a truncated `.sub` synthesizes past its end). Values parse
  as decimal or `0x` hex. Lead-in entries: A0 carries the first track's
  control, A1 and A2 the last track's — the synthesizer in `mmc.rs` and
  the generator in `discx` both do that now (they disagreed on A2 at
  first; real CloneCD dumps of mixed-mode discs show A2 with the audio
  control). The `ccd` check compares every reply and every sector across
  the three images and covers no-`.sub`, truncated `.sub`,
  `DataTracksScrambled=1` and a missing `.img`.
- **Step 2 done** (commit "M5 step 2"): `mmc.rs` (READ TOC 0/1/2, READ
  SUB-CHANNEL 1/2/3, READ DISC INFORMATION, READ CD length + fill),
  `capi.rs` + `libdisc/libdisc.h` (v1; 17 functions, `nm` shows all 17 as
  `T libdisc_*` in `target/release/liblibdisc.a`). `discx selftest` calls
  the `extern "C"` functions only, via a small `CDisc` wrapper; new checks
  `toc`, `read-cd-length`, `read-cd-fill`, `panic-safety`; `dump toc |
  subq | discinfo | readcd`. Decisions taken while writing it (doc 17 §4
  says the rest): the lead-out descriptor of READ TOC format 0 carries
  control `0x14` for a data disc as real drives report (QEMU's
  `cdrom_read_toc` says `0x16`; the no-disc path keeps QEMU's bytes, the
  disc path the drive's); READ CD with expected type 0 uses the Mode 1
  field lengths, delivers the whole sector when all main fields are
  selected (`0xF8`), a sector's own 2048 user bytes for `0x10` (Mode 2
  form 1 included), and refuses other combinations on non-Mode-1 sectors
  with EMODE; the READ CD length table is the MMC-3 contiguity rule
  (selected non-empty fields must be adjacent in the sector layout), which
  reproduces every legal/illegal entry of tables 356–360; the MCN reply is
  24 bytes (13 digits, NUL, pad), the ISRC reply 24 bytes.
- **Step 1 done** (commit "M5 step 1"): `libdisc/src/{lib,cue,iso,sector,ecc,subq}.rs`
  and `src/bin/discx.rs`. `Disc::open` for `.cue` / `.iso` (content sniff
  for other extensions), `read_raw` / `read_cooked` / `read_sub` /
  `sector_info` / `classify`, EDC/ECC generation and verification,
  `c2_bits`, Q synthesis (ADR 1, MCN at `lba % 100 == 98`, ISRC at 99),
  P pause flag, interleave ⇄ deinterleave. `discx selftest` writes
  `mixed.cue/.bin` (+ `mixed.ccd/.img/.sub` from the model, read back from
  step 3 on), `cooked.cue/.bin`, `plain.iso`, `lec.cue/.bin` and checks
  `msf`, `raw-synth`, `lec`, `edges`, `subq-synth`. Wired into `scripts/test.sh`
  as `libdisc` (host stage). The old `#[test]`s in `msf.rs` are gone.
- **EDC/ECC oracle:** Neill Corlett's `ecm` (ecm-tools 1.03, public domain,
  `pacman -S ecm-tools` or three files from github.com/alucryd/ecm-tools:
  `ecm.c`, `common.h`, `banner.h`, `gcc -O2 -o ecm ecm.c`) strips a sector's
  EDC/ECC only when its own regeneration reproduces them, so `ecm mixed.bin
  x.ecm` reporting `Mode 1 sectors.......... 2000` proves the generator
  byte-exact against the reference implementation (and 1999 on `lec.bin`).
  Its "Mode 2 form 1 sectors... 151" are the all-zero audio pregap sectors
  (ecm's known false positive on zero blocks), not ours. Re-run after any
  change to `ecc.rs`.
- `discx convert plain.iso out.cue --audio tone.wav` produces a MODE1/2352
  cue/bin with one AUDIO track per WAVE (padded to whole sectors, `PREGAP
  00:02:00`); `info` prints the layout, `dump readraw|readcooked|sub|info
  <lba>` one sector.
- The pinned QEMU (v9.2.4) ATAPI layer, as surveyed for doc 17: 19
  commands in `atapi_cmd_table` (`hw/ide/atapi.c` ~line 1280), READ CD
  accepts only byte-9 values `0x10` and `0xF8`, raw sectors are faked by
  `cd_data_to_raw` (sync + BCD header + data, **no EDC/ECC**), the TOC is
  synthesized as one data track from the image size (`hw/block/cdrom.c`),
  no READ SUB-CHANNEL, no audio commands, no READ CD MSF, no MODE SELECT,
  DVD-ROM profile whenever the image is larger than a CD. Every reply the
  guest gets today for anything but a plain data ISO is wrong or missing;
  that is the gap the plan below closes in order.
- Images on hand: `~/vms/bench.iso`, `~/vms/FIFA2000.ISO` (cooked ISOs)
  and, since the evening of 2026-09-04, the user's raw dumps under
  `/mnt/data2/david/Downloads/oldstuff` (cue/bin and MDS/MDF with
  subchannel, see step 5 above): unprotected mixed-mode discs. **No
  SafeDisc / SecuROM dump yet** (step 7): the L-EC path is proven on
  synthetic bad sectors and clean real discs only.
- This track's worktree (`.claude/worktrees/m5-cdrom`) has its own
  submodules (`git submodule update --init --depth 1 …`) and QEMU build
  (`build/qemu` there, ~15 min from scratch on the Linux box).

## Build / test loop

```sh
cargo build --release -p libdisc                     # liblibdisc.a + target/release/discx
target/release/discx selftest build/test/disc        # the host exerciser (step 3 onwards)
scripts/prepare-qemu.sh && scripts/configure-qemu.sh # overlay + patches 50/51 (step 4 onwards)
ninja -C build/qemu qemu-system-i386 qemu-img libqemu-embed-i386.dylib   # .so on Linux
build/qemu/qemu-img info build/test/disc/mixed.cue   # must say "file format: cdimage"
python3 tools/atapi-guest-test.py                    # DOS ATAPI battery vs discx dump (step 5)
scripts/test.sh all                                  # before every commit (policy)
# CD audio headless: the tone track into a wav through MCI (needs mingw for CDTEST.EXE)
CDTEST=build/test/CDTEST.EXE tools/xp-cdimage-test.sh ~/vms/winxp.qcow2 build/test/disc/gt.cue build/test/gt-iso
# XP / Win98 on a converted ISO (step 6), player on the Air:
target/release/discx convert guest-tools/out/guest-tools-3dfx-<rev>.iso build/test/disc/gt.cue
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 512 -hda ~/vms/winxp.qcow2 \
  -drive if=none,id=cd0,media=cdrom,format=cdimage,file=build/test/disc/gt.cue \
  -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0 \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -device sb16,audiodev=embed0
```

Editing a QEMU patch (the M8 track's recipe, verbatim because it bites):
edit the files in `qemu/`, copy them aside, move the patch out of
`patches/qemu/`, run `prepare-qemu.sh` (tree = the previous patches),
then `git -C qemu checkout --` every file that *only* this patch touches,
regenerate with `diff -u` against the copies using `--- a/` / `+++ b/`
headers (new files `--- /dev/null`), put it back, then `prepare-qemu.sh`
twice and compare the tree with the copies byte for byte. Files the
overlay provides (`block/cdimage.c`, `include/block/cdimage.h`,
`include/block/libdisc.h`) are **not** in the patch: they are copied by
prepare and edited in the repo (`libdisc/qemu/`, `libdisc/libdisc.h`).
Never `git checkout` inside `qemu/` between prepare runs otherwise.

## Steps, in order (each ends with a commit that passes its checks)

Every step names its acceptance; do not move on with a failing check,
and do not skip the docs part (status row, README rows, this file's
State section) — they are the handoff.

1. **Model + cue/bin + ISO parsing** — *done 2026-09-04* (`lib.rs`, `cue.rs`, `iso.rs`,
   `sector.rs`, `ecc.rs`, `subq.rs`; doc 17 §2.1–2.6). `Disc::open` for
   `.cue` and `.iso`; `read_raw` / `read_cooked` / `read_sub` /
   `sector_info`; EDC/ECC generation and `verify_mode1`; Q synthesis with
   CRC-16. No unit tests: the proof is step 3's exerciser, so write the
   `discx` skeleton with the image *generator* in this step
   (`selftest` writes `mixed.cue/.bin`, `cooked.cue/.bin`, `plain.iso`)
   and make `raw-synth`, `lec`, `edges`, `msf` pass. Acceptance: `cargo
   build --release -p libdisc` warning-free; `discx selftest` prints PASS
   for those four; a hand check that `discx dump build/test/disc/mixed.cue
   readraw 16` shows the sync pattern, BCD header `00 02 16 01`, and
   non-zero EDC/P/Q.
2. **MMC responders + C API** — *done 2026-09-04* (`mmc.rs`, `capi.rs`, `libdisc/libdisc.h`;
   doc 17 §3–4). READ TOC 0/1/2, READ SUB-CHANNEL 1/2/3, READ CD length
   table + sector fill (all byte-9/byte-10 combinations of the MMC-3
   table), READ DISC INFORMATION. Switch `discx selftest` to call the
   `extern "C"` functions exclusively (the boundary QEMU will use) and add
   `toc0/1/2`, `subq-synth` (against the synthesized `.sub` written in
   step 1: at this point the CCD reader does not exist, so compare the
   synthesizer with itself through the two access paths, `read_sub` vs
   `readcd` subch=1 de-interleaved), the `read_cd_length` table check and
   `panic-safety` (a deliberately corrupt cue must return an error string,
   never abort). Wire `run_check libdisc` into `scripts/test.sh`'s host
   stage and the `discx` row into CLAUDE.md's tools table. Acceptance:
   `scripts/test.sh` host stage green with the new check; `nm
   target/release/liblibdisc.a | grep ' T _libdisc_'` (macOS; no
   underscore on Linux) lists every function in the header.
3. **CCD reader** — *done 2026-09-04* (`ccd.rs`; doc 17 §2.4) including verbatim raw TOC
   entries and `.sub` replay; `selftest` gains `mixed.ccd/.img/.sub` and
   the cross-format identity checks (`toc*` identical across cue, ccd,
   cooked cue; `subq-synth` now stored vs synthesized). Acceptance: all
   `selftest` checks PASS; a CCD without `.sub` opens and synthesizes.
4. **The `cdimage` block driver + patch 50** — *done 2026-09-04* (`libdisc/qemu/cdimage.c`,
   `cdimage.h`, overlay lines in `prepare-qemu.sh`, cargo + `-Dlibdisc_dir`
   in `configure-qemu.sh`, `50-cdimage-block-driver.patch`, README row;
   doc 17 §5.1–5.2). Acceptance: `qemu-img info mixed.cue` → `cdimage`,
   virtual size = `sector_count × 2048`; `qemu-img dd`/`qemu-io -r -c
   "read -v 32768 2048"` of the cue equals the same read of `plain.iso`;
   `qemu-img info plain.iso` still says `raw`; XP boots in the player with
   `-cdrom build/test/disc/gt.cue` (probe path) and lists `D:\`; the
   `cdimage` check joins `scripts/test.sh`; on Linux the `nm -D … _ZN3std`
   check from doc 17 §5.2 is done and recorded here.
5. **Patch 51, data path + TOC + subchannel** — *done 2026-09-04* (audio position tracking included; the voice is step 6) (doc 17 §5.3 without
   §5.4): `atapi_disc_read` PIO and DMA paths, READ(10)/(12) through
   libdisc with the audio-track check and L-EC sense, READ CD / READ CD
   MSF full table, READ SUB-CHANNEL, READ TOC 0/1/2, READ DISC
   INFORMATION, GET CONFIGURATION / mode page 0x2A as a CD-ROM, INQUIRY
   product from `model=`; audio commands accepted as no-ops for now.
   Write `tools/atapi-guest-test.py` (doc 17 §6.2) alongside — it is the
   only way to see the PIO/DMA state machine work, and it doubles as the
   debugging tool: every reply in hex on the serial log. Acceptance:
   `atapi-guest` PASS (every reply identical to `discx dump`, BCL 512 and
   65534, the flipped sector returns 03/11/05 to READ(10) and raw bytes to
   READ CD); XP and Win98 copy the converted guest-tools ISO's files
   (doc 17 §6.3) with matching hashes; `scripts/test.sh all` green,
   including the existing XP guest stage still on `-cdrom <iso>` (the
   no-disc path must be untouched: diff the QEMU log's ATAPI trace lines
   before/after on the ISO boot, `-trace 'ide_atapi*'`).
6. **CD-DA** (doc 17 §5.4) — *done 2026-09-04 except the by-ear Win98 CD Player run and the swap-while-playing check*: `audiodev` property, the voice, play / pause /
   resume / stop / position, page 0x0E + MODE SELECT, the timer fallback,
   `guest-tools/src/cdtest.c` + `build-wrappers.sh` line, the player /
   cheat-sheet `-drive` + `-device ide-cd` lines (doc 00, README),
   `blockdev-change-medium` on a cue while the guest runs (medium-change
   stops audio, the new TOC is seen: Win98 Explorer refreshes).
   Acceptance: `atapi-guest`'s play/position/pause/stop section;
   `CDTEST.EXE` in XP and Win98 with `-audiodev wav` shows the 1 kHz tone
   in the wav; Win98's CD Player plays track 2 audibly in the player on
   the Air.
7. **Real dumps.** Dump one owned mixed-mode disc and one SecuROM-era disc
   with CloneCD (subchannel on) on the rig, one SafeDisc disc raw; copy
   them to `~/vms/discs/` on both machines (not in the repo). Verify: the
   CCD's `[Entry]` TOC through READ TOC format 2 in the guest, `.sub`
   replay, and — the first hard evidence — that `verify_mode1` agrees with
   the dump's own error sectors (SafeDisc: the tool's log of unreadable
   LBAs must be exactly the LBAs `sector_info.lec == 0`). Fix whatever the
   synthesizers got wrong; record the findings in doc 17.
8. **M5c/M5d: the titles.** Install the SecuROM title and the SafeDisc
   title on the XP image (a copy: `winxp-m5.qcow2`), launch from the
   dump. If a check fails, the ATAPI trace (`-trace 'ide_atapi*'` plus a
   per-command hex log behind a `CDIMAGE_TRACE=1` env in atapi.c) shows
   the command the protection issued and the reply; compare with the rig
   (doc 09: write the SPTI logger then). Result row per title in doc 05's
   acceptance table.
9. **M5e**: MDS/MDF (DPM data), CHD (v5, `cdlz`/`cdzl` hunks: needs zlib +
   LZMA + FLAC decoding — evaluate a pure-Rust decode vs shelling out to
   `chdman` on import before committing to it), timing profile only if
   StarForce needs it. Then the disc shelf with M6.

## Gotchas (read before step 1; add to as you go)

- `INDEX` times in a cue are file-relative; `PREGAP`/`POSTGAP` sectors
  are not in the file; track 1 of every image starts at LBA 0. Header
  MSF and Q times are BCD of `lba + 150`; MMC replies are binary.
- `FILE … WAVE`: find the `data` chunk, do not assume a 44-byte header.
  `MOTOROLA` = byte-swapped samples.
- L-EC is verified, never corrected (doc 17 §2.5): a mismatch is the
  SafeDisc signal. C2 bits are approximate until a dump with C2 data
  exists.
- `.sub` is deinterleaved (P..W, 12 bytes each); the MMC "raw" 96-byte
  form is interleaved — interleave only in the READ CD responder.
- Never call `ide_atapi_cmd_reply_end` recursively from a synchronous
  read (return 1 = "filled, continue"); DMA chunks re-enter through a
  bottom half, never inline. `io_buffer` is 131076 bytes.
- The disc handle is fetched from `blk_bs(s->blk)` on every command; a
  medium change swaps the BDS underneath. Never cache it in IDEState.
- `s->nb_sectors >> 2` (2048-byte sectors) comes from the block driver's
  length = lead-out × 2048; it is the right bound for every read command.
- A plain `.iso` must keep probing to `raw`: the existing XP guest stage
  and every recorded number were taken on that path.
- No vmstate for the new IDEState fields: migration/snapshots with a
  `cdimage` medium are unsupported (say so in the README row and in doc
  00 if a snapshot flow ever matters).
- Two Rust `std` copies in the player process (libdisc inside
  libqemu-embed, the player itself): harmless, but check the export list
  on Linux (doc 17 §5.2) so nothing leaks.
- New patches: git-format diffs, forward-applied from a pristine tree
  before pushing (`patches/qemu/README.md` recipe). The `hw/ide` files are
  not touched by any other patch in the queue today; keep it that way.
- End scripted Win98 runs with a Start-menu shutdown (CLAUDE.md); Win98
  ScanDisk after a killed VM costs the next boot.
