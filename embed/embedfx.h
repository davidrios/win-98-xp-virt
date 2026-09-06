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
/* Glide (doc 12 §5): a context of our own on the same offscreen drawable,
 * sized to the Glide resolution. The wrapper renders into it and
 * embed_gl_fx_present publishes FBO 0 exactly as a GL swap does. */
int embed_gl_fx_begin(int w, int h);
void embed_gl_fx_present(void);
void embed_gl_fx_end(void);
void *embed_gl_fx_proc(const char *name);

/* provider (embedfx.c): register the QemuFxUiOps table; call after qemu_init */
void embed_fx_register(void);

/* libqemu_embed.c: deliver to the frontend (vCPU thread, BQL held) */
void embed_fx_active(bool on);
void embed_fx_frame(const uint32_t *px, int w, int h, int stride, int bottom_up);
/* zero-copy: offer a ring slot's dma-buf (fd is dup'ed for the frontend) */
int embed_fx_dmabuf(int slot, int fd, int w, int h, int stride,
                    uint32_t fourcc, uint64_t modifier);
void embed_fx_frame_ready(int slot);
int embed_fx_iosurface(int slot, void *iosurface, int w, int h);

#endif
