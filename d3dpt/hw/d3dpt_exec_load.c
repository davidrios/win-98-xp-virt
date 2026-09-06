/*
 * d3dpt_exec_load.c — dlopen libd3dpt_exec once for every d3dpt device
 * (see d3dpt_exec_load.h). A machine without the library (or without a
 * Vulkan device) boots normally; the devices report "no executor".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

/*
 * Windows spells the same three calls differently and has no dlfcn.h.
 * Kept as a local shim rather than glib's GModule because QEMU's glib
 * dependency does not carry gmodule-2.0 on every host, and this is the
 * only dynamic load in the tree.
 */
#ifdef _WIN32
#include <windows.h>
#define D3DPT_DLOPEN(p)     ((void *)LoadLibraryA(p))
#define D3DPT_DLSYM(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#define D3DPT_DLCLOSE(h)    FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define D3DPT_DLOPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define D3DPT_DLSYM(h, s)   dlsym((h), (s))
#define D3DPT_DLCLOSE(h)    dlclose(h)
#endif

static D3dptExecLib lib;
static bool tried;

const D3dptExecLib *d3dpt_exec_lib(void)
{
    const char *env = getenv("D3DPT_EXEC_LIB");
    const char *candidates[] = {
        env,
#if defined(__APPLE__)
        "build/d3dpt/libd3dpt_exec.dylib", "libd3dpt_exec.dylib",
#elif defined(_WIN32)
        /* Bare name last: LoadLibrary searches the executable's own
         * directory first, which is where the package puts it. */
        "build/win/d3dpt/d3dpt_exec.dll", "d3dpt_exec.dll",
#else
        "build/d3dpt/libd3dpt_exec.so", "libd3dpt_exec.so",
#endif
    };

    if (tried) {
        return lib.handle ? &lib : NULL;
    }
    tried = true;
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        if (!candidates[i]) {
            continue;
        }
        lib.handle = D3DPT_DLOPEN(candidates[i]);
        if (lib.handle) {
            info_report("d3dpt: executor %s", candidates[i]);
            break;
        }
    }
    if (!lib.handle) {
        warn_report("d3dpt: libd3dpt_exec not found (D3DPT_EXEC_LIB); Direct3D pass-through off");
        return NULL;
    }
    lib.version = D3DPT_DLSYM(lib.handle, "d3dpt_exec_version");
    lib.create = D3DPT_DLSYM(lib.handle, "d3dpt_exec_create");
    lib.destroy = D3DPT_DLSYM(lib.handle, "d3dpt_exec_destroy");
    lib.attach = D3DPT_DLSYM(lib.handle, "d3dpt_exec_attach");
    lib.submit = D3DPT_DLSYM(lib.handle, "d3dpt_exec_submit");
    lib.set_vram = D3DPT_DLSYM(lib.handle, "d3dpt_exec_set_vram");
    if (!lib.version || !lib.create || !lib.destroy || !lib.attach || !lib.submit || !lib.set_vram ||
        lib.version() != D3DPT_PROTO_VERSION) {
        warn_report("d3dpt: executor library mismatch (protocol %u, need %u)",
                    lib.version ? lib.version() : 0, D3DPT_PROTO_VERSION);
        D3DPT_DLCLOSE(lib.handle);
        memset(&lib, 0, sizeof(lib));
        return NULL;
    }
    return &lib;
}
