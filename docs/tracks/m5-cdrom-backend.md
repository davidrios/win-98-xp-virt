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

## State (2026-09-04, evening)

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
- Images on hand: `~/vms/bench.iso`, `~/vms/FIFA2000.ISO` (cooked ISOs);
  **no raw dump (cue/bin or CCD) exists on either machine yet.** Until one
  does, EDC/ECC is verified by the `ecm` oracle above and Q synthesis only
  by self-consistency (doc 17 §6.1). The first real dump of an owned disc
  is a milestone in itself (step 7).
- This track's worktree (`.claude/worktrees/m5-cdrom`) has no prepared
  `qemu/` tree yet: `git submodule update --init --depth 1 qemu
  third_party/qemu-3dfx` then the usual prepare → configure → ninja before
  step 4; `scripts/test.sh`'s `x87-fast` reports a build failure there
  until then (it compiles against `qemu/target/i386/tcg` headers).

## Build / test loop

```sh
cargo build --release -p libdisc                     # liblibdisc.a + target/release/discx
target/release/discx selftest build/test/disc        # the host exerciser (step 3 onwards)
scripts/prepare-qemu.sh && scripts/configure-qemu.sh # overlay + patches 50/51 (step 4 onwards)
ninja -C build/qemu qemu-system-i386 qemu-img libqemu-embed-i386.dylib   # .so on Linux
build/qemu/qemu-img info build/test/disc/mixed.cue   # must say "file format: cdimage"
python3 tools/atapi-guest-test.py                    # DOS ATAPI battery vs discx dump (step 5)
scripts/test.sh all                                  # before every commit (policy)
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
4. **The `cdimage` block driver + patch 50** (`libdisc/qemu/cdimage.c`,
   `cdimage.h`, overlay lines in `prepare-qemu.sh`, cargo + `-Dlibdisc_dir`
   in `configure-qemu.sh`, `50-cdimage-block-driver.patch`, README row;
   doc 17 §5.1–5.2). Acceptance: `qemu-img info mixed.cue` → `cdimage`,
   virtual size = `sector_count × 2048`; `qemu-img dd`/`qemu-io -r -c
   "read -v 32768 2048"` of the cue equals the same read of `plain.iso`;
   `qemu-img info plain.iso` still says `raw`; XP boots in the player with
   `-cdrom build/test/disc/gt.cue` (probe path) and lists `D:\`; the
   `cdimage` check joins `scripts/test.sh`; on Linux the `nm -D … _ZN3std`
   check from doc 17 §5.2 is done and recorded here.
5. **Patch 51, data path + TOC + subchannel** (doc 17 §5.3 without
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
6. **CD-DA** (doc 17 §5.4): `audiodev` property, the voice, play / pause /
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
