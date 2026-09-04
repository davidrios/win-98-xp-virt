/*
 * d3d7test.c — Direct3D 7 test for the d3dptdisp driver (doc 15, M7c).
 *
 *   D3D7TEST.EXE [w h bpp] [frames] [-noz]
 *
 * Enumerates the Direct3D devices (is there a HAL?), creates an exclusive
 * flip chain with a Z buffer and an IDirect3DDevice7 on the back buffer,
 * then draws the scene of tools/d3dpt-dp2-test.cpp (the host-side test
 * that feeds the executor the same DP2 tokens without a guest): a cyan
 * triangle behind a wrapped checkerboard-textured quad, a Gouraud fan in
 * front, a half-transparent red strip. Frames per second over the run,
 * the last back buffer to d3d7test.bmp (through Lock: the driver reads
 * the host's frame back into VRAM), everything to d3d7test.log. The BMP
 * is diffed against the host test's by tools/xp-driver-test.sh d3d7.
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
#include <math.h>

static FILE *logfile;
static void logp(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
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
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void dump_bmp(const char *path, const DDSURFACEDESC2 *sd)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "wb");
    unsigned w = sd->dwWidth, h = sd->dwHeight, bpp = sd->ddpfPixelFormat.dwRGBBitCount, y, x, row_bytes = (w * 3 + 3) & ~3u;
    unsigned char *row = malloc(row_bytes);
    if (!f || !row) return;
    memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih)); memset(row, 0, row_bytes);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row_bytes * h;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    for (y = h; y-- > 0;) {
        const unsigned char *src = (const unsigned char *)sd->lpSurface + y * sd->lPitch;
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
    fclose(f);
    free(row);
}

/* --- the scene, shared with tools/d3dpt-dp2-test.cpp --- */
#define TEX 64
#define CLEAR_COLOR 0xff203040u

static D3DTLVERTEX V(float x, float y, float z, DWORD c, float u, float v)
{
    D3DTLVERTEX t;
    t.sx = x; t.sy = y; t.sz = z; t.rhw = 1.0f;
    t.color = c; t.specular = 0xff000000u; t.tu = u; t.tv = v;
    return t;
}

static int enum_hal;
static HRESULT CALLBACK enum_dev(char *desc, char *name, D3DDEVICEDESC7 *dd, void *ctx)
{
    int hal = memcmp(&dd->deviceGUID, &IID_IDirect3DHALDevice, sizeof(GUID)) == 0;
    int tnl = memcmp(&dd->deviceGUID, &IID_IDirect3DTnLHalDevice, sizeof(GUID)) == 0;
    logp("device: %s (%s)%s%s devcaps %08lx tex %lux%lu..%lux%lu stages %u simtex %u\n", name, desc,
         hal ? " HAL" : "", tnl ? " TnLHAL" : "", dd->dwDevCaps, dd->dwMinTextureWidth, dd->dwMinTextureHeight,
         dd->dwMaxTextureWidth, dd->dwMaxTextureHeight, dd->wMaxTextureBlendStages, dd->wMaxSimultaneousTextures);
    if (hal) enum_hal = 1;
    return D3DENUMRET_OK;
}

static DDPIXELFORMAT zfmt;
static HRESULT CALLBACK enum_z(DDPIXELFORMAT *pf, void *ctx)
{
    logp("z format: %lu bits%s\n", pf->dwZBufferBitDepth, (pf->dwFlags & DDPF_STENCILBUFFER) ? " + stencil" : "");
    if (!zfmt.dwSize && pf->dwZBufferBitDepth == 16) zfmt = *pf;
    return D3DENUMRET_OK;
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 32, frames = 300, i, argn = 0, noz = 0;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL, zbuf = NULL, tex = NULL;
    LPDIRECT3D7 d3d = NULL;
    LPDIRECT3DDEVICE7 dev = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    DDCAPS hal, hel;
    D3DVIEWPORT7 vp;
    D3DTLVERTEX vtx[19];
    WORD tri_idx[3] = { 0, 1, 2 };
    HRESULT hr;
    WNDCLASSA wc;
    HWND hwnd;
    DWORD t0, t1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-noz")) noz = 1;
        else if (argn == 0) { w = atoi(argv[i]); argn++; }
        else if (argn == 1) { h = atoi(argv[i]); argn++; }
        else if (argn == 2) { bpp = atoi(argv[i]); argn++; }
        else if (argn == 3) { frames = atoi(argv[i]); argn++; }
    }
    logfile = fopen("d3d7test.log", "w");
    logp("d3d7test: %dx%d %d bpp, %d frames\n", w, h, bpp, frames);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d7test";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "d3d7test", "d3d7test", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    /* GDI's software pointer lives in VRAM (no hardware cursor yet): keep
     * it out of the frame the test dumps and diffs */
    ShowCursor(FALSE);
    pump();

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx failed %08lx\n", hr); return 1; }
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    dd->lpVtbl->GetCaps(dd, &hal, &hel);
    logp("HAL caps %08lx (%s) ddsCaps %08lx z depths %08lx vidmem %lu KiB\n", hal.dwCaps,
         (hal.dwCaps & DDCAPS_3D) ? "3D" : "no 3D", hal.ddsCaps.dwCaps, hal.dwZBufferBitDepths, hal.dwVidMemTotal / 1024);

    hr = dd->lpVtbl->QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    logp("QueryInterface(IDirect3D7) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    d3d->lpVtbl->EnumDevices(d3d, enum_dev, NULL);
    logp("HAL device %s\n", enum_hal ? "present" : "ABSENT");
    if (!enum_hal) goto out;

    hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    logp("SetCooperativeLevel %08lx\n", hr);
    hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp, 0, 0);
    logp("SetDisplayMode %dx%dx%d %08lx\n", w, h, bpp, hr);
    if (FAILED(hr)) goto out;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    logp("CreateSurface(primary + back, 3DDEVICE) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
    logp("GetAttachedSurface(back) %08lx\n", hr);
    if (FAILED(hr)) goto out;

    d3d->lpVtbl->EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, enum_z, NULL);
    if (!zfmt.dwSize) { logp("no 16-bit Z format\n"); goto out; }
    if (noz) goto nozbuf;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = w; sd.dwHeight = h;
    sd.ddpfPixelFormat = zfmt;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &zbuf, NULL);
    logp("CreateSurface(Z %dx%d) %08lx\n", w, h, hr);
    if (FAILED(hr)) goto out;
    hr = back->lpVtbl->AddAttachedSurface(back, zbuf);
    logp("AddAttachedSurface(Z) %08lx\n", hr);

nozbuf:
    hr = d3d->lpVtbl->CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev);
    logp("CreateDevice(HAL) %08lx\n", hr);
    if (FAILED(hr)) goto out;

    /* the checkerboard texture, A8R8G8B8 in video memory */
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = TEX; sd.dwHeight = TEX;
    sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0x00ff0000; sd.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    sd.ddpfPixelFormat.dwBBitMask = 0x000000ff; sd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &tex, NULL);
    logp("CreateSurface(texture %dx%d A8R8G8B8 vidmem) %08lx\n", TEX, TEX, hr);
    if (FAILED(hr)) goto out;
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = tex->lpVtbl->Lock(tex, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    logp("Lock(texture) %08lx caps %08lx\n", hr, sd.ddsCaps.dwCaps);
    if (FAILED(hr)) goto out;
    {
        unsigned y, x;
        for (y = 0; y < TEX; y++)
            for (x = 0; x < TEX; x++) {
                unsigned c = ((x / 8) + (y / 8)) & 1 ? 0xffffffffu : 0xff2040ffu;
                if (y >= 24 && y < 40) c = (c & 0x00ffffffu) | 0x80000000u;
                ((unsigned *)((unsigned char *)sd.lpSurface + y * sd.lPitch))[x] = c;
            }
    }
    tex->lpVtbl->Unlock(tex, NULL);

    /* vertices: quad (0..5), fan (6..11), triangle (12..14), alpha strip (15..18) */
    vtx[0] = V(100, 80, 0.5f, 0xffffffff, 0, 0); vtx[1] = V(420, 80, 0.5f, 0xffffffff, 2, 0); vtx[2] = V(100, 320, 0.5f, 0xffffffff, 0, 2);
    vtx[3] = V(100, 320, 0.5f, 0xffffffff, 0, 2); vtx[4] = V(420, 80, 0.5f, 0xffffffff, 2, 0); vtx[5] = V(420, 320, 0.5f, 0xffffffff, 2, 2);
    {
        DWORD col[] = { 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff0000 };
        for (i = 0; i < 6; i++) {
            float a = i ? (float)(i - 1) * 6.2831853f / 4.0f : 0.0f;
            vtx[6 + i] = i == 0 ? V(480, 240, 0.3f, col[0], 0, 0)
                                : V(480 + 100 * cosf(a), 240 + 100 * sinf(a), 0.3f, col[i], 0, 0);
        }
    }
    vtx[12] = V(300, 40, 0.7f, 0xff00ffff, 0, 0); vtx[13] = V(600, 420, 0.7f, 0xff00ffff, 0, 0); vtx[14] = V(60, 460, 0.7f, 0xff00ffff, 0, 0);
    vtx[15] = V(20, 360, 0.1f, 0x80ff0000, 0, 0); vtx[16] = V(220, 360, 0.1f, 0x80ff0000, 0, 0);
    vtx[17] = V(20, 470, 0.1f, 0x80ff0000, 0, 0); vtx[18] = V(220, 470, 0.1f, 0x80ff0000, 0, 0);

    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = w; vp.dwHeight = h; vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    hr = dev->lpVtbl->SetViewport(dev, &vp);
    logp("SetViewport %08lx\n", hr);

    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        pump();
        hr = dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f, 0);
        if (FAILED(hr)) { logp("Clear failed %08lx at frame %d\n", hr, i); goto out; }
        hr = dev->lpVtbl->BeginScene(dev);
        if (FAILED(hr)) { logp("BeginScene failed %08lx at frame %d\n", hr, i); goto out; }
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZWRITEENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_DITHERENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_SPECULARENABLE, FALSE);
        /* the cyan triangle, behind the quad */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        hr = dev->lpVtbl->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, vtx + 12, 3, tri_idx, 3, 0);
        if (FAILED(hr)) { logp("DrawIndexedPrimitive failed %08lx at frame %d\n", hr, i); goto out; }
        /* the textured quad */
        dev->lpVtbl->SetTexture(dev, 0, tex);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTFP_NONE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP);
        hr = dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, vtx, 6, 0);
        if (FAILED(hr)) { logp("DrawPrimitive(quad) failed %08lx at frame %d\n", hr, i); goto out; }
        /* the coloured fan in front */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        hr = dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLEFAN, D3DFVF_TLVERTEX, vtx + 6, 6, 0);
        if (FAILED(hr)) { logp("DrawPrimitive(fan) failed %08lx at frame %d\n", hr, i); goto out; }
        /* the half-transparent strip */
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        hr = dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, vtx + 15, 4, 0);
        if (FAILED(hr)) { logp("DrawPrimitive(strip) failed %08lx at frame %d\n", hr, i); goto out; }
        dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        hr = dev->lpVtbl->EndScene(dev);
        if (FAILED(hr)) { logp("EndScene failed %08lx at frame %d\n", hr, i); goto out; }
        if (i == frames - 1) {
            /* the last frame from the back buffer before it is flipped away */
            memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
            hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
            if (SUCCEEDED(hr)) {
                unsigned *px = (unsigned *)((unsigned char *)sd.lpSurface + 240 * sd.lPitch) + 480;
                logp("back buffer after EndScene: fan centre %06x, corner %06x\n", *px & 0xffffff,
                     *(unsigned *)((unsigned char *)sd.lpSurface + 20 * sd.lPitch + 620 * 4) & 0xffffff);
                dump_bmp("d3d7test.bmp", &sd);
                back->lpVtbl->Unlock(back, NULL);
                logp("d3d7test.bmp written\n");
            } else {
                logp("Lock(back) failed %08lx\n", hr);
            }
        }
        hr = prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        if (FAILED(hr)) { logp("Flip failed %08lx at frame %d\n", hr, i); goto out; }
        if (i == 60) {
            t1 = GetTickCount();
            logp("first 60 frames: %lu ms\n", t1 - t0);
        }
    }
    t1 = GetTickCount();
    logp("%d frames in %lu ms = %.1f fps\n", frames, t1 - t0, t1 > t0 ? frames * 1000.0 / (t1 - t0) : 0.0);

out:
    if (tex) tex->lpVtbl->Release(tex);
    if (dev) dev->lpVtbl->Release(dev);
    if (zbuf) zbuf->lpVtbl->Release(zbuf);
    if (prim) prim->lpVtbl->Release(prim);
    if (d3d) d3d->lpVtbl->Release(d3d);
    if (dd) {
        dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
    ShowCursor(TRUE);
    DestroyWindow(hwnd);
    logp("d3d7test: done\n");
    if (logfile) fclose(logfile);
    return 0;
}
