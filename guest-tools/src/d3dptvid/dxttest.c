/*
 * dxttest.c — which texture formats does XP's d3d8.dll actually create on
 * our driver, and in which pool?  Written for the DXT question of doc 15
 * (M7c, DX8 DDI): CreateTexture(DXT1) succeeds at the API and no DXT
 * surface ever reaches the driver.
 *
 *   DXTTEST [-fullscreen]
 *
 * A windowed 320x240 device (hardware vertex processing when offered), then
 * for every format in the table and every pool (DEFAULT, MANAGED,
 * SYSTEMMEM): CheckDeviceFormat, CreateTexture 64x64 with one level,
 * LockRect / UnlockRect, GetLevelDesc (pitch, size), then SetTexture and one
 * textured quad drawn and presented, so the driver's surface / TEXBLT /
 * draw lines in the QEMU log show what arrived for each case. Also
 * CreateImageSurface for the same formats. Every HRESULT is logged with the
 * case's name; the log is dxttest.log in the current directory.
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

struct fmt { D3DFORMAT f; const char *name; int dxt; };
static const struct fmt fmts[] = {
    { D3DFMT_X8R8G8B8, "X8R8G8B8", 0 },
    { D3DFMT_R5G6B5,   "R5G6B5",   0 },
    { D3DFMT_A4R4G4B4, "A4R4G4B4", 0 },
    { D3DFMT_DXT1,     "DXT1",     1 },
    { D3DFMT_DXT3,     "DXT3",     1 },
    { D3DFMT_DXT5,     "DXT5",     1 },
};
static const struct { D3DPOOL p; const char *name; } pools[] = {
    { D3DPOOL_DEFAULT,   "DEFAULT" },
    { D3DPOOL_MANAGED,   "MANAGED" },
    { D3DPOOL_SYSTEMMEM, "SYSTEMMEM" },
};

struct vtx { float x, y, z, rhw; DWORD color; float u, v; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void fill_level(const struct fmt *f, void *bits, int pitch)
{
    int y, x;
    if (f->dxt) {
        /* DXT1: 8-byte blocks, colour0 = red, colour1 = blue, all indices 0;
         * DXT3/5: 16-byte blocks, alpha block then the same colour block */
        int bs = f->f == D3DFMT_DXT1 ? 8 : 16, rows = 16;
        for (y = 0; y < rows; y++) {
            BYTE *row = (BYTE *)bits + y * pitch;
            for (x = 0; x < 16; x++) {
                BYTE *b = row + x * bs;
                memset(b, 0xff, bs);            /* alpha block opaque */
                b = row + x * bs + (bs - 8);
                b[0] = 0x00; b[1] = 0xf8;       /* colour0 0xf800 red */
                b[2] = 0x1f; b[3] = 0x00;       /* colour1 0x001f blue */
                b[4] = b[5] = b[6] = b[7] = (x + y) & 1 ? 0x55 : 0x00;
            }
        }
    } else {
        int bpp = f->f == D3DFMT_X8R8G8B8 ? 4 : 2;
        for (y = 0; y < 64; y++) {
            BYTE *row = (BYTE *)bits + y * pitch;
            for (x = 0; x < 64; x++) {
                DWORD c = ((x ^ y) & 8) ? 0xffff0000 : 0xff0000ff;
                if (bpp == 4) ((DWORD *)row)[x] = c;
                else if (f->f == D3DFMT_R5G6B5) ((WORD *)row)[x] = (c & 0xff0000) ? 0xf800 : 0x001f;
                else ((WORD *)row)[x] = (c & 0xff0000) ? 0xff00 : 0xf00f;
            }
        }
    }
}

/* the rendered quad, read back before Present: four texels' worth of pixels
 * (the fills are red / blue checkers, so pure red or pure blue is the pass) */
static void readback(IDirect3DDevice8 *dev, const struct fmt *f, const char *pn)
{
    IDirect3DSurface8 *bb = NULL, *img = NULL;
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    static const POINT pts[4] = { { 20, 20 }, { 60, 60 }, { 160, 120 }, { 290, 210 } };
    DWORD px[4] = { 0, 0, 0, 0 };
    int i;

    hr = IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        logp("%-8s %-9s GetBackBuffer     0x%08lx\n", f->name, pn, (unsigned long)hr);
        return;
    }
    IDirect3DSurface8_GetDesc(bb, &sd);
    hr = IDirect3DDevice8_CreateImageSurface(dev, sd.Width, sd.Height, sd.Format, &img);
    if (SUCCEEDED(hr) && img) {
        hr = IDirect3DDevice8_CopyRects(dev, bb, NULL, 0, img, NULL);
        if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface8_LockRect(img, &lr, NULL, D3DLOCK_READONLY))) {
            for (i = 0; i < 4; i++) {
                const BYTE *row = (const BYTE *)lr.pBits + pts[i].y * lr.Pitch;
                px[i] = sd.Format == D3DFMT_R5G6B5 ? ((const WORD *)row)[pts[i].x] : ((const DWORD *)row)[pts[i].x] & 0xffffff;
            }
            IDirect3DSurface8_UnlockRect(img);
        }
        logp("%-8s %-9s readback          0x%08lx pixels %06lx %06lx %06lx %06lx\n", f->name, pn, (unsigned long)hr,
             (unsigned long)px[0], (unsigned long)px[1], (unsigned long)px[2], (unsigned long)px[3]);
        IDirect3DSurface8_Release(img);
    } else {
        logp("%-8s %-9s CreateImageSurface(readback) 0x%08lx\n", f->name, pn, (unsigned long)hr);
    }
    IDirect3DSurface8_Release(bb);
}

static void one_case(IDirect3D8 *d3d, IDirect3DDevice8 *dev, D3DFORMAT dispfmt, const struct fmt *f, int pi)
{
    IDirect3DTexture8 *tex = NULL;
    D3DLOCKED_RECT lr;
    D3DSURFACE_DESC sd;
    HRESULT hr;
    const char *pn = pools[pi].name;

    hr = IDirect3D8_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dispfmt, 0, D3DRTYPE_TEXTURE, f->f);
    logp("%-8s %-9s CheckDeviceFormat 0x%08lx\n", f->name, pn, (unsigned long)hr);
    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, f->f, pools[pi].p, &tex);
    logp("%-8s %-9s CreateTexture     0x%08lx\n", f->name, pn, (unsigned long)hr);
    if (FAILED(hr) || !tex) {
        return;
    }
    hr = IDirect3DTexture8_GetLevelDesc(tex, 0, &sd);
    logp("%-8s %-9s GetLevelDesc      0x%08lx fmt %lu size %lu pool %lu\n", f->name, pn, (unsigned long)hr,
         (unsigned long)sd.Format, (unsigned long)sd.Size, (unsigned long)sd.Pool);
    hr = IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0);
    logp("%-8s %-9s LockRect          0x%08lx pitch %ld bits %p\n", f->name, pn, (unsigned long)hr, (long)lr.Pitch, lr.pBits);
    if (SUCCEEDED(hr)) {
        fill_level(f, lr.pBits, lr.Pitch);
        hr = IDirect3DTexture8_UnlockRect(tex, 0);
        logp("%-8s %-9s UnlockRect        0x%08lx\n", f->name, pn, (unsigned long)hr);
    }
    if (pools[pi].p != D3DPOOL_SYSTEMMEM) {
        struct vtx q[4] = {
            {  10.0f,  10.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 0.0f },
            { 310.0f,  10.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 0.0f },
            {  10.0f, 230.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 1.0f },
            { 310.0f, 230.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 1.0f },
        };
        hr = IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
        logp("%-8s %-9s SetTexture        0x%08lx\n", f->name, pn, (unsigned long)hr);
        IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff004000, 1.0f, 0);
        IDirect3DDevice8_BeginScene(dev);
        IDirect3DDevice8_SetVertexShader(dev, FVF);
        hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
        IDirect3DDevice8_EndScene(dev);
        logp("%-8s %-9s DrawPrimitiveUP   0x%08lx\n", f->name, pn, (unsigned long)hr);
        readback(dev, f, pn);
        hr = IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
        logp("%-8s %-9s Present           0x%08lx\n", f->name, pn, (unsigned long)hr);
        IDirect3DDevice8_SetTexture(dev, 0, NULL);
        pump();
        Sleep(300);
    }
    IDirect3DTexture8_Release(tex);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    HRESULT hr;
    unsigned i, pi;

    logf = fopen("dxttest.log", "w");
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dxttest";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "dxttest", "dxttest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 320, 240,
                           NULL, NULL, wc.hInstance, NULL);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) {
        logp("Direct3DCreate8 failed\n");
        return 1;
    }
    IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    logp("dxttest: display format %lu, devcaps 0x%08lx, texture caps 0x%08lx\n", (unsigned long)mode.Format,
         (unsigned long)caps.DevCaps, (unsigned long)caps.TextureCaps);
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
                                 (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                                                                                 : D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &dev);
    logp("dxttest: CreateDevice 0x%08lx (%s vertex processing)\n", (unsigned long)hr,
         (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "hardware" : "software");
    if (FAILED(hr)) {
        return 1;
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pump();

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        for (pi = 0; pi < sizeof(pools) / sizeof(pools[0]); pi++) {
            one_case(d3d, dev, mode.Format, &fmts[i], pi);
        }
    }
    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        IDirect3DSurface8 *s = NULL;
        hr = IDirect3DDevice8_CreateImageSurface(dev, 64, 64, fmts[i].f, &s);
        logp("%-8s image     CreateImageSurface 0x%08lx\n", fmts[i].name, (unsigned long)hr);
        if (s) IDirect3DSurface8_Release(s);
    }
    logp("dxttest: done\n");
    IDirect3DDevice8_Release(dev);
    IDirect3D8_Release(d3d);
    if (logf) fclose(logf);
    return 0;
}
