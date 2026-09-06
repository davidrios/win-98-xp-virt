/*
 * embedfx.c — qemu-3dfx UI provider for the embed library (patch 30's
 * QemuFxUiOps). No window: the backend's pbuffer is the drawable, so the
 * Mesa entry points just acknowledge, report activation to the frontend
 * and answer size queries.
 *
 * Glide takes the same drawable by the reverse of upstream's handshake
 * (doc 12 §5): instead of handing the wrapper a window to make a context
 * on, we hand it the table in glide_host.h and it renders into ours.
 * hw/3dfx passes that table to the wrapper's setHostOps at load time
 * (patch 33), so a wrapper that does not export the symbol is unaffected.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "embedfx.h"
#include "glide_host.h"
#include "hw/d3dpt/d3dpt.h"

static int fx_active;

/* Mesa and Glide share one "the frontend is showing 3D" edge: while it is
 * up the VGA device is not rendered and frames come from swaps. */
static void fx_set_active(int on)
{
    if (on == fx_active) {
        return;
    }
    fx_active = on;
    graphic_hw_passthrough(qemu_console_lookup_by_index(0), on);
    embed_fx_active(on != 0);
}

static void fx_mesa_prepare_window(int msaa, int alpha, int scale_x, void *cwnd_fn)
{
    void (*cwnd)(void *, void *, void *) = cwnd_fn;
    if (embed_gl_available()) {
        /* non-NULL "native window" = drawable available */
        cwnd(NULL, (void *)(uintptr_t)1, NULL);
    } else {
        static bool warned;
        if (!warned) {
            warned = true;
            error_report("mesapt: no EGL on this host; refusing GL context");
        }
        cwnd(NULL, NULL, NULL);
    }
}

static void fx_mesa_release_window(void)
{
    fx_set_active(0);
}

static void fx_mesa_renderer_stat(const int activate)
{
    fx_set_active(activate);
}

static int fx_mesa_gui_fullscreen(int *sizev)
{
    if (sizev) {
        QemuConsole *con = qemu_console_lookup_by_index(0);
        DisplaySurface *s = con ? qemu_console_surface(con) : NULL;
        sizev[0] = s ? surface_width(s) : 640;
        sizev[1] = s ? surface_height(s) : 480;
        /* drawable == guest surface: the in-QEMU scaler stays inert */
        embed_gl_drawable_size(&sizev[2], &sizev[3]);
    }
    return 0;
}

/* ------------------------------------------------------------- Glide
 *
 * The table the wrapper is given: our context, our present. Everything it
 * points at runs on the vCPU thread with the BQL held, which is where
 * hw/3dfx calls the wrapper from, so it is the same thread that made the
 * context current.
 */
static int fx_glide_begin(int w, int h)
{
    return embed_gl_fx_begin(w, h);
}

static void fx_glide_present(void)
{
    embed_gl_fx_present();
}

static void fx_glide_end(void)
{
    embed_gl_fx_end();
}

static void *fx_glide_get_proc(const char *name)
{
    return embed_gl_fx_proc(name);
}

static const GlideHostOps embed_glide_host_ops = {
    .size     = sizeof(GlideHostOps),
    .version  = GLIDE_HOST_ABI_VERSION,
    .begin    = fx_glide_begin,
    .present  = fx_glide_present,
    .end      = fx_glide_end,
    .get_proc = fx_glide_get_proc,
};

static const void *fx_glide_host_ops(void)
{
    return embed_gl_available() ? &embed_glide_host_ops : NULL;
}

/*
 * The window handshake, upstream's shape kept: prepare_window records what
 * grSstWinOpen asked for, and the guest's poll of MMIO 0xFB8 (stat_window
 * → window_stat) is what actually opens it, exactly as ui/sdl2.c does.
 * There is no window to wait for here, so the open is synchronous.
 */
typedef void (*fx_cwnd_fn)(void *, void *, void *);
static uint32_t glide_res;      /* (h << 16) | w, from glidewnd.c */
static void *glide_opaque;
static fx_cwnd_fn glide_cwnd;
static int glide_open;

static void fx_glide_prepare_window(uint32_t res, int msaa, void *opaque,
                                    void *cwnd_fn)
{
    glide_res = res;
    glide_opaque = opaque;
    glide_cwnd = cwnd_fn;
}

static int fx_glide_window_stat(const int activate)
{
    int w, h;

    /* deactivation is grSstWinClose's business (release_window below): the
     * guest need not poll again for it, and calling cwnd_fn twice would run
     * grSstWinClose twice. 0 = "closed, all well". */
    if (!activate) {
        return 0;
    }
    if (glide_open) {
        return (int)glide_res;
    }
    if (!glide_cwnd) {
        return 1;               /* 1 = no window: grSstWinOpen fails */
    }
    w = glide_res & 0xFFFFU;
    h = (glide_res >> 0x10) & 0x7FFFU;
    if (!embed_gl_fx_begin(w, h)) {
        static bool warned;
        if (!warned) {
            warned = true;
            error_report("glidept: no host GL context; refusing Glide "
                         "pass-through");
        }
        return 1;
    }
    /* runs grSstWinOpen inside the wrapper, on the context just bound. The
     * handle is ignored by a wrapper that took our host ops; it is passed
     * as both the SDL and the native window so either signature works. */
    glide_cwnd((void *)&embed_glide_host_ops, (void *)&embed_glide_host_ops,
               glide_opaque);
    glide_open = 1;
    fx_set_active(1);
    return (int)glide_res;
}

static void fx_glide_release_window(void *opaque, void *cwnd_fn)
{
    if (glide_open) {
        glide_open = 0;
        /* grSstWinClose (glidewnd.c set disp_cb->FEnum before calling) */
        ((fx_cwnd_fn)cwnd_fn)((void *)&embed_glide_host_ops,
                              (void *)&embed_glide_host_ops, opaque);
    }
    embed_gl_fx_end();
    fx_set_active(0);
}

static int fx_glide_gui_fullscreen(int *width, int *height)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *s = con ? qemu_console_surface(con) : NULL;

    if (width) {
        *width = s ? surface_width(s) : 640;
    }
    if (height) {
        *height = s ? surface_height(s) : 480;
    }
    /*
     * "Fullscreen" here means the drawable is the Glide resolution and
     * nothing in QEMU rescales it -- the same bargain mesa_gui_fullscreen
     * makes by reporting the drawable size as the target size. It also
     * stops glidewnd.c upscaling a 640x480 game to the desktop's width,
     * which would hand the player a frame the CRT presets are not
     * calibrated for (doc 03), and silences its stderr fps counter.
     */
    return 1;
}

static void fx_glide_renderer_stat(const int activate)
{
    /* grEnable(GR_PASSTHRU) / grSstControl: the guest asking for the 3D
     * output or the VGA desktop. */
    fx_set_active(activate);
}

static const QemuFxUiOps embed_fx_ui_ops = {
    .glide_prepare_window = fx_glide_prepare_window,
    .glide_release_window = fx_glide_release_window,
    .glide_window_stat    = fx_glide_window_stat,
    .glide_gui_fullscreen = fx_glide_gui_fullscreen,
    .glide_renderer_stat  = fx_glide_renderer_stat,
    .glide_host_ops       = fx_glide_host_ops,
    .mesa_renderer_stat   = fx_mesa_renderer_stat,
    .mesa_prepare_window  = fx_mesa_prepare_window,
    .mesa_release_window  = fx_mesa_release_window,
    .mesa_cursor_define   = fx_ui_default_cursor_define,
    .mesa_mouse_warp      = fx_ui_default_mouse_warp,
    .mesa_gui_fullscreen  = fx_mesa_gui_fullscreen,
};

/* paravirtual Direct3D (hw/d3dpt): the executor's frames take the same
 * path as GL swaps. Unlike the GL path the VGA surface keeps rendering
 * while a device exists: a game's process can die without releasing it
 * (no DLL_PROCESS_DETACH on a crash), and games draw on the VGA surface
 * between CreateDevice and their first Present (Vice City's intro movies
 * through DirectShow, launcher dialogs); the player shows whichever of
 * the two was drawn last. */
static void d3dpt_present_active(bool on)
{
    embed_fx_active(on);
}

static void d3dpt_present_frame(const void *px, int w, int h, int stride)
{
    embed_fx_frame(px, w, h, stride, /* bottom_up */ 0);
}

static const D3dptPresentOps embed_d3dpt_present_ops = {
    .active = d3dpt_present_active,
    .frame = d3dpt_present_frame,
};

void embed_fx_register(void)
{
    qemu_fx_ui_register(&embed_fx_ui_ops);
    d3dpt_set_present_ops(&embed_d3dpt_present_ops);
}
