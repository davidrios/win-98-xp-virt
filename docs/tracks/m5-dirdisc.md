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

Opened 2026-09-06 on `track/m5-dirdisc`, branched off `main`; nothing
written yet — this doc is the whole of it. Rebase on `main` before
each step and keep the shared files (§Scope) to minimal edits.

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
- Shared (rebase first, edit minimally, name the track in the commit):
  `launcher/src/disc_library.rs`, `launcher/src/discshelf.rs`,
  `launcher/src/bundle.rs` (the `-drive …,file=` line, `bundle.rs:523`),
  `launcher/src/control.rs` (live insert) — all M6's files; `CLAUDE.md`'s
  tools table.

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

## The launcher side (M6's files, small)

- `disc_library.rs`: a shelf entry may be a directory. `DISC_FILTER`
  (`:24`) stays for files; add an "Add folder…" button next to
  "Add disc…" (`rfd`'s folder picker). `default_label` for a directory
  is the folder's name.
- `write_shelf_file` (`:120`) writes `isodir:<path>` for a directory —
  the shelf line file is what the drive reads, so the prefix belongs
  there, not in the C.
- `bundle.rs:523` (`,file={}`) and `control.rs`'s live insert take the
  same prefixed string. **While there:** a path containing a comma
  breaks a QEMU option string and needs its commas doubled; that bug is
  already live for disc paths and a folder picked out of a user's
  Documents is exactly how someone finds it.

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

1. **The layout generator + the model additions.** `isodir.rs`,
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
2. **The QEMU driver.** `bdrv_isodir` in `libdisc/qemu/cdimage.c`,
   `cdimage_disc()` taught both drivers.
   *Acceptance:* `qemu-img info isodir:<dir>` says `file format: isodir`
   and `qemu-img convert -f isodir isodir:<dir> out.iso` is byte-identical
   to step 1's `discx convert` output; `qemu-img info --output=json` shows
   no node above `isodir` (or `cdimage_disc()` handles it and a debug
   print proves the handle is found); prepare-qemu twice leaves an
   identical tree.
3. **XP, end to end.** `tools/xp-cdimage-test.sh` already boots XP, copies
   the whole disc through cdrom.sys and diffs every file against a
   reference directory — point it at `isodir:<dir>` with *that same
   directory* as the reference.
   *Acceptance:* every file identical, long names intact in Explorer's
   view (the copy proves it), the QEMU log free of medium errors; the
   guest-tools folder (`guest-tools/out/iso`) served as a disc installs
   through `SETUP /ALL` exactly like the burned ISO does. Wire the run
   into `scripts/test.sh`'s guest stage next to `guest-cdimage`.
4. **The launcher and the shelf.** The four edits above.
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
