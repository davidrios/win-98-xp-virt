//**************************************************************
//* OpenGLide platform layer for a host that provides the context
//* (2ksbox, doc 12 §5). Replaces platform/linux/window.cpp and
//* platform/sdl/window.cpp: no X11, no SDL, no window at all.
//*
//* OpenGLide's whole windowing seam is four functions with four call
//* sites, so the reversal is small: instead of opening a drawable and
//* swapping it, InitialiseOpenGLWindow asks the host to make its context
//* current (mglcntx_embed.c's EGL pbuffer / CGL stand-in FBO, the same
//* framebuffer the Mesa pass-through renders into) and SwapBuffers tells
//* the host the frame is done. The host publishes it to the frontend, so
//* a Glide frame reaches the player's shader chain by the same path as a
//* wglSwapBuffers, dma-buf ring included.
//*
//* Without host ops (an ordinary process that loaded libglide2x on its
//* own — tools/glide-host-test) the context has to be current already;
//* we check for one and refuse if there is none, rather than drawing
//* into nothing.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include "GlOgl.h"
#include "platform/window.h"

#include "host.h"

bool InitialiseOpenGLWindow(FxU wnd, int x, int y, int width, int height)
{
    const GlideHostOps *ops = GlideHost_Ops();

    if (ops) {
        if (!ops->begin(width, height)) {
            GlideHost_Log("host refused a %dx%d drawable", width, height);
            return false;
        }
        GlideHost_Log("grSstWinOpen %dx%d on the host's context", width, height);
        return true;
    }

    /* No host ops: whoever loaded us must have made a context current. */
    if (!glGetString(GL_VERSION)) {
        GlideHost_Log("no host ops and no current GL context; refusing "
                      "grSstWinOpen");
        return false;
    }
    GlideHost_Log("grSstWinOpen %dx%d on the caller's context", width, height);
    return true;
}

void FinaliseOpenGLWindow(void)
{
    const GlideHostOps *ops = GlideHost_Ops();

    if (ops) {
        ops->end();
    }
}

void SwapBuffers(void)
{
    const GlideHostOps *ops = GlideHost_Ops();

    if (ops) {
        ops->present();
    } else {
        glFlush();
    }
}

/*
 * The host owns the mode and the gamma ramp: a window-less drawable has no
 * ramp to load, and the player's shader chain is where a guest's gamma
 * belongs anyway. grLoadGammaTable still reaches OpenGLide, which applies
 * it to the frames it draws; only the display-wide ramp is dropped.
 */
void SetGamma(float value)
{
}

void RestoreGamma(void)
{
}

bool SetScreenMode(int &xsize, int &ysize)
{
    return true;
}

void ResetScreenMode(void)
{
}
