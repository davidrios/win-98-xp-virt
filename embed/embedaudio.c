/*
 * embedaudio.c — QEMU audio backend that clips the mixed guest output
 * straight into a caller-owned SPSC ring buffer (libqemu_embed.h:
 * qemu_embed_set_audio_ring). Launch with:
 *   -audiodev embed,id=snd0,out.frequency=48000,out.channels=2,out.format=s16
 *                   [,out.buffer-length=60000]
 * fixed_settings (default) yields exactly one HWVoiceOut in that format.
 *
 * Producer (QEMU main loop, BQL held, one mixer tick every timer-period =
 * 10 ms) advances wr; consumer (host audio thread, the DAC's clock) advances
 * rd. Indices are byte positions in [0, size) with one slot kept free; size
 * must be a power of two.
 *
 * Pacing. The producer keeps a cushion of out.buffer-length (default 60 ms)
 * ahead of the consumer and tops it up every tick, so the main loop may be
 * late by up to the cushion before the host hears a gap (under TCG the vCPU
 * thread holds the BQL for MMIO, code generation and the Direct3D executor;
 * a 10 ms cushion was not enough). The guest's audio clock is wall time
 * (RateCtl): each tick the guest is drained of exactly what wall time says
 * has played. After a stall longer than the cushion, what the guest produced
 * meanwhile beyond the cushion is discarded instead of queued, so a stall
 * costs one gap of its length and never adds latency (queued, it would sit
 * in the guest's DMA buffers for the rest of the session: the "audio gets
 * laggier the longer it runs" symptom). A sagging cushion (host DAC faster
 * than the wall clock) is refilled by letting the guest run ahead by the
 * deficit; a cushion beyond target + slack (host DAC slower, or a consumer
 * that started late) is trimmed by dropping one tick. Latency therefore
 * stays within [target, target + slack] for the whole session.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "audio/audio.h"
#include "libqemu_embed.h"

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

/* cushion above target that is tolerated before a tick is dropped */
#define SLACK_US        20000
/* a stall longer than this resyncs the guest clock instead of dropping */
#define MAX_CATCHUP_US  2000000

typedef struct EmbedVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
    size_t target;        /* cushion, bytes */
    size_t slack;         /* bytes */
    size_t pending;       /* bytes handed out by get_buffer_out, not yet put */
    bool pending_drop;    /* ... into the scratch buffer, not the ring */
    /* statistics, stderr every 5 s when something was off */
    bool empty;           /* the ring was empty at the last tick */
    unsigned gaps;        /* ticks that found the ring empty (host heard silence) */
    size_t dropped;       /* bytes discarded */
    size_t min_fill;      /* bytes, since the last report */
    int64_t last_report;  /* g_get_monotonic_time */
    bool started;         /* something has been written since enable */
} EmbedVoiceOut;

static uint8_t scratch[16384];

static inline size_t ring_free(void)
{
    uint32_t wr = qatomic_read(ring_wr);
    uint32_t rd = qatomic_load_acquire(ring_rd);
    return (rd - wr - 1) & (ring_size - 1);
}

static inline size_t ring_fill(void)
{
    return ring_size - 1 - ring_free();
}

static inline size_t usecs_to_bytes(HWVoiceOut *hw, int64_t us)
{
    size_t b = muldiv64(us, hw->info.bytes_per_second, 1000000);
    return b - b % hw->info.bytes_per_frame;
}

static inline size_t bytes_to_ms(HWVoiceOut *hw, size_t b)
{
    return hw->info.bytes_per_second ? b * 1000 / hw->info.bytes_per_second : 0;
}

/* Bytes wall time says the guest should have played since the rate control
 * started, minus what was already taken. Unlike audio_rate_peek_bytes this
 * never resets on a long stall: up to MAX_CATCHUP the backlog is drained
 * (and dropped by the caller), beyond it the clock resyncs. */
static size_t wall_due(EmbedVoiceOut *vo, HWVoiceOut *hw)
{
    int64_t ticks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - vo->rate.start_ticks;
    int64_t bytes = muldiv64(ticks, hw->info.bytes_per_second, NANOSECONDS_PER_SECOND)
                    - vo->rate.bytes_sent;
    int64_t cap = usecs_to_bytes(hw, MAX_CATCHUP_US);
    if (bytes < 0) {
        bytes = 0;
    } else if (bytes > cap) {
        vo->rate.bytes_sent += bytes - cap;   /* resync: forget the excess */
        bytes = cap;
    }
    return bytes - bytes % hw->info.bytes_per_frame;
}

static void report(EmbedVoiceOut *vo, HWVoiceOut *hw, bool force)
{
    int64_t now = g_get_monotonic_time();
    if (!force && now - vo->last_report < 5 * 1000000) {
        return;
    }
    if (vo->gaps || vo->dropped) {
        fprintf(stderr, "qemu-embed: audio: %u gaps, %zu ms dropped, cushion min %zu ms "
                "(target %zu ms): the main loop was late\n",
                vo->gaps, bytes_to_ms(hw, vo->dropped), bytes_to_ms(hw, vo->min_fill),
                bytes_to_ms(hw, vo->target));
    }
    vo->last_report = now;
    vo->gaps = 0;
    vo->dropped = 0;
    vo->min_fill = SIZE_MAX;
}

/* Bytes to take from the guest now: what wall time owes, or the cushion
 * deficit when that is larger (the guest runs ahead to refill it). */
static size_t take_now(EmbedVoiceOut *vo, HWVoiceOut *hw)
{
    size_t due = wall_due(vo, hw);
    size_t fill = ring_fill();
    size_t want = fill < vo->target ? vo->target - fill : 0;

    if (vo->started) {
        if (fill < vo->min_fill) {
            vo->min_fill = fill;
        }
        if (fill == 0 && !vo->empty) {
            vo->gaps++;
        }
        vo->empty = fill == 0;
    }
    if (want > due + vo->slack) {
        /* pre-charge the rate control: the deficit is due now */
        size_t extra = want - due;
        vo->rate.start_ticks -= muldiv64(extra, NANOSECONDS_PER_SECOND,
                                         hw->info.bytes_per_second);
        due = wall_due(vo, hw);
    }
    report(vo, hw, false);
    return due;
}

static size_t embed_buffer_get_free(HWVoiceOut *hw)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    size_t max = hw->samples * hw->info.bytes_per_frame;
    if (!ring_base) {
        return max;
    }
    return MIN(take_now(vo, hw), max);
}

static void *embed_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    size_t bpf = hw->info.bytes_per_frame;

    if (!ring_base) {
        /* no consumer attached: behave like the 'none' backend */
        *size = MIN(*size, sizeof(scratch));
        *size = MIN(*size, audio_rate_peek_bytes(&vo->rate, &hw->info));
        *size -= *size % bpf;
        vo->pending = *size;
        vo->pending_drop = true;
        return scratch;
    }

    size_t due = take_now(vo, hw);
    size_t fill = ring_fill();
    size_t room = fill < vo->target ? vo->target - fill : 0;
    size_t n = MIN(*size, due);
    n -= n % bpf;
    if (!n) {
        *size = 0;
        return NULL;
    }

    bool drop;
    if (room) {
        drop = false;
        n = MIN(n, room);
    } else {
        /* at target: a backlog beyond the slack is a stall's worth of audio
         * the host can no longer play on time; a cushion beyond target +
         * slack means the consumer is slow. Both are trimmed. A tick's
         * worth within the slack goes into the ring (steady state). */
        drop = due > vo->slack || fill > vo->target + vo->slack;
    }
    if (!drop) {
        uint32_t wr = qatomic_read(ring_wr);
        size_t contiguous = MIN(ring_free(), ring_size - wr);
        size_t m = MIN(n, contiguous);
        m -= m % bpf;
        if (m) {
            vo->pending = m;
            vo->pending_drop = false;
            vo->started = true;
            *size = m;
            return ring_base + wr;
        }
        drop = true;   /* ring physically full */
    }
    n = MIN(n, sizeof(scratch));
    n -= n % bpf;
    vo->pending = n;
    vo->pending_drop = true;
    vo->dropped += n;
    *size = n;
    return scratch;
}

static size_t embed_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    assert(size <= vo->pending);
    audio_rate_add_bytes(&vo->rate, size);
    if (!vo->pending_drop) {
        uint32_t wr = qatomic_read(ring_wr);
        assert(buf == ring_base + wr);
        qatomic_store_release(ring_wr, (uint32_t)((wr + size) & (ring_size - 1)));
    }
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
    Audiodev *dev = drv_opaque;

    audio_pcm_init_info(&hw->info, as);
    size_t bpf = hw->info.bytes_per_frame;
    /* out.buffer-length is the cushion; the mixer must be able to refill
     * all of it in one tick, so mix_buf holds that many frames */
    size_t frames = audio_buffer_frames(dev->u.embed.out, as, 60000);
    vo->target = frames * bpf;
    if (ring_base && vo->target > ring_size / 2) {
        vo->target = ring_size / 2;
        vo->target -= vo->target % bpf;
        frames = vo->target / bpf;
    }
    vo->slack = MAX(usecs_to_bytes(hw, SLACK_US), bpf);
    hw->samples = frames;
    vo->min_fill = SIZE_MAX;
    vo->last_report = g_get_monotonic_time();
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
        vo->started = false;
        vo->empty = false;
    } else {
        /* a short stream (a system sound) never reaches the 5 s report */
        report(vo, hw, true);
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
