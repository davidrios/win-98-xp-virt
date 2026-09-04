/*
 * d3dpt_exec.h — C API of the host-side decoder/executor of the
 * paravirtual Direct3D device (libd3dpt_exec, C++ over DXVK's d3d9).
 * The QEMU device (hw/d3dpt) dlopens it, so QEMU stays C and the
 * protocol evolves without a QEMU rebuild.
 *
 * Threading: one caller thread (the vCPU that took the doorbell write,
 * BQL held). DXVK runs its own worker threads underneath.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_EXEC_H
#define D3DPT_EXEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D3DPT_EXEC_API __attribute__((visibility("default")))

typedef struct d3dpt_exec_ops {
    void *ud;
    void (*log)(void *ud, const char *msg);
    /* a device was created (on) / the last one released (off) */
    void (*active)(void *ud, int on);
    /* a frame was presented: XRGB8888, top-down, stride in bytes; valid during the call */
    void (*frame)(void *ud, const void *pixels, int width, int height, int stride);
} d3dpt_exec_ops;

typedef struct d3dpt_exec d3dpt_exec_t;

/* protocol version this library speaks (D3DPT_PROTO_VERSION) */
D3DPT_EXEC_API uint32_t d3dpt_exec_version(void);
/* load DXVK's d3d9 (env D3DPT_DXVK_LIB, else build/dxvk/src/d3d9/libdxvk_d3d9.so.0
 * relative to the cwd, else the bare soname) and create IDirect3D9; NULL if
 * no usable Vulkan device */
D3DPT_EXEC_API d3dpt_exec_t *d3dpt_exec_create(const d3dpt_exec_ops *ops);
D3DPT_EXEC_API void d3dpt_exec_destroy(d3dpt_exec_t *x);
/* guest process attached (1) / detached (0): detach releases every object */
D3DPT_EXEC_API void d3dpt_exec_attach(d3dpt_exec_t *x, int attach);
/* execute the batch described by the window's header; returns D3DPT_ERR_*
 * and writes ret_status/ret_index into the header */
D3DPT_EXEC_API uint32_t d3dpt_exec_submit(d3dpt_exec_t *x, void *shm, uint32_t shm_size);

#ifdef __cplusplus
}
#endif
#endif
