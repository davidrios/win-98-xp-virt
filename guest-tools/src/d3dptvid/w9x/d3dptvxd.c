/*
 * d3dptvxd.c — the Win98/Me mini-VDD for the d3dpt-vga adapter (doc 19,
 * ADR-012 / M10). Ring 0, a dynamically loadable VxD, built with Open
 * Watcom's 32-bit compiler.
 *
 * Why this exists, and why before the display driver: on 9x the ring-0
 * half owns the adapter. Windows unmaps a PCI device's base addresses
 * within half a minute of boot when nothing claims them, so a 16-bit
 * display driver that maps the BARs itself finds zeros and GDI falls back
 * to VGA — measured, doc 19 §11. This VxD is what claims the device:
 *
 *   - it finds the adapter on the bus and, if Windows has already taken
 *     the base addresses away, programs them back and re-enables memory
 *     decoding;
 *   - it maps VRAM and the register page into ring-0 linear space;
 *   - it registers itself in the main VDD's mini-VDD dispatch table, so
 *     the display driver's VDD_REGISTER_DISPLAY_DRIVER_INFO call reaches
 *     us and we can hand back 16-bit selectors onto both — the display
 *     driver never touches PCI configuration space again.
 *
 * Debug output goes to port 0xE9 (QEMU's `-debugcon`) and, once the
 * register page is mapped, to the adapter's DEBUG register and so into the
 * QEMU log — the same two channels the .drv uses, and in ring 0 both
 * actually work.
 *
 * Build: guest-tools/build-driver9x.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include "vmm.h"
#include "d3dpt9v.h"
#include "../../../../d3dpt/d3dpt_fb.h"

/* Everything — code, data and constants — goes into one segment of class
 * CODE, so the module links to a single LE object and the DDB below lands
 * at its offset 0. That is where the VMM's loader expects a VxD's
 * descriptor block: a real driver's entry table names object 1 offset 0
 * (checked against the guest's own FXMEMMAP.VXD), while letting the DDB
 * fall into _DATA gives a 16-bit entry into the second object and the VxD
 * silently does not load. vmdisp9x's source carries the same warning. */
#pragma data_seg("_LTEXT", "CODE")
#pragma code_seg("_LTEXT", "CODE")
#pragma const_seg("_LTEXT", "CODE")

void VXD_control(void);
void __stdcall Device_Init_proc(DWORD VM);

DDB VXD_DDB = {
    NULL,                       /* DDB_Next, VMM's */
    DDK_VERSION,
    D3DPT_VXD_ID,
    D3DPT_VXD_MAJOR, D3DPT_VXD_MINOR,
    0,                          /* flags */
    D3DPT_VXD_NAME,
    VDD_Init_Order,             /* with the VDD: we are its mini-VDD */
    (DWORD)VXD_control,
    NULL, NULL,                 /* no V86 / PM API entry: the VDD calls us */
    NULL, NULL,
    NULL,                       /* reference data */
    NULL, 0,                    /* no service table */
    NULL,                       /* no Win32 service table */
    'Prev',
    sizeof(DDB),
    'Rsv1', 'Rsv2', 'Rsv3',
};

/* ------------------------------------------------------------ VMM services */

static ULONG __declspec(naked) __cdecl _MapPhysToLinear(ULONG phys, ULONG bytes, ULONG flags)
{
    VMMJmp(_MapPhysToLinear);
}

static void __declspec(naked) __cdecl BuildDesc_(ULONG base, ULONG limit, ULONG type,
                                                 ULONG size, ULONG flags)
{
    VMMJmp(_BuildDescriptorDWORDs);
}

static void __declspec(naked) __cdecl AllocGDT_(ULONG hi, ULONG lo, ULONG flags)
{
    VMMJmp(_Allocate_GDT_Selector);
}

/* A 16-bit data selector onto a ring-0 linear range, handed to the display
 * driver so it can address VRAM and the registers with a far pointer. */
static WORD MakeSelector(DWORD linear, DWORD bytes)
{
    DWORD hi = 0, lo = 0, sel = 0;

    BuildDesc_(linear, (bytes + 0xfff) >> 12, D3DPT_SEL_TYPE, 0x80, 0);
    _asm {
        mov [hi], edx
        mov [lo], eax
    }
    AllocGDT_(hi, lo, 0);
    _asm mov [sel], eax
    return (WORD)sel;
}

/* --------------------------------------------------------------- debugging */

static void OutE9(BYTE c);
#pragma aux OutE9 = "out 0E9h, al" parm [al];

DWORD dwRegsLin = 0, dwVramLin = 0, dwVramSize = 0;
DWORD dwRegsPhys = 0, dwVramPhys = 0;

static void dbg_ch(BYTE c)
{
    OutE9(c);
    if (dwRegsLin) *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_DEBUG) = c;
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

/* ---------------------------------------------------------- PCI, ring 0 */

static DWORD PciRead(DWORD addr);
#pragma aux PciRead =           \
    "mov    dx, 0CF8h"          \
    "out    dx, eax"            \
    "mov    dx, 0CFCh"          \
    "in     eax, dx"            \
    parm [eax] value [eax] modify [dx];

static void PciWrite(DWORD addr, DWORD val);
#pragma aux PciWrite =          \
    "mov    dx, 0CF8h"          \
    "out    dx, eax"            \
    "mov    dx, 0CFCh"          \
    "mov    eax, ebx"           \
    "out    dx, eax"            \
    parm [eax] [ebx] modify [dx eax];

#define CFG(dev, off) (0x80000000uL | ((DWORD)(dev) << 11) | ((off) & 0xfc))

/* Find the adapter and make sure it is decoding memory. Windows may have
 * stripped the base addresses already (doc 19 §11), in which case we put
 * back what we were told at the first sighting — the BIOS's assignment,
 * which nothing else is using. */
static BOOL AdapterClaim(void)
{
    DWORD dev, id, cmd;

    for (dev = 0; dev < 32; ++dev) {
        id = PciRead(CFG(dev, 0));
        if (id == ((DWORD)D3DPT_FB_PCI_DEVICE << 16 | D3DPT_FB_PCI_VENDOR))
            break;
    }
    if (dev == 32) { dbg_str("d3dptvxd: adapter not on the bus"); return FALSE; }

    if (!dwVramPhys) {
        dwVramPhys = PciRead(CFG(dev, 0x10)) & 0xfffffff0uL;
        dwRegsPhys = PciRead(CFG(dev, 0x14)) & 0xfffffff0uL;
        dbg_val("d3dptvxd: bar0", dwVramPhys);
        dbg_val("d3dptvxd: bar1", dwRegsPhys);
    }
    if (!dwVramPhys || !dwRegsPhys) {
        dbg_str("d3dptvxd: the adapter has no base addresses");
        return FALSE;
    }

    /* put them back if they have been taken away, and decode memory */
    if ((PciRead(CFG(dev, 0x10)) & 0xfffffff0uL) != dwVramPhys)
        PciWrite(CFG(dev, 0x10), dwVramPhys | 0x8);     /* prefetchable */
    if ((PciRead(CFG(dev, 0x14)) & 0xfffffff0uL) != dwRegsPhys)
        PciWrite(CFG(dev, 0x14), dwRegsPhys);
    cmd = PciRead(CFG(dev, 0x04));
    if (!(cmd & 0x2)) PciWrite(CFG(dev, 0x04), cmd | 0x2);
    return TRUE;
}

static BOOL AdapterMap(void)
{
    if (!AdapterClaim()) return FALSE;

    if (!dwRegsLin) {
        dwRegsLin = _MapPhysToLinear(dwRegsPhys, D3DPT_FB_REGS_SIZE, 0);
        if (dwRegsLin == 0xffffffffuL) { dwRegsLin = 0; return FALSE; }
    }
    if (*(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_MAGIC) != D3DPT_FB_MAGIC) {
        dbg_str("d3dptvxd: not our adapter after all");
        dwRegsLin = 0;
        return FALSE;
    }
    if (*(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_VERSION) != D3DPT_FB_VERSION) {
        dbg_str("d3dptvxd: register set mismatch, refusing the device");
        dwRegsLin = 0;
        return FALSE;
    }
    dwVramSize = *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_VRAM_SIZE);

    if (!dwVramLin) {
        dwVramLin = _MapPhysToLinear(dwVramPhys, dwVramSize, 0);
        if (dwVramLin == 0xffffffffuL) { dwVramLin = 0; return FALSE; }
    }
    dbg_val("d3dptvxd: vram", dwVramSize);
    return TRUE;
}

/* ------------------------------------------------- the mini-VDD dispatch */

static DWORD *DispatchTable = 0;
static DWORD DispatchTableLength = 0;

static void VDD_Get_Mini_Dispatch_Table(void)
{
    VxDCall(VDD, Get_Mini_Dispatch_Table);
    _asm mov [DispatchTable], edi
    _asm mov [DispatchTableLength], ecx
}

static WORD wVramSel = 0, wRegsSel = 0;

/*
 * REGISTER_DISPLAY_DRIVER (mini-VDD function 0), reached from the display
 * driver's VDD_REGISTER_DISPLAY_DRIVER_INFO. We answer with what the
 * driver cannot get for itself:
 *   EAX = the register page's selector
 *   EDX = VRAM's selector
 *   ECX = VRAM's size in bytes
 *   ESI = VRAM's ring-0 linear address (for whoever comes next: the
 *         DirectDraw heap and the ring-3 HAL both want it)
 */
static void __stdcall register_display_driver_proc(DWORD vm, PCRS_32 state)
{
    if (!AdapterMap()) {
        state->Client_EAX = 0;
        state->Client_EDX = 0;
        state->Client_ECX = 0;
        state->Client_ESI = 0;
        state->Client_EFlags |= 0x1;    /* carry: nothing to give */
        return;
    }

    if (!wRegsSel) wRegsSel = MakeSelector(dwRegsLin, D3DPT_FB_REGS_SIZE);
    if (!wVramSel) wVramSel = MakeSelector(dwVramLin, dwVramSize);

    dbg_val("d3dptvxd: regs sel", wRegsSel);
    dbg_val("d3dptvxd: vram sel", wVramSel);

    state->Client_EAX = wRegsSel;
    state->Client_EDX = wVramSel;
    state->Client_ECX = dwVramSize;
    state->Client_ESI = dwVramLin;
    state->Client_EFlags &= 0xfffffffeuL;
    dbg_str("d3dptvxd: display driver registered");
}

/* The main VDD calls a dispatch entry with EBX = VM, EBP = client
 * registers, and expects the flags left alone; this is the thunk that
 * turns that into a C call. */
static void __declspec(naked) register_display_driver_entry(void)
{
    _asm {
        pushad
        push ebp
        push ebx
        call register_display_driver_proc
        popad
        retn
    }
}

/* ------------------------------------------------------------ control */

void __stdcall Device_Init_proc(DWORD VM)
{
    dbg_str("d3dptvxd: Device_Init");

    if (!AdapterMap()) {
        dbg_str("d3dptvxd: no adapter, staying out of the way");
        return;
    }

    VDD_Get_Mini_Dispatch_Table();
    dbg_val("d3dptvxd: dispatch entries", DispatchTableLength);
    if (DispatchTable && DispatchTableLength >= 0x31)
        DispatchTable[VDD_REGISTER_DISPLAY_DRIVER] = (DWORD)register_display_driver_entry;
    else
        dbg_str("d3dptvxd: the VDD's dispatch table is not the shape we expect");

    dbg_str("d3dptvxd: ready");
}

/* Every control message: carry clear means "handled, no objection". */
void __declspec(naked) VXD_control(void)
{
    _asm {
        cmp eax, Sys_Critical_Init
        jz  ctl_ok
        cmp eax, Device_Init
        jz  ctl_init
        cmp eax, Sys_Dynamic_Device_Init
        jz  ctl_init
        jmp ctl_ok

      ctl_init:
        push ebx            /* the VM handle */
        call Device_Init_proc

      ctl_ok:
        clc
        ret
    }
}
