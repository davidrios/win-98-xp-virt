//**************************************************************
//* The exports hw/3dfx looks for in a host-side Glide wrapper
//* (2ksbox, doc 12 §5), plus the log that replaces OpenGLide's own.
//*
//*   setConfig(flags, magic)  — glidewnd.c's WRAPPER_FLAG_* word, and a
//*                              signature written back into *magic.
//*                              Upstream OpenGLide's setConfig takes the
//*                              flags alone (_setConfig@4); qemu-3dfx
//*                              calls _setConfig@8, so the one-argument
//*                              form is patched out and this replaces it.
//*   setConfigRes(res, swap)  — the scaled width glidewnd.c computed, or 0.
//*   setHostOps(ops)          — glide_host.h: our context, our present.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GlOgl.h"
#include "wrapper_config.h"

#include "host.h"

/* qemu-3dfx sends '$g2X' and reads back what the wrapper is. 'SDL2' means
 * "hand me the SDL_Window*"; ours means "the host holds the context", which
 * hw/3dfx does not have to understand — it passes the same pointer as both
 * the SDL and the native window handle (embedfx.c). The word is here so a
 * QEMU log, or a later protocol step, can name the wrapper it loaded. */
#define GLIDE_HOST_SIGN 0x42534b32 /* '2KSB' */

static const GlideHostOps *host_ops;
static int cfg_width;

const GlideHostOps *GlideHost_Ops(void)
{
    return host_ops;
}

int GlideHost_ConfigWidth(void)
{
    return cfg_width;
}

FILE *GlideHost_LogFile(void)
{
    static FILE *fp;
    static int tried;

    if (!tried) {
        const char *path = getenv("GLIDE_HOST_LOG");
        tried = 1;
        if (path && *path) {
            fp = !strcmp(path, "-") ? stderr : fopen(path, "w");
        }
    }
    return fp;
}

void GlideHost_Log(const char *fmt, ...)
{
    FILE *fp = GlideHost_LogFile();
    va_list ap;

    if (!fp) {
        return;
    }
    fputs("glide2x: ", fp);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fflush(fp);
}

extern "C" {

FX_ENTRY void FX_CALL setConfig(FxU32 flags, void *magic)
{
    UserConfig.EnableMipMaps = ((flags & WRAPPER_FLAG_MIPMAPS) != 0);
    /* The host draws into an offscreen buffer whatever the guest asked for;
     * a wrapper-created window is exactly what this build exists to avoid. */
    UserConfig.CreateWindow = false;
    UserConfig.InitFullScreen = false;

    if (magic) {
        *(FxU32 *)magic = GLIDE_HOST_SIGN;
    }
    GlideHost_Log("setConfig flags %08x%s", flags,
                  (flags & 0x80) ? " (qemu)" : "");
}

FX_ENTRY void FX_CALL setConfigRes(int res, void *swap12)
{
    /* glidewnd.c's upscale width. The player scales the frame itself with
     * the shader chain, at the guest's own aspect, so this is recorded and
     * not acted on: rendering at the Glide resolution is what the CRT
     * presets are calibrated against (doc 03). */
    cfg_width = res;
    GlideHost_Log("setConfigRes %d (recorded, host scales)", res);
}

void setHostOps(const GlideHostOps *ops)
{
    if (!ops) {
        host_ops = NULL;
        GlideHost_Log("host ops cleared");
        return;
    }
    if (ops->version != GLIDE_HOST_ABI_VERSION ||
        ops->size < sizeof(GlideHostOps)) {
        GlideHost_Log("host ops rejected: version %u size %u, want %u/%u",
                      ops->version, ops->size,
                      (unsigned)GLIDE_HOST_ABI_VERSION,
                      (unsigned)sizeof(GlideHostOps));
        host_ops = NULL;
        return;
    }
    host_ops = ops;
    GlideHost_Log("host ops v%u accepted", ops->version);
}

} /* extern "C" */
