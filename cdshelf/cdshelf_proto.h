/*
 * cdshelf_proto.h — the in-guest disc-shelf protocol (doc 07's disc shelf,
 * doc 17's ATAPI drive).
 *
 * ONE header for every side: the QEMU ATAPI handler (hw/ide/atapi.c,
 * patch 52), the DOS program (NASM — it includes the numbers by hand,
 * so keep the constants here the single source of truth), and the
 * Win98/XP program (32-bit mingw, C). Bump CDSHELF_PROTO_VERSION on any
 * change; the guest checks it and refuses a mismatch rather than
 * misreading a reply.
 *
 * WHY THE CD-ROM DRIVE IS THE CHANNEL. The guest program has to run on
 * DOS, Win98 and XP, which rules out most transports: XP blocks ring-3
 * port I/O, DOS has no networking to speak of, and the d3dpt device
 * (doc 14) is XP-only. What all three *do* have is a way to send a raw
 * command to their own optical drive — direct ATAPI PIO on DOS (the way
 * tools/atapi-guest-test.py already drives it), ASPI on Win98, SPTI
 * (IOCTL_SCSI_PASS_THROUGH_DIRECT) on XP — and we own that drive's
 * firmware, because it is our ATAPI model (patch 51). So the shelf is a
 * vendor-specific command on the drive itself: ask the drive what discs
 * are on the shelf, tell the drive to load one. No new device, no
 * driver, nothing to install in the guest.
 *
 * The opcode is in MMC's vendor-specific range (0xC0-0xFF), so it
 * cannot collide with a standard command an OS might send, and a drive
 * without a shelf (a plain -cdrom, or real hardware) answers
 * ILLEGAL REQUEST exactly as it should.
 *
 * All values little-endian, all structs fixed-size with explicit
 * padding: a 16-bit DOS program, a 32-bit guest and a 64-bit host must
 * agree byte for byte, and a fixed stride is what makes the DOS build
 * able to walk the reply with an index register rather than a parser.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CDSHELF_PROTO_H
#define CDSHELF_PROTO_H

#include <stdint.h>

#define CDSHELF_PROTO_VERSION 1

/* Vendor-specific ATAPI opcode carrying every shelf request. */
#define CDSHELF_OPCODE 0xD0

/* CDB[1], the subcommand. */
enum {
    /* Data-in: CdshelfListHeader followed by `count` CdshelfEntry. */
    CDSHELF_SUB_LIST  = 0x00,
    /* No data: load the disc in slot CDB[2..3] into the tray. */
    CDSHELF_SUB_LOAD  = 0x01,
    /* No data: empty the tray. */
    CDSHELF_SUB_EJECT = 0x02,
};

/*
 * The 12-byte CDB. Laid out so the fields a DOS program computes sit at
 * fixed offsets:
 *
 *   [0]     CDSHELF_OPCODE
 *   [1]     subcommand (CDSHELF_SUB_*)
 *   [2..3]  slot index, big-endian (LOAD only; 0 otherwise)
 *   [4..6]  reserved, zero
 *   [7..8]  allocation length, big-endian (LIST only; 0 otherwise)
 *   [9..11] reserved, zero
 *
 * Big-endian lengths and indices because that is what every other ATAPI
 * command uses, and the guest code that builds these CDBs is already
 * byte-swapping for READ CD and friends.
 */
#define CDSHELF_CDB_LEN 12

#define CDSHELF_LABEL_MAX 64

/* CdshelfEntry.flags */
enum {
    /* This is the disc currently in the tray. */
    CDSHELF_FLAG_LOADED = 0x01,
    /* The host could not open the file (moved, unplugged drive, typo in
     * a hand-edited shelf). Listed anyway — a shelf that silently drops
     * entries is worse than one that shows a disc as unavailable — but
     * LOAD on it will fail. */
    CDSHELF_FLAG_MISSING = 0x02,
};

typedef struct CdshelfListHeader {
    uint16_t version;       /* CDSHELF_PROTO_VERSION */
    uint16_t entry_size;    /* sizeof(CdshelfEntry), so a guest built
                             * against an older header can still stride
                             * correctly over a newer reply */
    uint16_t count;         /* entries that follow */
    uint16_t total;         /* entries on the shelf, which may exceed
                             * `count` when the allocation length cut the
                             * reply short */
    uint16_t loaded;        /* slot in the tray, or CDSHELF_NO_SLOT */
    uint16_t reserved;
} CdshelfListHeader;

#define CDSHELF_NO_SLOT 0xFFFF

typedef struct CdshelfEntry {
    uint16_t slot;                       /* index for CDSHELF_SUB_LOAD */
    uint8_t  flags;                      /* CDSHELF_FLAG_* */
    uint8_t  label_len;                  /* bytes used in `label` */
    char     label[CDSHELF_LABEL_MAX];   /* NOT null-terminated when full */
} CdshelfEntry;

#define CDSHELF_LIST_HEADER_SIZE 12
#define CDSHELF_ENTRY_SIZE       68

/*
 * The shelf file the QEMU device reads (`-device ide-cd,shelf=<path>`).
 * Deliberately not the launcher's own `discs.toml`: QEMU's side of this
 * is C, and a tab-separated line file is a parser you can read in one
 * sitting and can't be tricked by. The launcher writes it next to the
 * machine's monitor socket and rewrites it whenever the shelf changes,
 * so a disc added while the guest is running shows up in the next LIST.
 *
 *   <label>\t<path>\n
 *
 * A label may not contain a tab or a newline (the launcher replaces
 * them); a path may not contain a newline, which no image path
 * realistically does and the launcher rejects.
 */
#define CDSHELF_FILE_MAX_ENTRIES 256

#endif /* CDSHELF_PROTO_H */
