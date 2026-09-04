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
 * GDI's too (no DrvSetPointerShape). Nothing here is accelerated on
 * purpose: this step buys the mode table and the kernel workflow; M7b
 * adds the DirectDraw DDI, M7c Direct3D.
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
    hsurf = (HSURF)EngCreateBitmap(sizl, p->pitch, p->bpp == 32 ? BMF_32BPP : BMF_16BPP,
                                   BMF_TOPDOWN | BMF_NOZEROINIT, p->fb);
    if (!hsurf) {
        dbg_puts(p, "d3dptdisp: EngCreateBitmap failed\n");
        goto unmap;
    }
    /* no hooks: GDI draws into the frame buffer itself */
    if (!EngAssociateSurface(hsurf, p->hdev, 0)) {
        dbg_puts(p, "d3dptdisp: EngAssociateSurface failed\n");
        EngDeleteSurface(hsurf);
        goto unmap;
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

/* -------------------------------------------------------------- entry */

static DRVFN drv_fn[] = {
    { INDEX_DrvEnablePDEV,     (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,   (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,    (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,  (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,     (PFN)DrvAssertMode },
    { INDEX_DrvGetModes,       (PFN)DrvGetModes },
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
