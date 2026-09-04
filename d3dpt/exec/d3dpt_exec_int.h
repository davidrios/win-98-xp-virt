/*
 * d3dpt_exec_int.h — what d3dpt_exec.cpp (the d3d9 records, doc 14) and
 * d3dpt_exec_ddi.cpp (the display driver's records, doc 15 M7c) share:
 * the executor state, the batch parser state and the record accessors.
 * Internal to libd3dpt_exec.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_EXEC_INT_H
#define D3DPT_EXEC_INT_H

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <unordered_map>
#include <vector>

#include "d3dpt_exec.h"
#include "../d3dpt_proto.h"

namespace d3dpt {

struct Exec;
void exec_ddi_release(Exec &x);

enum Kind : uint8_t { K_NONE, K_DEVICE, K_VB, K_IB, K_TEX, K_SURF, K_VS, K_PS, K_CUBE, K_DECL, K_QUERY };

struct Obj { Kind kind; IUnknown *p; };

struct Exec {
    d3dpt_exec_ops ops;
    void *dxvk = nullptr;
    IDirect3D9 *d3d = nullptr;
    IDirect3DDevice9 *dev = nullptr;
    uint32_t dev_handle = 0;
    std::unordered_map<uint32_t, Obj> objs;
    /* readback staging for Present */
    IDirect3DSurface9 *sys = nullptr;
    uint32_t sys_w = 0, sys_h = 0;
    D3DFORMAT sys_fmt = D3DFMT_UNKNOWN;
    std::vector<uint32_t> conv;
    int attach = 0;
    /* M7c: guest VRAM (d3dpt_exec_set_vram) and the display driver's objects */
    uint8_t *vram = nullptr;
    uint32_t vram_size = 0;
    struct Ddi *ddi = nullptr;

    void log(const char *fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (ops.log) ops.log(ops.ud, buf); else fprintf(stderr, "d3dpt: %s\n", buf);
    }
    template<class T> T *get(uint32_t h, Kind k) {
        auto it = objs.find(h);
        if (it == objs.end() || it->second.kind != k) return nullptr;
        return static_cast<T *>(it->second.p);
    }
    bool put(uint32_t h, Kind k, IUnknown *p) {
        if (!h || objs.count(h)) { if (p) p->Release(); return false; }
        objs[h] = { k, p };
        return true;
    }
    void release_all() {
        exec_ddi_release(*this);
        for (auto &kv : objs) if (kv.second.kind != K_DEVICE && kv.second.p) kv.second.p->Release();
        objs.clear();
        if (sys) { sys->Release(); sys = nullptr; }
        sys_w = sys_h = 0;
        if (dev) {
            dev->Release(); dev = nullptr; dev_handle = 0;
            if (ops.active) ops.active(ops.ud, 0);
        }
    }
};

/* the parse state of one batch */
struct Batch {
    Exec &x;
    uint8_t *shm;
    d3dpt_shm_hdr *hdr;
    uint8_t *ret;       /* return area */
    uint32_t err = D3DPT_ERR_OK;
    uint32_t index = 0;

    /* a return slot: the guest's offset must fit the whole payload */
    d3dpt_ret *slot(uint32_t off, uint32_t payload) {
        if (off % 8 || (uint64_t)off + sizeof(d3dpt_ret) + payload > D3DPT_RET_SIZE) { err = D3DPT_ERR_BAD_ARG; return nullptr; }
        d3dpt_ret *r = (d3dpt_ret *)(ret + off);
        r->hr = (uint32_t)E_FAIL; r->bytes = 0;
        return r;
    }
};

template<class T> static const T *body(const d3dpt_cmd *c, uint32_t extra, Batch &b) {
    if (c->size < sizeof(d3dpt_cmd) + sizeof(T) + extra) { b.err = D3DPT_ERR_MALFORMED; return nullptr; }
    return (const T *)(c + 1);
}
template<class T> static const uint8_t *tail(const T *t) { return (const uint8_t *)(t + 1); }

/* the M7c records (d3dpt_exec_ddi.cpp): true if the op was one of theirs */
bool exec_ddi_op(Batch &b, const d3dpt_cmd *c);

} // namespace d3dpt

#endif
