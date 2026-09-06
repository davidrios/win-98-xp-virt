/*
 * The display driver's `config.bin` resource (oembin 1). Every Windows 9x
 * display driver carries it: GDI and USER read the machine-dependent
 * metrics and the default system colours out of the driver itself, and a
 * driver without it is not usable as a display driver at all (doc 19).
 * The layout is the Windows 3.1 DDK's; the colours are the standard
 * Windows scheme, which is what a modern desktop overrides anyway.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include <gdidefs.h>

typedef struct {
    short VertThumHeight;   /* vertical scrollbar thumb, pixels */
    short HorizThumWidth;
    short IconXRatio;
    short IconYRatio;
    short CurXRatio;
    short CurYRatio;
    short Reserved;
    short XBorder;
    short YBorder;

    RGBQUAD clrScrollbar;
    RGBQUAD clrDesktop;
    RGBQUAD clrActiveCaption;
    RGBQUAD clrInactiveCaption;
    RGBQUAD clrMenu;
    RGBQUAD clrWindow;
    RGBQUAD clrWindowFrame;
    RGBQUAD clrMenuText;
    RGBQUAD clrWindowText;
    RGBQUAD clrCaptionText;
    RGBQUAD clrActiveBorder;
    RGBQUAD clrInactiveBorder;
    RGBQUAD clrAppWorkspace;
    RGBQUAD clrHiliteBk;
    RGBQUAD clrHiliteText;
    RGBQUAD clrBtnFace;
    RGBQUAD clrBtnShadow;
    RGBQUAD clrGrayText;
    RGBQUAD clrBtnText;
    RGBQUAD clrInactiveCaptionText;
} CONFIG_BIN;

/* blue, green, red, reserved — RGBQUAD's order */
CONFIG_BIN Config = {
    17, 17, 2, 2, 1, 1, 0, 1, 1,

    192, 192, 192, 0,       /* scrollbar */
    192, 192, 192, 0,       /* desktop */
      0,   0, 128, 0,       /* active caption */
    255, 255, 255, 0,       /* inactive caption */
    255, 255, 255, 0,       /* menu */
    255, 255, 255, 0,       /* window */
      0,   0,   0, 0,       /* window frame */
      0,   0,   0, 0,       /* menu text */
      0,   0,   0, 0,       /* window text */
    255, 255, 255, 0,       /* caption text */
    192, 192, 192, 0,       /* active border */
    192, 192, 192, 0,       /* inactive border */
    255, 255, 255, 0,       /* app workspace */
      0,   0, 128, 0,       /* highlight */
    255, 255, 255, 0,       /* highlight text */
    192, 192, 192, 0,       /* button face */
    128, 128, 128, 0,       /* button shadow */
    192, 192, 192, 0,       /* grey text */
      0,   0,   0, 0,       /* button text */
      0,   0,   0, 0        /* inactive caption text */
};
