/*
 * d3dpt9v.h — what the Win98/Me mini-VDD and the display driver agree on
 * (doc 19, M10).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT9V_H
#define D3DPT9V_H

/* Our VxD's identity. The device ID is what int 2Fh AX=1684h looks up;
 * 0x4334 is in the range the RBIL lists as unassigned, one past the one
 * vmdisp9x took, so both can be installed on the same machine. */
#define D3DPT_VXD_ID     0x4334
#define D3DPT_VXD_NAME   "D3DPTVXD"
#define D3DPT_VXD_MAJOR  4      /* 4 for Windows 95 and newer */
#define D3DPT_VXD_MINOR  0

/* The mini-VDD function the display driver reaches through the main VDD's
 * VDD_REGISTER_DISPLAY_DRIVER_INFO, and the main VDD service that hands
 * out the dispatch table it lives in. */
#define VDD_REGISTER_DISPLAY_DRIVER 0
#define VDD__Get_Mini_Dispatch_Table 14

/* What the display driver calls on the main VDD to reach the function
 * above: minivdd.h's VDD_REGISTER_DISPLAY_DRIVER_INFO. Repeated here so
 * the two halves cannot drift. The answer is
 *   EAX = selector onto the register page   ECX = VRAM bytes
 *   EDX = selector onto VRAM                ESI = VRAM's ring-0 linear address
 * and carry set means the adapter is not usable. */
#define D3DPT_VDD_REGISTER_INFO 0x83

/* The selectors handed back are DPL 3: the display driver is ring-3 code
 * and the DIB Engine draws through them. */
#define D3DPT_SEL_TYPE 0xf2

#endif
