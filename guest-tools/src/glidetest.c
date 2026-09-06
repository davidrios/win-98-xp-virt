/*
 * GLIDETEST.EXE — Glide 2.x through the pass-through device (doc 12 §5),
 * from inside the guest.
 *
 * The guest half of what tools/glide-host-test.cpp proves on the host: the
 * same scene, drawn by the same wrapper, so the two are comparable. Here
 * the whole chain is under test — GLIDE2X.DLL in the guest, the MMIO FIFO,
 * hw/3dfx's dispatcher, our libglide2x on the host, and the frontend's
 * context — and the guest checks its own pixels rather than trusting the
 * host to look at them, by reading the buffer back through grLfbLock. A
 * Glide game does exactly that for screenshots, so the path is a real one.
 *
 * Cases, each read back and compared:
 *   1. a clear: every sampled pixel is the clear colour
 *   2. a green triangle over the upper-left half at 640x480 — the corners
 *      are the orientation check, since Glide's origin is upper-left and
 *      the host's framebuffer is not
 *   3. a second clear: nothing of the triangle survives into the new frame
 *   4. grSstWinClose then grSstWinOpen again: a game's mode switch, which
 *      is where a host that leaked its context fails the second open
 *
 * Prints "glidetest: N cases, M failed" as its last line and writes
 * glidetest.log beside itself, like the other guest tests. Console
 * program: a guest script can read the exit code (0 = all passed).
 *
 *   GLIDETEST            640x480, the four cases
 *   GLIDETEST -res 8     another resolution (0-15, glidewnd.c's table)
 *   GLIDETEST -hold 5    keep the last frame up for 5 s, to look at it
 *
 * Built by guest-tools/build-wrappers.sh into TESTS\ on the guest ISO;
 * needs GLIDE2X.DLL installed (SETUP.EXE's Glide component — on 2000/XP
 * the device mapper step is required too, or the DLL cannot reach the
 * device and every entry point fails).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk2_glide.h"

static FILE *logfp;
static int cases, failed;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fflush(logfp);
    }
}

/* 0xRRGGBB of a pixel read out of the locked buffer, whatever the depth */
static unsigned pix_rgb(const GrLfbInfo_t *info, int w, int h, int x, int y)
{
    const unsigned char *base = (const unsigned char *)info->lfbPtr;
    const unsigned char *row = base + (size_t)y * info->strideInBytes;

    /* the pass-through opens 16-bit RGB565 buffers, which is what a Voodoo
     * had; a host that hands back 32-bit is handled rather than guessed at */
    if (info->strideInBytes >= (FxU32)w * 4) {
        unsigned p = ((const unsigned *)row)[x];
        return p & 0xffffffu;
    }
    unsigned short p = ((const unsigned short *)row)[x];
    unsigned r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

/* 5 % per channel: 565 quantisation, and the host may dither */
static int near_rgb(unsigned got, unsigned want)
{
    int i;
    for (i = 0; i < 3; i++) {
        int g = (got >> (i * 8)) & 0xff, w = (want >> (i * 8)) & 0xff;
        if (abs(g - w) > 13) {
            return 0;
        }
    }
    return 1;
}

struct probe { int x, y; unsigned want; const char *what; };

static void check(const char *name, int w, int h,
                  const struct probe *probes, int n)
{
    GrLfbInfo_t info;
    int i, bad = 0;

    cases++;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER,
                   GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE,
                   &info)) {
        failed++;
        say("  %-12s FAIL grLfbLock refused\n", name);
        return;
    }
    for (i = 0; i < n; i++) {
        unsigned got = pix_rgb(&info, w, h, probes[i].x, probes[i].y);
        int ok = near_rgb(got, probes[i].want);
        if (!ok) {
            bad++;
        }
        say("  %-12s %-14s (%3d,%3d) %06x want %06x  %s\n",
            i ? "" : name, probes[i].what, probes[i].x, probes[i].y,
            got, probes[i].want, ok ? "ok" : "WRONG");
    }
    grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER);
    if (bad) {
        failed++;
    }
}

/* the same triangle tools/glide-host-test.cpp draws: the upper-left half */
static void draw_triangle(int w, int h)
{
    GrVertex a, b, c;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    a.x = 0.f;            a.y = 0.f;
    b.x = (float)(w - 1); b.y = 0.f;
    c.x = 0.f;            c.y = (float)(h - 1);
    a.r = b.r = c.r = 0.f;
    a.g = b.g = c.g = 255.f;
    a.b = b.b = c.b = 0.f;
    a.a = b.a = c.a = 255.f;
    a.oow = b.oow = c.oow = 1.f;
    grDrawTriangle(&a, &b, &c);
}

static const struct tbl { int w, h; } tblRes[] = {
    { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 },
    { 640, 200 }, { 640, 350 }, { 640, 400 }, { 640, 480 },
    { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 },
    { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 }, { 400, 300 },
};

int main(int argc, char **argv)
{
    int res = GR_RESOLUTION_640x480, hold = 0, i, w, h;
    GrHwConfiguration hw;
    char version[80] = "";

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-res") && i + 1 < argc) {
            res = atoi(argv[++i]) & 0xf;
        } else if (!strcmp(argv[i], "-hold") && i + 1 < argc) {
            hold = atoi(argv[++i]);
        }
    }
    w = tblRes[res].w;
    h = tblRes[res].h;
    logfp = fopen("glidetest.log", "w");

    grGlideInit();
    grGlideGetVersion(version);
    say("glidetest: Glide %s, resolution %d = %dx%d\n", version, res, w, h);

    if (!grSstQueryHardware(&hw)) {
        say("glidetest: no Glide hardware — is GLIDE2X.DLL the pass-through "
            "wrapper, and (on 2000/XP) is the device mapper installed?\n");
        say("glidetest: 0 cases, 1 failed\n");
        return 1;
    }
    say("glidetest: %d board(s) reported\n", hw.num_sst);
    grSstSelect(0);

    if (!grSstWinOpen(0, res, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                      GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        say("glidetest: grSstWinOpen(%d) failed — the host refused a "
            "drawable\n", res);
        say("glidetest: 0 cases, 1 failed\n");
        return 1;
    }
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    {
        /* red everywhere. 0x0000ff is red in ABGR, the format opened above */
        struct probe p[] = {
            { 10, 10, 0xff0000, "top-left" },
            { w - 10, h - 10, 0xff0000, "bottom-right" },
        };
        grBufferClear(0x0000ff, 0, 0);
        grBufferSwap(0);
        check("clear", w, h, p, 2);
    }
    {
        /* the triangle covers the upper-left half: if anything in the chain
         * flips y, these two swap and the case fails */
        struct probe p[] = {
            { 10, 10, 0x00ff00, "in triangle" },
            { w - 10, h - 10, 0xff0000, "outside" },
            { w / 8, h - h / 8, 0xff0000, "below hypotenuse" },
        };
        grBufferClear(0x0000ff, 0, 0);
        draw_triangle(w, h);
        grBufferSwap(0);
        check("triangle", w, h, p, 3);
    }
    {
        struct probe p[] = {
            { 10, 10, 0x0000ff, "top-left" },
        };
        /* blue this time: a stale frame would read green or red */
        grBufferClear(0xff0000, 0, 0);
        grBufferSwap(0);
        check("reclear", w, h, p, 1);
    }

    if (hold > 0) {
        Sleep(hold * 1000);
    }

    /* a game's mode switch: close and open again. A host that leaked its
     * context or its drawable fails the second open, not the first. */
    grSstWinClose();
    cases++;
    if (!grSstWinOpen(0, res, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                      GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        failed++;
        say("  %-12s FAIL the second grSstWinOpen was refused\n", "reopen");
    } else {
        struct probe p[] = {
            { 10, 10, 0x00ff00, "in triangle" },
            { w - 10, h - 10, 0xff0000, "outside" },
        };
        cases--;   /* check() counts it */
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE,
                       FXFALSE);
        grBufferClear(0x0000ff, 0, 0);
        draw_triangle(w, h);
        grBufferSwap(0);
        check("reopen", w, h, p, 2);
        grSstWinClose();
    }

    grGlideShutdown();
    say("glidetest: %d cases, %d failed\n", cases, failed);
    if (logfp) {
        fclose(logfp);
    }
    return failed ? 1 : 0;
}
