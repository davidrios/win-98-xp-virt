/*
 * d3dptdisp.c — the XP display driver DLL for the d3dpt-vga adapter (doc
 * 15, ADR-008 / M7a). Kernel mode (win32k loads it), no CRT; the GDI DDI
 * of winddi.h.
 *
 * The "framebuf" shape: the driver exposes the modes its miniport
 * (d3dptvid.sys) enumerates, switches to one, maps the linear frame
 * buffer and hands GDI an engine bitmap that *is* the frame buffer
 * (EngCreateBitmap over the mapped VRAM, no hooks). GDI then draws every
 * pixel itself, straight into guest VRAM, which QEMU shows without a copy
 * and the player uploads by dirty rectangle. The software cursor is
 * GDI's too (no DrvSetPointerShape). Nothing GDI does here is accelerated
 * on purpose: this step buys the mode table and the kernel workflow.
 *
 * M7b, the DirectDraw DDI (bottom of the file): the surface is a device
 * surface GDI still draws on (EngModifySurface with pvScan0), VRAM after
 * the primary is one linear heap dxg.sys allocates DirectDraw surfaces
 * from, DdMapMemory maps VRAM into the game's process, DdFlip is a write
 * of the back buffer's offset into the device's OFFSET register (a real
 * page flip, no copy) and DdWaitForVerticalBlank waits for the device's
 * frame counter. Blits are not hooked: DirectDraw's HEL does them on the
 * mapped VRAM. No Direct3D callbacks yet (M7c).
 *
 * Build: guest-tools/build-driver.sh.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#include "../../../d3dpt/d3dpt_fb.h"

#define ALLOC_TAG 0x64336d64   /* 'dm3d' */

/* -device d3dpt-vga,ddflags=N: bisection knob while the DDI is brought up */
#define DDF_NO_GETDRIVERINFO   0x1    /* no GetDriverInfo / GETDRIVERINFOSET */
#define DDF_NO_SURFACE_CB      0x4    /* only MapMemory + CanCreateSurface */
#define DDF_ENGINE_BITMAP      0x8    /* EngCreateBitmap primary instead of a device surface */
#define DDF_GDI_CAP            0x10   /* add DDCAPS_GDI to dwCaps: dxg then drops the HAL (kept as the repro) */

typedef struct _PDEV {
    HANDLE hDriver;             /* the miniport, for EngDeviceIoControl */
    HDEV hdev;                  /* GDI's handle for this PDEV */
    HSURF hsurf;
    HPALETTE hpal;
    ULONG mode_index;           /* miniport mode index */
    ULONG w, h, bpp, pitch, hz;
    ULONG rmask, gmask, bmask;
    PVOID fb;                   /* mapped frame buffer */
    ULONG fb_len;
    volatile ULONG *regs;       /* public access range (register page) */
    BOOL device_surface;        /* EngCreateDeviceSurface took: DirectDraw possible */
} PDEV, *PPDEV;

/* ------------------------------------------------------------ debug log */

static void dbg_puts(PPDEV p, const char *s)
{
    if (!p || !p->regs) {
        return;
    }
    while (*s) {
        p->regs[D3DPT_FB_REG_DEBUG / 4] = (unsigned char)*s++;
    }
}

static void dbg_hex(PPDEV p, const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    int i;

    if (!p || !p->regs) {
        return;
    }
    dbg_puts(p, tag);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(p, buf);
}

/* --------------------------------------------------------------- modes */

/* the miniport's list; the caller frees *out with EngFreeMem */
static ULONG get_modes(HANDLE hDriver, PVIDEO_MODE_INFORMATION *out)
{
    VIDEO_NUM_MODES nm;
    DWORD ret;
    ULONG bytes;
    PVIDEO_MODE_INFORMATION modes;

    *out = NULL;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL, 0,
                           &nm, sizeof(nm), &ret) != 0 || nm.NumModes == 0 ||
        nm.ModeInformationLength != sizeof(VIDEO_MODE_INFORMATION)) {
        return 0;
    }
    bytes = nm.NumModes * nm.ModeInformationLength;
    modes = EngAllocMem(FL_ZERO_MEMORY, bytes, ALLOC_TAG);
    if (!modes) {
        return 0;
    }
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0,
                           modes, bytes, &ret) != 0) {
        EngFreeMem(modes);
        return 0;
    }
    *out = modes;
    return nm.NumModes;
}

static WCHAR device_name[] = L"d3dptdisp";

ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    PVIDEO_MODE_INFORMATION modes;
    ULONG n, i, bytes;

    n = get_modes(hDriver, &modes);
    if (n == 0) {
        return 0;
    }
    bytes = n * sizeof(DEVMODEW);
    if (pdm == NULL) {
        EngFreeMem(modes);
        return bytes;
    }
    if (cjSize < bytes) {
        n = cjSize / sizeof(DEVMODEW);
        bytes = n * sizeof(DEVMODEW);
    }
    for (i = 0; i < n; i++) {
        DEVMODEW *dm = &pdm[i];
        ULONG j;
        for (j = 0; j < sizeof(*dm) / 4; j++) {
            ((ULONG *)dm)[j] = 0;
        }
        for (j = 0; device_name[j]; j++) {
            dm->dmDeviceName[j] = device_name[j];
        }
        dm->dmSpecVersion = DM_SPECVERSION;
        dm->dmDriverVersion = DM_SPECVERSION;
        dm->dmSize = sizeof(DEVMODEW);
        dm->dmDriverExtra = 0;
        dm->dmBitsPerPel = modes[i].NumberOfPlanes * modes[i].BitsPerPlane;
        dm->dmPelsWidth = modes[i].VisScreenWidth;
        dm->dmPelsHeight = modes[i].VisScreenHeight;
        dm->dmDisplayFrequency = modes[i].Frequency;
        dm->dmDisplayFlags = 0;
        dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                       DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    }
    EngFreeMem(modes);
    return bytes;
}

/* pick the miniport mode for a DEVMODE; zero fields mean "any" */
static BOOL pick_mode(PPDEV p, DEVMODEW *pdm)
{
    PVIDEO_MODE_INFORMATION modes, best = NULL;
    ULONG n, i;

    n = get_modes(p->hDriver, &modes);
    if (n == 0) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        PVIDEO_MODE_INFORMATION m = &modes[i];
        if (pdm->dmPelsWidth && m->VisScreenWidth != pdm->dmPelsWidth) continue;
        if (pdm->dmPelsHeight && m->VisScreenHeight != pdm->dmPelsHeight) continue;
        if (pdm->dmBitsPerPel && m->BitsPerPlane * m->NumberOfPlanes != pdm->dmBitsPerPel) continue;
        if (pdm->dmDisplayFrequency && pdm->dmDisplayFrequency != 1 &&
            m->Frequency != pdm->dmDisplayFrequency) continue;
        best = m;
        break;
    }
    if (!best) {
        EngFreeMem(modes);
        return FALSE;
    }
    p->mode_index = best->ModeIndex;
    p->w = best->VisScreenWidth;
    p->h = best->VisScreenHeight;
    p->bpp = best->BitsPerPlane * best->NumberOfPlanes;
    p->pitch = best->ScreenStride;
    p->hz = best->Frequency;
    p->rmask = best->RedMask;
    p->gmask = best->GreenMask;
    p->bmask = best->BlueMask;
    EngFreeMem(modes);
    return TRUE;
}

/* ------------------------------------------------------------ the PDEV */

/* GDI's standard fonts and the sRGB-ish COLORINFO of the DDK's framebuf */
static const LOGFONTW font_system = {
    16, 7, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"System"
};
static const LOGFONTW font_ansi_var = {
    12, 9, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"MS Sans Serif"
};
static const LOGFONTW font_ansi_fix = {
    12, 9, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, FIXED_PITCH | FF_DONTCARE, L"Courier"
};
static const COLORINFO color_info = {
    { 6700, 3300, 0 }, { 2100, 7100, 0 }, { 1400, 800, 0 },
    { 1750, 3950, 0 }, { 4050, 2050, 0 }, { 4400, 5200, 0 },
    { 3127, 3290, 0 },
    20000, 20000, 20000,
    0, 0, 0, 0, 0, 0
};

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR pwszLogAddress, ULONG cPat,
                              HSURF *phsurfPatterns, ULONG cjCaps, ULONG *pdevcaps,
                              ULONG cjDevInfo, DEVINFO *pdi, HDEV hdev,
                              LPWSTR pwszDeviceName, HANDLE hDriver)
{
    PPDEV p;
    GDIINFO *gi = (GDIINFO *)pdevcaps;
    GDIINFO g;
    DEVINFO d;
    ULONG i;

    if (cjCaps < sizeof(GDIINFO) || cjDevInfo < sizeof(DEVINFO)) {
        return NULL;
    }
    p = EngAllocMem(FL_ZERO_MEMORY, sizeof(*p), ALLOC_TAG);
    if (!p) {
        return NULL;
    }
    p->hDriver = hDriver;
    if (!pick_mode(p, pdm)) {
        EngFreeMem(p);
        return NULL;
    }

    for (i = 0; i < sizeof(g) / 4; i++) ((ULONG *)&g)[i] = 0;
    for (i = 0; i < sizeof(d) / 4; i++) ((ULONG *)&d)[i] = 0;

    g.ulVersion = GDI_DRIVER_VERSION;
    g.ulTechnology = DT_RASDISPLAY;
    g.ulHorzSize = 320;
    g.ulVertSize = 240;
    g.ulHorzRes = p->w;
    g.ulVertRes = p->h;
    g.cBitsPixel = p->bpp;
    g.cPlanes = 1;
    g.ulNumColors = (ULONG)-1;
    g.ulVRefresh = p->hz;
    g.ulBltAlignment = 1;
    g.ulLogPixelsX = pdm->dmLogPixels ? pdm->dmLogPixels : 96;
    g.ulLogPixelsY = g.ulLogPixelsX;
    g.flTextCaps = TC_RA_ABLE;
    if (p->bpp == 32) {
        g.ulDACRed = g.ulDACGreen = g.ulDACBlue = 8;
        g.ulHTOutputFormat = HT_FORMAT_32BPP;
    } else {
        g.ulDACRed = 5; g.ulDACGreen = 6; g.ulDACBlue = 5;
        g.ulHTOutputFormat = HT_FORMAT_16BPP;
    }
    g.ulAspectX = 36;
    g.ulAspectY = 36;
    g.ulAspectXY = 51;
    g.xStyleStep = 1;
    g.yStyleStep = 1;
    g.denStyleStep = 3;
    g.ulNumPalReg = 0;
    g.ciDevice = color_info;
    g.ulDevicePelsDPI = 0;
    g.ulPrimaryOrder = PRIMARY_ORDER_CBA;
    g.ulHTPatternSize = HT_PATSIZE_4x4_M;
    g.flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    g.ulPhysicalPixelCharacteristics = PPC_UNDEFINED;
    g.ulPhysicalPixelGamma = PPG_DEFAULT;
    *gi = g;

    d.flGraphicsCaps = 0;
    d.lfDefaultFont = font_system;
    d.lfAnsiVarFont = font_ansi_var;
    d.lfAnsiFixFont = font_ansi_fix;
    d.cFonts = 0;
    d.iDitherFormat = p->bpp == 32 ? BMF_32BPP : BMF_16BPP;
    d.cxDither = 0;
    d.cyDither = 0;
    d.hpalDefault = EngCreatePalette(PAL_BITFIELDS, 0, NULL, p->rmask, p->gmask, p->bmask);
    if (!d.hpalDefault) {
        EngFreeMem(p);
        return NULL;
    }
    p->hpal = d.hpalDefault;
    d.flGraphicsCaps2 = 0;
    *pdi = d;

    return (DHPDEV)p;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    ((PPDEV)dhpdev)->hdev = hdev;
}

VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;

    if (p->hpal) {
        EngDeletePalette(p->hpal);
    }
    EngFreeMem(p);
}

/* ---------------------------------------------------------- the surface */

static BOOL set_mode(PPDEV p)
{
    VIDEO_MODE vm;
    DWORD ret;

    vm.RequestedMode = p->mode_index;
    return EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE, &vm, sizeof(vm),
                              NULL, 0, &ret) == 0;
}

static void map_regs(PPDEV p)
{
    VIDEO_PUBLIC_ACCESS_RANGES r;
    DWORD ret;

    if (p->regs) {
        return;
    }
    if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES, NULL, 0,
                           &r, sizeof(r), &ret) == 0 && r.VirtualAddress) {
        p->regs = r.VirtualAddress;
    }
}

static void unmap_regs(PPDEV p)
{
    VIDEO_MEMORY vm;
    DWORD ret;

    if (!p->regs) {
        return;
    }
    vm.RequestedVirtualAddress = (PVOID)p->regs;
    EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES, &vm, sizeof(vm),
                       NULL, 0, &ret);
    p->regs = NULL;
}

HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;
    VIDEO_MEMORY vm;
    VIDEO_MEMORY_INFORMATION vmi;
    DWORD ret;
    SIZEL sizl;
    HSURF hsurf;

    map_regs(p);
    dbg_puts(p, "d3dptdisp: enable surface ");
    dbg_hex(p, "mode ", p->mode_index);
    dbg_puts(p, "\n");

    if (!set_mode(p)) {
        dbg_puts(p, "d3dptdisp: set mode failed\n");
        return NULL;
    }
    vm.RequestedVirtualAddress = NULL;
    if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY, &vm, sizeof(vm),
                           &vmi, sizeof(vmi), &ret) != 0) {
        dbg_puts(p, "d3dptdisp: map failed\n");
        return NULL;
    }
    p->fb = vmi.FrameBufferBase;
    p->fb_len = vmi.FrameBufferLength;

    sizl.cx = p->w;
    sizl.cy = p->h;
    /* A device surface (DirectDraw needs one) that GDI still draws on
     * itself: EngModifySurface hands it the frame buffer bytes. The hook /
     * flag combinations win32k accepts are not documented consistently, so
     * try them in order and say which one took; the engine bitmap of M7a
     * is the last resort (desktop works, no DirectDraw). */
    hsurf = (p->regs && (p->regs[D3DPT_FB_REG_DDFLAGS / 4] & DDF_ENGINE_BITMAP)) ? NULL :
            EngCreateDeviceSurface((DHSURF)p, sizl, p->bpp == 32 ? BMF_32BPP : BMF_16BPP);
    if (hsurf) {
        static const struct { FLONG hooks, surf; } variants[] = {
            { HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY },
            { 0, MS_NOTSYSTEMMEMORY },
            { HOOK_SYNCHRONIZE, 0 },
            { 0, 0 },
        };
        ULONG v;
        for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            if (EngModifySurface(hsurf, p->hdev, variants[v].hooks, variants[v].surf,
                                 (DHSURF)p, p->fb, (LONG)p->pitch, NULL)) {
                dbg_hex(p, "d3dptdisp: device surface, variant ", v);
                dbg_puts(p, "\n");
                p->device_surface = TRUE;
                break;
            }
        }
        if (!p->device_surface) {
            dbg_puts(p, "d3dptdisp: EngModifySurface refused every variant\n");
            EngDeleteSurface(hsurf);
            hsurf = NULL;
        }
    } else {
        dbg_puts(p, "d3dptdisp: EngCreateDeviceSurface failed\n");
    }
    if (!hsurf) {
        hsurf = (HSURF)EngCreateBitmap(sizl, p->pitch, p->bpp == 32 ? BMF_32BPP : BMF_16BPP,
                                       BMF_TOPDOWN | BMF_NOZEROINIT, p->fb);
        if (!hsurf) {
            dbg_puts(p, "d3dptdisp: EngCreateBitmap failed\n");
            goto unmap;
        }
        if (!EngAssociateSurface(hsurf, p->hdev, 0)) {
            dbg_puts(p, "d3dptdisp: EngAssociateSurface failed\n");
            EngDeleteSurface(hsurf);
            goto unmap;
        }
        dbg_puts(p, "d3dptdisp: engine bitmap surface (no DirectDraw)\n");
    }
    p->hsurf = hsurf;
    dbg_hex(p, "d3dptdisp: surface ", (ULONG)(ULONG_PTR)p->fb);
    dbg_hex(p, " pitch ", p->pitch);
    dbg_puts(p, "\n");
    return hsurf;

unmap:
    vm.RequestedVirtualAddress = p->fb;
    EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm), NULL, 0, &ret);
    p->fb = NULL;
    return NULL;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;
    VIDEO_MEMORY vm;
    DWORD ret;

    dbg_puts(p, "d3dptdisp: disable surface\n");
    if (p->hsurf) {
        EngDeleteSurface(p->hsurf);
        p->hsurf = NULL;
    }
    if (p->fb) {
        vm.RequestedVirtualAddress = p->fb;
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm),
                           NULL, 0, &ret);
        p->fb = NULL;
    }
    unmap_regs(p);
}

/* GDI calls this before touching the surface when HOOK_SYNCHRONIZE is set:
 * nothing to wait for, every access is a plain memory write */
VOID APIENTRY DrvSynchronizeSurface(SURFOBJ *pso, RECTL *prcl, FLONG fl)
{
}

BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable)
{
    PPDEV p = (PPDEV)dhpdev;
    DWORD ret;

    if (bEnable) {
        dbg_puts(p, "d3dptdisp: assert mode on\n");
        return set_mode(p);
    }
    dbg_puts(p, "d3dptdisp: assert mode off\n");
    /* another PDEV (a full-screen console, the logon desktop switching) takes
     * the screen: back to VGA text through the miniport */
    return EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0,
                              NULL, 0, &ret) == 0;
}

/* ------------------------------------------------------------ DirectDraw
 * dxg.sys drives these (ddrawint.h, the NT DirectDraw DDI). Surfaces come
 * out of one linear heap in VRAM behind the primary; the runtime does the
 * allocation and the HEL blits, we do memory mapping, flips and vblank. */

static ULONG heap_start(PPDEV p)
{
    return (p->pitch * p->h + 4095) & ~4095u;
}


static ULONG ddflags(PPDEV p)
{
    return p->regs ? p->regs[D3DPT_FB_REG_DDFLAGS / 4] : 0;
}

static void wait_frame(PPDEV p)
{
    ULONG f;
    LONGLONG t0, t, freq;

    if (!p->regs) {
        return;
    }
    f = p->regs[D3DPT_FB_REG_FRAMES / 4];
    EngQueryPerformanceFrequency(&freq);
    EngQueryPerformanceCounter(&t0);
    /* the counter moves at the player's refresh pull; give up after 50 ms */
    for (;;) {
        if (p->regs[D3DPT_FB_REG_FRAMES / 4] != f) {
            return;
        }
        EngQueryPerformanceCounter(&t);
        if ((t - t0) * 20 > freq) {
            return;
        }
    }
}

static DWORD APIENTRY DdMapMemory(PDD_MAPMEMORYDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    VIDEO_SHARE_MEMORY sh;
    VIDEO_SHARE_MEMORY_INFORMATION info;
    DWORD ret;

    sh.ProcessHandle = d->hProcess;
    sh.ViewOffset = 0;
    sh.ViewSize = p->fb_len;
    if (d->bMap) {
        sh.RequestedVirtualAddress = NULL;
        if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SHARE_VIDEO_MEMORY, &sh, sizeof(sh),
                               &info, sizeof(info), &ret) != 0) {
            dbg_puts(p, "d3dptdisp: share video memory failed\n");
            d->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        d->fpProcess = (FLATPTR)info.VirtualAddress;
        dbg_hex(p, "d3dptdisp: dd map ", (ULONG)d->fpProcess);
        dbg_puts(p, "\n");
    } else {
        sh.RequestedVirtualAddress = (PVOID)d->fpProcess;
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY, &sh, sizeof(sh),
                           NULL, 0, &ret);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdWaitForVerticalBlank(PDD_WAITFORVERTICALBLANKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = FALSE;
        d->ddRVal = DD_OK;
        break;
    case DDWAITVB_BLOCKBEGIN:
    case DDWAITVB_BLOCKEND:
        wait_frame(p);
        d->ddRVal = DD_OK;
        break;
    default:
        d->ddRVal = DDERR_UNSUPPORTED;
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdCanCreateSurface(PDD_CANCREATESURFACEDATA d)
{
    /* the display format only: no Z buffers, FourCC or alpha formats here */
    d->ddRVal = d->bIsDifferentPixelFormat ? DDERR_INVALIDPIXELFORMAT : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlip(PDD_FLIPDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (!p->regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    /* the page flip: scan out from the target's VRAM offset */
    p->regs[D3DPT_FB_REG_OFFSET / 4] = (ULONG)d->lpSurfTarg->lpGbl->fpVidMem;
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdGetBltStatus(PDD_GETBLTSTATUSDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdGetFlipStatus(PDD_GETFLIPSTATUSDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdSetExclusiveMode(PDD_SETEXCLUSIVEMODEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    dbg_puts(p, d->dwEnterExcl ? "d3dptdisp: dd exclusive on\n" : "d3dptdisp: dd exclusive off\n");
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlipToGDISurface(PDD_FLIPTOGDISURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (d->dwToGDI && p->regs) {
        p->regs[D3DPT_FB_REG_OFFSET / 4] = 0;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static BOOL guid_eq(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

/* GUID_NTCallbacks of ddrawint.h; spelled out because INITGUID would define
 * every GUID of ddrawint.h and d3dnthal.h twice */
static const GUID guid_ntcallbacks = {
    0x6fe9ecde, 0xdf89, 0x11d1, { 0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba }
};

static DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA d)
{
    PPDEV p = (PPDEV)d->dhpdev;

    dbg_hex(p, "d3dptdisp: dd getinfo ", d->guidInfo.Data1);
    dbg_hex(p, " expected ", d->dwExpectedSize);
    dbg_puts(p, "\n");
    if (guid_eq(&d->guidInfo, &guid_ntcallbacks)) {
        DD_NTCALLBACKS nt;
        ULONG n = sizeof(nt), i;
        for (i = 0; i < sizeof(nt) / 4; i++) ((ULONG *)&nt)[i] = 0;
        nt.dwSize = sizeof(nt);
        nt.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE | DDHAL_NTCB32_FLIPTOGDISURFACE;
        nt.SetExclusiveMode = DdSetExclusiveMode;
        nt.FlipToGDISurface = DdFlipToGDISurface;
        if (n > d->dwExpectedSize) n = d->dwExpectedSize;
        for (i = 0; i < n; i++) ((UCHAR *)d->lpvData)[i] = ((UCHAR *)&nt)[i];
        d->dwActualSize = sizeof(nt);
        d->ddRVal = DD_OK;
    } else {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    }
    return DDHAL_DRIVER_HANDLED;
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *pHalInfo, DWORD *pdwNumHeaps,
                                   VIDEOMEMORY *pvmList, DWORD *pdwNumFourCCCodes, DWORD *pdwFourCC)
{
    PPDEV p = (PPDEV)dhpdev;
    ULONG i, start = heap_start(p);

    dbg_hex(p, "d3dptdisp: dd info call, lists ", pvmList ? 1 : 0);
    dbg_hex(p, " halinfo ", sizeof(*pHalInfo));
    dbg_hex(p, " corecaps ", sizeof(DDNTCORECAPS));
    dbg_hex(p, " cb ", sizeof(DD_CALLBACKS));
    dbg_hex(p, " scb ", sizeof(DD_SURFACECALLBACKS));
    dbg_puts(p, "\n");
    if (!p->fb || start >= p->fb_len) {
        return FALSE;
    }
    if (!p->device_surface && !(ddflags(p) & DDF_ENGINE_BITMAP)) {
        return FALSE;
    }
    *pdwNumHeaps = 1;
    *pdwNumFourCCCodes = 0;

    for (i = 0; i < sizeof(*pHalInfo) / 4; i++) ((ULONG *)pHalInfo)[i] = 0;
    pHalInfo->dwSize = sizeof(*pHalInfo);
    pHalInfo->vmiData.fpPrimary = 0;
    pHalInfo->vmiData.dwDisplayWidth = p->w;
    pHalInfo->vmiData.dwDisplayHeight = p->h;
    pHalInfo->vmiData.lDisplayPitch = (LONG)p->pitch;
    pHalInfo->vmiData.ddpfDisplay.dwSize = sizeof(DDPIXELFORMAT);
    pHalInfo->vmiData.ddpfDisplay.dwFlags = DDPF_RGB;
    pHalInfo->vmiData.ddpfDisplay.dwRGBBitCount = p->bpp;
    pHalInfo->vmiData.ddpfDisplay.dwRBitMask = p->rmask;
    pHalInfo->vmiData.ddpfDisplay.dwGBitMask = p->gmask;
    pHalInfo->vmiData.ddpfDisplay.dwBBitMask = p->bmask;
    pHalInfo->vmiData.dwOffscreenAlign = 32;
    pHalInfo->vmiData.dwOverlayAlign = 32;
    pHalInfo->vmiData.dwTextureAlign = 32;
    pHalInfo->vmiData.dwZBufferAlign = 32;
    pHalInfo->vmiData.dwAlphaAlign = 32;
    pHalInfo->vmiData.pvPrimary = p->fb;

    pHalInfo->ddCaps.dwSize = sizeof(DDNTCORECAPS);
    /* The caps dxg accepts (2026-09-04 bisection, doc 15): no blit caps
     * (DirectDraw's HEL blits on the mapped VRAM), wide surfaces, primary,
     * offscreen and flip chains. DDCAPS_GDI in dwCaps makes dxg drop the
     * HAL altogether (NOHARDWARE, system-memory surfaces). */
    pHalInfo->ddCaps.dwCaps = 0;
    pHalInfo->ddCaps.dwCaps2 = DDCAPS2_WIDESURFACES;
    pHalInfo->ddCaps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN |
                                       DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER;
    if (ddflags(p) & DDF_GDI_CAP) {
        pHalInfo->ddCaps.dwCaps |= DDCAPS_GDI;
    }
    pHalInfo->ddCaps.dwVidMemTotal = p->fb_len - start;
    pHalInfo->ddCaps.dwVidMemFree = p->fb_len - start;
    if (!(ddflags(p) & DDF_NO_GETDRIVERINFO)) {
        pHalInfo->GetDriverInfo = DdGetDriverInfo;
        pHalInfo->dwFlags = DDHALINFO_GETDRIVERINFOSET;
    }

    if (pvmList) {
        for (i = 0; i < sizeof(*pvmList) / 4; i++) ((ULONG *)pvmList)[i] = 0;
        pvmList->dwFlags = VIDMEM_ISLINEAR;
        pvmList->fpStart = start;
        pvmList->fpEnd = p->fb_len - 1;
        pvmList->ddsCaps.dwCaps = 0;             /* any surface type */
        pvmList->ddsCapsAlt.dwCaps = 0;
        dbg_hex(p, "d3dptdisp: dd heap ", start);
        dbg_hex(p, "..", p->fb_len - 1);
        dbg_puts(p, "\n");
    }
    return TRUE;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb)
{
    ULONG i;

    for (i = 0; i < sizeof(*cb) / 4; i++) ((ULONG *)cb)[i] = 0;
    for (i = 0; i < sizeof(*scb) / 4; i++) ((ULONG *)scb)[i] = 0;
    for (i = 0; i < sizeof(*pcb) / 4; i++) ((ULONG *)pcb)[i] = 0;
    cb->dwSize = sizeof(*cb);
    cb->dwFlags = DDHAL_CB32_WAITFORVERTICALBLANK | DDHAL_CB32_MAPMEMORY | DDHAL_CB32_CANCREATESURFACE;
    cb->WaitForVerticalBlank = DdWaitForVerticalBlank;
    cb->MapMemory = DdMapMemory;
    cb->CanCreateSurface = DdCanCreateSurface;
    scb->dwSize = sizeof(*scb);
    if (ddflags((PPDEV)dhpdev) & DDF_NO_SURFACE_CB) {
        cb->dwFlags = DDHAL_CB32_MAPMEMORY | DDHAL_CB32_CANCREATESURFACE;
    } else {
        scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETBLTSTATUS | DDHAL_SURFCB32_GETFLIPSTATUS;
        scb->Flip = DdFlip;
        scb->GetBltStatus = DdGetBltStatus;
        scb->GetFlipStatus = DdGetFlipStatus;
    }
    pcb->dwSize = sizeof(*pcb);
    dbg_puts((PPDEV)dhpdev, "d3dptdisp: dd enabled\n");
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    dbg_puts((PPDEV)dhpdev, "d3dptdisp: dd disabled\n");
}

/* -------------------------------------------------------------- entry */

static DRVFN drv_fn[] = {
    { INDEX_DrvEnablePDEV,     (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,   (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,    (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,  (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,     (PFN)DrvAssertMode },
    { INDEX_DrvGetModes,       (PFN)DrvGetModes },
    { INDEX_DrvSynchronizeSurface, (PFN)DrvSynchronizeSurface },
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw,  (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw },
};

BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj, DRVENABLEDATA *pded)
{
    if (iEngineVersion < DDI_DRIVER_VERSION_NT5 || cj < sizeof(DRVENABLEDATA)) {
        return FALSE;
    }
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5_01;
    pded->c = sizeof(drv_fn) / sizeof(drv_fn[0]);
    pded->pdrvfn = drv_fn;
    return TRUE;
}

VOID APIENTRY DrvDisableDriver(VOID)
{
}
