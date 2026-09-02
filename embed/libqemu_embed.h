/*
 * libqemu_embed.h — minimal C API to run QEMU in-process.
 *
 * Thread contract:
 *   - qemu_embed_new(), qemu_embed_run(), qemu_embed_destroy(): one dedicated
 *     caller-created thread (the "QEMU thread"). new() returns with the VM
 *     created and paused (-S is forced); run() blocks until shutdown.
 *   - display callbacks fire on the QEMU thread with the BQL held. They must
 *     not block and must not retain the surface pointer past on_switch's
 *     replacement.
 *   - vm_* and input functions are safe from any thread (async).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIBQEMU_EMBED_H
#define LIBQEMU_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qemu_embed qemu_embed_t;

/* Pixel format of the surface handed to on_switch. v1 accepts only 32bpp
 * XRGB (little-endian 0x00RRGGBB per pixel); QEMU shadows other guest depths
 * into it for us. */
#define QEMU_EMBED_FMT_XRGB8888 1

typedef struct qemu_embed_display_cb {
    /* New surface: pixels valid until the next on_switch. stride in bytes. */
    void (*on_switch)(void *ud, const uint8_t *pixels, int width, int height,
                      int stride, uint32_t fmt);
    /* Rect (already clamped) changed in the current surface. */
    void (*on_update)(void *ud, int x, int y, int w, int h);
    /* One refresh tick finished (all updates of this tick delivered). */
    void (*on_refresh_done)(void *ud);
    /* Hardware cursor image (0xAARRGGBB per pixel, hot spot) — NULL to clear. */
    void (*on_cursor)(void *ud, const uint32_t *argb, int w, int h,
                      int hot_x, int hot_y);
    /* Guest-driven cursor position / visibility. */
    void (*on_mouse_set)(void *ud, int x, int y, bool visible);
} qemu_embed_display_cb;

/* Create + initialize QEMU. argv is a plain qemu-system command line
 * (argv[0] ignored); "-S" and "-display none" are appended. Fatal config
 * errors exit the process (QEMU semantics) — validate before calling.
 * Returns NULL only on argument errors. */
qemu_embed_t *qemu_embed_new(int argc, char **argv,
                             const qemu_embed_display_cb *cb, void *ud);

/* Run the main loop. Returns QEMU's exit status when the guest shuts down
 * or qemu_embed_vm_shutdown() was requested. */
int qemu_embed_run(qemu_embed_t *e);

/* Tear down. QEMU cleanup is incomplete upstream: one VM per process. */
void qemu_embed_destroy(qemu_embed_t *e, int status);

/* --- VM control: async, any thread --- */
void qemu_embed_vm_start(qemu_embed_t *e);      /* resume / first start */
void qemu_embed_vm_pause(qemu_embed_t *e);
void qemu_embed_vm_reset(qemu_embed_t *e);
void qemu_embed_vm_powerdown(qemu_embed_t *e);  /* ACPI power button */
void qemu_embed_vm_shutdown(qemu_embed_t *e);   /* hard stop of the loop */
bool qemu_embed_vm_running(qemu_embed_t *e);

/* --- Input: enqueue from any thread, then flush once per batch --- */
/* qcode = QKeyCode (qapi enum value). See qemu_embed_atset1_to_qcode(). */
void qemu_embed_key(qemu_embed_t *e, uint32_t qcode, bool down);
/* AT set-1 scancode (0xE0-prefixed codes as 0xE0xx) -> QKeyCode, 0 if none */
uint32_t qemu_embed_atset1_to_qcode(uint32_t atset1);
void qemu_embed_mouse_rel(qemu_embed_t *e, int dx, int dy);
/* x,y in [0,w) x [0,h): scaled to the guest's absolute range */
void qemu_embed_mouse_abs(qemu_embed_t *e, int x, int y, int w, int h);
/* button: 0 left, 1 middle, 2 right, 3 wheel up, 4 wheel down, 5 side, 6 extra */
void qemu_embed_mouse_btn(qemu_embed_t *e, uint32_t button, bool down);
/* Whether the active guest pointer device wants absolute coordinates. */
bool qemu_embed_mouse_is_absolute(qemu_embed_t *e);
/* Schedule delivery of everything enqueued (one sync). */
void qemu_embed_input_flush(qemu_embed_t *e);

/* Library version of the embed API, for the bindings to sanity-check. */
uint32_t qemu_embed_api_version(void);
#define QEMU_EMBED_API_VERSION 1

#ifdef __cplusplus
}
#endif
#endif /* LIBQEMU_EMBED_H */
