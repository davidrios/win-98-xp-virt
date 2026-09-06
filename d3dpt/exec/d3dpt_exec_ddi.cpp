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
 * The DX8 DDI adds the driver's self-contained D3DPT_DP2_DRAW8 draws and,
 * with protocol v7, the shader tokens: CREATEVERTEXSHADER becomes a d3d9
 * vertex declaration (+ a vertex shader with dcl instructions in front),
 * CREATEPIXELSHADER a d3d9 pixel shader, the constants go straight
 * through; a DRAW8 under a shader carries the handle in its fvf field.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt_exec_int.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#ifndef _WIN32
#include <unistd.h>
#endif

/* `access(p, F_OK)` without unistd.h, which Windows does not have; the
 * trace dumps are the only thing that asks. */
static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

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
    /* a render target's VRAM as of the last time the host and VRAM agreed
     * (after an upload or a readback): what differs from it later was
     * written by the guest without a VRAM_DIRTY — GDI on the surface's DC
     * (GetDC bypasses DdLock / DdUnlock), a title's own writes through a
     * cached pointer — and is uploaded before the frame's first draw or
     * kept over the host's pixels at the readback (doc 15 "Untracked
     * writes") */
    std::vector<uint8_t> shadow;
    bool checked = false;               /* the untracked-write check ran since the last readback */
    /* v8: a P8 texture's palette (SETPALETTE: the runtime's palette handle,
     * whether its entries carry alpha) and a source colour key
     * (VRAM_COLORKEY): both make the host texture an A8R8G8B8 expansion of
     * the VRAM texels (host_format) */
    uint32_t palette = 0;
    bool pal_alpha = false;
    bool ckey = false;
    uint32_t ckey_lo = 0, ckey_hi = 0;
    D3DFORMAT host_fmt = D3DFMT_UNKNOWN;    /* the format tex was created in */
    void release() {
        if (tex) tex->Release();
        if (rt) rt->Release();
        tex = nullptr; rt = nullptr;
        host_fmt = D3DFMT_UNKNOWN;
    }
};

struct Palette { uint32_t argb[256]; };

/* a DX8 vertex shader (CREATEVERTEXSHADER, protocol v7): the D3DVSD_*
 * declaration as a d3d9 vertex declaration, the function (if any) as a
 * d3d9 vertex shader with the dcl instructions d3d9 wants in front, the
 * declaration's D3DVSD_CONST runs (loaded when the shader is set). A
 * declaration without a function is the fixed function on that layout. */
struct VShader8 {
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    struct ConstRun { uint32_t reg; std::vector<float> v; };
    std::vector<ConstRun> consts;
    uint32_t vertex_bytes = 0;          /* what the declaration reads of a stream-0 vertex */
    uint32_t streams = 0;               /* bitmask of the streams it reads */
    void release() {
        if (decl) decl->Release();
        if (vs) vs->Release();
        decl = nullptr; vs = nullptr;
    }
};

struct Ctx {
    uint32_t rt = 0, z = 0;
    D3DVIEWPORT9 vp = { 0, 0, 0, 0, 0.0f, 1.0f };
    /* the DX8 shaders by the runtime's handle (per device = per context) */
    std::unordered_map<uint32_t, VShader8> vshaders;
    std::unordered_map<uint32_t, IDirect3DPixelShader9 *> pshaders;
    void release_shaders() {
        for (auto &kv : vshaders) kv.second.release();
        for (auto &kv : pshaders) if (kv.second) kv.second->Release();
        vshaders.clear(); pshaders.clear();
    }
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
    /* v8: the runtime's palettes (UPDATEPALETTE) by handle, and colour
     * keying: render state 41, the surface bound at stage 0, whether the
     * alpha test is forced on for its key (the app's own alpha test state
     * is restored when it is not) */
    std::unordered_map<uint32_t, Palette> palettes;
    uint32_t ckey_rs = 0, stage_tex[8] = {};    /* the surface handle bound at each stage */
    bool ckey_forced = false, ckey_alpha_ovr = false;
    bool legacy_blend = false;          /* TEXTUREMAPBLEND set since the app's last explicit stage-0 op: the blend follows the texture */
    uint32_t pal_lines = 0;                     /* palette / colour-key events logged (the first few) */
    uint32_t dp2_calls = 0, draws = 0, readbacks = 0;
    uint32_t untracked = 0;                     /* target pixels the guest wrote without VRAM_DIRTY (uploaded or kept) */
    uint32_t untracked_lines = 0;               /* the first such events logged */
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
    uint32_t stat_dp2 = 0, stat_draws = 0, stat_rb = 0, stat_untracked = 0, stat_bw = 0, stat_bb = 0;
    uint32_t buffer_writes = 0, buffer_bytes = 0;   /* v9: VRAM_DIRTY_RANGE records (the guest's vertex / index buffer writes) */

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

/* the format the host texture is created in: P8 and colour-keyed textures
 * are expanded to A8R8G8B8 at upload (DXVK has no P8; the key becomes alpha 0) */
static D3DFORMAT host_format(const VramSurf &s) {
    if (s.d.format == D3DFMT_P8) return D3DFMT_A8R8G8B8;
    if (s.ckey && !fmt_dxt(s.d.format) && fmt_row_bytes(s.d.format, 1) && s.d.format != D3DFMT_A8R8G8B8) return D3DFMT_A8R8G8B8;
    return (D3DFORMAT)s.d.format;
}
static bool needs_expand(const VramSurf &s) { return host_format(s) != (D3DFORMAT)s.d.format || s.ckey; }
/* a format with an alpha channel of its own (the DX3 MODULATE blend reads the alpha from such a texture, else from the diffuse) */
static bool fmt_has_alpha(uint32_t f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8: case D3DFMT_A8R3G3B2:
    case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_A4L4:
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return true;
    default:
        return false;
    }
}

/* one texel of the VRAM formats the expansion handles, as A8R8G8B8 (raw = its VRAM value) */
static uint32_t texel_argb(uint32_t f, uint32_t raw, const Palette *pal, bool pal_alpha) {
    switch (f) {
    case D3DFMT_A8R8G8B8: return raw;
    case D3DFMT_X8R8G8B8: return raw | 0xff000000u;
    case D3DFMT_R5G6B5: return 0xff000000u | ((raw & 0xf800) << 8) | ((raw & 0xe000) << 3) | ((raw & 0x07e0) << 5) | ((raw & 0x0600) >> 1) | ((raw & 0x1f) << 3) | ((raw & 0x1c) >> 2);
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: {
        uint32_t c = ((raw & 0x7c00) << 9) | ((raw & 0x7000) << 4) | ((raw & 0x03e0) << 6) | ((raw & 0x0380) << 1) | ((raw & 0x1f) << 3) | ((raw & 0x1c) >> 2);
        return c | (f == D3DFMT_A1R5G5B5 && !(raw & 0x8000) ? 0u : 0xff000000u);
    }
    case D3DFMT_X4R4G4B4: case D3DFMT_A4R4G4B4: {
        uint32_t c = ((raw & 0xf00) << 12) | ((raw & 0xf00) << 8) | ((raw & 0x0f0) << 8) | ((raw & 0x0f0) << 4) | ((raw & 0x00f) << 4) | (raw & 0x00f);
        return c | (f == D3DFMT_A4R4G4B4 ? ((raw & 0xf000) << 16) | ((raw & 0xf000) << 12) : 0xff000000u);
    }
    case D3DFMT_P8: {
        uint32_t c = pal ? pal->argb[raw & 0xff] : (0xff000000u | (raw & 0xff) * 0x010101u);
        return pal && !pal_alpha ? c | 0xff000000u : c;
    }
    default: return 0xff000000u;
    }
}

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
    if (s.d.caps & D3DPT_VS_BUFFER) return false;       /* a vertex / index buffer: read from VRAM at each draw, no host object */
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
        s.host_fmt = host_format(s);
        hr = x.dev->CreateTexture(s.d.width, s.d.height, s.d.levels, 0, s.host_fmt, D3DPOOL_MANAGED, &s.tex, nullptr);
        if (FAILED(hr)) x.log("ddi: texture %ux%u fmt %u (host %u) levels %u: 0x%08x", s.d.width, s.d.height, s.d.format, s.host_fmt, s.d.levels, (unsigned)hr);
    }
    return SUCCEEDED(hr);
}

/* the host texture of a surface whose expansion changed (a colour key set or
 * cleared): recreated in the new format on the next use */
static void refresh_object(VramSurf &s) {
    if (s.tex && !s.rt && s.host_fmt != host_format(s)) s.release();
    s.dirty = true;
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

/* VRAM -> host texture (every level); a P8 or colour-keyed texture is
 * expanded texel by texel: the palette's colour, alpha 0 for a keyed value */
static void upload_texture(Exec &x, Ddi &d, VramSurf &s) {
    D3DLOCKED_RECT lr;
    bool expand = needs_expand(s) && s.host_fmt == D3DFMT_A8R8G8B8;
    const Palette *pal = nullptr;
    if (s.d.format == D3DFMT_P8) {
        auto it = d.palettes.find(s.palette);
        if (it != d.palettes.end()) pal = &it->second;
    }
    uint32_t bpp = fmt_row_bytes(s.d.format, 1);
    for (uint32_t l = 0; l < s.d.levels; l++) {
        uint32_t w = s.d.width >> l, h = s.d.height >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        uint32_t off = l ? s.levels[l - 1].a : s.d.offset, pitch = l ? s.levels[l - 1].b : s.d.pitch;
        uint32_t row = fmt_row_bytes(s.d.format, w), rows = fmt_rows(s.d.format, h);
        if (FAILED(s.tex->LockRect(l, &lr, nullptr, 0))) continue;
        if (!expand || (bpp != 1 && bpp != 2 && bpp != 4)) {
            copy_rows(lr.pBits, lr.Pitch, x.vram + off, pitch, row < (uint32_t)lr.Pitch ? row : (uint32_t)lr.Pitch, rows);
        } else {
            for (uint32_t yy = 0; yy < rows; yy++) {
                const uint8_t *src = x.vram + off + (size_t)yy * pitch;
                uint32_t *dst = (uint32_t *)((uint8_t *)lr.pBits + (size_t)yy * lr.Pitch);
                for (uint32_t xx = 0; xx < w; xx++) {
                    uint32_t raw;
                    if (bpp == 1) raw = src[xx];
                    else if (bpp == 2) { uint16_t v; memcpy(&v, src + xx * 2, 2); raw = v; }
                    else memcpy(&raw, src + xx * 4, 4);
                    uint32_t c = texel_argb(s.d.format, raw, pal, s.pal_alpha);
                    if (s.ckey && raw >= s.ckey_lo && raw <= s.ckey_hi) c &= 0x00ffffffu;
                    dst[xx] = c;
                }
            }
        }
        s.tex->UnlockRect(l);
    }
    s.dirty = false;
}

/* the target's VRAM rows into its shadow (host and VRAM agree from here) */
static void shadow_take(Exec &x, VramSurf &s) {
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width);
    s.shadow.resize((size_t)row * s.d.height);
    copy_rows(s.shadow.data(), row, x.vram + s.d.offset, s.d.pitch, row, s.d.height);
}

/* the target's VRAM differs from its shadow: the guest wrote it without a
 * VRAM_DIRTY (counted in pixels; 0 when there is no shadow yet) */
static uint32_t shadow_diff(Exec &x, VramSurf &s) {
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width), bpp = fmt_row_bytes(s.d.format, 1), n = 0;
    if (s.shadow.size() != (size_t)row * s.d.height || !bpp) return 0;
    for (uint32_t yy = 0; yy < s.d.height; yy++) {
        const uint8_t *v = x.vram + s.d.offset + (size_t)yy * s.d.pitch, *sh = s.shadow.data() + (size_t)yy * row;
        if (memcmp(v, sh, row) == 0) continue;
        for (uint32_t xx = 0; xx < row; xx += bpp) if (memcmp(v + xx, sh + xx, bpp) != 0) n++;
    }
    return n;
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
    shadow_take(x, s);
}

/* host render target -> VRAM. Pixels the guest changed since the shadow
 * was taken (untracked writes: GDI through GetDC, drawn after the scene
 * as a rule — a title's text and panels) stay over the host's. */
static HRESULT readback(Exec &x, Ddi &d, VramSurf &s) {
    if (!s.rt || (s.d.caps & D3DPT_VS_ZBUFFER)) return D3DERR_INVALIDCALL;
    if (!ensure_stage(x, d, s.d.width, s.d.height, (D3DFORMAT)s.d.format, false)) return E_FAIL;
    HRESULT hr = x.dev->GetRenderTargetData(s.rt, d.stage);
    if (FAILED(hr)) { x.log("ddi: readback: GetRenderTargetData 0x%08x", (unsigned)hr); return hr; }
    D3DLOCKED_RECT lr;
    if (FAILED(d.stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return E_FAIL;
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width), bpp = fmt_row_bytes(s.d.format, 1), kept = 0;
    bool have = s.shadow.size() == (size_t)row * s.d.height && bpp;
    if (!have) s.shadow.resize((size_t)row * s.d.height);
    for (uint32_t yy = 0; yy < s.d.height; yy++) {
        uint8_t *v = x.vram + s.d.offset + (size_t)yy * s.d.pitch, *sh = s.shadow.data() + (size_t)yy * row;
        const uint8_t *h = (const uint8_t *)lr.pBits + (size_t)yy * lr.Pitch;
        if (have && memcmp(v, sh, row) != 0) {
            for (uint32_t xx = 0; xx < row; xx += bpp) {
                if (memcmp(v + xx, sh + xx, bpp) != 0) kept++;      /* the guest's pixel stays */
                else memcpy(v + xx, h + xx, bpp);
            }
        } else memcpy(v, h, row);
        memcpy(sh, v, row);
    }
    d.stage->UnlockRect();
    if (kept) {
        d.untracked += kept;
        if (d.untracked_lines < 8) { d.untracked_lines++; x.log("ddi: target %u: %u pixels written by the guest since the last frame without VRAM_DIRTY, kept over the host frame", s.d.handle, kept); }
        if (d.trace) x.log("ddi: trace: readback of %u keeps %u untracked guest pixels", s.d.handle, kept);
    }
    s.rendered = false;
    s.dirty = kept != 0;                /* the host frame lacks the kept pixels: refreshed from VRAM before the next draw */
    s.checked = false;
    d.readbacks++;
    auto now = std::chrono::steady_clock::now();
    if (d.stat_t0 == std::chrono::steady_clock::time_point{}) d.stat_t0 = now;
    double dt = std::chrono::duration<double>(now - d.stat_t0).count();
    if (dt >= 5.0) {
        char extra[128] = "";
        size_t n = 0;
        if (d.untracked != d.stat_untracked) n += (size_t)snprintf(extra + n, sizeof extra - n, ", %u untracked guest pixels", d.untracked - d.stat_untracked);
        if (d.buffer_writes != d.stat_bw) snprintf(extra + n, sizeof extra - n, ", %u buffer writes of %u KiB", d.buffer_writes - d.stat_bw, (d.buffer_bytes - d.stat_bb) >> 10);
        x.log("ddi: %.1f frames/s (%u readbacks, %u dp2 calls, %u draws%s in %.1f s)",
              (d.readbacks - d.stat_rb) / dt, d.readbacks - d.stat_rb, d.dp2_calls - d.stat_dp2, d.draws - d.stat_draws, extra, dt);
        d.stat_t0 = now;
        d.stat_rb = d.readbacks; d.stat_dp2 = d.dp2_calls; d.stat_draws = d.draws; d.stat_untracked = d.untracked;
        d.stat_bw = d.buffer_writes; d.stat_bb = d.buffer_bytes;
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
        /* once per frame, before its first draw: VRAM the guest changed
         * without telling (GDI on the surface's DC) goes into the target
         * like a tracked write would */
        if (!rt->dirty && !rt->checked) {
            uint32_t n = shadow_diff(x, *rt);
            if (n) {
                rt->dirty = true;
                d.untracked += n;
                if (d.untracked_lines < 8) { d.untracked_lines++; x.log("ddi: target %u: %u pixels written by the guest without VRAM_DIRTY, uploaded before the frame's draws", rt->d.handle, n); }
            }
        }
        rt->checked = true;
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

/* --- DX8 vertex shader declarations (D3DVSD_* tokens) -> d3d9 elements.
 * The register number of a D3DVSD_REG is the shader's input register and,
 * by the DX8 convention the fixed function relies on, names the usage
 * (0 position, 3 normal, 5 diffuse, 7.. texcoords); the d3d9 declaration
 * and the dcl instructions prepended to the function use the same table,
 * so a shader reading v5 finds the element declared as COLOR0. Data types
 * 0..7 are numbered as D3DDECLTYPE_*. --- */
static const struct { uint8_t usage, index; } vsde_usage[17] = {
    { D3DDECLUSAGE_POSITION, 0 }, { D3DDECLUSAGE_BLENDWEIGHT, 0 }, { D3DDECLUSAGE_BLENDINDICES, 0 }, { D3DDECLUSAGE_NORMAL, 0 },
    { D3DDECLUSAGE_PSIZE, 0 }, { D3DDECLUSAGE_COLOR, 0 }, { D3DDECLUSAGE_COLOR, 1 }, { D3DDECLUSAGE_TEXCOORD, 0 },
    { D3DDECLUSAGE_TEXCOORD, 1 }, { D3DDECLUSAGE_TEXCOORD, 2 }, { D3DDECLUSAGE_TEXCOORD, 3 }, { D3DDECLUSAGE_TEXCOORD, 4 },
    { D3DDECLUSAGE_TEXCOORD, 5 }, { D3DDECLUSAGE_TEXCOORD, 6 }, { D3DDECLUSAGE_TEXCOORD, 7 }, { D3DDECLUSAGE_POSITION, 1 }, { D3DDECLUSAGE_NORMAL, 1 },
};
static const uint8_t vsdt_size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };

/* ntokens DWORDs of declaration; false = malformed (no END, a bad register
 * or type, too many elements). regs: bitmask of the input registers fed */
static bool vsd_convert(const uint32_t *d, uint32_t ntokens, std::vector<D3DVERTEXELEMENT9> &el, uint32_t &regs, VShader8 &s) {
    uint32_t i = 0, stream = 0, offset = 0;
    regs = 0; s.vertex_bytes = 0; s.streams = 0;
    while (i < ntokens) {
        uint32_t t = d[i], type = (t >> 29) & 7;
        if (t == 0xFFFFFFFFu) {                                             /* D3DVSD_END */
            D3DVERTEXELEMENT9 end = D3DDECL_END();
            el.push_back(end);
            return true;
        }
        switch (type) {
        case 0: i++; break;                                                 /* NOP */
        case 1: stream = t & 0xF; offset = 0; i++; break;                  /* STREAM (the tessellator bit ignored) */
        case 2:
            if (t & (1u << 28)) { offset += 4 * ((t >> 16) & 0xF); i++; break; }   /* SKIP */
            {
                uint32_t reg = t & 0x1F, dt = (t >> 16) & 0xF;
                if (reg > 16 || dt > 7 || el.size() >= 64) return false;
                D3DVERTEXELEMENT9 e = { (WORD)stream, (WORD)offset, (BYTE)dt, D3DDECLMETHOD_DEFAULT, vsde_usage[reg].usage, vsde_usage[reg].index };
                el.push_back(e);
                regs |= 1u << reg;
                s.streams |= 1u << stream;
                offset += vsdt_size[dt];
                if (stream == 0 && offset > s.vertex_bytes) s.vertex_bytes = offset;
                i++;
            }
            break;
        case 3: i++; break;                                                 /* TESSELLATOR: patches are not supported */
        case 4: {                                                           /* CONST: count float4s at register */
            uint32_t cnt = (t >> 25) & 0xF, reg = t & 0x7F;
            if (i + 1 + cnt * 4 > ntokens || reg + cnt > 256) return false;
            VShader8::ConstRun run; run.reg = reg;
            for (uint32_t j = 0; j < cnt * 4; j++) { float f; memcpy(&f, &d[i + 1 + j], 4); run.v.push_back(f); }
            if (cnt) s.consts.push_back(run);
            i += 1 + cnt * 4;
            break;
        }
        case 5: i += 1 + ((t >> 24) & 0x1F); break;                         /* EXT: skipped with its DWORDs */
        default: return false;
        }
    }
    return false;                                                           /* no END */
}

/* --- shader model 1.x bytecode validation. DXVK's compiler asserts on an
 * unknown opcode or a missing operand (an abort, uncatchable: QEMU dies),
 * so only a vs_1_x / ps_1_x stream of known instructions with the right
 * operand counts and registers in range reaches CreateVertexShader /
 * CreatePixelShader. The DX8 runtime validates every shader before its
 * CREATE token, so a real guest never trips this. Tokens: bit 31 set =
 * a parameter (register type in bits 28..30, number in 0..10), clear =
 * an instruction (opcode in bits 0..15); DEF carries four raw floats,
 * COMMENT its length. --- */
struct Sm1Op { uint16_t op; uint8_t vs, ps, min, max; };     /* stage allowed; operands including the destination */
static const Sm1Op sm1_ops[] = {
    { 0, 1, 1, 0, 0 },      /* NOP */
    { 1, 1, 1, 2, 2 },      /* MOV */
    { 2, 1, 1, 3, 3 },      /* ADD */
    { 3, 1, 1, 3, 3 },      /* SUB */
    { 4, 1, 1, 4, 4 },      /* MAD */
    { 5, 1, 1, 3, 3 },      /* MUL */
    { 6, 1, 0, 2, 2 },      /* RCP */
    { 7, 1, 0, 2, 2 },      /* RSQ */
    { 8, 1, 1, 3, 3 },      /* DP3 */
    { 9, 1, 1, 3, 3 },      /* DP4 */
    { 10, 1, 0, 3, 3 },     /* MIN */
    { 11, 1, 0, 3, 3 },     /* MAX */
    { 12, 1, 0, 3, 3 },     /* SLT */
    { 13, 1, 0, 3, 3 },     /* SGE */
    { 14, 1, 0, 2, 2 },     /* EXP */
    { 15, 1, 0, 2, 2 },     /* LOG */
    { 16, 1, 0, 2, 2 },     /* LIT */
    { 17, 1, 0, 3, 3 },     /* DST */
    { 18, 0, 1, 4, 4 },     /* LRP */
    { 19, 1, 0, 2, 2 },     /* FRC */
    { 20, 1, 0, 3, 3 },     /* M4x4 */
    { 21, 1, 0, 3, 3 },     /* M4x3 */
    { 22, 1, 0, 3, 3 },     /* M3x4 */
    { 23, 1, 0, 3, 3 },     /* M3x3 */
    { 24, 1, 0, 3, 3 },     /* M3x2 */
    { 64, 0, 1, 1, 2 },     /* TEXCOORD (ps 1.4 texcrd: + source) */
    { 65, 0, 1, 1, 1 },     /* TEXKILL */
    { 66, 0, 1, 1, 2 },     /* TEX (ps 1.4 texld: + source) */
    { 67, 0, 1, 2, 2 },     /* TEXBEM */
    { 68, 0, 1, 2, 2 },     /* TEXBEML */
    { 69, 0, 1, 2, 2 },     /* TEXREG2AR */
    { 70, 0, 1, 2, 2 },     /* TEXREG2GB */
    { 71, 0, 1, 2, 2 },     /* TEXM3x2PAD */
    { 72, 0, 1, 2, 2 },     /* TEXM3x2TEX */
    { 73, 0, 1, 2, 2 },     /* TEXM3x3PAD */
    { 74, 0, 1, 2, 2 },     /* TEXM3x3TEX */
    { 76, 0, 1, 3, 3 },     /* TEXM3x3SPEC */
    { 77, 0, 1, 2, 2 },     /* TEXM3x3VSPEC */
    { 78, 1, 0, 2, 2 },     /* EXPP */
    { 79, 1, 0, 2, 2 },     /* LOGP */
    { 80, 0, 1, 4, 4 },     /* CND */
    { 81, 1, 1, 5, 5 },     /* DEF: destination + four floats */
    { 82, 0, 1, 2, 2 },     /* TEXREG2RGB */
    { 83, 0, 1, 2, 2 },     /* TEXDP3TEX */
    { 84, 0, 1, 2, 2 },     /* TEXM3x2DEPTH */
    { 85, 0, 1, 2, 2 },     /* TEXDP3 */
    { 86, 0, 1, 2, 2 },     /* TEXM3x3 */
    { 87, 0, 1, 1, 1 },     /* TEXDEPTH */
    { 88, 0, 1, 4, 4 },     /* CMP */
    { 89, 0, 1, 3, 3 },     /* BEM */
};

/* a parameter token's register within the stage's file sizes */
static bool sm1_reg_ok(uint32_t t, bool vs, uint32_t minor) {
    uint32_t type = (t >> 28) & 7, num = t & 0x7ff;
    if (t & 0x1800) return false;                                   /* SM2 register type bits */
    if (vs) {
        switch (type) {
        case 0: return num < 12;                                    /* r */
        case 1: return num < 16;                                    /* v */
        case 2: return num < 256;                                   /* c (the runtime caps it at the driver's count) */
        case 3: return num < 1;                                     /* a0 */
        case 4: return num < 3;                                     /* oPos, oFog, oPts */
        case 5: return num < 2;                                     /* oD0, oD1 */
        case 6: return num < 8;                                     /* oT0..7 */
        default: return false;
        }
    }
    switch (type) {
    case 0: return num < (minor >= 4 ? 6u : 2u);                    /* r */
    case 1: return num < 2;                                         /* v */
    case 2: return num < 8;                                         /* c */
    case 3: return num < (minor >= 4 ? 6u : 4u);                    /* t */
    default: return false;
    }
}

static bool sm1_valid(const uint32_t *t, size_t n, bool vs) {
    if (n < 2 || (t[0] >> 16) != (vs ? 0xfffeu : 0xffffu) || ((t[0] >> 8) & 0xff) != 1 || t[n - 1] != 0x0000ffffu) return false;
    uint32_t minor = t[0] & 0xff;
    if (vs ? minor > 1 : minor > 4) return false;
    size_t i = 1, end = n - 1;
    while (i < end) {
        uint32_t ins = t[i];
        if (ins & 0x80000000u) return false;                        /* a parameter where an instruction should be */
        uint32_t op = ins & 0xffff;
        if (op == 0xfffe) {                                         /* COMMENT: its length in DWORDs */
            i += 1 + ((ins >> 16) & 0x7fff);
            if (i > end) return false;
            continue;
        }
        if (op == 0xfffd) {                                         /* PHASE: ps 1.4 */
            if (vs || minor != 4) return false;
            i++;
            continue;
        }
        const Sm1Op *o = nullptr;
        for (const Sm1Op &e : sm1_ops) if (e.op == op) { o = &e; break; }
        if (!o || !(vs ? o->vs : o->ps)) return false;
        i++;
        if (op == 81) {                                             /* DEF: a constant register, then raw floats */
            if (i + 5 > end || !(t[i] & 0x80000000u) || ((t[i] >> 28) & 7) != 2 || !sm1_reg_ok(t[i], vs, minor)) return false;
            i += 5;
            continue;
        }
        uint32_t got = 0;
        while (i < end && (t[i] & 0x80000000u)) {
            if (!sm1_reg_ok(t[i], vs, minor)) return false;
            got++; i++;
        }
        if (got < o->min || got > o->max) return false;
        if (got && ((t[i - got] >> 28) & 7) == (vs ? 2u : 2u) && op != 0) return false;   /* a constant as the destination */
    }
    return i == end;
}

struct Dp2 {
    Exec &x; Ddi &d; Ctx &c; Batch &b;
    const uint8_t *cmd, *cmd_end, *vtx;
    uint32_t stride, nverts, fvf_;
    uint32_t cur_vs = ~0u;      /* the SETVERTEXSHADER value last applied (FVF or shader handle); ~0 = unknown */
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
        if (file_exists(path)) return;   /* one dump per handle */
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
    /* SETVERTEXSHADER: an FVF (the fixed function on it), or a shader
     * handle (bit 0): its declaration, its function or none (the fixed
     * function on the declaration), its declaration constants */
    void apply_vs(uint32_t h) {
        if (h == cur_vs) return;
        auto it = c.vshaders.find(h);
        if (it != c.vshaders.end()) {
            VShader8 &s = it->second;
            x.dev->SetVertexDeclaration(s.decl);
            x.dev->SetVertexShader(s.vs);
            for (const auto &r : s.consts) x.dev->SetVertexShaderConstantF(r.reg, r.v.data(), (UINT)(r.v.size() / 4));
        } else if (!(h & 1)) {
            x.dev->SetVertexShader(nullptr);
            x.dev->SetFVF(h);
        } else {
            if (d.warn_once(0xc0000)) x.log("ddi: dp2: vertex shader handle 0x%x unknown (draws with it are skipped)", h);
            return;
        }
        cur_vs = h;
    }
    /* CREATEVERTEXSHADER: handle, the declaration tokens, the function (may be empty) */
    void create_vshader(uint32_t handle, const uint8_t *decl, uint32_t declbytes, const uint8_t *code, uint32_t codebytes) {
        auto old = c.vshaders.find(handle);
        if (old != c.vshaders.end()) { old->second.release(); c.vshaders.erase(old); }
        if (handle == cur_vs) cur_vs = ~0u;
        if (!handle || declbytes < 4 || declbytes % 4 || declbytes > (64u << 10) || codebytes % 4 || codebytes > (256u << 10) ||
            (codebytes && (codebytes < 8 || u32(code) >> 16 != 0xfffe || u32(code + codebytes - 4) != 0x0000ffffu))) {
            if (d.warn_once(0xc1000)) x.log("ddi: dp2: vertex shader 0x%x refused: declaration %u bytes, function %u bytes", handle, declbytes, codebytes);
            return;
        }
        std::vector<uint32_t> dt(declbytes / 4);
        memcpy(dt.data(), decl, declbytes);
        VShader8 s;
        std::vector<D3DVERTEXELEMENT9> el;
        uint32_t regs = 0;
        if (!vsd_convert(dt.data(), (uint32_t)dt.size(), el, regs, s)) {
            if (d.warn_once(0xc1001)) x.log("ddi: dp2: vertex shader 0x%x: malformed declaration (%u tokens)", handle, declbytes / 4);
            return;
        }
        HRESULT hr = x.dev->CreateVertexDeclaration(el.data(), &s.decl);
        if (FAILED(hr) || !s.decl) {
            if (d.warn_once(0xc1002)) x.log("ddi: dp2: vertex shader 0x%x: CreateVertexDeclaration 0x%08x (%zu elements)", handle, (unsigned)hr, el.size() - 1);
            return;
        }
        if (codebytes) {
            std::vector<uint32_t> g(codebytes / 4);
            memcpy(g.data(), code, codebytes);
            if (!sm1_valid(g.data(), g.size(), true)) {
                if (d.warn_once(0xc1008)) x.log("ddi: dp2: vertex shader 0x%x: the function is not valid vs 1.x (version 0x%08x, %u bytes), refused", handle, u32(code), codebytes);
                s.release();
                return;
            }
            /* a dcl per input register the declaration feeds, in front of the
             * function (d3d9 wants them; DX8 shaders have none) */
            std::vector<uint32_t> f;
            f.push_back(u32(code));
            for (uint32_t r = 0; r <= 16; r++) if (regs & (1u << r)) {
                f.push_back(0x0000001Fu);
                f.push_back(0x80000000u | vsde_usage[r].usage | ((uint32_t)vsde_usage[r].index << 16));
                f.push_back(0x900F0000u | r);
            }
            for (uint32_t i = 4; i < codebytes; i += 4) f.push_back(u32(code + i));
            hr = x.dev->CreateVertexShader((const DWORD *)f.data(), &s.vs);
            if (FAILED(hr) || !s.vs) {
                if (d.warn_once(0xc1003)) x.log("ddi: dp2: vertex shader 0x%x: CreateVertexShader 0x%08x (version 0x%08x, %u bytes)", handle, (unsigned)hr, u32(code), codebytes);
                s.release();
                return;
            }
        }
        tr("vertex shader 0x%x created: %zu elements, %u bytes on stream 0, streams 0x%x, %s function, %zu constant runs",
           handle, el.size() - 1, s.vertex_bytes, s.streams, codebytes ? "a" : "no", s.consts.size());
        c.vshaders[handle] = s;
    }
    void create_pshader(uint32_t handle, const uint8_t *code, uint32_t codebytes) {
        auto old = c.pshaders.find(handle);
        if (old != c.pshaders.end()) { if (old->second) old->second->Release(); c.pshaders.erase(old); }
        if (!handle || codebytes < 8 || codebytes % 4 || codebytes > (256u << 10) || u32(code) >> 16 != 0xffff || u32(code + codebytes - 4) != 0x0000ffffu) {
            if (d.warn_once(0xc1004)) x.log("ddi: dp2: pixel shader 0x%x refused: %u bytes", handle, codebytes);
            return;
        }
        std::vector<uint32_t> f(codebytes / 4);
        memcpy(f.data(), code, codebytes);
        if (!sm1_valid(f.data(), f.size(), false)) {
            if (d.warn_once(0xc1009)) x.log("ddi: dp2: pixel shader 0x%x is not valid ps 1.x (version 0x%08x, %u bytes), refused", handle, u32(code), codebytes);
            return;
        }
        IDirect3DPixelShader9 *ps = nullptr;
        HRESULT hr = x.dev->CreatePixelShader((const DWORD *)f.data(), &ps);
        if (FAILED(hr) || !ps) {
            if (d.warn_once(0xc1005)) x.log("ddi: dp2: pixel shader 0x%x: CreatePixelShader 0x%08x (version 0x%08x, %u bytes)", handle, (unsigned)hr, u32(code), codebytes);
            return;
        }
        tr("pixel shader 0x%x created: version 0x%08x, %u bytes", handle, u32(code), codebytes);
        c.pshaders[handle] = ps;
    }
    void set_pshader(uint32_t h) {
        IDirect3DPixelShader9 *ps = nullptr;
        if (h) {
            auto it = c.pshaders.find(h);
            if (it != c.pshaders.end()) ps = it->second;
            else if (d.warn_once(0xc1006)) x.log("ddi: dp2: pixel shader handle 0x%x unknown (fixed function instead)", h);
        }
        tr("pixel shader 0x%x%s", h, h && !ps ? " (unknown)" : "");
        x.dev->SetPixelShader(ps);
    }
    void delete_vshader(uint32_t h) {
        auto it = c.vshaders.find(h);
        if (it == c.vshaders.end()) return;
        if (h == cur_vs) { x.dev->SetVertexShader(nullptr); cur_vs = ~0u; }
        it->second.release();
        c.vshaders.erase(it);
    }
    void delete_pshader(uint32_t h) {
        auto it = c.pshaders.find(h);
        if (it == c.pshaders.end()) return;
        x.dev->SetPixelShader(nullptr);         /* it may be the current one; the runtime sets another before drawing */
        if (it->second) it->second->Release();
        c.pshaders.erase(it);
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
    /* v9: a range of a VRAM buffer (D3DPT_VS_BUFFER) named by a DRAW8, or
     * null (the draw is skipped, once logged) */
    const uint8_t *vram_range(uint32_t handle, uint32_t off, size_t bytes, const char *what) {
        VramSurf *s = surf(x, handle);
        if (!s || !(s->d.caps & D3DPT_VS_BUFFER)) {
            if (d.warn_once(0xa0100)) x.log("ddi: dp2: draw8 %s buffer %u unknown%s (draw skipped)", what, handle, s ? " (not a buffer)" : "");
            return nullptr;
        }
        if (off > s->d.width || bytes > s->d.width - off) {
            if (d.warn_once(0xa0101)) x.log("ddi: dp2: draw8 %s range %u+%zu beyond buffer %u (%u bytes; draw skipped)", what, off, bytes, handle, s->d.width);
            return nullptr;
        }
        return x.vram + s->d.offset + off;
    }
    /* the driver's self-contained DX8 draw: vertices and 16-bit indices
     * inline, or (v9) in VRAM buffers it names by handle and offset */
    bool draw8(const uint8_t *q, size_t left, size_t &need) {
        d3dpt_dp2_draw8 h;
        if (left < sizeof h) return fail("truncated DRAW8");
        memcpy(&h, q, sizeof h);
        if (h.flags & ~(D3DPT_DRAW8_VRAM_VB | D3DPT_DRAW8_VRAM_IB)) return fail("bad DRAW8 flags");
        bool ext_vb = (h.flags & D3DPT_DRAW8_VRAM_VB) != 0, ext_ib = (h.flags & D3DPT_DRAW8_VRAM_IB) != 0;
        size_t vb = ext_vb ? 8 : ((size_t)h.nverts * h.stride + 3) & ~(size_t)3;
        size_t ib = !h.nindices ? 0 : ext_ib ? 8 : ((size_t)h.nindices * 2 + 3) & ~(size_t)3;
        need = sizeof h + vb + ib;
        if (need > left) return fail("truncated DRAW8 data");
        bool shader = (h.fvf & 1) != 0;
        uint32_t st = shader ? 0 : stride_of_fvf(h.fvf);
        if ((!shader && (!st || st > h.stride)) || h.stride > 1024 || h.nverts > 0x10000 || h.nindices > 0x100000 || h.prim_type < 1 || h.prim_type > 6)
            return fail("bad DRAW8");
        const uint8_t *vd = q + sizeof h, *id = vd + vb;
        D3DPRIMITIVETYPE t = (D3DPRIMITIVETYPE)h.prim_type;
        uint32_t nv = prim_verts(t, h.prim_count);
        tr("draw8 type %u prims %u %s 0x%x stride %u vertices %u%s indices %u%s (min %u)", h.prim_type, h.prim_count, shader ? "shader" : "fvf", h.fvf, h.stride,
           h.nverts, ext_vb ? " (vram)" : "", h.nindices, ext_ib && h.nindices ? " (vram)" : "", h.min_index);
        if (ext_vb) {
            vd = vram_range(u32(q + sizeof h), u32(q + sizeof h + 4), (size_t)h.nverts * h.stride, "vertex");
            if (!vd) return true;
        }
        if (h.nindices && ext_ib) {
            id = vram_range(u32(q + sizeof h + vb), u32(q + sizeof h + vb + 4), (size_t)h.nindices * 2, "index");
            if (!id) return true;
        }
        if (shader) {
            /* the vertices are read through the shader's declaration: it
             * must be known, read stream 0 only (the driver copies one
             * stream) and fit the stride (the copied vertex) */
            auto it = c.vshaders.find(h.fvf);
            if (it == c.vshaders.end()) { if (d.warn_once(0xc0000)) x.log("ddi: dp2: vertex shader handle 0x%x unknown (draws with it are skipped)", h.fvf); return true; }
            if (it->second.streams & ~1u) { if (d.warn_once(0xc0001)) x.log("ddi: dp2: vertex shader 0x%x reads streams 0x%x: one stream only, draw skipped", h.fvf, it->second.streams); return true; }
            if (it->second.vertex_bytes > h.stride) { if (d.warn_once(0xc0002)) x.log("ddi: dp2: vertex shader 0x%x reads %u bytes of a %u-byte vertex, draw skipped", h.fvf, it->second.vertex_bytes, h.stride); return true; }
        } else if (d.trace) trv(vd, h.nverts, h.stride, h.fvf);
        apply_vs(h.fvf);
        if (!h.prim_count) return true;
        pre_draw();
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
        pre_draw();
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
        pre_draw();
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

    /* a stage's texture: the surface's host object, re-read from VRAM when
     * dirty (the guest wrote it, its palette changed, its key changed) */
    void bind_texture(uint32_t stage, uint32_t handle) {
        IDirect3DBaseTexture9 *t = nullptr;
        VramSurf *s = handle ? surf(x, handle) : nullptr;
        if (s && ensure_object(x, *s) && s->tex) {
            if (s->dirty) {
                if (d.pal_lines < 48 && needs_expand(*s)) { d.pal_lines++; x.log("ddi: dp2: expanding texture %u (fmt %u, palette %u%s, key %s 0x%x..0x%x) for stage %u", handle, s->d.format, s->palette, d.palettes.count(s->palette) ? "" : " unknown", s->ckey ? "on" : "off", s->ckey_lo, s->ckey_hi, stage); }
                if (s->rt) upload_target(x, d, *s); else upload_texture(x, d, *s);
            }
            t = s->tex;
        }
        x.dev->SetTexture(stage, t);
        if (stage < 8) d.stage_tex[stage] = t ? handle : 0;
        if (stage == 0) apply_ckey();
    }
    /* before a draw: a bound texture whose VRAM / palette / key changed
     * since it was bound is uploaded again (the runtime re-sends TEXTUREMAP
     * only on a SetTexture) */
    void pre_draw() {
        for (uint32_t st = 0; st < 8; st++) {
            VramSurf *s = d.stage_tex[st] ? surf(x, d.stage_tex[st]) : nullptr;
            if (s && (s->dirty || !s->tex)) bind_texture(st, d.stage_tex[st]);
        }
    }

    /* colour keying (v8): a keyed texture's key texels carry alpha 0, so
     * while COLORKEYENABLE is on and such a texture is at stage 0 the alpha
     * test is forced on (GREATEREQUAL 1) unless the app runs its own, and
     * stage 0's alpha op is made to pass the texture alpha through when the
     * app's does not (the DX7 runtime's TEXTUREMAPBLEND emulation selects
     * the diffuse alpha for a texture format without alpha — every keyed
     * R5G6B5 / P8 texture); the app's states come back when the key no
     * longer applies */
    void apply_ckey() {
        VramSurf *s = surf(x, d.stage_tex[0]);
        bool want = d.ckey_rs && s && s->ckey && !(d.rs_set[15] && d.rs_val[15]);
        uint32_t aop = d.tss_set[0][4] ? d.tss_val[0][4] : D3DTOP_SELECTARG1;
        uint32_t a1 = d.tss_set[0][5] ? d.tss_val[0][5] : D3DTA_TEXTURE, a2 = d.tss_set[0][6] ? d.tss_val[0][6] : D3DTA_CURRENT;
        bool uses_tex = aop == D3DTOP_SELECTARG1 ? (a1 & 0xf) == D3DTA_TEXTURE
                      : aop == D3DTOP_SELECTARG2 ? (a2 & 0xf) == D3DTA_TEXTURE
                      : aop != D3DTOP_DISABLE && ((a1 & 0xf) == D3DTA_TEXTURE || (a2 & 0xf) == D3DTA_TEXTURE);
        bool ovr = want && !uses_tex;
        if (want != d.ckey_forced) {
            d.ckey_forced = want;
            tr("colour key alpha test %s", want ? "forced on" : "restored");
            if (want) {
                x.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                x.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                x.dev->SetRenderState(D3DRS_ALPHAREF, 1);
            } else {
                x.dev->SetRenderState(D3DRS_ALPHATESTENABLE, d.rs_set[15] ? d.rs_val[15] : 0);
                x.dev->SetRenderState(D3DRS_ALPHAFUNC, d.rs_set[25] ? d.rs_val[25] : D3DCMP_ALWAYS);
                x.dev->SetRenderState(D3DRS_ALPHAREF, d.rs_set[24] ? d.rs_val[24] : 0);
            }
        }
        if (ovr != d.ckey_alpha_ovr) {
            d.ckey_alpha_ovr = ovr;
            tr("colour key stage 0 alpha op %s", ovr ? "= texture alpha" : "restored");
            x.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, ovr ? D3DTOP_SELECTARG1 : aop);
            x.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, ovr ? D3DTA_TEXTURE : a1);
        }
    }

    /* The DirectX 3 / 5 texture states, which only a DX3 execute buffer
     * still delivers as render states (the DX6+ runtimes turn them into
     * stage states before the driver sees them; doc 15 "Execute buffers"):
     * TEXTUREHANDLE (1) binds stage 0, TEXTUREMAPBLEND (21) picks stage 0's
     * colour / alpha ops the way the old fixed function did — no texture:
     * the diffuse colour; MODULATE: texture x diffuse, the alpha from the
     * texture when its format has one (the colour-key expansion counts:
     * apply_ckey overrides it for a keyed texture anyway), else from the
     * diffuse. */
    static bool legacy_texture_state(uint32_t s) { return s == 1 || s == 3 || s == 5 || s == 6 || s == 17 || s == 18 || s == 21; }
    void apply_mapblend() {
        uint32_t blend = d.rs_set[21] ? d.rs_val[21] : 2 /* MODULATE */;
        VramSurf *t = d.stage_tex[0] ? surf(x, d.stage_tex[0]) : nullptr;
        bool tex_alpha = t && (fmt_has_alpha(t->d.format) || t->ckey || needs_expand(*t));
        uint32_t cop = D3DTOP_SELECTARG2, aop = D3DTOP_SELECTARG2;              /* no texture: diffuse only */
        if (t) {
            switch (blend) {
            case 1: case 5: case 7: cop = D3DTOP_SELECTARG1; aop = D3DTOP_SELECTARG1; break;      /* DECAL, DECALMASK, COPY */
            case 3: cop = D3DTOP_BLENDTEXTUREALPHA; aop = D3DTOP_SELECTARG2; break;              /* DECALALPHA */
            case 4: cop = D3DTOP_MODULATE; aop = D3DTOP_MODULATE; break;                           /* MODULATEALPHA */
            case 8: cop = D3DTOP_ADD; aop = D3DTOP_SELECTARG2; break;                              /* ADD */
            default: cop = D3DTOP_MODULATE; aop = tex_alpha ? D3DTOP_SELECTARG1 : D3DTOP_SELECTARG2; break;   /* MODULATE, MODULATEMASK */
            }
        }
        tr("map blend %u with%s texture: colour op %u alpha op %u", blend, t ? "" : "out", cop, aop);
        stage_state(0, 2, D3DTA_TEXTURE); stage_state(0, 3, D3DTA_DIFFUSE);
        stage_state(0, 5, D3DTA_TEXTURE); stage_state(0, 6, D3DTA_DIFFUSE);
        stage_state(0, 1, cop); stage_state(0, 4, aop);
    }
    void legacy_render_state(uint32_t s, uint32_t v) {
        switch (s) {
        case 1:                                                             /* TEXTUREHANDLE: the surface handle */
            stage_state(0, 0, v);
            apply_mapblend();
            break;
        case 3: x.dev->SetSamplerState(0, D3DSAMP_ADDRESSU, v); x.dev->SetSamplerState(0, D3DSAMP_ADDRESSV, v); break;   /* TEXTUREADDRESS */
        case 5: case 6: {                                                   /* WRAPU / WRAPV: WRAP0 bits */
            uint32_t w0 = ((d.rs_set[5] && d.rs_val[5]) ? 1u : 0u) | ((d.rs_set[6] && d.rs_val[6]) ? 2u : 0u);
            x.dev->SetRenderState(D3DRS_WRAP0, w0);
            break;
        }
        case 17: x.dev->SetSamplerState(0, D3DSAMP_MAGFILTER, v == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR); break;          /* TEXTUREMAG */
        case 18: {                                                          /* TEXTUREMIN: NEAREST, LINEAR, MIPNEAREST, MIPLINEAR, LINEARMIPNEAREST, LINEARMIPLINEAR */
            static const D3DTEXTUREFILTERTYPE minf[7] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
            static const D3DTEXTUREFILTERTYPE mipf[7] = { D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR };
            uint32_t i = v < 7 ? v : 2;
            x.dev->SetSamplerState(0, D3DSAMP_MINFILTER, minf[i]);
            x.dev->SetSamplerState(0, D3DSAMP_MIPFILTER, mipf[i]);
            break;
        }
        case 21: d.legacy_blend = true; apply_mapblend(); break;            /* TEXTUREMAPBLEND */
        }
    }

    void render_state(uint32_t s, uint32_t v) {
        tr("rs %u = 0x%x%s", s, v, rs_passthrough(s) || s == 41 || s == 47 || legacy_texture_state(s) ? "" : " (dropped)");
        if (s < 256) { d.rs_val[s] = v; d.rs_set[s] = 1; }
        if (s == 28 && d.nofog) v = 0;
        if (s == 41) { d.ckey_rs = v; apply_ckey(); return; }              /* COLORKEYENABLE */
        if (s == 47) {
            /* ZBIAS (0..16, DX6-DX8) -> d3d9's DEPTHBIAS: the scale DXVK's
             * own d3d8 layer uses (-1/65535 per step, D16 precision) */
            float bias = (float)(v & 0xffff) * (-1.0f / 65535.0f);
            uint32_t bits; memcpy(&bits, &bias, 4);
            x.dev->SetRenderState(D3DRS_DEPTHBIAS, bits);
            return;
        }
        if (legacy_texture_state(s)) { legacy_render_state(s, v); return; }
        if (rs_passthrough(s)) {
            x.dev->SetRenderState((D3DRENDERSTATETYPE)s, v);
            /* the app's alpha test states: re-forced for the key, or followed */
            if ((s == 15 || s == 24 || s == 25) && d.ckey_forced) { d.ckey_forced = false; apply_ckey(); }
            return;
        }
        if (s < 64 && d.warn_once(0x40000 | s)) x.log("ddi: dp2: render state %u dropped (no d3d9 equivalent)", s);
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
            }
            (void)t;
            bind_texture(stage, v);
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
            /* the app's stage-0 alpha op while the key overrides it: re-applied or followed */
            if (stage == 0 && st >= 4 && st <= 6 && d.ckey_alpha_ovr) { d.ckey_alpha_ovr = false; apply_ckey(); }
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
        if (rt && (flags & D3DCLEAR_TARGET) && nrects == 0) { rt->dirty = false; rt->checked = true; }
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
                if (count) { if (d.trace) trv(q + pad + 4, count + 2); pre_draw(); x.dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, count, q + pad + 4, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_LINELIST_IMM: {
                size_t pad = align_next(0);
                need = pad + (size_t)count * 2 * stride;
                if (need > left) return fail("truncated LINELIST_IMM");
                if (count) { if (d.trace) trv(q + pad, count * 2); pre_draw(); x.dev->DrawPrimitiveUP(D3DPT_LINELIST, count, q + pad, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_TEXTURESTAGESTATE:
                need = count * 8u;
                if (need > left) return fail("truncated TEXTURESTAGESTATE");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t stg = u16(q + 8 * i), st = u16(q + 8 * i + 2), v = u32(q + 8 * i + 4);
                    if (stg == 0 && st >= 1 && st <= 6) d.legacy_blend = false;     /* the app's own ops from now on */
                    stage_state(stg, st, v);
                    /* a DirectX 6 title picks its blend with TEXTUREMAPBLEND once (no texture bound
                     * yet: the diffuse alone) and binds textures as a stage state per draw; the
                     * DX6 runtime passes both through, so the blend is re-evaluated for the
                     * texture now bound (GTA 2's menu text drew as white boxes, 2026-09-05) */
                    if (stg == 0 && st == 0 && d.legacy_blend) apply_mapblend();
                }
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
                /* palette handle, flags (DDRAWIPAL_ALPHA 0x2000: the entries' peFlags are alpha), surface handle */
                need = count * 12u;
                if (need > left) return fail("truncated SETPALETTE");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t ph = u32(q + 12 * i), fl = u32(q + 12 * i + 4), sh = u32(q + 12 * i + 8);
                    VramSurf *s = surf(x, sh);
                    tr("palette %u (flags 0x%x) on surface %u%s", ph, fl, sh, s ? "" : " (unknown)");
                    if (d.pal_lines < 48) { d.pal_lines++; x.log("ddi: dp2: SETPALETTE palette %u flags 0x%x on surface %u%s", ph, fl, sh, s ? "" : " (unknown)"); }
                    if (!s) { if (d.warn_once(0xd0000)) x.log("ddi: dp2: SETPALETTE on unknown surface %u", sh); continue; }
                    if (s->palette != ph || s->pal_alpha != ((fl & 0x2000) != 0)) { s->palette = ph; s->pal_alpha = (fl & 0x2000) != 0; s->dirty = true; }
                }
                break;
            case DP2_UPDATEPALETTE: {
                /* palette handle, start index, entry count, then PALETTEENTRYs (r, g, b, flags) */
                if (left < 8) return fail("truncated UPDATEPALETTE");
                uint32_t ph = u32(q), start = u16(q + 4), n = u16(q + 6);
                need = 8 + (size_t)n * 4;
                if (need > left) return fail("truncated UPDATEPALETTE");
                if (start + n > 256) return fail("UPDATEPALETTE beyond 256 entries");
                tr("palette %u entries %u..%u", ph, start, start + n);
                if (d.pal_lines < 48) { d.pal_lines++; x.log("ddi: dp2: UPDATEPALETTE palette %u entries %u..%u (first %02x%02x%02x/%02x)", ph, start, start + n, q[8], q[9], q[10], q[11]); }
                Palette &pal = d.palettes[ph];
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t *e = q + 8 + 4 * i;
                    pal.argb[start + i] = ((uint32_t)e[3] << 24) | ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
                }
                for (auto &kv : d.surfs) if (kv.second.palette == ph && kv.second.d.format == D3DFMT_P8) kv.second.dirty = true;
                break;
            }
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
            case DP2_SETVERTEXSHADER:                            /* an FVF, or a shader handle (bit 0) */
                need = count * 4u;
                if (need > left) return fail("truncated SETVERTEXSHADER");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    tr("vertex shader 0x%x", h);
                    apply_vs(h);
                }
                break;
            case DP2_CREATEVERTEXSHADER: case DP2_CREATEPIXELSHADER: case DP2_SETVERTEXSHADERCONST: case DP2_SETPIXELSHADERCONST: {
                /* variable: handle [, decl size], code size / register, count */
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated shader token");
                    const uint8_t *e = q + need;
                    if (op == DP2_CREATEVERTEXSHADER) {
                        if (left < need + 12) return fail("truncated CREATEVERTEXSHADER");
                        size_t ds = u32(e + 4), cs = u32(e + 8);
                        need += 12 + ds + cs;
                        if (need > left) return fail("truncated CREATEVERTEXSHADER data");
                        create_vshader(u32(e), e + 12, (uint32_t)ds, e + 12 + ds, (uint32_t)cs);
                    } else if (op == DP2_CREATEPIXELSHADER) {
                        size_t cs = u32(e + 4);
                        need += 8 + cs;
                        if (need > left) return fail("truncated CREATEPIXELSHADER data");
                        create_pshader(u32(e), e + 8, (uint32_t)cs);
                    } else {
                        uint32_t reg = u32(e), cnt = u32(e + 4);
                        need += 8 + (size_t)cnt * 16;
                        if (need > left) return fail("truncated shader constants");
                        /* DXVK indexes its constant arrays with these: keep them inside d3d9's */
                        bool ok = cnt && (uint64_t)reg + cnt <= (op == DP2_SETVERTEXSHADERCONST ? 256u : 224u);
                        tr("%s constants c%u.. x%u%s", op == DP2_SETVERTEXSHADERCONST ? "vertex shader" : "pixel shader", reg, cnt, ok ? "" : " (dropped)");
                        if (!ok) { if (cnt && d.warn_once(0xc1007 | (op << 16))) x.log("ddi: dp2: shader constants c%u x%u out of range, dropped", reg, cnt); continue; }
                        std::vector<float> f(cnt * 4);
                        memcpy(f.data(), e + 8, (size_t)cnt * 16);
                        if (op == DP2_SETVERTEXSHADERCONST) x.dev->SetVertexShaderConstantF(reg, f.data(), cnt);
                        else x.dev->SetPixelShaderConstantF(reg, f.data(), cnt);
                    }
                }
                break;
            }
            case DP2_DELETEVERTEXSHADER: case DP2_DELETEPIXELSHADER: case DP2_SETPIXELSHADER:
                need = count * 4u;
                if (need > left) return fail("truncated shader handle token");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    if (op == DP2_SETPIXELSHADER) set_pshader(h);
                    else if (op == DP2_DELETEVERTEXSHADER) { tr("delete vertex shader 0x%x", h); delete_vshader(h); }
                    else { tr("delete pixel shader 0x%x", h); delete_pshader(h); }
                }
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
            case DP2_BUFFERBLT:                                  /* done in the driver (VRAM buffers, v9); 24 bytes: dst, src, dst offset, D3DRANGE, one more */
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
    for (auto &kv : x.ddi->ctxs) kv.second.release_shaders();
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
        if (a->caps & D3DPT_VS_BUFFER) {
            /* v9: a vertex / index buffer in VRAM: width = pitch = bytes,
             * height 1, no format, no levels, nothing else; no host object
             * (a DRAW8 reads its range from VRAM) */
            if (!a->handle || !a->width || a->height != 1 || a->pitch != a->width || a->format || a->levels > 1 ||
                (a->caps & ~D3DPT_VS_BUFFER) || (uint64_t)a->offset + a->width > x.vram_size ||
                c->size < sizeof(d3dpt_cmd) + sizeof *a) { b.err = D3DPT_ERR_BAD_ARG; return true; }
            Ddi &d = ddi(x);
            VramSurf &s = d.surfs[a->handle];
            if (s.tex || s.rt) s.release();                 /* the handle was a texture / target before */
            s.shadow.clear();
            s.levels.clear();
            s.d = *a;
            s.d.levels = 1;
            s.dirty = true;
            s.rendered = false;
            break;
        }
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
        /* moved (a runtime that swaps two flip buffers' memory, doc 15): what the shadow
         * remembers of the old memory says nothing about the new, or the next readback
         * would keep every differing pixel as the guest's */
        if (s.d.offset != nd.offset || s.d.pitch != nd.pitch) { s.dirty = true; s.shadow.clear(); }
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
        for (uint32_t st = 0; st < 8; st++) if (x.ddi->stage_tex[st] == a->handle) x.ddi->stage_tex[st] = 0;
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
    case D3DPT_OP_VRAM_DIRTY_RANGE: {
        /* v9: a range of a VRAM buffer the guest wrote (Unlock, BUFFERBLT).
         * Nothing is cached of a buffer yet — a DRAW8 reads it from VRAM —
         * so this only keeps the flag honest (and counts, for the log) */
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->a);
        if (s) { s->dirty = true; ddi(x).buffer_writes++; ddi(x).buffer_bytes += a->c; }
        break;
    }
    case D3DPT_OP_VRAM_COLORKEY: {
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->a);
        if (ddi(x).pal_lines < 48) { ddi(x).pal_lines++; x.log("ddi: colour key on surface %u: 0x%x..0x%x flags %u%s", a->a, a->b, a->c, a->d, s ? "" : " (unknown)"); }
        if (!s) { if (ddi(x).warn_once(0xd0001)) x.log("ddi: colour key on unknown surface %u", a->a); break; }
        bool on = (a->d & 1) != 0;
        if (s->ckey != on || s->ckey_lo != a->b || s->ckey_hi != a->c) {
            s->ckey = on; s->ckey_lo = a->b; s->ckey_hi = a->c;
            refresh_object(*s);
        }
        break;
    }
    case D3DPT_OP_CTX_CREATE: {
        auto *a = body<d3dpt_ctx_create>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        Ddi &d = ddi(x);
        if (!a->handle) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto stale = d.ctxs.find(a->handle);
        if (stale != d.ctxs.end()) {
            /* the guest never destroyed it (a display driver that lost its context table
             * with the PDEV, before 2026-09-05): the new one takes its place */
            x.log("ddi: context %u still open, replaced", a->handle);
            if (x.dev && (!stale->second.vshaders.empty() || !stale->second.pshaders.empty())) { x.dev->SetVertexShader(nullptr); x.dev->SetPixelShader(nullptr); }
            stale->second.release_shaders();
            d.ctxs.erase(stale);
        }
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
        if (x.ddi) {
            auto it = x.ddi->ctxs.find(a->handle);
            if (it != x.ddi->ctxs.end()) {
                if (x.dev && (!it->second.vshaders.empty() || !it->second.pshaders.empty())) { x.dev->SetVertexShader(nullptr); x.dev->SetPixelShader(nullptr); }
                it->second.release_shaders();
                x.ddi->ctxs.erase(it);
            }
        }
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
        if (x.ddi->trace_flag && !x.ddi->trace && !x.ddi->trace_armed && file_exists(x.ddi->trace_flag)) {
            x.ddi->trace_armed = true;                 /* the trace starts with the next frame */
            x.log("ddi: trace: armed at dp2 call %u", x.ddi->dp2_calls);
        }
        if (x.ddi->trace) x.log("ddi: trace: dp2 call %u: ctx %u flags 0x%x fvf 0x%x stride %u, %u vertices, %u command bytes", x.ddi->dp2_calls, a->ctx, a->flags, a->fvf, stride, stride ? a->vertex_bytes / stride : 0, a->command_bytes);
        if (stride) { x.dev->SetVertexShader(nullptr); x.dev->SetFVF(a->fvf); p.cur_vs = a->fvf; }   /* a DX7 record: its vertices, the fixed function */
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
