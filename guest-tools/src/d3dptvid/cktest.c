/*
 * cktest.c — palettized textures and colour keying through our driver's
 * DX7 HAL (doc 15 "Palettized textures and colour keying", protocol v8):
 * what a 1997 title asks a Voodoo-class card for and what made Moto Racer
 * fall back to its software rasterizer.
 *
 *   CKTEST [w h bpp]
 *
 * Fullscreen, HAL device on the back buffer (as D3D7TEST), then:
 *   an 8-bit palettized texture (DDPF_PALETTEINDEXED8) with its own
 *   DirectDraw palette: index 1 red on the left half, index 2 green on the
 *   right; a textured quad read back at both halves;
 *   IDirectDrawPalette::SetEntries turns entry 1 blue: the left half follows;
 *   a R5G6B5 texture, magenta / white checker, SetColorKey(DDCKEY_SRCBLT,
 *   magenta): with COLORKEYENABLE the magenta cells show the clear colour,
 *   without it they are magenta again.
 * Every pixel is read back from the back buffer before the flip and
 * compared; cktest.log ends with "cktest: N cases, M failed".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define D3D_OVERLOADS 0
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX 64
#define CLEAR_COLOR 0xff004000

static FILE *logfile;
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
    if (logfile) {
        fputs(buf, logfile);
        fflush(logfile);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

static D3DTLVERTEX V(float x, float y, float z, DWORD c, float u, float v)
{
    D3DTLVERTEX r;
    memset(&r, 0, sizeof r);
    r.sx = x; r.sy = y; r.sz = z; r.rhw = 1.0f; r.color = c; r.specular = 0xff000000; r.tu = u; r.tv = v;
    return r;
}

static int enum_hal;
static DWORD hal_texcaps;
static HRESULT CALLBACK enum_dev(char *desc, char *name, D3DDEVICEDESC7 *dd, void *ctx)
{
    if (IsEqualGUID(&dd->deviceGUID, &IID_IDirect3DHALDevice)) {
        enum_hal = 1;
        hal_texcaps = dd->dpcTriCaps.dwTextureCaps;
    }
    return D3DENUMRET_OK;
}

/* the back buffer at (x, y) as 8-bit RGB */
static DWORD readback(LPDIRECTDRAWSURFACE7 back, int x, int y)
{
    DDSURFACEDESC2 sd;
    DWORD px = 0xdeadbeef;
    HRESULT hr;

    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) { logp("  Lock(back) %08lx\n", hr); return px; }
    if (sd.ddpfPixelFormat.dwRGBBitCount == 16) {
        WORD v = *(WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch + x * 2);
        px = ((DWORD)(v >> 11) << 19) | ((DWORD)((v >> 5) & 0x3f) << 10) | ((DWORD)(v & 0x1f) << 3);
    } else {
        px = *(DWORD *)((BYTE *)sd.lpSurface + y * sd.lPitch + x * 4) & 0xffffff;
    }
    back->lpVtbl->Unlock(back, NULL);
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

/* the whole back buffer as ckN.bmp (24-bit), for the eye when a case fails */
static void dump_bmp(LPDIRECTDRAWSURFACE7 back, const char *path)
{
    DDSURFACEDESC2 sd;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    unsigned w, h, bpp, y, x, row_bytes;
    unsigned char *row;

    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    if (FAILED(back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return;
    w = sd.dwWidth; h = sd.dwHeight; bpp = sd.ddpfPixelFormat.dwRGBBitCount; row_bytes = (w * 3 + 3) & ~3u;
    f = fopen(path, "wb");
    row = malloc(row_bytes);
    if (f && row) {
        memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih); memset(row, 0, row_bytes);
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + row_bytes * h;
        ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
        for (y = h; y-- > 0;) {
            const unsigned char *src = (const unsigned char *)sd.lpSurface + y * sd.lPitch;
            for (x = 0; x < w; x++) {
                if (bpp == 32) {
                    row[x * 3 + 0] = src[x * 4 + 0]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2];
                } else {
                    unsigned v = ((const unsigned short *)src)[x];
                    row[x * 3 + 0] = (v & 0x1f) << 3; row[x * 3 + 1] = ((v >> 5) & 0x3f) << 2; row[x * 3 + 2] = ((v >> 11) & 0x1f) << 3;
                }
            }
            fwrite(row, 1, row_bytes, f);
        }
    }
    if (f) fclose(f);
    free(row);
    back->lpVtbl->Unlock(back, NULL);
}

static void check(LPDIRECTDRAWSURFACE7 back, const char *name, int x1, int y1, DWORD want1, int x2, int y2, DWORD want2)
{
    DWORD p1 = readback(back, x1, y1), p2 = readback(back, x2, y2), p3 = readback(back, 340, 200), p1b;
    char path[32];
    int ok = near_(p1, want1, 12) && near_(p2, want2, 12);
    cases++;
    if (!ok) failed++;
    p1b = readback(back, x1, y1);       /* a second Lock: the same frame, or a readback caught mid-way */
    logp("%-52s (%d,%d) %06lx want %06lx  (%d,%d) %06lx want %06lx  (340,200) %06lx  again %06lx  %s\n", name,
         x1, y1, p1, want1, x2, y2, p2, want2, p3, p1b, ok ? "PASS" : "FAIL");
    sprintf(path, "ck%u.bmp", cases);
    dump_bmp(back, path);
}

/* the quad 100..420 x 80..320, u/v 0..1: texel (tx, ty) is at pixel (100 + tx * 5, 80 + ty * 3.75) */
static void draw_quad(LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 tex, DWORD ckey_enable)
{
    D3DTLVERTEX q[6];
    q[0] = V(100, 80, 0.5f, 0xffffffff, 0, 0); q[1] = V(420, 80, 0.5f, 0xffffffff, 1, 0); q[2] = V(100, 320, 0.5f, 0xffffffff, 0, 1);
    q[3] = V(100, 320, 0.5f, 0xffffffff, 0, 1); q[4] = V(420, 80, 0.5f, 0xffffffff, 1, 0); q[5] = V(420, 320, 0.5f, 0xffffffff, 1, 1);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_COLORKEYENABLE, ckey_enable);
    dev->lpVtbl->SetTexture(dev, 0, tex);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, q, 6, 0);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->EndScene(dev);
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 32;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL, texp8 = NULL, tex16 = NULL;
    LPDIRECTDRAWPALETTE pal = NULL;
    LPDIRECT3D7 d3d = NULL;
    LPDIRECT3DDEVICE7 dev = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    DDCAPS hal, hel;
    PALETTEENTRY pe[256];
    DDCOLORKEY ck;
    D3DVIEWPORT7 vp;
    HRESULT hr;
    WNDCLASSA wc;
    HWND hwnd;
    int i;

    if (argc > 3) { w = atoi(argv[1]); h = atoi(argv[2]); bpp = atoi(argv[3]); }
    logfile = fopen("cktest.log", "w");
    logp("cktest: %dx%d %d bpp\n", w, h, bpp);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "cktest";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "cktest", "cktest", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    ShowCursor(FALSE);
    pump();

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx failed %08lx\n", hr); return 1; }
    memset(&hal, 0, sizeof hal); hal.dwSize = sizeof hal;
    memset(&hel, 0, sizeof hel); hel.dwSize = sizeof hel;
    dd->lpVtbl->GetCaps(dd, &hal, &hel);
    logp("HAL caps %08lx (%s colour key) ckey caps %08lx pal caps %08lx\n", hal.dwCaps,
         (hal.dwCaps & DDCAPS_COLORKEY) ? "with" : "no", hal.dwCKeyCaps, hal.dwPalCaps);
    hr = dd->lpVtbl->QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr)) { logp("QueryInterface(IDirect3D7) %08lx\n", hr); goto out; }
    d3d->lpVtbl->EnumDevices(d3d, enum_dev, NULL);
    logp("HAL device %s, texture caps %08lx (%s TRANSPARENCY, %s ALPHAPALETTE)\n", enum_hal ? "present" : "ABSENT", hal_texcaps,
         (hal_texcaps & D3DPTEXTURECAPS_TRANSPARENCY) ? "with" : "no", (hal_texcaps & D3DPTEXTURECAPS_ALPHAPALETTE) ? "with" : "no");
    if (!enum_hal) goto out;

    hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    logp("SetCooperativeLevel %08lx\n", hr);
    hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp, 0, 0);
    logp("SetDisplayMode %dx%dx%d %08lx\n", w, h, bpp, hr);
    if (FAILED(hr)) goto out;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    logp("CreateSurface(primary + back, 3DDEVICE) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
    if (FAILED(hr)) { logp("GetAttachedSurface(back) %08lx\n", hr); goto out; }
    hr = d3d->lpVtbl->CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev);
    logp("CreateDevice(HAL) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = w; vp.dwHeight = h; vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    dev->lpVtbl->SetViewport(dev, &vp);

    /* --- the palettized texture: index 1 on the left half, 2 on the right --- */
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = TEX; sd.dwHeight = TEX;
    sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    sd.ddpfPixelFormat.dwRGBBitCount = 8;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &texp8, NULL);
    logp("CreateSurface(texture %dx%d P8 vidmem) %08lx\n", TEX, TEX, hr);
    if (SUCCEEDED(hr)) {
        memset(pe, 0, sizeof pe);
        pe[1].peRed = 255; pe[2].peGreen = 255; pe[3].peBlue = 255;
        hr = dd->lpVtbl->CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL);
        logp("CreatePalette(8 bit) %08lx\n", hr);
        hr = texp8->lpVtbl->SetPalette(texp8, pal);
        logp("SetPalette(texture) %08lx\n", hr);
        memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
        hr = texp8->lpVtbl->Lock(texp8, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        logp("Lock(P8 texture) %08lx pitch %ld\n", hr, sd.lPitch);
        if (SUCCEEDED(hr)) {
            int x, y;
            for (y = 0; y < TEX; y++)
                for (x = 0; x < TEX; x++) ((BYTE *)sd.lpSurface)[y * sd.lPitch + x] = x < TEX / 2 ? 1 : 2;
            texp8->lpVtbl->Unlock(texp8, NULL);
        }
        draw_quad(dev, texp8, FALSE);
        check(back, "P8 texture: left index 1 red, right index 2 green", 180, 200, 0xff0000, 340, 200, 0x00ff00);
        prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        pump();
        pe[1].peRed = 0; pe[1].peBlue = 255;
        hr = pal->lpVtbl->SetEntries(pal, 0, 1, 1, &pe[1]);
        logp("SetEntries(entry 1 = blue) %08lx\n", hr);
        draw_quad(dev, texp8, FALSE);
        check(back, "SetEntries: the left half follows the palette (blue)", 180, 200, 0x0000ff, 340, 200, 0x00ff00);
        prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        pump();
    } else {
        cases++; failed++;
        logp("P8 texture: CreateSurface failed, FAIL\n");
    }

    /* --- the colour-keyed texture: a magenta / white checker, magenta keyed --- */
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = TEX; sd.dwHeight = TEX;
    sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 16;
    sd.ddpfPixelFormat.dwRBitMask = 0xf800; sd.ddpfPixelFormat.dwGBitMask = 0x07e0; sd.ddpfPixelFormat.dwBBitMask = 0x001f;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &tex16, NULL);
    logp("CreateSurface(texture %dx%d R5G6B5 vidmem) %08lx\n", TEX, TEX, hr);
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
        hr = tex16->lpVtbl->Lock(tex16, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        logp("Lock(R5G6B5 texture) %08lx\n", hr);
        if (SUCCEEDED(hr)) {
            int x, y;
            for (y = 0; y < TEX; y++)
                for (x = 0; x < TEX; x++)
                    ((WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch))[x] = ((x / 16) + (y / 16)) & 1 ? 0xffff : 0xf81f;
            tex16->lpVtbl->Unlock(tex16, NULL);
        }
        ck.dwColorSpaceLowValue = 0xf81f; ck.dwColorSpaceHighValue = 0xf81f;
        hr = tex16->lpVtbl->SetColorKey(tex16, DDCKEY_SRCBLT, &ck);
        logp("SetColorKey(SRCBLT, magenta) %08lx\n", hr);
        /* texel (8, 8) is a magenta cell, (24, 8) a white one: pixels (140, 110) and (220, 110) */
        draw_quad(dev, tex16, TRUE);
        check(back, "colour key on: keyed cell = clear colour, other white", 140, 110, CLEAR_COLOR & 0xffffff, 220, 110, 0xffffff);
        prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        pump();
        draw_quad(dev, tex16, FALSE);
        check(back, "colour key off: the keyed cell is magenta again", 140, 110, 0xff00ff, 220, 110, 0xffffff);
        prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        pump();
    } else {
        cases++; failed++;
        logp("R5G6B5 texture: CreateSurface failed, FAIL\n");
    }
    for (i = 0; i < 30; i++) { pump(); Sleep(30); }

out:
    logp("cktest: %u cases, %u failed\n", cases, failed);
    if (tex16) tex16->lpVtbl->Release(tex16);
    if (pal) pal->lpVtbl->Release(pal);
    if (texp8) texp8->lpVtbl->Release(texp8);
    if (dev) dev->lpVtbl->Release(dev);
    if (prim) prim->lpVtbl->Release(prim);
    if (d3d) d3d->lpVtbl->Release(d3d);
    if (dd) {
        dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
    ShowCursor(TRUE);
    DestroyWindow(hwnd);
    logp("cktest: done\n");
    if (logfile) fclose(logfile);
    return failed ? 1 : 0;
}
