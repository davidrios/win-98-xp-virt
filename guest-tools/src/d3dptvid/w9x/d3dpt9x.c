/*
 * d3dpt9x.c — the Windows 98/Me display driver for the d3dpt-vga adapter
 * (doc 19, ADR-012 / M10). The 16-bit half: a DIB Engine mini display
 * driver, ring 3, built with Open Watcom.
 *
 * This is the 9x counterpart of the XP driver's "framebuf" shape (doc 15,
 * M7a) and it does the same thing by the other operating system's rules.
 * On NT the miniport enumerates modes and win32k loads a kernel DLL that
 * hands GDI an engine bitmap over VRAM. On 9x this file is a 16-bit NE
 * module GDI loads as DISPLAY, every drawing export jumps straight to the
 * DIB Engine (dibthunk.asm), and the only things the driver itself does
 * are: find the adapter on the PCI bus, map its two BARs, program the mode
 * registers, and describe the frame buffer to the DIB Engine so that GDI
 * draws directly into guest VRAM with no copy anywhere.
 *
 * What is deliberately *not* here yet (doc 19, and the track's next
 * steps): the mini-VDD, so DOS boxes and screen switching are not handled;
 * DirectDraw (the DCICOMMAND escapes and the ring-3 HAL DLL); 8 bpp
 * palettized modes; the hardware cursor. The DIB Engine's software cursor
 * is used instead, and the mode list comes from the INF.
 *
 * Debug output goes to the adapter's DEBUG register and so into the QEMU
 * log, exactly as the XP driver's does — no COM port, no debugger.
 *
 * Build: guest-tools/build-driver9x.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <minivdd.h>
#include <valmode.h>

#include "d3dpt9x.h"
#include "../../../../d3dpt/d3dpt_fb.h"

/* Pretend we have a 208 by 156 mm screen, as every driver of the era does. */
#define DISPLAY_HORZ_MM     208
#define DISPLAY_VERT_MM     156
#define DISPLAY_SIZE_EN     325
#define DISPLAY_SIZE_TWP    2340

WORD  wScrX = 640, wScrY = 480, wBpp = 32, wDpi = 96, wPalettized = 0;
WORD  OurVMHandle = 0;
DWORD VDDEntryPoint = 0;

DWORD dwRegsPhys = 0, dwVramPhys = 0, dwVramSize = 0;
WORD  wRegsSel = 0, wVramSel = 0;
DWORD dwPitch = 0;

LPDIBENGINE lpDriverPDevice = 0;
WORD wEnabled = 0;
RGBQUAD FAR *lpColorTable = 0;

static BYTE bReEnabling = 0;
static WORD wDIBPdevSize = 0;

/* ------------------------------------------------------- no CRT, no helpers */

/* The driver links no C runtime (the XP one does the same with kcrt.c), so
 * the two things the compiler would otherwise pull in come from here: a
 * 16x16 -> 32 bit multiply, which also keeps __U4M out of the object, and
 * a far memset. */
static DWORD MulW(WORD a, WORD b);
#pragma aux MulW = "mul bx" parm [ax] [bx] value [dx ax];

static void ZeroFar(void __far *p, WORD n)
{
    BYTE __far *q = p;
    while (n--) *q++ = 0;
}

/* ------------------------------------------------------------ DPMI, ports */

/* DPMI 0800h: map a physical range and get a linear address back. The
 * compiler wants CX:BX and DI:SI; DPMI wants the words the other way. */
static DWORD DPMI_MapPhys(DWORD base, DWORD size);
#pragma aux DPMI_MapPhys =      \
    "xchg   cx, bx"             \
    "xchg   si, di"             \
    "mov    ax, 800h"           \
    "int    31h"                \
    "jnc    mp_ok"              \
    "xor    bx, bx"             \
    "xor    cx, cx"             \
    "mp_ok:"                    \
    "mov    dx, bx"             \
    "mov    ax, cx"             \
    parm [cx bx] [di si];

static WORD DPMI_AllocSel(WORD count);
#pragma aux DPMI_AllocSel =     \
    "xor    ax, ax"             \
    "int    31h"                \
    "jnc    as_ok"              \
    "xor    ax, ax"             \
    "as_ok:"                    \
    parm [cx];

static void DPMI_SetBase(WORD sel, DWORD base);
#pragma aux DPMI_SetBase =      \
    "mov    ax, 7"              \
    "int    31h"                \
    parm [bx] [cx dx];

static void DPMI_SetLimit(WORD sel, DWORD limit);
#pragma aux DPMI_SetLimit =     \
    "mov    ax, 8"              \
    "int    31h"                \
    parm [bx] [cx dx];

/* PCI configuration space, mechanism 1. Windows 9x leaves 0xCF8/0xCFC
 * untrapped, so a ring-3 driver may read them directly — which is what
 * every display minidriver of the era does. */
static DWORD PciRead(DWORD addr);
#pragma aux PciRead =           \
    ".386"                      \
    "shl    edx, 16"            \
    "mov    dx, ax"             \
    "mov    eax, edx"           \
    "mov    dx, 0CF8h"          \
    "out    dx, eax"            \
    "mov    dx, 0CFCh"          \
    "in     eax, dx"            \
    "mov    edx, eax"           \
    "shr    edx, 16"            \
    parm [dx ax] value [dx ax];

/* One dword of the adapter's config space: bus 0, device `dev`, function 0. */
static DWORD PciCfg(WORD dev, WORD off)
{
    return PciRead(0x80000000uL | ((DWORD)(dev & 0x1f) << 11) | (off & 0xfc));
}

/* ------------------------------------------------------------ the adapter */

#define REG_PTR(off) ((DWORD __far *)(((DWORD)wRegsSel << 16) | (WORD)(off)))

DWORD RegGet(WORD off)
{
    return *REG_PTR(off);
}

void RegPut(WORD off, DWORD val)
{
    *REG_PTR(off) = val;
}

/* The device's DEBUG register is a character port whose lines reach the
 * QEMU log; the XP driver logs the same way (doc 15). */
void dbg_str(const char *s)
{
    if (!wRegsSel) return;
    while (*s) RegPut(D3DPT_FB_REG_DEBUG, (DWORD)(BYTE)*s++);
    RegPut(D3DPT_FB_REG_DEBUG, '\n');
}

void dbg_val(const char *tag, DWORD v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int i;

    if (!wRegsSel) return;
    while (*tag) RegPut(D3DPT_FB_REG_DEBUG, (DWORD)(BYTE)*tag++);
    RegPut(D3DPT_FB_REG_DEBUG, '=');
    for (i = 28; i >= 0; i -= 4) buf[7 - i / 4] = hex[(v >> i) & 0xf];
    for (i = 0; i < 8; ++i) RegPut(D3DPT_FB_REG_DEBUG, (DWORD)(BYTE)buf[i]);
    RegPut(D3DPT_FB_REG_DEBUG, '\n');
}

/* Find the adapter, map its BARs, and check that the device speaks the
 * register set this driver was built against. */
BOOL AdapterFind(void)
{
    WORD dev;
    DWORD id, lin;

    if (wRegsSel) return TRUE;          /* already found */

    for (dev = 0; dev < 32; ++dev) {
        id = PciCfg(dev, 0);
        if (id == ((DWORD)D3DPT_FB_PCI_DEVICE << 16 | D3DPT_FB_PCI_VENDOR))
            break;
    }
    if (dev == 32) return FALSE;

    dwVramPhys = PciCfg(dev, 0x10) & 0xfffffff0uL;
    dwRegsPhys = PciCfg(dev, 0x14) & 0xfffffff0uL;
    if (!dwVramPhys || !dwRegsPhys) return FALSE;

    /* the register page first: nothing can be reported before it exists */
    lin = DPMI_MapPhys(dwRegsPhys, D3DPT_FB_REGS_SIZE);
    if (!lin) return FALSE;
    wRegsSel = DPMI_AllocSel(1);
    if (!wRegsSel) return FALSE;
    DPMI_SetBase(wRegsSel, lin);
    DPMI_SetLimit(wRegsSel, D3DPT_FB_REGS_SIZE - 1);

    if (RegGet(D3DPT_FB_REG_MAGIC) != D3DPT_FB_MAGIC) {
        wRegsSel = 0;
        return FALSE;
    }
    dbg_val("d3dpt9x: version", RegGet(D3DPT_FB_REG_VERSION));
    if (RegGet(D3DPT_FB_REG_VERSION) != D3DPT_FB_VERSION) {
        dbg_str("d3dpt9x: register set mismatch, refusing the device");
        wRegsSel = 0;
        return FALSE;
    }

    dwVramSize = RegGet(D3DPT_FB_REG_VRAM_SIZE);
    lin = DPMI_MapPhys(dwVramPhys, dwVramSize);
    if (!lin) return FALSE;
    wVramSel = DPMI_AllocSel(1);
    if (!wVramSel) return FALSE;
    DPMI_SetBase(wVramSel, lin);
    DPMI_SetLimit(wVramSel, dwVramSize - 1);

    dbg_val("d3dpt9x: vram", dwVramSize);
    dbg_str("d3dpt9x: adapter found");
    return TRUE;
}

/* Is this a mode the adapter can show? */
static BOOL ModeOk(WORD x, WORD y, WORD bpp)
{
    DWORD caps, need;

    if (!wRegsSel) return FALSE;
    caps = RegGet(D3DPT_FB_REG_CAPS);
    if (bpp == 16 && !(caps & D3DPT_FB_CAP_BPP16)) return FALSE;
    if (bpp == 32 && !(caps & D3DPT_FB_CAP_BPP32)) return FALSE;
    if (bpp != 16 && bpp != 32) return FALSE;   /* 8 bpp is a later step */
    if (x < 320 || y < 200) return FALSE;

    need = MulW(x, (WORD)(y * (bpp / 8)));
    return need <= dwVramSize;
}

int PhysicalEnable(void)
{
    if (!AdapterFind()) return 0;
    if (!ModeOk(wScrX, wScrY, wBpp)) {
        dbg_str("d3dpt9x: refused mode");
        return 0;
    }

    dwPitch = MulW(wScrX, wBpp / 8);

    RegPut(D3DPT_FB_REG_ENABLE, 0);
    RegPut(D3DPT_FB_REG_WIDTH,  wScrX);
    RegPut(D3DPT_FB_REG_HEIGHT, wScrY);
    RegPut(D3DPT_FB_REG_BPP,    wBpp);
    RegPut(D3DPT_FB_REG_PITCH,  dwPitch);
    RegPut(D3DPT_FB_REG_OFFSET, 0);
    RegPut(D3DPT_FB_REG_ENABLE, 1);

    if (!RegGet(D3DPT_FB_REG_ENABLE)) {
        dbg_str("d3dpt9x: the device refused the mode");
        return 0;
    }
    dbg_val("d3dpt9x: mode w", wScrX);
    dbg_val("d3dpt9x: mode h", wScrY);
    dbg_val("d3dpt9x: mode bpp", wBpp);
    return 1;
}

void PhysicalDisable(void)
{
    if (wRegsSel) RegPut(D3DPT_FB_REG_ENABLE, 0);
}

/* ------------------------------------------------------- the display config */

/* The main VDD answers VDD_GET_DISPLAY_CONFIG with the mode the registry
 * holds; SYSTEM.INI is the fallback, as on every 9x display driver. */
static DWORD CallVDD(WORD fn, WORD size, LPVOID p);
#pragma aux CallVDD =               \
    ".386"                          \
    "movzx  eax, ax"                \
    "movzx  ecx, cx"                \
    "movzx  ebx, OurVMHandle"       \
    "movzx  edi, di"                \
    "call   dword ptr VDDEntryPoint"\
    "mov    edx, eax"               \
    "shr    edx, 16"                \
    parm [ax] [cx] [es di] modify [bx];

void ReadDisplayConfig(void)
{
    DISPLAYINFO di;
    WORD x, y, bpp;
    DWORD rc;

    wDpi = GetPrivateProfileInt("display", "dpi", 96, "system.ini");
    x    = GetPrivateProfileInt("display", "x_resolution", 0, "system.ini");
    y    = GetPrivateProfileInt("display", "y_resolution", 0, "system.ini");
    bpp  = GetPrivateProfileInt("display", "bpp", 0, "system.ini");

    if (VDDEntryPoint) {
        rc = CallVDD(VDD_GET_DISPLAY_CONFIG, sizeof(di), &di);
        if (rc != VDD_GET_DISPLAY_CONFIG && !rc) {
            x = di.diXRes;
            y = di.diYRes;
            bpp = di.diBpp;
            wDpi = di.diDPI ? di.diDPI : wDpi;
        }
    }

    if (x && y) { wScrX = x; wScrY = y; }
    if (bpp)    wBpp = bpp;
    if (wBpp != 16 && wBpp != 32) wBpp = 32;    /* until 8 bpp lands */
    wPalettized = 0;
}

/* ------------------------------------------------------------ GDI: Enable */

/* GDI's software cursor lives in the DIB Engine; these two are the
 * surface-access callbacks it uses to exclude it while it draws. */
static VOID WINAPI __loadds BeginAccess(LPPDEVICE lpDevice, WORD l, WORD t,
                                        WORD r, WORD b, WORD flags)
{
    DIB_BeginAccess(lpDevice, l, t, r, b, flags);
}

static VOID WINAPI __loadds EndAccess(LPPDEVICE lpDevice, WORD flags)
{
    DIB_EndAccess(lpDevice, flags);
}

/* An undocumented USER callback: which font resource to use at this DPI. */
DWORD WINAPI __loadds GetDriverResourceID(WORD wResID, LPSTR lpResType)
{
    if (wResID == OBJ_FONT && wDpi != 96) return 2003;
    return wResID;
}

BOOL WINAPI __loadds UserRepaintDisable(BOOL bDisable)
{
    return TRUE;
}

/* GDI calls Enable twice: once (odd style) to fill GDIINFO, once to bring
 * the hardware up and build the PDEVICE. */
UINT WINAPI __loadds Enable(LPVOID lpDevice, UINT style, LPSTR lpDeviceType,
                            LPSTR lpOutputFile, LPVOID lpStuff)
{
    if (!(style & 1)) {
        LPDIBENGINE  lpEng = lpDevice;
        LPBITMAPINFO lpInfo;

        lpDriverPDevice = lpDevice;
        if (!PhysicalEnable()) return 0;
        if (!bReEnabling) {
            /* the VGA core is not ours any more: stop trapping its I/O */
            _asm { mov ax, STOP_IO_TRAP
                   int 2Fh }
        }

        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);

        lpInfo = (LPVOID)((LPBYTE)lpDevice + wDIBPdevSize);
        ZeroFar(&lpInfo->bmiHeader, sizeof(lpInfo->bmiHeader));
        lpInfo->bmiHeader.biSize     = sizeof(lpInfo->bmiHeader);
        lpInfo->bmiHeader.biWidth    = wScrX;
        lpInfo->bmiHeader.biHeight   = wScrY;
        lpInfo->bmiHeader.biPlanes   = 1;
        lpInfo->bmiHeader.biBitCount = wBpp;

        /* Describe guest VRAM to the DIB Engine and let it draw there.
         * This is the whole of the "no copy anywhere" claim on 9x: the
         * bits GDI writes are the bits the device scans out. */
        lpEng->deType         = TYPE_DIBENG;
        lpEng->deFlags        = MINIDRIVER | VRAM;
        lpEng->deWidth        = wScrX;
        lpEng->deHeight       = wScrY;
        lpEng->deWidthBytes   = (WORD)dwPitch;
        lpEng->deDeltaScan    = dwPitch;
        lpEng->dePlanes       = 1;
        lpEng->deBitsPixel    = (BYTE)((wBpp + 7) & 0xf8);
        lpEng->deReserved1    = 0;
        lpEng->delpPDevice    = 0;
        lpEng->deBitsOffset   = 0;
        lpEng->deBitsSelector = wVramSel;
        lpEng->deBitmapInfo   = lpInfo;
        lpEng->deVersion      = VER_DIBENG;
        lpEng->deBeginAccess  = BeginAccess;
        lpEng->deEndAccess    = EndAccess;

        wEnabled = 1;
        dbg_str("d3dpt9x: enabled");
        return 1;
    } else {
        LPGDIINFO lpInfo = lpDevice;

        ZeroFar(lpInfo, sizeof(GDIINFO));
        /* the DIB Engine fills the curve/line/polygon capabilities */
        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);

        lpInfo->dpVersion    = DRV_VERSION;
        lpInfo->dpTechnology = DT_RASDISPLAY;
        lpInfo->dpHorzSize   = DISPLAY_HORZ_MM;
        lpInfo->dpVertSize   = DISPLAY_VERT_MM;
        lpInfo->dpPlanes     = 1;
        lpInfo->dpNumFonts   = 0;
        lpInfo->dpHorzRes    = wScrX;
        lpInfo->dpVertRes    = wScrY;

        lpInfo->dpMLoWin.xcoord = DISPLAY_HORZ_MM * 10;
        lpInfo->dpMLoWin.ycoord = DISPLAY_VERT_MM * 10;
        lpInfo->dpMLoVpt.xcoord = wScrX;
        lpInfo->dpMLoVpt.ycoord = -wScrY;
        lpInfo->dpMHiWin.xcoord = DISPLAY_HORZ_MM * 100;
        lpInfo->dpMHiWin.ycoord = DISPLAY_VERT_MM * 100;
        lpInfo->dpMHiVpt.xcoord = wScrX;
        lpInfo->dpMHiVpt.ycoord = -wScrY;
        lpInfo->dpELoWin.xcoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpELoWin.ycoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpELoVpt.xcoord = wScrX;
        lpInfo->dpELoVpt.ycoord = -wScrX;
        lpInfo->dpEHiVpt.xcoord = -wScrX / 2;
        lpInfo->dpEHiVpt.ycoord = wScrX / 2;
        lpInfo->dpTwpWin.xcoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpWin.ycoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpVpt.xcoord = -wScrX / 2;
        lpInfo->dpTwpVpt.ycoord = wScrX / 2;

        lpInfo->dpLogPixelsX = wDpi;
        lpInfo->dpLogPixelsY = wDpi;
        lpInfo->dpBitsPixel  = (wBpp + 7) & 0xfff8;
        lpInfo->dpDCManage   = DC_IgnoreDFNP;
        lpInfo->dpCaps1     |= C1_COLORCURSOR | C1_REINIT_ABLE | C1_BYTE_PACKED |
                               C1_GLYPH_INDEX;
        lpInfo->dpText      |= TC_CP_STROKE | TC_RA_ABLE;

        wDIBPdevSize = lpInfo->dpDEVICEsize;
        lpInfo->dpNumBrushes  = -1;
        lpInfo->dpNumPens     = -1;
        lpInfo->dpNumColors   = -1;
        lpInfo->dpNumPalReg   = 0;
        lpInfo->dpPalReserved = 0;
        lpInfo->dpColorRes    = 0;
        lpInfo->dpRaster     |= RC_DIBTODEV;
        lpInfo->dpDEVICEsize += sizeof(BITMAPINFOHEADER);

        return sizeof(GDIINFO);
    }
}

/* Called to change resolution without a reboot. */
UINT WINAPI __loadds ReEnable(LPVOID lpDevice, LPGDIINFO lpInfo)
{
    UINT rc;

    ReadDisplayConfig();
    bReEnabling = 1;
    rc = Enable(lpDevice, 0, NULL, NULL, NULL);
    if (rc) Enable(lpInfo, 1, NULL, NULL, NULL);
    bReEnabling = 0;
    return rc;
}

VOID WINAPI __loadds Disable(LPPDEVICE lpDevice)
{
    if (wEnabled) {
        DIB_Disable(lpDevice);
        PhysicalDisable();
        wEnabled = 0;
    }
}

/* ValidateMode (ordinal 700) — GDI asks before it switches. */
UINT WINAPI __loadds ValidateMode(DISPVALMODE FAR *lpMode)
{
    if (!AdapterFind()) return VALMODE_NO_WRONGDRV;
    if (!ModeOk((WORD)lpMode->dvmXRes, (WORD)lpMode->dvmYRes, (WORD)lpMode->dvmBpp))
        return VALMODE_NO_NOMEM;
    return VALMODE_YES;
}

/* ---------------------------------------------------------------- Control */

#define QUERYESCSUPPORT 8

LONG WINAPI __loadds Control(LPVOID lpDevice, UINT function,
                             LPVOID lpInput, LPVOID lpOutput)
{
    if (function == QUERYESCSUPPORT) {
        WORD code = *(WORD FAR *)lpInput;
        if (code == QUERYESCSUPPORT) return 1;
        /* everything else the DIB Engine answers for us */
    }
    return DIB_Control(lpDevice, function, lpInput, lpOutput);
}

/* -------------------------------------------------------------- DriverInit */

/* Int 2Fh AX=1684h: the entry point of a virtual device (the main VDD). */
#define VDD_ID 10
static void __far *int2F_GetEP(WORD ax, WORD bx);
#pragma aux int2F_GetEP =   \
    "int    2Fh"            \
    parm [ax] [bx] value [es di];

static WORD int2F_GetVMID(WORD ax);
#pragma aux int2F_GetVMID = \
    "int    2Fh"            \
    parm [ax] value [bx];

UINT WINAPI GlobalSmartPageLock(HGLOBAL hglb);

/* Dummy pointer, only to get at the _TEXT segment's selector. */
extern char __based(__segname("_TEXT")) *pText;

#pragma aux DriverInit parm [cx] [di] [es si]

UINT FAR DriverInit(UINT cbHeap, UINT hModule, LPSTR lpCmdLine)
{
    /* The code segment must not move while we are on the hardware. */
    GlobalSmartPageLock((__segment)pText);

    VDDEntryPoint = (DWORD)int2F_GetEP(0x1684, VDD_ID);
    OurVMHandle   = int2F_GetVMID(0x1683);

    if (!AdapterFind()) return 0;
    ReadDisplayConfig();
    dbg_str("d3dpt9x: DriverInit");
    return 1;
}
