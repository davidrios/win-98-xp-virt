/*
 * modetest: what does the display driver let ChangeDisplaySettingsEx do?
 * Prints the current mode, the mode list, then tries the switches Wine's
 * ddraw/wined3d and DirectDraw itself make (fullscreen re-set of the
 * current mode, 640x480x16, 800x600x16/32, 1024x768x16) and prints the
 * DISP_CHANGE_* return code of each. Restores the desktop at the end.
 * Build: i686-w64-mingw32-gcc -O2 -o modetest.exe modetest.c -luser32
 */
#include <windows.h>
#include <stdio.h>

static const char *rc(LONG r)
{
    switch (r) {
    case DISP_CHANGE_SUCCESSFUL: return "SUCCESSFUL";
    case DISP_CHANGE_RESTART: return "RESTART";
    case DISP_CHANGE_FAILED: return "FAILED";
    case DISP_CHANGE_BADMODE: return "BADMODE";
    case DISP_CHANGE_NOTUPDATED: return "NOTUPDATED";
    case DISP_CHANGE_BADFLAGS: return "BADFLAGS";
    case DISP_CHANGE_BADPARAM: return "BADPARAM";
    case DISP_CHANGE_BADDUALVIEW: return "BADDUALVIEW";
    default: return "?";
    }
}

static void try_mode(const char *what, DWORD w, DWORD h, DWORD bpp, DWORD freq, DWORD flags)
{
    DEVMODEA dm;
    LONG r;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    if (freq) {
        dm.dmFields |= DM_DISPLAYFREQUENCY;
        dm.dmDisplayFrequency = freq;
    }
    r = ChangeDisplaySettingsExA(NULL, &dm, NULL, flags | CDS_TEST, NULL);
    printf("%-38s %lux%lux%lu@%lu flags %#lx: TEST %s", what, w, h, bpp, freq, flags, rc(r));
    if (r == DISP_CHANGE_SUCCESSFUL) {
        r = ChangeDisplaySettingsExA(NULL, &dm, NULL, flags, NULL);
        printf(", SET %s", rc(r));
        Sleep(500);
        ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);   /* back to the registry mode */
        Sleep(300);
    }
    printf("\n");
    fflush(stdout);
}

int main(void)
{
    DEVMODEA cur, m;
    int i, n = 0;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    printf("current: %lux%lux%lu@%lu, driver \"%s\"\n", cur.dmPelsWidth, cur.dmPelsHeight,
           cur.dmBitsPerPel, cur.dmDisplayFrequency, cur.dmDeviceName);
    {
        DISPLAY_DEVICEA dd;
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
            printf("device 0: %s (%s) flags %#lx\n", dd.DeviceString, dd.DeviceName, dd.StateFlags);
        }
    }
    printf("modes:");
    memset(&m, 0, sizeof(m));
    m.dmSize = sizeof(m);
    for (i = 0; EnumDisplaySettingsA(NULL, i, &m); i++) {
        printf(" %lux%lux%lu@%lu", m.dmPelsWidth, m.dmPelsHeight, m.dmBitsPerPel, m.dmDisplayFrequency);
        n++;
    }
    printf(" (%d)\n", n);
    fflush(stdout);
    try_mode("current, CDS_FULLSCREEN (wined3d)", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmBitsPerPel, 0, CDS_FULLSCREEN);
    try_mode("current, CDS_FULLSCREEN + freq", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmBitsPerPel, cur.dmDisplayFrequency, CDS_FULLSCREEN);
    try_mode("current, flags 0", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmBitsPerPel, 0, 0);
    try_mode("640x480x16, CDS_FULLSCREEN", 640, 480, 16, 0, CDS_FULLSCREEN);
    try_mode("640x480x32, CDS_FULLSCREEN", 640, 480, 32, 0, CDS_FULLSCREEN);
    try_mode("800x600x16, CDS_FULLSCREEN", 800, 600, 16, 0, CDS_FULLSCREEN);
    try_mode("800x600x32, CDS_FULLSCREEN", 800, 600, 32, 0, CDS_FULLSCREEN);
    try_mode("1024x768x16, CDS_FULLSCREEN", 1024, 768, 16, 0, CDS_FULLSCREEN);
    try_mode("1024x768x32, CDS_FULLSCREEN", 1024, 768, 32, 0, CDS_FULLSCREEN);
    printf("done\n");
    return 0;
}
