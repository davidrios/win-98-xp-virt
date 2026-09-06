/*
 * d3dpt9x.h — the Win98/Me display driver for the d3dpt-vga adapter
 * (doc 19, ADR-012 / M10): what the driver's own translation units share.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT9X_H
#define D3DPT9X_H

#define DRV_VERSION         0x0400      /* Windows 4.0 (Windows 95). */

/* Int 2Fh subfunctions the display driver uses. */
#define STOP_IO_TRAP        0x4000      /* stop trapping video I/O */

/* the mode the driver is running, filled from the registry / SYSTEM.INI */
extern WORD wScrX, wScrY, wBpp, wDpi, wPalettized;
extern WORD OurVMHandle;
extern DWORD VDDEntryPoint;

/* the adapter, as the mini-VDD hands it to us */
extern DWORD dwVramSize;
extern WORD  wRegsSel, wVramSel;        /* selectors onto the registers and VRAM */
extern DWORD dwPitch;                   /* bytes per line of the current mode */

extern LPDIBENGINE lpDriverPDevice;
extern WORD wEnabled;

int  PhysicalEnable(void);
void PhysicalDisable(void);
BOOL AdapterFind(void);
void ReadDisplayConfig(void);
void dbg_str(const char *s);
void dbg_val(const char *tag, DWORD v);

/* the adapter's registers, through wRegsSel */
DWORD RegGet(WORD off);
void  RegPut(WORD off, DWORD val);

#endif
