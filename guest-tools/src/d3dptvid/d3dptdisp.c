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
 * mapped VRAM.
 *
 * M7c, the Direct3D DDI (after the DirectDraw section): a DX7 non-T&L HAL.
 * Every surface dxg creates is registered with the host by its VRAM
 * offset (DdCreateSurfaceEx); a context is a render target + Z pair
 * (D3dContextCreate); D3dDrawPrimitives2 copies the runtime's DP2 token
 * stream and vertex buffer into the device's command window (top 64 MiB
 * of VRAM, d3dpt_proto.h layout, encoder d3dpt_enc.h) and rings the
 * DOORBELL register; the host interprets the tokens on DXVK and, at
 * EndScene / Lock / Flip, writes the rendered frame back into the render
 * target's VRAM (READBACK), so flips and HEL blits see it.
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
#include <d3dnthal.h>
#include "../../../d3dpt/d3dpt_fb.h"
#include "../../../d3dpt/d3dpt_enc.h"

#define ALLOC_TAG 0x64336d64   /* 'dm3d' */

/* -device d3dpt-vga,ddflags=N: bisection knob while the DDI is brought up */
#define DDF_NO_GETDRIVERINFO   0x1    /* no GetDriverInfo / GETDRIVERINFOSET */
#define DDF_NO_SURFACE_CB      0x4    /* only MapMemory + CanCreateSurface */
#define DDF_ENGINE_BITMAP      0x8    /* EngCreateBitmap primary instead of a device surface */
#define DDF_GDI_CAP            0x10   /* add DDCAPS_GDI to dwCaps: dxg then drops the HAL (kept as the repro) */
#define DDF_NO_D3D             0x20   /* no Direct3D: DirectDraw HAL only, as in M7b */
#define DDF_NO_3D_CAP          0x40   /* bisection: Direct3D data without DDCAPS_3D / the 3D ddsCaps */
#define DDF_NO_D3D_INFO        0x80   /* bisection: no D3D answers from GetDriverInfo */
#define DDF_NO_D3D_BUF         0x100  /* bisection: no D3D buffer callbacks */
#define DDF_NO_D3D_CB3         0x200  /* bisection: no GUID_D3DCallbacks3 answer */
#define DDF_NO_MISC2           0x400  /* bisection: no GUID_Miscellaneous2Callbacks answer */
#define DDF_NO_PARSEUNKNOWN    0x800  /* bisection: refuse GUID_D3DParseUnknownCommandCallback */

#define D3D_MAX_CTX 16

typedef struct _D3DCTX {
    ULONG pid;
    ULONG rt, z;                /* VRAM surface handles */
    BOOL used;
} D3DCTX;

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
    ULONG pal[256];             /* 8 bpp: GDI's default palette (PALETTEENTRY form) */

    /* M7c: the command window and the Direct3D state */
    ULONG cmd_offset;           /* window offset in VRAM (0 = the device has none) */
    BOOL d3d;                   /* window mapped and the host executor answered */
    d3dpt_enc enc;
    D3DCTX ctx[D3D_MAX_CTX];
    ULONG ctx_live;
    ULONG dp2_calls, dp2_errors, reg_lines;
} PDEV, *PPDEV;

static PPDEV d3d_pdev;              /* the PDEV whose Direct3D is on (the primary display) */

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

static ULONG bmf_of(PPDEV p)
{
    return p->bpp == 32 ? BMF_32BPP : p->bpp == 16 ? BMF_16BPP : BMF_8BPP;
}

/* the 20 Windows system colours (PALETTEENTRY form: red in the low byte) */
static const ULONG base_colors[20] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xc0c0c0,
    0xc0dcc0, 0xf0caa6,
    0xf0fbff, 0xa4a0a0, 0x808080, 0x0000ff, 0x00ff00, 0x00ffff, 0xff0000, 0xff00ff,
    0xffff00, 0xffffff,
};

/* GDI's default 8 bpp palette: system colours at 0-9 and 246-255, a 6x6x6
 * cube at 10-225, 20 greys at 226-245 */
static void build_palette(ULONG *pal)
{
    ULONG i, r, g, b;

    for (i = 0; i < 10; i++) {
        pal[i] = base_colors[i];
        pal[246 + i] = base_colors[10 + i];
    }
    i = 10;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                pal[i++] = (r * 51) | ((g * 51) << 8) | ((b * 51) << 16);
            }
        }
    }
    for (; i < 246; i++) {
        ULONG v = (i - 226) * 255 / 19;
        pal[i] = v | (v << 8) | (v << 16);
    }
}

/* n PALETTEENTRY-form colours into the device's PALETTE registers from
 * entry start (the host applies them at its next refresh); through the
 * miniport's IOCTL while the register page is not mapped yet */
static void set_clut(PPDEV p, ULONG start, ULONG n, const ULONG *rgb)
{
    ULONG i;

    if (start >= 256) {
        return;
    }
    if (n > 256 - start) {
        n = 256 - start;
    }
    if (p->regs) {
        for (i = 0; i < n; i++) {
            ULONG c = rgb[i];
            p->regs[(D3DPT_FB_REG_PALETTE + 4 * (start + i)) / 4] =
                ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
        }
    } else {
        UCHAR buf[4 + 256 * 4];
        PVIDEO_CLUT clut = (PVIDEO_CLUT)buf;
        DWORD ret;
        clut->NumEntries = (USHORT)n;
        clut->FirstEntry = (USHORT)start;
        for (i = 0; i < n; i++) {
            ULONG c = rgb[i];
            clut->LookupTable[i].RgbArray.Red = (UCHAR)c;
            clut->LookupTable[i].RgbArray.Green = (UCHAR)(c >> 8);
            clut->LookupTable[i].RgbArray.Blue = (UCHAR)(c >> 16);
            clut->LookupTable[i].RgbArray.Unused = 0;
        }
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SET_COLOR_REGISTERS, clut, 4 + n * 4,
                           NULL, 0, &ret);
    }
}

/* palette-managed 8 bpp: GDI's system palette, at mode set and whenever a
 * palette is realized */
BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, FLONG fl, ULONG iStart, ULONG cColors)
{
    PPDEV p = (PPDEV)dhpdev;
    ULONG colors[256];

    if (p->bpp != 8 || iStart >= 256) {
        return FALSE;
    }
    if (cColors > 256 - iStart) {
        cColors = 256 - iStart;
    }
    if (PALOBJ_cGetColors(ppalo, iStart, cColors, colors) != cColors) {
        return FALSE;
    }
    set_clut(p, iStart, cColors, colors);
    return TRUE;
}

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
    g.ulNumColors = p->bpp == 8 ? 20 : (ULONG)-1;
    g.ulVRefresh = p->hz;
    g.ulBltAlignment = 1;
    g.ulLogPixelsX = pdm->dmLogPixels ? pdm->dmLogPixels : 96;
    g.ulLogPixelsY = g.ulLogPixelsX;
    g.flTextCaps = TC_RA_ABLE;
    if (p->bpp == 32) {
        g.ulDACRed = g.ulDACGreen = g.ulDACBlue = 8;
        g.ulHTOutputFormat = HT_FORMAT_32BPP;
    } else if (p->bpp == 16) {
        g.ulDACRed = 5; g.ulDACGreen = 6; g.ulDACBlue = 5;
        g.ulHTOutputFormat = HT_FORMAT_16BPP;
    } else {
        g.ulDACRed = g.ulDACGreen = g.ulDACBlue = 8;
        g.ulHTOutputFormat = HT_FORMAT_8BPP;
    }
    g.ulAspectX = 36;
    g.ulAspectY = 36;
    g.ulAspectXY = 51;
    g.xStyleStep = 1;
    g.yStyleStep = 1;
    g.denStyleStep = 3;
    g.ulNumPalReg = p->bpp == 8 ? 256 : 0;
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
    d.iDitherFormat = bmf_of(p);
    d.cxDither = 0;
    d.cyDither = 0;
    if (p->bpp == 8) {
        /* palette-managed: GDI owns the 256 entries and hands them to
         * DrvSetPalette; the default palette has the 20 system colours
         * where GDI expects them */
        build_palette(p->pal);
        d.flGraphicsCaps = GCAPS_PALMANAGED | GCAPS_COLOR_DITHER;
        d.cxDither = d.cyDither = 8;
        d.hpalDefault = EngCreatePalette(PAL_INDEXED, 256, p->pal, 0, 0, 0);
    } else {
        d.hpalDefault = EngCreatePalette(PAL_BITFIELDS, 0, NULL, p->rmask, p->gmask, p->bmask);
    }
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
    p->cmd_offset = (p->regs && (p->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_D3D)) ?
                    p->regs[D3DPT_FB_REG_CMD_OFFSET / 4] : 0;

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
    if (p->bpp == 8) {
        set_clut(p, 0, 256, p->pal);
    }

    sizl.cx = p->w;
    sizl.cy = p->h;
    /* A device surface (DirectDraw needs one) that GDI still draws on
     * itself: EngModifySurface hands it the frame buffer bytes. The hook /
     * flag combinations win32k accepts are not documented consistently, so
     * try them in order and say which one took; the engine bitmap of M7a
     * is the last resort (desktop works, no DirectDraw). */
    hsurf = (p->regs && (p->regs[D3DPT_FB_REG_DDFLAGS / 4] & DDF_ENGINE_BITMAP)) ? NULL :
            EngCreateDeviceSurface((DHSURF)p, sizl, bmf_of(p));
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
        hsurf = (HSURF)EngCreateBitmap(sizl, p->pitch, bmf_of(p),
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
    if (p->d3d) {
        p->d3d = FALSE;
        if (d3d_pdev == p) {
            d3d_pdev = NULL;
        }
    }
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

/* the Direct3D section below */
static BOOL d3d_init(PPDEV p);
static ULONG pf_format(const DDPIXELFORMAT *f);
static void d3d_register_at(PPDEV p, PDD_SURFACE_LOCAL s, ULONG offset);
static HRESULT d3d_readback(PPDEV p, PDD_SURFACE_LOCAL s);
static DWORD APIENTRY DdCreateSurfaceEx(PDD_CREATESURFACEEXDATA d);
static DWORD APIENTRY DdGetDriverState(PDD_GETDRIVERSTATEDATA d);
static DWORD APIENTRY DdDestroySurface(PDD_DESTROYSURFACEDATA d);
static DWORD APIENTRY DdLock(PDD_LOCKDATA d);
static DWORD APIENTRY DdUnlock(PDD_UNLOCKDATA d);
static DWORD APIENTRY D3dClear2(LPD3DNTHAL_CLEAR2DATA d);
static DWORD APIENTRY D3dValidateTextureStageState(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA d);
static DWORD APIENTRY D3dDrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA d);
static DWORD APIENTRY D3dSetRenderTarget(LPD3DNTHAL_SETRENDERTARGETDATA d);
static DWORD APIENTRY D3dCanCreateD3DBuffer(PDD_CANCREATESURFACEDATA d);
static DWORD APIENTRY D3dCreateD3DBuffer(PDD_CREATESURFACEDATA d);
static DWORD APIENTRY D3dDestroyD3DBuffer(PDD_DESTROYSURFACEDATA d);
static DWORD APIENTRY D3dLockD3DBuffer(PDD_LOCKDATA d);
static DWORD APIENTRY D3dUnlockD3DBuffer(PDD_UNLOCKDATA d);
static D3DNTHAL_GLOBALDRIVERDATA d3d_global;
static D3DNTHAL_CALLBACKS d3d_callbacks;
static DD_D3DBUFCALLBACKS d3d_bufcallbacks;
static DWORD APIENTRY D3dContextCreate(LPD3DNTHAL_CONTEXTCREATEDATA d);
static DWORD APIENTRY D3dContextDestroy(LPD3DNTHAL_CONTEXTDESTROYDATA d);
static DWORD APIENTRY D3dContextDestroyAll(LPD3DNTHAL_CONTEXTDESTROYALLDATA d);
static DWORD APIENTRY D3dSceneCapture(LPD3DNTHAL_SCENECAPTUREDATA d);
static D3DNTHAL_D3DEXTENDEDCAPS d3d_extcaps;
static struct { DWORD count; DDPIXELFORMAT pf[3]; } d3d_zformats;

static ULONG heap_start(PPDEV p)
{
    return (p->pitch * p->h + 4095) & ~4095u;
}

/* the DirectDraw heap ends where the command window starts */
static ULONG heap_end(PPDEV p)
{
    return p->cmd_offset ? p->cmd_offset : p->fb_len;
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
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    /* the display format always; with Direct3D also the texture and Z
     * formats the host mirrors (pf_format) */
    if (!d->bIsDifferentPixelFormat) {
        d->ddRVal = DD_OK;
    } else if (p->d3d && pf_format(&d->lpDDSurfaceDesc->ddpfPixelFormat) != 0) {
        d->ddRVal = DD_OK;
    } else {
        d->ddRVal = DDERR_INVALIDPIXELFORMAT;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlip(PDD_FLIPDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (!p->regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    if (p->ctx_live) {
        /* what Direct3D rendered into the back buffer must be in its VRAM
         * before it is scanned out; afterwards dxg exchanges the two
         * surfaces' VRAM, so the host mirror follows */
        d3d_readback(p, d->lpSurfTarg);
    }
    /* the page flip: scan out from the target's VRAM offset */
    p->regs[D3DPT_FB_REG_OFFSET / 4] = (ULONG)d->lpSurfTarg->lpGbl->fpVidMem;
    if (p->ctx_live) {
        d3d_register_at(p, d->lpSurfTarg, (ULONG)d->lpSurfCurr->lpGbl->fpVidMem);
        d3d_register_at(p, d->lpSurfCurr, (ULONG)d->lpSurfTarg->lpGbl->fpVidMem);
    }
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

static const GUID guid_d3dcallbacks2 = {
    0x0ba584e1, 0x70b6, 0x11d0, { 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a }
};
static const GUID guid_d3dcallbacks3 = {
    0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e }
};
static const GUID guid_d3dextendedcaps = {
    0x7de41f80, 0x9d93, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 }
};
static const GUID guid_zpixelformats = {
    0x93869880, 0x36cf, 0x11d1, { 0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae }
};
static const GUID guid_misc2callbacks = {
    0x406b2f00, 0x3e5a, 0x11d1, { 0xb6, 0x40, 0x00, 0xaa, 0x00, 0xa1, 0xf9, 0x6a }
};
static const GUID guid_parseunknown = {
    0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 }
};

static void info_copy(PDD_GETDRIVERINFODATA d, const void *src, ULONG n)
{
    ULONG i;
    if (n > d->dwExpectedSize) n = d->dwExpectedSize;
    for (i = 0; i < n; i++) ((UCHAR *)d->lpvData)[i] = ((const UCHAR *)src)[i];
    d->dwActualSize = n;
    d->ddRVal = DD_OK;
}

static DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA d)
{
    PPDEV p = (PPDEV)d->dhpdev;

    dbg_hex(p, "d3dptdisp: dd getinfo ", d->guidInfo.Data1);
    dbg_hex(p, " expected ", d->dwExpectedSize);
    dbg_puts(p, "\n");
    if (guid_eq(&d->guidInfo, &guid_ntcallbacks)) {
        DD_NTCALLBACKS nt;
        ULONG i;
        for (i = 0; i < sizeof(nt) / 4; i++) ((ULONG *)&nt)[i] = 0;
        nt.dwSize = sizeof(nt);
        nt.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE | DDHAL_NTCB32_FLIPTOGDISURFACE;
        nt.SetExclusiveMode = DdSetExclusiveMode;
        nt.FlipToGDISurface = DdFlipToGDISurface;
        info_copy(d, &nt, sizeof(nt));
    } else if (p->d3d && (ddflags(p) & DDF_NO_D3D_INFO)) {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    } else if (p->d3d && !(ddflags(p) & DDF_NO_D3D_CB3) && guid_eq(&d->guidInfo, &guid_d3dcallbacks3)) {
        D3DNTHAL_CALLBACKS3 cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DNTHAL3_CB32_CLEAR2 | D3DNTHAL3_CB32_VALIDATETEXTURESTAGESTATE | D3DNTHAL3_CB32_DRAWPRIMITIVES2;
        cb.Clear2 = D3dClear2;
        cb.ValidateTextureStageState = D3dValidateTextureStageState;
        cb.DrawPrimitives2 = D3dDrawPrimitives2;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->d3d && guid_eq(&d->guidInfo, &guid_d3dcallbacks2)) {
        D3DNTHAL_CALLBACKS2 cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DNTHAL2_CB32_SETRENDERTARGET;
        cb.SetRenderTarget = D3dSetRenderTarget;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->d3d && guid_eq(&d->guidInfo, &guid_d3dextendedcaps)) {
        info_copy(d, &d3d_extcaps, sizeof(d3d_extcaps));
    } else if (p->d3d && guid_eq(&d->guidInfo, &guid_zpixelformats)) {
        info_copy(d, &d3d_zformats, sizeof(d3d_zformats));
    } else if (p->d3d && !(ddflags(p) & DDF_NO_MISC2) && guid_eq(&d->guidInfo, &guid_misc2callbacks)) {
        DD_MISCELLANEOUS2CALLBACKS cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        /* GetDriverState is not optional: ddraw.dll drops the whole HAL
         * (DDCAPS_NOHARDWARE) when a device with DRAWPRIMITIVES2EX or T&L
         * caps answers Miscellaneous2Callbacks without it (2026-09-04
         * bisection + ddraw disassembly) */
        cb.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE;
        cb.CreateSurfaceEx = DdCreateSurfaceEx;
        cb.GetDriverState = DdGetDriverState;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->d3d && !(ddflags(p) & DDF_NO_PARSEUNKNOWN) && guid_eq(&d->guidInfo, &guid_parseunknown)) {
        /* the runtime hands us its parser for driver-private tokens; we emit none */
        d->dwActualSize = d->dwExpectedSize;
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
    if (!p->fb || start >= heap_end(p)) {
        return FALSE;
    }
    if (!p->device_surface && !(ddflags(p) & DDF_ENGINE_BITMAP)) {
        return FALSE;
    }
    if (p->cmd_offset + D3DPT_SHM_SIZE > p->fb_len) {
        p->cmd_offset = 0;
    }
    d3d_init(p);
    *pdwNumHeaps = 1;
    *pdwNumFourCCCodes = 0;

    for (i = 0; i < sizeof(*pHalInfo) / 4; i++) ((ULONG *)pHalInfo)[i] = 0;
    pHalInfo->dwSize = sizeof(*pHalInfo);
    pHalInfo->vmiData.fpPrimary = 0;
    pHalInfo->vmiData.dwDisplayWidth = p->w;
    pHalInfo->vmiData.dwDisplayHeight = p->h;
    pHalInfo->vmiData.lDisplayPitch = (LONG)p->pitch;
    pHalInfo->vmiData.ddpfDisplay.dwSize = sizeof(DDPIXELFORMAT);
    pHalInfo->vmiData.ddpfDisplay.dwFlags = p->bpp == 8 ? DDPF_RGB | DDPF_PALETTEINDEXED8 : DDPF_RGB;
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
    pHalInfo->ddCaps.dwVidMemTotal = heap_end(p) - start;
    pHalInfo->ddCaps.dwVidMemFree = heap_end(p) - start;
    /* dwPalCaps stays 0 at 8 bpp too: XP's dxg.sys drops the whole HAL when
     * a driver reports palette caps (its post-enable validation, next to the
     * DDCAPS_GDI check; 2026-09-04 disassembly). On NT the primary's palette
     * is GDI's: SetPalette / SetEntries reach DrvSetPalette. */
    if (!(ddflags(p) & DDF_NO_GETDRIVERINFO)) {
        pHalInfo->GetDriverInfo = DdGetDriverInfo;
        pHalInfo->dwFlags = DDHALINFO_GETDRIVERINFOSET;
    }
    if (p->d3d) {
        /* the Direct3D HAL: caps here, the callbacks through GetDriverInfo */
        if (!(ddflags(p) & DDF_NO_3D_CAP)) {
            pHalInfo->ddCaps.dwCaps |= DDCAPS_3D;
            pHalInfo->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
            pHalInfo->ddCaps.dwZBufferBitDepths = DDBD_16_ | DDBD_24_ | DDBD_32_;
        }
        pHalInfo->lpD3DGlobalDriverData = &d3d_global;
        pHalInfo->lpD3DHALCallbacks = &d3d_callbacks;
        if (!(ddflags(p) & DDF_NO_D3D_BUF)) {
            pHalInfo->lpD3DBufCallbacks = &d3d_bufcallbacks;
        }
        dbg_puts(p, "d3dptdisp: d3d caps offered\n");
    }

    if (pvmList) {
        for (i = 0; i < sizeof(*pvmList) / 4; i++) ((ULONG *)pvmList)[i] = 0;
        pvmList->dwFlags = VIDMEM_ISLINEAR;
        pvmList->fpStart = start;
        pvmList->fpEnd = heap_end(p) - 1;
        pvmList->ddsCaps.dwCaps = 0;             /* any surface type */
        pvmList->ddsCapsAlt.dwCaps = 0;
        dbg_hex(p, "d3dptdisp: dd heap ", start);
        dbg_hex(p, "..", heap_end(p) - 1);
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
        if (((PPDEV)dhpdev)->d3d) {
            /* Direct3D: surfaces come and go (host mirror), Lock reads back a
             * rendered target, Unlock marks texels the guest wrote */
            scb->dwFlags |= DDHAL_SURFCB32_DESTROYSURFACE | DDHAL_SURFCB32_LOCK | DDHAL_SURFCB32_UNLOCK;
            scb->DestroySurface = DdDestroySurface;
            scb->Lock = DdLock;
            scb->Unlock = DdUnlock;
        }
    }
    pcb->dwSize = sizeof(*pcb);         /* no palette callbacks: see dwPalCaps above */
    dbg_puts((PPDEV)dhpdev, "d3dptdisp: dd enabled\n");
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    dbg_puts((PPDEV)dhpdev, "d3dptdisp: dd disabled\n");
}


/* -------------------------------------------------------------- Direct3D
 * The DX7 HAL (doc 15, M7c). dxg.sys calls these with its device lock
 * held, so one encoder per PDEV is enough. Surfaces are dxg's, in VRAM;
 * the host mirrors the ones Direct3D touches by their VRAM offset. */

#define D3DFMT_X8R8G8B8_  22u
#define D3DFMT_A8R8G8B8_  21u
#define D3DFMT_R5G6B5_    23u
#define D3DFMT_X1R5G5B5_  24u
#define D3DFMT_A1R5G5B5_  25u
#define D3DFMT_A4R4G4B4_  26u
#define D3DFMT_X4R4G4B4_  30u
#define D3DFMT_D16_       80u
#define D3DFMT_D24X8_     77u
#define D3DFMT_D24S8_     75u
#define D3DFMT_D15S1_     73u
#define D3DFMT_D32_       71u
#define FOURCC_(a, b, c, d) ((ULONG)(UCHAR)(a) | ((ULONG)(UCHAR)(b) << 8) | ((ULONG)(UCHAR)(c) << 16) | ((ULONG)(UCHAR)(d) << 24))

#ifndef DDRAWISURF_HASPIXELFORMAT
#define DDRAWISURF_HASPIXELFORMAT 0x00002000
#endif

/* the pixel format of a DirectDraw surface as a D3DFORMAT the host knows; 0 = not mirrored */
static ULONG pf_format(const DDPIXELFORMAT *f)
{
    if (f->dwFlags & DDPF_FOURCC) {
        ULONG cc = f->dwFourCC;
        if (cc == FOURCC_('D', 'X', 'T', '1') || cc == FOURCC_('D', 'X', 'T', '2') || cc == FOURCC_('D', 'X', 'T', '3') ||
            cc == FOURCC_('D', 'X', 'T', '4') || cc == FOURCC_('D', 'X', 'T', '5')) {
            return cc;
        }
        return 0;
    }
    if (f->dwFlags & DDPF_ZBUFFER) {
        ULONG bits = f->dwZBufferBitDepth, st = (f->dwFlags & DDPF_STENCILBUFFER) ? f->dwStencilBitDepth : 0;
        if (bits == 16) return st ? D3DFMT_D15S1_ : D3DFMT_D16_;
        if (bits == 32 || bits == 24) return st ? D3DFMT_D24S8_ : D3DFMT_D24X8_;
        return 0;
    }
    if (f->dwFlags & DDPF_RGB) {
        BOOL alpha = (f->dwFlags & DDPF_ALPHAPIXELS) && f->dwRGBAlphaBitMask;
        if (f->dwRGBBitCount == 32 && f->dwRBitMask == 0x00ff0000) return alpha ? D3DFMT_A8R8G8B8_ : D3DFMT_X8R8G8B8_;
        if (f->dwRGBBitCount == 16) {
            if (f->dwRBitMask == 0xf800) return D3DFMT_R5G6B5_;
            if (f->dwRBitMask == 0x7c00) return alpha ? D3DFMT_A1R5G5B5_ : D3DFMT_X1R5G5B5_;
            if (f->dwRBitMask == 0x0f00) return alpha ? D3DFMT_A4R4G4B4_ : D3DFMT_X4R4G4B4_;
        }
    }
    return 0;
}

static void pf_rgb(DDPIXELFORMAT *f, ULONG bits, ULONG r, ULONG g, ULONG b, ULONG a)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    f->dwRGBBitCount = bits;
    f->dwRBitMask = r; f->dwGBitMask = g; f->dwBBitMask = b; f->dwRGBAlphaBitMask = a;
}

static void pf_fourcc(DDPIXELFORMAT *f, ULONG cc)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_FOURCC;
    f->dwFourCC = cc;
}

static void pf_z(DDPIXELFORMAT *f, ULONG bits, ULONG stencil)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_ZBUFFER | (stencil ? DDPF_STENCILBUFFER : 0);
    f->dwZBufferBitDepth = bits;
    f->dwStencilBitDepth = stencil;
    f->dwZBitMask = bits == 16 ? 0xffff : stencil ? 0xffffff00 : 0xffffff;
    f->dwStencilBitMask = stencil ? 0xff : 0;
}

static DDSURFACEDESC d3d_texformats[9];

static void d3d_caps_init(void)
{
    D3DNTHAL_GLOBALDRIVERDATA *g = &d3d_global;
    D3DNTHALDEVICEDESC_V1 *c = &g->hwCaps;
    D3DPRIMCAPS_ *t = &c->dpcTriCaps;
    D3DNTHAL_D3DEXTENDEDCAPS *e = &d3d_extcaps;
    ULONG i;

    for (i = 0; i < sizeof(*g) / 4; i++) ((ULONG *)g)[i] = 0;
    for (i = 0; i < sizeof(d3d_callbacks) / 4; i++) ((ULONG *)&d3d_callbacks)[i] = 0;
    for (i = 0; i < sizeof(*e) / 4; i++) ((ULONG *)e)[i] = 0;
    for (i = 0; i < sizeof(d3d_texformats) / 4; i++) ((ULONG *)d3d_texformats)[i] = 0;

    g->dwSize = sizeof(*g);
    c->dwSize = sizeof(*c);
    c->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                 D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                 D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    c->dcmColorModel = D3DCOLOR_RGB_;
    /* a rasterizer: the runtime transforms and lights (no HWTRANSFORMANDLIGHT yet) */
    c->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION;
    c->dtcTransformCaps.dwSize = sizeof(c->dtcTransformCaps);
    c->bClipping = FALSE;
    c->dlcLightingCaps.dwSize = sizeof(c->dlcLightingCaps);
    c->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    t->dwSize = sizeof(*t);
    t->dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    t->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ZFOG;
    t->dwZCmpCaps = D3DPCMPCAPS_ALL;
    t->dwSrcBlendCaps = D3DPBLENDCAPS_ALL;
    t->dwDestBlendCaps = D3DPBLENDCAPS_ALL;
    t->dwAlphaCmpCaps = D3DPCMPCAPS_ALL;
    t->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB |
                     D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                     D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    t->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED;
    t->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                             D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
                             D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                             D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    t->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    t->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    c->dpcLineCaps = *t;
    c->dwDeviceRenderBitDepth = DDBD_16_ | DDBD_32_;
    c->dwDeviceZBufferBitDepth = DDBD_16_ | DDBD_24_ | DDBD_32_;
    c->dwMaxBufferSize = 0;
    c->dwMaxVertexCount = 65535;

    /* texture formats: what the host reads straight from VRAM */
    pf_rgb(&d3d_texformats[0].ddpfPixelFormat, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    pf_rgb(&d3d_texformats[1].ddpfPixelFormat, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    pf_rgb(&d3d_texformats[2].ddpfPixelFormat, 16, 0xf800, 0x07e0, 0x001f, 0);
    pf_rgb(&d3d_texformats[3].ddpfPixelFormat, 16, 0x7c00, 0x03e0, 0x001f, 0);
    pf_rgb(&d3d_texformats[4].ddpfPixelFormat, 16, 0x7c00, 0x03e0, 0x001f, 0x8000);
    pf_rgb(&d3d_texformats[5].ddpfPixelFormat, 16, 0x0f00, 0x00f0, 0x000f, 0xf000);
    pf_fourcc(&d3d_texformats[6].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '1'));
    pf_fourcc(&d3d_texformats[7].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '3'));
    pf_fourcc(&d3d_texformats[8].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '5'));
    for (i = 0; i < 9; i++) {
        d3d_texformats[i].dwSize = sizeof(DDSURFACEDESC);
        d3d_texformats[i].dwFlags = DDSD_PIXELFORMAT;
    }
    g->dwNumTextureFormats = 9;
    g->lpTextureFormats = d3d_texformats;

    d3d_zformats.count = 3;
    pf_z(&d3d_zformats.pf[0], 16, 0);
    pf_z(&d3d_zformats.pf[1], 32, 0);
    pf_z(&d3d_zformats.pf[2], 32, 8);

    e->dwSize = sizeof(*e);
    e->dwMinTextureWidth = e->dwMinTextureHeight = 1;
    e->dwMaxTextureWidth = e->dwMaxTextureHeight = 4096;
    e->dwMaxTextureRepeat = 8192;
    e->dwMaxAnisotropy = 1;
    e->dwStencilCaps = D3DSTENCILCAPS_ALL;
    e->dwFVFCaps = 8;
    e->dwTextureOpCaps = D3DTEXOPCAPS_ALL;
    e->wMaxTextureBlendStages = 8;
    e->wMaxSimultaneousTextures = 8;
    e->dvMaxVertexW = 1.0e10f;

    d3d_callbacks.dwSize = sizeof(d3d_callbacks);
    d3d_callbacks.ContextCreate = D3dContextCreate;
    d3d_callbacks.ContextDestroy = D3dContextDestroy;
    d3d_callbacks.ContextDestroyAll = D3dContextDestroyAll;
    d3d_callbacks.SceneCapture = D3dSceneCapture;

    /* the command / vertex buffers of DrawPrimitives2: dxg allocates them
     * in system memory once the driver says so (NOTHANDLED + the caps) */
    for (i = 0; i < sizeof(d3d_bufcallbacks) / 4; i++) ((ULONG *)&d3d_bufcallbacks)[i] = 0;
    d3d_bufcallbacks.dwSize = sizeof(d3d_bufcallbacks);
    d3d_bufcallbacks.dwFlags = DDHAL_D3DBUFCB32_CANCREATED3DBUF | DDHAL_D3DBUFCB32_CREATED3DBUF |
                               DDHAL_D3DBUFCB32_DESTROYD3DBUF | DDHAL_D3DBUFCB32_LOCKD3DBUF | DDHAL_D3DBUFCB32_UNLOCKD3DBUF;
    d3d_bufcallbacks.CanCreateD3DBuffer = D3dCanCreateD3DBuffer;
    d3d_bufcallbacks.CreateD3DBuffer = D3dCreateD3DBuffer;
    d3d_bufcallbacks.DestroyD3DBuffer = D3dDestroyD3DBuffer;
    d3d_bufcallbacks.LockD3DBuffer = D3dLockD3DBuffer;
    d3d_bufcallbacks.UnlockD3DBuffer = D3dUnlockD3DBuffer;
}

/* --- D3D buffers (the runtime's command and vertex buffers): system memory, dxg's --- */

static DWORD APIENTRY D3dCanCreateD3DBuffer(PDD_CANCREATESURFACEDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dCreateD3DBuffer(PDD_CREATESURFACEDATA d)
{
    ULONG i;

    for (i = 0; i < d->dwSCnt; i++) {
        d->lplpSList[i]->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
        d->lplpSList[i]->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    }
    d->lpDDSurfaceDesc->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    d->lpDDSurfaceDesc->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dDestroyD3DBuffer(PDD_DESTROYSURFACEDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dLockD3DBuffer(PDD_LOCKDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dUnlockD3DBuffer(PDD_UNLOCKDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --- the command window --- */

static void d3d_doorbell(d3dpt_enc *e)
{
    PPDEV p = (PPDEV)((UCHAR *)e - (ULONG_PTR)&((PPDEV)0)->enc);

    p->regs[D3DPT_FB_REG_DOORBELL / 4] = 1;
    if (d3dpt_enc_hdr(e)->ret_status && p->dp2_errors < 8) {
        p->dp2_errors++;
        dbg_hex(p, "d3dptdisp: batch error ", d3dpt_enc_hdr(e)->ret_status);
        dbg_hex(p, " at record ", d3dpt_enc_hdr(e)->ret_index);
        dbg_puts(p, "\n");
    }
}

static BOOL d3d_init(PPDEV p)
{
    if (p->d3d) {
        return TRUE;
    }
    if (!p->regs || !p->fb || !p->cmd_offset || (ddflags(p) & DDF_NO_D3D)) {
        return FALSE;
    }
    /* offered at 8 bpp too (no DX7 device can be created on a palettized
     * primary, the runtime refuses that itself): ddraw.dll fails a mode
     * switch when the HAL loses its Direct3D between two PDEVs (2026-09-04,
     * DDTEST 640x480x8 -> DDERR_UNSUPPORTEDMODE until this was consistent) */
    if (p->regs[D3DPT_FB_REG_D3D_STATUS / 4] != D3DPT_STATUS_READY) {
        dbg_puts(p, "d3dptdisp: no Direct3D executor on the host\n");
        return FALSE;
    }
    d3d_caps_init();
    d3dpt_enc_init(&p->enc, (uint8_t *)p->fb + p->cmd_offset, d3d_doorbell);
    p->d3d = TRUE;
    d3d_pdev = p;
    dbg_hex(p, "d3dptdisp: d3d window at ", p->cmd_offset);
    dbg_puts(p, "\n");
    return TRUE;
}

/* --- surfaces --- */

static ULONG surf_handle(PDD_SURFACE_LOCAL s)
{
    return (s && s->lpSurfMore) ? s->lpSurfMore->dwSurfaceHandle : 0;
}

static ULONG surf_format(PPDEV p, PDD_SURFACE_LOCAL s)
{
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) {
        return pf_format(&s->lpGbl->ddpfSurface);
    }
    return p->bpp == 32 ? D3DFMT_X8R8G8B8_ : D3DFMT_R5G6B5_;
}

static ULONG surf_caps(PDD_SURFACE_LOCAL s)
{
    ULONG c = s->ddsCaps.dwCaps, r = 0;

    if (c & DDSCAPS_TEXTURE) r |= D3DPT_VS_TEXTURE;
    if (c & DDSCAPS_ZBUFFER) r |= D3DPT_VS_ZBUFFER;
    if (c & DDSCAPS_3DDEVICE) r |= D3DPT_VS_RENDER_TARGET;
    if (c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) r |= D3DPT_VS_PRIMARY;
    return r;
}

/* the next mip level attached to s (smaller, DDSCAPS_MIPMAP), or NULL */
static PDD_SURFACE_LOCAL surf_next_mip(PDD_SURFACE_LOCAL s)
{
    PDD_ATTACHLIST a;

    for (a = s->lpAttachList; a; a = a->lpLink) {
        PDD_SURFACE_LOCAL t = a->lpAttached;
        if (t && t->lpGbl && (t->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && t != s &&
            (t->lpGbl->wWidth < s->lpGbl->wWidth || t->lpGbl->wHeight < s->lpGbl->wHeight)) {
            return t;
        }
    }
    return NULL;
}

/* VRAM_SURFACE for s at the given VRAM offset (its own, or the one a flip hands it) */
static void d3d_register_at(PPDEV p, PDD_SURFACE_LOCAL s, ULONG offset)
{
    d3dpt_vram_surface *r;
    d3dpt_u32x2 lv[15];
    ULONG handle, fmt, caps, n = 1, i;
    PDD_SURFACE_LOCAL m;

    if (!p->d3d || !s || !s->lpGbl) {
        return;
    }
    handle = surf_handle(s);
    fmt = surf_format(p, s);
    caps = surf_caps(s);
    /* one line per surface in the QEMU log: what the host will know it as
     * (a "skipped" surface is one a later SETRENDERTARGET / TEXTUREMAP
     * would report unknown) */
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: surface ", handle);
        dbg_hex(p, " caps ", s->ddsCaps.dwCaps);
        dbg_hex(p, " w ", s->lpGbl->wWidth);
        dbg_hex(p, " h ", s->lpGbl->wHeight);
        dbg_hex(p, " fmt ", fmt);
        dbg_hex(p, " at ", offset);
        if (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) dbg_puts(p, " sysmem, skipped");
        else if (!fmt) dbg_puts(p, " no format, skipped");
        dbg_puts(p, "\n");
    }
    if ((s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) || !handle || !fmt ||
        (ULONGLONG)offset + (ULONGLONG)s->lpGbl->lPitch * s->lpGbl->wHeight > heap_end(p)) {
        return;
    }
    if (caps & D3DPT_VS_TEXTURE) {
        for (m = surf_next_mip(s); m && n < 16; m = surf_next_mip(m)) {
            lv[n - 1].a = (ULONG)m->lpGbl->fpVidMem;
            lv[n - 1].b = (ULONG)m->lpGbl->lPitch;
            n++;
        }
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*r), (n - 1) * sizeof(d3dpt_u32x2));
    if (!r) {
        return;
    }
    r->handle = handle;
    r->offset = offset;
    r->width = s->lpGbl->wWidth;
    r->height = s->lpGbl->wHeight;
    r->pitch = (ULONG)s->lpGbl->lPitch;
    r->format = fmt;
    r->caps = caps;
    r->levels = n;
    for (i = 0; i + 1 < n; i++) ((d3dpt_u32x2 *)(r + 1))[i] = lv[i];
}

static void d3d_register(PPDEV p, PDD_SURFACE_LOCAL s)
{
    d3d_register_at(p, s, (ULONG)s->lpGbl->fpVidMem);
}

static void d3d_handle_op(PPDEV p, ULONG op, ULONG handle)
{
    d3dpt_handle *h;

    if (!p->d3d || !handle) {
        return;
    }
    h = d3dpt_enc_cmd(&p->enc, op, sizeof(*h), 0);
    if (h) {
        h->handle = handle;
        h->pad = 0;
    }
}

/* the host's rendering into s, into its VRAM (S_FALSE = nothing pending) */
static HRESULT d3d_readback(PPDEV p, PDD_SURFACE_LOCAL s)
{
    ULONG handle = surf_handle(s);

    if (!p->d3d || !handle) {
        return DD_OK;
    }
    return (HRESULT)d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, handle);
}

static BOOL surf_is_target(PDD_SURFACE_LOCAL s)
{
    return (s->ddsCaps.dwCaps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                                 DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) != 0;
}

/* dxg calls this once per surface it creates (each member of a flip chain
 * or mip chain gets its own call); with Direct3D on, every one is mirrored */
static DWORD APIENTRY DdCreateSurfaceEx(PDD_CREATESURFACEEXDATA d)
{
    /* one PDEV has Direct3D (the primary display); the data's lpDDLcl is a
     * union with the global in some DDK versions, so it is not dereferenced */
    PPDEV p = d3d_pdev;
    PDD_SURFACE_LOCAL s = d->lpDDSLcl;

    if (p && s && s->lpGbl && p->d3d) {
        d3d_register(p, s);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* the runtime's device-info queries (D3DDEVINFOID_*: texture manager,
 * vertex stats): nothing to report, the buffer stays as it was */
static DWORD APIENTRY DdGetDriverState(PDD_GETDRIVERSTATEDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdDestroySurface(PDD_DESTROYSURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (d->lpDDSurface && !(d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        d3d_handle_op(p, D3DPT_OP_VRAM_RELEASE, surf_handle(d->lpDDSurface));
    }
    d->ddRVal = DD_OK;
    /* dxg frees the heap block */
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdLock(PDD_LOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    /* a render target the host drew into: bring the frame into VRAM first */
    if (p->ctx_live && s && surf_is_target(s)) {
        d3d_readback(p, s);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdUnlock(PDD_UNLOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    /* the guest wrote texels (or a target): the host re-reads them before use */
    if (p->d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        ((s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (p->ctx_live && surf_is_target(s)))) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, surf_handle(s));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --- contexts --- */

static D3DCTX *ctx_of(PPDEV p, ULONG_PTR h)
{
    if (h == 0 || h > D3D_MAX_CTX || !p->ctx[h - 1].used) {
        return NULL;
    }
    return &p->ctx[h - 1];
}

static DWORD APIENTRY D3dContextCreate(LPD3DNTHAL_CONTEXTCREATEDATA d)
{
    PPDEV p = d3d_pdev;
    PDD_SURFACE_LOCAL rt = d->lpDDSLcl, z = d->lpDDSZLcl;
    d3dpt_ctx_create *c;
    ULONG i, off, hr;

    d->ddrval = DDERR_GENERIC;
    if (!p || !p->d3d || !rt || !rt->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    for (i = 0; i < D3D_MAX_CTX && p->ctx[i].used; i++) {
    }
    if (i == D3D_MAX_CTX) {
        dbg_puts(p, "d3dptdisp: out of contexts\n");
        return DDHAL_DRIVER_HANDLED;
    }
    /* the targets again: a flip chain's surfaces may have moved */
    d3d_register(p, rt);
    if (z) {
        d3d_register(p, z);
    }
    off = d3dpt_enc_ret(&p->enc, 0);
    c = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CREATE, sizeof(*c), 0);
    if (!c) {
        return DDHAL_DRIVER_HANDLED;
    }
    c->handle = i + 1;
    c->ret_off = off;
    c->rt = surf_handle(rt);
    c->z = z ? surf_handle(z) : 0;
    d3dpt_enc_flush(&p->enc);
    hr = p->enc.last_status ? 0x80004005u : d3dpt_enc_result(&p->enc, off)->hr;
    dbg_hex(p, "d3dptdisp: d3d context ", i + 1);
    dbg_hex(p, " rt ", c->rt);
    dbg_hex(p, " z ", c->z);
    dbg_hex(p, " pid ", d->dwPID);
    dbg_hex(p, " -> ", hr);
    dbg_puts(p, "\n");
    if (hr & 0x80000000u) {
        return DDHAL_DRIVER_HANDLED;
    }
    p->ctx[i].used = TRUE;
    p->ctx[i].pid = d->dwPID;
    p->ctx[i].rt = c->rt;
    p->ctx[i].z = c->z;
    p->ctx_live++;
    d->dwhContext = i + 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static void ctx_destroy(PPDEV p, ULONG i)
{
    d3d_handle_op(p, D3DPT_OP_CTX_DESTROY, i + 1);
    p->ctx[i].used = FALSE;
    p->ctx_live--;
    d3dpt_enc_flush(&p->enc);
    dbg_hex(p, "d3dptdisp: d3d context gone ", i + 1);
    dbg_hex(p, " dp2 calls ", p->dp2_calls);
    dbg_puts(p, "\n");
}

static DWORD APIENTRY D3dContextDestroy(LPD3DNTHAL_CONTEXTDESTROYDATA d)
{
    /* no PDEV in the data: the context id is enough once we find its PDEV,
     * and there is one PDEV with Direct3D on (the primary display) */
    PPDEV p = d3d_pdev;

    if (p && ctx_of(p, d->dwhContext)) {
        ctx_destroy(p, (ULONG)d->dwhContext - 1);
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dContextDestroyAll(LPD3DNTHAL_CONTEXTDESTROYALLDATA d)
{
    PPDEV p = d3d_pdev;
    ULONG i;

    if (p) {
        for (i = 0; i < D3D_MAX_CTX; i++) {
            if (p->ctx[i].used && p->ctx[i].pid == d->dwPID) {
                ctx_destroy(p, i);
            }
        }
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dSceneCapture(LPD3DNTHAL_SCENECAPTUREDATA d)
{
    PPDEV p = d3d_pdev;
    D3DCTX *c = p ? ctx_of(p, d->dwhContext) : NULL;

    /* EndScene: the frame into the target's VRAM, where Flip / Blt / Lock expect it */
    if (c && d->dwFlag == D3DNTHAL_SCENE_CAPTURE_END) {
        d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, c->rt);
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dSetRenderTarget(LPD3DNTHAL_SETRENDERTARGETDATA d)
{
    PPDEV p = d3d_pdev;
    D3DCTX *c = p ? ctx_of(p, d->dwhContext) : NULL;
    d3dpt_u32x3 *r;

    d->ddrval = DDERR_GENERIC;
    if (!c || !d->lpDDS || !d->lpDDS->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    d3d_register(p, d->lpDDS);
    if (d->lpDDSZ) {
        d3d_register(p, d->lpDDSZ);
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_SET_RT, sizeof(*r), 0);
    if (r) {
        r->a = (ULONG)d->dwhContext;
        r->b = surf_handle(d->lpDDS);
        r->c = d->lpDDSZ ? surf_handle(d->lpDDSZ) : 0;
        r->pad = 0;
        c->rt = r->b;
        c->z = r->c;
        d->ddrval = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dClear2(LPD3DNTHAL_CLEAR2DATA d)
{
    PPDEV p = d3d_pdev;
    D3DCTX *c = p ? ctx_of(p, d->dwhContext) : NULL;
    ULONG done = 0, n;

    d->ddrval = DDERR_GENERIC;
    if (!c) {
        return DDHAL_DRIVER_HANDLED;
    }
    do {
        d3dpt_ctx_clear *r;
        n = d->dwNumRects - done;
        if (n > 64) n = 64;
        r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CLEAR, sizeof(*r), n * sizeof(D3DRECT_));
        if (!r) {
            return DDHAL_DRIVER_HANDLED;
        }
        r->ctx = (ULONG)d->dwhContext;
        r->flags = d->dwFlags;
        r->color = d->dwFillColor;
        r->count = n;
        r->z = d->dvFillDepth;
        r->stencil = d->dwFillStencil;
        if (n) {
            memcpy(r + 1, d->lpRects + done, n * sizeof(D3DRECT_));
        }
        done += n;
    } while (done < d->dwNumRects);
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dValidateTextureStageState(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA d)
{
    d->dwNumPasses = 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* the render-state array the runtime keeps next to the stream: it expects
 * the driver to mirror every RENDERSTATE token into it */
static void dp2_track_states(const UCHAR *cmd, ULONG len, DWORD *rstates)
{
    ULONG pos = 0;

    while (pos + sizeof(D3DNTHAL_DP2COMMAND) <= len) {
        const D3DNTHAL_DP2COMMAND *c = (const D3DNTHAL_DP2COMMAND *)(cmd + pos);
        ULONG count = c->wStateCount, i;
        pos += sizeof(*c);
        if (c->bCommand != D3DDP2OP_RENDERSTATE_) {
            return;                 /* other tokens have other sizes: stop here */
        }
        if (pos + count * 8 > len) {
            return;
        }
        for (i = 0; i < count; i++) {
            const ULONG *e = (const ULONG *)(cmd + pos + i * 8);
            if (e[0] < 256 && rstates) {
                rstates[e[0]] = e[1];
            }
        }
        pos += count * 8;
    }
}

static DWORD APIENTRY D3dDrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA d)
{
    PPDEV p = d3d_pdev;
    D3DCTX *c = p ? ctx_of(p, d->dwhContext) : NULL;
    const UCHAR *cmd, *vtx;
    ULONG clen = d->dwCommandLength, vsize = d->dwVertexSize, vlen, off;
    d3dpt_dp2 *r;
    d3dpt_ret *res;

    if (!c || !d->lpDDCommands || !d->lpDDCommands->lpGbl) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    cmd = (const UCHAR *)d->lpDDCommands->lpGbl->fpVidMem + d->dwCommandOffset;
    if (d->dwFlags & D3DNTHALDP2_USERMEMVERTICES) {
        vtx = (const UCHAR *)d->lpVertices + d->dwVertexOffset;
    } else if (d->lpDDVertex && d->lpDDVertex->lpGbl) {
        vtx = (const UCHAR *)d->lpDDVertex->lpGbl->fpVidMem + d->dwVertexOffset;
    } else {
        vtx = NULL;
    }
    vlen = vtx ? d->dwVertexLength * vsize : 0;
    if (!vsize || clen > (16u << 20) || vlen > (32u << 20) ||
        D3DPT_ALIGN8(clen) + vlen + sizeof(*r) + sizeof(d3dpt_cmd) > D3DPT_CMD_SIZE) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    dp2_track_states(cmd, clen, d->lpdwRStates);
    p->dp2_calls++;
    off = d3dpt_enc_ret(&p->enc, 0);
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_DP2, sizeof(*r), D3DPT_ALIGN8(clen) + vlen);
    if (!r) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    r->ctx = (ULONG)d->dwhContext;
    r->ret_off = off;
    r->flags = d->dwFlags;
    r->fvf = d->dwVertexType;
    r->vertex_stride = vsize;
    r->command_bytes = clen;
    r->vertex_bytes = vlen;
    r->pad = 0;
    memcpy(r + 1, cmd, clen);
    if (vlen) {
        memcpy((UCHAR *)(r + 1) + D3DPT_ALIGN8(clen), vtx, vlen);
    }
    d3dpt_enc_flush(&p->enc);
    res = d3dpt_enc_result(&p->enc, off);
    if (p->enc.last_status) {
        d->ddrval = DDERR_GENERIC;
        d->dwErrorOffset = 0;
    } else {
        d->ddrval = (HRESULT)res->hr;
        d->dwErrorOffset = res->bytes;
    }
    if (d->ddrval != DD_OK && p->dp2_errors < 8) {
        p->dp2_errors++;
        dbg_hex(p, "d3dptdisp: dp2 ", (ULONG)d->ddrval);
        dbg_hex(p, " at ", d->dwErrorOffset);
        dbg_hex(p, " len ", clen);
        dbg_hex(p, " fvf ", d->dwVertexType);
        dbg_puts(p, "\n");
    }
    return DDHAL_DRIVER_HANDLED;
}

/* -------------------------------------------------------------- entry */

static DRVFN drv_fn[] = {
    { INDEX_DrvEnablePDEV,     (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,   (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,    (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,  (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,     (PFN)DrvAssertMode },
    { INDEX_DrvSetPalette,     (PFN)DrvSetPalette },
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
