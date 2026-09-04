/*
 * embedfx.c — qemu-3dfx UI provider for the embed library (patch 30's
 * QemuFxUiOps). No window: the backend's pbuffer is the drawable, so the
 * Mesa entry points just acknowledge, report activation to the frontend
 * and answer size queries; Glide keeps the no-provider defaults until it
 * gets an offscreen path (doc 12 §5).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "embedfx.h"
#include "hw/d3dpt/d3dpt.h"

static int fx_active;

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
    if (fx_active) {
        fx_active = 0;
        graphic_hw_passthrough(qemu_console_lookup_by_index(0), 0);
        embed_fx_active(false);
    }
}

static void fx_mesa_renderer_stat(const int activate)
{
    if (activate == fx_active) {
        return;
    }
    fx_active = activate;
    /* while active the VGA device is not rendered; frames come from swaps */
    graphic_hw_passthrough(qemu_console_lookup_by_index(0), activate);
    embed_fx_active(activate != 0);
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

static const QemuFxUiOps embed_fx_ui_ops = {
    /* glide_*: NULL = ui/fxui.c defaults (no window, reported cleanly) */
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
