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
#define DDF_NO_TNL             0x1000 /* bisection: no HWTRANSFORMANDLIGHT claim (the runtime transforms) */
#define DDF_NO_DX8             0x2000 /* bisection: refuse GetDriverInfo2 (a DirectX 7 driver to d3d8.dll) */
#define DDF_NO_SHADERS         0x4000 /* bisection: no vertex / pixel shader versions in D3DCAPS8 */
#define DDF_NO_VSYNC           0x8000 /* flips complete instantly again (M7b): throughput runs */
#define DDF_NO_CKEY            0x10000 /* bisection: no colour keying (caps, callbacks, the key check), no P8 textures */
#define DDF_CKEY_NOBLTCB       0x20000 /* the repro: the colour-key caps without a Blt callback make dxg drop the HAL */
#define DDF_EB_MAXVERT_65535   0x40000 /* the repro: dwMaxVertexCount 65535 makes every DX3 Execute fail with E_OUTOFMEMORY (doc 15) */
#define DDF_NO_PARSEUNKNOWN_CALL 0x80000 /* bisection: never call the runtime's D3DParseUnknownCommand (legacy tokens reach the host) */

#define D3D_MAX_CTX 16

/* a DX8 stream binding: where the vertices / indices are */
typedef struct _DP2STREAM {
    ULONG_PTR mem;
    ULONG bytes, stride;
} DP2STREAM;

typedef struct _D3DCTX {
    ULONG pid;
    ULONG rt, z;                /* VRAM surface handles */
    BOOL used;
    /* the DX8 device state that persists between DrawPrimitives2 calls (the
     * runtime sends SETVERTEXSHADER / SETSTREAMSOURCE / SETINDICES only on
     * change): the vertex format, stream 0, the index buffer */
    ULONG fvf;                  /* the SETVERTEXSHADER value: an FVF, or a vertex shader handle (bit 0) */
    BOOL shader;                /* fvf is a vertex shader handle */
    BOOL vb_um;                 /* stream 0 is the call's own vertex buffer (user memory) */
    ULONG vb_handle, ib_handle; /* the bound buffers (their memory can move between calls: a
                                 * Lock with DISCARD gives a buffer new memory, CreateSurfaceEx again) */
    ULONG vb_stride, ib_stride;
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

    ULONG refusals;             /* pixel formats logged by DdCanCreateSurface */

    /* the flip chain's vertical blank (see flip_done) */
    BOOL flip_pending;          /* a flip is still waiting to be scanned out */
    ULONG flip_frame;           /* FRAMES when it was issued */
    LONGLONG flip_qpc;          /* and when, so a stalled refresh cannot hang a game */

    /* M7c: the command window and the Direct3D state */
    ULONG cmd_offset;           /* window offset in VRAM (0 = the device has none) */
    BOOL d3d;                   /* window mapped and the host executor answered */
    d3dpt_enc enc;
    ULONG dp2_calls, dp2_errors, reg_lines;
    /* the runtime's parser for the tokens a DrawPrimitives2 stream may carry
     * that are not the driver's: the DX3 execute-buffer opcodes
     * (D3DOP_PROCESSVERTICES and friends) on the legacy path (doc 15) */
    HRESULT (APIENTRY *parse_unknown)(PVOID cmd, PVOID *next);
    ULONG parse_lines;
    ULONG blt_lines;                    /* the first DdBlt calls logged */
    ULONG flip_lines;                   /* the first flips logged: the two buffers' handles and offsets */

    /* the hardware cursor (register set v4): its image lives in the
     * D3DPT_FB_CURSOR_BYTES above the DirectDraw heap */
    ULONG cursor_lines;                 /* the first shapes logged */
} PDEV, *PPDEV;

static PPDEV d3d_pdev;              /* the PDEV whose Direct3D is on (the primary display) */

/* the contexts, by handle - 1: global, not in the PDEV. A game's exclusive
 * mode switch gives GDI a new PDEV (the context is created on that one),
 * and its switch back at exit another, before dxg's ContextDestroyAll for
 * the process arrives; a table in the PDEV was empty by then, the host kept
 * the context, and the game's next run had its CTX_CREATE of the same
 * handle refused — E_FAIL from CreateDevice, a crash (GTA 2, 2026-09-05) */
static D3DCTX d3d_ctx[D3D_MAX_CTX];
static ULONG d3d_ctx_live;

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

    d.flGraphicsCaps = GCAPS_ASYNCMOVE;     /* the hardware cursor moves at any time (v4) */
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
        d.flGraphicsCaps = GCAPS_PALMANAGED | GCAPS_COLOR_DITHER | GCAPS_ASYNCMOVE;
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
    if (p->regs) {
        p->regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 0;    /* no sprite over the VGA text */
    }
    /* another PDEV (a full-screen console, the logon desktop switching) takes
     * the screen: back to VGA text through the miniport */
    return EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0,
                              NULL, 0, &ret) == 0;
}

/* ------------------------------------------------------ hardware cursor
 * (register set v4, doc 15 "The hardware cursor"). GDI hands the pointer
 * as a 1 bpp mask surface (AND rows over XOR rows) and, for a colour
 * pointer, a colour surface with a translation to the screen format; the
 * driver turns it into a8r8g8b8 in the VRAM area above the DirectDraw
 * heap and tells the device, which hands it to the host as a cursor
 * sprite. Pointers beyond D3DPT_FB_CURSOR_MAX stay with GDI's software
 * pointer (SPS_DECLINE). */
#ifndef SPS_ALPHA
#define SPS_ALPHA 0x00000010
#endif

static ULONG cursor_offset(PPDEV p);

static void cursor_show(PPDEV p, LONG x, LONG y)
{
    if (!p->regs) {
        return;
    }
    if (x == -1) {
        p->regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 0;
        return;
    }
    p->regs[D3DPT_FB_REG_CURSOR_X / 4] = (ULONG)x;
    p->regs[D3DPT_FB_REG_CURSOR_Y / 4] = (ULONG)y;
    p->regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 1;
}

static ULONG mask_bit(const SURFOBJ *so, ULONG row, ULONG x)
{
    const UCHAR *b = (const UCHAR *)so->pvScan0 + (LONG)row * so->lDelta;
    return (b[x >> 3] >> (7 - (x & 7))) & 1;
}

ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo,
                                  LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl)
{
    PPDEV p = (PPDEV)pso->dhpdev;
    ULONG w, h, i, j;
    ULONG *img;
    BOOL alpha = (fl & SPS_ALPHA) != 0;

    if (!p->regs || !p->fb || !(p->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_CURSOR)) {
        return SPS_DECLINE;
    }
    if (!psoMask && !psoColor) {
        /* no shape: the pointer goes away */
        cursor_show(p, -1, 0);
        return SPS_ACCEPT_NOEXCLUDE;
    }
    if (psoMask) {
        w = psoMask->sizlBitmap.cx;
        h = psoMask->sizlBitmap.cy / 2;
        if (psoMask->iBitmapFormat != BMF_1BPP) {
            return SPS_DECLINE;
        }
    } else {
        w = psoColor->sizlBitmap.cx;
        h = psoColor->sizlBitmap.cy;
    }
    if (!w || !h || w > D3DPT_FB_CURSOR_MAX || h > D3DPT_FB_CURSOR_MAX ||
        xHot < 0 || yHot < 0 || (ULONG)xHot >= w || (ULONG)yHot >= h ||
        (psoColor && ((ULONG)psoColor->sizlBitmap.cx < w || (ULONG)psoColor->sizlBitmap.cy < h))) {
        return SPS_DECLINE;
    }
    img = (ULONG *)((UCHAR *)p->fb + cursor_offset(p));

    if (psoColor) {
        /* the colour pointer as 32 bpp: straight when it is, through a
         * 32 bpp engine bitmap and the translation otherwise */
        if (psoColor->iBitmapFormat == BMF_32BPP) {
            for (j = 0; j < h; j++) {
                const ULONG *row = (const ULONG *)((const UCHAR *)psoColor->pvScan0 + (LONG)j * psoColor->lDelta);
                for (i = 0; i < w; i++) {
                    img[j * w + i] = alpha ? row[i] : (row[i] | 0xff000000u);
                }
            }
        } else {
            SIZEL sz;
            HBITMAP hb;
            SURFOBJ *so;
            RECTL r;
            POINTL pt;

            sz.cx = w;
            sz.cy = h;
            hb = EngCreateBitmap(sz, w * 4, BMF_32BPP, BMF_TOPDOWN, NULL);
            so = hb ? EngLockSurface((HSURF)hb) : NULL;
            if (!so) {
                if (hb) EngDeleteSurface((HSURF)hb);
                return SPS_DECLINE;
            }
            r.left = 0; r.top = 0; r.right = w; r.bottom = h;
            pt.x = 0; pt.y = 0;
            EngCopyBits(so, psoColor, NULL, pxlo, &r, &pt);
            for (j = 0; j < h; j++) {
                const ULONG *row = (const ULONG *)((const UCHAR *)so->pvScan0 + (LONG)j * so->lDelta);
                for (i = 0; i < w; i++) {
                    img[j * w + i] = row[i] | 0xff000000u;
                }
            }
            EngUnlockSurface(so);
            EngDeleteSurface((HSURF)hb);
            alpha = FALSE;
        }
        if (psoMask && !alpha) {
            /* the AND mask: 1 = the screen shows through */
            for (j = 0; j < h; j++) {
                for (i = 0; i < w; i++) {
                    if (mask_bit(psoMask, j, i)) {
                        img[j * w + i] &= 0x00ffffffu;
                    }
                }
            }
        }
    } else {
        /* a monochrome pointer: AND 1 / XOR 0 transparent, AND 0 black or
         * white by XOR, AND 1 / XOR 1 (invert the screen) approximated as
         * black — a sprite has no way to invert */
        for (j = 0; j < h; j++) {
            for (i = 0; i < w; i++) {
                ULONG a = mask_bit(psoMask, j, i), xr = mask_bit(psoMask, h + j, i);
                img[j * w + i] = (a && !xr) ? 0 : xr && !a ? 0xffffffffu : 0xff000000u;
            }
        }
    }

    p->regs[D3DPT_FB_REG_CURSOR_ADDR / 4] = cursor_offset(p);
    p->regs[D3DPT_FB_REG_CURSOR_W / 4] = w;
    p->regs[D3DPT_FB_REG_CURSOR_H / 4] = h;
    p->regs[D3DPT_FB_REG_CURSOR_HOT_X / 4] = (ULONG)xHot;
    p->regs[D3DPT_FB_REG_CURSOR_HOT_Y / 4] = (ULONG)yHot;
    p->regs[D3DPT_FB_REG_CURSOR_DEFINE / 4] = 1;
    cursor_show(p, x, y);
    if (p->cursor_lines < 8) {
        p->cursor_lines++;
        dbg_hex(p, "d3dptdisp: pointer ", w);
        dbg_hex(p, "x", h);
        dbg_puts(p, psoColor ? " colour" : " mono");
        dbg_hex(p, " hot ", (ULONG)xHot);
        dbg_hex(p, ",", (ULONG)yHot);
        dbg_hex(p, " flags ", fl);
        dbg_puts(p, "\n");
    }
    return SPS_ACCEPT_NOEXCLUDE;
}

VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl)
{
    cursor_show((PPDEV)pso->dhpdev, x, y);
}

/* ------------------------------------------------------------ DirectDraw
 * dxg.sys drives these (ddrawint.h, the NT DirectDraw DDI). Surfaces come
 * out of one linear heap in VRAM behind the primary; the runtime does the
 * allocation and the HEL blits, we do memory mapping, flips and vblank. */

/* the Direct3D section below */
static BOOL d3d_init(PPDEV p);
static ULONG pf_format(const DDPIXELFORMAT *f);
static void d3d_register_at(PPDEV p, PDD_SURFACE_LOCAL s, ULONG offset, BOOL quiet);
static void d3d_register_chain(PPDEV p, PDD_SURFACE_LOCAL s);
static void d3d_register_moved(PPDEV p, PDD_SURFACE_LOCAL s);
static ULONG surf_handle(PDD_SURFACE_LOCAL s);
static void d3d_colorkey_op(PPDEV p, ULONG handle, ULONG lo, ULONG hi, ULONG flags);
static void surf_colorkey_check(PPDEV p, ULONG handle);
static DWORD APIENTRY DdSetColorKey(PDD_SETCOLORKEYDATA d);
static DWORD APIENTRY DdBlt(PDD_BLTDATA d);
static HRESULT d3d_readback(PPDEV p, PDD_SURFACE_LOCAL s);
static DWORD APIENTRY DdCreateSurface(PDD_CREATESURFACEDATA d);
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
static D3DCAPS8_ d3d_caps8;                 /* the DX8 DDI's caps (GetDriverInfo2) */
static DDPIXELFORMAT d3d_fmt8[16];          /* its format list */
static ULONG d3d_fmt8_n;
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

/* the DirectDraw heap ends below the hardware cursor's image (v4) */
static ULONG dd_heap_end(PPDEV p)
{
    return heap_end(p) - D3DPT_FB_CURSOR_BYTES;
}

static ULONG cursor_offset(PPDEV p)
{
    return dd_heap_end(p);
}


static ULONG ddflags(PPDEV p)
{
    return p->regs ? p->regs[D3DPT_FB_REG_DDFLAGS / 4] : 0;
}

static void wait_frame(PPDEV p)
{
    ULONG f, i;
    volatile ULONG spin = 0;
    LONGLONG t0, t, freq;

    if (!p->regs) {
        return;
    }
    f = p->regs[D3DPT_FB_REG_FRAMES / 4];
    EngQueryPerformanceFrequency(&freq);
    EngQueryPerformanceCounter(&t0);
    /* the counter moves at the mode's refresh rate; give up after 50 ms so
     * a device that has stopped counting cannot stop the guest with it */
    for (;;) {
        if (p->regs[D3DPT_FB_REG_FRAMES / 4] != f) {
            return;
        }
        /* both the register read and XP's performance counter leave the
         * guest; pause between two polls so a 16 ms wait costs hundreds of
         * exits instead of tens of thousands */
        for (i = 0; i < 4000; i++) {
            spin = i;
        }
        (void)spin;
        EngQueryPerformanceCounter(&t);
        if ((t - t0) * 20 > freq) {
            return;
        }
    }
}

/* Has the last page flip been scanned out?
 *
 * FRAMES counts the mode's vertical blanks, so a flip issued at one count
 * is on the screen once the count has moved. Until then the flip is in the
 * air — which is exactly what a real card does to a double-buffered chain,
 * and the only frame-rate cap a game of the era has. Without it a flip
 * chain runs at thousands of frames a second and every title that paces
 * itself by its own frame loop (Moto Racer, most 1997 racers) plays far
 * too fast.
 *
 * Bounded like wait_frame: a device that has stopped counting must not
 * stop the guest with it, so after 50 ms the flip counts as done.
 * DDF_NO_VSYNC brings the M7b behaviour back for throughput runs.
 */
static BOOL flip_done(PPDEV p)
{
    LONGLONG t, freq;

    if (!p->flip_pending) {
        return TRUE;
    }
    if (p->regs[D3DPT_FB_REG_FRAMES / 4] != p->flip_frame) {
        p->flip_pending = FALSE;
        return TRUE;
    }
    EngQueryPerformanceFrequency(&freq);
    EngQueryPerformanceCounter(&t);
    if ((t - p->flip_qpc) * 20 > freq) {
        p->flip_pending = FALSE;
        return TRUE;
    }
    return FALSE;
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

    if (p->reg_lines < 4096 && d->lpDDSurfaceDesc && (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: can create buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(p, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(p, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(p, " height ", d->lpDDSurfaceDesc->dwHeight);
        dbg_hex(p, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_hex(p, " different pf ", d->bIsDifferentPixelFormat);
        dbg_puts(p, "\n");
    }
    /* the display format always; with Direct3D also the texture and Z
     * formats the host mirrors (pf_format) */
    if (!d->bIsDifferentPixelFormat) {
        d->ddRVal = DD_OK;
    } else if (p->d3d && pf_format(&d->lpDDSurfaceDesc->ddpfPixelFormat) != 0) {
        d->ddRVal = DD_OK;
    } else {
        /* The surface a game cannot have is why it falls back to its
         * software renderer, so say which format was refused (the first
         * few: a game that keeps asking would flood the log). Palettized
         * and colour-keyed textures are what a 1997 title asks for and
         * this HAL does not offer yet. */
        if (p->refusals < 8) {
            const DDPIXELFORMAT *f = &d->lpDDSurfaceDesc->ddpfPixelFormat;
            p->refusals++;
            dbg_hex(p, "d3dptdisp: refused pixel format, flags ", f->dwFlags);
            dbg_hex(p, " fourcc ", f->dwFourCC);
            dbg_hex(p, " bits ", f->dwRGBBitCount);
            dbg_hex(p, " r ", f->dwRBitMask);
            dbg_hex(p, " g ", f->dwGBitMask);
            dbg_hex(p, " b ", f->dwBBitMask);
            dbg_hex(p, " a ", f->dwRGBAlphaBitMask);
            dbg_hex(p, " caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
            dbg_puts(p, "\n");
        }
        d->ddRVal = DDERR_INVALIDPIXELFORMAT;
        if (p->reg_lines < 4096) {
            p->reg_lines++;
            dbg_hex(p, "d3dptdisp: cannot create surface, pixel format flags ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwFlags);
            dbg_hex(p, " fourcc ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwFourCC);
            dbg_hex(p, " bits ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwRGBBitCount);
            dbg_hex(p, " caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
            dbg_puts(p, "\n");
        }
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
    /* the previous flip is still on its way to the screen: this is where a
     * double-buffered game waits for the refresh, as it would on a real
     * card. Nothing above may have happened yet — without DDFLIP_WAIT the
     * runtime hands DDERR_WASSTILLDRAWING straight to the game. */
    if (!flip_done(p)) {
        if (!(d->dwFlags & DDFLIP_WAIT)) {
            d->ddRVal = DDERR_WASSTILLDRAWING;
            return DDHAL_DRIVER_HANDLED;
        }
        wait_frame(p);
        p->flip_pending = FALSE;
    }
    if (p->flip_lines < 8 && d->lpSurfCurr && d->lpSurfCurr->lpGbl && d->lpSurfTarg->lpGbl) {
        /* which object is where: under dxg's model the offsets never change
         * and the runtime's render target handle alternates; a runtime that
         * swaps memory shows the same handle at alternating offsets */
        p->flip_lines++;
        dbg_hex(p, "d3dptdisp: flip curr ", surf_handle(d->lpSurfCurr));
        dbg_hex(p, " at ", (ULONG)d->lpSurfCurr->lpGbl->fpVidMem);
        dbg_hex(p, " targ ", surf_handle(d->lpSurfTarg));
        dbg_hex(p, " at ", (ULONG)d->lpSurfTarg->lpGbl->fpVidMem);
        dbg_puts(p, "\n");
    }
    if (d3d_ctx_live) {
        /* what Direct3D rendered into the back buffer must be in its VRAM
         * before it is scanned out — the VRAM the target has *now*: a
         * surface the host knows at another offset is registered again
         * first (a no-op under dxg's model below) */
        d3d_register_moved(p, d->lpSurfCurr);
        d3d_register_moved(p, d->lpSurfTarg);
        d3d_readback(p, d->lpSurfTarg);
    }
    /* the page flip: scan out from the target's VRAM offset. Nothing to
     * exchange: on NT dxg does not exchange the two surfaces' memory, it
     * exchanges their roles (the PRIMARYSURFACE caps move, each handle keeps
     * its VRAM, the application's "back buffer" is the other object from now
     * on) and tells the driver with a CreateSurfaceEx pair. The first M7c cut
     * re-registered the target at the current surface's offset and vice versa
     * here, which put the host's render target in the displayed buffer every
     * other frame (CKTEST, 2026-09-05). */
    p->regs[D3DPT_FB_REG_OFFSET / 4] = (ULONG)d->lpSurfTarg->lpGbl->fpVidMem;
    p->flip_frame = p->regs[D3DPT_FB_REG_FRAMES / 4];
    EngQueryPerformanceCounter(&p->flip_qpc);
    p->flip_pending = !(ddflags(p) & DDF_NO_VSYNC);
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
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    /* DDGFS_CANFLIP and DDGFS_ISFLIPDONE both come down to "is the last
     * flip on the screen": the runtime spins here for DDFLIP_WAIT and
     * passes DDERR_WASSTILLDRAWING to the game without it */
    d->ddRVal = (p->regs && !flip_done(p)) ? DDERR_WASSTILLDRAWING : DD_OK;
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
        p->flip_pending = FALSE;
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
/* GUID_DDStereoMode doubles as GUID_GetDriverInfo2 (DX8 DDI) when the data
 * carries the D3DGDI2 magic; a real stereo query is refused */
static const GUID guid_stereomode = {
    0xf828169c, 0xa8e8, 0x11d2, { 0xa1, 0xf2, 0x00, 0xa0, 0xc9, 0x83, 0xea, 0xf6 }
};

/* the DX8 runtime's questions (GetDriverInfo2): the answer goes into the
 * same buffer. The size that counts is the one inside the GDI2 header:
 * d3d8.dll leaves the outer dwExpectedSize at the previous query's 24
 * bytes and rejects the driver unless dwActualSize equals the inner one
 * (its disassembly, 2026-09-05) */
static void gdi2_answer(PPDEV p, PDD_GETDRIVERINFODATA d)
{
    DD_GETDRIVERINFO2DATA_ *g = (DD_GETDRIVERINFO2DATA_ *)d->lpvData;
    ULONG want = g->dwExpectedSize, n;

    dbg_hex(p, "d3dptdisp: gdi2 type ", g->dwType);
    dbg_hex(p, " expected ", want);
    dbg_puts(p, "\n");
    switch (g->dwType) {
    case D3DGDI2_TYPE_GETD3DCAPS8_:
        n = sizeof(d3d_caps8);
        if (n > want) n = want;
        memcpy(d->lpvData, &d3d_caps8, n);
        d->dwActualSize = n;
        d->ddRVal = DD_OK;
        break;
    case D3DGDI2_TYPE_GETFORMATCOUNT_: {
        DD_GETFORMATCOUNTDATA_ *c = (DD_GETFORMATCOUNTDATA_ *)g;
        if (want < sizeof(*c)) { d->ddRVal = DDERR_CURRENTLYNOTAVAIL; break; }
        c->dwFormatCount = d3d_fmt8_n;
        d->dwActualSize = sizeof(*c);
        d->ddRVal = DD_OK;
        break;
    }
    case D3DGDI2_TYPE_GETFORMAT_: {
        DD_GETFORMATDATA_ *f = (DD_GETFORMATDATA_ *)g;
        if (want < sizeof(*f) || f->dwFormatIndex >= d3d_fmt8_n) { d->ddRVal = DDERR_CURRENTLYNOTAVAIL; break; }
        f->format = d3d_fmt8[f->dwFormatIndex];
        d->dwActualSize = sizeof(*f);
        d->ddRVal = DD_OK;
        break;
    }
    case D3DGDI2_TYPE_DXVERSION_: {
        DD_DXVERSION_ *v = (DD_DXVERSION_ *)g;
        if (want >= sizeof(*v)) {
            dbg_hex(p, "d3dptdisp: runtime DirectX version ", v->dwDXVersion);
            dbg_puts(p, "\n");
        }
        d->dwActualSize = sizeof(*v) <= want ? sizeof(*v) : want;
        d->ddRVal = DD_OK;
        break;
    }
    default:
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        break;
    }
}

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
        /* the runtime hands us its parser (lpvData is the function itself,
         * dwExpectedSize 0): the legacy execute-buffer opcodes the DX3 path
         * leaves in a DrawPrimitives2 stream are skipped with it (walk) */
        p->parse_unknown = (HRESULT (APIENTRY *)(PVOID, PVOID *))d->lpvData;
        d->dwActualSize = d->dwExpectedSize;
        d->ddRVal = DD_OK;
    } else if (p->d3d && !(ddflags(p) & DDF_NO_DX8) && guid_eq(&d->guidInfo, &guid_stereomode) &&
               d->lpvData && d->dwExpectedSize >= sizeof(DD_GETDRIVERINFO2DATA_) &&
               ((DD_GETDRIVERINFO2DATA_ *)d->lpvData)->dwMagic == D3DGDI2_MAGIC_) {
        /* the DX8 DDI: without this answer d3d8.dll takes us for a DirectX 7
         * driver (software vertex processing, the DX7 token set) */
        gdi2_answer(p, d);
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
    /* the FOURCC surfaces DirectDraw may create at all (it checks this
     * list before the pixel-format callbacks): the compressed textures.
     * First call: the count; second call: the codes */
    *pdwNumFourCCCodes = p->d3d ? 3 : 0;
    if (pdwFourCC && p->d3d) {
        pdwFourCC[0] = 0x31545844;      /* 'DXT1' (FOURCC_ is defined further down) */
        pdwFourCC[1] = 0x33545844;      /* 'DXT3' */
        pdwFourCC[2] = 0x35545844;      /* 'DXT5' */
    }

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
    /* No DDCAPS_BLT / BLTSTRETCH / BLTCOLORFILL: tried 2026-09-05 evening
     * (doc 15 "Blit caps and the HEL") — on XP a DdBlt that returns
     * DDHAL_DRIVER_NOTHANDLED is E_NOTIMPL to the application, not a
     * fallback to the HEL (DDTEST's windowed colour fill failed), so the
     * caps need a real blitter in the driver; and they did not change
     * FIFA 2000's 1:1 videos, which never Blt at all. */
    /* Source colour keys on video-memory textures (doc 15 "Palettized
     * textures and colour keying"): without these caps user-mode ddraw
     * keeps a texture's key to itself (SetColorKey succeeds, nothing
     * reaches the kernel); with them dxg calls DdSetColorKey and records
     * the key in the surface — provided the driver also has a Blt
     * callback, or dxg drops the whole HAL (DDCAPS_NOHARDWARE; CKTEST
     * bisection 2026-09-05, DDF_CKEY_NOBLTCB is the repro). No DDCAPS_BLT,
     * so the HEL still does every blit and DdBlt is never called. */
    if (p->d3d && !(ddflags(p) & DDF_NO_CKEY)) {
        pHalInfo->ddCaps.dwCaps |= DDCAPS_COLORKEY;
        pHalInfo->ddCaps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    }
    pHalInfo->ddCaps.dwVidMemTotal = dd_heap_end(p) - start;
    pHalInfo->ddCaps.dwVidMemFree = dd_heap_end(p) - start;
    /* dwPalCaps stays 0 at 8 bpp too: XP's dxg.sys drops the whole HAL when
     * a driver reports palette caps (its post-enable validation, next to the
     * DDCAPS_GDI check; 2026-09-04 disassembly). On NT the primary's palette
     * is GDI's: SetPalette / SetEntries reach DrvSetPalette. */
    if (!(ddflags(p) & DDF_NO_GETDRIVERINFO)) {
        pHalInfo->GetDriverInfo = DdGetDriverInfo;
        pHalInfo->dwFlags = DDHALINFO_GETDRIVERINFOSET;
        /* the DX8 runtime asks its GetDriverInfo2 questions (D3DCAPS8, the
         * format list) only when this is set; without it we are a DirectX 7
         * driver to d3d8.dll whatever the answers would have been */
        if (p->d3d && !(ddflags(p) & DDF_NO_DX8)) {
            pHalInfo->dwFlags |= DDHALINFO_GETDRIVERINFO2;
        }
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
        pvmList->fpEnd = dd_heap_end(p) - 1;
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
            if (!(ddflags((PPDEV)dhpdev) & DDF_NO_CKEY)) {
                scb->dwFlags |= DDHAL_SURFCB32_SETCOLORKEY;     /* a texture's source colour key -> the host */
                scb->SetColorKey = DdSetColorKey;
            }
            if (!(ddflags((PPDEV)dhpdev) & DDF_CKEY_NOBLTCB)) {
                /* every blit under the blit caps: declined back to the HEL
                 * (and its presence keeps the HAL with the colour-key caps) */
                scb->dwFlags |= DDHAL_SURFCB32_BLT;
                scb->Blt = DdBlt;
            }
        }
    }
    /* CreateSurface only sizes compressed textures for dxg's allocator */
    cb->dwFlags |= DDHAL_CB32_CREATESURFACE;
    cb->CreateSurface = DdCreateSurface;
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
#define D3DFMT_P8_        41u
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
    if (f->dwFlags & DDPF_D3DFORMAT_) {
        return f->dwFourCC;                 /* the D3DFORMAT itself (the DX8 format list's entries) */
    }
    if (f->dwFlags & DDPF_FOURCC) {
        ULONG cc = f->dwFourCC;
        if (cc < 256) {
            return cc;                      /* a D3DFORMAT in the FOURCC slot */
        }
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
    if ((f->dwFlags & DDPF_PALETTEINDEXED8) && f->dwRGBBitCount == 8) {
        return D3DFMT_P8_;                  /* the palette reaches the host in the DP2 stream (SETPALETTE / UPDATEPALETTE) */
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

static void pf_p8(DDPIXELFORMAT *f)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    f->dwRGBBitCount = 8;
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

static DDSURFACEDESC d3d_texformats[10];
static ULONG d3d_texformats_n;

static void fmt8_add(ULONG fmt, ULONG ops)
{
    DDPIXELFORMAT *f = &d3d_fmt8[d3d_fmt8_n++];

    f->dwSize = sizeof(*f);
    /* all D3DFORMAT-coded, the compressed ones too: d3d8.dll matches the
     * application's format against dwFourCC under this flag (a FOURCC
     * entry made CreateTexture(DXT1) fail; the surface itself needs the
     * code in DrvGetDirectDrawInfo's FOURCC list) */
    f->dwFlags = DDPF_D3DFORMAT_;
    f->dwFourCC = fmt;
    f->dwRBitMask = ops;                    /* dwOperations */
}

static void d3d_caps_init(PPDEV p)
{
    D3DNTHAL_GLOBALDRIVERDATA *g = &d3d_global;
    D3DCAPS8_ *c8 = &d3d_caps8;
    BOOL tnl = !(ddflags(p) & DDF_NO_TNL);
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
    /* a T&L device (ddflags 0x1000 makes it a rasterizer again): the
     * executor maps SETTRANSFORM / SETLIGHT / SETMATERIAL and the lighting
     * states onto DXVK's fixed-function pipeline, so the runtime hands us
     * untransformed vertices instead of transforming them itself */
    c->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION;
    if (tnl) c->dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    c->dtcTransformCaps.dwSize = sizeof(c->dtcTransformCaps);
    c->dtcTransformCaps.dwCaps = tnl ? D3DTRANSFORMCAPS_CLIP : 0;
    c->bClipping = tnl;
    c->dlcLightingCaps.dwSize = sizeof(c->dlcLightingCaps);
    c->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    c->dlcLightingCaps.dwCaps = tnl ? D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL : 0;
    c->dlcLightingCaps.dwNumLights = tnl ? 8 : 0;
    t->dwSize = sizeof(*t);
    t->dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    t->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS;
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
    /* The DX3 execute-buffer path (d3dim.dll's IDirect3DDevice::Execute)
     * sizes its vertex buffer as max(the buffer's vertex count, this cap)
     * vertices plus a page, and its vertex-buffer constructor refuses more
     * than 65535 vertices: with 65535 here every Execute failed with
     * E_OUTOFMEMORY before a single token was built (doc 15 "Execute
     * buffers"). 2048 vertices are exactly the 64 KiB DP2 vertex buffer
     * dxg gives every context, so the runtime never has to regrow it at
     * Execute time (a regrow there left the TL buffer it then handed us
     * empty: the copied vertices went into the old one). */
    c->dwMaxBufferSize = 0;
    c->dwMaxVertexCount = (ddflags(p) & DDF_EB_MAXVERT_65535) ? 65535 : 2048;

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
    d3d_texformats_n = 9;
    if (!(ddflags(p) & DDF_NO_CKEY)) {
        /* 8-bit palettized textures (a palette per texture, what a 1997
         * title's art is stored as) and colour keying: the host expands
         * both to A8R8G8B8 (doc 15 "Palettized textures and colour keying") */
        pf_p8(&d3d_texformats[9].ddpfPixelFormat);
        d3d_texformats_n = 10;
        t->dwTextureCaps |= D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_ALPHAPALETTE;
    }
    for (i = 0; i < d3d_texformats_n; i++) {
        d3d_texformats[i].dwSize = sizeof(DDSURFACEDESC);
        d3d_texformats[i].dwFlags = DDSD_PIXELFORMAT;
    }
    g->dwNumTextureFormats = d3d_texformats_n;
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
    if (tnl) {
        e->dwMaxActiveLights = 8;
        e->wMaxUserClipPlanes = 6;
        e->wMaxVertexBlendMatrices = 4;
        e->dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_VERTEXFOG |
                                    D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    }

    /* the DX8 DDI's caps: the same device, in D3DCAPS8 form (vertex and
     * pixel shaders 1.x run on the host; one stream: the driver copies each
     * draw's vertex range into the record, see D3dDrawPrimitives2) */
    for (i = 0; i < sizeof(*c8) / 4; i++) ((ULONG *)c8)[i] = 0;
    c8->DeviceType = D3DDEVTYPE_HAL_;
    c8->Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_DYNAMICTEXTURES;
    c8->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    c8->DevCaps = c->dwDevCaps | D3DDEVCAPS_PUREDEVICE;
    /* no CLIPTLVERTS: with it the runtime stops clipping pre-transformed
     * vertices and hands us polygons crossing the camera plane, which the
     * host rasterizes as garbage (Max Payne's alley walls, 2026-09-05);
     * without it the runtime clips them itself, as the DX7 runtime did */
    c8->PrimitiveMiscCaps = t->dwMiscCaps | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP;
    c8->RasterCaps = t->dwRasterCaps | D3DPRASTERCAPS_COLORPERSPECTIVE;
    c8->ZCmpCaps = t->dwZCmpCaps;
    c8->SrcBlendCaps = t->dwSrcBlendCaps;
    c8->DestBlendCaps = t->dwDestBlendCaps;
    c8->AlphaCmpCaps = t->dwAlphaCmpCaps;
    c8->ShadeCaps = t->dwShadeCaps;
    c8->TextureCaps = (t->dwTextureCaps & ~D3DPTEXTURECAPS_TRANSPARENCY) | D3DPTEXTURECAPS_MIPMAP;   /* DX8 has no colour key; bit 3 is unused there */
    c8->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                            D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c8->TextureAddressCaps = t->dwTextureAddressCaps | D3DPTADDRESSCAPS_MIRRORONCE;
    c8->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    c8->MaxTextureWidth = c8->MaxTextureHeight = 4096;
    c8->MaxTextureRepeat = 8192;
    c8->MaxAnisotropy = 1;
    c8->MaxVertexW = 1.0e10f;
    c8->StencilCaps = D3DSTENCILCAPS_ALL;
    c8->FVFCaps = 8;
    c8->TextureOpCaps = D3DTEXOPCAPS_ALL;
    c8->MaxTextureBlendStages = 8;
    c8->MaxSimultaneousTextures = 8;
    c8->VertexProcessingCaps = e->dwVertexProcessingCaps;
    c8->MaxActiveLights = e->dwMaxActiveLights;
    c8->MaxUserClipPlanes = e->wMaxUserClipPlanes;
    c8->MaxVertexBlendMatrices = e->wMaxVertexBlendMatrices;
    c8->MaxPointSize = 64.0f;
    c8->MaxPrimitiveCount = 0xffff;
    c8->MaxVertexIndex = 0xffff;
    c8->MaxStreams = 1;
    c8->MaxStreamStride = 256;
    if (ddflags(p) & DDF_NO_SHADERS) {
        c8->VertexShaderVersion = D3DVS_VERSION_0;
        c8->PixelShaderVersion = D3DPS_VERSION_0;
    } else {
        /* vs 1.1 / ps 1.4 (what DXVK's d3d9 compiles of the 1.x models; the
         * runtime validates every shader against these before the
         * CREATE*SHADER token reaches us); 96 vertex constants as the era's
         * hardware, ps 1.x values clamped to +-8 */
        c8->VertexShaderVersion = D3DVS_VERSION_(1, 1);
        c8->MaxVertexShaderConst = 96;
        c8->PixelShaderVersion = D3DPS_VERSION_(1, 4);
        c8->MaxPixelShaderValue = 8.0f;
    }

    /* its format list (DDPF_D3DFORMAT entries: the D3DFORMAT in dwFourCC,
     * the operations in the dwRBitMask slot) */
    for (i = 0; i < sizeof(d3d_fmt8) / 4; i++) ((ULONG *)d3d_fmt8)[i] = 0;
    d3d_fmt8_n = 0;
    fmt8_add(D3DFMT_X8R8G8B8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_DISPLAYMODE_ | D3DFORMAT_OP_3DACCELERATION_ |
                               D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_);
    fmt8_add(D3DFMT_A8R8G8B8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_);
    fmt8_add(D3DFMT_R5G6B5_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_DISPLAYMODE_ | D3DFORMAT_OP_3DACCELERATION_ |
                             D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_);
    fmt8_add(D3DFMT_X1R5G5B5_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_);
    fmt8_add(D3DFMT_A1R5G5B5_, D3DFORMAT_OP_TEXTURE_);
    fmt8_add(D3DFMT_A4R4G4B4_, D3DFORMAT_OP_TEXTURE_);
    fmt8_add(FOURCC_('D', 'X', 'T', '1'), D3DFORMAT_OP_TEXTURE_);
    fmt8_add(FOURCC_('D', 'X', 'T', '3'), D3DFORMAT_OP_TEXTURE_);
    fmt8_add(FOURCC_('D', 'X', 'T', '5'), D3DFORMAT_OP_TEXTURE_);
    if (!(ddflags(p) & DDF_NO_CKEY)) {
        fmt8_add(D3DFMT_P8_, D3DFORMAT_OP_TEXTURE_);
    }
    fmt8_add(D3DFMT_D16_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);
    fmt8_add(D3DFMT_D24X8_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);
    fmt8_add(D3DFMT_D24S8_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);

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
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p && p->reg_lines < 4096 && d->lpDDSurfaceDesc) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: can create d3d buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(p, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(p, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(p, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_puts(p, "\n");
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dCreateD3DBuffer(PDD_CREATESURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    ULONG i;

    if (p && p->reg_lines < 4096 && d->lpDDSurfaceDesc) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: create d3d buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(p, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(p, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(p, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_hex(p, " count ", d->dwSCnt);
        if (d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpGbl) {
            dbg_hex(p, " lcl caps ", d->lplpSList[0]->ddsCaps.dwCaps);
            dbg_hex(p, " gbl linear ", d->lplpSList[0]->lpGbl->dwLinearSize);
            dbg_hex(p, " vidmem ", (ULONG)d->lplpSList[0]->lpGbl->fpVidMem);
        }
        dbg_puts(p, "\n");
    }
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
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p && p->reg_lines < 4096 && d->lpDDSurface && d->lpDDSurface->lpGbl) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: lock d3d buffer ", surf_handle(d->lpDDSurface));
        dbg_hex(p, " caps ", d->lpDDSurface->ddsCaps.dwCaps);
        dbg_hex(p, " vidmem ", (ULONG)d->lpDDSurface->lpGbl->fpVidMem);
        dbg_hex(p, " linear ", d->lpDDSurface->lpGbl->dwLinearSize);
        dbg_hex(p, " flags ", d->dwFlags);
        dbg_puts(p, "\n");
    }
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
    d3d_caps_init(p);
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

/* --- the surface table (DX8 DDI): every surface dxg told us about, by
 * handle, VRAM and system memory alike. The DX8 tokens name vertex / index
 * buffers and the system-memory side of a TEXBLT by handle; the memory is
 * the caller's (user-mode pointers for system-memory surfaces, read only
 * inside DrawPrimitives2, which runs in the caller's context) --- */

typedef struct _SURF_LEVEL {
    ULONG_PTR mem;
    ULONG pitch;
} SURF_LEVEL;

typedef struct _SURF {
    ULONG_PTR mem;              /* system memory: the user pointer; VRAM: the mapped address */
    ULONG size;                 /* bytes (the linear size of a buffer, pitch * height otherwise) */
    ULONG pitch, w, h, fmt;
    UCHAR used, sysmem, buffer, levels;
    SURF_LEVEL lv[15];          /* mip levels 1.. */
    PDD_SURFACE_LOCAL lcl;      /* dxg's surface (valid until DestroySurface): the colour key lives in it */
    UCHAR ck_on;                /* the key the host was told (0xff: not yet) */
    ULONG ck_lo, ck_hi;
} SURF;

static SURF *surf_tab;
static ULONG surf_tab_n;

#define DDSCAPS2_VERTEXBUFFER_ 0x02000000
#define DDSCAPS2_INDEXBUFFER_  0x04000000

static SURF *surf_slot(ULONG handle, BOOL create)
{
    if (handle >= surf_tab_n) {
        SURF *t;
        ULONG n = surf_tab_n ? surf_tab_n : 256;

        if (!create || handle >= 0x100000) {
            return NULL;
        }
        while (n <= handle) n *= 2;
        t = EngAllocMem(FL_ZERO_MEMORY, n * sizeof(SURF), ALLOC_TAG);
        if (!t) {
            return NULL;
        }
        if (surf_tab) {
            memcpy(t, surf_tab, surf_tab_n * sizeof(SURF));
            EngFreeMem(surf_tab);
        }
        surf_tab = t;
        surf_tab_n = n;
    }
    if (!create && !surf_tab[handle].used) {
        return NULL;
    }
    return &surf_tab[handle];
}

/* bytes of one row of w pixels (blocks for DXT); 0 = unknown format */
static ULONG fmt_row_bytes(ULONG f, ULONG w)
{
    switch (f) {
    case D3DFMT_X8R8G8B8_: case D3DFMT_A8R8G8B8_: case D3DFMT_D24X8_: case D3DFMT_D24S8_: case D3DFMT_D32_:
        return w * 4;
    case D3DFMT_R5G6B5_: case D3DFMT_X1R5G5B5_: case D3DFMT_A1R5G5B5_: case D3DFMT_A4R4G4B4_: case D3DFMT_X4R4G4B4_:
    case D3DFMT_D16_: case D3DFMT_D15S1_:
        return w * 2;
    case D3DFMT_P8_:
        return w;
    default:
        if (f == FOURCC_('D', 'X', 'T', '1')) return ((w + 3) / 4) * 8;
        if (f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
            f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5')) return ((w + 3) / 4) * 16;
        return 0;
    }
}

static BOOL fmt_is_dxt(ULONG f)
{
    return f == FOURCC_('D', 'X', 'T', '1') || f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
           f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5');
}

/* the row pitch of a surface: dxg's lPitch, except that for a compressed
 * format the union holds the linear size (DDSD_LINEARSIZE), and a block
 * row is what the copies and the host want */
static ULONG surf_pitch(ULONG fmt, ULONG w, ULONG lpitch)
{
    return fmt_is_dxt(fmt) ? fmt_row_bytes(fmt, w) : lpitch;
}

static ULONG surf_rows(ULONG fmt, ULONG h)
{
    return fmt_is_dxt(fmt) ? (h + 3) / 4 : h;
}

/* VRAM_SURFACE for s at the given VRAM offset (its own, or the one a flip hands it) */
static void d3d_register_at(PPDEV p, PDD_SURFACE_LOCAL s, ULONG offset, BOOL quiet)
{
    d3dpt_vram_surface *r;
    d3dpt_u32x2 lv[15];
    ULONG handle, fmt, caps, n = 1, i;
    PDD_SURFACE_LOCAL m;
    BOOL sysmem, buffer;
    SURF *t;
    ULONG pitch0, rows0;

    if (!p->d3d || !s || !s->lpGbl) {
        return;
    }
    handle = surf_handle(s);
    fmt = surf_format(p, s);
    caps = surf_caps(s);
    pitch0 = surf_pitch(fmt, s->lpGbl->wWidth, (ULONG)s->lpGbl->lPitch);
    rows0 = surf_rows(fmt, s->lpGbl->wHeight);
    sysmem = (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) != 0;
    buffer = (s->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER) != 0 ||
             (s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps2 & (DDSCAPS2_VERTEXBUFFER_ | DDSCAPS2_INDEXBUFFER_)));
    /* one line per surface in the QEMU log: what the host will know it as
     * (a "skipped" surface is one a later SETRENDERTARGET / TEXTUREMAP
     * would report unknown) */
    if (!quiet && p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: surface ", handle);
        dbg_hex(p, " caps ", s->ddsCaps.dwCaps);
        dbg_hex(p, " w ", s->lpGbl->wWidth);
        dbg_hex(p, " h ", s->lpGbl->wHeight);
        dbg_hex(p, " fmt ", fmt);
        dbg_hex(p, " pitch ", (ULONG)s->lpGbl->lPitch);
        dbg_hex(p, " pf ", (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) ? s->lpGbl->ddpfSurface.dwFlags : 0xffffffffu);
        dbg_hex(p, " at ", offset);
        if (buffer) dbg_hex(p, " buffer of ", s->lpGbl->dwLinearSize);
        if (sysmem) dbg_puts(p, " sysmem");
        else if (!fmt) dbg_puts(p, " no format, skipped");
        dbg_puts(p, "\n");
    }
    if (!handle) {
        return;
    }
    if (caps & D3DPT_VS_TEXTURE) {
        for (m = surf_next_mip(s); m && n < 16; m = surf_next_mip(m)) {
            lv[n - 1].a = (ULONG)m->lpGbl->fpVidMem;
            lv[n - 1].b = surf_pitch(fmt, m->lpGbl->wWidth, (ULONG)m->lpGbl->lPitch);
            n++;
        }
    }
    /* the table entry (system-memory surfaces live only here) */
    t = surf_slot(handle, TRUE);
    if (t) {
        t->used = 1;
        t->lcl = s;
        t->ck_on = 0xff;
        t->sysmem = sysmem;
        t->buffer = buffer;
        t->mem = sysmem ? (ULONG_PTR)s->lpGbl->fpVidMem : (ULONG_PTR)p->fb + offset;
        t->pitch = pitch0;
        t->w = s->lpGbl->wWidth;
        t->h = s->lpGbl->wHeight;
        t->fmt = fmt;
        t->size = buffer ? s->lpGbl->dwLinearSize : pitch0 * rows0;
        t->levels = (UCHAR)n;
        for (i = 0; i + 1 < n; i++) {
            t->lv[i].mem = sysmem ? (ULONG_PTR)lv[i].a : (ULONG_PTR)p->fb + lv[i].a;
            t->lv[i].pitch = lv[i].b;
        }
    }
    if (sysmem || !fmt || (ULONGLONG)offset + (ULONGLONG)pitch0 * rows0 > heap_end(p)) {
        return;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*r), (n - 1) * sizeof(d3dpt_u32x2));
    if (!r) {
        return;
    }
    r->handle = handle;
    r->offset = offset;
    r->width = s->lpGbl->wWidth;
    r->height = s->lpGbl->wHeight;
    r->pitch = pitch0;
    r->format = fmt;
    r->caps = caps;
    r->levels = n;
    for (i = 0; i + 1 < n; i++) ((d3dpt_u32x2 *)(r + 1))[i] = lv[i];
}

/* The texture's source colour key, read off dxg's surface when a
 * TEXTURESTAGESTATE binds it (DDRAWISURF_HASCKEYSRCBLT + ddckCKSrcBlt, as
 * the DDK's sample drivers do). dxg records it there — and calls
 * DdSetColorKey — only for a driver with the colour-key DirectDraw caps;
 * this check covers a key set before the surface was mirrored and a
 * surface re-created under the same handle. The host hears of it once per
 * change. */
static void surf_colorkey_check(PPDEV p, ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);
    PDD_SURFACE_LOCAL s;
    UCHAR on;
    ULONG lo, hi;

    if (!t || !t->lcl || t->sysmem || t->buffer) {
        return;
    }
    s = t->lcl;
    on = (s->dwFlags & DDRAWISURF_HASCKEYSRCBLT) ? 1 : 0;
    lo = on ? s->ddckCKSrcBlt.dwColorSpaceLowValue : 0;
    hi = on ? s->ddckCKSrcBlt.dwColorSpaceHighValue : 0;
    if (t->ck_on == 0xff && p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: texture ", handle);
        dbg_hex(p, " bound, surface flags ", s->dwFlags);
        dbg_hex(p, " src key ", s->ddckCKSrcBlt.dwColorSpaceLowValue);
        dbg_hex(p, "..", s->ddckCKSrcBlt.dwColorSpaceHighValue);
        dbg_hex(p, " dst key ", s->ddckCKDestBlt.dwColorSpaceLowValue);
        dbg_puts(p, "\n");
    }
    if (t->ck_on == on && t->ck_lo == lo && t->ck_hi == hi) {
        return;
    }
    if (t->ck_on == 0xff && !on) {
        t->ck_on = 0;               /* never keyed: nothing to tell */
        return;
    }
    t->ck_on = on;
    t->ck_lo = lo;
    t->ck_hi = hi;
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: colour key of surface ", handle);
        dbg_hex(p, on ? " on " : " off ", lo);
        dbg_hex(p, "..", hi);
        dbg_puts(p, "\n");
    }
    d3d_colorkey_op(p, handle, lo, hi, on);
}

/* VRAM_COLORKEY: the texture's source colour key (protocol v8) */
static void d3d_colorkey_op(PPDEV p, ULONG handle, ULONG lo, ULONG hi, ULONG flags)
{
    d3dpt_u32x4 *k;

    if (!p->d3d || !handle) {
        return;
    }
    k = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_COLORKEY, sizeof(*k), 0);
    if (k) {
        k->a = handle;
        k->b = lo;
        k->c = hi;
        k->d = flags;
    }
}

static void d3d_register(PPDEV p, PDD_SURFACE_LOCAL s)
{
    d3d_register_at(p, s, (ULONG)s->lpGbl->fpVidMem, FALSE);
}

/* s and what is attached to it — a flip chain's other buffers, a Z buffer
 * — each under its own handle, the ones the host does not know yet or
 * knows at another offset. dxg's CreateSurfaceEx comes for the root of a
 * complex surface; a DirectX 7 interface's flip chain gets one call per
 * member as well, a DirectX 6 title's arrives as its primary alone (GTA 2,
 * 2026-09-05): the back buffer, handle 2, was never registered, the
 * runtime's SETRENDERTARGET 2 was unknown to the host, every frame went
 * into the front buffer's VRAM while the flips alternated the scanout, and
 * half the frames showed the buffer nobody drew. A flip chain's attach
 * list is a ring; mip levels go with their texture (d3d_register_at) */
static void d3d_register_chain(PPDEV p, PDD_SURFACE_LOCAL s)
{
    PDD_SURFACE_LOCAL seen[8];
    ULONG n = 0, i, j;

    d3d_register(p, s);
    seen[n++] = s;
    for (i = 0; i < n; i++) {
        PDD_ATTACHLIST a;
        for (a = seen[i]->lpAttachList; a && n < 8; a = a->lpLink) {
            PDD_SURFACE_LOCAL t = a->lpAttached;
            SURF *k;

            if (!t || !t->lpGbl || (t->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) {
                continue;
            }
            for (j = 0; j < n && seen[j] != t; j++) {
            }
            if (j < n) {
                continue;
            }
            seen[n++] = t;
            k = (t->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? NULL : surf_slot(surf_handle(t), FALSE);
            if (!k || k->mem != (ULONG_PTR)p->fb + (ULONG)t->lpGbl->fpVidMem) {
                d3d_register(p, t);
            }
        }
    }
}

/* a video-memory surface the host knows at another offset: registered
 * again where it is now, silently. Under dxg's flip model nothing moves
 * (DdFlip); a runtime that swaps two buffers' memory instead is caught
 * here at the next flip or lock */
static void d3d_register_moved(PPDEV p, PDD_SURFACE_LOCAL s)
{
    SURF *k;

    if (!p->d3d || !s || !s->lpGbl || (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        return;
    }
    k = surf_slot(surf_handle(s), FALSE);
    if (!k) {
        d3d_register(p, s);
    } else if (k->mem != (ULONG_PTR)p->fb + (ULONG)s->lpGbl->fpVidMem) {
        d3d_register_at(p, s, (ULONG)s->lpGbl->fpVidMem, TRUE);
    }
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

/* dxg calls this once per surface it creates; with Direct3D on, every one is
 * mirrored (CreateSurfaceEx: a flip chain's members from the attach list) */
/* dxg sizes a video-memory surface from its pixel format's bit count before
 * it takes it from the heap; a compressed FOURCC format has none, so the
 * request was for zero bytes and every DXT texture failed at CreateTexture
 * with D3DERR_OUTOFVIDEOMEMORY (DXTTEST, doc 15). As the DDK samples do:
 * hand dxg the block size (the whole compressed image as one block) and the
 * linear size, and let it allocate. Everything else is left to dxg. */
static DWORD APIENTRY DdCreateSurface(PDD_CREATESURFACEDATA d)
{
    DDSURFACEDESC *sd = d->lpDDSurfaceDesc;
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    ULONG i, f;

    d->ddRVal = DD_OK;
    if (p && p->reg_lines < 4096 && sd && (sd->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: create buffer, caps ", sd->ddsCaps.dwCaps);
        dbg_hex(p, " flags ", sd->dwFlags);
        dbg_hex(p, " width ", sd->dwWidth);
        dbg_hex(p, " linear ", sd->dwLinearSize);
        dbg_hex(p, " count ", d->dwSCnt);
        if (d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpGbl) {
            dbg_hex(p, " lcl caps ", d->lplpSList[0]->ddsCaps.dwCaps);
            dbg_hex(p, " gbl linear ", d->lplpSList[0]->lpGbl->dwLinearSize);
            dbg_hex(p, " vidmem ", (ULONG)d->lplpSList[0]->lpGbl->fpVidMem);
        }
        dbg_puts(p, "\n");
    }
    if (!sd || !(sd->ddpfPixelFormat.dwFlags & DDPF_FOURCC) || !fmt_is_dxt(sd->ddpfPixelFormat.dwFourCC)) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    f = sd->ddpfPixelFormat.dwFourCC;
    for (i = 0; i < d->dwSCnt; i++) {
        PDD_SURFACE_LOCAL s = d->lplpSList[i];
        PDD_SURFACE_GLOBAL g = s ? s->lpGbl : NULL;
        ULONG size;

        if (!g) {
            continue;
        }
        size = fmt_row_bytes(f, g->wWidth) * ((g->wHeight + 3) / 4);
        g->dwLinearSize = size;             /* the lPitch union: the linear size, as surf_pitch expects */
        if (!(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            g->dwBlockSizeX = size;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        if (i == 0) {
            sd->dwFlags |= DDSD_LINEARSIZE;
            sd->dwLinearSize = size;
        }
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdCreateSurfaceEx(PDD_CREATESURFACEEXDATA d)
{
    /* one PDEV has Direct3D (the primary display); the data's lpDDLcl is a
     * union with the global in some DDK versions, so it is not dereferenced */
    PPDEV p = d3d_pdev;
    PDD_SURFACE_LOCAL s = d->lpDDSLcl;

    if (p && s && s->lpGbl && p->d3d) {
        d3d_register_chain(p, s);
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
    SURF *t = d->lpDDSurface ? surf_slot(surf_handle(d->lpDDSurface), FALSE) : NULL;

    if (p->reg_lines < 4096 && d->lpDDSurface && (d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: destroy buffer ", surf_handle(d->lpDDSurface));
        dbg_hex(p, " caps ", d->lpDDSurface->ddsCaps.dwCaps);
        dbg_puts(p, "\n");
    }
    if (t) {
        t->used = 0;
    }
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
    if (d3d_ctx_live && s && surf_is_target(s)) {
        d3d_register_moved(p, s);
        d3d_readback(p, s);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* SetColorKey(DDCKEY_SRCBLT) on a video-memory texture: the host keys the
 * texels in [low, high] (alpha 0 + alpha test while COLORKEYENABLE is on).
 * dxg records the key in the surface as well (surf_colorkey_check finds
 * it there at texture bind, which covers a key set before the surface was
 * mirrored); dwFlags also carries DDCKEY_COLORSPACE for a range */
static DWORD APIENTRY DdSetColorKey(PDD_SETCOLORKEYDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: setcolorkey surface ", s ? surf_handle(s) : 0);
        dbg_hex(p, " flags ", d->dwFlags);
        dbg_hex(p, " key ", d->ckNew.dwColorSpaceLowValue);
        dbg_hex(p, "..", d->ckNew.dwColorSpaceHighValue);
        dbg_puts(p, "\n");
    }
    if (p->d3d && s && (d->dwFlags & DDCKEY_SRCBLT) && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        (s->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) {
        SURF *t = surf_slot(surf_handle(s), FALSE);
        if (t) {
            t->ck_on = 1;
            t->ck_lo = d->ckNew.dwColorSpaceLowValue;
            t->ck_hi = d->ckNew.dwColorSpaceHighValue;
        }
        d3d_colorkey_op(p, surf_handle(s), d->ckNew.dwColorSpaceLowValue, d->ckNew.dwColorSpaceHighValue, 1);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Present so that dxg accepts the colour-key caps; never called, since the
 * driver claims no DDCAPS_BLT (the HEL does every blit in user mode).
 * Declines anything that does arrive — which on XP the application sees
 * as E_NOTIMPL, not as a HEL fallback (doc 15 "Blit caps and the HEL"),
 * so claiming blit caps needs a real blitter here. The first few calls
 * are logged with their rectangles. */
static DWORD APIENTRY DdBlt(PDD_BLTDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p->blt_lines < 8) {
        p->blt_lines++;
        dbg_hex(p, "d3dptdisp: blt flags ", d->dwFlags);
        dbg_hex(p, " rop ", d->bltFX.dwROP);
        dbg_hex(p, " src ", (ULONG)(d->rSrc.right - d->rSrc.left));
        dbg_hex(p, "x", (ULONG)(d->rSrc.bottom - d->rSrc.top));
        dbg_hex(p, " dst ", (ULONG)(d->rDest.right - d->rDest.left));
        dbg_hex(p, "x", (ULONG)(d->rDest.bottom - d->rDest.top));
        dbg_puts(p, " declined to the HEL\n");
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
        ((s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (d3d_ctx_live && surf_is_target(s)))) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, surf_handle(s));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --- contexts --- */

static D3DCTX *ctx_of(PPDEV p, ULONG_PTR h)
{
    if (h == 0 || h > D3D_MAX_CTX || !d3d_ctx[h - 1].used) {
        return NULL;
    }
    return &d3d_ctx[h - 1];
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
    for (i = 0; i < D3D_MAX_CTX && d3d_ctx[i].used; i++) {
    }
    if (i == D3D_MAX_CTX) {
        dbg_puts(p, "d3dptdisp: out of contexts\n");
        return DDHAL_DRIVER_HANDLED;
    }
    /* the targets again (and the chain the target belongs to: a DirectX 6
     * title's back buffer is first seen here): a flip chain's surfaces may
     * have moved */
    d3d_register_chain(p, rt);
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
    memset(&d3d_ctx[i], 0, sizeof(d3d_ctx[i]));
    d3d_ctx[i].used = TRUE;
    d3d_ctx[i].pid = d->dwPID;
    d3d_ctx[i].rt = c->rt;
    d3d_ctx[i].z = c->z;
    d3d_ctx_live++;
    d->dwhContext = i + 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static void ctx_destroy(PPDEV p, ULONG i)
{
    d3d_handle_op(p, D3DPT_OP_CTX_DESTROY, i + 1);
    d3d_ctx[i].used = FALSE;
    d3d_ctx_live--;
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
            if (d3d_ctx[i].used && d3d_ctx[i].pid == d->dwPID) {
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
    d3d_register_chain(p, d->lpDDS);
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

/* --- the DP2 stream: the DX7 tokens pass through, the DX8 ones are
 * rewritten (DX8 DDI, doc 15). The runtime's draw tokens name vertex and
 * index buffers the host cannot see, so each draw becomes a self-contained
 * D3DPT_DP2_DRAW8 token carrying its vertex range and indices; TEXBLT is a
 * copy done here (system memory -> VRAM, then VRAM_DIRTY); the shader
 * tokens pass through (the host keeps the shaders per context, protocol
 * v7, and a DRAW8 under a shader carries the handle in its fvf field); the
 * rest (patches, buffer blits, dirty rects) is dropped. Two passes over the
 * stream: the first measures the output and does the blits, the second
 * writes into the record. --- */

typedef struct _DP2WALK {
    PPDEV p;
    const UCHAR *cmd;
    ULONG clen;
    const UCHAR *vtx;           /* the DP2 vertex buffer (user memory), from dwVertexOffset on */
    ULONG vlen, vsize, vcount;  /* its bytes (dwVertexLength * dwVertexSize), stride, vertex count */
    ULONG vall;                 /* the whole buffer from vtx on (a dxg buffer's linear size) */
    ULONG fvf;                  /* the current vertex format (SETVERTEXSHADER): an FVF, or a vertex shader handle (bit 0) */
    DP2STREAM vb, ib;           /* stream 0 and the index buffer */
    ULONG vb_handle, ib_handle;
    BOOL vb_um;                 /* stream 0 is the DP2 vertex buffer */
    BOOL shader;                /* fvf is a vertex shader handle: the host reads the vertices through its declaration */
    BOOL needs_vb;              /* a DX7 draw token references the DP2 vertex buffer */
    UCHAR *out;                 /* pass 2: the record's command area (NULL in pass 1) */
    ULONG outlen;
    ULONG skipped;              /* draws skipped (bad ranges, unknown buffers) */
    ULONG skip_why;             /* bits: 2 no fvf, 4 no stream, 8 stride < fvf, 16 vertex range, 32 index range, 64 prim */
    ULONG skip_info[6];         /* the first skipped draw: prim, count, voff, nverts, ioff, nindices */
    DWORD *rstates;
    BOOL eb;                    /* D3DNTHALDP2_EXECUTEBUFFER: the stream is a DX3 execute buffer's instructions (doc 15) */
    ULONG bounce;               /* eb: offset of the first instruction the runtime must execute itself (~0 = none) */
    ULONG stop;                 /* eb: bytes of the stream walked (an EXIT or the bounce ends it early) */
} DP2WALK;

static ULONG prim_verts(ULONG prim, ULONG n)
{
    switch (prim) {
    case 1: return n;               /* points */
    case 2: return n * 2;           /* line list */
    case 3: return n + 1;           /* line strip */
    case 4: return n * 3;           /* triangle list */
    case 5: case 6: return n + 2;   /* strip, fan */
    default: return 0;
    }
}

/* the vertex size of an FVF (the host computes the same) */
static ULONG fvf_stride(ULONG fvf)
{
    ULONG n = 0, tex = (fvf >> 8) & 0xf, i;

    switch (fvf & 0xe) {
    case 0x2: n = 12; break;
    case 0x4: n = 16; break;
    case 0x6: n = 16; break;
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    default: return 0;
    }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    for (i = 0; i < tex; i++) {
        switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: n += 8; break;
        case 1: n += 12; break;
        case 2: n += 16; break;
        case 3: n += 4; break;
        }
    }
    return n;
}

static void walk_put(DP2WALK *w, const void *src, ULONG bytes)
{
    if (w->out && bytes) {
        memcpy(w->out + w->outlen, src, bytes);
    }
    w->outlen += bytes;
}

static void walk_pad(DP2WALK *w)
{
    static const UCHAR zero[4] = { 0, 0, 0, 0 };
    ULONG pad = (4 - (w->outlen & 3)) & 3;

    walk_put(w, zero, pad);
}

/* one self-contained draw for the host */
static void walk_draw(DP2WALK *w, ULONG prim, ULONG count, const DP2STREAM *vs, ULONG voff, ULONG nverts,
                      ULONG ioff, ULONG nindices, ULONG min_index)
{
    D3DNTHAL_DP2COMMAND h;
    d3dpt_dp2_draw8 t;
    ULONG stride = vs->stride, vbytes;

    /* a stride wider than the FVF is legal (the runtime passes the
     * application's stride for user-memory draws); under a vertex shader
     * the declaration decides what the stride must cover (checked on the
     * host, which has the declaration) */
    if (!w->fvf) w->skip_why |= 2;
    else if (!stride || !vs->mem) w->skip_why |= 4;
    else if (!w->shader && fvf_stride(w->fvf) > stride) w->skip_why |= 8;
    else if (!prim_verts(prim, count)) w->skip_why |= 64;
    if (!w->fvf || !stride || !vs->mem || (!w->shader && fvf_stride(w->fvf) > stride) || !prim_verts(prim, count)) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = w->fvf; w->skip_info[4] = stride; w->skip_info[5] = nindices;
        }
        w->skipped++;
        return;
    }
    if (!nindices) {
        nverts = prim_verts(prim, count);
    }
    vbytes = nverts * stride;
    if (nverts > 0x10000 || voff > vs->bytes || vbytes > vs->bytes - voff ||
        (nindices && (!w->ib.mem || w->ib.stride != 2 || ioff > w->ib.bytes || nindices * 2 > w->ib.bytes - ioff))) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = nverts; w->skip_info[4] = ioff; w->skip_info[5] = nindices;
        }
        w->skipped++;
        w->skip_why |= (nverts > 0x10000 || voff > vs->bytes || vbytes > vs->bytes - voff) ? 16 : 32;
        return;
    }
    h.bCommand = (BYTE)D3DPT_DP2_DRAW8;
    h.bReserved = 0;
    h.wPrimitiveCount = 0;
    t.prim_type = prim;
    t.prim_count = count;
    t.fvf = w->fvf;
    t.stride = stride;
    t.nverts = nverts;
    t.nindices = nindices;
    t.min_index = min_index;
    t.pad = 0;
    walk_put(w, &h, sizeof(h));
    walk_put(w, &t, sizeof(t));
    walk_put(w, (const void *)(vs->mem + voff), vbytes);
    walk_pad(w);
    if (nindices) {
        walk_put(w, (const void *)(w->ib.mem + ioff), nindices * 2);
        walk_pad(w);
    }
}

/* TEXBLT: system memory -> the VRAM texture, every level, then VRAM_DIRTY */
static void walk_texblt(DP2WALK *w, const ULONG *b)
{
    PPDEV p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    LONG dx = (LONG)b[2], dy = (LONG)b[3], sl = (LONG)b[4], st = (LONG)b[5], sr = (LONG)b[6], sb = (LONG)b[7];
    ULONG lv, levels, bpp;
    BOOL dxt;

    if (!dst || !src || !dst->fmt || dst->fmt != src->fmt || !src->sysmem || dst->buffer || src->buffer) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: texblt refused, dst ", b[0]);
            dbg_hex(p, " fmt ", dst ? dst->fmt : 0);
            dbg_hex(p, " w ", dst ? dst->w : 0);
            dbg_hex(p, " h ", dst ? dst->h : 0);
            dbg_hex(p, " src ", b[1]);
            dbg_hex(p, " fmt ", src ? src->fmt : 0);
            dbg_hex(p, " w ", src ? src->w : 0);
            dbg_hex(p, " h ", src ? src->h : 0);
            dbg_hex(p, " pitch ", src ? src->pitch : 0);
            dbg_hex(p, " sysmem ", src ? src->sysmem : 0);
            dbg_hex(p, " rect ", (b[4] << 16) | (b[5] & 0xffff));
            dbg_hex(p, "..", (b[6] << 16) | (b[7] & 0xffff));
            dbg_puts(p, "\n");
        }
        return;
    }
    if (sl < 0 || st < 0 || sr <= sl || sb <= st || dx < 0 || dy < 0) {
        return;
    }
    dxt = fmt_is_dxt(dst->fmt);
    bpp = fmt_row_bytes(dst->fmt, 1);
    levels = dst->levels < src->levels ? dst->levels : src->levels;
    for (lv = 0; lv < levels; lv++) {
        ULONG_PTR smem = lv ? src->lv[lv - 1].mem : src->mem, dmem = lv ? dst->lv[lv - 1].mem : dst->mem;
        ULONG spitch = lv ? src->lv[lv - 1].pitch : src->pitch, dpitch = lv ? dst->lv[lv - 1].pitch : dst->pitch;
        ULONG sw = src->w >> lv, sh = src->h >> lv, dw = dst->w >> lv, dh = dst->h >> lv;
        ULONG x0 = (ULONG)sl >> lv, y0 = (ULONG)st >> lv, x1 = (ULONG)dx >> lv, y1 = (ULONG)dy >> lv;
        ULONG cw = (ULONG)(sr - sl) >> lv, ch = (ULONG)(sb - st) >> lv, rows, rowbytes, y;

        if (!sw) sw = 1;
        if (!sh) sh = 1;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        if (!cw) cw = 1;
        if (!ch) ch = 1;
        if (x0 + cw > sw) cw = sw > x0 ? sw - x0 : 0;
        if (y0 + ch > sh) ch = sh > y0 ? sh - y0 : 0;
        if (x1 + cw > dw) cw = dw > x1 ? dw - x1 : 0;
        if (y1 + ch > dh) ch = dh > y1 ? dh - y1 : 0;
        if (!cw || !ch || !smem || !dmem) {
            continue;
        }
        if (dxt) {
            rows = (ch + 3) / 4;
            rowbytes = fmt_row_bytes(dst->fmt, cw);
            smem += (y0 / 4) * spitch + fmt_row_bytes(dst->fmt, x0);
            dmem += (y1 / 4) * dpitch + fmt_row_bytes(dst->fmt, x1);
        } else {
            rows = ch;
            rowbytes = cw * bpp;
            smem += y0 * spitch + x0 * bpp;
            dmem += y1 * dpitch + x1 * bpp;
        }
        for (y = 0; y < rows; y++) {
            memcpy((void *)(dmem + y * dpitch), (const void *)(smem + y * spitch), rowbytes);
        }
    }
    if (!dst->sysmem) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, b[0]);
    }
}

/* the body size of a token, ~0 when unknown or truncated; the IMM tokens'
 * payload is DWORD-aligned by offset (their size depends on where they sit) */
static ULONG walk_body_size(ULONG op, ULONG count, const UCHAR *q, ULONG left, ULONG pos, ULONG stride)
{
    ULONG pad = (4 - ((pos + 4) & 3)) & 3, n, i, sz;

    switch (op) {
    case 1: return count * 4;                                   /* POINTS */
    case 2: return count * 4;                                   /* INDEXEDLINELIST */
    case 3: return count * 8;                                   /* INDEXEDTRIANGLELIST */
    case 8: return count * 8;                                   /* RENDERSTATE */
    case 15: case 16: case 18: case 19: case 21: return 2;      /* LINELIST .. TRIANGLEFAN: wVStart */
    case 17: return 2 + (count + 1) * 2;                        /* INDEXEDLINESTRIP */
    case 20: case 22: return 2 + (count + 2) * 2;               /* INDEXEDTRIANGLESTRIP / FAN */
    case 23: return pad + 4 + (count + 2) * stride;             /* TRIANGLEFAN_IMM (+ the pad after) */
    case 24: return pad + count * 2 * stride;                   /* LINELIST_IMM */
    case 25: return count * 8;                                  /* TEXTURESTAGESTATE */
    case 26: return 2 + count * 6;                              /* INDEXEDTRIANGLELIST2 */
    case 27: return 2 + count * 4;                              /* INDEXEDLINELIST2 */
    case 28: return count * 16;                                 /* VIEWPORTINFO */
    case 29: return count * 8;                                  /* WINFO */
    case 30: return count * 12;                                 /* SETPALETTE */
    case 31: return left < 8 ? ~0u : 8 + (ULONG)((const USHORT *)q)[3] * 4;   /* UPDATEPALETTE */
    case 32: return count * 8;                                  /* ZRANGE */
    case 33: return count * 68;                                 /* SETMATERIAL */
    case 34:                                                    /* SETLIGHT: 8, + 104 with data */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1] == 2 ? 112 : 8;
            sz += n;
        }
        return sz;
    case 35: return count * 4;                                  /* CREATELIGHT */
    case 36: return count * 68;                                 /* SETTRANSFORM */
    case 38: return count * 36;                                 /* TEXBLT */
    case 39: return count * 12;                                 /* STATESET */
    case 40: return count * 8;                                  /* SETPRIORITY */
    case 41: return count * 8;                                  /* SETRENDERTARGET */
    case 42: return 16 + count * 16;                            /* CLEAR */
    case 43: return count * 8;                                  /* SETTEXLOD */
    case 44: return count * 20;                                 /* SETCLIPPLANE */
    case 45:                                                    /* CREATEVERTEXSHADER: handle, decl size, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 12) return ~0u;
            sz += 12 + ((const ULONG *)(q + sz))[1] + ((const ULONG *)(q + sz))[2];
        }
        return sz;
    case 46: case 47: case 55: case 56: return count * 4;       /* shader handles */
    case 48: case 57:                                           /* shader constants: register, count, count * 16 */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1] * 16;
        }
        return sz;
    case 49: return count * 12;                                 /* SETSTREAMSOURCE */
    case 50: return count * 8;                                  /* SETSTREAMSOURCEUM */
    case 51: return count * 8;                                  /* SETINDICES */
    case 52: return count * 12;                                 /* DRAWPRIMITIVE */
    case 53: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE */
    case 54:                                                    /* CREATEPIXELSHADER: handle, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1];
        }
        return sz;
    case 58: return count * 12;                                 /* CLIPPEDTRIANGLEFAN */
    case 59: return count * 12;                                 /* DRAWPRIMITIVE2 */
    case 60: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE2 */
    case 61: case 62:                                           /* patches: handle, flags [, segments] [, info] */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1];
            sz += 8 + ((n & 1) ? (op == 61 ? 16 : 12) : 0) + ((n & 2) ? (op == 61 ? 28 : 16) : 0);
        }
        return sz;
    case 63: return count * 48;                                 /* VOLUMEBLT */
    case 64: return count * 24;                                 /* BUFFERBLT */
    case 65: return count * 68;                                 /* MULTIPLYTRANSFORM */
    case 66: return count * 20;                                 /* ADDDIRTYRECT */
    case 67: return count * 28;                                 /* ADDDIRTYBOX */
    default: return ~0u;
    }
}

/* one pass over the runtime's stream; FALSE = an unknown token stopped it
 * there (the rest is copied verbatim for the host to report) */
static BOOL walk(DP2WALK *w)
{
    ULONG pos = 0, i;

    while (pos + 4 <= w->clen) {
        const D3DNTHAL_DP2COMMAND *c = (const D3DNTHAL_DP2COMMAND *)(w->cmd + pos);
        const UCHAR *q = w->cmd + pos + 4;
        ULONG op = c->bCommand, count = c->wPrimitiveCount, left = w->clen - pos - 4, size;
        ULONG stride = w->vsize ? w->vsize : (w->fvf ? fvf_stride(w->fvf) : 0);

        if (w->eb) {
            /* A DX3 execute buffer (IDirect3DDevice::Execute, d3dim.dll's
             * UNCLIPPED path): the stream is the buffer's own D3DINSTRUCTION
             * list from the current instruction on, and the runtime is a
             * pass-through — it executes nothing here itself. The opcodes
             * share the DP2 numbering where the payloads match (POINT /
             * LINE / TRIANGLE / STATERENDER are 1 / 2 / 3 / 8: POINTS,
             * INDEXEDLINELIST, the 8-byte INDEXEDTRIANGLELIST, RENDERSTATE),
             * and those the driver must consume, along with SPAN (13,
             * skipped) and EXIT (11, the end). Everything else —
             * PROCESSVERTICES (9) first of all, the matrix / light opcodes
             * 4..7, TEXTURELOAD, BRANCHFORWARD, SETSTATUS — is the runtime's:
             * the call ends *before* it with D3DERR_COMMAND_UNPARSED and its
             * offset in dwErrorOffset, the runtime executes it (a
             * PROCESSVERTICES COPY / TRANSFORM fills the TL vertex buffer
             * we draw from) and calls again from the next instruction.
             * Skipping it instead leaves the vertices unwritten (doc 15
             * "Execute buffers"). */
            if (op == 11) {                                     /* EXIT */
                w->stop = pos;
                return TRUE;
            }
            if (op == 13) {                                     /* SPAN: bSize x count, nothing for the host */
                size = (ULONG)c->bReserved * count;
                if (size > left) size = left;
                pos += 4 + size;
                continue;
            }
            if (op != 1 && op != 2 && op != 3 && op != 8) {
                w->bounce = pos;
                w->stop = pos;
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: execute buffer: opcode ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " bounced to the runtime at ", pos);
                    dbg_puts(w->p, "\n");
                }
                return TRUE;
            }
        }
        size = walk_body_size(op, count, q, left, pos, stride);
        if (size == ~0u && w->p->parse_unknown && !(ddflags(w->p) & DDF_NO_PARSEUNKNOWN_CALL)) {
            /* not one of ours and not a legacy opcode: the runtime's parser
             * gets a chance (it knows VIEWPORTINFO / WINFO, which the host
             * handles anyway); the token is dropped from the host's stream */
            PVOID next = NULL;
            HRESULT hr = w->p->parse_unknown((PVOID)c, &next);
            if (hr == DD_OK && next && (const UCHAR *)next > q && (const UCHAR *)next <= w->cmd + w->clen) {
                size = (ULONG)((const UCHAR *)next - q);
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " parsed by the runtime, bytes ", size);
                    dbg_puts(w->p, "\n");
                }
                pos += 4 + size;
                continue;
            }
            if (!w->out && w->p->parse_lines < 8) {
                w->p->parse_lines++;
                dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                dbg_hex(w->p, " unparsed by the runtime too, hr ", (ULONG)hr);
                dbg_puts(w->p, "\n");
            }
        }
        if (size == ~0u || size > left) {
            walk_put(w, w->cmd + pos, w->clen - pos);
            return FALSE;
        }
        if (op == 23 || op == 24) {                             /* the next token is DWORD-aligned by offset */
            ULONG e = (pos + 4 + size + 3) & ~3u;
            size = e - pos - 4 <= left ? e - pos - 4 : left;
        }
        switch (op) {
        case 8:                                                 /* RENDERSTATE: mirrored for the runtime */
            if (w->rstates && !w->out) {
                for (i = 0; i < count; i++) {
                    const ULONG *e = (const ULONG *)(q + i * 8);
                    if (e[0] < 256) w->rstates[e[0]] = e[1];
                }
            }
            walk_put(w, c, 4 + size);
            break;
        case 23: case 24: {                                     /* IMM: re-pad for the output offset */
            ULONG ipad = (4 - ((pos + 4) & 3)) & 3;
            walk_put(w, c, 4);
            walk_pad(w);
            walk_put(w, q + ipad, size - ipad);
            walk_pad(w);
            w->needs_vb = TRUE;
            break;
        }
        case 1: case 2: case 3: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22:
        case 26: case 27:
            w->needs_vb = TRUE;
            walk_put(w, c, 4 + size);
            break;
        case 25:                                                /* TEXTURESTAGESTATE: a bound texture's colour key, in pass 1 */
            if (!w->out) {
                for (i = 0; i < count; i++) {
                    const USHORT *e = (const USHORT *)(q + i * 8);
                    if (e[1] == 0 && ((const ULONG *)(q + i * 8))[1] != 0) {
                        surf_colorkey_check(w->p, ((const ULONG *)(q + i * 8))[1]);
                    }
                }
            }
            walk_put(w, c, 4 + size);
            break;
        case 38:                                                /* TEXBLT: done here, in pass 1 */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_texblt(w, (const ULONG *)(q + i * 36));
            }
            break;
        case 47:                                                /* SETVERTEXSHADER: an FVF, or a shader handle (bit 0) */
            for (i = 0; i < count; i++) {
                ULONG h = ((const ULONG *)q)[i];
                w->fvf = h;
                w->shader = (h & 1) != 0;
            }
            walk_put(w, c, 4 + size);
            break;
        case 49:                                                /* SETSTREAMSOURCE: stream, handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                SURF *t = surf_slot(e[1], FALSE);
                if (e[0] == 0) {
                    w->vb.mem = t ? t->mem : 0;
                    w->vb.bytes = t ? t->size : 0;
                    w->vb.stride = e[2];
                    w->vb_handle = e[1];
                    w->vb_um = FALSE;
                }
            }
            break;
        case 50:                                                /* SETSTREAMSOURCEUM: stream, stride (the DP2 vertex buffer) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                if (e[0] == 0) {
                    w->vb.mem = (ULONG_PTR)w->vtx;
                    w->vb.bytes = w->vall;
                    w->vb.stride = e[1];
                    w->vb_um = TRUE;
                }
            }
            break;
        case 51:                                                /* SETINDICES: handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                SURF *t = e[0] ? surf_slot(e[0], FALSE) : NULL;
                w->ib.mem = t ? t->mem : 0;
                w->ib.bytes = t ? t->size : 0;
                w->ib.stride = e[1];
                w->ib_handle = e[0];
            }
            break;
        case 52:                                                /* DRAWPRIMITIVE: type, VStart, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->vb, e[1] * w->vb.stride, 0, 0, 0, 0);
            }
            break;
        case 59:                                                /* DRAWPRIMITIVE2: type, first vertex offset (bytes), count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->vb, e[1], 0, 0, 0, 0);
            }
            break;
        case 53:                                                /* DRAWINDEXEDPRIMITIVE: type, base, min, nverts, start index, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->vb, (e[1] + e[2]) * w->vb.stride, e[3], e[4] * 2, prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 60:                                                /* DRAWINDEXEDPRIMITIVE2: type, base offset, min, nverts, start offset, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->vb, (ULONG)((LONG)e[1] + (LONG)(e[2] * w->vb.stride)), e[3], e[4], prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 58:                                                /* CLIPPEDTRIANGLEFAN: first vertex offset, edge flags, count */
            /* the DX8 runtime clips pre-transformed triangles itself into
             * a vertex buffer of its own and binds that buffer as stream 0
             * (SETSTREAMSOURCE, its stride) before these tokens: the offset
             * is a byte offset into stream 0, not into the DP2 vertex
             * buffer (which is a dummy under d3d8.dll; doc 15) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, 6, e[2], &w->vb, e[0], 0, 0, 0, 0);
            }
            break;
        case 61: case 62: case 63: case 64: case 66: case 67:            /* patches, volume / buffer blits, dirty rects */
            break;
        default:                                                /* the DX7 state tokens and the shader tokens (45, 46, 48, 54..57) */
            walk_put(w, c, 4 + size);
            break;
        }
        pos += 4 + size;
    }
    if (pos < w->clen) {
        walk_put(w, w->cmd + pos, w->clen - pos);
    }
    w->stop = w->clen;
    return TRUE;
}

static DWORD APIENTRY D3dDrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA d)
{
    PPDEV p = d3d_pdev;
    D3DCTX *c = p ? ctx_of(p, d->dwhContext) : NULL;
    const UCHAR *cmd, *vtx;
    ULONG clen = d->dwCommandLength, vsize = d->dwVertexSize, vlen, vall, vcopy, off;
    d3dpt_dp2 *r;
    d3dpt_ret *res;
    DP2WALK w, w0;

    if (!c || !d->lpDDCommands || !d->lpDDCommands->lpGbl) {
        if (p && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, context ", (ULONG)d->dwhContext);
            dbg_hex(p, " commands ", (ULONG)(ULONG_PTR)d->lpDDCommands);
            dbg_puts(p, "\n");
        }
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    if (p->dp2_calls < 8) {
        dbg_hex(p, "d3dptdisp: dp2 call flags ", d->dwFlags);
        dbg_hex(p, " cmd caps ", d->lpDDCommands->ddsCaps.dwCaps);
        dbg_hex(p, " at ", (ULONG)d->lpDDCommands->lpGbl->fpVidMem);
        dbg_hex(p, " +", d->dwCommandOffset);
        dbg_hex(p, " len ", d->dwCommandLength);
        if (d->lpDDVertex && d->lpDDVertex->lpGbl) {
            dbg_hex(p, " vtx caps ", d->lpDDVertex->ddsCaps.dwCaps);
            dbg_hex(p, " at ", (ULONG)d->lpDDVertex->lpGbl->fpVidMem);
            dbg_hex(p, " linear ", d->lpDDVertex->lpGbl->dwLinearSize);
        } else {
            dbg_hex(p, " user vtx ", (ULONG)(ULONG_PTR)d->lpVertices);
        }
        dbg_hex(p, " +", d->dwVertexOffset);
        dbg_hex(p, " n ", d->dwVertexLength);
        dbg_hex(p, " type ", d->dwVertexType);
        dbg_puts(p, "\n");
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
    if (clen > (16u << 20) || vlen > (32u << 20)) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, command bytes ", clen);
            dbg_hex(p, " vertex bytes ", vlen);
            dbg_puts(p, "\n");
        }
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    /* what the buffer really holds from vtx on: the declared vertices for
     * user memory, a dxg buffer's linear size otherwise */
    vall = vlen;
    if (vtx && !(d->dwFlags & D3DNTHALDP2_USERMEMVERTICES) && d->lpDDVertex->lpGbl->dwLinearSize > d->dwVertexOffset) {
        vall = d->lpDDVertex->lpGbl->dwLinearSize - d->dwVertexOffset;
        if (vall > (32u << 20)) vall = vlen;
    }
    /* pass 1: the output size, the render-state mirror, the TEXBLTs; both
     * passes start from the context's DX8 state */
    memset(&w0, 0, sizeof(w0));
    w0.p = p;
    w0.cmd = cmd;
    w0.clen = clen;
    w0.vtx = vtx;
    w0.vlen = vlen;
    w0.vsize = vsize;
    w0.vcount = d->dwVertexLength;
    w0.vall = vall;
    w0.eb = (d->dwFlags & D3DNTHALDP2_EXECUTEBUFFER) != 0;
    w0.bounce = ~0u;
    w0.fvf = c->fvf ? c->fvf : (d->dwVertexType & 1 ? 0 : d->dwVertexType);
    w0.shader = c->shader;
    w0.vb_um = c->vb_um;
    w0.vb_handle = c->vb_handle;
    w0.ib_handle = c->ib_handle;
    w0.vb.stride = c->vb_stride;
    w0.ib.stride = c->ib_stride;
    if (w0.vb_um) {
        w0.vb.mem = (ULONG_PTR)vtx;
        w0.vb.bytes = vall;
    } else if (w0.vb_handle) {
        SURF *t = surf_slot(w0.vb_handle, FALSE);
        w0.vb.mem = t ? t->mem : 0;
        w0.vb.bytes = t ? t->size : 0;
    }
    if (w0.ib_handle) {
        SURF *t = surf_slot(w0.ib_handle, FALSE);
        w0.ib.mem = t ? t->mem : 0;
        w0.ib.bytes = t ? t->size : 0;
    }
    w = w0;
    w.rstates = d->lpdwRStates;
    walk(&w);
    if (w.eb && w.bounce == 0) {
        /* nothing of ours before the runtime's instruction: bounce at once */
        d->ddrval = D3DERR_COMMAND_UNPARSED_;
        d->dwErrorOffset = d->dwCommandOffset;
        return DDHAL_DRIVER_HANDLED;
    }
    vcopy = w.needs_vb ? vlen : 0;
    if (w.outlen > (48u << 20) || D3DPT_ALIGN8(w.outlen) + vcopy + sizeof(*r) + sizeof(d3dpt_cmd) > D3DPT_CMD_SIZE) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, output bytes ", w.outlen);
            dbg_hex(p, " vertex copy ", vcopy);
            dbg_puts(p, "\n");
        }
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    p->dp2_calls++;
    off = d3dpt_enc_ret(&p->enc, 0);
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_DP2, sizeof(*r), D3DPT_ALIGN8(w.outlen) + vcopy);
    if (!r) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    /* pass 2: the same walk, writing into the record; its end state is the
     * context's for the next call */
    {
        ULONG outlen = w.outlen, skipped = w.skipped;
        w = w0;
        w.out = (UCHAR *)(r + 1);
        walk(&w);
        if (w.outlen != outlen) {           /* cannot happen: the same stream twice */
            w.outlen = outlen;
        }
        c->fvf = w.fvf;
        c->shader = w.shader;
        c->vb_um = w.vb_um;
        c->vb_handle = w.vb_handle;
        c->ib_handle = w.ib_handle;
        c->vb_stride = w.vb.stride;
        c->ib_stride = w.ib.stride;
        if (skipped && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dx8 draws skipped ", skipped);
            dbg_hex(p, " why ", w.skip_why);
            dbg_hex(p, " fvf ", w0.fvf);
            dbg_hex(p, " stride ", w0.vb.stride);
            dbg_hex(p, " bytes ", w0.vb.bytes);
            dbg_hex(p, " ib ", w0.ib.bytes);
            dbg_hex(p, "; first: prim ", w.skip_info[0]);
            dbg_hex(p, " count ", w.skip_info[1]);
            dbg_hex(p, " voff ", w.skip_info[2]);
            dbg_hex(p, " nverts ", w.skip_info[3]);
            dbg_hex(p, " ioff ", w.skip_info[4]);
            dbg_hex(p, " nindices ", w.skip_info[5]);
            dbg_puts(p, "\n");
        }
    }
    r->ctx = (ULONG)d->dwhContext;
    r->ret_off = off;
    r->flags = d->dwFlags;
    r->fvf = d->dwVertexType;
    r->vertex_stride = vsize;
    r->command_bytes = w.outlen;
    r->vertex_bytes = vcopy;
    r->pad = 0;
    if (vcopy) {
        memcpy((UCHAR *)(r + 1) + D3DPT_ALIGN8(w.outlen), vtx, vcopy);
    }
    d3dpt_enc_flush(&p->enc);
    res = d3dpt_enc_result(&p->enc, off);
    if (p->enc.last_status) {
        d->ddrval = DDERR_GENERIC;
        d->dwErrorOffset = 0;
    } else if (w.eb && w.bounce != ~0u && res->hr == DD_OK) {
        /* the host drew what came before it; the runtime takes over at
         * this instruction (dwErrorOffset counts from the buffer's start,
         * like dwCommandOffset) and calls again from the next one */
        d->ddrval = D3DERR_COMMAND_UNPARSED_;
        d->dwErrorOffset = d->dwCommandOffset + w.bounce;
    } else {
        d->ddrval = (HRESULT)res->hr;
        d->dwErrorOffset = res->bytes;
    }
    if (d->ddrval != DD_OK && !(w.eb && w.bounce != ~0u) && p->dp2_errors < 8) {
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
    { INDEX_DrvSetPointerShape, (PFN)DrvSetPointerShape },
    { INDEX_DrvMovePointer,    (PFN)DrvMovePointer },
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
