/*
 * d3dpt_vga.c — the d3dpt-vga display adapter (doc 15, ADR-008 / M7a).
 *
 * A PCI VGA (QEMU's standard VGA core: SeaBIOS' stdvga ROM boots it, XP's
 * inbox vga.sys drives it at 640x480x16 until our driver is installed) with
 * one extra MMIO BAR of paravirtual registers (d3dpt/d3dpt_fb.h). Our XP
 * video miniport reads the host's mode table from it, programs a linear
 * mode and sets ENABLE; from then on the console shows VRAM directly:
 * the DisplaySurface is created over the guest's framebuffer bytes (no
 * copy inside QEMU), dirty lines come from the memory dirty log, and the
 * embed library's listener hands the same pointer to the player. 16 bpp
 * and 8 bpp (palettized, the PALETTE register block) modes are converted
 * into an x8r8g8b8 shadow per dirty span because the embed listener takes
 * one format; a palette write repaints the whole frame at the next
 * refresh. ENABLE = 0 returns the console to the VGA core (BSOD, reboot,
 * the BIOS).
 *
 * M7c (doc 15): the top 64 MiB of BAR 0 is a command window in the
 * d3dpt_proto.h layout. The display driver's Direct3D DDI appends records
 * there (surfaces it registers by VRAM offset, contexts, DrawPrimitives2
 * token streams) and writes DOORBELL; the batch runs synchronously in
 * the same libd3dpt_exec as the SysBus device (its own instance), which
 * reads texels straight out of VRAM and writes rendered frames back into
 * the render target's VRAM (marked dirty here so the scanout shows them).
 *
 * The device is not a QEMU patch: prepare-qemu.sh overlays d3dpt/hw into
 * qemu/hw/d3dpt and patch 40 adds the meson subdir. Use it as
 *   -vga none -device d3dpt-vga
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"
#include "../display/vga_int.h"

#include "hw/d3dpt/d3dpt_fb.h"
#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

#define TYPE_D3DPT_VGA "d3dpt-vga"
OBJECT_DECLARE_SIMPLE_TYPE(D3dptVgaState, D3DPT_VGA)

typedef struct D3dptLinearMode {
    uint32_t w, h, bpp, pitch, offset;
} D3dptLinearMode;

struct D3dptVgaState {
    PCIDevice dev;
    VGACommonState vga;
    MemoryRegion regs;

    /* guest-programmed registers */
    uint32_t r_enable, r_w, r_h, r_bpp, r_pitch, r_offset, r_hz, r_sel;
    int64_t vbl_ns;             /* clock at the last ENABLE: the vblank counter's origin */
    uint32_t flips;
    uint32_t flips_last;        /* flips at the last rate report */
    int64_t flips_ns;           /* and when it was made */
    uint32_t ddflags;           /* property: test knob read by the guest driver */

    /* the linear mode currently shown (lin_on) */
    bool lin_on;
    bool full_update;
    unsigned vga_grace;         /* refreshes to hold the last frame after ENABLE 1->0 */
    D3dptLinearMode lin;
    pixman_image_t *shadow;     /* x8r8g8b8 copy for 16 and 8 bpp modes */
    pixman_image_t *src;        /* r5g6b5 / c8 view of VRAM for the conversion */
    uint32_t pal[D3DPT_FB_PALETTE_SIZE]; /* the PALETTE registers */
    bool pal_dirty;             /* written since the last refresh */
    pixman_indexed_t *indexed;  /* pal as pixman's palette for the c8 view */

    char dbg[256];
    unsigned dbg_len;

    /* M7c: the command window at the top of VRAM and its executor */
    uint32_t cmd_offset;        /* 0 = VRAM too small for a window */
    const D3dptExecLib *lib;
    d3dpt_exec_t *exec;
    bool exec_tried;
    uint32_t d3d_err;           /* last D3DPT_ERR_* of the doorbell */
    uint32_t batches;
};

/* ------------------------------------------------------------ mode table
 * Static for M7a; the M2 mode table (pixel aspect, per-preset lists) will
 * feed this from the player. Every size is offered at each refresh and
 * depth: XP's mode picker and fullscreen games choose from it, the refresh
 * is what the CRT preset later honours, no timing is emulated. */
static const struct { uint16_t w, h; } fb_sizes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 },
    { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
};
static const uint8_t fb_hz[] = { 60, 75, 85 };
static const uint8_t fb_bpp[] = { 8, 16, 32 };
#define FB_MODE_COUNT (ARRAY_SIZE(fb_sizes) * ARRAY_SIZE(fb_hz) * ARRAY_SIZE(fb_bpp))

/* refreshes (the player's pull interval, 16 ms by default) the last linear
 * frame stays up after ENABLE goes 0 before the VGA core is shown */
#define D3DPT_FB_VGA_GRACE_REFRESHES 15

static bool fb_mode_entry(uint32_t sel, uint32_t *w, uint32_t *h,
                          uint32_t *bpp, uint32_t *hz)
{
    if (sel >= FB_MODE_COUNT) {
        return false;
    }
    *bpp = fb_bpp[sel % ARRAY_SIZE(fb_bpp)];
    sel /= ARRAY_SIZE(fb_bpp);
    *hz = fb_hz[sel % ARRAY_SIZE(fb_hz)];
    sel /= ARRAY_SIZE(fb_hz);
    *w = fb_sizes[sel].w;
    *h = fb_sizes[sel].h;
    return true;
}

/* --------------------------------------------------------------- display */

static bool fb_get_mode(D3dptVgaState *s, D3dptLinearMode *m)
{
    uint32_t bytepp;

    if (!s->r_enable) {
        return false;
    }
    if (s->r_bpp != 8 && s->r_bpp != 16 && s->r_bpp != 32) {
        return false;
    }
    bytepp = s->r_bpp / 8;
    m->w = s->r_w;
    m->h = s->r_h;
    m->bpp = s->r_bpp;
    m->pitch = s->r_pitch ? s->r_pitch : m->w * bytepp;
    m->offset = s->r_offset;
    if (m->w < 64 || m->h < 64 || m->w > 8192 || m->h > 8192) {
        return false;
    }
    if (m->pitch < m->w * bytepp || m->pitch & 3) {
        return false;
    }
    if ((uint64_t)m->offset + (uint64_t)m->pitch * m->h > s->vga.vram_size) {
        return false;
    }
    return true;
}

static void fb_drop_shadow(D3dptVgaState *s)
{
    if (s->shadow) {
        qemu_pixman_image_unref(s->shadow);
        s->shadow = NULL;
    }
    if (s->src) {
        qemu_pixman_image_unref(s->src);
        s->src = NULL;
    }
}

static void fb_apply_palette(D3dptVgaState *s)
{
    int i;

    for (i = 0; i < D3DPT_FB_PALETTE_SIZE; i++) {
        s->indexed->rgba[i] = 0xff000000u | (s->pal[i] & 0xffffffu);
    }
    s->pal_dirty = false;
}

static void fb_switch(D3dptVgaState *s, const D3dptLinearMode *m)
{
    uint8_t *ptr = memory_region_get_ram_ptr(&s->vga.vram) + m->offset;
    DisplaySurface *ds;

    fb_drop_shadow(s);
    if (m->bpp == 32) {
        ds = qemu_create_displaysurface_from(m->w, m->h, PIXMAN_x8r8g8b8,
                                             m->pitch, ptr);
    } else {
        if (m->bpp == 16) {
            s->src = pixman_image_create_bits(PIXMAN_r5g6b5, m->w, m->h,
                                              (uint32_t *)ptr, m->pitch);
        } else {
            /* indices through the palette: pixman's c8 fetcher looks each
             * byte up in the indexed table the image points at */
            s->src = pixman_image_create_bits(PIXMAN_c8, m->w, m->h,
                                              (uint32_t *)ptr, m->pitch);
            fb_apply_palette(s);
            pixman_image_set_indexed(s->src, s->indexed);
        }
        s->shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8, m->w, m->h,
                                             NULL, 0);
        ds = qemu_create_displaysurface_pixman(s->shadow);
    }
    dpy_gfx_replace_surface(s->vga.con, ds);
    s->lin = *m;
    s->lin_on = true;
    s->full_update = true;
}

static void fb_update_span(D3dptVgaState *s, int y0, int y1)
{
    if (s->shadow) {
        pixman_image_composite(PIXMAN_OP_SRC, s->src, NULL, s->shadow,
                               0, y0, 0, 0, 0, y0, s->lin.w, y1 - y0);
    }
    dpy_gfx_update(s->vga.con, 0, y0, s->lin.w, y1 - y0);
}

/* The vertical blank the guest waits on (REG_FRAMES).
 *
 * It counts periods of the mode's refresh rate since the mode was enabled,
 * off the host clock — not the display client's pull. The guest's frame
 * pacing then does not depend on whether anything is looking: the player
 * pulls at its own interval, a headless run pulls not at all, and a game
 * still gets the 60 (or 85) Hz it asked for.
 */
static uint32_t fb_vblank_count(D3dptVgaState *s)
{
    uint32_t hz = s->r_hz;

    if (!s->vbl_ns) {
        return 0;
    }
    if (hz < 30 || hz > 200) {
        hz = 60;   /* the guest left HZ unset or nonsensical */
    }
    return (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->vbl_ns) *
                      hz / NANOSECONDS_PER_SECOND);
}

/* The guest's real frame rate. A game of the era paces itself by its flip
 * chain, so page flips per second is the number that says whether it runs
 * at the display's rate or as fast as the CPU allows; a game that blits to
 * the primary instead prints nothing here, which is the answer too. It is
 * driven by the flips themselves, not by the refresh, so a headless run
 * (xp-game-test.sh) reports the same as the player. */
static void fb_flip_rate(D3dptVgaState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    double secs;

    if (!s->flips_ns) {
        s->flips_ns = now;
        s->flips_last = s->flips;
        return;
    }
    if (now - s->flips_ns < 5 * NANOSECONDS_PER_SECOND) {
        return;
    }
    secs = (double)(now - s->flips_ns) / NANOSECONDS_PER_SECOND;
    if (s->flips != s->flips_last) {
        info_report("d3dpt-vga: %u page flips in %.1f s (%.1f/s)",
                    s->flips - s->flips_last, secs,
                    (s->flips - s->flips_last) / secs);
    }
    s->flips_ns = now;
    s->flips_last = s->flips;
}

static void d3dpt_vga_gfx_update(void *opaque)
{
    D3dptVgaState *s = opaque;
    D3dptLinearMode m;
    DirtyBitmapSnapshot *snap;
    int y, ys;

    if (!fb_get_mode(s, &m)) {
        if (s->lin_on) {
            /* A mode switch is RESET (ENABLE = 0) followed by the new mode a
             * few ms later; showing the VGA core in between flashes text
             * mode or stale VGA memory in the player. Hold the last frame
             * for a moment: a real return to VGA (BSOD, full-screen console,
             * reboot) is only delayed by that. */
            if (s->vga_grace) {
                s->vga_grace--;
                return;
            }
            /* back to the VGA core: it recreates its own surface */
            s->lin_on = false;
            fb_drop_shadow(s);
            s->vga.hw_ops->invalidate(&s->vga);
        }
        s->vga.hw_ops->gfx_update(&s->vga);
        return;
    }

    if (!s->lin_on || memcmp(&s->lin, &m, sizeof(m)) != 0) {
        fb_switch(s, &m);
    }
    if (s->pal_dirty && m.bpp == 8) {
        /* a new palette recolours every pixel: one full conversion */
        fb_apply_palette(s);
        s->full_update = true;
    }

    if (s->full_update) {
        s->full_update = false;
        /* consume the dirty log so the next refresh starts clean */
        snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                      (uint64_t)m.pitch * m.h,
                                                      DIRTY_MEMORY_VGA);
        g_free(snap);
        fb_update_span(s, 0, m.h);
        return;
    }

    snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                  (uint64_t)m.pitch * m.h,
                                                  DIRTY_MEMORY_VGA);
    ys = -1;
    for (y = 0; y < (int)m.h; y++) {
        bool dirty = memory_region_snapshot_get_dirty(&s->vga.vram, snap,
                                                      m.offset + (uint64_t)m.pitch * y,
                                                      m.pitch);
        if (dirty && ys < 0) {
            ys = y;
        }
        if (!dirty && ys >= 0) {
            fb_update_span(s, ys, y);
            ys = -1;
        }
    }
    if (ys >= 0) {
        fb_update_span(s, ys, y);
    }
    g_free(snap);
}

static void d3dpt_vga_invalidate(void *opaque)
{
    D3dptVgaState *s = opaque;

    s->full_update = true;
    s->vga.hw_ops->invalidate(&s->vga);
}

static void d3dpt_vga_text_update(void *opaque, console_ch_t *chardata)
{
    D3dptVgaState *s = opaque;

    if (!s->r_enable) {
        s->vga.hw_ops->text_update(&s->vga, chardata);
    }
}

static const GraphicHwOps d3dpt_vga_gfx_ops = {
    .invalidate  = d3dpt_vga_invalidate,
    .gfx_update  = d3dpt_vga_gfx_update,
    .text_update = d3dpt_vga_text_update,
};

/* ------------------------------------------------------------- Direct3D */

static void d3d_log(void *ud, const char *msg)
{
    info_report("d3dpt-vga: %s", msg);
}

static void d3d_active(void *ud, int on)
{
    /* frames land in VRAM: nothing to switch in the presenter */
}

static void d3d_vram_dirty(void *ud, uint32_t offset, uint32_t bytes)
{
    D3dptVgaState *s = ud;

    if ((uint64_t)offset + bytes <= s->cmd_offset) {
        memory_region_set_dirty(&s->vga.vram, offset, bytes);
    }
}

static bool d3d_load(D3dptVgaState *s)
{
    d3dpt_exec_ops ops = { s, d3d_log, d3d_active, NULL, d3d_vram_dirty };

    if (s->exec_tried || !s->cmd_offset) {
        return s->exec != NULL;
    }
    s->exec_tried = true;
    s->lib = d3dpt_exec_lib();
    if (!s->lib) {
        return false;
    }
    s->exec = s->lib->create(&ops);
    if (!s->exec) {
        warn_report("d3dpt-vga: executor refused to start (no DXVK / Vulkan device)");
        return false;
    }
    s->lib->set_vram(s->exec, memory_region_get_ram_ptr(&s->vga.vram), s->cmd_offset);
    s->lib->attach(s->exec, 1);
    info_report("d3dpt-vga: Direct3D executor ready, window at %u MiB", s->cmd_offset >> 20);
    return true;
}

static void d3d_doorbell(D3dptVgaState *s)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;

    if (!s->cmd_offset) {
        return;
    }
    s->batches++;
    if (d3d_load(s)) {
        s->d3d_err = s->lib->submit(s->exec, win, D3DPT_SHM_SIZE);
    } else {
        s->d3d_err = D3DPT_ERR_HOST;
        h->ret_status = D3DPT_ERR_HOST;
        h->cmd_bytes = 0;
        h->cmd_count = 0;
    }
    if (s->d3d_err && s->batches <= 8) {
        info_report("d3dpt-vga: batch %u: error %u at record %u", s->batches, s->d3d_err, h->ret_index);
    }
}

/* guest reset: every context and surface the driver registered is gone */
static void d3d_reset(D3dptVgaState *s)
{
    if (s->exec) {
        s->lib->attach(s->exec, 0);
        s->lib->attach(s->exec, 1);
    }
    if (s->cmd_offset) {
        memset(memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset, 0, sizeof(d3dpt_shm_hdr));
    }
    s->d3d_err = 0;
    s->batches = 0;
}

/* ------------------------------------------------------------- registers */

static uint64_t d3dpt_vga_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    D3dptVgaState *s = opaque;
    uint32_t w, h, bpp, hz;

    switch (addr) {
    case D3DPT_FB_REG_MAGIC:
        return D3DPT_FB_MAGIC;
    case D3DPT_FB_REG_VERSION:
        return D3DPT_FB_VERSION;
    case D3DPT_FB_REG_VRAM_SIZE:
        return s->vga.vram_size;
    case D3DPT_FB_REG_CAPS:
        return D3DPT_FB_CAP_BPP8 | D3DPT_FB_CAP_BPP16 | D3DPT_FB_CAP_BPP32 |
               (s->cmd_offset ? D3DPT_FB_CAP_D3D : 0);
    case D3DPT_FB_REG_MODE_COUNT:
        return FB_MODE_COUNT;
    case D3DPT_FB_REG_MODE_SEL:
        return s->r_sel;
    case D3DPT_FB_REG_MODE_W:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? w : 0;
    case D3DPT_FB_REG_MODE_H:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? h : 0;
    case D3DPT_FB_REG_MODE_BPP:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? bpp : 0;
    case D3DPT_FB_REG_MODE_HZ:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? hz : 0;
    case D3DPT_FB_REG_ENABLE:
        return s->r_enable;
    case D3DPT_FB_REG_WIDTH:
        return s->r_w;
    case D3DPT_FB_REG_HEIGHT:
        return s->r_h;
    case D3DPT_FB_REG_BPP:
        return s->r_bpp;
    case D3DPT_FB_REG_PITCH:
        return s->r_pitch;
    case D3DPT_FB_REG_OFFSET:
        return s->r_offset;
    case D3DPT_FB_REG_HZ:
        return s->r_hz;
    case D3DPT_FB_REG_FRAMES:
        return fb_vblank_count(s);
    case D3DPT_FB_REG_DDFLAGS:
        return s->ddflags;
    case D3DPT_FB_REG_CMD_OFFSET:
        return s->cmd_offset;
    case D3DPT_FB_REG_DOORBELL:
        return s->d3d_err;
    case D3DPT_FB_REG_D3D_STATUS:
        /* probing loads the executor: the answer must be honest */
        return d3d_load(s) ? D3DPT_STATUS_READY : D3DPT_STATUS_NO_EXEC;
    default:
        if (addr >= D3DPT_FB_REG_PALETTE &&
            addr < D3DPT_FB_REG_PALETTE + 4 * D3DPT_FB_PALETTE_SIZE) {
            return s->pal[(addr - D3DPT_FB_REG_PALETTE) / 4];
        }
        return 0;
    }
}

static void d3dpt_vga_regs_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    D3dptVgaState *s = opaque;

    switch (addr) {
    case D3DPT_FB_REG_MODE_SEL:
        s->r_sel = val;
        break;
    case D3DPT_FB_REG_ENABLE:
        if ((val != 0) != (s->r_enable != 0)) {
            info_report("d3dpt-vga: linear mode %s (%ux%ux%u pitch %u offset %u, %u Hz)",
                        val ? "on" : "off", s->r_w, s->r_h, s->r_bpp,
                        s->r_pitch, s->r_offset, s->r_hz);
        }
        if (!val && s->r_enable) {
            s->vga_grace = D3DPT_FB_VGA_GRACE_REFRESHES;
        }
        s->r_enable = val != 0;
        s->vbl_ns = s->r_enable ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        graphic_hw_invalidate(s->vga.con);
        break;
    case D3DPT_FB_REG_WIDTH:
        s->r_w = val;
        break;
    case D3DPT_FB_REG_HEIGHT:
        s->r_h = val;
        break;
    case D3DPT_FB_REG_BPP:
        s->r_bpp = val;
        break;
    case D3DPT_FB_REG_PITCH:
        s->r_pitch = val;
        break;
    case D3DPT_FB_REG_OFFSET:
        if (val != s->r_offset && s->flips < 4) {
            /* DirectDraw page flips: the first few, so the log shows them */
            info_report("d3dpt-vga: scanout offset %u -> %" PRIu64, s->r_offset, val);
        }
        if (val != s->r_offset) {
            s->flips++;
            fb_flip_rate(s);
        }
        s->r_offset = val;
        break;
    case D3DPT_FB_REG_HZ:
        s->r_hz = val;
        break;
    case D3DPT_FB_REG_DEBUG: {
        char c = (char)val;
        if (c == '\n' || s->dbg_len == sizeof(s->dbg) - 1) {
            s->dbg[s->dbg_len] = 0;
            info_report("d3dpt-vga: guest: %s", s->dbg);
            s->dbg_len = 0;
            if (c == '\n') {
                break;
            }
        }
        if (c != '\r') {
            s->dbg[s->dbg_len++] = c;
        }
        break;
    }
    case D3DPT_FB_REG_DOORBELL:
        d3d_doorbell(s);
        break;
    default:
        if (addr >= D3DPT_FB_REG_PALETTE &&
            addr < D3DPT_FB_REG_PALETTE + 4 * D3DPT_FB_PALETTE_SIZE) {
            s->pal[(addr - D3DPT_FB_REG_PALETTE) / 4] = val & 0xffffffu;
            s->pal_dirty = true;
        }
        break;
    }
}

static const MemoryRegionOps d3dpt_vga_regs_ops = {
    .read = d3dpt_vga_regs_read,
    .write = d3dpt_vga_regs_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---------------------------------------------------------------- device */

static void d3dpt_vga_realize(PCIDevice *dev, Error **errp)
{
    D3dptVgaState *s = D3DPT_VGA(dev);
    VGACommonState *vga = &s->vga;

    if (!vga_common_init(vga, OBJECT(dev), errp)) {
        return;
    }
    vga_init(vga, OBJECT(dev), pci_address_space(dev),
             pci_address_space_io(dev), true);
    /* one console; the VGA core's ops run through ours while ENABLE is 0 */
    vga->con = graphic_console_init(DEVICE(dev), 0, &d3dpt_vga_gfx_ops, s);

    memory_region_init_io(&s->regs, OBJECT(dev), &d3dpt_vga_regs_ops, s,
                          "d3dpt-vga.regs", D3DPT_FB_REGS_SIZE);
    s->indexed = g_new0(pixman_indexed_t, 1);
    s->indexed->color = true;

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);

    /* the command window takes the top 64 MiB when at least as much is
     * left below it for the frame buffer and the DirectDraw heap */
    if (vga->vram_size >= 2 * D3DPT_SHM_SIZE) {
        s->cmd_offset = vga->vram_size - D3DPT_SHM_SIZE;
    } else {
        warn_report("d3dpt-vga: vgamem_mb=%u leaves no room for the Direct3D window (needs 128)",
                    vga->vram_size_mb);
    }
}

static void d3dpt_vga_reset(DeviceState *dev)
{
    D3dptVgaState *s = D3DPT_VGA(PCI_DEVICE(dev));

    /* the VGA core registers its own reset; ours goes back to VGA text */
    s->r_enable = s->r_w = s->r_h = s->r_bpp = s->r_pitch = 0;
    s->r_offset = s->r_hz = s->r_sel = 0;
    s->vbl_ns = 0;
    s->flips = s->flips_last = 0;
    s->flips_ns = 0;
    s->vga_grace = 0;
    s->dbg_len = 0;
    d3d_reset(s);
}

static Property d3dpt_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", D3dptVgaState, vga.vram_size_mb, D3DPT_FB_VRAM_MB),
    DEFINE_PROP_BOOL("global-vmstate", D3dptVgaState, vga.global_vmstate, false),
    DEFINE_PROP_UINT32("ddflags", D3dptVgaState, ddflags, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void d3dpt_vga_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = d3dpt_vga_realize;
    k->romfile = "vgabios-stdvga.bin";
    k->vendor_id = D3DPT_FB_PCI_VENDOR;
    k->device_id = D3DPT_FB_PCI_DEVICE;
    k->subsystem_vendor_id = D3DPT_FB_PCI_VENDOR;
    k->subsystem_id = D3DPT_FB_PCI_DEVICE;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "paravirtual framebuffer VGA (win98-xp-virt, doc 15)";
    dc->hotpluggable = false;
    device_class_set_props(dc, d3dpt_vga_properties);
    device_class_set_legacy_reset(dc, d3dpt_vga_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo d3dpt_vga_info = {
    .name = TYPE_D3DPT_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(D3dptVgaState),
    .class_init = d3dpt_vga_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void d3dpt_vga_register_types(void)
{
    type_register_static(&d3dpt_vga_info);
}

type_init(d3dpt_vga_register_types)
