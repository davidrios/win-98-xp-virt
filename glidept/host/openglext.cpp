//**************************************************************
//* OpenGLide extension lookup for the 2ksbox build: through the host's
//* own resolver when there is one (eglGetProcAddress under the embed
//* backend, CGL symbol lookup on macOS, wglGetProcAddress on Windows),
//* so an extension is resolved against the context that will use it.
//*
//* Upstream resolves through glXGetProcAddress, which needs GLX — and the
//* whole point of this build is a host with no X connection.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "platform/openglext.h"

#include "host.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

ExtFn OGLGetProcAddress(const char *x)
{
    const GlideHostOps *ops = GlideHost_Ops();

    if (ops && ops->get_proc) {
        return (ExtFn)ops->get_proc(x);
    }

#ifdef WIN32
    return (ExtFn)wglGetProcAddress(x);
#else
    /*
     * No host resolver (tools/glide-host-test drives us directly): ask
     * whichever of GLX / EGL the process already has, then fall back to the
     * plain symbol — a GL 1.1 name is in libGL itself, and libglide2x links
     * against it. dlsym'ing the resolvers rather than linking them keeps
     * this build free of both GLX and EGL.
     */
    typedef void *(*ProcFn)(const char *);
    static ProcFn glx_proc, egl_proc;
    static int looked_up;
    void *p;

    if (!looked_up) {
        looked_up = 1;
        glx_proc = (ProcFn)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
        if (!glx_proc) {
            glx_proc = (ProcFn)dlsym(RTLD_DEFAULT, "glXGetProcAddress");
        }
        egl_proc = (ProcFn)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    }
    if (egl_proc && (p = egl_proc(x))) {
        return (ExtFn)p;
    }
    if (glx_proc && (p = glx_proc(x))) {
        return (ExtFn)p;
    }
    return (ExtFn)dlsym(RTLD_DEFAULT, x);
#endif
}
