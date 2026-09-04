/*
 * d3dpt_exec.cpp — decoder + executor of the paravirtual Direct3D device
 * over DXVK's native d3d9 (doc 14, ADR-006/007). Parses the batch the
 * guest left in the shared window (d3dpt_proto.h), validates every
 * record against the object mirror, and calls IDirect3DDevice9. Present
 * reads the backbuffer back and hands it to the host through ops.frame
 * (zero-copy through DXVK's Vulkan interop comes later).
 *
 * Build: scripts/build-d3dpt-exec.sh -> build/d3dpt/libd3dpt_exec.so
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <d3d9.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3dpt_exec.h"
#include "../d3dpt_proto.h"

static_assert(sizeof(D3DCAPS9) == D3DPT_SIZEOF_CAPS9, "D3DCAPS9 layout");
static_assert(sizeof(D3DLIGHT9) == sizeof(((d3dpt_light *)0)->light), "D3DLIGHT9 layout");
static_assert(sizeof(D3DMATERIAL9) == sizeof(((d3dpt_material *)0)->material), "D3DMATERIAL9 layout");
static_assert(sizeof(D3DVIEWPORT9) == sizeof(d3dpt_viewport), "D3DVIEWPORT9 layout");
static_assert(sizeof(D3DRECT) == 16, "D3DRECT layout");

namespace {

enum Kind : uint8_t { K_NONE, K_DEVICE, K_VB, K_IB, K_TEX, K_SURF, K_VS, K_PS };

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

static uint32_t prim_vertices(uint32_t type, uint32_t count, bool &ok) {
    ok = count > 0 && count < (1u << 24);
    switch (type) {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return count * 2;
    case D3DPT_LINESTRIP: return count + 1;
    case D3DPT_TRIANGLELIST: return count * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return count + 2;
    default: ok = false; return 0;
    }
}

static bool need_device(Batch &b) {
    if (!b.x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return false; }
    return true;
}

static void present_frame(Exec &x) {
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(x.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (!x.sys || x.sys_w != d.Width || x.sys_h != d.Height || x.sys_fmt != d.Format) {
        if (x.sys) { x.sys->Release(); x.sys = nullptr; }
        if (FAILED(x.dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &x.sys, nullptr))) {
            x.log("present: no staging surface for %ux%u fmt %u", d.Width, d.Height, (unsigned)d.Format);
            bb->Release();
            return;
        }
        x.sys_w = d.Width; x.sys_h = d.Height; x.sys_fmt = d.Format;
    }
    HRESULT hr = x.dev->GetRenderTargetData(bb, x.sys);
    bb->Release();
    if (FAILED(hr)) { x.log("present: GetRenderTargetData 0x%08x", (unsigned)hr); return; }
    D3DLOCKED_RECT lr;
    if (FAILED(x.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    if (x.ops.frame) {
        if (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) {
            x.ops.frame(x.ops.ud, lr.pBits, (int)d.Width, (int)d.Height, lr.Pitch);
        } else {
            /* 16-bit backbuffers: expand to XRGB */
            x.conv.resize((size_t)d.Width * d.Height);
            for (uint32_t y = 0; y < d.Height; y++) {
                const uint16_t *s = (const uint16_t *)((const uint8_t *)lr.pBits + y * lr.Pitch);
                uint32_t *o = x.conv.data() + (size_t)y * d.Width;
                for (uint32_t i = 0; i < d.Width; i++) {
                    uint32_t v = s[i], r, g, bl;
                    if (d.Format == D3DFMT_R5G6B5) { r = (v >> 11) & 31; g = (v >> 5) & 63; bl = v & 31; g = (g << 2) | (g >> 4); }
                    else { r = (v >> 10) & 31; g = (v >> 5) & 31; bl = v & 31; g = (g << 3) | (g >> 2); }
                    r = (r << 3) | (r >> 2); bl = (bl << 3) | (bl >> 2);
                    o[i] = 0xff000000u | (r << 16) | (g << 8) | bl;
                }
            }
            x.ops.frame(x.ops.ud, x.conv.data(), (int)d.Width, (int)d.Height, (int)d.Width * 4);
        }
    }
    x.sys->UnlockRect();
}

static void fill_pp(D3DPRESENT_PARAMETERS &pp, const d3dpt_present_params &g) {
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = g.width; pp.BackBufferHeight = g.height;
    pp.BackBufferFormat = (D3DFORMAT)g.format; pp.BackBufferCount = g.backbuffer_count;
    pp.MultiSampleType = (D3DMULTISAMPLE_TYPE)g.multisample; pp.MultiSampleQuality = g.multisample_quality;
    pp.SwapEffect = (D3DSWAPEFFECT)g.swap_effect;
    pp.hDeviceWindow = nullptr;
    pp.Windowed = TRUE;                      /* fullscreen is the guest's business; no host window */
    pp.EnableAutoDepthStencil = g.auto_depth ? TRUE : FALSE;
    pp.AutoDepthStencilFormat = (D3DFORMAT)g.depth_format;
    pp.Flags = g.flags & ~(DWORD)D3DPRESENTFLAG_DEVICECLIP;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   /* pacing is the guest's / the player's */
}

static void exec_one(Batch &b, const d3dpt_cmd *c) {
    Exec &x = b.x;
    switch (c->op) {
    case D3DPT_OP_NOP:
        break;

    case D3DPT_OP_GET_ADAPTER: {
        auto *a = body<d3dpt_get_adapter>(c, 0, b); if (!a) return;
        UINT n = x.d3d->GetAdapterModeCount(a->adapter, D3DFMT_X8R8G8B8);
        if (n > 64) n = 64;
        d3dpt_ret *r = b.slot(a->ret_off, sizeof(d3dpt_adapter_info) + n * sizeof(d3dpt_mode)); if (!r) return;
        d3dpt_adapter_info *info = (d3dpt_adapter_info *)(r + 1);
        memset(info, 0, sizeof *info);
        D3DADAPTER_IDENTIFIER9 id;
        HRESULT hr = x.d3d->GetAdapterIdentifier(a->adapter, 0, &id);
        if (SUCCEEDED(hr)) {
            memcpy(info->identifier.description, id.Description, sizeof id.Description);
            memcpy(info->identifier.driver, id.Driver, sizeof id.Driver);
            memcpy(info->identifier.device_name, id.DeviceName, sizeof id.DeviceName);
            info->identifier.driver_version_lo = (uint32_t)id.DriverVersion.LowPart;
            info->identifier.driver_version_hi = (uint32_t)id.DriverVersion.HighPart;
            info->identifier.vendor_id = id.VendorId; info->identifier.device_id = id.DeviceId;
            info->identifier.subsys_id = id.SubSysId; info->identifier.revision = id.Revision;
            memcpy(info->identifier.guid, &id.DeviceIdentifier, 16);
            info->identifier.whql_level = id.WHQLLevel;
            D3DCAPS9 caps;
            hr = x.d3d->GetDeviceCaps(a->adapter, D3DDEVTYPE_HAL, &caps);
            if (SUCCEEDED(hr)) memcpy(info->caps, &caps, sizeof caps);
        }
        d3dpt_mode *modes = (d3dpt_mode *)(info + 1);
        for (UINT i = 0; i < n; i++) {
            D3DDISPLAYMODE m;
            if (FAILED(x.d3d->EnumAdapterModes(a->adapter, D3DFMT_X8R8G8B8, i, &m))) { n = i; break; }
            modes[i] = { m.Width, m.Height, m.RefreshRate, (uint32_t)m.Format };
        }
        info->mode_count = n;
        r->hr = (uint32_t)hr; r->bytes = sizeof *info + n * sizeof(d3dpt_mode);
        break;
    }

    case D3DPT_OP_CREATE_DEVICE:
    case D3DPT_OP_RESET_DEVICE: {
        auto *a = body<d3dpt_create_device>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        D3DPRESENT_PARAMETERS pp; fill_pp(pp, a->pp);
        if (pp.BackBufferWidth == 0 || pp.BackBufferHeight == 0 || pp.BackBufferWidth > 4096 || pp.BackBufferHeight > 4096) {
            r->hr = (uint32_t)D3DERR_INVALIDCALL; return;
        }
        if (c->op == D3DPT_OP_RESET_DEVICE) {
            if (!x.dev || a->handle != x.dev_handle) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
            if (x.sys) { x.sys->Release(); x.sys = nullptr; x.sys_w = x.sys_h = 0; }
            r->hr = (uint32_t)x.dev->Reset(&pp);
            x.log("reset %ux%u fmt %u -> 0x%08x", pp.BackBufferWidth, pp.BackBufferHeight, (unsigned)pp.BackBufferFormat, r->hr);
            return;
        }
        if (x.dev) x.release_all();       /* one device per attached process */
        DWORD flags = a->behavior & (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                     D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_MULTITHREADED |
                                     D3DCREATE_FPU_PRESERVE);
        if (!(flags & (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)))
            flags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        IDirect3DDevice9 *dev = nullptr;
        HRESULT hr;
        try {
            hr = x.d3d->CreateDevice(a->adapter, D3DDEVTYPE_HAL, nullptr, flags, &pp, &dev);
        } catch (...) { hr = E_FAIL; dev = nullptr; }
        r->hr = (uint32_t)hr;
        x.log("create device %ux%u fmt %u depth %u/%u flags 0x%x -> 0x%08x", pp.BackBufferWidth, pp.BackBufferHeight,
              (unsigned)pp.BackBufferFormat, pp.EnableAutoDepthStencil, (unsigned)pp.AutoDepthStencilFormat, (unsigned)flags, (unsigned)hr);
        if (SUCCEEDED(hr) && dev) {
            x.dev = dev; x.dev_handle = a->handle;
            x.objs[a->handle] = { K_DEVICE, dev };
            if (x.ops.active) x.ops.active(x.ops.ud, 1);
        }
        break;
    }

    case D3DPT_OP_RELEASE: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return;
        auto it = x.objs.find(a->handle);
        if (it == x.objs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        if (it->second.kind == K_DEVICE) { x.release_all(); return; }
        it->second.p->Release();
        x.objs.erase(it);
        break;
    }

    case D3DPT_OP_PRESENT: {
        auto *a = body<d3dpt_sync>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        r->hr = (uint32_t)x.dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (SUCCEEDED(r->hr)) { present_frame(x); b.hdr->frames++; }
        break;
    }

    case D3DPT_OP_CLEAR: {
        auto *a = body<d3dpt_clear>(c, 0, b); if (!a) return;
        if (a->count > 64 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->count * sizeof(D3DRECT)) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        x.dev->Clear(a->count, a->count ? (const D3DRECT *)tail(a) : nullptr, a->flags, a->color, a->z, a->stencil);
        break;
    }
    case D3DPT_OP_BEGIN_SCENE: if (need_device(b)) x.dev->BeginScene(); break;
    case D3DPT_OP_END_SCENE:   if (need_device(b)) x.dev->EndScene(); break;

    case D3DPT_OP_SET_RENDER_STATE: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetRenderState((D3DRENDERSTATETYPE)a->a, a->b);
        break;
    }
    case D3DPT_OP_SET_FVF: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetFVF(a->a);
        break;
    }
    case D3DPT_OP_SET_VIEWPORT: {
        auto *a = body<d3dpt_viewport>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetViewport((const D3DVIEWPORT9 *)a);
        break;
    }
    case D3DPT_OP_SET_TRANSFORM: {
        auto *a = body<d3dpt_transform>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetTransform((D3DTRANSFORMSTATETYPE)a->state, (const D3DMATRIX *)a->m);
        break;
    }
    case D3DPT_OP_SET_TEXTURE_STAGE_STATE: {
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetTextureStageState(a->a, (D3DTEXTURESTAGESTATETYPE)a->b, a->c);
        break;
    }
    case D3DPT_OP_SET_SAMPLER_STATE: {
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetSamplerState(a->a, (D3DSAMPLERSTATETYPE)a->b, a->c);
        break;
    }
    case D3DPT_OP_SET_LIGHT: {
        auto *a = body<d3dpt_light>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetLight(a->index, (const D3DLIGHT9 *)a->light);
        break;
    }
    case D3DPT_OP_LIGHT_ENABLE: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->LightEnable(a->a, a->b ? TRUE : FALSE);
        break;
    }
    case D3DPT_OP_SET_MATERIAL: {
        auto *a = body<d3dpt_material>(c, 0, b); if (!a || !need_device(b)) return;
        x.dev->SetMaterial((const D3DMATERIAL9 *)a->material);
        break;
    }
    case D3DPT_OP_SET_TEXTURE: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DTexture9 *t = nullptr;
        if (a->b && !(t = x.get<IDirect3DTexture9>(a->b, K_TEX))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetTexture(a->a, t);
        break;
    }
    case D3DPT_OP_SET_STREAM_SOURCE: {
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DVertexBuffer9 *vb = nullptr;
        if (a->b && !(vb = x.get<IDirect3DVertexBuffer9>(a->b, K_VB))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetStreamSource(a->a, vb, a->c, a->d);
        break;
    }
    case D3DPT_OP_SET_INDICES: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DIndexBuffer9 *ib = nullptr;
        if (a->a && !(ib = x.get<IDirect3DIndexBuffer9>(a->a, K_IB))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetIndices(ib);
        break;
    }
    case D3DPT_OP_SET_VERTEX_SHADER: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DVertexShader9 *s = nullptr;
        if (a->a && !(s = x.get<IDirect3DVertexShader9>(a->a, K_VS))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetVertexShader(s);
        break;
    }
    case D3DPT_OP_SET_PIXEL_SHADER: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DPixelShader9 *s = nullptr;
        if (a->a && !(s = x.get<IDirect3DPixelShader9>(a->a, K_PS))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetPixelShader(s);
        break;
    }
    case D3DPT_OP_SET_VS_CONST_F:
    case D3DPT_OP_SET_PS_CONST_F: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        if (a->b > 256 || a->a > 256 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->b * 16) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        if (c->op == D3DPT_OP_SET_VS_CONST_F) x.dev->SetVertexShaderConstantF(a->a, (const float *)tail(a), a->b);
        else x.dev->SetPixelShaderConstantF(a->a, (const float *)tail(a), a->b);
        break;
    }
    case D3DPT_OP_SET_RENDER_TARGET: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        if (a->a > 3) { b.err = D3DPT_ERR_BAD_ARG; return; }
        IDirect3DSurface9 *s = nullptr;
        if (a->b) {
            if (!(s = x.get<IDirect3DSurface9>(a->b, K_SURF))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
            x.dev->SetRenderTarget(a->a, s);
        } else if (a->a == 0) {
            if (SUCCEEDED(x.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) { x.dev->SetRenderTarget(0, s); s->Release(); }
        } else {
            x.dev->SetRenderTarget(a->a, nullptr);
        }
        break;
    }
    case D3DPT_OP_SET_DEPTH_STENCIL: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DSurface9 *s = nullptr;
        if (a->a && !(s = x.get<IDirect3DSurface9>(a->a, K_SURF))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetDepthStencilSurface(s);
        break;
    }

    case D3DPT_OP_SET_SCISSOR_RECT: {
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a || !need_device(b)) return;
        RECT rc = { (LONG)a->a, (LONG)a->b, (LONG)a->c, (LONG)a->d };
        x.dev->SetScissorRect(&rc);
        break;
    }
    case D3DPT_OP_DRAW_PRIMITIVE: {
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a || !need_device(b)) return;
        bool ok; prim_vertices(a->a, a->c, ok);
        if (!ok) { b.err = D3DPT_ERR_BAD_ARG; return; }
        x.dev->DrawPrimitive((D3DPRIMITIVETYPE)a->a, a->b, a->c);
        break;
    }
    case D3DPT_OP_DRAW_INDEXED_PRIMITIVE: {
        auto *a = body<d3dpt_draw_indexed>(c, 0, b); if (!a || !need_device(b)) return;
        bool ok; prim_vertices(a->type, a->prim_count, ok);
        if (!ok) { b.err = D3DPT_ERR_BAD_ARG; return; }
        x.dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)a->type, (INT)a->base_vertex, a->min_index, a->num_vertices, a->start_index, a->prim_count);
        break;
    }
    case D3DPT_OP_DRAW_PRIMITIVE_UP: {
        auto *a = body<d3dpt_draw_up>(c, 0, b); if (!a) return;
        bool ok; uint32_t nv = prim_vertices(a->type, a->prim_count, ok);
        if (!ok || a->stride == 0 || a->stride > 1024 || (uint64_t)nv * a->stride != a->bytes ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        x.dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)a->type, a->prim_count, tail(a), a->stride);
        break;
    }
    case D3DPT_OP_DRAW_INDEXED_PRIMITIVE_UP: {
        auto *a = body<d3dpt_draw_indexed_up>(c, 0, b); if (!a) return;
        bool ok; uint32_t ni = prim_vertices(a->type, a->prim_count, ok);
        uint32_t isz = a->index_format == D3DFMT_INDEX32 ? 4 : a->index_format == D3DFMT_INDEX16 ? 2 : 0;
        uint64_t idx_aligned = D3DPT_ALIGN8(a->index_bytes);
        if (!ok || !isz || (uint64_t)ni * isz != a->index_bytes || a->stride == 0 || a->stride > 1024 ||
            (uint64_t)a->num_vertices * a->stride != a->vertex_bytes || a->num_vertices == 0 ||
            (uint64_t)a->min_index + a->num_vertices > (1u << 24) ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + idx_aligned + a->vertex_bytes) { b.err = D3DPT_ERR_BAD_ARG; return; }
        /* every index must address a vertex we were given */
        const uint8_t *idx = tail(a);
        for (uint32_t i = 0; i < ni; i++) {
            uint32_t v = isz == 2 ? ((const uint16_t *)idx)[i] : ((const uint32_t *)idx)[i];
            if (v < a->min_index || v >= a->min_index + a->num_vertices) { b.err = D3DPT_ERR_BAD_ARG; return; }
        }
        if (!need_device(b)) return;
        x.dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)a->type, a->min_index, a->num_vertices, a->prim_count,
                                      idx, (D3DFORMAT)a->index_format, idx + idx_aligned, a->stride);
        break;
    }

    case D3DPT_OP_CREATE_VERTEX_BUFFER:
    case D3DPT_OP_CREATE_INDEX_BUFFER: {
        auto *a = body<d3dpt_create_buffer>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        if (a->length == 0 || a->length > (64u << 20)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }
        DWORD usage = a->usage;
        D3DPOOL pool = (D3DPOOL)a->pool;
        if (c->op == D3DPT_OP_CREATE_VERTEX_BUFFER) {
            IDirect3DVertexBuffer9 *vb = nullptr;
            r->hr = (uint32_t)x.dev->CreateVertexBuffer(a->length, usage, a->fvf_or_format, pool, &vb, nullptr);
            if (SUCCEEDED(r->hr) && !x.put(a->handle, K_VB, vb)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        } else {
            IDirect3DIndexBuffer9 *ib = nullptr;
            r->hr = (uint32_t)x.dev->CreateIndexBuffer(a->length, usage, (D3DFORMAT)a->fvf_or_format, pool, &ib, nullptr);
            if (SUCCEEDED(r->hr) && !x.put(a->handle, K_IB, ib)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        }
        break;
    }
    case D3DPT_OP_CREATE_TEXTURE: {
        auto *a = body<d3dpt_create_texture>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        if (!a->width || !a->height || a->width > 8192 || a->height > 8192 || a->levels > 16) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }
        IDirect3DTexture9 *t = nullptr;
        r->hr = (uint32_t)x.dev->CreateTexture(a->width, a->height, a->levels, a->usage, (D3DFORMAT)a->format, (D3DPOOL)a->pool, &t, nullptr);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_TEX, t)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_CREATE_DEPTH_STENCIL:
    case D3DPT_OP_CREATE_RENDER_TARGET: {
        auto *a = body<d3dpt_create_texture>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        if (!a->width || !a->height || a->width > 8192 || a->height > 8192) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }
        IDirect3DSurface9 *s = nullptr;
        if (c->op == D3DPT_OP_CREATE_DEPTH_STENCIL)
            r->hr = (uint32_t)x.dev->CreateDepthStencilSurface(a->width, a->height, (D3DFORMAT)a->format, (D3DMULTISAMPLE_TYPE)a->multisample, a->ms_quality, a->lockable ? TRUE : FALSE, &s, nullptr);
        else
            r->hr = (uint32_t)x.dev->CreateRenderTarget(a->width, a->height, (D3DFORMAT)a->format, (D3DMULTISAMPLE_TYPE)a->multisample, a->ms_quality, a->lockable ? TRUE : FALSE, &s, nullptr);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_SURF, s)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_GET_SURFACE: {
        auto *a = body<d3dpt_get_surface>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        IDirect3DSurface9 *s = nullptr;
        if (a->texture == 0) {
            r->hr = a->level == 0 ? (uint32_t)x.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)
                                  : (uint32_t)x.dev->GetDepthStencilSurface(&s);
        } else {
            IDirect3DTexture9 *t = x.get<IDirect3DTexture9>(a->texture, K_TEX);
            if (!t) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
            r->hr = (uint32_t)t->GetSurfaceLevel(a->level, &s);
        }
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_SURF, s)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_BUFFER_UPDATE: {
        auto *a = body<d3dpt_update>(c, 0, b); if (!a) return;
        if (a->bytes == 0 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes) { b.err = D3DPT_ERR_BAD_ARG; return; }
        auto it = x.objs.find(a->handle);
        if (it == x.objs.end() || (it->second.kind != K_VB && it->second.kind != K_IB)) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        void *p = nullptr;
        HRESULT hr;
        if (it->second.kind == K_VB) {
            auto *vb = static_cast<IDirect3DVertexBuffer9 *>(it->second.p);
            D3DVERTEXBUFFER_DESC d; vb->GetDesc(&d);
            if ((uint64_t)a->offset + a->bytes > d.Size) { b.err = D3DPT_ERR_BAD_ARG; return; }
            hr = vb->Lock(a->offset, a->bytes, &p, a->flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
            if (SUCCEEDED(hr) && p) { memcpy(p, tail(a), a->bytes); vb->Unlock(); }
        } else {
            auto *ib = static_cast<IDirect3DIndexBuffer9 *>(it->second.p);
            D3DINDEXBUFFER_DESC d; ib->GetDesc(&d);
            if ((uint64_t)a->offset + a->bytes > d.Size) { b.err = D3DPT_ERR_BAD_ARG; return; }
            hr = ib->Lock(a->offset, a->bytes, &p, a->flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
            if (SUCCEEDED(hr) && p) { memcpy(p, tail(a), a->bytes); ib->Unlock(); }
        }
        if (FAILED(hr)) x.log("buffer update: Lock 0x%08x", (unsigned)hr);
        break;
    }
    case D3DPT_OP_TEXTURE_UPDATE: {
        auto *a = body<d3dpt_tex_update>(c, 0, b); if (!a) return;
        if (a->bytes == 0 || a->pitch == 0 || a->bytes % a->pitch || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes ||
            !a->w || !a->h) { b.err = D3DPT_ERR_BAD_ARG; return; }
        IDirect3DTexture9 *t = x.get<IDirect3DTexture9>(a->handle, K_TEX);
        if (!t) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        D3DSURFACE_DESC d;
        if (FAILED(t->GetLevelDesc(a->level, &d)) || (uint64_t)a->x + a->w > d.Width || (uint64_t)a->y + a->h > d.Height) { b.err = D3DPT_ERR_BAD_ARG; return; }
        RECT rc = { (LONG)a->x, (LONG)a->y, (LONG)(a->x + a->w), (LONG)(a->y + a->h) };
        D3DLOCKED_RECT lr;
        HRESULT hr = t->LockRect(a->level, &lr, &rc, 0);
        if (SUCCEEDED(hr)) {
            uint32_t rows = a->bytes / a->pitch;
            uint32_t row = a->pitch < (uint32_t)lr.Pitch ? a->pitch : (uint32_t)lr.Pitch;
            /* rows the locked box actually has (blocks for compressed formats) */
            uint32_t maxrows = a->h;
            if (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT2 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT4 || d.Format == D3DFMT_DXT5)
                maxrows = (a->h + 3) / 4;
            if (rows > maxrows) rows = maxrows;
            const uint8_t *src = tail(a);
            for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, src + (size_t)y * a->pitch, row);
            t->UnlockRect(a->level);
        } else x.log("texture update: LockRect 0x%08x", (unsigned)hr);
        break;
    }
    case D3DPT_OP_CREATE_VERTEX_SHADER:
    case D3DPT_OP_CREATE_PIXEL_SHADER: {
        auto *a = body<d3dpt_create_shader>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (a->bytes < 8 || a->bytes % 4 || a->bytes > (256u << 10) || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        const DWORD *code = (const DWORD *)tail(a);
        if (code[(a->bytes / 4) - 1] != 0x0000ffff) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }   /* must end with END */
        if (c->op == D3DPT_OP_CREATE_VERTEX_SHADER) {
            IDirect3DVertexShader9 *s = nullptr;
            r->hr = (uint32_t)x.dev->CreateVertexShader(code, &s);
            if (SUCCEEDED(r->hr) && !x.put(a->handle, K_VS, s)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        } else {
            IDirect3DPixelShader9 *s = nullptr;
            r->hr = (uint32_t)x.dev->CreatePixelShader(code, &s);
            if (SUCCEEDED(r->hr) && !x.put(a->handle, K_PS, s)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        }
        break;
    }
    case D3DPT_OP_GET_RENDER_TARGET_DATA: {
        auto *a = body<d3dpt_sync>(c, 0, b); if (!a) return;
        if (!need_device(b)) return;
        IDirect3DSurface9 *s = x.get<IDirect3DSurface9>(a->handle, K_SURF);
        if (!s) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        D3DSURFACE_DESC d; s->GetDesc(&d);
        uint32_t bpp = (d.Format == D3DFMT_R5G6B5 || d.Format == D3DFMT_X1R5G5B5 || d.Format == D3DFMT_A1R5G5B5) ? 2 : 4;
        uint64_t bytes = (uint64_t)d.Width * d.Height * bpp;
        if (bytes > D3DPT_RET_SIZE) { b.err = D3DPT_ERR_BAD_ARG; return; }
        d3dpt_ret *r = b.slot(a->ret_off, (uint32_t)bytes); if (!r) return;
        IDirect3DSurface9 *tmp = nullptr;
        r->hr = (uint32_t)x.dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr);
        if (SUCCEEDED(r->hr)) {
            r->hr = (uint32_t)x.dev->GetRenderTargetData(s, tmp);
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(r->hr) && SUCCEEDED(tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                uint8_t *dst = (uint8_t *)(r + 1);
                for (uint32_t y = 0; y < d.Height; y++) memcpy(dst + (size_t)y * d.Width * bpp, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (size_t)d.Width * bpp);
                tmp->UnlockRect();
                r->bytes = (uint32_t)bytes;
            }
            tmp->Release();
        }
        break;
    }
    case D3DPT_OP_STRETCH_RECT: {
        auto *a = body<d3dpt_stretch_rect>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DSurface9 *src = x.get<IDirect3DSurface9>(a->src, K_SURF), *dst = x.get<IDirect3DSurface9>(a->dst, K_SURF);
        if (!src || !dst) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        RECT sr = { a->src_rect[0], a->src_rect[1], a->src_rect[2], a->src_rect[3] };
        RECT dr = { a->dst_rect[0], a->dst_rect[1], a->dst_rect[2], a->dst_rect[3] };
        x.dev->StretchRect(src, a->has_rects & 1 ? &sr : nullptr, dst, a->has_rects & 2 ? &dr : nullptr, (D3DTEXTUREFILTERTYPE)a->filter);
        break;
    }

    default:
        b.err = D3DPT_ERR_BAD_OP;
        break;
    }
}

} // namespace

extern "C" {

uint32_t d3dpt_exec_version(void) { return D3DPT_PROTO_VERSION; }

d3dpt_exec_t *d3dpt_exec_create(const d3dpt_exec_ops *ops)
{
    Exec *x = new Exec;
    x->ops = *ops;
    const char *lib = getenv("D3DPT_DXVK_LIB");
    const char *candidates[] = { lib, "build/dxvk/src/d3d9/libdxvk_d3d9.so.0",
#ifdef __APPLE__
        "build/dxvk/src/d3d9/libdxvk_d3d9.0.dylib", "libdxvk_d3d9.0.dylib",
#else
        "libdxvk_d3d9.so.0",
#endif
        nullptr };
    for (const char **c = candidates; *c || c == candidates; c++) {
        if (!*c) continue;
        x->dxvk = dlopen(*c, RTLD_NOW | RTLD_LOCAL);
        if (x->dxvk) { x->log("DXVK d3d9: %s", *c); break; }
    }
    if (!x->dxvk) { x->log("DXVK d3d9 library not found (D3DPT_DXVK_LIB)"); delete x; return nullptr; }
    setenv("DXVK_WSI_DRIVER", "Headless", 0);
    auto create = (IDirect3D9 *(*)(UINT))dlsym(x->dxvk, "Direct3DCreate9");
    if (!create) { x->log("no Direct3DCreate9 in the DXVK library"); dlclose(x->dxvk); delete x; return nullptr; }
    try { x->d3d = create(D3D_SDK_VERSION); } catch (...) { x->d3d = nullptr; }
    if (!x->d3d) { x->log("Direct3DCreate9 failed: no usable Vulkan device"); dlclose(x->dxvk); delete x; return nullptr; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(x->d3d->GetAdapterIdentifier(0, 0, &id))) x->log("adapter \"%s\"", id.Description);
    return (d3dpt_exec_t *)x;
}

void d3dpt_exec_destroy(d3dpt_exec_t *xp)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    x->release_all();
    if (x->d3d) x->d3d->Release();
    /* DXVK keeps worker threads; leave the library mapped */
    delete x;
}

void d3dpt_exec_attach(d3dpt_exec_t *xp, int attach)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    if (attach) x->attach++;
    else if (x->attach > 0 && --x->attach == 0) x->release_all();
}

uint32_t d3dpt_exec_submit(d3dpt_exec_t *xp, void *shm, uint32_t shm_size)
{
    Exec *x = (Exec *)xp;
    if (!x || shm_size < D3DPT_SHM_SIZE) return D3DPT_ERR_HOST;
    d3dpt_shm_hdr *hdr = (d3dpt_shm_hdr *)shm;
    Batch b = { *x, (uint8_t *)shm, hdr, (uint8_t *)shm + D3DPT_RET_OFFSET };
    uint32_t bytes = hdr->cmd_bytes, count = hdr->cmd_count;
    hdr->batches++;
    if (bytes > D3DPT_CMD_SIZE || bytes % 8) { b.err = D3DPT_ERR_MALFORMED; }
    const uint8_t *p = (const uint8_t *)shm + D3DPT_CMD_OFFSET, *end = p + (b.err ? 0 : bytes);
    while (!b.err && p < end) {
        if (end - p < (ptrdiff_t)sizeof(d3dpt_cmd)) { b.err = D3DPT_ERR_MALFORMED; break; }
        const d3dpt_cmd *c = (const d3dpt_cmd *)p;
        if (c->size < sizeof(d3dpt_cmd) || c->size % 8 || c->size > (uint32_t)(end - p)) { b.err = D3DPT_ERR_MALFORMED; break; }
        exec_one(b, c);
        p += c->size;
        b.index++;
    }
    if (!b.err && b.index != count) b.err = D3DPT_ERR_MALFORMED;
    if (b.err) x->log("batch error %u at record %u (op %u), %u bytes %u records", b.err, b.index,
                      b.index < count && p < end ? ((const d3dpt_cmd *)p)->op : 0, bytes, count);
    hdr->ret_status = b.err;
    hdr->ret_index = b.index;
    hdr->cmd_bytes = 0;
    hdr->cmd_count = 0;
    return b.err;
}

} // extern "C"
