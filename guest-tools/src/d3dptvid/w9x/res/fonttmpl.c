/*
 * The three LOGFONTs a Windows 9x display driver's font resource holds:
 * the OEM fixed font (whose size is the DPI-dependent part), and the ANSI
 * fixed and variable fonts. Included twice, once per DPI (fonts.c,
 * fonts120.c). Windows 3.1 DDK layout.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#define LF_FACESIZE             /* gdidefs.h then gives a variable-size LOGFONT */
#include <gdidefs.h>

LOGFONT OEMFixed = {
    OEM_FNT_HEIGHT, OEM_FNT_WIDTH,
    0, 0, 0, 0, 0, 0,
    255,            /* OEM character set */
    0, 2, 2, 1,     /* out precision, clip precision, quality, pitch */
    "Terminal"
};

LOGFONT ANSIFixed = {
    12, 9,
    0, 0, 0, 0, 0, 0,
    0,              /* ANSI character set */
    0, 2, 2, 1,
    "Courier"
};

LOGFONT ANSIVar = {
    12, 9,
    0, 0, 0, 0, 0, 0,
    0,
    0, 2, 2, 2,     /* variable pitch */
    "Helv"
};
