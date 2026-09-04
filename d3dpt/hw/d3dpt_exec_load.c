/*
 * d3dpt_exec_load.c — dlopen libd3dpt_exec once for every d3dpt device
 * (see d3dpt_exec_load.h). A machine without the library (or without a
 * Vulkan device) boots normally; the devices report "no executor".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include <dlfcn.h>

#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

static D3dptExecLib lib;
static bool tried;

const D3dptExecLib *d3dpt_exec_lib(void)
{
    const char *env = getenv("D3DPT_EXEC_LIB");
    const char *candidates[] = {
        env,
#ifdef __APPLE__
        "build/d3dpt/libd3dpt_exec.dylib", "libd3dpt_exec.dylib",
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
        lib.handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (lib.handle) {
            info_report("d3dpt: executor %s", candidates[i]);
            break;
        }
    }
    if (!lib.handle) {
        warn_report("d3dpt: libd3dpt_exec not found (D3DPT_EXEC_LIB); Direct3D pass-through off");
        return NULL;
    }
    lib.version = dlsym(lib.handle, "d3dpt_exec_version");
    lib.create = dlsym(lib.handle, "d3dpt_exec_create");
    lib.destroy = dlsym(lib.handle, "d3dpt_exec_destroy");
    lib.attach = dlsym(lib.handle, "d3dpt_exec_attach");
    lib.submit = dlsym(lib.handle, "d3dpt_exec_submit");
    lib.set_vram = dlsym(lib.handle, "d3dpt_exec_set_vram");
    if (!lib.version || !lib.create || !lib.destroy || !lib.attach || !lib.submit || !lib.set_vram ||
        lib.version() != D3DPT_PROTO_VERSION) {
        warn_report("d3dpt: executor library mismatch (protocol %u, need %u)",
                    lib.version ? lib.version() : 0, D3DPT_PROTO_VERSION);
        dlclose(lib.handle);
        memset(&lib, 0, sizeof(lib));
        return NULL;
    }
    return &lib;
}
