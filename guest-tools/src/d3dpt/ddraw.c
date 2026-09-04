/*
 * ddraw.dll shim for the paravirtual Direct3D folder (doc 14): forwards
 * every entry point to the system ddraw.dll and wraps the IDirectDraw7 that
 * DirectDrawCreateEx hands out so that GetAvailableVidMem / GetCaps report
 * a card with plenty of video memory.
 *
 * Why: GTA Vice City (and other RenderWare titles) decide whether to run
 * from DirectDraw 7's GetAvailableVidMem, not from Direct3D:
 * psInitialize() refuses below 12 MB ("cannot find enough available video
 * memory") and _psGetVideoModeList() drops every mode that leaves less than
 * 12 MB after the frame buffer. XP's Cirrus driver answers for the 4 MB the
 * emulated card has, while the rendering goes to our d3d8.dll and the host
 * GPU. Copy this DDRAW.DLL next to the EXE together with D3D8.DLL.
 *
 * Build (guest-tools/build-wrappers.sh):
 *   i686-w64-mingw32-gcc -O2 -Wall -shared -o ddraw.dll ddraw.c ddraw.def -Wl,--kill-at
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stddef.h>

#define VIDMEM_TOTAL (256u << 20)
#define VIDMEM_FREE  (256u << 20)

static HMODULE real;
static HRESULT (WINAPI *p_DirectDrawCreate)(GUID *, LPDIRECTDRAW *, IUnknown *);
static HRESULT (WINAPI *p_DirectDrawCreateEx)(GUID *, LPVOID *, REFIID, IUnknown *);
static HRESULT (WINAPI *p_DirectDrawCreateClipper)(DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *);
static HRESULT (WINAPI *p_DirectDrawEnumerateA)(LPDDENUMCALLBACKA, LPVOID);
static HRESULT (WINAPI *p_DirectDrawEnumerateW)(LPDDENUMCALLBACKW, LPVOID);
static HRESULT (WINAPI *p_DirectDrawEnumerateExA)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
static HRESULT (WINAPI *p_DirectDrawEnumerateExW)(LPDDENUMCALLBACKEXW, LPVOID, DWORD);
static HRESULT (WINAPI *p_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
static HRESULT (WINAPI *p_DllCanUnloadNow)(void);

static void dlog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

static int load_real(void)
{
    char path[MAX_PATH];
    if (real) return 1;
    if (!GetSystemDirectoryA(path, sizeof path)) return 0;
    strncat(path, "\\ddraw.dll", sizeof path - strlen(path) - 1);
    real = LoadLibraryA(path);
    if (!real) { dlog("d3dpt-ddraw: cannot load %s", path); return 0; }
#define GP(n) p_##n = (void *)GetProcAddress(real, #n)
    GP(DirectDrawCreate); GP(DirectDrawCreateEx); GP(DirectDrawCreateClipper);
    GP(DirectDrawEnumerateA); GP(DirectDrawEnumerateW); GP(DirectDrawEnumerateExA); GP(DirectDrawEnumerateExW);
    GP(DllGetClassObject); GP(DllCanUnloadNow);
#undef GP
    dlog("d3dpt-ddraw: forwarding to %s, video memory reported as %u MB", path, VIDMEM_TOTAL >> 20);
    return 1;
}

/* ------------------------------------------------ IDirectDraw7 wrapper */
typedef struct { IDirectDraw7Vtbl *lpVtbl; IDirectDraw7 *inner; LONG ref; } Wrap7;
#define INNER(This) (((Wrap7 *)(This))->inner)

static HRESULT WINAPI w7_QueryInterface(IDirectDraw7 *This, REFIID riid, void **pp)
{
    if (!pp) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IDirectDraw7) || IsEqualGUID(riid, &IID_IUnknown)) {
        *pp = This; InterlockedIncrement(&((Wrap7 *)This)->ref); return S_OK;
    }
    return IDirectDraw7_QueryInterface(INNER(This), riid, pp);
}
static ULONG WINAPI w7_AddRef(IDirectDraw7 *This)
{
    IDirectDraw7_AddRef(INNER(This));
    return InterlockedIncrement(&((Wrap7 *)This)->ref);
}
static ULONG WINAPI w7_Release(IDirectDraw7 *This)
{
    Wrap7 *w = (Wrap7 *)This;
    LONG r = InterlockedDecrement(&w->ref);
    IDirectDraw7_Release(w->inner);
    if (r == 0) HeapFree(GetProcessHeap(), 0, w);
    return r;
}
#define FWD(ret, name, params, args) \
    static ret WINAPI w7_##name params { return IDirectDraw7_##name args; }
FWD(HRESULT, Compact, (IDirectDraw7 *This), (INNER(This)))
FWD(HRESULT, CreateClipper, (IDirectDraw7 *This, DWORD a, LPDIRECTDRAWCLIPPER *b, IUnknown *c), (INNER(This), a, b, c))
FWD(HRESULT, CreatePalette, (IDirectDraw7 *This, DWORD a, LPPALETTEENTRY b, LPDIRECTDRAWPALETTE *c, IUnknown *d), (INNER(This), a, b, c, d))
FWD(HRESULT, CreateSurface, (IDirectDraw7 *This, LPDDSURFACEDESC2 a, LPDIRECTDRAWSURFACE7 *b, IUnknown *c), (INNER(This), a, b, c))
FWD(HRESULT, DuplicateSurface, (IDirectDraw7 *This, LPDIRECTDRAWSURFACE7 a, LPDIRECTDRAWSURFACE7 *b), (INNER(This), a, b))
FWD(HRESULT, EnumDisplayModes, (IDirectDraw7 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMMODESCALLBACK2 d), (INNER(This), a, b, c, d))
FWD(HRESULT, EnumSurfaces, (IDirectDraw7 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMSURFACESCALLBACK7 d), (INNER(This), a, b, c, d))
FWD(HRESULT, FlipToGDISurface, (IDirectDraw7 *This), (INNER(This)))
FWD(HRESULT, GetDisplayMode, (IDirectDraw7 *This, LPDDSURFACEDESC2 a), (INNER(This), a))
FWD(HRESULT, GetFourCCCodes, (IDirectDraw7 *This, LPDWORD a, LPDWORD b), (INNER(This), a, b))
FWD(HRESULT, GetGDISurface, (IDirectDraw7 *This, LPDIRECTDRAWSURFACE7 *a), (INNER(This), a))
FWD(HRESULT, GetMonitorFrequency, (IDirectDraw7 *This, LPDWORD a), (INNER(This), a))
FWD(HRESULT, GetScanLine, (IDirectDraw7 *This, LPDWORD a), (INNER(This), a))
FWD(HRESULT, GetVerticalBlankStatus, (IDirectDraw7 *This, WINBOOL *a), (INNER(This), a))
FWD(HRESULT, Initialize, (IDirectDraw7 *This, GUID *a), (INNER(This), a))
FWD(HRESULT, RestoreDisplayMode, (IDirectDraw7 *This), (INNER(This)))
FWD(HRESULT, SetCooperativeLevel, (IDirectDraw7 *This, HWND a, DWORD b), (INNER(This), a, b))
FWD(HRESULT, SetDisplayMode, (IDirectDraw7 *This, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e), (INNER(This), a, b, c, d, e))
FWD(HRESULT, WaitForVerticalBlank, (IDirectDraw7 *This, DWORD a, HANDLE b), (INNER(This), a, b))
FWD(HRESULT, GetSurfaceFromDC, (IDirectDraw7 *This, HDC a, LPDIRECTDRAWSURFACE7 *b), (INNER(This), a, b))
FWD(HRESULT, RestoreAllSurfaces, (IDirectDraw7 *This), (INNER(This)))
FWD(HRESULT, TestCooperativeLevel, (IDirectDraw7 *This), (INNER(This)))
FWD(HRESULT, GetDeviceIdentifier, (IDirectDraw7 *This, LPDDDEVICEIDENTIFIER2 a, DWORD b), (INNER(This), a, b))
FWD(HRESULT, StartModeTest, (IDirectDraw7 *This, LPSIZE a, DWORD b, DWORD c), (INNER(This), a, b, c))
FWD(HRESULT, EvaluateMode, (IDirectDraw7 *This, DWORD a, DWORD *b), (INNER(This), a, b))

static HRESULT WINAPI w7_GetCaps(IDirectDraw7 *This, LPDDCAPS hal, LPDDCAPS hel)
{
    HRESULT hr = IDirectDraw7_GetCaps(INNER(This), hal, hel);
    if (SUCCEEDED(hr) && hal && hal->dwSize >= offsetof(DDCAPS, dwVidMemFree) + sizeof(DWORD)) {
        hal->dwVidMemTotal = VIDMEM_TOTAL;
        hal->dwVidMemFree = VIDMEM_FREE;
    }
    return hr;
}
static HRESULT WINAPI w7_GetAvailableVidMem(IDirectDraw7 *This, LPDDSCAPS2 caps, LPDWORD total, LPDWORD free)
{
    DWORD t = 0, f = 0;
    HRESULT hr = IDirectDraw7_GetAvailableVidMem(INNER(This), caps, &t, &f);
    static int once;
    if (!once) { once = 1; dlog("d3dpt-ddraw: GetAvailableVidMem: driver says %u/%u MB, answering %u/%u MB (hr 0x%08lx)", t >> 20, f >> 20, VIDMEM_TOTAL >> 20, VIDMEM_FREE >> 20, (unsigned long)hr); }
    if (total) *total = VIDMEM_TOTAL;
    if (free) *free = VIDMEM_FREE;
    return DD_OK;
}

static IDirectDraw7Vtbl w7_vtbl = {
    w7_QueryInterface, w7_AddRef, w7_Release, w7_Compact, w7_CreateClipper, w7_CreatePalette,
    w7_CreateSurface, w7_DuplicateSurface, w7_EnumDisplayModes, w7_EnumSurfaces, w7_FlipToGDISurface,
    w7_GetCaps, w7_GetDisplayMode, w7_GetFourCCCodes, w7_GetGDISurface, w7_GetMonitorFrequency,
    w7_GetScanLine, w7_GetVerticalBlankStatus, w7_Initialize, w7_RestoreDisplayMode,
    w7_SetCooperativeLevel, w7_SetDisplayMode, w7_WaitForVerticalBlank, w7_GetAvailableVidMem,
    w7_GetSurfaceFromDC, w7_RestoreAllSurfaces, w7_TestCooperativeLevel, w7_GetDeviceIdentifier,
    w7_StartModeTest, w7_EvaluateMode,
};

static IDirectDraw7 *wrap7(IDirectDraw7 *inner)
{
    Wrap7 *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    if (!w) return inner;
    w->lpVtbl = &w7_vtbl; w->inner = inner; w->ref = 1;
    return (IDirectDraw7 *)w;
}

/* ------------------------------------------------------------ exports */
HRESULT WINAPI DirectDrawCreate(GUID *guid, LPDIRECTDRAW *dd, IUnknown *outer)
{
    if (!load_real() || !p_DirectDrawCreate) return DDERR_GENERIC;
    return p_DirectDrawCreate(guid, dd, outer);
}
HRESULT WINAPI DirectDrawCreateEx(GUID *guid, LPVOID *pp, REFIID iid, IUnknown *outer)
{
    HRESULT hr;
    if (!load_real() || !p_DirectDrawCreateEx) return DDERR_GENERIC;
    hr = p_DirectDrawCreateEx(guid, pp, iid, outer);
    if (SUCCEEDED(hr) && pp && *pp && IsEqualGUID(iid, &IID_IDirectDraw7)) {
        *pp = wrap7((IDirectDraw7 *)*pp);
    }
    return hr;
}
HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER *c, IUnknown *outer)
{
    if (!load_real() || !p_DirectDrawCreateClipper) return DDERR_GENERIC;
    return p_DirectDrawCreateClipper(flags, c, outer);
}
HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
    if (!load_real() || !p_DirectDrawEnumerateA) return DDERR_GENERIC;
    return p_DirectDrawEnumerateA(cb, ctx);
}
HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, LPVOID ctx)
{
    if (!load_real() || !p_DirectDrawEnumerateW) return DDERR_GENERIC;
    return p_DirectDrawEnumerateW(cb, ctx);
}
HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD flags)
{
    if (!load_real() || !p_DirectDrawEnumerateExA) return DDERR_GENERIC;
    return p_DirectDrawEnumerateExA(cb, ctx, flags);
}
HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, LPVOID ctx, DWORD flags)
{
    if (!load_real() || !p_DirectDrawEnumerateExW) return DDERR_GENERIC;
    return p_DirectDrawEnumerateExW(cb, ctx, flags);
}
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *pp)
{
    if (!load_real() || !p_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return p_DllGetClassObject(clsid, iid, pp);
}
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
