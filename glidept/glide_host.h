/*
 * glide_host.h — the ABI between QEMU's Glide pass-through device
 * (hw/3dfx) and the host-side Glide wrapper (libglide2x), for a host that
 * has no window.
 *
 * Upstream qemu-3dfx expects the wrapper to own its drawable: it hands
 * `grSstWinOpen` a native window handle (or an SDL_Window*, if the wrapper
 * signed itself 'SDL2') and the wrapper creates a GL context on it. The
 * player has no window to give — QEMU is a library inside it and the frame
 * has to arrive as a texture, not as pixels on screen — so the direction is
 * reversed here: *we* own the context, and the wrapper renders into it.
 *
 * hw/3dfx passes this table to the wrapper's optional `setHostOps` export
 * right after loading it (patch 33). A wrapper that doesn't export the
 * symbol keeps its own windowing and works exactly as upstream; a frontend
 * that registers no `glide_host_ops` passes NULL and gets the same.
 *
 * The one header three builds share — QEMU device, embed library and the
 * wrapper — like d3dpt_proto.h for Direct3D. Bump the version on any
 * change; `setHostOps` refuses a table it doesn't recognise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLIDE_HOST_H
#define GLIDE_HOST_H

#include <stdint.h>

#define GLIDE_HOST_ABI_VERSION 1

/* the wrapper's optional export; dlsym'ed by name, so it must not be
 * mangled or decorated (the wrapper defines it extern "C" and, on Win32,
 * cdecl — hw/3dfx looks it up undecorated on every platform) */
#define GLIDE_HOST_SETOPS_SYM "setHostOps"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GlideHostOps {
    uint32_t size;              /* sizeof(GlideHostOps) */
    uint32_t version;           /* GLIDE_HOST_ABI_VERSION */

    /*
     * grSstWinOpen: bind the host's GL context on the calling thread with a
     * drawable of at least w x h, and leave it current. Returns 0 if the
     * host has no context, which the wrapper must report as a failed
     * grSstWinOpen — the guest then falls back to software rendering
     * instead of drawing into nothing.
     */
    int (*begin)(int w, int h);

    /*
     * grBufferSwap: the frame in framebuffer 0 is complete. The host reads
     * it out (or hands off its dma-buf) and publishes it to the frontend.
     * Called from the vCPU thread with the context still current, and must
     * not block on the consumer.
     */
    void (*present)(void);

    /* grSstWinClose: release the drawable and unbind the context. */
    void (*end)(void);

    /*
     * GL entry points, for the extensions the wrapper resolves at run time
     * (glXGetProcAddress upstream). The core GL 1.1 functions it calls
     * directly come from the process's own libGL, which dispatches on the
     * context we made current, so only extensions need this.
     */
    void *(*get_proc)(const char *name);
} GlideHostOps;

typedef void (*GlideSetHostOps)(const GlideHostOps *ops);

#ifdef __cplusplus
}
#endif

#endif /* GLIDE_HOST_H */
