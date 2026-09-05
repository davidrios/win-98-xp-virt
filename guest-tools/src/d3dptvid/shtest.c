/*
 * shtest.c — vertex and pixel shaders 1.x through XP's own d3d8.dll on our
 * driver's DX8 DDI (doc 15, M7c, protocol v7): the runtime's CREATE / SET /
 * DELETE*SHADER and *SHADERCONST tokens reach the driver only with hardware
 * vertex processing, so this is the check that they get through the DP2
 * stream and that the host runs them.
 *
 *   SHTEST
 *
 * A windowed 320x240 device (hardware vertex processing when the caps offer
 * it; software otherwise, which the log says — then the runtime runs the
 * shaders itself and the driver sees XYZRHW draws), then one case after the
 * other, each a draw read back at a few pixels and compared with the colour
 * it must have:
 *   vs 1.1 through a declaration, oD0 = v5 * c0 (c0 red, then green),
 *   the same through a vertex + index buffer (DrawIndexedPrimitive),
 *   a declaration-only shader (the fixed function on a FLOAT3 + colour layout),
 *   D3DVSD_CONST in the declaration (loaded when the shader is set),
 *   ps 1.1 r0 = c0 (yellow), then off again,
 *   ps 1.1 tex t0 * v0 on XYZRHW vertices (a red / blue checker texture),
 *   the FVF path again, the shaders deleted.
 * The shaders are hand-assembled (no D3DX in mingw). Every HRESULT and
 * pixel is in shtest.log; the last line is "shtest: N cases, M failed".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *logf;
static unsigned cases, failed;

static void logp(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

/* --- shader models 1.x by hand: a version token, instruction tokens
 * (the opcode), parameter tokens (bit 31, the register type in bits 28..30,
 * write mask / swizzle in 16..23, the register number) --- */
#define VS11        0xFFFE0101u
#define PS11        0xFFFF0101u
#define SH_END      0x0000FFFFu
#define OP_MOV      1u
#define OP_MUL      5u
#define OP_TEX      66u
#define R_TEMP      0u
#define R_INPUT     1u
#define R_CONST     2u
#define R_TEXTURE   3u
#define R_RASTOUT   4u
#define R_ATTROUT   5u
#define DST(type, n) (0x80000000u | ((type) << 28) | (0xFu << 16) | (n))
#define SRC(type, n) (0x80000000u | ((type) << 28) | (0xE4u << 16) | (n))

/* vs 1.1: oPos = v0 (clip space as given), oD0 = v5 * c0 */
static const DWORD vs_code[] = {
    VS11,
    OP_MOV, DST(R_RASTOUT, 0), SRC(R_INPUT, D3DVSDE_POSITION),
    OP_MUL, DST(R_ATTROUT, 0), SRC(R_INPUT, D3DVSDE_DIFFUSE), SRC(R_CONST, 0),
    SH_END,
};
/* ps 1.1: r0 = c0 */
static const DWORD ps_const_code[] = { PS11, OP_MOV, DST(R_TEMP, 0), SRC(R_CONST, 0), SH_END };
/* ps 1.1: t0 sampled, r0 = t0 * v0 */
static const DWORD ps_tex_code[] = { PS11, OP_TEX, DST(R_TEXTURE, 0), OP_MUL, DST(R_TEMP, 0), SRC(R_TEXTURE, 0), SRC(R_INPUT, 0), SH_END };

/* the declarations: position + colour (20 bytes), a FLOAT3 position +
 * colour for the fixed function (16 bytes; d3d8.dll refuses a
 * declaration-only shader whose registers are not in FVF order —
 * D3DERR_INVALIDCALL before the driver sees it), position + colour with
 * c0 = blue */
static const DWORD decl_pc[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT4), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()
};
static const DWORD decl_ff[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()
};
static const DWORD decl_const[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT4), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
    D3DVSD_CONST(0, 1), 0x00000000, 0x00000000, 0x3f800000, 0x3f800000,     /* (0, 0, 1, 1) */
    D3DVSD_END()
};

struct pcvtx { float x, y, z, w; DWORD color; };
struct ffvtx { float x, y, z; DWORD color; };
struct tlvtx { float x, y, z, rhw; DWORD color; float u, v; };
#define FVF_TL (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

/* a white quad over the top-left quadrant in clip space (pixels 0..160 x 0..120) */
static const struct pcvtx sq[6] = {
    { -1.0f, 1.0f, 0.5f, 1.0f, 0xffffffff }, { 0.0f, 1.0f, 0.5f, 1.0f, 0xffffffff }, { -1.0f, 0.0f, 0.5f, 1.0f, 0xffffffff },
    { -1.0f, 0.0f, 0.5f, 1.0f, 0xffffffff }, { 0.0f, 1.0f, 0.5f, 1.0f, 0xffffffff }, { 0.0f, 0.0f, 0.5f, 1.0f, 0xffffffff },
};
static const struct ffvtx cq[6] = {
    { -1.0f, 1.0f, 0.5f, 0xff00ffff }, { 0.0f, 1.0f, 0.5f, 0xff00ffff }, { -1.0f, 0.0f, 0.5f, 0xff00ffff },
    { -1.0f, 0.0f, 0.5f, 0xff00ffff }, { 0.0f, 1.0f, 0.5f, 0xff00ffff }, { 0.0f, 0.0f, 0.5f, 0xff00ffff },
};
/* the textured quad, 10..310 x 10..230, uv 0..1 */
static const struct tlvtx tq[4] = {
    {  10.0f,  10.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 0.0f },
    { 310.0f,  10.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 0.0f },
    {  10.0f, 230.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 1.0f },
    { 310.0f, 230.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 1.0f },
};
#define CLEAR_COLOR 0xff004000

static D3DFORMAT dispfmt;

/* the back buffer at (x, y) as 8-bit RGB (before Present) */
static DWORD readback(IDirect3DDevice8 *dev, int x, int y)
{
    IDirect3DSurface8 *bb = NULL, *img = NULL;
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    DWORD px = 0xdeadbeef;
    HRESULT hr;

    hr = IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        logp("  GetBackBuffer 0x%08lx\n", (unsigned long)hr);
        return px;
    }
    IDirect3DSurface8_GetDesc(bb, &sd);
    hr = IDirect3DDevice8_CreateImageSurface(dev, sd.Width, sd.Height, sd.Format, &img);
    if (SUCCEEDED(hr) && img) {
        hr = IDirect3DDevice8_CopyRects(dev, bb, NULL, 0, img, NULL);
        if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface8_LockRect(img, &lr, NULL, D3DLOCK_READONLY))) {
            const BYTE *row = (const BYTE *)lr.pBits + y * lr.Pitch;
            if (sd.Format == D3DFMT_R5G6B5) {
                WORD v = ((const WORD *)row)[x];
                px = ((DWORD)(v >> 11) << 19) | ((DWORD)((v >> 5) & 0x3f) << 10) | ((DWORD)(v & 0x1f) << 3);
            } else {
                px = ((const DWORD *)row)[x] & 0xffffff;
            }
            IDirect3DSurface8_UnlockRect(img);
        } else {
            logp("  CopyRects / LockRect 0x%08lx\n", (unsigned long)hr);
        }
        IDirect3DSurface8_Release(img);
    } else {
        logp("  CreateImageSurface 0x%08lx\n", (unsigned long)hr);
    }
    IDirect3DSurface8_Release(bb);
    return px;
}

static int near_(DWORD a, DWORD b, int tol)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

/* one case: the frame was drawn; check two pixels, present */
static void check(IDirect3DDevice8 *dev, const char *name, int x1, int y1, DWORD want1, int x2, int y2, DWORD want2)
{
    DWORD p1 = readback(dev, x1, y1), p2 = readback(dev, x2, y2);
    int ok = near_(p1, want1, 12) && near_(p2, want2, 12);

    cases++;
    if (!ok) failed++;
    logp("%-44s (%3d,%3d) %06lx want %06lx  (%3d,%3d) %06lx want %06lx  %s\n", name, x1, y1, (unsigned long)p1,
         (unsigned long)want1, x2, y2, (unsigned long)p2, (unsigned long)want2, ok ? "PASS" : "FAIL");
    IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
    pump();
    Sleep(200);
}

static void begin(IDirect3DDevice8 *dev)
{
    IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
    IDirect3DDevice8_BeginScene(dev);
}

static void fill_checker(void *bits, int pitch)
{
    int x, y;
    for (y = 0; y < 64; y++) {
        DWORD *row = (DWORD *)((BYTE *)bits + y * pitch);
        for (x = 0; x < 64; x++) row[x] = ((x ^ y) & 8) ? 0xffff0000 : 0xff0000ff;
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DIndexBuffer8 *ib = NULL;
    IDirect3DTexture8 *tex = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    D3DMATRIX ident;
    DWORD h_vs = 0, h_ff = 0, h_const = 0, h_ps = 0, h_pstex = 0;
    HRESULT hr;
    int hwvp;
    static const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, magenta[4] = { 1, 0, 1, 1 }, yellow[4] = { 1, 1, 0, 1 };

    logf = fopen("shtest.log", "w");
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "shtest";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "shtest", "shtest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 320, 240,
                           NULL, NULL, wc.hInstance, NULL);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) {
        logp("Direct3DCreate8 failed\n");
        return 1;
    }
    IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    dispfmt = mode.Format;
    hwvp = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
    logp("shtest: display format %lu, devcaps 0x%08lx, vs %lu.%lu (%lu constants), ps %lu.%lu (max value %g)\n",
         (unsigned long)mode.Format, (unsigned long)caps.DevCaps,
         (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
         (unsigned long)caps.MaxVertexShaderConst,
         (unsigned long)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
         (double)caps.MaxPixelShaderValue);
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 hwvp ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    logp("shtest: CreateDevice 0x%08lx (%s vertex processing)\n", (unsigned long)hr, hwvp ? "hardware" : "software");
    if (FAILED(hr)) {
        return 1;
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    memset(&ident, 0, sizeof ident);
    ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
    IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &ident);
    IDirect3DDevice8_SetTransform(dev, D3DTS_VIEW, &ident);
    IDirect3DDevice8_SetTransform(dev, D3DTS_PROJECTION, &ident);
    pump();

    /* --- vs 1.1: oD0 = v5 * c0 --- */
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_pc, vs_code, &h_vs, 0);
    logp("CreateVertexShader (position + colour, vs 1.1) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_vs);
    hr = IDirect3DDevice8_SetVertexShader(dev, h_vs);
    logp("SetVertexShader 0x%08lx\n", (unsigned long)hr);
    hr = IDirect3DDevice8_SetVertexShaderConstant(dev, 0, red, 1);
    logp("SetVertexShaderConstant(c0 = red) 0x%08lx\n", (unsigned long)hr);
    begin(dev);
    hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, sq, sizeof sq[0]);
    IDirect3DDevice8_EndScene(dev);
    logp("DrawPrimitiveUP 0x%08lx\n", (unsigned long)hr);
    check(dev, "vs 1.1 v5 * c0, c0 red", 80, 60, 0xff0000, 240, 180, CLEAR_COLOR & 0xffffff);
    IDirect3DDevice8_SetVertexShaderConstant(dev, 0, green, 1);
    begin(dev);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, sq, sizeof sq[0]);
    IDirect3DDevice8_EndScene(dev);
    check(dev, "vs 1.1 c0 green", 80, 60, 0x00ff00, 240, 180, CLEAR_COLOR & 0xffffff);

    /* --- the same through a vertex + index buffer --- */
    hr = IDirect3DDevice8_CreateVertexBuffer(dev, 4 * sizeof sq[0], D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb);
    logp("CreateVertexBuffer 0x%08lx\n", (unsigned long)hr);
    hr = IDirect3DDevice8_CreateIndexBuffer(dev, 6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
    logp("CreateIndexBuffer 0x%08lx\n", (unsigned long)hr);
    if (vb && ib) {
        BYTE *p = NULL;
        static const WORD idx[6] = { 0, 1, 2, 2, 1, 3 };
        struct pcvtx q[4];
        q[0] = sq[0]; q[1] = sq[1]; q[2] = sq[2]; q[3] = sq[5];
        if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(vb, 0, 0, &p, 0))) { memcpy(p, q, sizeof q); IDirect3DVertexBuffer8_Unlock(vb); }
        if (SUCCEEDED(IDirect3DIndexBuffer8_Lock(ib, 0, 0, &p, 0))) { memcpy(p, idx, sizeof idx); IDirect3DIndexBuffer8_Unlock(ib); }
        IDirect3DDevice8_SetStreamSource(dev, 0, vb, sizeof sq[0]);
        IDirect3DDevice8_SetIndices(dev, ib, 0);
        IDirect3DDevice8_SetVertexShaderConstant(dev, 0, magenta, 1);
        begin(dev);
        hr = IDirect3DDevice8_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 4, 0, 2);
        IDirect3DDevice8_EndScene(dev);
        logp("DrawIndexedPrimitive 0x%08lx\n", (unsigned long)hr);
        check(dev, "vs 1.1 from a vertex + index buffer, c0 magenta", 80, 60, 0xff00ff, 240, 180, CLEAR_COLOR & 0xffffff);
        IDirect3DDevice8_SetIndices(dev, NULL, 0);
        IDirect3DDevice8_SetStreamSource(dev, 0, NULL, 0);
    }

    /* --- a declaration-only shader: the fixed function on a FLOAT3 position + colour --- */
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_ff, NULL, &h_ff, 0);
    logp("CreateVertexShader (FLOAT3 position + colour, no function) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_ff);
    IDirect3DDevice8_SetVertexShader(dev, h_ff);
    begin(dev);
    hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, cq, sizeof cq[0]);
    IDirect3DDevice8_EndScene(dev);
    logp("DrawPrimitiveUP 0x%08lx\n", (unsigned long)hr);
    check(dev, "declaration-only shader (fixed function, cyan)", 80, 60, 0x00ffff, 240, 180, CLEAR_COLOR & 0xffffff);

    /* --- D3DVSD_CONST: c0 = blue from the declaration --- */
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_const, vs_code, &h_const, 0);
    logp("CreateVertexShader (D3DVSD_CONST c0 blue) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_const);
    IDirect3DDevice8_SetVertexShader(dev, h_const);
    begin(dev);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, sq, sizeof sq[0]);
    IDirect3DDevice8_EndScene(dev);
    check(dev, "declaration constant c0 blue", 80, 60, 0x0000ff, 240, 180, CLEAR_COLOR & 0xffffff);

    /* --- ps 1.1: r0 = c0 (yellow), then off --- */
    hr = IDirect3DDevice8_CreatePixelShader(dev, ps_const_code, &h_ps);
    logp("CreatePixelShader (r0 = c0) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_ps);
    hr = IDirect3DDevice8_SetPixelShader(dev, h_ps);
    logp("SetPixelShader 0x%08lx\n", (unsigned long)hr);
    hr = IDirect3DDevice8_SetPixelShaderConstant(dev, 0, yellow, 1);
    logp("SetPixelShaderConstant(c0 = yellow) 0x%08lx\n", (unsigned long)hr);
    begin(dev);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, sq, sizeof sq[0]);
    IDirect3DDevice8_EndScene(dev);
    check(dev, "ps 1.1 r0 = c0 yellow", 80, 60, 0xffff00, 240, 180, CLEAR_COLOR & 0xffffff);
    IDirect3DDevice8_SetPixelShader(dev, 0);
    begin(dev);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, sq, sizeof sq[0]);
    IDirect3DDevice8_EndScene(dev);
    check(dev, "pixel shader off: the vertex colour (blue)", 80, 60, 0x0000ff, 240, 180, CLEAR_COLOR & 0xffffff);

    /* --- ps 1.1 tex t0 * v0 on XYZRHW vertices --- */
    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex);
    logp("CreateTexture (64x64 X8R8G8B8 checker) 0x%08lx\n", (unsigned long)hr);
    if (tex) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0))) { fill_checker(lr.pBits, lr.Pitch); IDirect3DTexture8_UnlockRect(tex, 0); }
    }
    hr = IDirect3DDevice8_CreatePixelShader(dev, ps_tex_code, &h_pstex);
    logp("CreatePixelShader (tex t0, r0 = t0 * v0) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_pstex);
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetVertexShader(dev, FVF_TL);
    IDirect3DDevice8_SetPixelShader(dev, h_pstex);
    begin(dev);
    hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, tq, sizeof tq[0]);
    IDirect3DDevice8_EndScene(dev);
    logp("DrawPrimitiveUP (FVF, textured) 0x%08lx\n", (unsigned long)hr);
    /* texel (12, 4) is in a red cell, texel (4, 4) in a blue one: pixels 10 + u * 300, 10 + v * 220 */
    check(dev, "ps 1.1 tex t0 * v0 on FVF vertices", 66, 24, 0xff0000, 30, 24, 0x0000ff);
    IDirect3DDevice8_SetPixelShader(dev, 0);
    IDirect3DDevice8_SetTexture(dev, 0, NULL);

    /* --- the FVF path again, the shaders deleted --- */
    hr = IDirect3DDevice8_DeleteVertexShader(dev, h_vs);
    logp("DeleteVertexShader 0x%08lx\n", (unsigned long)hr);
    IDirect3DDevice8_DeleteVertexShader(dev, h_ff);
    IDirect3DDevice8_DeleteVertexShader(dev, h_const);
    hr = IDirect3DDevice8_DeletePixelShader(dev, h_ps);
    logp("DeletePixelShader 0x%08lx\n", (unsigned long)hr);
    IDirect3DDevice8_DeletePixelShader(dev, h_pstex);
    IDirect3DDevice8_SetVertexShader(dev, FVF_TL);
    begin(dev);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, tq, sizeof tq[0]);
    IDirect3DDevice8_EndScene(dev);
    check(dev, "FVF again, no shaders (white)", 66, 24, 0xffffff, 315, 235, CLEAR_COLOR & 0xffffff);

    logp("shtest: %u cases, %u failed\n", cases, failed);
    if (tex) IDirect3DTexture8_Release(tex);
    if (vb) IDirect3DVertexBuffer8_Release(vb);
    if (ib) IDirect3DIndexBuffer8_Release(ib);
    IDirect3DDevice8_Release(dev);
    IDirect3D8_Release(d3d);
    if (logf) fclose(logf);
    return failed ? 1 : 0;
}
