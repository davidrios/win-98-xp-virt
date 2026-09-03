/*
 * embedfx.h — glue between the window-less GL backend (mglcntx_embed.c),
 * the qemu-3dfx UI provider (embedfx.c) and the embed API (libqemu_embed.c).
 * Internal to libqemu-embed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef EMBEDFX_H
#define EMBEDFX_H

#include <stdbool.h>
#include <stdint.h>

/* backend (mglcntx_embed.c) */
int embed_gl_available(void);                 /* EGL usable on this host */
void embed_gl_drawable_size(int *w, int *h);  /* current FBO 0 size */

/* provider (embedfx.c): register the QemuFxUiOps table; call after qemu_init */
void embed_fx_register(void);

/* libqemu_embed.c: deliver to the frontend (vCPU thread, BQL held) */
void embed_fx_active(bool on);
void embed_fx_frame(const uint32_t *px, int w, int h, int stride, int bottom_up);

#endif
