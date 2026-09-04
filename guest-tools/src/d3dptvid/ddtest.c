/*
 * ddtest.c — DirectDraw 7 test for the d3dpt-vga driver (doc 15, M7b).
 *
 *   DDTEST.EXE [w h bpp] [frames] [-windowed]
 *
 * Prints the HAL caps DirectDraw reports for the adapter (is there a HAL
 * at all, video memory, flip support), then runs an exclusive full-screen
 * flip chain: each frame Lock/Unlock draws a moving pattern into the back
 * buffer, a Blt colour fill paints a bar, Flip presents. Reports whether
 * the surfaces landed in video memory, the frame rate, and dumps the
 * last back buffer to ddtest.bmp. Everything also goes to ddtest.log.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *logf;
static void logp(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
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
    unsigned w = sd->dwWidth, h = sd->dwHeight, bpp = sd->ddpfPixelFormat.dwRGBBitCount, y, x;
    unsigned char *row = malloc(w * 3);
    if (!f || !row) return;
    memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + w * 3 * h;
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
        fwrite(row, 1, w * 3, f);
    }
    fclose(f);
    free(row);
}

static const char *caps_str(DWORD caps)
{
    static char buf[256];
    buf[0] = 0;
    if (caps & DDSCAPS_VIDEOMEMORY) strcat(buf, "VIDEOMEMORY ");
    if (caps & DDSCAPS_LOCALVIDMEM) strcat(buf, "LOCALVIDMEM ");
    if (caps & DDSCAPS_SYSTEMMEMORY) strcat(buf, "SYSTEMMEMORY ");
    if (caps & DDSCAPS_PRIMARYSURFACE) strcat(buf, "PRIMARY ");
    if (caps & DDSCAPS_BACKBUFFER) strcat(buf, "BACKBUFFER ");
    if (caps & DDSCAPS_FLIP) strcat(buf, "FLIP ");
    return buf;
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 16, frames = 300, windowed = 0, i, argn = 0;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    DDCAPS hal, hel;
    HRESULT hr;
    WNDCLASSA wc;
    HWND hwnd;
    DWORD t0, t1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-windowed")) windowed = 1;
        else if (argn == 0) { w = atoi(argv[i]); argn++; }
        else if (argn == 1) { h = atoi(argv[i]); argn++; }
        else if (argn == 2) { bpp = atoi(argv[i]); argn++; }
        else if (argn == 3) { frames = atoi(argv[i]); argn++; }
    }
    logf = fopen("ddtest.log", "w");
    logp("ddtest: %dx%d %d bpp, %d frames%s\n", w, h, bpp, frames, windowed ? ", windowed" : "");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ddtest";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "ddtest", "ddtest", windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP,
                           0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    pump();

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx failed %08lx\n", hr); return 1; }

    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    hr = dd->lpVtbl->GetCaps(dd, &hal, &hel);
    logp("GetCaps %08lx: HAL dwCaps %08lx dwCaps2 %08lx ddsCaps %08lx vidmem %lu/%lu KiB\n", hr,
         hal.dwCaps, hal.dwCaps2, hal.ddsCaps.dwCaps, hal.dwVidMemFree / 1024, hal.dwVidMemTotal / 1024);
    logp("  HAL: %s%s%s%s%s\n",
         hal.dwCaps & DDCAPS_NOHARDWARE ? "NOHARDWARE " : "HAL-present ",
         hal.dwCaps & DDCAPS_BLT ? "BLT " : "no-blt ",
         hal.dwCaps & DDCAPS_GDI ? "GDI " : "",
         hal.ddsCaps.dwCaps & DDSCAPS_FLIP ? "FLIP " : "no-flip ",
         hal.dwCaps & DDCAPS_3D ? "3D " : "no-3D ");

    hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd, windowed ? DDSCL_NORMAL :
                                         (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT));
    logp("SetCooperativeLevel %08lx\n", hr);
    if (!windowed) {
        hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp, 0, 0);
        logp("SetDisplayMode %dx%dx%d %08lx\n", w, h, bpp, hr);
        if (FAILED(hr)) goto out;
    }

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (windowed) {
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    } else {
        sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        sd.dwBackBufferCount = 1;
    }
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    logp("CreateSurface(primary%s) %08lx\n", windowed ? "" : " + 1 back buffer", hr);
    if (FAILED(hr)) goto out;
    if (windowed) {
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = w; sd.dwHeight = h;
        hr = dd->lpVtbl->CreateSurface(dd, &sd, &back, NULL);
        logp("CreateSurface(offscreen vidmem) %08lx\n", hr);
        if (FAILED(hr)) goto out;
    } else {
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        hr = prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
        logp("GetAttachedSurface(back) %08lx\n", hr);
        if (FAILED(hr)) goto out;
    }
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    prim->lpVtbl->GetSurfaceDesc(prim, &sd);
    logp("primary: %lux%lu %lu bpp pitch %ld caps %08lx %s\n", sd.dwWidth, sd.dwHeight,
         sd.ddpfPixelFormat.dwRGBBitCount, sd.lPitch, sd.ddsCaps.dwCaps, caps_str(sd.ddsCaps.dwCaps));
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    back->lpVtbl->GetSurfaceDesc(back, &sd);
    logp("back:    %lux%lu %lu bpp pitch %ld caps %08lx %s\n", sd.dwWidth, sd.dwHeight,
         sd.ddpfPixelFormat.dwRGBBitCount, sd.lPitch, sd.ddsCaps.dwCaps, caps_str(sd.ddsCaps.dwCaps));

    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        DDBLTFX fx;
        RECT r;
        unsigned y, x;

        pump();
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        if (FAILED(hr)) { logp("Lock failed %08lx at frame %d\n", hr, i); goto out; }
        for (y = 0; y < sd.dwHeight; y++) {
            unsigned char *row = (unsigned char *)sd.lpSurface + y * sd.lPitch;
            unsigned c = ((y + i * 2) / 16) & 1;
            if (sd.ddpfPixelFormat.dwRGBBitCount == 32) {
                unsigned *p = (unsigned *)row;
                unsigned v = c ? 0x00204080 : 0x00c0a040;
                for (x = 0; x < sd.dwWidth; x++) p[x] = ((x + i) / 16 & 1) ? v : v ^ 0x00ffffff;
            } else {
                unsigned short *p = (unsigned short *)row;
                unsigned short v = c ? 0x2210 : 0xc528;
                for (x = 0; x < sd.dwWidth; x++) p[x] = ((x + i) / 16 & 1) ? v : v ^ 0xffff;
            }
        }
        hr = back->lpVtbl->Unlock(back, NULL);
        if (FAILED(hr)) { logp("Unlock failed %08lx\n", hr); goto out; }

        /* a moving bar through Blt (HEL or HAL, whichever DirectDraw picks) */
        memset(&fx, 0, sizeof(fx)); fx.dwSize = sizeof(fx);
        fx.dwFillColor = sd.ddpfPixelFormat.dwRGBBitCount == 32 ? 0x00ff2020 : 0xf800;
        r.left = (i * 3) % (w - 40); r.right = r.left + 40; r.top = h / 4; r.bottom = h * 3 / 4;
        hr = back->lpVtbl->Blt(back, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (FAILED(hr)) { logp("Blt(COLORFILL) failed %08lx at frame %d\n", hr, i); goto out; }

        if (windowed) {
            hr = prim->lpVtbl->Blt(prim, NULL, back, NULL, DDBLT_WAIT, NULL);
        } else {
            hr = prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        }
        if (FAILED(hr)) { logp("%s failed %08lx at frame %d\n", windowed ? "Blt(primary)" : "Flip", hr, i); goto out; }
        if (i == 60) {
            t1 = GetTickCount();
            logp("first 60 frames: %lu ms\n", t1 - t0);
        }
    }
    t1 = GetTickCount();
    logp("%d frames in %lu ms = %.1f fps\n", frames, t1 - t0, t1 > t0 ? frames * 1000.0 / (t1 - t0) : 0.0);

    hr = dd->lpVtbl->WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logp("WaitForVerticalBlank %08lx\n", hr);

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (SUCCEEDED(hr)) {
        dump_bmp("ddtest.bmp", &sd);
        back->lpVtbl->Unlock(back, NULL);
        logp("ddtest.bmp written (last back buffer)\n");
    }
out:
    if (back && !windowed) back = NULL;      /* attached: released with the primary */
    else if (back) back->lpVtbl->Release(back);
    if (prim) prim->lpVtbl->Release(prim);
    if (dd) {
        if (!windowed) dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
    DestroyWindow(hwnd);
    logp("ddtest: done\n");
    if (logf) fclose(logf);
    return 0;
}
