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
 * **This half cannot work alone, and the run that proved it is written up
 * in doc 19 §11.** Windows unmaps the adapter's PCI base addresses within
 * half a minute of boot because nothing claimed its resources, so the
 * probe below finds zeros: on 9x the ring-0 mini-VDD claims the device and
 * hands the frame buffer to this driver, and it has to exist first. What
 * is here is correct and stays; what it needs is the VxD.
 *
 * Also not here yet: DirectDraw (the DCICOMMAND escapes and the ring-3
 * HAL DLL), 8 bpp palettized modes, and the hardware cursor — the DIB
 * Engine's software cursor is used instead, and the mode list comes from
 * the INF rather than from the adapter (doc 19 §6).
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
#include "d3dpt9v.h"
#include "../../../../d3dpt/d3dpt_fb.h"

/* Pretend we have a 208 by 156 mm screen, as every driver of the era does. */
#define DISPLAY_HORZ_MM     208
#define DISPLAY_VERT_MM     156
#define DISPLAY_SIZE_EN     325
#define DISPLAY_SIZE_TWP    2340

WORD  wScrX = 640, wScrY = 480, wBpp = 32, wDpi = 96, wPalettized = 0;
WORD  OurVMHandle = 0;
DWORD VDDEntryPoint = 0;

DWORD dwVramSize = 0;
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

/* The adapter is the mini-VDD's, not ours: it claims the device, maps
 * VRAM and the register page, and hands us selectors onto both. Doing it
 * here instead does not work — Windows takes the base addresses away from
 * a device nothing claimed (doc 19 §11). */
static WORD CallVDD_Register(void);
#pragma aux CallVDD_Register =      \
    ".386"                          \
    "mov    eax, 83h"               \
    "movzx  ebx, word ptr [OurVMHandle]" \
    "call   dword ptr [VDDEntryPoint]"   \
    "jc     vdd_fail"               \
    "mov    word ptr [wRegsSel], ax"     \
    "mov    word ptr [wVramSel], dx"     \
    "mov    dword ptr [dwVramSize], ecx" \
    "mov    ax, 1"                  \
    "jmp    vdd_done"               \
    "vdd_fail:"                     \
    "xor    ax, ax"                 \
    "vdd_done:"                     \
    value [ax] modify [bx cx dx si];

/* ------------------------------------------------------------ the adapter */

/* The adapter's registers are 32 bits wide and the device accepts **nothing
 * else**: its register BAR is `valid.min_access_size = 4`. A 16-bit compiler
 * turns `*(DWORD __far *)p` into two word accesses, and QEMU answers those
 * with zero and drops the writes without a word anywhere — a register set
 * that reads as a different device, from a driver whose every write is lost
 * (doc 19 Section 14). So both directions are one 32-bit access, written out
 * by hand; the module is .386 throughout. The XP driver never met this: it
 * is 32-bit code and the compiler emitted what the device wanted. */
static DWORD RegGetSel(WORD sel, WORD off);
#pragma aux RegGetSel =     \
    ".386"                  \
    "push   es"             \
    "mov    es, cx"         \
    "mov    eax, es:[bx]"   \
    "mov    edx, eax"       \
    "shr    edx, 16"        \
    "pop    es"             \
    parm [cx] [bx] value [dx ax] modify [ax dx];

static void RegPutSel(WORD sel, WORD off, DWORD val);
#pragma aux RegPutSel =     \
    ".386"                  \
    "push   es"             \
    "mov    es, cx"         \
    "shl    edx, 16"        \
    "mov    dx, ax"         \
    "mov    es:[bx], edx"   \
    "pop    es"             \
    parm [cx] [bx] [dx ax] modify [dx];

DWORD RegGet(WORD off)
{
    return RegGetSel(wRegsSel, off);
}

void RegPut(WORD off, DWORD val)
{
    RegPutSel(wRegsSel, off, val);
}

/* Two debug channels, and the driver needs both. The adapter's DEBUG
 * register is the real one — its lines reach the QEMU log exactly as the
 * XP driver's do (doc 15) — but nothing can reach it until the register
 * page is mapped, which is where the interesting failures are. So every
 * line also goes to port 0xE9, QEMU's debug console (`-debugcon file:…`,
 * ignored by anything else), which works from the first instruction. */
static void OutE9(BYTE c);
#pragma aux OutE9 = "out 0E9h, al" parm [al];

static void dbg_ch(BYTE c)
{
    OutE9(c);
    if (wRegsSel) RegPut(D3DPT_FB_REG_DEBUG, (DWORD)c);
}

void dbg_str(const char *s)
{
    while (*s) dbg_ch((BYTE)*s++);
    dbg_ch('\n');
}

void dbg_val(const char *tag, DWORD v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    while (*tag) dbg_ch((BYTE)*tag++);
    dbg_ch('=');
    for (i = 28; i >= 0; i -= 4) dbg_ch((BYTE)hex[(v >> i) & 0xf]);
    dbg_ch('\n');
}

/* Ask the mini-VDD for the adapter. Everything the driver knows about the
 * hardware arrives here. */
BOOL AdapterFind(void)
{
    if (wRegsSel) return TRUE;          /* already asked */
    if (!VDDEntryPoint) { dbg_str("d3dpt9x: no VDD"); return FALSE; }

    /* Carry says no, and so does a zero selector: the main VDD answers this
     * function itself when no mini-VDD claimed it, and then the registers
     * are whatever its own dispatch left in them. */
    if (!CallVDD_Register() || !wRegsSel || !wVramSel || !dwVramSize) {
        dbg_str("d3dpt9x: the mini-VDD has no adapter for us");
        wRegsSel = wVramSel = 0;
        return FALSE;
    }
    dbg_val("d3dpt9x: regs sel", wRegsSel);
    dbg_val("d3dpt9x: vram sel", wVramSel);
    dbg_val("d3dpt9x: vram", dwVramSize);

    if (RegGet(D3DPT_FB_REG_MAGIC) != D3DPT_FB_MAGIC ||
        RegGet(D3DPT_FB_REG_VERSION) != D3DPT_FB_VERSION) {
        dbg_val("d3dpt9x: magic", RegGet(D3DPT_FB_REG_MAGIC));
        dbg_val("d3dpt9x: version", RegGet(D3DPT_FB_REG_VERSION));
        dbg_str("d3dpt9x: register set mismatch, refusing the device");
        wRegsSel = wVramSel = 0;
        return FALSE;
    }
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

        dbg_str("d3dpt9x: Enable (hardware)");
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

        dbg_str("d3dpt9x: Enable (GDIINFO)");
        AdapterFind();
        ReadDisplayConfig();
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

/* The code segment's selector, for the page lock below. `extern char
 * __based(__segname("_TEXT")) *pText` — the idiom every 9x driver uses —
 * is a trap here: it puts the reference in a *second* segment also called
 * `_TEXT`, of class FAR_DATA, which stays empty in a driver this small.
 * wlink then drops the empty segment from the NE segment table and leaves
 * the relocation pointing at it, and KERNEL refuses to load a module whose
 * relocation names a segment that is not there — silently, which cost this
 * track several boots (doc 19 Section 13). CS is the same selector and needs
 * no relocation at all; build-driver9x.sh now fails the build if any
 * relocation ever points past the segment table again. */
static WORD GetCS(void);
#pragma aux GetCS = "mov ax, cs" value [ax];

#pragma aux DriverInit parm [cx] [di] [es si]

UINT FAR DriverInit(UINT cbHeap, UINT hModule, LPSTR lpCmdLine)
{
    /* The code segment must not move while we are on the hardware. */
    dbg_str("d3dpt9x: DriverInit entered");

    /* Nothing here may fail: GDI treats a zero from DriverInit as "no
     * driver" and silently falls back to VGA, which is indistinguishable
     * from the module never loading. The adapter is found on the first
     * Enable instead, where a failure is at least visible. */
    GlobalSmartPageLock((HGLOBAL)GetCS());
    VDDEntryPoint = (DWORD)int2F_GetEP(0x1684, VDD_ID);
    OurVMHandle   = int2F_GetVMID(0x1683);

    /* Ask the mini-VDD for the adapter here as well as in Enable: this is
     * the earliest point at which anything of ours can be *seen* from
     * outside, because the answer is logged in ring 0 where port 0xE9 and
     * the DEBUG register both work. */
    AdapterFind();
    dbg_str("d3dpt9x: DriverInit done");
    return 1;
}
