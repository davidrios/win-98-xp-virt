/*
 * setup.c — SETUP.EXE, the guest-tools installer, run inside the machine.
 *
 * The ISO used to be a pile of folders and a README telling you which
 * files to copy where for your Windows; the copies differ per family and
 * one of them (FXPTL.SYS) needs a service registered before OPENGL32.DLL
 * will even load. This does it: the components that apply to *this*
 * Windows, and a log of what actually happened.
 *
 * Guest tools come in two kinds and so does this program:
 *
 *   - things installed into Windows (system files, a driver, a service) —
 *     the numbered list and `I`;
 *   - things copied next to one game's EXE (our D3D DLLs, the GL wrapper,
 *     WineD3D) — `G`. Those are per-game by design, never system-wide, so
 *     an installer with only an install step would leave out half the ISO.
 *
 * A console program on purpose: it is the one interface Windows 98, XP
 * and a rescue command prompt all have, it needs no common controls, and
 * every step is scriptable — `SETUP /ALL` in a batch file is how our own
 * headless guest tests install the tools.
 *
 * Everything it installs comes from the folder SETUP.EXE is in, so it
 * works from the CD, from a copy on the hard disk, or from a share.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <winsvc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define MAX_COMPONENTS 8

/* Every path here is <the SETUP.EXE folder> + <folder> + <name>, so the
 * buffers are deliberately wider than MAX_PATH: a root path close to the
 * limit plus a subfolder is longer than MAX_PATH, and silently truncating
 * it would look for the wrong file. */
#define PATHBUF (MAX_PATH * 2)

static char g_root[MAX_PATH];    /* the folder SETUP.EXE lives in */
static char g_sys[MAX_PATH];     /* WINDOWS\SYSTEM or WINDOWS\system32 */
static char g_win[MAX_PATH];     /* WINDOWS */
static int g_nt;                 /* 2000/XP rather than 98/Me */
static int g_reboot;             /* a step said the machine must restart */
static FILE *g_log;

/* `rel` under the SETUP.EXE folder, in a caller-provided buffer. */
static const char *iso(char *buf, const char *rel)
{
    snprintf(buf, PATHBUF, "%s%s", g_root, rel);
    return buf;
}

/* Everything the user sees also goes to the log file: a console window
 * scrolls, and on a failed install the log is what gets pasted into a bug
 * report. Line-flushed, so a crash keeps the tail. */
static void say(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

/* ------------------------------------------------------------ file steps */

/* Copy one file and say so. A missing source is worth naming: it means
 * this ISO was built without that piece (no mingw DDK, say), not that the
 * user did anything wrong. */
static int copy_one(const char *src, const char *dstdir, const char *name)
{
    char dst[PATHBUF];

    snprintf(dst, sizeof dst, "%s\\%s", dstdir, name);
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
        say("    %s: not on this disc", name);
        return 1;
    }
    /* a read-only copy from a previous install would refuse to be replaced */
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileA(src, dst, FALSE)) {
        say("    %s: copy failed, error %lu", name, (unsigned long)GetLastError());
        return 1;
    }
    say("    %s -> %s", name, dstdir);
    return 0;
}

/* Copy `names` (NULL-terminated) from an ISO folder into `dstdir`. */
static int copy_set(const char *isodir, const char *dstdir, const char *const *names)
{
    char src[PATHBUF];
    int bad = 0, i;

    for (i = 0; names[i]; i++) {
        snprintf(src, sizeof src, "%s%s\\%s", g_root, isodir, names[i]);
        bad |= copy_one(src, dstdir, names[i]);
    }
    return bad;
}

/* Every file in an ISO folder, for the test programs. */
static int copy_folder(const char *isodir, const char *dstdir)
{
    WIN32_FIND_DATAA fd;
    char pat[PATHBUF], src[PATHBUF + MAX_PATH];
    HANDLE h;
    int bad = 0;

    CreateDirectoryA(dstdir, NULL);
    snprintf(pat, sizeof pat, "%s%s\\*.*", g_root, isodir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        say("    %s: not on this disc", isodir);
        return 1;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(src, sizeof src, "%s%s\\%s", g_root, isodir, fd.cFileName);
        bad |= copy_one(src, dstdir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return bad;
}

/* Run a program from the ISO and put its output in our log. Returns its
 * exit code, or -1 if it could not be started. Its stdout is read through
 * a pipe rather than left on our console so that the lines land in the
 * log file too, and in order. */
static int run_logged(const char *cmdline)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    char buf[512], line[512], cmd[PATHBUF * 2];
    DWORD n, code = 0, i;
    int len = 0;

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    lstrcpynA(cmd, cmdline, sizeof cmd);   /* CreateProcess may write to the command line */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, g_root, &si, &pi)) {
        say("    cannot run %s (error %lu)", cmdline, (unsigned long)GetLastError());
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);        /* our end, or the read below never sees EOF */
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == (int)sizeof line - 1) {
                line[len] = 0;
                if (len && line[len - 1] == '\r') line[len - 1] = 0;
                if (line[0]) say("    %s", line);
                len = 0;
            } else {
                line[len++] = buf[i];
            }
        }
    }
    if (len) { line[len] = 0; say("    %s", line); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* ------------------------------------------------------------ components */

/* Glide and the device mapper. The mapper is the part that matters even
 * to someone who never runs a Glide game: OPENGL32.DLL and our D3D DLLs
 * reach the device through it, and without it they refuse to load
 * (0xc0000142 on NT). 9x has it as a VxD that only needs to be in
 * SYSTEM; NT as a kernel driver a service must point at. */
static int step_glide(void)
{
    static const char *const dlls[] = { "GLIDE.DLL", "GLIDE2X.DLL", "GLIDE3X.DLL", NULL };
    static const char *const vxd[] = { "FXMEMMAP.VXD", NULL };
    static const char *const sys[] = { "FXPTL.SYS", NULL };
    char drivers[PATHBUF], cmd[PATHBUF * 2], path[PATHBUF];
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    int bad, running;

    say("Glide and the device mapper:");
    bad = copy_set("GLIDE", g_sys, dlls);
    if (!g_nt) return bad | copy_set("GLIDE", g_sys, vxd);

    snprintf(drivers, sizeof drivers, "%s\\drivers", g_sys);
    bad |= copy_set("GLIDE", drivers, sys);
    if (bad) return bad;
    /* INSTDRV registers the MAPMEM service on FXPTL.SYS and starts it. Its
     * exit code also covers a 3dfx-specific probe that has nothing to do
     * with us, so the service itself is what we check afterwards. */
    snprintf(cmd, sizeof cmd, "\"%s\"", iso(path, "GLIDE\\INSTDRV.EXE"));
    run_logged(cmd);
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    svc = scm ? OpenServiceA(scm, "MAPMEM", SERVICE_QUERY_STATUS) : NULL;
    running = svc && QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    say("    MAPMEM service: %s", running ? "running" : "NOT running (are you an Administrator?)");
    return running ? 0 : 1;
}

/* The display adapter driver, 2000/XP only. DRVINST.EXE is the tested
 * installer (signing policy, the Logo dialog, UpdateDriverForPlugAndPlay)
 * and says 2 when the machine has to restart before the driver can take
 * the adapter over from the boot VGA one. */
static int step_driver(void)
{
    char cmd[PATHBUF * 2 + 8], inf[PATHBUF], exe[PATHBUF];
    int rc;

    say("Display adapter driver (d3dpt-vga):");
    iso(exe, "DRIVER\\DRVINST.EXE");
    iso(inf, "DRIVER\\D3DPTVID.INF");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        say("    not on this disc");
        return 1;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, inf);
    rc = run_logged(cmd);
    if (rc == 2) { g_reboot = 1; return 0; }
    if (rc != 0) {
        say("    failed. The machine must run with -vga none -device d3dpt-vga.");
        return 1;
    }
    return 0;
}

/* CDSHELF.EXE into the Windows directory: it is a thing you want to reach
 * by name from a Run box or a batch file mid-game, and %windir% is on the
 * search path of both families. */
static int step_cdshelf(void)
{
    static const char *const exe[] = { "CDSHELF.EXE", NULL };

    say("Disc shelf tool:");
    return copy_set("CDSHELF", g_win, exe);
}

static int step_tests(void)
{
    say("Test programs:");
    return copy_folder("TESTS", "C:\\2KSBOX");
}

typedef struct {
    const char *label;
    const char *note;
    int on_9x, on_nt, def;
    int (*run)(void);
    int on;
} Component;

static Component g_comp[MAX_COMPONENTS] = {
    { "Display adapter driver (d3dpt-vga)", "needs a restart",              0, 1, 1, step_driver,  0 },
    { "Glide and the device mapper",        "also needed by OPENGL32.DLL",  1, 1, 1, step_glide,   0 },
    { "Disc shelf tool",                    "CDSHELF.EXE in the Windows folder", 1, 1, 1, step_cdshelf, 0 },
    { "Test programs",                      "in C:\\2KSBOX",                1, 1, 0, step_tests,   0 },
};
static int g_ncomp;

/* ------------------------------------------------- the per-game file sets */

/* Copied next to one game's EXE, never into the system directory. Each
 * set is self-contained — what a game needs to run on that stack and
 * nothing else — so two stacks can never end up in one folder. The files
 * are pairs, <name on the ISO> then <name it must have next to the EXE>,
 * which is how WineD3D's DLLs get their Microsoft names without the disc
 * carrying a second copy of them under those names. */
typedef struct {
    const char *label;
    const char *dir;
    const char *files[8];
} GameSet;

static const GameSet g_sets[] = {
    { "Direct3D 8/9 on the paravirtual device (D3D8.DLL D3D9.DLL DDRAW.DLL)",
      "D3DPT",   { "D3D8.DLL", "D3D8.DLL", "D3D9.DLL", "D3D9.DLL", "DDRAW.DLL", "DDRAW.DLL", NULL } },
    { "DirectInput keyboard fix (DINPUT.DLL)",
      "D3DPT",   { "DINPUT.DLL", "DINPUT.DLL", NULL } },
    { "OpenGL pass-through (OPENGL32.DLL)",
      "OPENGL",  { "OPENGL32.DLL", "OPENGL32.DLL", NULL } },
    { "WineD3D, Direct3D 8/9 (D3D8.DLL D3D9.DLL WINED3D.DLL)",
      "WINED3D", { "WINED8.DLL", "D3D8.DLL", "WINED9.DLL", "D3D9.DLL", "WINED3D.DLL", "WINED3D.DLL", NULL } },
    { "WineD3D, DirectDraw and Direct3D up to 7 (DDRAW.DLL WINED3D.DLL)",
      "WINED3D", { "WINEDD.DLL", "DDRAW.DLL", "WINED3D.DLL", "WINED3D.DLL", NULL } },
};
#define NSETS ((int)(sizeof g_sets / sizeof g_sets[0]))

static int copy_game_set(int n, const char *dir)
{
    const GameSet *s;
    char src[PATHBUF];
    int bad = 0, i;

    if (n < 1 || n > NSETS) { say("no such set: %d", n); return 1; }
    s = &g_sets[n - 1];
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        say("%s: no such folder", dir);
        return 1;
    }
    say("%s", s->label);
    say("  into %s", dir);
    for (i = 0; s->files[i]; i += 2) {
        snprintf(src, sizeof src, "%s%s\\%s", g_root, s->dir, s->files[i]);
        bad |= copy_one(src, dir, s->files[i + 1]);
    }
    return bad;
}

/* --------------------------------------------------------------- install */

static int install_selected(void)
{
    int i, bad = 0, any = 0;

    for (i = 0; i < g_ncomp; i++) {
        if (!g_comp[i].on) continue;
        any = 1;
        bad |= g_comp[i].run();
    }
    if (!any) { say("nothing selected"); return 0; }
    say("");
    if (bad) say("Finished with errors - see the lines above.");
    else if (g_reboot) say("Installed. Restart Windows to finish.");
    else say("Installed.");
    return bad;
}

static void reboot_now(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;

    say("restarting Windows");
    if (g_nt && OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
}

/* --------------------------------------------------------- the interface */

static void print_components(void)
{
    int i;

    say("Install into Windows:");
    for (i = 0; i < g_ncomp; i++)
        say("  %d [%c] %-34s %s", i + 1, g_comp[i].on ? 'x' : ' ', g_comp[i].label, g_comp[i].note);
}

static void print_sets(void)
{
    int i;

    say("Copy next to one game's EXE:");
    for (i = 0; i < NSETS; i++)
        say("  %d %s", i + 1, g_sets[i].label);
}

/* A line from the console, trimmed; NULL at end of input (a redirected
 * stdin that ran out, which is also how a batch file leaves us). */
static char *ask(const char *prompt, char *buf, int len)
{
    char *p;

    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, len, stdin)) return NULL;
    for (p = buf + strlen(buf); p > buf && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); p--)
        p[-1] = 0;
    for (p = buf; *p == ' '; p++) ;
    return p;
}

static void menu(void)
{
    char buf[PATHBUF + 16], *in;

    for (;;) {
        say("");
        print_components();
        in = ask("\nNumber toggles, I installs, G per-game files, Q quits: ", buf, sizeof buf);
        if (!in) return;
        if (isdigit((unsigned char)*in)) {
            int n = atoi(in);
            if (n >= 1 && n <= g_ncomp) g_comp[n - 1].on = !g_comp[n - 1].on;
            else say("no such component: %s", in);
            continue;
        }
        switch (toupper((unsigned char)*in)) {
        case 'I':
            say("");
            install_selected();
            if (g_reboot) {
                in = ask("\nRestart Windows now (y/N)? ", buf, sizeof buf);
                if (in && toupper((unsigned char)*in) == 'Y') { reboot_now(); return; }
            }
            break;
        case 'G': {
            int n;
            char dir[PATHBUF];
            say("");
            print_sets();
            in = ask("\nWhich set (Enter cancels)? ", buf, sizeof buf);
            if (!in || !*in) break;
            n = atoi(in);
            in = ask("Folder holding the game's EXE: ", dir, sizeof dir);
            if (!in || !*in) break;
            say("");
            copy_game_set(n, in);
            break;
        }
        case 'Q':
        case 0:
            return;
        default:
            say("?");
        }
    }
}

static void usage(void)
{
    printf("SETUP - 2ksbox guest tools\n\n"
           "  SETUP                 the menu\n"
           "  SETUP /ALL            install every component this Windows can use\n"
           "  SETUP /I <n> [<n>...] install those components\n"
           "  SETUP /LIST           print the component and file-set lists\n"
           "  SETUP /GAME <n> <dir> copy file set <n> next to a game's EXE\n"
           "  SETUP /REBOOT         with /ALL or /I: restart if one asked for it\n"
           "  SETUP /LOG <file>     write the log there (default SETUP.LOG)\n");
}

/* "Windows 98 SE" / "Windows XP" — what the user should see confirmed,
 * since the whole point is that the component list differs per family. */
static void os_name(char *out, OSVERSIONINFOA *v)
{
    const char *n = "Windows";

    if (v->dwPlatformId == VER_PLATFORM_WIN32_NT) {
        if (v->dwMajorVersion == 5 && v->dwMinorVersion == 0) n = "Windows 2000";
        else if (v->dwMajorVersion == 5) n = "Windows XP";
        else if (v->dwMajorVersion > 5) n = "Windows, newer than XP";
        else n = "Windows NT";
    } else if (v->dwMajorVersion == 4) {
        if (v->dwMinorVersion == 0) n = "Windows 95";
        else if (v->dwMinorVersion == 10) n = "Windows 98";
        else if (v->dwMinorVersion == 90) n = "Windows Me";
    }
    snprintf(out, 128, "%s (%lu.%lu build %lu)", n, (unsigned long)v->dwMajorVersion,
            (unsigned long)v->dwMinorVersion, (unsigned long)(v->dwBuildNumber & 0xffff));
}

/* The log next to the current directory, or in TEMP when that is the CD
 * itself (the usual case: SETUP is started from D:\). */
static void open_log(const char *want)
{
    char path[PATHBUF];

    if (want) { g_log = fopen(want, "w"); if (g_log) return; }
    g_log = fopen("SETUP.LOG", "w");
    if (g_log) return;
    if (GetTempPathA(sizeof path - 16, path)) {
        lstrcatA(path, "SETUP.LOG");
        g_log = fopen(path, "w");
    }
}

int main(int argc, char **argv)
{
    OSVERSIONINFOA ver;
    char osname[128], *slash;
    int i, want_reboot = 0, mode_all = 0, mode_list = 0, sel = 0, rc = 0;
    int game = 0;
    const char *game_dir = NULL, *logfile = NULL;

    ver.dwOSVersionInfoSize = sizeof ver;
    GetVersionExA(&ver);
    g_nt = ver.dwPlatformId == VER_PLATFORM_WIN32_NT;
    os_name(osname, &ver);

    GetModuleFileNameA(NULL, g_root, sizeof g_root);
    slash = strrchr(g_root, '\\');
    if (slash) slash[1] = 0;
    GetSystemDirectoryA(g_sys, sizeof g_sys);
    GetWindowsDirectoryA(g_win, sizeof g_win);

    /* only the components this Windows can use, in the listed order */
    for (i = 0; i < MAX_COMPONENTS && g_comp[i].label; i++) {
        if (!(g_nt ? g_comp[i].on_nt : g_comp[i].on_9x)) continue;
        if (i != g_ncomp) g_comp[g_ncomp] = g_comp[i];
        g_comp[g_ncomp].on = g_comp[g_ncomp].def;
        g_ncomp++;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') a++;
        if (!stricmp(a, "all")) mode_all = 1;
        else if (!stricmp(a, "list")) mode_list = 1;
        else if (!stricmp(a, "reboot")) want_reboot = 1;
        else if (!stricmp(a, "log") && i + 1 < argc) logfile = argv[++i];
        else if (!stricmp(a, "i")) {
            for (i++; i < argc && isdigit((unsigned char)argv[i][0]); i++) {
                int n = atoi(argv[i]);
                if (n >= 1 && n <= g_ncomp) sel |= 1 << (n - 1);
            }
            i--;
            mode_all = 2;
        } else if (!stricmp(a, "game") && i + 2 < argc) {
            game = atoi(argv[++i]);
            game_dir = argv[++i];
        } else {
            usage();
            return 1;
        }
    }

    open_log(logfile);
    say("2ksbox guest tools - %s", osname);
    say("files from %s", g_root);
    if (!g_nt)
        say("(Windows 98/Me uses the emulated Cirrus adapter; the display driver is 2000/XP only)");

    if (mode_list) { say(""); print_components(); say(""); print_sets(); return 0; }
    if (game_dir) return copy_game_set(game, game_dir);

    /* /ALL means all of them, including the ones the menu leaves
     * unticked: a batch file that says /ALL is not choosing defaults. */
    if (mode_all == 1)
        for (i = 0; i < g_ncomp; i++) g_comp[i].on = 1;
    if (mode_all == 2)
        for (i = 0; i < g_ncomp; i++) g_comp[i].on = (sel >> i) & 1;
    if (mode_all) {
        say("");
        print_components();
        say("");
        rc = install_selected();
        if (g_reboot && want_reboot) reboot_now();
        return rc;
    }
    menu();
    return 0;
}
