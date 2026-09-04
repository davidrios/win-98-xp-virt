/*
 * setmode.c — switch the XP desktop mode from a script (doc 15 tests).
 *
 *   SETMODE.EXE                      lists the modes the driver offers
 *   SETMODE.EXE <w> <h> <bpp> [hz]   switches (CDS_UPDATEREGISTRY: sticks
 *                                    across reboots) and prints the result
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    DEVMODEA dm;
    int i;

    if (argc < 4) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
            printf("%2d: %4lux%-4lu %2lu bpp %3lu Hz\n", i, dm.dmPelsWidth, dm.dmPelsHeight,
                   dm.dmBitsPerPel, dm.dmDisplayFrequency);
        }
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
            printf("current: %lux%lu %lu bpp %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight,
                   dm.dmBitsPerPel, dm.dmDisplayFrequency);
        }
        return 0;
    }
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = atoi(argv[1]);
    dm.dmPelsHeight = atoi(argv[2]);
    dm.dmBitsPerPel = atoi(argv[3]);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (argc > 4) {
        dm.dmDisplayFrequency = atoi(argv[4]);
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    i = ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
    printf("ChangeDisplaySettings(%lux%lu %lu bpp %lu Hz) = %d (%s)\n", dm.dmPelsWidth,
           dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency, i,
           i == DISP_CHANGE_SUCCESSFUL ? "ok" : i == DISP_CHANGE_BADMODE ? "bad mode" :
           i == DISP_CHANGE_RESTART ? "restart needed" : i == DISP_CHANGE_FAILED ? "failed" : "?");
    return i == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}
