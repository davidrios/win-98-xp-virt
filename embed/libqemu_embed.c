/*
 * libqemu_embed.c — run QEMU in-process behind a small C API.
 * See libqemu_embed.h for the thread contract. Design: docs/11-m1-embed-api.md
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "block/aio.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qemu-main.h"
#include "libqemu_embed.h"
#include "embedfx.h"

/*
 * system/main.c is not part of the library, but ui/cocoa.m (macOS) still
 * references qemu_default_main()/qemu_main: provide upstream's definitions.
 * Unused in the embed flow (-display none); the caller drives the loop via
 * qemu_embed_run().
 */
int qemu_default_main(void)
{
    int status = qemu_main_loop();
    qemu_cleanup(status);
    return status;
}
int (*qemu_main)(void) = qemu_default_main;

enum { EV_KEY, EV_REL, EV_ABS, EV_BTN };

typedef struct {
    uint8_t kind;
    bool down;
    int32_t a, b, c, d;
} in_event;

#define IN_QUEUE_LEN 512

struct qemu_embed {
    DisplayChangeListener dcl;
    qemu_embed_display_cb cb;
    void *ud;
    QemuConsole *con;

    QemuMutex in_lock;
    in_event in_q[IN_QUEUE_LEN];
    unsigned in_n;
    bool in_bh_pending;

    int argc;
    char **argv;
    uint32_t refresh_ms;
    uint32_t *flip;         /* row-flipped copy of a bottom-up 3D frame */
    size_t flip_len;
};

/* one VM per process: the 3D backend reports through this instance */
static qemu_embed_t *fx_instance;

void embed_fx_active(bool on)
{
    qemu_embed_t *e = fx_instance;
    if (e && e->cb.on_3d_active) {
        e->cb.on_3d_active(e->ud, on);
    }
}

void embed_fx_frame(const uint32_t *px, int w, int h, int stride, int bottom_up)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_frame) {
        return;
    }
    if (bottom_up) {
        size_t need = (size_t)w * h;
        if (e->flip_len < need) {
            e->flip = g_realloc(e->flip, need * sizeof(uint32_t));
            e->flip_len = need;
        }
        const uint8_t *src = (const uint8_t *)px;
        for (int y = 0; y < h; y++) {
            memcpy(e->flip + (size_t)y * w, src + (size_t)(h - 1 - y) * stride,
                   (size_t)w * 4);
        }
        e->cb.on_3d_frame(e->ud, (const uint8_t *)e->flip, w, h, w * 4);
    } else {
        e->cb.on_3d_frame(e->ud, (const uint8_t *)px, w, h, stride);
    }
}

int embed_fx_dmabuf(int slot, int fd, int w, int h, int stride,
                    uint32_t fourcc, uint64_t modifier)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_dmabuf || !e->cb.on_3d_frame_ready) {
        return 0;
    }
    int dupfd = dup(fd);
    if (dupfd < 0) {
        return 0;
    }
    if (!e->cb.on_3d_dmabuf(e->ud, slot, dupfd, w, h, stride, fourcc, modifier)) {
        close(dupfd);
        return 0;
    }
    return 1;
}

int embed_fx_iosurface(int slot, void *iosurface, int w, int h)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_iosurface || !e->cb.on_3d_frame_ready) {
        return 0;
    }
    return e->cb.on_3d_iosurface(e->ud, slot, iosurface, w, h) ? 1 : 0;
}

void embed_fx_frame_ready(int slot)
{
    qemu_embed_t *e = fx_instance;
    if (e && e->cb.on_3d_frame_ready) {
        e->cb.on_3d_frame_ready(e->ud, slot);
    }
}

/* ---------------------------------------------------------------- display */

static void embed_dpy_refresh(DisplayChangeListener *dcl)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    graphic_hw_update(dcl->con);
    if (e->cb.on_refresh_done) {
        e->cb.on_refresh_done(e->ud);
    }
}

static void embed_dpy_gfx_update(DisplayChangeListener *dcl,
                                 int x, int y, int w, int h)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_update) {
        e->cb.on_update(e->ud, x, y, w, h);
    }
}

static void embed_dpy_gfx_switch(DisplayChangeListener *dcl,
                                 DisplaySurface *surface)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_switch) {
        e->cb.on_switch(e->ud, surface_data(surface),
                        surface_width(surface), surface_height(surface),
                        surface_stride(surface), QEMU_EMBED_FMT_XRGB8888);
    }
}

static bool embed_dpy_gfx_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    /* v1: one format for the host side; QEMU shadows everything else. */
    return format == PIXMAN_x8r8g8b8;
}

static void embed_dpy_mouse_set(DisplayChangeListener *dcl,
                                int x, int y, bool on)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_mouse_set) {
        e->cb.on_mouse_set(e->ud, x, y, on);
    }
}

static void embed_dpy_cursor_define(DisplayChangeListener *dcl,
                                    QEMUCursor *c)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_cursor) {
        if (c) {
            e->cb.on_cursor(e->ud, c->data, c->width, c->height,
                            c->hot_x, c->hot_y);
        } else {
            e->cb.on_cursor(e->ud, NULL, 0, 0, 0, 0);
        }
    }
}

static const DisplayChangeListenerOps embed_dcl_ops = {
    .dpy_name             = "embed",
    .dpy_refresh          = embed_dpy_refresh,
    .dpy_gfx_update       = embed_dpy_gfx_update,
    .dpy_gfx_switch       = embed_dpy_gfx_switch,
    .dpy_gfx_check_format = embed_dpy_gfx_check_format,
    .dpy_mouse_set        = embed_dpy_mouse_set,
    .dpy_cursor_define    = embed_dpy_cursor_define,
};

/* -------------------------------------------------------------- lifecycle */

uint32_t qemu_embed_api_version(void)
{
    return QEMU_EMBED_API_VERSION;
}

qemu_embed_t *qemu_embed_new(int argc, char **argv,
                             const qemu_embed_display_cb *cb, void *ud)
{
    if (argc < 1 || !argv) {
        return NULL;
    }
    qemu_embed_t *e = g_new0(qemu_embed_t, 1);
    if (cb) {
        e->cb = *cb;
    }
    e->ud = ud;
    qemu_mutex_init(&e->in_lock);

    /* argv + "-S" + "-display none" (+ NULL); we own the copies. */
    e->argc = argc + 3;
    e->argv = g_new0(char *, e->argc + 1);
    for (int i = 0; i < argc; i++) {
        e->argv[i] = g_strdup(argv[i]);
    }
    e->argv[argc]     = g_strdup("-S");
    e->argv[argc + 1] = g_strdup("-display");
    e->argv[argc + 2] = g_strdup("none");

    /* Takes the BQL on this thread; exits the process on fatal errors. */
    qemu_init(e->argc, e->argv);

    e->con = qemu_console_lookup_default();
    e->dcl.ops = &embed_dcl_ops;
    e->dcl.con = e->con;
    /* Fires on_switch/on_update/on_cursor synchronously before returning. */
    register_displaychangelistener(&e->dcl);
    /* qemu-3dfx: window-less context provider (doc 12) */
    fx_instance = e;
    embed_fx_register();
    return e;
}

int qemu_embed_run(qemu_embed_t *e)
{
    (void)e;
    return qemu_main_loop();
}

void qemu_embed_destroy(qemu_embed_t *e, int status)
{
    fx_instance = NULL;
    unregister_displaychangelistener(&e->dcl);
    qemu_cleanup(status);
    g_free(e->flip);
    qemu_mutex_destroy(&e->in_lock);
    for (int i = 0; i < e->argc; i++) {
        g_free(e->argv[i]);
    }
    g_free(e->argv);
    g_free(e);
}

static void bh_set_refresh(void *opaque)
{
    qemu_embed_t *e = opaque;
    update_displaychangelistener(&e->dcl, e->refresh_ms);
}

void qemu_embed_set_refresh_ms(qemu_embed_t *e, uint32_t ms)
{
    e->refresh_ms = ms ? ms : 1;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_set_refresh, e);
}

/* ------------------------------------------------------------- vm control */

static void bh_vm_start(void *opaque)
{
    (void)opaque;
    if (!runstate_is_running()) {
        vm_start();
    }
}

void qemu_embed_vm_start(qemu_embed_t *e)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_vm_start, e);
}

void qemu_embed_vm_pause(qemu_embed_t *e)
{
    (void)e;
    qemu_system_vmstop_request(RUN_STATE_PAUSED);
}

void qemu_embed_vm_reset(qemu_embed_t *e)
{
    (void)e;
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_QMP_SYSTEM_RESET);
}

void qemu_embed_vm_powerdown(qemu_embed_t *e)
{
    (void)e;
    qemu_system_powerdown_request();
}

void qemu_embed_vm_shutdown(qemu_embed_t *e)
{
    (void)e;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
}

bool qemu_embed_vm_running(qemu_embed_t *e)
{
    (void)e;
    return runstate_is_running();
}

/* ------------------------------------------------------------------ input */

static const InputButton button_map[] = {
    INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
    INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
    INPUT_BUTTON_SIDE, INPUT_BUTTON_EXTRA,
};

static void enqueue(qemu_embed_t *e, in_event ev)
{
    qemu_mutex_lock(&e->in_lock);
    if (e->in_n < IN_QUEUE_LEN) {
        e->in_q[e->in_n++] = ev;
    }
    /* else: drop — a stalled main loop should not grow memory unbounded */
    qemu_mutex_unlock(&e->in_lock);
}

void qemu_embed_key(qemu_embed_t *e, uint32_t qcode, bool down)
{
    enqueue(e, (in_event){ .kind = EV_KEY, .down = down, .a = qcode });
}

uint32_t qemu_embed_atset1_to_qcode(uint32_t atset1)
{
    if (atset1 < qemu_input_map_atset1_to_qcode_len) {
        return qemu_input_map_atset1_to_qcode[atset1];
    }
    return 0;
}

void qemu_embed_mouse_rel(qemu_embed_t *e, int dx, int dy)
{
    enqueue(e, (in_event){ .kind = EV_REL, .a = dx, .b = dy });
}

void qemu_embed_mouse_abs(qemu_embed_t *e, int x, int y, int w, int h)
{
    enqueue(e, (in_event){ .kind = EV_ABS, .a = x, .b = y, .c = w, .d = h });
}

void qemu_embed_mouse_btn(qemu_embed_t *e, uint32_t button, bool down)
{
    if (button < ARRAY_SIZE(button_map)) {
        enqueue(e, (in_event){ .kind = EV_BTN, .down = down, .a = button });
    }
}

bool qemu_embed_mouse_is_absolute(qemu_embed_t *e)
{
    return qemu_input_is_absolute(e->con);
}

/* Runs on the main loop under BQL. */
static void bh_input_drain(void *opaque)
{
    qemu_embed_t *e = opaque;
    in_event batch[IN_QUEUE_LEN];
    unsigned n;

    qemu_mutex_lock(&e->in_lock);
    n = e->in_n;
    memcpy(batch, e->in_q, n * sizeof(in_event));
    e->in_n = 0;
    e->in_bh_pending = false;
    qemu_mutex_unlock(&e->in_lock);

    bool pointer = false;
    for (unsigned i = 0; i < n; i++) {
        in_event *ev = &batch[i];
        switch (ev->kind) {
        case EV_KEY:
            qemu_input_event_send_key_qcode(e->con, (QKeyCode)ev->a, ev->down);
            break;
        case EV_REL:
            qemu_input_queue_rel(e->con, INPUT_AXIS_X, ev->a);
            qemu_input_queue_rel(e->con, INPUT_AXIS_Y, ev->b);
            pointer = true;
            break;
        case EV_ABS:
            qemu_input_queue_abs(e->con, INPUT_AXIS_X, ev->a, 0, ev->c);
            qemu_input_queue_abs(e->con, INPUT_AXIS_Y, ev->b, 0, ev->d);
            pointer = true;
            break;
        case EV_BTN:
            qemu_input_queue_btn(e->con, button_map[ev->a], ev->down);
            pointer = true;
            break;
        }
    }
    if (pointer) {
        qemu_input_event_sync();
    }
}

void qemu_embed_input_flush(qemu_embed_t *e)
{
    bool schedule;
    qemu_mutex_lock(&e->in_lock);
    schedule = !e->in_bh_pending && e->in_n > 0;
    if (schedule) {
        e->in_bh_pending = true;
    }
    qemu_mutex_unlock(&e->in_lock);
    if (schedule) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_input_drain, e);
    }
}
