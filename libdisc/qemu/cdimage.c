/*
 * cdimage: a QEMU format block driver over libdisc (doc 17 §5.1).
 *
 * The block layer sees the cooked 2048-byte view of the disc (L-EC
 * verified user data of the data sectors), so qemu-img, SeaBIOS' El Torito
 * boot, blockdev-change-medium and eject keep working; cdimage_disc()
 * hands the raw model to hw/ide/atapi.c for everything an ATAPI drive
 * answers from the disc itself (raw sectors, TOC, subchannel, CD-DA).
 *
 * Probing: libdisc scores .cue and .ccd files 100 and everything else 0,
 * so a plain .iso stays on the raw driver, byte for byte as before.
 * Overlaid into qemu/block/ by scripts/prepare-qemu.sh from libdisc/qemu/;
 * built when meson found liblibdisc.a (-Dlibdisc_dir, patch 50).
 *
 * Copyright (c) 2026 the win98-xp-virt authors. GPL-2.0-only, like QEMU.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "block/cdimage.h"
#include "block/libdisc.h"

#define CDIMAGE_SECTOR 2048

typedef struct BDRVCdimageState {
    libdisc *disc;
    uint32_t sectors;       /* lead-out LBA of the last session */
} BDRVCdimageState;

static BlockDriver bdrv_cdimage;

static int cdimage_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    return libdisc_probe(buf, buf_size, filename);
}

static int cdimage_open(BlockDriverState *bs, QDict *options, int flags,
                        Error **errp)
{
    BDRVCdimageState *s = bs->opaque;
    char err[512];
    const char *path;
    int ret;

    GLOBAL_STATE_CODE();

    /* read-only medium: refuse a read-write open unless auto-read-only */
    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, "cdimage discs are read-only", errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    /* the .cue / .ccd itself becomes bs->file: tiny, read-only, harmless */
    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    if (libdisc_api_version() != LIBDISC_API_VERSION) {
        error_setg(errp, "libdisc API version %u, cdimage was built for %u",
                   libdisc_api_version(), LIBDISC_API_VERSION);
        return -EINVAL;
    }

    path = bs->file->bs->exact_filename;
    if (!path[0]) {
        path = bs->file->bs->filename;
    }
    if (!path[0]) {
        path = bs->filename;
    }
    err[0] = 0;
    s->disc = libdisc_open(path, err, sizeof(err));
    if (!s->disc) {
        error_setg(errp, "cdimage: %s", err[0] ? err : "cannot open the image");
        return -EINVAL;
    }
    s->sectors = libdisc_sector_count(s->disc);
    if (s->sectors == 0) {
        libdisc_close(s->disc);
        s->disc = NULL;
        error_setg(errp, "cdimage: %s: empty disc", path);
        return -EINVAL;
    }
    bs->total_sectors = (int64_t)s->sectors * CDIMAGE_SECTOR / BDRV_SECTOR_SIZE;
    return 0;
}

static void cdimage_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = CDIMAGE_SECTOR; /* whole sectors only */
}

static int coroutine_fn GRAPH_RDLOCK
cdimage_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                  QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVCdimageState *s = bs->opaque;
    uint8_t buf[CDIMAGE_SECTOR];
    int64_t done = 0;

    assert(QEMU_IS_ALIGNED(offset, CDIMAGE_SECTOR));
    assert(QEMU_IS_ALIGNED(bytes, CDIMAGE_SECTOR));

    while (done < bytes) {
        uint64_t lba = (offset + done) / CDIMAGE_SECTOR;
        int rc;

        if (lba >= s->sectors) {
            return -EIO;
        }
        /*
         * A data sector whose EDC/ECC does not verify, or an audio / gap
         * sector, is an I/O error here exactly as a drive answers a
         * READ(10) of it; the ATAPI path reports the precise sense itself.
         */
        rc = libdisc_read_cooked(s->disc, (uint32_t)lba, buf);
        if (rc != LIBDISC_OK) {
            return -EIO;
        }
        qemu_iovec_from_buf(qiov, done, buf, CDIMAGE_SECTOR);
        done += CDIMAGE_SECTOR;
    }
    return 0;
}

static void cdimage_close(BlockDriverState *bs)
{
    BDRVCdimageState *s = bs->opaque;

    libdisc_close(s->disc);
    s->disc = NULL;
}

libdisc *cdimage_disc(BlockDriverState *bs)
{
    GRAPH_RDLOCK_GUARD_MAINLOOP();

    if (!bs) {
        return NULL;
    }
    bs = bdrv_skip_filters(bs);
    if (!bs || bs->drv != &bdrv_cdimage) {
        return NULL;
    }
    return ((BDRVCdimageState *)bs->opaque)->disc;
}

static BlockDriver bdrv_cdimage = {
    .format_name         = "cdimage",
    .instance_size       = sizeof(BDRVCdimageState),
    .bdrv_probe          = cdimage_probe,
    .bdrv_open           = cdimage_open,
    .bdrv_child_perm     = bdrv_default_perms,
    .bdrv_refresh_limits = cdimage_refresh_limits,
    .bdrv_co_preadv      = cdimage_co_preadv,
    .bdrv_close          = cdimage_close,
    .is_format           = true,
};

static void bdrv_cdimage_init(void)
{
    bdrv_register(&bdrv_cdimage);
}

block_init(bdrv_cdimage_init);
