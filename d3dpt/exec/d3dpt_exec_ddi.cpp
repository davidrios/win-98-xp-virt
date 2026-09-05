/*
 * d3dpt_exec_ddi.cpp — the display driver's records of the paravirtual
 * Direct3D executor (doc 15, M7c): what the XP display driver
 * (guest-tools/src/d3dptvid/d3dptdisp.c) sends through the d3dpt-vga
 * adapter's command window when dxg.sys drives its Direct3D DDI.
 *
 * The model: every DirectDraw surface stays in guest VRAM (dxg's heap),
 * the host mirrors the ones Direct3D touches. A VRAM_SURFACE record
 * registers a surface by the runtime's handle with its VRAM offset,
 * size, pitch and D3DFORMAT; textures become DXVK textures whose texels
 * are read straight from the VRAM pointer (re-read after VRAM_DIRTY),
 * render targets and Z buffers become host surfaces. A CTX_CREATE binds
 * a render target + Z pair as a Direct3D context; a DP2 record carries
 * one D3dDrawPrimitives2 call (the DX7 token stream plus the vertex
 * buffer) and is interpreted on IDirect3DDevice9; READBACK copies the
 * host render target back into the surface's VRAM so the guest's flips,
 * HEL blits and Lock reads see the frame.
 *
 * First cut = the DX7 non-T&L HAL: the runtime transforms and lights, the
 * vertices arrive as XYZRHW, so the tokens needed are RENDERSTATE,
 * TEXTURESTAGESTATE, VIEWPORTINFO, ZRANGE, SETRENDERTARGET, CLEAR and the
 * primitive tokens. The T&L tokens (SETTRANSFORM / SETLIGHT / SETMATERIAL)
 * are mapped 1:1 onto d3d9 already so claiming T&L is a caps change.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt_exec_int.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>

namespace d3dpt {

namespace {

/* DP2 tokens (D3DHAL_DP2OPERATION of the DDK's d3dhal.h) */
enum {
    DP2_POINTS = 1, DP2_INDEXEDLINELIST = 2, DP2_INDEXEDTRIANGLELIST = 3, DP2_RENDERSTATE = 8,
    DP2_LINELIST = 15, DP2_LINESTRIP = 16, DP2_INDEXEDLINESTRIP = 17, DP2_TRIANGLELIST = 18,
    DP2_TRIANGLESTRIP = 19, DP2_INDEXEDTRIANGLESTRIP = 20, DP2_TRIANGLEFAN = 21,
    DP2_INDEXEDTRIANGLEFAN = 22, DP2_TRIANGLEFAN_IMM = 23, DP2_LINELIST_IMM = 24,
    DP2_TEXTURESTAGESTATE = 25, DP2_INDEXEDTRIANGLELIST2 = 26, DP2_INDEXEDLINELIST2 = 27,
    DP2_VIEWPORTINFO = 28, DP2_WINFO = 29, DP2_SETPALETTE = 30, DP2_UPDATEPALETTE = 31,
    DP2_ZRANGE = 32, DP2_SETMATERIAL = 33, DP2_SETLIGHT = 34, DP2_CREATELIGHT = 35,
    DP2_SETTRANSFORM = 36, DP2_TEXBLT = 38, DP2_STATESET = 39, DP2_SETPRIORITY = 40,
    DP2_SETRENDERTARGET = 41, DP2_CLEAR = 42, DP2_SETTEXLOD = 43, DP2_SETCLIPPLANE = 44,
    /* DX8 (the display driver rewrites the draws into D3DPT_DP2_DRAW8; the
     * rest is parsed for its size and dropped, or mapped when d3d9 has it) */
    DP2_CREATEVERTEXSHADER = 45, DP2_DELETEVERTEXSHADER = 46, DP2_SETVERTEXSHADER = 47,
    DP2_SETVERTEXSHADERCONST = 48, DP2_SETSTREAMSOURCE = 49, DP2_SETSTREAMSOURCEUM = 50, DP2_SETINDICES = 51,
    DP2_DRAWPRIMITIVE = 52, DP2_DRAWINDEXEDPRIMITIVE = 53, DP2_CREATEPIXELSHADER = 54, DP2_DELETEPIXELSHADER = 55,
    DP2_SETPIXELSHADER = 56, DP2_SETPIXELSHADERCONST = 57, DP2_CLIPPEDTRIANGLEFAN = 58, DP2_DRAWPRIMITIVE2 = 59,
    DP2_DRAWINDEXEDPRIMITIVE2 = 60, DP2_DRAWRECTPATCH = 61, DP2_DRAWTRIPATCH = 62, DP2_VOLUMEBLT = 63,
    DP2_BUFFERBLT = 64, DP2_MULTIPLYTRANSFORM = 65, DP2_ADDDIRTYRECT = 66, DP2_ADDDIRTYBOX = 67,
};
#define D3DERR_COMMAND_UNPARSED_ 0x88760BB8u

struct VramSurf {
    d3dpt_vram_surface d{};
    std::vector<d3dpt_u32x2> levels;    /* mip levels 1..n-1: offset, pitch */
    IDirect3DTexture9 *tex = nullptr;
    IDirect3DSurface9 *rt = nullptr;    /* render target or depth stencil */
    bool dirty = true;                  /* VRAM newer than the host object */
    bool rendered = false;              /* host render target newer than VRAM */
    void release() {
        if (tex) tex->Release();
        if (rt) rt->Release();
        tex = nullptr; rt = nullptr;
    }
};

struct Ctx {
    uint32_t rt = 0, z = 0;
    D3DVIEWPORT9 vp = { 0, 0, 0, 0, 0.0f, 1.0f };
};

} // namespace

struct Ddi {
    std::unordered_map<uint32_t, VramSurf> surfs;
    std::unordered_map<uint32_t, Ctx> ctxs;
    uint32_t bound_rt = 0, bound_z = 0;     /* what the device's targets are set to */
    IDirect3DSurface9 *stage = nullptr;     /* system-memory staging for readback / upload */
    IDirect3DSurface9 *stage_def = nullptr; /* default-pool hop for uploads into render targets */
    uint32_t stage_w = 0, stage_h = 0;
    D3DFORMAT stage_fmt = D3DFMT_UNKNOWN;
    std::vector<uint16_t> idx;
    std::vector<uint32_t> warned;           /* one log line per unsupported state / token */
    /* DX8 state sets (STATESET tokens) as d3d9 state blocks, by the runtime's handle */
    std::unordered_map<uint32_t, IDirect3DStateBlock9 *> sblocks;
    bool recording = false;
    uint32_t dp2_calls = 0, draws = 0, readbacks = 0;
    /* D3DPT_DP2_TRACE=<flag file>: while the file exists, every token of
     * the next frame (DP2 records up to the next READBACK) is logged with
     * its arguments; the file is removed when the frame ends */
    const char *trace_flag = getenv("D3DPT_DP2_TRACE");
    bool trace = false, trace_armed = false;
    uint32_t trace_draws = 0;               /* draw-<n>.ppm snapshots of the traced frame */
    /* the render / stage states seen so far (a snapshot at the start of a
     * traced frame: most are set once at scene start) */
    uint32_t rs_val[256] = {}, tss_val[8][33] = {};
    uint8_t rs_set[256] = {}, tss_set[8][33] = {};
    /* D3DPT_DDI_NOFOG=1: FOGENABLE forced off (an experiment switch) */
    bool nofog = getenv("D3DPT_DDI_NOFOG") && atoi(getenv("D3DPT_DDI_NOFOG")) != 0;
    /* D3DPT_DDI_REREAD=1: every texture is re-read from VRAM at every bind
     * (the experiment that tells a stale host copy from never-written VRAM) */
    bool reread_all = getenv("D3DPT_DDI_REREAD") && atoi(getenv("D3DPT_DDI_REREAD")) != 0;
    /* a rate line every 5 s of host time while frames are read back (the
     * frame rate of the guest's Direct3D, one readback per presented frame) */
    std::chrono::steady_clock::time_point stat_t0{};
    uint32_t stat_dp2 = 0, stat_draws = 0, stat_rb = 0;

    bool warn_once(uint32_t key) {
        for (uint32_t k : warned) if (k == key) return false;
        warned.push_back(key);
        return true;
    }
    void drop_stage() {
        if (stage) stage->Release();
        if (stage_def) stage_def->Release();
        stage = stage_def = nullptr;
        stage_w = stage_h = 0;
    }
};

namespace {

static Ddi &ddi(Exec &x) {
    if (!x.ddi) x.ddi = new Ddi;
    return *x.ddi;
}

/* --------------------------------------------------------------- formats */

static bool fmt_dxt(uint32_t f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
/* bytes of one row of w pixels (blocks for DXT); 0 = unknown format */
static uint32_t fmt_row_bytes(uint32_t f, uint32_t w) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
        return w * 4;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
    case D3DFMT_L16: case D3DFMT_V8U8:
        return w * 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A4L4: case D3DFMT_P8:
        return w;
    case D3DFMT_DXT1:
        return ((w + 3) / 4) * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return ((w + 3) / 4) * 16;
    default:
        return 0;
    }
}
static uint32_t fmt_rows(uint32_t f, uint32_t h) { return fmt_dxt(f) ? (h + 3) / 4 : h; }

/* -------------------------------------------------------------- surfaces */

static VramSurf *surf(Exec &x, uint32_t h) {
    if (!x.ddi || !h) return nullptr;
    auto it = x.ddi->surfs.find(h);
    return it == x.ddi->surfs.end() ? nullptr : &it->second;
}

static bool ensure_device(Exec &x, uint32_t w, uint32_t h) {
    if (x.dev) return true;
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = w; pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr;
    try {
        hr = x.d3d->CreateDevice(0, D3DDEVTYPE_HAL, nullptr,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    } catch (...) { hr = E_FAIL; dev = nullptr; }
    x.log("ddi: device for %ux%u render targets -> 0x%08x", w, h, (unsigned)hr);
    if (FAILED(hr) || !dev) return false;
    x.dev = dev;
    x.dev_handle = 0;
    if (x.ops.active) x.ops.active(x.ops.ud, 1);
    return true;
}

/* the host object behind a surface, created on first use */
static bool ensure_object(Exec &x, VramSurf &s) {
    if (s.tex || s.rt) return true;
    if (!ensure_device(x, s.d.width, s.d.height)) return false;
    HRESULT hr;
    if (s.d.caps & D3DPT_VS_ZBUFFER) {
        static const uint32_t fallback[] = { D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16 };
        hr = x.dev->CreateDepthStencilSurface(s.d.width, s.d.height, (D3DFORMAT)s.d.format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr);
        for (uint32_t i = 0; FAILED(hr) && i < 3; i++)
            hr = x.dev->CreateDepthStencilSurface(s.d.width, s.d.height, (D3DFORMAT)fallback[i], D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr);
        if (FAILED(hr)) x.log("ddi: depth surface %ux%u fmt %u: 0x%08x", s.d.width, s.d.height, s.d.format, (unsigned)hr);
    } else if ((s.d.caps & D3DPT_VS_TEXTURE) && (s.d.caps & D3DPT_VS_RENDER_TARGET)) {
        /* render-to-texture: a default-pool render-target texture, level 0 is the target */
        hr = x.dev->CreateTexture(s.d.width, s.d.height, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)s.d.format, D3DPOOL_DEFAULT, &s.tex, nullptr);
        if (SUCCEEDED(hr)) hr = s.tex->GetSurfaceLevel(0, &s.rt);
        if (FAILED(hr)) x.log("ddi: render-target texture %ux%u fmt %u: 0x%08x", s.d.width, s.d.height, s.d.format, (unsigned)hr);
    } else if (s.d.caps & (D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY)) {
        hr = x.dev->CreateRenderTarget(s.d.width, s.d.height, (D3DFORMAT)s.d.format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr);
        if (FAILED(hr)) x.log("ddi: render target %ux%u fmt %u: 0x%08x", s.d.width, s.d.height, s.d.format, (unsigned)hr);
    } else {
        hr = x.dev->CreateTexture(s.d.width, s.d.height, s.d.levels, 0, (D3DFORMAT)s.d.format, D3DPOOL_MANAGED, &s.tex, nullptr);
        if (FAILED(hr)) x.log("ddi: texture %ux%u fmt %u levels %u: 0x%08x", s.d.width, s.d.height, s.d.format, s.d.levels, (unsigned)hr);
    }
    return SUCCEEDED(hr);
}

static void copy_rows(void *dst, uint32_t dpitch, const void *src, uint32_t spitch, uint32_t row, uint32_t rows) {
    for (uint32_t y = 0; y < rows; y++)
        memcpy((uint8_t *)dst + (size_t)y * dpitch, (const uint8_t *)src + (size_t)y * spitch, row);
}

/* a system-memory + default-pool staging pair of the surface's size and format */
static bool ensure_stage(Exec &x, Ddi &d, uint32_t w, uint32_t h, D3DFORMAT fmt, bool with_default) {
    if (d.stage && (d.stage_w != w || d.stage_h != h || d.stage_fmt != fmt)) d.drop_stage();
    if (!d.stage) {
        if (FAILED(x.dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &d.stage, nullptr))) return false;
        d.stage_w = w; d.stage_h = h; d.stage_fmt = fmt;
    }
    if (with_default && !d.stage_def &&
        FAILED(x.dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_DEFAULT, &d.stage_def, nullptr))) return false;
    return true;
}

/* VRAM -> host texture (every level) */
static void upload_texture(Exec &x, VramSurf &s) {
    D3DLOCKED_RECT lr;
    for (uint32_t l = 0; l < s.d.levels; l++) {
        uint32_t w = s.d.width >> l, h = s.d.height >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        uint32_t off = l ? s.levels[l - 1].a : s.d.offset, pitch = l ? s.levels[l - 1].b : s.d.pitch;
        uint32_t row = fmt_row_bytes(s.d.format, w), rows = fmt_rows(s.d.format, h);
        if (FAILED(s.tex->LockRect(l, &lr, nullptr, 0))) continue;
        copy_rows(lr.pBits, lr.Pitch, x.vram + off, pitch, row < (uint32_t)lr.Pitch ? row : (uint32_t)lr.Pitch, rows);
        s.tex->UnlockRect(l);
    }
    s.dirty = false;
}

/* VRAM -> host render target (the guest drew into the target with GDI / the HEL) */
static void upload_target(Exec &x, Ddi &d, VramSurf &s) {
    D3DLOCKED_RECT lr;
    if (!ensure_stage(x, d, s.d.width, s.d.height, (D3DFORMAT)s.d.format, true)) return;
    if (FAILED(d.stage->LockRect(&lr, nullptr, 0))) return;
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width);
    copy_rows(lr.pBits, lr.Pitch, x.vram + s.d.offset, s.d.pitch, row, s.d.height);
    d.stage->UnlockRect();
    if (SUCCEEDED(x.dev->UpdateSurface(d.stage, nullptr, d.stage_def, nullptr)))
        x.dev->StretchRect(d.stage_def, nullptr, s.rt, nullptr, D3DTEXF_NONE);
    s.dirty = false;
}

/* host render target -> VRAM */
static HRESULT readback(Exec &x, Ddi &d, VramSurf &s) {
    if (!s.rt || (s.d.caps & D3DPT_VS_ZBUFFER)) return D3DERR_INVALIDCALL;
    if (!ensure_stage(x, d, s.d.width, s.d.height, (D3DFORMAT)s.d.format, false)) return E_FAIL;
    HRESULT hr = x.dev->GetRenderTargetData(s.rt, d.stage);
    if (FAILED(hr)) { x.log("ddi: readback: GetRenderTargetData 0x%08x", (unsigned)hr); return hr; }
    D3DLOCKED_RECT lr;
    if (FAILED(d.stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return E_FAIL;
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width);
    copy_rows(x.vram + s.d.offset, s.d.pitch, lr.pBits, lr.Pitch, row, s.d.height);
    d.stage->UnlockRect();
    s.rendered = false;
    s.dirty = false;
    d.readbacks++;
    auto now = std::chrono::steady_clock::now();
    if (d.stat_t0 == std::chrono::steady_clock::time_point{}) d.stat_t0 = now;
    double dt = std::chrono::duration<double>(now - d.stat_t0).count();
    if (dt >= 5.0) {
        x.log("ddi: %.1f frames/s (%u readbacks, %u dp2 calls, %u draws in %.1f s)",
              (d.readbacks - d.stat_rb) / dt, d.readbacks - d.stat_rb, d.dp2_calls - d.stat_dp2, d.draws - d.stat_draws, dt);
        d.stat_t0 = now;
        d.stat_rb = d.readbacks; d.stat_dp2 = d.dp2_calls; d.stat_draws = d.draws;
    }
    if (x.ops.vram_dirty) x.ops.vram_dirty(x.ops.ud, s.d.offset, s.d.pitch * s.d.height);
    return S_OK;
}

/* set the context's targets on the device (and re-apply its viewport, which
 * SetRenderTarget resets); uploads a target the guest wrote since */
static bool bind_ctx(Exec &x, Ddi &d, Ctx &c, Batch &b, bool for_draw) {
    VramSurf *rt = surf(x, c.rt);
    if (!rt || !ensure_object(x, *rt) || !rt->rt) { b.err = D3DPT_ERR_BAD_HANDLE; return false; }
    VramSurf *z = c.z ? surf(x, c.z) : nullptr;
    if (c.z && (!z || !ensure_object(x, *z))) { b.err = D3DPT_ERR_BAD_HANDLE; return false; }
    if (d.bound_rt != c.rt || d.bound_z != c.z) {
        x.dev->SetRenderTarget(0, rt->rt);
        x.dev->SetDepthStencilSurface(z ? z->rt : nullptr);
        d.bound_rt = c.rt; d.bound_z = c.z;
        if (c.vp.Width && c.vp.Height) x.dev->SetViewport(&c.vp);
    }
    if (for_draw) {
        if (rt->dirty) upload_target(x, d, *rt);
        rt->rendered = true;
    }
    return true;
}

/* ------------------------------------------------------------- DP2 state */

/* DX7 render states that exist in d3d9 under the same number pass through;
 * the DX5/6-era ones (texture handle, stipple, ROP, colour key, ...) have
 * no d3d9 equivalent and are dropped */
static bool rs_passthrough(uint32_t s) {
    static const uint8_t dropped[] = { 1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 17, 18, 21, 30, 31, 32, 33, 39, 40, 41,
                                       43, 44, 45, 46, 47, 49, 50, 51, 138, 144,
                                       153, 164, 172, 173 };    /* d3d8: SOFTWAREVERTEXPROCESSING, PATCHSEGMENTS, POSITION/NORMALORDER */
    if (s >= 210 || (s >= 64 && s <= 95)) return false;
    for (uint8_t v : dropped) if (v == s) return false;
    return true;
}

static uint32_t stride_of_fvf(uint32_t fvf) {
    uint32_t n = 0, pos = fvf & 0xe;
    switch (pos) {
    case 0x2: n = 12; break;                    /* XYZ */
    case 0x4: n = 16; break;                    /* XYZRHW */
    case 0x6: n = 16; break;                    /* XYZB1 */
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    default: return 0;
    }
    if (fvf & 0x10) n += 12;                     /* NORMAL */
    if (fvf & 0x20) n += 4;                      /* PSIZE */
    if (fvf & 0x40) n += 4;                      /* DIFFUSE */
    if (fvf & 0x80) n += 4;                      /* SPECULAR */
    uint32_t tex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < tex; i++) {
        switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: n += 8; break;
        case 1: n += 12; break;
        case 2: n += 16; break;
        case 3: n += 4; break;
        }
    }
    return n;
}

struct Dp2 {
    Exec &x; Ddi &d; Ctx &c; Batch &b;
    const uint8_t *cmd, *cmd_end, *vtx;
    uint32_t stride, nverts, fvf_;
    uint32_t cur_fvf = 0;       /* what SetFVF was last given */
    uint32_t pos = 0;           /* offset of the current token, for dwErrorOffset */
    HRESULT hr = S_OK;

    uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
    uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
    /* the last tokens parsed, printed with the first failure of a kind: a
     * mis-sized token shows up as garbage several tokens later */
    struct Tok { uint32_t pos, op, count; } hist[24];
    uint32_t nhist = 0;
    void log_hist() {
        char line[400]; size_t n = 0;
        for (uint32_t i = nhist > 24 ? nhist - 24 : 0; i < nhist && n < sizeof line - 24; i++) {
            const Tok &t = hist[i % 24];
            n += (size_t)snprintf(line + n, sizeof line - n, " %u:%ux%u", t.pos, t.op, t.count);
        }
        x.log("ddi: dp2:   tokens before it (offset:op x count):%s", line);
    }
    void tr(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!d.trace) return;
        char buf[400]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        x.log("ddi: trace: %6u %s", pos, buf);
    }
    /* every level of a traced texture, as tex-<handle>-l<n>.ppm (RGB) +
     * tex-<handle>-l<n>-a.pgm (alpha) next to the flag file, once per handle;
     * 32-bit and the three 16-bit RGB formats */
    void dump_texture(const VramSurf &s, uint32_t handle) {
        char path[512]; const char *slash = strrchr(d.trace_flag, '/');
        int dirlen = slash ? (int)(slash - d.trace_flag) : 1;
        const char *dir = slash ? d.trace_flag : ".";
        snprintf(path, sizeof path, "%.*s/tex-%u-l0.ppm", dirlen, dir, handle);
        if (access(path, F_OK) == 0) return;
        uint32_t bpp = fmt_row_bytes(s.d.format, 1);
        for (uint32_t l = 0; l < s.d.levels; l++) {
            uint32_t w = s.d.width >> l, h = s.d.height >> l;
            if (!w) w = 1;
            if (!h) h = 1;
            uint32_t off = l ? s.levels[l - 1].a : s.d.offset, pitch = l ? s.levels[l - 1].b : s.d.pitch;
            snprintf(path, sizeof path, "%.*s/tex-%u-l%u.ppm", dirlen, dir, handle, l);
            FILE *rgb = fopen(path, "wb");
            snprintf(path, sizeof path, "%.*s/tex-%u-l%u-a.pgm", dirlen, dir, handle, l);
            FILE *al = fopen(path, "wb");
            if (!rgb || !al) { if (rgb) fclose(rgb); if (al) fclose(al); return; }
            fprintf(rgb, "P6\n%u %u\n255\n", w, h);
            fprintf(al, "P5\n%u %u\n255\n", w, h);
            for (uint32_t yy = 0; yy < h; yy++) {
                const uint8_t *row = x.vram + off + (size_t)yy * pitch;
                for (uint32_t xx = 0; xx < w; xx++) {
                    uint8_t px[4] = { 0, 0, 0, 255 };
                    if (bpp == 4) { uint32_t v; memcpy(&v, row + xx * 4, 4); px[0] = v >> 16; px[1] = v >> 8; px[2] = v; px[3] = s.d.format == D3DFMT_A8R8G8B8 ? v >> 24 : 255; }
                    else {
                        uint16_t v; memcpy(&v, row + xx * 2, 2);
                        if (s.d.format == D3DFMT_R5G6B5) { px[0] = (v >> 8) & 0xf8; px[1] = (v >> 3) & 0xfc; px[2] = (v << 3) & 0xf8; }
                        else if (s.d.format == D3DFMT_A4R4G4B4 || s.d.format == D3DFMT_X4R4G4B4) { px[0] = (v >> 4) & 0xf0; px[1] = v & 0xf0; px[2] = (v << 4) & 0xf0; px[3] = s.d.format == D3DFMT_A4R4G4B4 ? (v >> 8) & 0xf0 : 255; }
                        else { px[0] = (v >> 7) & 0xf8; px[1] = (v >> 2) & 0xf8; px[2] = (v << 3) & 0xf8; px[3] = s.d.format == D3DFMT_A1R5G5B5 ? ((v & 0x8000) ? 255 : 0) : 255; }
                    }
                    fwrite(px, 1, 3, rgb); fwrite(px + 3, 1, 1, al);
                }
            }
            fclose(rgb); fclose(al);
        }
    }
    /* trace: the first three vertices of a draw (position, colours, two uv sets) */
    void trv(const uint8_t *v, uint32_t n, uint32_t st = 0, uint32_t fvf = 0) {
        if (!st) st = stride;
        if (!fvf) fvf = fvf_;
        for (uint32_t i = 0; i < n && i < 3; i++, v += st) {
            /* position (3 or 4 floats), blend weights, normal, then the colours and uv sets */
            uint32_t pos = fvf & 0xe, npos = pos == 0x4 ? 4 : pos == 0x2 ? 3 : 3 + (pos - 4) / 2;
            float f[4] = { 0, 0, 0, 0 }; memcpy(f, v, npos > 4 ? 16 : npos * 4);
            const uint8_t *q = v + npos * 4;
            float nrm[3] = { 0, 0, 0 };
            if (fvf & 0x10) { memcpy(nrm, q, 12); q += 12; }
            if (fvf & 0x20) q += 4;
            uint32_t col = 0, spec = 0;
            if (fvf & 0x40) { memcpy(&col, q, 4); q += 4; }
            if (fvf & 0x80) { memcpy(&spec, q, 4); q += 4; }
            float uv[4] = { 0, 0, 0, 0 }; uint32_t nt = (fvf >> 8) & 0xf;
            if (nt > 0) memcpy(uv, q, 8);
            if (nt > 1) memcpy(uv + 2, q + 8, 8);
            if (pos == 0x4)
                tr("    v%u %.1f,%.1f z %.4f rhw %.5f diffuse %08x specular %08x uv0 %.3f,%.3f uv1 %.3f,%.3f", i, f[0], f[1], f[2], f[3], col, spec, uv[0], uv[1], uv[2], uv[3]);
            else
                tr("    v%u %.3f,%.3f,%.3f n %.2f,%.2f,%.2f diffuse %08x specular %08x uv0 %.3f,%.3f uv1 %.3f,%.3f", i, f[0], f[1], f[2], nrm[0], nrm[1], nrm[2], col, spec, uv[0], uv[1], uv[2], uv[3]);
        }
    }
    void set_fvf(uint32_t fvf) {
        if (fvf != cur_fvf) { x.dev->SetFVF(fvf); cur_fvf = fvf; }
    }
    static uint32_t prim_verts(uint32_t t, uint32_t count) {
        switch (t) {
        case D3DPT_POINTLIST: return count;
        case D3DPT_LINELIST: return count * 2;
        case D3DPT_LINESTRIP: return count + 1;
        case D3DPT_TRIANGLELIST: return count * 3;
        case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return count + 2;
        default: return 0;
        }
    }
    /* the driver's self-contained DX8 draw: vertices and 16-bit indices inline */
    bool draw8(const uint8_t *q, size_t left, size_t &need) {
        d3dpt_dp2_draw8 h;
        if (left < sizeof h) return fail("truncated DRAW8");
        memcpy(&h, q, sizeof h);
        size_t vb = ((size_t)h.nverts * h.stride + 3) & ~(size_t)3, ib = ((size_t)h.nindices * 2 + 3) & ~(size_t)3;
        need = sizeof h + vb + ib;
        if (need > left) return fail("truncated DRAW8 data");
        uint32_t st = stride_of_fvf(h.fvf);
        if (!st || st > h.stride || h.stride > 1024 || h.nverts > 0x10000 || h.nindices > 0x100000 || h.prim_type < 1 || h.prim_type > 6)
            return fail("bad DRAW8");
        const uint8_t *vd = q + sizeof h, *id = vd + vb;
        D3DPRIMITIVETYPE t = (D3DPRIMITIVETYPE)h.prim_type;
        uint32_t nv = prim_verts(t, h.prim_count);
        set_fvf(h.fvf);
        tr("draw8 type %u prims %u fvf 0x%x stride %u vertices %u indices %u (min %u)", h.prim_type, h.prim_count, h.fvf, h.stride, h.nverts, h.nindices, h.min_index);
        if (d.trace) trv(vd, h.nverts, h.stride, h.fvf);
        if (!h.prim_count) return true;
        if (!h.nindices) {
            if (nv > h.nverts) { if (d.warn_once(0xa0000 | t)) x.log("ddi: dp2: draw8 primitive %u: %u vertices of %u", t, nv, h.nverts); return true; }
            x.dev->DrawPrimitiveUP(t, h.prim_count, vd, h.stride);
        } else {
            if (nv > h.nindices) { if (d.warn_once(0xa0010 | t)) x.log("ddi: dp2: draw8 indexed primitive %u: %u indices of %u", t, nv, h.nindices); return true; }
            d.idx.resize(nv);
            for (uint32_t i = 0; i < nv; i++) {
                uint32_t v = u16(id + 2 * i);
                if (v < h.min_index || v - h.min_index >= h.nverts) {
                    if (d.warn_once(0xa0020 | t)) x.log("ddi: dp2: draw8 index %u outside %u..%u", v, h.min_index, h.min_index + h.nverts);
                    return true;
                }
                d.idx[i] = (uint16_t)(v - h.min_index);
            }
            x.dev->DrawIndexedPrimitiveUP(t, 0, h.nverts, h.prim_count, d.idx.data(), D3DFMT_INDEX16, vd, h.stride);
        }
        d.draws++;
        snap();
        return true;
    }
    /* trace: the render target after a draw, as draw-<n>.ppm next to the flag file */
    void snap() {
        if (!d.trace) return;
        VramSurf *rt = surf(x, c.rt);
        if (!rt || !rt->rt) return;
        if (!ensure_stage(x, d, rt->d.width, rt->d.height, (D3DFORMAT)rt->d.format, false)) return;
        if (FAILED(x.dev->GetRenderTargetData(rt->rt, d.stage))) return;
        D3DLOCKED_RECT lr;
        if (FAILED(d.stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
        char path[512]; const char *slash = strrchr(d.trace_flag, '/');
        snprintf(path, sizeof path, "%.*s/draw-%03u.ppm", slash ? (int)(slash - d.trace_flag) : 1, slash ? d.trace_flag : ".", ++d.trace_draws);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", rt->d.width, rt->d.height);
            for (uint32_t yy = 0; yy < rt->d.height; yy++) {
                const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)yy * lr.Pitch;
                for (uint32_t xx = 0; xx < rt->d.width; xx++) {
                    uint8_t px[3];
                    if (rt->d.format == D3DFMT_R5G6B5) { uint16_t v; memcpy(&v, row + xx * 2, 2); px[0] = (v >> 8) & 0xf8; px[1] = (v >> 3) & 0xfc; px[2] = (v << 3) & 0xf8; }
                    else { uint32_t v; memcpy(&v, row + xx * 4, 4); px[0] = v >> 16; px[1] = v >> 8; px[2] = v; }
                    fwrite(px, 1, 3, f);
                }
            }
            fclose(f);
        }
        d.stage->UnlockRect();
        tr("  -> draw-%03u.ppm", d.trace_draws);
    }
    bool fail(const char *why) {
        if (d.warn_once(0x10000 | (uint32_t)(uintptr_t)why)) { x.log("ddi: dp2: %s at offset %u", why, pos); log_hist(); }
        hr = D3DERR_COMMAND_UNPARSED_;
        return false;
    }
    bool fail_token(uint32_t op, uint32_t count) {
        if (d.warn_once(0x80000 | op)) { x.log("ddi: dp2: unknown token %u (count %u) at offset %u", op, count, pos); log_hist(); }
        hr = D3DERR_COMMAND_UNPARSED_;
        return false;
    }

    void draw(D3DPRIMITIVETYPE t, uint32_t first, uint32_t count) {
        bool ok; uint32_t nv = prim_verts(t, count);
        ok = count && (uint64_t)first + nv <= nverts;
        tr("draw type %u first %u prims %u (%u vertices)%s", t, first, count, nv, ok ? "" : " OUT OF RANGE");
        if (!ok) { if (d.warn_once(0x20000 | t)) x.log("ddi: dp2: primitive %u: vertices %u+%u of %u", t, first, nv, nverts); return; }
        if (d.trace) trv(vtx + (size_t)first * stride, nv);
        x.dev->DrawPrimitiveUP(t, count, vtx + (size_t)first * stride, stride);
        d.draws++;
        snap();
    }
    /* d.idx holds the indices (absolute); the draw uses the touched vertex range */
    void draw_indexed(D3DPRIMITIVETYPE t, uint32_t count) {
        uint32_t lo = ~0u, hi = 0;
        for (uint16_t i : d.idx) { if (i < lo) lo = i; if (i > hi) hi = i; }
        tr("draw indexed type %u prims %u indices %zu vertices %u..%u%s", t, count, d.idx.size(), lo, hi, hi >= nverts ? " OUT OF RANGE" : "");
        if (!count || d.idx.empty() || hi >= nverts) {
            if (d.warn_once(0x30000 | t)) x.log("ddi: dp2: indexed primitive %u: index %u of %u vertices", t, hi, nverts);
            return;
        }
        if (d.trace) trv(vtx + (size_t)d.idx[0] * stride, 1), trv(vtx + (size_t)d.idx[1] * stride, 1), trv(vtx + (size_t)d.idx[2 < d.idx.size() ? 2 : 0] * stride, 1);
        x.dev->DrawIndexedPrimitiveUP(t, lo, hi - lo + 1, count, d.idx.data(), D3DFMT_INDEX16, vtx, stride);
        d.draws++;
        snap();
    }
    /* count indices at p, each + base */
    bool gather(const uint8_t *p, uint32_t count, uint32_t base) {
        d.idx.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = u16(p + 2 * i) + base;
            if (v > 0xffff) return false;
            d.idx[i] = (uint16_t)v;
        }
        return true;
    }

    void render_state(uint32_t s, uint32_t v) {
        tr("rs %u = 0x%x%s", s, v, rs_passthrough(s) ? "" : " (dropped)");
        if (s < 256) { d.rs_val[s] = v; d.rs_set[s] = 1; }
        if (s == 28 && d.nofog) v = 0;
        if (rs_passthrough(s)) { x.dev->SetRenderState((D3DRENDERSTATETYPE)s, v); return; }
        if (s == 41 && v && d.warn_once(0x40000 | s)) x.log("ddi: dp2: colour keying (render state 41) is not implemented");
        else if (s != 41 && s < 64 && d.warn_once(0x40000 | s)) x.log("ddi: dp2: render state %u dropped (no d3d9 equivalent)", s);
    }

    void stage_state(uint32_t stage, uint32_t st, uint32_t v) {
        if (stage >= 8) return;
        tr("tss %u.%u = 0x%x", stage, st, v);
        if (st < 33) { d.tss_val[stage][st] = v; d.tss_set[stage][st] = 1; }
        switch (st) {
        case 0: {                                           /* TEXTUREMAP: the surface handle */
            IDirect3DBaseTexture9 *t = nullptr;
            if (v) {
                VramSurf *s = surf(x, v);
                if (s && d.reread_all) s->dirty = true;
                if (s) {
                    /* the mean of the level-0 texels in VRAM (every 8th pixel of every 8th row, 32/16-bit only):
                     * black VRAM = the guest never wrote it, content = the host copy may be stale */
                    uint32_t bpp = fmt_row_bytes(s->d.format, 1), n = 0; uint64_t sum = 0;
                    if (fmt_dxt(s->d.format)) {                 /* compressed: the mean of the block bytes */
                        uint32_t rows = fmt_rows(s->d.format, s->d.height), row = fmt_row_bytes(s->d.format, s->d.width);
                        for (uint32_t yy = 0; yy < rows; yy++) for (uint32_t xx = 0; xx < row; xx += 4, n++) sum += 3 * x.vram[s->d.offset + (size_t)yy * s->d.pitch + xx];
                    } else if (bpp == 4 || bpp == 2) {
                        for (uint32_t yy = 0; yy < s->d.height; yy += 8) {
                            const uint8_t *row = x.vram + s->d.offset + (size_t)yy * s->d.pitch;
                            for (uint32_t xx = 0; xx < s->d.width; xx += 8, n++) {
                                if (bpp == 4) { uint32_t px; memcpy(&px, row + xx * 4, 4); sum += ((px >> 16) & 0xff) + ((px >> 8) & 0xff) + (px & 0xff); }
                                else { uint16_t px; memcpy(&px, row + xx * 2, 2); sum += px ? 128 : 0; }
                            }
                        }
                    }
                    tr("  texture %u: %ux%u fmt %u levels %u caps 0x%x%s, vram mean %u/765 of %u samples", v, s->d.width, s->d.height, s->d.format, s->d.levels, s->d.caps, s->dirty ? " (re-read)" : "", n ? (unsigned)(sum / n) : 0, n);
                    if (d.trace && (bpp == 4 || bpp == 2)) dump_texture(*s, v);
                }
                if (!s) { if (d.warn_once(0x50000)) x.log("ddi: dp2: texture handle %u unknown", v); }
                else if (ensure_object(x, *s) && s->tex) { if (s->dirty) { if (s->rt) upload_target(x, d, *s); else upload_texture(x, *s); } t = s->tex; }
            }
            x.dev->SetTexture(stage, t);
            break;
        }
        case 12: x.dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, v); x.dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, v); break;
        case 13: x.dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, v); break;
        case 14: x.dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, v); break;
        case 15: x.dev->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, v); break;
        case 16: x.dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, v == 1 ? D3DTEXF_POINT : v == 5 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR); break;
        case 17: x.dev->SetSamplerState(stage, D3DSAMP_MINFILTER, v == 1 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR); break;
        case 18: x.dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, v == 2 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_LINEAR : D3DTEXF_NONE); break;
        case 19: x.dev->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, v); break;
        case 20: x.dev->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, v); break;
        case 21: x.dev->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, v); break;
        default:
            if (st < 33) x.dev->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)st, v);
            break;
        }
    }

    void set_targets(uint32_t rt, uint32_t z) {
        tr("render target %u z %u", rt, z);
        if (!surf(x, rt)) { if (d.warn_once(0x60000)) x.log("ddi: dp2: render target handle %u unknown", rt); return; }
        if (z && !surf(x, z)) z = 0;
        c.rt = rt; c.z = z;
        bind_ctx(x, d, c, b, true);
    }

    void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, uint32_t nrects, const uint8_t *rects) {
        VramSurf *rt = surf(x, c.rt);
        tr("clear flags 0x%x color 0x%08x z %g stencil %u rects %u", flags, color, z, stencil, nrects);
        /* a full clear of the target makes any guest-side content moot: skip its upload */
        if (rt && (flags & D3DCLEAR_TARGET) && nrects == 0) rt->dirty = false;
        if (!bind_ctx(x, d, c, b, true)) return;
        std::vector<D3DRECT> r(nrects);
        if (nrects) memcpy(r.data(), rects, nrects * sizeof(D3DRECT));
        x.dev->Clear(nrects, nrects ? r.data() : nullptr, flags & 7, color, z, stencil);
    }

    /* the body size that puts the offset after it at a DWORD boundary
     * (the buffer itself is 8-aligned in the record); align_next(0) is the
     * padding between a token's header and its DWORD-aligned payload */
    size_t align_next(size_t need) const {
        size_t next = ((size_t)pos + 4 + need + 3) & ~(size_t)3;
        return next - pos - 4;
    }

    bool run() {
        if (!bind_ctx(x, d, c, b, true)) return false;
        const uint8_t *p = cmd;
        while (p < cmd_end) {
            pos = (uint32_t)(p - cmd);
            if (cmd_end - p < 4) return fail("truncated command");
            uint32_t op = p[0], count = u16(p + 2);
            const uint8_t *q = p + 4;
            hist[nhist++ % 24] = { pos, op, count };
            tr("op %u x%u", op, count);
            size_t left = cmd_end - q, need = 0;
            switch (op) {
            case DP2_POINTS:
                need = count * 4u;
                if (need > left) return fail("truncated POINTS");
                for (uint32_t i = 0; i < count; i++) draw(D3DPT_POINTLIST, u16(q + 4 * i + 2), u16(q + 4 * i));
                break;
            case DP2_INDEXEDLINELIST:
                need = count * 4u;
                if (need > left) return fail("truncated INDEXEDLINELIST");
                if (gather(q, count * 2, 0)) draw_indexed(D3DPT_LINELIST, count);
                break;
            case DP2_INDEXEDTRIANGLELIST:
                need = count * 8u;
                if (need > left) return fail("truncated INDEXEDTRIANGLELIST");
                d.idx.resize(count * 3);
                for (uint32_t i = 0; i < count; i++) {
                    d.idx[3 * i] = u16(q + 8 * i); d.idx[3 * i + 1] = u16(q + 8 * i + 2); d.idx[3 * i + 2] = u16(q + 8 * i + 4);
                }
                draw_indexed(D3DPT_TRIANGLELIST, count);
                break;
            case DP2_RENDERSTATE:
                need = count * 8u;
                if (need > left) return fail("truncated RENDERSTATE");
                for (uint32_t i = 0; i < count; i++) render_state(u32(q + 8 * i), u32(q + 8 * i + 4));
                break;
            case DP2_LINELIST:
                need = 2;
                if (need > left) return fail("truncated LINELIST");
                draw(D3DPT_LINELIST, u16(q), count);
                break;
            case DP2_LINESTRIP:
                need = 2;
                if (need > left) return fail("truncated LINESTRIP");
                draw(D3DPT_LINESTRIP, u16(q), count);
                break;
            case DP2_INDEXEDLINESTRIP:
                need = 2 + (count + 1) * 2u;
                if (need > left) return fail("truncated INDEXEDLINESTRIP");
                if (gather(q + 2, count + 1, u16(q))) draw_indexed(D3DPT_LINESTRIP, count);
                break;
            case DP2_TRIANGLELIST:
                need = 2;
                if (need > left) return fail("truncated TRIANGLELIST");
                draw(D3DPT_TRIANGLELIST, u16(q), count);
                break;
            case DP2_TRIANGLESTRIP:
                need = 2;
                if (need > left) return fail("truncated TRIANGLESTRIP");
                draw(D3DPT_TRIANGLESTRIP, u16(q), count);
                break;
            case DP2_INDEXEDTRIANGLESTRIP:
                need = 2 + (count + 2) * 2u;
                if (need > left) return fail("truncated INDEXEDTRIANGLESTRIP");
                if (gather(q + 2, count + 2, u16(q))) draw_indexed(D3DPT_TRIANGLESTRIP, count);
                break;
            case DP2_TRIANGLEFAN:
                need = 2;
                if (need > left) return fail("truncated TRIANGLEFAN");
                draw(D3DPT_TRIANGLEFAN, u16(q), count);
                break;
            case DP2_INDEXEDTRIANGLEFAN:
                need = 2 + (count + 2) * 2u;
                if (need > left) return fail("truncated INDEXEDTRIANGLEFAN");
                if (gather(q + 2, count + 2, u16(q))) draw_indexed(D3DPT_TRIANGLEFAN, count);
                break;
            case DP2_TRIANGLEFAN_IMM: {
                /* the payload (edge flags, then count + 2 vertices inline)
                 * starts at the next DWORD-aligned *offset* of the buffer,
                 * and so does the next token: a token after an odd-length
                 * one (INDEXEDTRIANGLELIST2: 2 + 6n bytes) sits at offset
                 * 2 mod 4 and the runtime pads after its 4-byte header. The
                 * DX7 runtime never produced that sequence (D3D7TEST, FIFA);
                 * the DX8 runtime's legacy path does it every frame, and
                 * reading the vertices 2 bytes early drew Max Payne's alley
                 * with black bands (2026-09-05). */
                size_t pad = align_next(0);
                need = pad + 4 + (size_t)(count + 2) * stride;
                if (need > left) return fail("truncated TRIANGLEFAN_IMM");
                if (count) { if (d.trace) trv(q + pad + 4, count + 2); x.dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, count, q + pad + 4, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_LINELIST_IMM: {
                size_t pad = align_next(0);
                need = pad + (size_t)count * 2 * stride;
                if (need > left) return fail("truncated LINELIST_IMM");
                if (count) { if (d.trace) trv(q + pad, count * 2); x.dev->DrawPrimitiveUP(D3DPT_LINELIST, count, q + pad, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_TEXTURESTAGESTATE:
                need = count * 8u;
                if (need > left) return fail("truncated TEXTURESTAGESTATE");
                for (uint32_t i = 0; i < count; i++) stage_state(u16(q + 8 * i), u16(q + 8 * i + 2), u32(q + 8 * i + 4));
                break;
            case DP2_INDEXEDTRIANGLELIST2: {
                need = 2 + count * 6u;
                if (need > left) return fail("truncated INDEXEDTRIANGLELIST2");
                uint32_t base = u16(q);
                if (gather(q + 2, count * 3, base)) draw_indexed(D3DPT_TRIANGLELIST, count);
                break;
            }
            case DP2_INDEXEDLINELIST2: {
                need = 2 + count * 4u;
                if (need > left) return fail("truncated INDEXEDLINELIST2");
                uint32_t base = u16(q);
                if (gather(q + 2, count * 2, base)) draw_indexed(D3DPT_LINELIST, count);
                break;
            }
            case DP2_VIEWPORTINFO:
                need = count * 16u;
                if (need > left) return fail("truncated VIEWPORTINFO");
                if (count) {
                    const uint8_t *v = q + 16 * (count - 1);
                    c.vp.X = u32(v); c.vp.Y = u32(v + 4); c.vp.Width = u32(v + 8); c.vp.Height = u32(v + 12);
                    tr("viewport %u,%u %ux%u", c.vp.X, c.vp.Y, c.vp.Width, c.vp.Height);
                    x.dev->SetViewport(&c.vp);
                }
                break;
            case DP2_WINFO:
                need = count * 8u;
                if (need > left) return fail("truncated WINFO");
                break;                                          /* w-buffer range: no d3d9 equivalent */
            case DP2_SETPALETTE:
                need = count * 12u;
                if (need > left) return fail("truncated SETPALETTE");
                if (d.warn_once(op)) x.log("ddi: dp2: palettes are not supported");
                break;
            case DP2_UPDATEPALETTE:
                if (left < 8) return fail("truncated UPDATEPALETTE");
                need = 8 + (size_t)u16(q + 6) * 4;
                if (need > left) return fail("truncated UPDATEPALETTE");
                break;
            case DP2_ZRANGE:
                need = count * 8u;
                if (need > left) return fail("truncated ZRANGE");
                if (count) {
                    const uint8_t *v = q + 8 * (count - 1);
                    memcpy(&c.vp.MinZ, v, 4); memcpy(&c.vp.MaxZ, v + 4, 4);
                    x.dev->SetViewport(&c.vp);
                }
                break;
            case DP2_SETMATERIAL:
                need = count * 68u;
                if (need > left) return fail("truncated SETMATERIAL");
                if (count) { D3DMATERIAL9 m; memcpy(&m, q + 68 * (count - 1), sizeof m); x.dev->SetMaterial(&m); }
                break;
            case DP2_SETLIGHT: {
                const uint8_t *e = q;
                for (uint32_t i = 0; i < count; i++) {
                    if (cmd_end - e < 8) return fail("truncated SETLIGHT");
                    uint32_t index = u32(e), type = u32(e + 4);
                    e += 8;
                    /* DXVK grows its light array to the index (a garbage
                     * index = std::bad_alloc, which cannot be caught across
                     * its statically linked unwinder: QEMU aborts) */
                    bool ok = index < 1024;
                    if (!ok && d.warn_once(0x90000)) x.log("ddi: dp2: light index %u out of range, dropped", index);
                    if (type == 2) {                            /* D3DHAL_SETLIGHT_DATA: a D3DLIGHT7 follows */
                        if (cmd_end - e < 104) return fail("truncated SETLIGHT data");
                        D3DLIGHT9 l; memcpy(&l, e, sizeof l);
                        if (ok) x.dev->SetLight(index, &l);
                        e += 104;
                    } else if (ok) {
                        x.dev->LightEnable(index, type == 0);
                    }
                }
                need = e - q;
                break;
            }
            case DP2_CREATELIGHT:
                need = count * 4u;
                if (need > left) return fail("truncated CREATELIGHT");
                break;
            case DP2_SETTRANSFORM:
            case DP2_MULTIPLYTRANSFORM:
                need = count * 68u;
                if (need > left) return fail("truncated SETTRANSFORM");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t t = u32(q + 68 * i);
                    D3DMATRIX m; memcpy(&m, q + 68 * i + 4, sizeof m);
                    /* DX7 WORLD(1) / WORLD1..3 (4..6) -> d3d9 WORLD = 256..
                     * (DX8 already numbers them 256..); anything else DXVK
                     * indexes an array with: dropped */
                    if (t == 1) t = 256; else if (t >= 4 && t <= 6) t = 256 + (t - 3);
                    tr("%s %u", op == DP2_SETTRANSFORM ? "transform" : "multiply transform", t);
                    if (t == 2 || t == 3 || (t >= 16 && t <= 23) || (t >= 256 && t <= 511)) {
                        if (op == DP2_SETTRANSFORM) x.dev->SetTransform((D3DTRANSFORMSTATETYPE)t, &m);
                        else x.dev->MultiplyTransform((D3DTRANSFORMSTATETYPE)t, &m);
                    } else if (d.warn_once(0x90001)) x.log("ddi: dp2: transform %u out of range, dropped", u32(q + 68 * i));
                }
                break;
            case DP2_TEXBLT:
                need = count * 36u;
                if (need > left) return fail("truncated TEXBLT");
                if (d.warn_once(op)) x.log("ddi: dp2: TEXBLT is not supported (no driver-managed textures)");
                break;
            case DP2_STATESET:
                /* the runtime's state sets: BEGIN records what follows into a
                 * block (a predefined block type captures the device state
                 * at once), EXECUTE applies it, CAPTURE refreshes it */
                need = count * 12u;
                if (need > left) return fail("truncated STATESET");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t sop = u32(q + 12 * i), handle = u32(q + 12 * i + 4), type = u32(q + 12 * i + 8);
                    auto it = d.sblocks.find(handle);
                    tr("stateset op %u handle %u type %u", sop, handle, type);
                    switch (sop) {
                    case 0:                                             /* BEGIN */
                        if (d.recording) { IDirect3DStateBlock9 *sb = nullptr; x.dev->EndStateBlock(&sb); if (sb) sb->Release(); d.recording = false; }
                        if (it != d.sblocks.end()) { if (it->second) it->second->Release(); d.sblocks.erase(it); }
                        if (type >= 1 && type <= 3) {
                            IDirect3DStateBlock9 *sb = nullptr;
                            if (SUCCEEDED(x.dev->CreateStateBlock((D3DSTATEBLOCKTYPE)type, &sb)) && sb) d.sblocks[handle] = sb;
                        } else if (SUCCEEDED(x.dev->BeginStateBlock())) {
                            d.recording = true;
                            d.sblocks[handle] = nullptr;
                        }
                        break;
                    case 1:                                             /* END */
                        if (d.recording) {
                            IDirect3DStateBlock9 *sb = nullptr;
                            x.dev->EndStateBlock(&sb);
                            d.recording = false;
                            if (it != d.sblocks.end() && !it->second) it->second = sb;
                            else if (sb) sb->Release();
                        }
                        break;
                    case 2:                                             /* DELETE */
                        if (it != d.sblocks.end()) { if (it->second) it->second->Release(); d.sblocks.erase(it); }
                        break;
                    case 3:                                             /* EXECUTE */
                        if (it != d.sblocks.end() && it->second) { it->second->Apply(); x.dev->GetViewport(&c.vp); }
                        else if (d.warn_once(0xb0000)) x.log("ddi: dp2: state set %u executed before it was recorded", handle);
                        break;
                    case 4:                                             /* CAPTURE */
                        if (it != d.sblocks.end() && it->second) it->second->Capture();
                        break;
                    }
                }
                break;
            case DP2_SETPRIORITY:
                need = count * 8u;
                if (need > left) return fail("truncated SETPRIORITY");
                break;
            case DP2_SETRENDERTARGET:
                need = count * 8u;
                if (need > left) return fail("truncated SETRENDERTARGET");
                if (count) set_targets(u32(q + 8 * (count - 1)), u32(q + 8 * (count - 1) + 4));
                if (b.err) return false;
                break;
            case DP2_CLEAR: {
                need = 16 + count * 16u;
                if (need > left) return fail("truncated CLEAR");
                float z; memcpy(&z, q + 8, 4);
                clear(u32(q), u32(q + 4), z, u32(q + 12), count, q + 16);
                if (b.err) return false;
                break;
            }
            case DP2_SETTEXLOD:
                need = count * 8u;
                if (need > left) return fail("truncated SETTEXLOD");
                break;
            case DP2_SETCLIPPLANE:
                need = count * 20u;
                if (need > left) return fail("truncated SETCLIPPLANE");
                for (uint32_t i = 0; i < count; i++) {
                    float pl[4]; memcpy(pl, q + 20 * i + 4, sizeof pl);
                    if (u32(q + 20 * i) < 6) x.dev->SetClipPlane(u32(q + 20 * i), pl);
                }
                break;
            case D3DPT_DP2_DRAW8:
                if (!draw8(q, left, need)) return false;
                break;
            case DP2_SETVERTEXSHADER:                            /* an FVF, or a shader handle (bit 0) we do not claim */
                need = count * 4u;
                if (need > left) return fail("truncated SETVERTEXSHADER");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    tr("vertex shader 0x%x", h);
                    if (!(h & 1)) set_fvf(h);
                    else if (d.warn_once(0xc0000)) x.log("ddi: dp2: a vertex shader handle (0x%x) with no shader support", h);
                }
                break;
            case DP2_CREATEVERTEXSHADER: case DP2_CREATEPIXELSHADER: case DP2_SETVERTEXSHADERCONST: case DP2_SETPIXELSHADERCONST: {
                /* variable: handle [, decl size], code size / register, count */
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated shader token");
                    if (op == DP2_CREATEVERTEXSHADER) { if (left < need + 12) return fail("truncated CREATEVERTEXSHADER"); need += 12 + (size_t)u32(q + need + 4) + u32(q + need + 8); }
                    else if (op == DP2_CREATEPIXELSHADER) need += 8 + (size_t)u32(q + need + 4);
                    else need += 8 + (size_t)u32(q + need + 4) * 16;
                    if (need > left) return fail("truncated shader token");
                }
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: token %u (shaders) is not supported: dropped", op);
                break;
            }
            case DP2_DELETEVERTEXSHADER: case DP2_DELETEPIXELSHADER: case DP2_SETPIXELSHADER:
                need = count * 4u;
                if (need > left) return fail("truncated shader handle token");
                if (op == DP2_SETPIXELSHADER && count && u32(q) && d.warn_once(0xc0000 | op)) x.log("ddi: dp2: a pixel shader handle with no shader support");
                break;
            case DP2_SETSTREAMSOURCE: case DP2_DRAWPRIMITIVE: case DP2_DRAWPRIMITIVE2: case DP2_CLIPPEDTRIANGLEFAN:
                need = count * 12u;
                if (need > left) return fail("truncated DX8 stream token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: DX8 token %u reached the host (the driver did not rewrite it)", op);
                break;
            case DP2_SETSTREAMSOURCEUM: case DP2_SETINDICES:
                need = count * 8u;
                if (need > left) return fail("truncated DX8 stream token");
                break;
            case DP2_DRAWINDEXEDPRIMITIVE: case DP2_DRAWINDEXEDPRIMITIVE2:
                need = count * 24u;
                if (need > left) return fail("truncated DX8 draw token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: DX8 token %u reached the host (the driver did not rewrite it)", op);
                break;
            case DP2_DRAWRECTPATCH: case DP2_DRAWTRIPATCH: {
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated patch token");
                    uint32_t fl = u32(q + need + 4);
                    need += 8 + ((fl & 1) ? (op == DP2_DRAWRECTPATCH ? 16 : 12) : 0) + ((fl & 2) ? (op == DP2_DRAWRECTPATCH ? 28 : 16) : 0);
                }
                if (need > left) return fail("truncated patch token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: patches are not supported: dropped");
                break;
            }
            case DP2_VOLUMEBLT:
                need = count * 48u;
                if (need > left) return fail("truncated VOLUMEBLT");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: volume textures are not supported: VOLUMEBLT dropped");
                break;
            case DP2_BUFFERBLT:
                need = count * 24u;
                if (need > left) return fail("truncated BUFFERBLT");
                break;
            case DP2_ADDDIRTYRECT:
                need = count * 20u;
                if (need > left) return fail("truncated ADDDIRTYRECT");
                break;
            case DP2_ADDDIRTYBOX:
                need = count * 28u;
                if (need > left) return fail("truncated ADDDIRTYBOX");
                break;
            default:
                return fail_token(op, count);
            }
            p = q + need;
        }
        return true;
    }
};

} // namespace

void exec_ddi_release(Exec &x)
{
    if (!x.ddi) return;
    for (auto &kv : x.ddi->surfs) kv.second.release();
    for (auto &kv : x.ddi->sblocks) if (kv.second) kv.second->Release();
    x.ddi->sblocks.clear();
    x.ddi->drop_stage();
    delete x.ddi;
    x.ddi = nullptr;
}

bool exec_ddi_op(Batch &b, const d3dpt_cmd *c)
{
    Exec &x = b.x;
    switch (c->op) {
    case D3DPT_OP_VRAM_SURFACE: {
        auto *a = body<d3dpt_vram_surface>(c, 0, b); if (!a) return true;
        if (!x.vram) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        uint32_t levels = a->levels ? a->levels : 1;
        uint32_t row = fmt_row_bytes(a->format, a->width), rows = fmt_rows(a->format, a->height);
        if (!a->handle || !a->width || !a->height || a->width > 8192 || a->height > 8192 || levels > 16 ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + (levels - 1) * sizeof(d3dpt_u32x2)) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        if (!row) {
            if (ddi(x).warn_once(0x70000 | a->format)) x.log("ddi: surface format %u (0x%08x) not mirrored", a->format, a->format);
            return true;
        }
        if (a->pitch < row || (uint64_t)a->offset + (uint64_t)a->pitch * rows > x.vram_size) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        const d3dpt_u32x2 *lv = (const d3dpt_u32x2 *)tail(a);
        for (uint32_t l = 1; l < levels; l++) {
            uint32_t w = a->width >> l, h = a->height >> l;
            if (!w) w = 1;
            if (!h) h = 1;
            if (lv[l - 1].b < fmt_row_bytes(a->format, w) ||
                (uint64_t)lv[l - 1].a + (uint64_t)lv[l - 1].b * fmt_rows(a->format, h) > x.vram_size) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        }
        Ddi &d = ddi(x);
        VramSurf &s = d.surfs[a->handle];
        d3dpt_vram_surface nd = *a;
        nd.levels = levels;
        /* the same surface again (a flip moved it): keep the host object if it still fits */
        if ((s.tex || s.rt) && (s.d.width != nd.width || s.d.height != nd.height || s.d.format != nd.format ||
                                s.d.caps != nd.caps || s.d.levels != nd.levels)) s.release();
        if (s.d.offset != nd.offset || s.d.pitch != nd.pitch) s.dirty = true;
        if (!s.tex && !s.rt) s.dirty = true;
        s.d = nd;
        s.levels.assign(lv, lv + (levels - 1));
        break;
    }
    case D3DPT_OP_VRAM_RELEASE: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        if (!x.ddi) return true;
        auto it = x.ddi->surfs.find(a->handle);
        if (it == x.ddi->surfs.end()) return true;
        if (x.ddi->bound_rt == a->handle || x.ddi->bound_z == a->handle) {
            /* the device may still reference it: unbind first */
            if (x.dev) { x.dev->SetDepthStencilSurface(nullptr); }
            x.ddi->bound_rt = x.ddi->bound_z = 0;
        }
        it->second.release();
        x.ddi->surfs.erase(it);
        break;
    }
    case D3DPT_OP_VRAM_DIRTY: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->handle);
        if (s) { s->dirty = true; s->rendered = false; }
        break;
    }
    case D3DPT_OP_CTX_CREATE: {
        auto *a = body<d3dpt_ctx_create>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        Ddi &d = ddi(x);
        if (!a->handle || d.ctxs.count(a->handle)) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        VramSurf *rt = surf(x, a->rt);
        if (!rt) { r->hr = (uint32_t)D3DERR_INVALIDCALL; x.log("ddi: context %u: render target %u unknown", a->handle, a->rt); return true; }
        Ctx cx;
        cx.rt = a->rt;
        cx.z = surf(x, a->z) ? a->z : 0;
        cx.vp = { 0, 0, rt->d.width, rt->d.height, 0.0f, 1.0f };
        if (!ensure_object(x, *rt) || (cx.z && !ensure_object(x, *surf(x, cx.z)))) { r->hr = (uint32_t)E_FAIL; return true; }
        d.ctxs[a->handle] = cx;
        x.log("ddi: context %u on %ux%u fmt %u (z %u), %zu contexts", a->handle, rt->d.width, rt->d.height, rt->d.format, cx.z, d.ctxs.size());
        r->hr = S_OK;
        break;
    }
    case D3DPT_OP_CTX_DESTROY: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        if (x.ddi) x.ddi->ctxs.erase(a->handle);
        break;
    }
    case D3DPT_OP_CTX_SET_RT: {
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a) return true;
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->a);
        if (it == x.ddi->ctxs.end() || !surf(x, a->b)) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        it->second.rt = a->b;
        it->second.z = surf(x, a->c) ? a->c : 0;
        VramSurf *rt = surf(x, a->b);
        it->second.vp = { 0, 0, rt->d.width, rt->d.height, 0.0f, 1.0f };
        bind_ctx(x, *x.ddi, it->second, b, false);
        break;
    }
    case D3DPT_OP_CTX_CLEAR: {
        auto *a = body<d3dpt_ctx_clear>(c, 0, b); if (!a) return true;
        if (a->count > 64 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->count * sizeof(D3DRECT)) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->ctx);
        if (it == x.ddi->ctxs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        if (!x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        Dp2 p = { x, *x.ddi, it->second, b, nullptr, nullptr, nullptr, 0, 0 };
        p.clear(a->flags, a->color, a->z, a->stencil, a->count, tail(a));
        break;
    }
    case D3DPT_OP_DP2: {
        auto *a = body<d3dpt_dp2>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        uint64_t cmd_aligned = D3DPT_ALIGN8(a->command_bytes);
        uint32_t stride = stride_of_fvf(a->fvf);
        if (a->command_bytes > (48u << 20) || a->vertex_bytes > (32u << 20) ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + cmd_aligned + a->vertex_bytes) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        /* a record without vertices (DX8: every draw carries its own) may name any FVF */
        if (a->vertex_bytes && (!stride || stride != a->vertex_stride || a->vertex_bytes % stride)) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->ctx);
        if (it == x.ddi->ctxs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        if (!x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        const uint8_t *cmds = tail(a);
        Dp2 p = { x, *x.ddi, it->second, b, cmds, cmds + a->command_bytes, cmds + cmd_aligned, stride, stride ? a->vertex_bytes / stride : 0, a->fvf };
        x.ddi->dp2_calls++;
        if (x.ddi->trace_flag && !x.ddi->trace && !x.ddi->trace_armed && access(x.ddi->trace_flag, F_OK) == 0) {
            x.ddi->trace_armed = true;                 /* the trace starts with the next frame */
            x.log("ddi: trace: armed at dp2 call %u", x.ddi->dp2_calls);
        }
        if (x.ddi->trace) x.log("ddi: trace: dp2 call %u: ctx %u flags 0x%x fvf 0x%x stride %u, %u vertices, %u command bytes", x.ddi->dp2_calls, a->ctx, a->flags, a->fvf, stride, stride ? a->vertex_bytes / stride : 0, a->command_bytes);
        if (stride) { x.dev->SetFVF(a->fvf); p.cur_fvf = a->fvf; }
        bool ok = p.run();
        r->hr = b.err ? (uint32_t)E_FAIL : ok ? (uint32_t)S_OK : (uint32_t)p.hr;
        r->bytes = ok ? 0 : p.pos;
        break;
    }
    case D3DPT_OP_READBACK: {
        auto *a = body<d3dpt_sync>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        VramSurf *s = surf(x, a->handle);
        if (!s) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return true; }
        bool had = s->rendered;
        r->hr = had ? (uint32_t)readback(x, *x.ddi, *s) : (uint32_t)S_FALSE;
        if (x.ddi->trace && had) {
            x.ddi->trace = false;
            unlink(x.ddi->trace_flag);
            x.log("ddi: trace: frame ends (readback of %u)", a->handle);
        } else if (x.ddi->trace_armed && had) {
            Ddi &d = *x.ddi;
            d.trace_armed = false;
            d.trace = true;
            d.trace_draws = 0;
            x.log("ddi: trace: frame starts after dp2 call %u; the states set so far:", d.dp2_calls);
            char line[400]; size_t n = 0;
            for (uint32_t i = 0; i < 256; i++) if (d.rs_set[i]) {
                n += (size_t)snprintf(line + n, sizeof line - n, " %u=0x%x", i, d.rs_val[i]);
                if (n > sizeof line - 40) { x.log("ddi: trace:   rs%s", line); n = 0; }
            }
            if (n) x.log("ddi: trace:   rs%s", line);
            for (uint32_t st = 0; st < 8; st++) {
                n = 0;
                for (uint32_t i = 0; i < 33; i++) if (d.tss_set[st][i]) n += (size_t)snprintf(line + n, sizeof line - n, " %u=0x%x", i, d.tss_val[st][i]);
                if (n) x.log("ddi: trace:   tss %u:%s", st, line);
            }
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

} // namespace d3dpt
