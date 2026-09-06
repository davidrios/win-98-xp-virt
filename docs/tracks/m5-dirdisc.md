# Track: M5g — a host directory as a CD-ROM (`isodir:`)

The handoff for the session that makes **"share this folder with the
guest" a disc in the drive**: a host directory served as a read-only
ISO 9660 + Joliet volume, generated lazily inside `libdisc` — no image
file written, no `xorriso` at run time, nothing copied.

Read `docs/00-status.md` first (global picture, track rules), then this
file, then doc 17 (`libdisc`'s spec: the model, the raw ⇄ cooked
synthesis, the C API, the QEMU hooks — do not re-derive what it fixes)
and the M5 track doc `docs/tracks/m5-cdrom-backend.md`, whose code this
extends. This file is the *plan and the ISO 9660 spec* for the new
source; if it outgrows the track, its §"The layout" moves to doc 17 §8.

Opened 2026-09-06 on `track/m5-dirdisc`, branched off `main`. Rebase on
`main` before each step and keep the shared files (§Scope) to minimal
edits.

## State (2026-09-06: steps 1–4 landed)

`libdisc/src/isodir.rs` generates the volume, `discx` exercises it, and
`scripts/test.sh`'s new **`dirdisc`** check hands the result to an ISO
9660 reader that is not ours. On this Mac: `discx selftest` 12 passed 0
failed (the new `dirdisc` case among them), `scripts/test.sh host` 9
passed 0 failed 3 skipped, and `xorriso` extracts the fixture tree back
out of the generated volume identical to the folder it was served from —
`diff -r` clean but for the two names Joliet cannot hold, which come back
mangled as designed. `qemu-img` was relinked against the new staticlib
and the existing `cdimage` check still passes, so the payload change did
not disturb the image path.

What is in place:

- **`isodir.rs`** — the walk, the two name trees, path tables, directory
  records, both volume descriptors, the layout and the extents. No
  dependencies (UCS-2, the civil-date conversion and the sort are all
  hand-written; the crate links into QEMU and keeps none).
- **`Source::Mem`** for the metadata blob and **`eof_pad`** on
  `Source::File`, the flag that says an extent's last sector may run past
  the end of its file — set only here, so a short read on an image file
  is still the error it always was.
- **Lazy payload handles** with an 8-entry MRU cache and a re-`stat` on
  every open: a changed file is `Error::Medium`, the read error a drive
  gives for a damaged sector, not a torn read. `add_file` still opens
  once so a missing payload fails at open time as before.
- **`extent_at` is a binary search** now. It was a linear scan, which
  costs a comparison per shared file on every sector read once a disc has
  thousands of extents rather than a handful.
- **`discx export` / `discx mktree`**, and the `dirdisc` case in
  `discx selftest`: the volume describes itself (PVD/SVD/terminator,
  space size, block size, the Joliet escape), the mangled identifiers are
  in the tree that should carry them, every file extent read through the
  **C API** equals the host file, the tail padding is there, two opens of
  an unchanged tree are byte-identical, a file changed under an open disc
  reads as `EMEDIUM`, and a symlink loop is refused.

Step 2 added the `isodir` BlockDriver to `libdisc/qemu/cdimage.c` — a
protocol driver with `bdrv_parse_filename`, no `file` child, and
everything else shared with `cdimage` (the same state, reads, close, and
the same `cdimage_disc()` handle reaching `hw/ide/atapi.c`). No QEMU
patch changed: that file is ours.

**The unknown resolved the way the doc feared.** `qemu-img info` on
`isodir:<dir>` reports a **`raw` format node above the `isodir` one**:
the block layer probes for a format on top of a protocol driver it found
by filename prefix, and `raw` matches anything. vvfat lives with the
same. `cdimage_disc()` therefore walks down through format nodes as well
as filters, and the check below is what proves it rather than a reading
of `block.c`.

Measured here: `qemu-img info` names `isodir`, `qemu-img convert -O raw`
is byte-identical to `discx export`, and **SeaBIOS probing the drive
issues 4 packets that reach the disc model** (TEST UNIT READY and a
READ(10) of LBA 17) with `CDIMAGE_TRACE=1`, where the same run on a
plain `.iso` — the raw driver, no model — issues none. Both are in the
suite's `dirdisc` check now; the boot ends on SeaBIOS' own "No bootable
device" through `-debugcon`, so it costs about a second each.

Step 3 ran **on the Mac**, which the doc had assumed impossible: XP
copies the folder through cdrom.sys and every file comes back identical.

```
RUN.BAT started after 30 s
copy done after 33 s; shutting XP down
xcopy: 311 arquivo(s) copiado(s), rc 0
PASS: 311 files copied from the disc match the reference
```

`D:` is the volume: `dir` shows `café.txt` with its accent, `Program
Files.txt` with its space, the 58-character name, `EMPTY.BIN` at 0 bytes
and `odd2049.bin` at 2049 — Joliet, read by XP's own driver. The fixture
is `discx mktree` minus the two names Joliet cannot hold (they would be
reported as missing by a comparison that knows nothing about mangling;
the host `dirdisc` check covers them). `scripts/test.sh`'s guest stage
gained **`guest-dirdisc`**, which serves the extracted guest-tools ISO
directory as a folder and compares against that same directory — the
disc being generated on the fly is then the only difference from the
`guest-cdimage` check next to it.

`tools/xp-cdimage-test.sh` needed three fixes to run here, all of them
improvements on the Linux side too (§Gotchas): the scratch disk is built
with mtools where `sfdisk`/`mkfs.fat` are absent, the wait watches COM1
rather than the guest's unflushed FAT writes, and `tr` runs in the C
locale.

Step 4 put it in the launcher. The whole choice is one function —
`disc_library::qemu_medium(path)`, `isodir:<path>` for a directory and
the plain path for a file — and everything that names a medium to QEMU
goes through it: the boot drive (`bundle.rs`), a live insert
(`control.rs`) and the flat shelf file the in-guest CDSHELF reads. It is
decided from the path each time rather than remembered, so a folder that
has since been deleted is a missing file, which is what it is.

Both front ends grew an **"Add folder…"** button (egui through `rfd`'s
folder dialog, Qt through `FolderDialog` — the same backends `PathField`
already reaches), because no name filter can express "a folder". A
folder's shelf label is its own name, extension and all: `patch13` would
otherwise lose its `.3` to a file stem. New headless verbs to match:
`--pick-folder`, next to the existing `--pick-file`, and `--discs publish
<bundle dir>`, which is what the GUI does by itself when the shelf
changes under a running machine.

**The comma bug the track doc predicted was real and older than this
track**: QEMU option strings separate on commas, so a disk in
`~/Games/Doom, Quake and friends/` silently became an unknown option and
QEMU refused the whole line. `bundle.rs` doubles them now — for the disk,
the floppy, the disc and the shelf path, not only the one this track
touched.

The suite's new **`dirshelf`** check runs the launcher's own headless
verbs over three folders (one with a space, one with a comma, one plain):
each is on the shelf under its own name, the shelf file names it as
`isodir:` and only that way, `--print-args` puts the prefix on the boot
drive with the comma doubled, and our QEMU is handed the result and opens
it. `scripts/test.sh host`: 14 passed, 0 failed, 3 skipped.

Next: step 5 — Win98 and DOS legs, the refusals and warnings each with a
case, and the stale-file rule watched in a guest.

## Why it is small

`libdisc` never required an image *file* anywhere in its read path. A
`Disc` is sessions → tracks → **extents**, and `Source::File { layout:
Cooked2048 }` already means "2048 bytes from a host file, sync + header +
EDC/ECC synthesized" (`libdisc/src/lib.rs:450`, `synth_from_cooked`). A
folder-as-disc is therefore not a new subsystem: it is a new *layout
builder* that emits one extent per host file plus a few synthesized ones
for the ISO 9660 metadata. Everything downstream — `read_cooked` /
`read_raw` / `read_sub`, the MMC responders, patch 51's ATAPI paths,
`qemu-img`, the disc shelf — is untouched.

## Locked decisions (do not reopen in this track)

1. **Read-only.** A CD is read-only; so is this. Getting files *out* of
   a guest is the FAT scratch disk the test scripts already use
   (`~/vms/scratch.img` as `-hdb`), never vvfat rw.
2. **The tree is snapshotted at insert**, like a disc that was burned.
   Host edits appear on the next eject/reinsert — which is exactly what
   the shelf's `LOAD` performs (patch 52), and it raises the UNIT
   ATTENTION that makes XP's cdfs drop its cached directory.
3. **Lazily generated, never written out.** No temp ISO, no copy of the
   tree, no run-time dependency on `xorriso` (build-only today, and it
   stays that way). `discx convert` can still write a real `.iso` from a
   folder — that is the test oracle, not the mechanism.
4. **Joliet for Windows, ISO 9660 level 1 (8.3) for DOS**, in one
   volume. No Rock Ridge (nothing in our guests reads it). Both trees
   point at the *same* file extents; a file is laid out once.
5. **A protocol prefix, not a probe.** `isodir:/path/to/folder`. A
   directory cannot be probed, and it cannot be a block-layer `file`
   child; QEMU's own answer to this is vvfat's `fat:` prefix
   (`qemu/block/vvfat.c:3254`) and we mirror it.
6. **No new QEMU patch.** `block/cdimage.c` is *our* overlay file
   (`libdisc/qemu/cdimage.c`), copied into the tree by
   `prepare-qemu.sh`; the new driver is an edit of a file we own. Patch
   50 gains nothing, patch 51 gains nothing.
7. **Not bootable.** No El Torito boot catalogue in this track. (If a
   guest ever needs to boot from a folder, it is a boot-image file named
   in the folder and a catalogue at a fixed LBA — a later step, not a
   redesign.)

## Scope and files (this track owns them)

- `libdisc/src/isodir.rs` — new: the layout generator.
- `libdisc/src/lib.rs` — the two model additions below.
- `libdisc/src/bin/discx.rs` — `selftest`'s `dirdisc` case, `info` /
  `dump` / `convert` accepting a directory.
- `libdisc/qemu/cdimage.c`, `libdisc/qemu/cdimage.h` — the `isodir`
  BlockDriver next to `cdimage`.
- `libdisc/libdisc.h` — comment only (see "No API bump" below).
- `docs/tracks/m5-dirdisc.md` (this file), the M5g row in
  `docs/00-status.md`, a pointer from doc 17 §5.1 and doc 05.
- Tests: the `dirdisc` case in `scripts/test.sh`, a folder run of
  `tools/xp-cdimage-test.sh`.
- Shared (rebase first, edit minimally, name the track in the commit) —
  all M6's files, and all moved by main's launcher split on 2026-09-06:
  `launcher-core/src/{disc_library,bundle,control,cli,shelf}.rs`, the two
  front ends (`launcher/src/{discshelf,filepicker,main}.rs`,
  `launcher-qt/qml/DiscShelfWindow.qml`); `CLAUDE.md`'s tools table.

## The layout (what step 1 must produce)

Sector = 2048 cooked bytes = one LBA. The volume is one Mode 1 data
track from LBA 0 to the lead-out; `libdisc` synthesizes sync, BCD header
and EDC/ECC for every sector, as it already does for `.iso` files.

| LBA | Contents |
|---|---|
| 0–15 | system area, zero |
| 16 | Primary Volume Descriptor (type 1, `CD001`, version 1) |
| 17 | Supplementary VD for Joliet (type 2, escape `%/E`, UCS-2 level 3) |
| 18 | Volume Descriptor Set Terminator (type 255) |
| 19… | L (little-endian) and M (big-endian) path tables, primary then Joliet |
| … | directory records: the primary tree, then the Joliet tree |
| … | file extents, 2048-aligned, one contiguous run per file |
| last | 150 zero sectors of tail padding |

Rules that are easy to get wrong, fixed here:

- **Both-endian fields.** Extent LBA and data length in a directory
  record are 8 bytes: LE then BE. Path-table pointers in the PVD are
  four separate 4-byte fields (L, optional L, M, optional M) — the L
  fields little-endian, the M fields big-endian. Write 0 for the
  optional copies.
- **Directory record date** is 7 binary bytes (year − 1900, month, day,
  hour, minute, second, GMT offset in 15-minute units), from the host
  file's mtime. The PVD's volume dates are the 17-byte decimal-digit
  form. Do not mix them up.
- **Every directory begins with its `.` and `..` records** (identifiers
  `0x00` and `0x01`, length 1); the root's `..` points at the root.
- **A directory's records never straddle a sector boundary**: pad with
  zeros to the next sector when the next record would not fit.
- **Path-table records** are ordered by level, then by parent number,
  then by identifier; a parent's number is always ≤ its child's. Number
  the root 1. The Joliet tree has its own path tables with the same
  shape and UCS-2 identifiers.
- **Names, primary tree:** ISO 9660 level 1 — uppercase `A–Z 0–9 _`,
  8.3, files carry `;1`, directories carry no extension and no version.
  Anything else is replaced by `_`; collisions are resolved with `~1`,
  `~2`, … in the deterministic order below. This is the tree MSCDEX
  reads, and it is the reason we do not simply use level 2.
- **Names, Joliet:** UCS-2 big-endian, ≤ 64 characters per element,
  `* / : ; ? \` and controls replaced by `_`, `;1` appended (mkisofs
  does; Windows hides it). Encode by hand — `libdisc` has no
  dependencies and keeps none.
- **Deterministic order:** directories breadth-first, entries within a
  directory sorted by their *primary* identifier (that is also the order
  ISO 9660 requires), files laid out in that same walk. Two runs over an
  unchanged tree must produce an identical image, byte for byte — step
  2's acceptance depends on it.
- **Zero-length files:** data length 0, extent LBA 0, no sectors
  allocated.
- **Volume space size** (PVD) = total sectors including the system area
  and the tail padding; it is also the lead-out LBA and what READ
  CAPACITY reports.
- **Refuse, with a message naming the path:** a file ≥ 4 GiB (single
  extent only; multi-extent is a Windows-version minefield), a tree
  deeper than 30 levels, a symlink loop. **Warn and continue:** a tree
  deeper than 8 levels (ISO 9660's limit; Windows copes, MSCDEX may
  not), a file ≥ 2 GiB (dicey on Win98), > 65535 directories.
- Symlinks are followed for regular files and directories, refused when
  they escape the shared root by more than one resolution (report the
  path); sockets, fifos and devices are skipped with a warning.

## The model additions

1. `Source::Mem { blob: usize, offset: u64 }` in `libdisc/src/lib.rs:186`
   — cooked 2048-byte blocks from an in-memory `Vec<u8>` held by the
   `Disc` (descriptors, path tables, directory records). A few hundred
   KB for tens of thousands of files. `read_raw` handles it exactly like
   `Layout::Cooked2048`.
2. **Lazy file handles.** `add_file` opens every payload eagerly today;
   a folder with 5,000 files would blow past macOS's 256-fd default. So
   `Payload` keeps `path`, `len`, `mtime` and opens on demand behind a
   small mutex-guarded MRU cache (8 entries — sequential reads inside
   one file are the dominant pattern). On every open, re-`stat`: a
   changed size or mtime is `Error::Medium`, i.e. the guest gets an L-EC
   read error exactly as it would from a scratched disc, instead of a
   torn file. That is the whole answer to "the user edited the folder
   while it was mounted", and it is honest.

**No API bump.** Nothing in `libdisc.h` changes shape: `libdisc_open` on
a directory path is the whole interface, so `LIBDISC_API_VERSION` stays
1. Only the header's "carries no per-handle mutable state" sentence needs
a word (still thread-safe; now with a lock).

## The QEMU side

A second `BlockDriver` in `libdisc/qemu/cdimage.c`, mirroring vvfat:

```c
static BlockDriver bdrv_isodir = {
    .format_name         = "isodir",
    .protocol_name       = "isodir",     /* isodir:/path/to/folder */
    .instance_size       = sizeof(BDRVCdimageState),
    .bdrv_parse_filename = isodir_parse_filename,   /* strips the prefix into options["dir"] */
    .bdrv_open           = isodir_open,             /* libdisc_open(dir), no file child */
    .bdrv_refresh_limits = cdimage_refresh_limits,  /* shared */
    .bdrv_co_preadv      = cdimage_co_preadv,       /* shared */
    .bdrv_close          = cdimage_close,           /* shared */
};
```

`cdimage_disc()` must accept **both** drivers, and that is where the one
real unknown sits: a protocol driver opened by filename prefix can end
up with QEMU probing a *format* on top of it (vvfat lives with this).
Step 2 checks the node graph and, if a `raw` node does appear above
`isodir`, `cdimage_disc()` walks down through format nodes rather than
only `bdrv_skip_filters`. Get this wrong and the symptom is silent: the
disc reads fine and the ATAPI path falls back to QEMU's stock handler,
so the guest sees a drive with no TOC of ours.

The shelf needs nothing: patch 52's `LOAD` passes the filename to
`blockdev_change_medium` with **no format** and lets the block layer
resolve it (`52-atapi-disc-shelf.patch:159`), which is precisely what a
protocol prefix does.

## The launcher side (M6's files, small) — done, see State

One function decides it (`disc_library::qemu_medium`), three callers use
it (the boot drive, the live insert, the flat shelf file), and both front
ends grew an "Add folder…" button because no name filter can express "a
folder". The comma doubling landed with it.

## Build / test loop

```sh
cargo build --release -p libdisc
target/release/discx selftest build/test/disc          # gains the dirdisc case
target/release/discx info isodir:build/test/dirsrc     # tracks, size, the mangled names
target/release/discx convert isodir:build/test/dirsrc build/test/dir.iso
bsdtar -tvf build/test/dir.iso                         # an independent reader's opinion
scripts/prepare-qemu.sh && scripts/configure-qemu.sh   # overlay picks up cdimage.c
ninja -C build/qemu qemu-system-i386 qemu-img libqemu-embed-i386.dylib
build/qemu/qemu-img info isodir:build/test/dirsrc      # "file format: isodir", no raw node
tools/xp-cdimage-test.sh ~/vms/winxp.qcow2 isodir:build/test/dirsrc build/test/dirsrc
scripts/test.sh all                                    # before every commit (policy)
```

## Steps, in order (each ends with a commit that passes its checks)

Every step names its acceptance; do not move on with a failing check,
and do not skip the docs part (this file's state, the status row) — that
is the handoff.

1. **The layout generator + the model additions** — *done 2026-09-06*. `isodir.rs`,
   `Source::Mem`, lazy payloads, `Disc::open` dispatching a directory
   path (and `isodir:` stripped if present) to it, `discx` accepting a
   directory for `info` / `dump` / `convert`.
   *Acceptance:* a `dirdisc` case in `discx selftest` builds a deliberately
   nasty tree — a 0-byte file, a 3 MB file, 300 entries in one directory,
   9 levels deep, names with spaces / accents / `*` / two files whose 8.3
   forms collide / a name at Joliet's 64-character limit — writes it out
   with `convert`, extracts it with `bsdtar`, and every file comes back
   **byte-identical with its original name**; the DOS names are checked
   against the expected mangling table; a second run produces the same
   image byte for byte. `cargo build --release -p libdisc` warning-free.
   Add the check to `scripts/test.sh` (`dirdisc`, host stage) in this step.
2. **The QEMU driver** — *done 2026-09-06*. `bdrv_isodir` in `libdisc/qemu/cdimage.c`,
   `cdimage_disc()` taught both drivers.
   *Acceptance:* `qemu-img info isodir:<dir>` says `file format: isodir`
   and `qemu-img convert -f isodir isodir:<dir> out.iso` is byte-identical
   to step 1's `discx convert` output; `qemu-img info --output=json` shows
   no node above `isodir` (or `cdimage_disc()` handles it and a debug
   print proves the handle is found); prepare-qemu twice leaves an
   identical tree.
3. **XP, end to end** — *done 2026-09-06*. `tools/xp-cdimage-test.sh` already boots XP, copies
   the whole disc through cdrom.sys and diffs every file against a
   reference directory — point it at `isodir:<dir>` with *that same
   directory* as the reference.
   *Acceptance:* every file identical, long names intact in Explorer's
   view (the copy proves it), the QEMU log free of medium errors; the
   guest-tools folder (`guest-tools/out/iso`) served as a disc installs
   through `SETUP /ALL` exactly like the burned ISO does. Wire the run
   into `scripts/test.sh`'s guest stage next to `guest-cdimage`.
4. **The launcher and the shelf** — *done 2026-09-06*, against the
   post-split layout (`launcher-core` + two front ends, not the single
   `launcher/` this doc was written for).
   *Acceptance:* a folder added to the shelf, inserted into a running XP
   from the launcher, listed and read by `CDSHELF LIST` / `CDSHELF <n>`
   in the guest (`tools/cdshelf-guest-test.sh`'s pattern), ejected; a
   folder set as a machine's boot disc survives a bundle round-trip; a
   path with a comma in it works.
5. **The other guests, and the edges.** Win98 (`-vga cirrus`, TCG) reads
   the same folder; a DOS leg proves the 8.3 tree under MSCDEX; the
   refusals and warnings of "The layout" each get a case in the
   `dirdisc` selftest; the stale-file rule is watched to produce a read
   error rather than a torn file (touch a file mid-run).
   *Acceptance:* all three families read the same folder; `scripts/test.sh
   all` green; doc 05's guest-visibility section, doc 17 §5.1, the tools
   table in `CLAUDE.md` and this file's state updated.

## Gotchas (read before step 1; add to as you go)

- **`libdisc` has no dependencies and keeps none** (it links into QEMU).
  UCS-2 encoding, date conversion and the directory walk are hand-written.
- The mandatory 150-sector tail padding is not decoration: guest
  drivers read ahead past the last file extent, and every real ISO has it.
- `bsdtar` keeps ISO 9660's read-only modes — `scripts/test.sh:262`
  already has to make the previous extraction deletable first. Do the
  same in the `dirdisc` case.
- A directory served to a *running* guest that then changes on the host
  is decision 2's job, not a bug report: eject and re-insert.
- Never `git checkout` inside `qemu/` between `prepare-qemu.sh` runs;
  `block/cdimage.c` comes from `libdisc/qemu/`, not from a patch.

Learned in step 1:

- **libarchive normalizes names to NFD on macOS.** `bsdtar` extracting
  our volume turns `café.txt` into `café.txt` and `diff -r` then
  reports the file as missing from both sides. The volume carries the
  host's own bytes and `xorriso` round-trips them exactly, so the
  `dirdisc` check prefers xorriso and, when it falls back to bsdtar on a
  Mac, excludes that one name. No guest does this.
- **libarchive walks an ISO in extent order**, not directory order, so
  its listing is the order files were laid out — which is how the layout
  order was confirmed, and why an entry can appear far from its
  neighbours in a listing without anything being wrong.
- **An empty file needs a plausible extent.** Addressed at LBA 0, inside
  the system area, it is the kind of thing a reader drops; it gets the
  first file extent's LBA with a length of 0 instead.
- The 8.3 mangling has to be decided from a **sorted** directory listing,
  or which of two colliding names gets `~1` depends on the order the
  filesystem happened to hand back and two runs stop agreeing.

Learned in step 2:

- **A `raw` format node does end up above the protocol node**, exactly as
  the risk note said, and nothing about it is visible from the guest: a
  `cdimage_disc()` that stops there returns NULL, the ATAPI path falls
  back to QEMU's stock answers, and the guest still reads files fine. It
  is only the commands the model answers — READ CD, the TOC, the sense of
  a bad sector — that would quietly go missing. `CDIMAGE_TRACE=1` plus a
  SeaBIOS probe is the cheapest proof that the handle is found, and it
  needs no guest image.
- macOS has no `timeout(1)`; the suite's probe waits for SeaBIOS' "No
  bootable device" on `-debugcon` and kills the machine itself.
- `$OUT` in `scripts/test.sh` is already absolute; a `"$PWD/$OUT/…"`
  built for QEMU is a path that does not exist, and `qemu-img` says only
  that it could not open it.
- A probe that waits for a marker in a log file has to **delete the log
  first**. The SeaBIOS check read the previous run's "No bootable
  device", killed the machine before it had probed anything, and
  reported that the drive saw no disc model.

Learned in step 3 (all in `tools/xp-cdimage-test.sh`, which now runs on
both systems):

- **XP boots in about 35 seconds under TCG on the M1** — `RUN.BAT started
  after 30 s`, the whole 311-file copy done at 33 s. Nothing here is slow
  enough to need a generous timeout; when a run takes minutes, something
  is wrong rather than slow.
- **The guest's FAT writes are not visible to the host when they matter.**
  XP's lazy writer held `E:\OUT\STARTED.TXT` for minutes: `mdir` on the
  host showed the directory appear and stay empty while the batch file
  was already through the copy. The script polled that file to know the
  run had started, so it kept re-launching `RUN.BAT` every 11 seconds and
  timed out having done the work several times over. It waits on **COM1**
  now (a host file written as the guest writes it), with the FAT poll as
  a fallback; the artefacts are still read off the FAT, after the
  shutdown flushes it.
- **`mformat` leaves the BPB's hidden-sectors field at 0.** The partition
  starts at LBA 2048, so XP does not mount the volume at all — no E:, and
  a test that only says the batch file never ran. `-H 2048` fixes it;
  `mkfs.fat --offset` had always set it, which is why the Linux path
  never met this.
- macOS has no `truncate`, no `du -sb`, and GNU's `stat -f` means
  something else entirely, so the size helpers decide once which system
  they are on. BSD `tr` also refuses the guest's CP-850 bytes in a UTF-8
  locale — one accented filename in the listing is enough — so it runs
  under `LC_ALL=C`.

Learned in step 4:

- A check that re-splits `--print-args`' flat line in the shell **cannot
  carry a path with a space** — the launcher spawns an argv and never
  has this problem, so it is the harness's limit, not the product's. The
  space case is asserted on the string; the comma case, which is what an
  option parser actually cares about, is the one handed to QEMU.
- `--print-args` puts `file=` last in the `-drive` value, so a pattern
  that expects a trailing comma after it never matches. Two of the first
  three failures in the new check were the check, not the code.
