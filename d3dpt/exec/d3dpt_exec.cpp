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
#include <dlfcn.h>
#include <cstdlib>
#include <string>
#include <exception>

#include "d3dpt_exec_int.h"

static_assert(sizeof(D3DCAPS9) == D3DPT_SIZEOF_CAPS9, "D3DCAPS9 layout");
static_assert(sizeof(D3DLIGHT9) == sizeof(((d3dpt_light *)0)->light), "D3DLIGHT9 layout");
static_assert(sizeof(D3DMATERIAL9) == sizeof(((d3dpt_material *)0)->material), "D3DMATERIAL9 layout");
static_assert(sizeof(D3DVIEWPORT9) == sizeof(d3dpt_viewport), "D3DVIEWPORT9 layout");
static_assert(sizeof(D3DRECT) == 16, "D3DRECT layout");

namespace {

using namespace d3dpt;

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

/* D3DPT_DUMP_DIR=dir writes every D3DPT_DUMP_EVERY-th presented frame (default
 * 60) as dir/frame-NNNNNN.ppm: the only way to see a game's frames from a
 * bare qemu-system-i386 (a QMP screendump shows the VGA surface, which is
 * frozen while the device presents). */
static void dump_frame(const uint32_t *px, int w, int h, int pitch) {
    static const char *dir = getenv("D3DPT_DUMP_DIR");
    static unsigned every = getenv("D3DPT_DUMP_EVERY") ? (unsigned)atoi(getenv("D3DPT_DUMP_EVERY")) : 60;
    static unsigned n;
    if (!dir || !*dir || !every || n++ % every) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame-%06u.ppm", dir, n - 1);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)((const uint8_t *)px + (size_t)y * pitch);
        for (int i = 0; i < w; i++) { row[i * 3] = s[i] >> 16; row[i * 3 + 1] = s[i] >> 8; row[i * 3 + 2] = s[i]; }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
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
            dump_frame((const uint32_t *)lr.pBits, (int)d.Width, (int)d.Height, lr.Pitch);
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
            dump_frame(x.conv.data(), (int)d.Width, (int)d.Height, (int)d.Width * 4);
        }
    }
    x.sys->UnlockRect();
}

/* Depth formats real 2001 cards offered but DXVK's D3D9 refuses outright
 * (d3d9_format.cpp: D32 / D15S1 / D24X4S4 "Unsupported (everywhere)").
 * The guest DLL advertises them in CheckDeviceFormat / CheckDepthStencilMatch
 * — Max Payne picks D32 for 32-bit modes and got D3DERR_NOTAVAILABLE from
 * CreateDevice — so keep the promise with the closest layout DXVK has. The
 * guest keeps answering GetDesc with the format the game asked for. */
static D3DFORMAT depth_norm(uint32_t f) {
    switch (f) {
    case D3DFMT_D32:     return D3DFMT_D24X8;
    case D3DFMT_D15S1:   return D3DFMT_D24S8;
    case D3DFMT_D24X4S4: return D3DFMT_D24S8;
    default:             return (D3DFORMAT)f;
    }
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
    pp.AutoDepthStencilFormat = depth_norm(g.depth_format);
    pp.Flags = g.flags & ~(DWORD)D3DPRESENTFLAG_DEVICECLIP;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   /* pacing is the guest's / the player's */
}

static void exec_one(Batch &b, const d3dpt_cmd *c) {
    Exec &x = b.x;
    switch (c->op) {
    case D3DPT_OP_NOP:
        break;
    case D3DPT_OP_LOG: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        if (a->a > 512 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->a) { b.err = D3DPT_ERR_BAD_ARG; return; }
        char buf[513];
        memcpy(buf, tail(a), a->a); buf[a->a] = 0;
        for (char *p = buf; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        x.log("guest: %s", buf);
        break;
    }

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
        IDirect3DBaseTexture9 *t = nullptr;
        if (a->b) {
            auto it = x.objs.find(a->b);
            if (it == x.objs.end() || (it->second.kind != K_TEX && it->second.kind != K_CUBE)) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
            t = static_cast<IDirect3DBaseTexture9 *>(it->second.p);
        }
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
    case D3DPT_OP_SET_VS_CONST_I:
    case D3DPT_OP_SET_PS_CONST_I: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        if (a->b > 16 || a->a > 16 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->b * 16) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        if (c->op == D3DPT_OP_SET_VS_CONST_I) x.dev->SetVertexShaderConstantI(a->a, (const int *)tail(a), a->b);
        else x.dev->SetPixelShaderConstantI(a->a, (const int *)tail(a), a->b);
        break;
    }
    case D3DPT_OP_SET_VS_CONST_B:
    case D3DPT_OP_SET_PS_CONST_B: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        if (a->b > 16 || a->a > 16 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->b * 4) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        if (c->op == D3DPT_OP_SET_VS_CONST_B) x.dev->SetVertexShaderConstantB(a->a, (const BOOL *)tail(a), a->b);
        else x.dev->SetPixelShaderConstantB(a->a, (const BOOL *)tail(a), a->b);
        break;
    }
    case D3DPT_OP_SET_CLIP_PLANE: {
        auto *a = body<d3dpt_clip_plane>(c, 0, b); if (!a || !need_device(b)) return;
        if (a->index > 5) { b.err = D3DPT_ERR_BAD_ARG; return; }
        x.dev->SetClipPlane(a->index, a->plane);
        break;
    }
    case D3DPT_OP_SET_VERTEX_DECL: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DVertexDeclaration9 *d = nullptr;
        if (a->a && !(d = x.get<IDirect3DVertexDeclaration9>(a->a, K_DECL))) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        x.dev->SetVertexDeclaration(d);
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
    case D3DPT_OP_CREATE_CUBE_TEXTURE: {
        auto *a = body<d3dpt_create_texture>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        if (!a->width || a->width > 8192 || a->levels > 16) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }
        IDirect3DCubeTexture9 *t = nullptr;
        r->hr = (uint32_t)x.dev->CreateCubeTexture(a->width, a->levels, a->usage, (D3DFORMAT)a->format, (D3DPOOL)a->pool, &t, nullptr);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_CUBE, t)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_CREATE_OFFSCREEN: {
        auto *a = body<d3dpt_create_texture>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        if (!a->width || !a->height || a->width > 8192 || a->height > 8192) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }
        IDirect3DSurface9 *s = nullptr;
        r->hr = (uint32_t)x.dev->CreateOffscreenPlainSurface(a->width, a->height, (D3DFORMAT)a->format, (D3DPOOL)a->pool, &s, nullptr);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_SURF, s)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_CREATE_VERTEX_DECL: {
        auto *a = body<d3dpt_create_shader>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (a->bytes < 8 || a->bytes % 8 || a->bytes > 8 * 65 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes) { b.err = D3DPT_ERR_BAD_ARG; return; }
        if (!need_device(b)) return;
        const D3DVERTEXELEMENT9 *el = (const D3DVERTEXELEMENT9 *)tail(a);
        if (el[a->bytes / 8 - 1].Stream != 0xFF) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return; }   /* must end with D3DDECL_END */
        IDirect3DVertexDeclaration9 *d = nullptr;
        r->hr = (uint32_t)x.dev->CreateVertexDeclaration(el, &d);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_DECL, d)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_CREATE_QUERY: {
        auto *a = body<d3dpt_create_query>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        IDirect3DQuery9 *q = nullptr;
        r->hr = (uint32_t)x.dev->CreateQuery((D3DQUERYTYPE)a->type, &q);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_QUERY, q)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_QUERY_ISSUE: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        IDirect3DQuery9 *q = x.get<IDirect3DQuery9>(a->a, K_QUERY);
        if (!q) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        q->Issue(a->b & (D3DISSUE_BEGIN | D3DISSUE_END));
        break;
    }
    case D3DPT_OP_QUERY_GET_DATA: {
        auto *a = body<d3dpt_query_get>(c, 0, b); if (!a) return;
        if (a->size > 256) { b.err = D3DPT_ERR_BAD_ARG; return; }
        d3dpt_ret *r = b.slot(a->ret_off, a->size); if (!r) return;
        IDirect3DQuery9 *q = x.get<IDirect3DQuery9>(a->handle, K_QUERY);
        if (!q) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        r->hr = (uint32_t)q->GetData(a->size ? (void *)(r + 1) : nullptr, a->size, a->flags & D3DGETDATA_FLUSH);
        r->bytes = r->hr == S_OK ? a->size : 0;
        break;
    }
    case D3DPT_OP_COLOR_FILL: {
        auto *a = body<d3dpt_color_fill>(c, 0, b); if (!a || !need_device(b)) return;
        IDirect3DSurface9 *s = x.get<IDirect3DSurface9>(a->handle, K_SURF);
        if (!s) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        RECT rc = { a->rect[0], a->rect[1], a->rect[2], a->rect[3] };
        x.dev->ColorFill(s, a->has_rect ? &rc : nullptr, a->color);
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
            r->hr = (uint32_t)x.dev->CreateDepthStencilSurface(a->width, a->height, depth_norm(a->format), (D3DMULTISAMPLE_TYPE)a->multisample, a->ms_quality, a->lockable ? TRUE : FALSE, &s, nullptr);
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
            auto it = x.objs.find(a->texture);
            if (it == x.objs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
            if (it->second.kind == K_TEX) r->hr = (uint32_t)static_cast<IDirect3DTexture9 *>(it->second.p)->GetSurfaceLevel(a->level & 0xff, &s);
            else if (it->second.kind == K_CUBE) r->hr = (uint32_t)static_cast<IDirect3DCubeTexture9 *>(it->second.p)->GetCubeMapSurface((D3DCUBEMAP_FACES)((a->level >> 8) & 0xf), a->level & 0xff, &s);
            else { b.err = D3DPT_ERR_BAD_HANDLE; return; }
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
    case D3DPT_OP_TEXTURE_UPDATE:
    case D3DPT_OP_SURFACE_UPDATE: {
        auto *a = body<d3dpt_tex_update>(c, 0, b); if (!a) return;
        if (a->bytes == 0 || a->pitch == 0 || a->bytes % a->pitch || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->bytes ||
            !a->w || !a->h) { b.err = D3DPT_ERR_BAD_ARG; return; }
        auto it = x.objs.find(a->handle);
        if (it == x.objs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        HRESULT hr;
        RECT rc = { (LONG)a->x, (LONG)a->y, (LONG)(a->x + a->w), (LONG)(a->y + a->h) };
        IDirect3DTexture9 *t = nullptr; IDirect3DCubeTexture9 *ct = nullptr; IDirect3DSurface9 *sf = nullptr;
        uint32_t level = a->level & 0xff; D3DCUBEMAP_FACES face = (D3DCUBEMAP_FACES)((a->level >> 8) & 0xf);
        if (c->op == D3DPT_OP_TEXTURE_UPDATE && it->second.kind == K_TEX) { t = static_cast<IDirect3DTexture9 *>(it->second.p); hr = t->GetLevelDesc(level, &d); }
        else if (c->op == D3DPT_OP_TEXTURE_UPDATE && it->second.kind == K_CUBE) { ct = static_cast<IDirect3DCubeTexture9 *>(it->second.p); hr = ct->GetLevelDesc(level, &d); }
        else if (c->op == D3DPT_OP_SURFACE_UPDATE && it->second.kind == K_SURF) { sf = static_cast<IDirect3DSurface9 *>(it->second.p); hr = sf->GetDesc(&d); }
        else { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        if (FAILED(hr) || (uint64_t)a->x + a->w > d.Width || (uint64_t)a->y + a->h > d.Height) { b.err = D3DPT_ERR_BAD_ARG; return; }
        uint32_t rows = a->bytes / a->pitch;
        uint32_t maxrows = a->h;
        bool dxt = d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT2 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT4 || d.Format == D3DFMT_DXT5;
        if (dxt) maxrows = (a->h + 3) / 4;
        if (rows > maxrows) rows = maxrows;
        const uint8_t *src = tail(a);
        auto copy_rows = [&](void *bits, INT pitch) {
            uint32_t row = a->pitch < (uint32_t)pitch ? a->pitch : (uint32_t)pitch;
            for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)bits + (size_t)y * pitch, src + (size_t)y * a->pitch, row);
        };
        hr = t ? t->LockRect(level, &lr, &rc, 0) : ct ? ct->LockRect(face, level, &lr, &rc, 0) : sf->LockRect(&lr, &rc, 0);
        if (SUCCEEDED(hr)) {
            copy_rows(lr.pBits, lr.Pitch);
            if (t) t->UnlockRect(level); else if (ct) ct->UnlockRect(face, level); else sf->UnlockRect();
        } else {
            /* DEFAULT-pool textures are not lockable: stage through a system-memory surface */
            IDirect3DSurface9 *dst = nullptr, *tmp = nullptr;
            if (t) t->GetSurfaceLevel(level, &dst); else if (ct) ct->GetCubeMapSurface(face, level, &dst); else { dst = sf; dst->AddRef(); }
            hr = x.dev->CreateOffscreenPlainSurface(a->w, a->h, d.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr);
            if (SUCCEEDED(hr) && dst) {
                if (SUCCEEDED(tmp->LockRect(&lr, nullptr, 0))) { copy_rows(lr.pBits, lr.Pitch); tmp->UnlockRect(); }
                POINT pt = { (LONG)a->x, (LONG)a->y };
                hr = x.dev->UpdateSurface(tmp, nullptr, dst, &pt);
            }
            if (FAILED(hr)) x.log("%s update: staged upload 0x%08x", t ? "texture" : ct ? "cube" : "surface", (unsigned)hr);
            if (tmp) tmp->Release();
            if (dst) dst->Release();
        }
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
        if (!exec_ddi_op(b, c)) b.err = D3DPT_ERR_BAD_OP;
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

void d3dpt_exec_set_vram(d3dpt_exec_t *xp, void *vram, uint32_t size)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    x->vram = (uint8_t *)vram;
    x->vram_size = vram ? size : 0;
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
        /* a record must never take the process down: an exception (a
         * std::bad_alloc out of DXVK on a garbage count, say) refuses the
         * batch like a malformed record does */
        try { exec_one(b, c); }
        catch (const std::exception &e) { x->log("record %u (op %u) threw: %s", b.index, c->op, e.what()); if (!b.err) b.err = D3DPT_ERR_HOST; }
        catch (...) { x->log("record %u (op %u) threw", b.index, c->op); if (!b.err) b.err = D3DPT_ERR_HOST; }
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
