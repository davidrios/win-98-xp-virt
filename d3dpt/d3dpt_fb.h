/*
 * d3dpt_fb.h — the paravirtual framebuffer register set of the d3dpt-vga
 * display adapter (doc 15, ADR-008 / M7a).
 *
 * ONE header for both sides: the QEMU device model (d3dpt/hw/d3dpt_vga.c)
 * and the XP video miniport (guest-tools/src/d3dptvid/d3dptvid.c). Plain
 * C, fixed-width types only: the miniport is a kernel-mode PE with no
 * CRT and includes this next to video.h.
 *
 * The adapter is a PCI VGA (class 03.00, QEMU's standard VGA core with
 * the Bochs VBE ports, so SeaBIOS' stdvga ROM boots it and XP's inbox
 * vga.sys drives it until our driver is installed) plus one MMIO BAR of
 * paravirtual registers:
 *
 *   BAR 0  VRAM, prefetchable, D3DPT_FB_VRAM_MB (power of two)
 *   BAR 1  this register page, 4 KiB, 32-bit accesses
 *
 * The guest driver reads the host's mode table (MODE_COUNT, then MODE_SEL
 * + MODE_W/H/BPP/HZ per entry), programs a linear mode (W, H, BPP, PITCH,
 * OFFSET into VRAM) and sets ENABLE = 1; the VGA core is bypassed while
 * ENABLE is set and the console shows VRAM directly (no copy inside QEMU,
 * dirty pages from the memory log). ENABLE = 0 hands the console back to
 * the VGA core (text mode for BSODs / reboot via the BIOS).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_FB_H
#define D3DPT_FB_H

#include <stdint.h>

#define D3DPT_FB_VERSION      1u
#define D3DPT_FB_MAGIC        0x42463344u          /* "D3FB" at REG_MAGIC */

/* PCI identity: QEMU/Bochs pseudo vendor, our device id ("3D00"). The INF
 * matches PCI\VEN_1234&DEV_3D00. */
#define D3DPT_FB_PCI_VENDOR   0x1234u
#define D3DPT_FB_PCI_DEVICE   0x3d00u

#define D3DPT_FB_VRAM_MB      32u                  /* default BAR 0 size */
#define D3DPT_FB_REGS_SIZE    0x1000u

/* register page (byte offsets, 32-bit accesses) */
#define D3DPT_FB_REG_MAGIC       0x00u   /* R: D3DPT_FB_MAGIC */
#define D3DPT_FB_REG_VERSION     0x04u   /* R: D3DPT_FB_VERSION */
#define D3DPT_FB_REG_VRAM_SIZE   0x08u   /* R: bytes in BAR 0 */
#define D3DPT_FB_REG_CAPS        0x0cu   /* R: D3DPT_FB_CAP_* */
#define D3DPT_FB_REG_MODE_COUNT  0x10u   /* R: entries in the host mode table */
#define D3DPT_FB_REG_MODE_SEL    0x14u   /* RW: table index the MODE_* registers describe */
#define D3DPT_FB_REG_MODE_W      0x18u   /* R: width of the selected entry (0 = out of range) */
#define D3DPT_FB_REG_MODE_H      0x1cu   /* R: height */
#define D3DPT_FB_REG_MODE_BPP    0x20u   /* R: 16 or 32 */
#define D3DPT_FB_REG_MODE_HZ     0x24u   /* R: refresh the mode is advertised with */

#define D3DPT_FB_REG_ENABLE      0x40u   /* RW: 1 = linear mode below is shown, 0 = VGA core */
#define D3DPT_FB_REG_WIDTH       0x44u   /* RW: visible pixels per line */
#define D3DPT_FB_REG_HEIGHT      0x48u   /* RW: visible lines */
#define D3DPT_FB_REG_BPP         0x4cu   /* RW: 16 (r5g6b5) or 32 (x8r8g8b8) */
#define D3DPT_FB_REG_PITCH       0x50u   /* RW: bytes per line (0 = width * bpp / 8) */
#define D3DPT_FB_REG_OFFSET      0x54u   /* RW: byte offset of the first line in VRAM */
#define D3DPT_FB_REG_HZ          0x58u   /* RW: refresh the guest picked (informational) */

#define D3DPT_FB_REG_FRAMES      0x60u   /* R: console refreshes since ENABLE (vblank stand-in) */
#define D3DPT_FB_REG_DEBUG       0x70u   /* W: one character; lines go to the QEMU log */

#define D3DPT_FB_CAP_BPP16       0x1u
#define D3DPT_FB_CAP_BPP32       0x2u

#endif
