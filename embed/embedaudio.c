/*
 * embedaudio.c — QEMU audio backend that clips the mixed guest output
 * straight into a caller-owned SPSC ring buffer (libqemu_embed.h:
 * qemu_embed_set_audio_ring). Launch with:
 *   -audiodev embed,id=snd0,out.frequency=48000,out.channels=2,out.format=s16
 * fixed_settings (default) yields exactly one HWVoiceOut in that format.
 *
 * Producer (QEMU main loop / vCPU, BQL held) advances wr; consumer (host
 * audio thread) advances rd. Indices are byte positions in [0, size) with
 * one slot kept free; size must be a power of two.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "audio/audio.h"

#define AUDIO_CAP "embed"
#include "audio/audio_int.h"

static uint8_t *ring_base;
static size_t ring_size;               /* power of two */
static uint32_t *ring_wr;              /* producer index (us) */
static const uint32_t *ring_rd;        /* consumer index (host) */

void qemu_embed_set_audio_ring(void *base, size_t bytes,
                               uint32_t *wr_idx, const uint32_t *rd_idx)
{
    ring_base = base;
    ring_size = bytes;
    ring_wr = wr_idx;
    ring_rd = rd_idx;
}

typedef struct EmbedVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
    size_t pending;   /* bytes handed out by get_buffer_out, not yet put */
} EmbedVoiceOut;

static inline size_t ring_free(void)
{
    uint32_t wr = qatomic_read(ring_wr);
    uint32_t rd = qatomic_load_acquire(ring_rd);
    return (rd - wr - 1) & (ring_size - 1);
}

static size_t embed_buffer_get_free(HWVoiceOut *hw)
{
    if (!ring_base) {
        return hw->samples * hw->info.bytes_per_frame;
    }
    return ring_free();
}

static void *embed_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    if (!ring_base) {
        /* no consumer attached: behave like the 'none' backend */
        static uint8_t scratch[8192];
        *size = MIN(*size, sizeof(scratch));
        *size = audio_rate_get_bytes(&vo->rate, &hw->info, *size);
        return scratch;
    }
    uint32_t wr = qatomic_read(ring_wr);
    size_t contiguous = MIN(ring_free(), ring_size - wr);
    *size = MIN(*size, contiguous);
    /* pace to real time even when the consumer is slow to start */
    *size = audio_rate_get_bytes(&vo->rate, &hw->info, *size);
    *size -= *size % hw->info.bytes_per_frame;
    vo->pending = *size;
    return ring_base + wr;
}

static size_t embed_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    if (!ring_base) {
        return size;
    }
    uint32_t wr = qatomic_read(ring_wr);
    assert(buf == ring_base + wr && size <= vo->pending);
    qatomic_store_release(ring_wr, (uint32_t)((wr + size) & (ring_size - 1)));
    vo->pending = 0;
    return size;
}

static size_t embed_write(HWVoiceOut *hw, void *buf, size_t len)
{
    /* mixeng=off path: copy into the ring in up to two chunks */
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        void *dst = embed_get_buffer_out(hw, &n);
        if (!n) {
            break;
        }
        memcpy(dst, (uint8_t *)buf + done, n);
        embed_put_buffer_out(hw, dst, n);
        done += n;
    }
    return done;
}

static int embed_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    audio_pcm_init_info(&hw->info, as);
    hw->samples = 480;                 /* 10 ms at 48 kHz */
    audio_rate_start(&vo->rate);
    return 0;
}

static void embed_fini_out(HWVoiceOut *hw)
{
}

static void embed_enable_out(HWVoiceOut *hw, bool enable)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    if (enable) {
        audio_rate_start(&vo->rate);
    }
}

static void *embed_audio_init(Audiodev *dev, Error **errp)
{
    return dev;   /* non-NULL = success */
}

static void embed_audio_fini(void *opaque)
{
}

static struct audio_pcm_ops embed_pcm_ops = {
    .init_out        = embed_init_out,
    .fini_out        = embed_fini_out,
    .write           = embed_write,
    .buffer_get_free = embed_buffer_get_free,
    .get_buffer_out  = embed_get_buffer_out,
    .put_buffer_out  = embed_put_buffer_out,
    .run_buffer_out  = audio_generic_run_buffer_out,
    .enable_out      = embed_enable_out,
};

static struct audio_driver embed_audio_driver = {
    .name           = "embed",
    .descr          = "Ring buffer into the embedding application",
    .init           = embed_audio_init,
    .fini           = embed_audio_fini,
    .pcm_ops        = &embed_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof(EmbedVoiceOut),
    .voice_size_in  = 0,
};

static void register_audio_embed(void)
{
    audio_driver_register(&embed_audio_driver);
}
type_init(register_audio_embed);
