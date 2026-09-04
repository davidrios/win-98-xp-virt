/*
 * cdimage: the libdisc handle behind a BlockDriverState (doc 17 §5.1).
 *
 * The only thing hw/ide/atapi.c includes besides libdisc.h. Overlaid into
 * qemu/include/block/ by scripts/prepare-qemu.sh from libdisc/qemu/.
 */
#ifndef BLOCK_CDIMAGE_H
#define BLOCK_CDIMAGE_H

#include "block/libdisc.h"

typedef struct BlockDriverState BlockDriverState;

/*
 * The disc model of the medium in `bs` (filters skipped), or NULL when the
 * medium is not a cdimage (a plain ISO on the raw driver, no medium). Call
 * it on every command from under the BQL and never cache the result: a
 * medium change replaces the BlockDriverState underneath.
 */
libdisc *cdimage_disc(BlockDriverState *bs);

#endif
