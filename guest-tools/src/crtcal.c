/*
 * crtcal.c — the CRT calibration patterns on a real tube (doc 09).
 *
 *   CRTCAL.EXE [w h [bpp]]        default 640 480 32
 *
 * Exclusive full-screen DirectDraw at the exact mode, patterns written
 * straight into the primary surface — no blit, no stretch, no scaling
 * anywhere, because a scaler is exactly what would be measured otherwise.
 * Runs on the reference rig (doc 09) and, unchanged, in our own guests, so
 * the same binary gives the photograph and the emulated comparison.
 *
 *   SPACE / RIGHT   next pattern        1..8   jump to a pattern
 *   BACKSPACE/LEFT  previous            L      legend on/off
 *   M               next mode           ESC    quit
 *
 * The patterns and what each one asks are in crtcal.h; how to photograph
 * them is in doc 09.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crtcal.h"

static FILE *calf;

static void logp(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (calf) { fputs(buf, calf); fflush(calf); }
}

static const struct { int w, h; } MODES[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 },
    { 320, 200 }, { 320, 240 }, { 640, 400 },
};
#define NMODES ((int)(sizeof MODES / sizeof *MODES))

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* the legend: pattern number and mode, in the pattern's own 3x5 digits, so
   a photograph always says what it is looking at. 'L' takes it away for the
   shots where even a corner of lit phosphor is unwelcome. */
static void legend(uint32_t *fb, int w, int h, int pat, int mw, int mh)
{
    int s = (w >= 1024) ? 2 : 1, x = 4, y = h - 6 * s - 4;
    uint32_t c = CRTCAL_GREY(0x90);
    crtcal__num(fb, w, h, x, y, pat + 1, s, c);
    x += 6 * s;
    crtcal__num(fb, w, h, x, y, mw, s, c);
    x += 4 * s * 4;
    crtcal__num(fb, w, h, x, y, mh, s, c);
}

/* XRGB8888 -> the surface's format, row by row (no scaling: sizes match) */
static void blit(const uint32_t *fb, int w, int h, DDSURFACEDESC2 *sd, int bpp)
{
    int x, y;
    for (y = 0; y < h; y++) {
        unsigned char *row = (unsigned char *)sd->lpSurface + (size_t)y * sd->lPitch;
        const uint32_t *src = fb + (size_t)y * w;
        if (bpp == 32) {
            memcpy(row, src, (size_t)w * 4);
        } else if (bpp == 16) {
            unsigned short *d = (unsigned short *)row;
            for (x = 0; x < w; x++) {
                uint32_t p = src[x];
                d[x] = (unsigned short)((((p >> 16) & 0xf8) << 8) |
                                        (((p >> 8) & 0xfc) << 3) |
                                        ((p & 0xff) >> 3));
            }
        } else {                       /* 24 bpp */
            for (x = 0; x < w; x++) {
                uint32_t p = src[x];
                row[x * 3 + 0] = p & 0xff;
                row[x * 3 + 1] = (p >> 8) & 0xff;
                row[x * 3 + 2] = (p >> 16) & 0xff;
            }
        }
    }
}

static int keydown(int vk)          /* edge-triggered: one step per press */
{
    static unsigned char was[256];
    int now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = now && !was[vk & 0xff];
    was[vk & 0xff] = (unsigned char)now;
    return hit;
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 32, mode = 0, pat = 0, show_legend = 1;
    int redraw = 1, running = 1, i;
    HWND hwnd;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL;
    DDSURFACEDESC2 sd;
    uint32_t *fb = NULL;
    HRESULT hr;

    calf = fopen("crtcal.log", "w");
    if (argc >= 3) { w = atoi(argv[1]); h = atoi(argv[2]); }
    if (argc >= 4) bpp = atoi(argv[3]);
    for (i = 0; i < NMODES; i++)
        if (MODES[i].w == w && MODES[i].h == h) mode = i;

    logp("CRTCAL — calibration patterns (doc 09), %dx%dx%d\n", w, h, bpp);
    for (i = 0; i < CRTCAL_COUNT; i++)
        logp("  %d %-10s %s\n", i + 1, crtcal_name(i), crtcal_asks(i));

    hwnd = CreateWindowExA(0, "STATIC", "crtcal", WS_POPUP | WS_VISIBLE,
                           0, 0, 16, 16, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { logp("CreateWindow failed\n"); return 1; }
    SetForegroundWindow(hwnd);

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx %08lx\n", hr); return 1; }

    while (running) {
        hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd,
                 DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
        if (FAILED(hr)) { logp("SetCooperativeLevel %08lx\n", hr); break; }
        hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp, 0, 0);
        if (FAILED(hr)) {
            logp("SetDisplayMode %dx%dx%d failed %08lx — skipping\n", w, h, bpp, hr);
            mode = (mode + 1) % NMODES;
            w = MODES[mode].w; h = MODES[mode].h;
            continue;
        }
        logp("mode %dx%dx%d\n", w, h, bpp);

        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
        if (FAILED(hr)) { logp("CreateSurface(primary) %08lx\n", hr); break; }

        free(fb);
        fb = (uint32_t *)malloc((size_t)w * h * 4);
        if (!fb) { logp("out of memory for %dx%d\n", w, h); break; }
        redraw = 1;

        for (;;) {
            pump();
            if (redraw) {
                crtcal_render(pat, w, h, fb);
                if (show_legend) legend(fb, w, h, pat, w, h);
                memset(&sd, 0, sizeof sd);
                sd.dwSize = sizeof sd;
                hr = prim->lpVtbl->Lock(prim, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
                if (hr == DDERR_SURFACELOST) { prim->lpVtbl->Restore(prim); continue; }
                if (FAILED(hr)) { logp("Lock %08lx\n", hr); running = 0; break; }
                blit(fb, w, h, &sd, sd.ddpfPixelFormat.dwRGBBitCount);
                prim->lpVtbl->Unlock(prim, NULL);
                logp("  showing %d %s — %s\n", pat + 1, crtcal_name(pat), crtcal_shot(pat));
                redraw = 0;
            }
            if (keydown(VK_ESCAPE)) { running = 0; break; }
            if (keydown(VK_SPACE) || keydown(VK_RIGHT)) {
                pat = (pat + 1) % CRTCAL_COUNT; redraw = 1;
            }
            if (keydown(VK_BACK) || keydown(VK_LEFT)) {
                pat = (pat + CRTCAL_COUNT - 1) % CRTCAL_COUNT; redraw = 1;
            }
            if (keydown('L')) { show_legend = !show_legend; redraw = 1; }
            for (i = 0; i < CRTCAL_COUNT && i < 9; i++)
                if (keydown('1' + i)) { pat = i; redraw = 1; }
            if (keydown('M')) {
                mode = (mode + 1) % NMODES;
                w = MODES[mode].w; h = MODES[mode].h;
                break;                       /* re-enter with the new mode */
            }
            Sleep(15);
        }
        if (prim) { prim->lpVtbl->Release(prim); prim = NULL; }
    }

    free(fb);
    if (dd) {
        dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
    DestroyWindow(hwnd);
    logp("done\n");
    if (calf) fclose(calf);
    return 0;
}
