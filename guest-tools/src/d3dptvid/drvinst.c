/*
 * drvinst.c — installs the d3dpt-vga display driver into a 2000/XP guest
 * from a script: what `devcon update` does, without the DDK.
 *
 *   DRVINST.EXE [path\d3dptvid.inf] [-reboot]
 *
 * Sets the driver-signing policy to "ignore" (the driver is unsigned; XP
 * would otherwise stop at the Logo dialog), then calls newdev's
 * UpdateDriverForPlugAndPlayDevices for PCI\VEN_1234&DEV_3D00 with
 * INSTALLFLAG_FORCE so the INF replaces the inbox VGA binding. Prints
 * the outcome and, with -reboot, restarts Windows when SetupAPI asks for
 * it (the boot VGA driver holds the adapter until then). Run as
 * Administrator. Exit code 0 = installed, 2 = installed + reboot needed
 * (without -reboot), 1 = failed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define HWID "PCI\\VEN_1234&DEV_3D00"
#define INSTALLFLAG_FORCE 0x00000001

typedef BOOL (WINAPI *pfn_update)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

static void set_signing_policy(void)
{
    /* per-machine and per-user: 0 = ignore, 1 = warn, 2 = block */
    static const struct { HKEY root; const char *path; } keys[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Driver Signing" },
        { HKEY_CURRENT_USER,  "Software\\Microsoft\\Driver Signing" },
    };
    BYTE zero = 0;
    DWORD dzero = 0;
    HKEY k;
    int i;
    for (i = 0; i < 2; i++) {
        if (RegCreateKeyExA(keys[i].root, keys[i].path, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == 0) {
            RegSetValueExA(k, "Policy", 0, REG_BINARY, &zero, 1);
            RegCloseKey(k);
        }
    }
    /* the Group Policy setting overrides both when present; XP SP2+ consults
     * it first, and the Logo dialog still appeared with only the two above */
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows NT\\Driver Signing",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == 0) {
        RegSetValueExA(k, "BehaviorOnFailedVerify", 0, REG_DWORD, (BYTE *)&dzero, sizeof(dzero));
        RegCloseKey(k);
    }
}

/* XP SP3 shows the "has not passed Windows Logo testing" dialog no matter
 * what the registry policy says (the value is hash-protected; only the
 * Control Panel can change it). setupapi creates that dialog inside our
 * own process, on the thread blocked in UpdateDriverForPlugAndPlayDevices,
 * so a second thread can find it (a #32770 dialog of this process) and
 * press its "Continue Anyway" button, which is the first push button in
 * the template in every language. What commercial installers do. */
static DWORD g_pid;
static HWND g_button;

static BOOL CALLBACK find_button(HWND h, LPARAM lp)
{
    char cls[32];
    LONG style;
    if (!GetClassNameA(h, cls, sizeof(cls)) || lstrcmpiA(cls, "Button") != 0) return TRUE;
    style = GetWindowLongA(h, GWL_STYLE) & 0xF;
    if (style != BS_PUSHBUTTON && style != BS_DEFPUSHBUTTON) return TRUE;
    g_button = h;
    return FALSE;
}

static BOOL CALLBACK find_dialog(HWND h, LPARAM lp)
{
    char cls[32];
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid || !IsWindowVisible(h)) return TRUE;
    if (!GetClassNameA(h, cls, sizeof(cls)) || strcmp(cls, "#32770") != 0) return TRUE;
    g_button = NULL;
    EnumChildWindows(h, find_button, 0);
    if (g_button) {
        printf("drvinst: pressing the Logo dialog's first button\n");
        fflush(stdout);
        PostMessageA(g_button, BM_CLICK, 0, 0);
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI logo_watcher(LPVOID arg)
{
    int i;
    g_pid = GetCurrentProcessId();
    for (i = 0; i < 1200; i++) {          /* up to two minutes */
        Sleep(100);
        EnumWindows(find_dialog, 0);
    }
    return 0;
}

static void reboot(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
}

int main(int argc, char **argv)
{
    char inf[MAX_PATH], full[MAX_PATH];
    const char *arg_inf = NULL;
    BOOL want_reboot = FALSE, need_reboot = FALSE;
    HMODULE newdev;
    pfn_update update;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-reboot")) want_reboot = TRUE;
        else arg_inf = argv[i];
    }
    if (!arg_inf) {
        /* next to the EXE */
        char *slash;
        GetModuleFileNameA(NULL, inf, sizeof(inf));
        slash = strrchr(inf, '\\');
        if (slash) slash[1] = 0;
        strcat(inf, "d3dptvid.inf");
        arg_inf = inf;
    }
    if (!GetFullPathNameA(arg_inf, sizeof(full), full, NULL)) {
        printf("drvinst: bad path %s\n", arg_inf);
        return 1;
    }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        printf("drvinst: %s not found\n", full);
        return 1;
    }

    newdev = LoadLibraryA("newdev.dll");
    update = newdev ? (pfn_update)GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    if (!update) {
        printf("drvinst: newdev.dll/UpdateDriverForPlugAndPlayDevicesA missing (2000/XP only)\n");
        return 1;
    }

    set_signing_policy();
    CloseHandle(CreateThread(NULL, 0, logo_watcher, NULL, 0, NULL));
    printf("drvinst: installing %s for %s\n", full, HWID);
    fflush(stdout);
    if (!update(NULL, HWID, full, INSTALLFLAG_FORCE, &need_reboot)) {
        DWORD e = GetLastError();
        printf("drvinst: failed, error %lu (0x%lx)%s\n", e, e,
               e == 0xE000020B /* ERROR_NO_SUCH_DEVINST */ ? " - device not present (-device d3dpt-vga?)" :
               e == ERROR_NO_MORE_ITEMS ? " - no better driver / INF does not match" : "");
        return 1;
    }
    printf("drvinst: installed%s\n", need_reboot ? ", reboot required" : "");
    if (need_reboot && want_reboot) {
        printf("drvinst: rebooting\n");
        reboot();
        return 0;
    }
    return need_reboot ? 2 : 0;
}
