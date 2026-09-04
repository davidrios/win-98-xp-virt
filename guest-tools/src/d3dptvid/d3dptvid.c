/*
 * d3dptvid.c — the XP video miniport for the d3dpt-vga adapter (doc 15,
 * ADR-008 / M7a). Kernel mode, loaded by videoprt.sys; no CRT.
 *
 * The miniport is the part of an NT display driver that owns the hardware:
 * it finds the PCI device, maps its BARs, enumerates the modes and switches
 * them. Everything that draws lives in the display driver DLL
 * (d3dptdisp.c), which talks to us through the IOCTL_VIDEO_* requests
 * videoprt hands to HwStartIO.
 *
 * Hardware side: d3dpt/d3dpt_fb.h. The mode list is the host's table read
 * from the register BAR (so the player, not the driver, decides what XP
 * can pick); a mode switch programs WIDTH/HEIGHT/BPP/PITCH/OFFSET and
 * ENABLE, a reset clears ENABLE so the VGA core takes over again (the BIOS'
 * int10 mode 3 for BSODs and reboots is done by videoprt because
 * HwResetHw returns FALSE).
 *
 * Build: guest-tools/build-driver.sh (mingw-w64 i686, -nostdlib, native
 * subsystem, entry DriverEntry, libvideoprt + libntoskrnl).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* the video-miniport header set: ntdef.h for the base types, miniport.h
 * instead of ntddk.h (the two conflict), then the video port headers */
#include <ntdef.h>
#include <ddk/dderror.h>
#include <devioctl.h>
#include <ddk/miniport.h>
#include <ntddvdeo.h>
#include <ddk/video.h>
#include "../../../d3dpt/d3dpt_fb.h"

#define DEBUG_LOG 1

typedef struct _DEVICE_EXTENSION {
    volatile ULONG *regs;                 /* BAR 1 mapped (kernel VA) */
    PHYSICAL_ADDRESS regs_phys;
    ULONG regs_len;
    PHYSICAL_ADDRESS vram_phys;           /* BAR 0 */
    ULONG vram_len;
    PUCHAR vram;                          /* BAR 0 mapped (kernel VA), for clearing */
    ULONG num_modes;
    PVIDEO_MODE_INFORMATION modes;        /* num_modes entries */
    ULONG cur_mode;                       /* index into modes, or ~0 */
    BOOLEAN enabled;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

/* ------------------------------------------------------------ registers */

static ULONG reg_read(PDEVICE_EXTENSION d, ULONG off)
{
    return VideoPortReadRegisterUlong((PULONG)((PUCHAR)d->regs + off));
}

static void reg_write(PDEVICE_EXTENSION d, ULONG off, ULONG val)
{
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)d->regs + off), val);
}

/* debug text into the QEMU log through REG_DEBUG (one character per write) */
static void dbg_puts(PDEVICE_EXTENSION d, const char *s)
{
    if (!DEBUG_LOG || !d->regs) {
        return;
    }
    while (*s) {
        reg_write(d, D3DPT_FB_REG_DEBUG, (ULONG)(unsigned char)*s++);
    }
}

static void dbg_hex(PDEVICE_EXTENSION d, const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int i;

    if (!DEBUG_LOG || !d->regs) {
        return;
    }
    dbg_puts(d, tag);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(d, buf);
}

/* -------------------------------------------------------------- modes */

static ULONG bpp_pitch(ULONG w, ULONG bpp)
{
    /* 32-byte aligned lines keep every row cache-line aligned */
    return ((w * (bpp / 8)) + 31) & ~31u;
}

static VP_STATUS build_mode_table(PDEVICE_EXTENSION d)
{
    ULONG n, i, kept = 0;

    n = reg_read(d, D3DPT_FB_REG_MODE_COUNT);
    if (n == 0 || n > 256) {
        return ERROR_DEV_NOT_EXIST;
    }
    d->modes = VideoPortAllocatePool(d, VpPagedPool, n * sizeof(VIDEO_MODE_INFORMATION), 0x64336d64);
    if (!d->modes) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    VideoPortZeroMemory(d->modes, n * sizeof(VIDEO_MODE_INFORMATION));

    for (i = 0; i < n; i++) {
        ULONG w, h, bpp, hz, pitch;
        PVIDEO_MODE_INFORMATION m;

        reg_write(d, D3DPT_FB_REG_MODE_SEL, i);
        w = reg_read(d, D3DPT_FB_REG_MODE_W);
        h = reg_read(d, D3DPT_FB_REG_MODE_H);
        bpp = reg_read(d, D3DPT_FB_REG_MODE_BPP);
        hz = reg_read(d, D3DPT_FB_REG_MODE_HZ);
        if (!w || !h || (bpp != 16 && bpp != 32)) {
            continue;
        }
        pitch = bpp_pitch(w, bpp);
        if (pitch * h > d->vram_len) {
            continue;
        }
        m = &d->modes[kept];
        m->Length = sizeof(*m);
        m->ModeIndex = kept;
        m->VisScreenWidth = w;
        m->VisScreenHeight = h;
        m->ScreenStride = pitch;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = bpp;
        m->Frequency = hz;
        m->XMillimeter = 320;                  /* a 4:3 monitor of 16" diagonal */
        m->YMillimeter = 240;
        if (bpp == 32) {
            m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
            m->RedMask = 0x00ff0000; m->GreenMask = 0x0000ff00; m->BlueMask = 0x000000ff;
        } else {
            m->NumberRedBits = 5; m->NumberGreenBits = 6; m->NumberBlueBits = 5;
            m->RedMask = 0xf800; m->GreenMask = 0x07e0; m->BlueMask = 0x001f;
        }
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_NO_OFF_SCREEN;
        m->VideoMemoryBitmapWidth = w;
        m->VideoMemoryBitmapHeight = h;
        m->DriverSpecificAttributeFlags = 0;
        kept++;
    }
    d->num_modes = kept;
    dbg_hex(d, "modes: ", kept);
    dbg_puts(d, "\n");
    return kept ? NO_ERROR : ERROR_DEV_NOT_EXIST;
}

static VP_STATUS set_mode(PDEVICE_EXTENSION d, ULONG index, BOOLEAN zero)
{
    PVIDEO_MODE_INFORMATION m;

    if (index >= d->num_modes) {
        return ERROR_INVALID_PARAMETER;
    }
    m = &d->modes[index];
    reg_write(d, D3DPT_FB_REG_ENABLE, 0);
    /* what real adapters do: the new mode comes up black, not with the old
     * desktop bytes reinterpreted at the new pitch */
    if (zero && d->vram) {
        VideoPortZeroMemory(d->vram, m->ScreenStride * m->VisScreenHeight);
    }
    reg_write(d, D3DPT_FB_REG_WIDTH, m->VisScreenWidth);
    reg_write(d, D3DPT_FB_REG_HEIGHT, m->VisScreenHeight);
    reg_write(d, D3DPT_FB_REG_BPP, m->BitsPerPlane);
    reg_write(d, D3DPT_FB_REG_PITCH, m->ScreenStride);
    reg_write(d, D3DPT_FB_REG_OFFSET, 0);
    reg_write(d, D3DPT_FB_REG_HZ, m->Frequency);
    reg_write(d, D3DPT_FB_REG_ENABLE, 1);
    d->cur_mode = index;
    d->enabled = TRUE;
    return NO_ERROR;
}

static void reset_to_vga(PDEVICE_EXTENSION d)
{
    if (d->regs) {
        reg_write(d, D3DPT_FB_REG_ENABLE, 0);
    }
    d->enabled = FALSE;
}

/* ------------------------------------------------------- miniport entry */

static VP_STATUS NTAPI HwFindAdapter(PVOID ext, PVOID ctx, PWSTR args,
                                     PVIDEO_PORT_CONFIG_INFO cfg, PUCHAR again)
{
    PDEVICE_EXTENSION d = ext;
    VIDEO_ACCESS_RANGE ranges[2];
    USHORT vendor = D3DPT_FB_PCI_VENDOR, device = D3DPT_FB_PCI_DEVICE;
    ULONG slot = 0;
    VP_STATUS st;
    ULONG magic;

    if (cfg->Length < sizeof(VIDEO_PORT_CONFIG_INFO)) {
        return ERROR_INVALID_PARAMETER;
    }
    VideoPortZeroMemory(ranges, sizeof(ranges));
    /* PnP: videoprt already knows the device; this fetches its BARs */
    st = VideoPortGetAccessRanges(d, 0, NULL, 2, ranges, &vendor, &device, &slot);
    if (st != NO_ERROR) {
        VideoPortDebugPrint(Error, "d3dptvid: GetAccessRanges %x\n", st);
        return ERROR_DEV_NOT_EXIST;
    }
    if (ranges[0].RangeLength == 0 || ranges[1].RangeLength < D3DPT_FB_REGS_SIZE ||
        ranges[0].RangeInIoSpace || ranges[1].RangeInIoSpace) {
        return ERROR_DEV_NOT_EXIST;
    }
    d->vram_phys = ranges[0].RangeStart;
    d->vram_len = ranges[0].RangeLength;
    d->regs_phys = ranges[1].RangeStart;
    d->regs_len = D3DPT_FB_REGS_SIZE;

    d->regs = VideoPortGetDeviceBase(d, d->regs_phys, d->regs_len, VIDEO_MEMORY_SPACE_MEMORY);
    if (!d->regs) {
        return ERROR_DEV_NOT_EXIST;
    }
    magic = reg_read(d, D3DPT_FB_REG_MAGIC);
    if (magic != D3DPT_FB_MAGIC || reg_read(d, D3DPT_FB_REG_VERSION) != D3DPT_FB_VERSION) {
        VideoPortDebugPrint(Error, "d3dptvid: bad magic %x\n", magic);
        VideoPortFreeDeviceBase(d, (PVOID)d->regs);
        d->regs = NULL;
        return ERROR_DEV_NOT_EXIST;
    }
    /* the whole of VRAM in kernel space: mode-set clearing now, the
     * DirectDraw heap later. 32 MiB of system PTEs is what any real
     * adapter's miniport takes. */
    d->vram = VideoPortGetDeviceBase(d, d->vram_phys, d->vram_len, VIDEO_MEMORY_SPACE_MEMORY);
    dbg_puts(d, "d3dptvid: adapter found\n");
    dbg_hex(d, "  vram ", d->vram_phys.LowPart);
    dbg_hex(d, " len ", d->vram_len);
    dbg_hex(d, " regs ", d->regs_phys.LowPart);
    dbg_puts(d, "\n");

    st = build_mode_table(d);
    if (st != NO_ERROR) {
        return st;
    }
    d->cur_mode = ~0u;

    /* what the display driver sees in the registry; the chip strings are
     * cosmetic (Display Properties' Adapter tab) */
    {
        static WCHAR chip[] = L"win98-xp-virt d3dpt-vga";
        static WCHAR dac[] = L"paravirtual";
        static WCHAR adapter[] = L"d3dpt-vga paravirtual framebuffer";
        ULONG mem = d->vram_len;
        VideoPortSetRegistryParameters(d, L"HardwareInformation.ChipType", chip, sizeof(chip));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.DacType", dac, sizeof(dac));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.AdapterString", adapter, sizeof(adapter));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.MemorySize", &mem, sizeof(mem));
    }

    cfg->NumEmulatorAccessEntries = 0;
    cfg->EmulatorAccessEntries = NULL;
    cfg->EmulatorAccessEntriesContext = 0;
    cfg->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    cfg->VdmPhysicalVideoMemoryLength = 0;
    cfg->HardwareStateSize = 0;
    *again = 0;
    return NO_ERROR;
}

static BOOLEAN NTAPI HwInitialize(PVOID ext)
{
    return TRUE;
}

static BOOLEAN NTAPI HwResetHw(PVOID ext, ULONG columns, ULONG rows)
{
    /* back to the VGA core; FALSE = videoprt does the int10 mode 3 */
    reset_to_vga(ext);
    return FALSE;
}

static VP_STATUS NTAPI HwGetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    return NO_ERROR;
}

static VP_STATUS NTAPI HwSetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    return NO_ERROR;
}

static VP_STATUS NTAPI HwGetVideoChildDescriptor(PVOID ext, PVIDEO_CHILD_ENUM_INFO info,
                                             PVIDEO_CHILD_TYPE type, PUCHAR desc,
                                             PULONG uid, PULONG unused)
{
    /* no children: XP attaches its default monitor, mode picker uses our list */
    return VIDEO_ENUM_NO_MORE_DEVICES;
}

static BOOLEAN NTAPI HwStartIO(PVOID ext, PVIDEO_REQUEST_PACKET rp)
{
    PDEVICE_EXTENSION d = ext;
    VP_STATUS st = NO_ERROR;

    rp->StatusBlock->Information = 0;

    switch (rp->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        PVIDEO_NUM_MODES nm = rp->OutputBuffer;
        if (rp->OutputBufferLength < sizeof(*nm)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        nm->NumModes = d->num_modes;
        nm->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        rp->StatusBlock->Information = sizeof(*nm);
        break;
    }
    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        ULONG need = d->num_modes * sizeof(VIDEO_MODE_INFORMATION);
        if (rp->OutputBufferLength < need) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortMoveMemory(rp->OutputBuffer, d->modes, need);
        rp->StatusBlock->Information = need;
        break;
    }
    case IOCTL_VIDEO_QUERY_CURRENT_MODE: {
        if (rp->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (d->cur_mode >= d->num_modes) {
            st = ERROR_INVALID_FUNCTION;
            break;
        }
        VideoPortMoveMemory(rp->OutputBuffer, &d->modes[d->cur_mode], sizeof(VIDEO_MODE_INFORMATION));
        rp->StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
        break;
    }
    case IOCTL_VIDEO_SET_CURRENT_MODE: {
        PVIDEO_MODE vm = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*vm)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        dbg_hex(d, "d3dptvid: set mode ", vm->RequestedMode);
        dbg_puts(d, "\n");
        st = set_mode(d, vm->RequestedMode & ~(VIDEO_MODE_NO_ZERO_MEMORY | VIDEO_MODE_MAP_MEM_LINEAR),
                      !(vm->RequestedMode & VIDEO_MODE_NO_ZERO_MEMORY));
        break;
    }
    case IOCTL_VIDEO_RESET_DEVICE:
        dbg_puts(d, "d3dptvid: reset device\n");
        reset_to_vga(d);
        break;

    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        PVIDEO_MEMORY in = rp->InputBuffer;
        PVIDEO_MEMORY_INFORMATION out = rp->OutputBuffer;
        ULONG len, space = VIDEO_MEMORY_SPACE_MEMORY;
        if (rp->InputBufferLength < sizeof(*in) || rp->OutputBufferLength < sizeof(*out)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        len = d->vram_len;
        out->VideoRamBase = in->RequestedVirtualAddress;
        st = VideoPortMapMemory(d, d->vram_phys, &len, &space, &out->VideoRamBase);
        if (st != NO_ERROR) {
            break;
        }
        out->VideoRamLength = len;
        out->FrameBufferBase = out->VideoRamBase;
        out->FrameBufferLength = len;
        rp->StatusBlock->Information = sizeof(*out);
        break;
    }
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY: {
        PVIDEO_MEMORY in = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*in)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        st = VideoPortUnmapMemory(d, in->RequestedVirtualAddress, NULL);
        break;
    }
    case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES: {
        /* the register page for the display driver (debug log now, the
         * DirectDraw/Direct3D DDI records later) */
        PVIDEO_PUBLIC_ACCESS_RANGES out = rp->OutputBuffer;
        ULONG len = d->regs_len, space = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_USER_MODE;
        if (rp->OutputBufferLength < sizeof(*out)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        out->InIoSpace = FALSE;
        out->MappedInIoSpace = FALSE;
        out->VirtualAddress = NULL;
        st = VideoPortMapMemory(d, d->regs_phys, &len, &space, &out->VirtualAddress);
        if (st == NO_ERROR) {
            rp->StatusBlock->Information = sizeof(*out);
        }
        break;
    }
    case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES: {
        PVIDEO_MEMORY in = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*in)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        st = VideoPortUnmapMemory(d, in->RequestedVirtualAddress, NULL);
        break;
    }
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY:
        /* DirectDraw's HAL path: M7b */
        st = ERROR_INVALID_FUNCTION;
        break;
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        /* no palettized modes */
        st = ERROR_INVALID_FUNCTION;
        break;
    default:
        st = ERROR_INVALID_FUNCTION;
        break;
    }

    rp->StatusBlock->Status = st;
    return TRUE;
}

ULONG NTAPI DriverEntry(PVOID ctx1, PVOID ctx2)
{
    VIDEO_HW_INITIALIZATION_DATA init;

    VideoPortZeroMemory(&init, sizeof(init));
    init.HwInitDataSize = sizeof(init);
    init.AdapterInterfaceType = PCIBus;
    init.HwFindAdapter = HwFindAdapter;
    init.HwInitialize = HwInitialize;
    init.HwStartIO = HwStartIO;
    init.HwResetHw = HwResetHw;
    init.HwGetPowerState = HwGetPowerState;
    init.HwSetPowerState = HwSetPowerState;
    init.HwGetVideoChildDescriptor = HwGetVideoChildDescriptor;
    init.HwDeviceExtensionSize = sizeof(DEVICE_EXTENSION);
    return VideoPortInitialize(ctx1, ctx2, &init, NULL);
}
