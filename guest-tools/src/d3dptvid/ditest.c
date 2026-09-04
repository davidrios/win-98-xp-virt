/*
 * ditest.c — does a game-style DirectInput keyboard see the keys the guest
 * receives, under load?  Found with FIFA 2000 on the d3dpt-vga HAL
 * (2026-09-04): keys work in the match under KVM and not under TCG.
 *
 *   DITEST [seconds] [busy-ms] [-window] [-nonexcl]
 *
 * A fullscreen 640x480x16 DirectDraw flip chain (or a plain window with
 * -window), a DirectInput keyboard device created the DX5/6 way
 * (DirectInputCreateA, DISCL_FOREGROUND | DISCL_EXCLUSIVE, a 32-event
 * buffer), then a loop that burns busy-ms of CPU per iteration (a game's
 * frame), pumps the message queue and polls four sources: DirectInput
 * buffered data (GetDeviceData), DirectInput immediate state
 * (GetDeviceState), GetAsyncKeyState, and WM_KEYDOWN. Every key-down each
 * source sees is logged with its time; the fill colour of the flip chain
 * changes on every buffered DirectInput key, so a screendump shows whether
 * DirectInput got the key. Totals at the end. Log: ditest.log.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *logf;
static DWORD t0;
static void logp(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
}
static DWORD now(void) { return GetTickCount() - t0; }

static int n_di_buf, n_di_state, n_async, n_msg, n_reacquire, n_lost, n_overflow;
static volatile int fill_index;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(lp & (1 << 30))) {                     /* not an auto-repeat */
            n_msg++;
            logp("  msg    down vk 0x%02x t=%lu\n", (unsigned)wp, now());
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void spin(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    volatile double x = 1.0;
    while (GetTickCount() < end)
        for (int i = 0; i < 1000; i++) x = x * 1.0000001 + 0.5;
}

int main(int argc, char **argv)
{
    int seconds = 30, busy = 0, window = 0, excl = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-window")) window = 1;
        else if (!strcmp(argv[i], "-nonexcl")) excl = 0;
        else if (seconds == 30 && atoi(argv[i]) > 0 && i == 1) seconds = atoi(argv[i]);
        else busy = atoi(argv[i]);
    }
    logf = fopen("ditest.log", "w");
    t0 = GetTickCount();
    logp("ditest: %d s, busy %d ms per iteration, %s, %s\n", seconds, busy,
         window ? "plain window" : "DirectDraw 640x480x16 fullscreen", excl ? "DISCL_EXCLUSIVE|FOREGROUND" : "DISCL_NONEXCLUSIVE|FOREGROUND");

    HINSTANCE hinst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = "ditest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(window ? 0 : WS_EX_TOPMOST, "ditest", "ditest",
                                window ? WS_OVERLAPPEDWINDOW : WS_POPUP, 0, 0, 640, 480, NULL, NULL, hinst, NULL);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 primary = NULL, back = NULL;
    if (!window) {
        HRESULT hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
        if (SUCCEEDED(hr)) hr = IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
        if (SUCCEEDED(hr)) hr = IDirectDraw7_SetDisplayMode(dd, 640, 480, 16, 0, 0);
        if (SUCCEEDED(hr)) {
            DDSURFACEDESC2 sd;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
            sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY;
            sd.dwBackBufferCount = 1;
            hr = IDirectDraw7_CreateSurface(dd, &sd, &primary, NULL);
            if (SUCCEEDED(hr)) {
                DDSCAPS2 caps;
                memset(&caps, 0, sizeof caps);
                caps.dwCaps = DDSCAPS_BACKBUFFER;
                hr = IDirectDrawSurface7_GetAttachedSurface(primary, &caps, &back);
            }
        }
        logp("ddraw: fullscreen 640x480x16 flip chain -> 0x%08lx\n", (unsigned long)hr);
        if (FAILED(hr)) { window = 1; back = NULL; }
    }

    LPDIRECTINPUT di = NULL;
    LPDIRECTINPUTDEVICE kbd = NULL;
    HRESULT hr = DirectInputCreateA(hinst, DIRECTINPUT_VERSION, &di, NULL);
    logp("dinput: DirectInputCreateA(0x%04x) -> 0x%08lx\n", DIRECTINPUT_VERSION, (unsigned long)hr);
    if (SUCCEEDED(hr)) hr = IDirectInput_CreateDevice(di, &GUID_SysKeyboard, &kbd, NULL);
    logp("dinput: CreateDevice(SysKeyboard) -> 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) hr = IDirectInputDevice_SetDataFormat(kbd, &c_dfDIKeyboard);
    logp("dinput: SetDataFormat -> 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) hr = IDirectInputDevice_SetCooperativeLevel(kbd, hwnd, DISCL_FOREGROUND | (excl ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE));
    logp("dinput: SetCooperativeLevel -> 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        DIPROPDWORD p;
        memset(&p, 0, sizeof p);
        p.diph.dwSize = sizeof p;
        p.diph.dwHeaderSize = sizeof p.diph;
        p.diph.dwHow = DIPH_DEVICE;
        p.dwData = 32;
        hr = IDirectInputDevice_SetProperty(kbd, DIPROP_BUFFERSIZE, &p.diph);
        logp("dinput: SetProperty(BUFFERSIZE 32) -> 0x%08lx\n", (unsigned long)hr);
    }
    if (SUCCEEDED(hr)) hr = IDirectInputDevice_Acquire(kbd);
    logp("dinput: Acquire -> 0x%08lx\n", (unsigned long)hr);
    int have_di = SUCCEEDED(hr);

    BYTE state[256], prev_state[256];
    SHORT async_prev[256];
    memset(prev_state, 0, sizeof prev_state);
    memset(async_prev, 0, sizeof async_prev);
    DWORD end = now() + (DWORD)seconds * 1000, next_alive = 1000;
    unsigned iters = 0;
    MSG msg;
    while (now() < end) {
        iters++;
        if (busy) spin(busy);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (have_di) {
            DIDEVICEOBJECTDATA ev[32];
            DWORD n = 32;
            hr = IDirectInputDevice_GetDeviceData(kbd, sizeof ev[0], ev, &n, 0);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                n_lost++;
                HRESULT ahr = IDirectInputDevice_Acquire(kbd);
                n_reacquire++;
                logp("  dinput lost (0x%08lx), Acquire -> 0x%08lx t=%lu\n", (unsigned long)hr, (unsigned long)ahr, now());
            } else if (SUCCEEDED(hr)) {
                if (hr == DI_BUFFEROVERFLOW) n_overflow++;
                for (DWORD i = 0; i < n; i++) {
                    if (ev[i].dwData & 0x80) {
                        n_di_buf++;
                        fill_index++;
                        logp("  di-buf down dik 0x%02lx t=%lu (seq %lu)\n", (unsigned long)ev[i].dwOfs, now(), (unsigned long)ev[i].dwSequence);
                    }
                }
            } else if (iters == 1) {
                logp("  GetDeviceData -> 0x%08lx\n", (unsigned long)hr);
            }
            hr = IDirectInputDevice_GetDeviceState(kbd, sizeof state, state);
            if (SUCCEEDED(hr)) {
                for (int k = 0; k < 256; k++) {
                    if ((state[k] & 0x80) && !(prev_state[k] & 0x80)) {
                        n_di_state++;
                        logp("  di-state down dik 0x%02x t=%lu\n", k, now());
                    }
                }
                memcpy(prev_state, state, sizeof state);
            }
        }
        for (int vk = 8; vk < 256; vk++) {
            SHORT s = GetAsyncKeyState(vk);
            if ((s & 0x8000) && !(async_prev[vk] & 0x8000)) {
                n_async++;
                logp("  async  down vk 0x%02x t=%lu\n", vk, now());
            }
            async_prev[vk] = s;
        }
        if (back) {
            static const DWORD colours[] = { 0x001f, 0x07e0, 0xf800, 0xffe0, 0x07ff, 0xf81f };
            DDBLTFX fx;
            memset(&fx, 0, sizeof fx);
            fx.dwSize = sizeof fx;
            fx.dwFillColor = colours[fill_index % 6];
            IDirectDrawSurface7_Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            IDirectDrawSurface7_Flip(primary, NULL, DDFLIP_WAIT);
        }
        if (now() >= next_alive) {
            logp("alive t=%lu iters=%u di-buf %d di-state %d async %d msg %d\n", now(), iters, n_di_buf, n_di_state, n_async, n_msg);
            next_alive += 5000;
        }
        if (!busy) Sleep(1);
    }
done:
    logp("totals: di-buf %d di-state %d async %d msg %d; lost %d reacquire %d overflow %d; %u iterations in %lu ms\n",
         n_di_buf, n_di_state, n_async, n_msg, n_lost, n_reacquire, n_overflow, iters, now());
    if (kbd) { IDirectInputDevice_Unacquire(kbd); IDirectInputDevice_Release(kbd); }
    if (di) IDirectInput_Release(di);
    if (primary) IDirectDrawSurface7_Release(primary);
    if (dd) { IDirectDraw7_RestoreDisplayMode(dd); IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL); IDirectDraw7_Release(dd); }
    DestroyWindow(hwnd);
    if (logf) fclose(logf);
    return 0;
}
