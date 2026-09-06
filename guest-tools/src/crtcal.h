/*
 * crtcal.h — the CRT calibration patterns (doc 09).
 *
 * One definition, compiled into both sides of the comparison: CRTCAL.EXE
 * puts them on a real tube to be photographed, tools/crtcal-render.c writes
 * the same pixels to BMPs for the player to run through a shader preset.
 * Nothing here allocates or touches files, so it drops into a DirectDraw
 * lock loop unchanged.
 *
 * Every pattern answers one question about the tube; crtcal_asks() says
 * which, and doc 09 says how to photograph it. Patterns are drawn in
 * XRGB8888 at the mode's exact size — never scaled, or the thing being
 * measured is the scaler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CRTCAL_H
#define CRTCAL_H

#include <math.h>
#include <stdint.h>

enum {
    CRTCAL_GRID,
    CRTCAL_SCANLINES,
    CRTCAL_MASK,
    CRTCAL_BLOOM,
    CRTCAL_SHARP,
    CRTCAL_HALATION,
    CRTCAL_GAMMA,
    CRTCAL_COLOUR,
    CRTCAL_COUNT
};

static const char *crtcal_name(int pat)
{
    static const char *n[CRTCAL_COUNT] = {
        "grid", "scanlines", "mask", "bloom",
        "sharp", "halation", "gamma", "colour"
    };
    return (pat >= 0 && pat < CRTCAL_COUNT) ? n[pat] : "?";
}

static const char *crtcal_asks(int pat)
{
    static const char *a[CRTCAL_COUNT] = {
        "geometry: does the mode fill 4:3, how much falls off the edges, is it linear",
        "the beam's vertical profile, and how many scanlines the tube really draws",
        "the mask: kind, pitch in mm, and the stagger",
        "how much the beam widens as it gets brighter",
        "horizontal spot size, and where the video bandwidth gives out",
        "halation and glow: how far light spreads into black",
        "the tube's gamma, against a dithered reference",
        "phosphor primaries and colour temperature"
    };
    return (pat >= 0 && pat < CRTCAL_COUNT) ? a[pat] : "?";
}

/* how each one wants to be shot; the detail is in doc 09 */
static const char *crtcal_shot(int pat)
{
    static const char *s[CRTCAL_COUNT] = {
        "whole screen, straight on, lens centred on the middle of the tube",
        "macro, centre of a band; also one whole-screen frame",
        "macro, as close as the lens focuses, with a ruler in the frame",
        "macro across the stack, one exposure for all rows",
        "macro on the bar bands; repeat the mode sweep to find the limit",
        "whole screen, dark room, fixed exposure, do not change it between shots",
        "whole screen, straight on, slightly defocused is fine",
        "whole screen, fixed white balance (daylight), not auto"
    };
    return (pat >= 0 && pat < CRTCAL_COUNT) ? s[pat] : "?";
}

/* ------------------------------------------------------------------ paint */

#define CRTCAL_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define CRTCAL_GREY(v)      CRTCAL_RGB((v), (v), (v))

static void crtcal__px(uint32_t *fb, int w, int h, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        fb[(long)y * w + x] = c;
}

static void crtcal__fill(uint32_t *fb, int w, int h, int x0, int y0,
                         int x1, int y1, uint32_t c)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            crtcal__px(fb, w, h, x, y, c);
}

/* 3x5 digits, one byte per row, bit 2 leftmost */
static const unsigned char crtcal__font[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}
};

static void crtcal__digit(uint32_t *fb, int w, int h, int x, int y,
                          int d, int s, uint32_t c)
{
    int r, b, i, j;
    for (r = 0; r < 5; r++)
        for (b = 0; b < 3; b++)
            if (crtcal__font[d][r] & (4 >> b))
                for (i = 0; i < s; i++)
                    for (j = 0; j < s; j++)
                        crtcal__px(fb, w, h, x + b * s + i, y + r * s + j, c);
}

static void crtcal__num(uint32_t *fb, int w, int h, int x, int y,
                        int v, int s, uint32_t c)
{
    int d[8], n = 0;
    if (v == 0) d[n++] = 0;
    while (v > 0) { d[n++] = v % 10; v /= 10; }
    while (n-- > 0) {
        crtcal__digit(fb, w, h, x, y, d[n], s, c);
        x += 4 * s;
    }
}

/*
 * The aspect the mode was meant to fill, so `grid`'s circle is a circle on
 * the tube rather than in the framebuffer. Mirrors the player's mode table
 * (player/src/mode.rs) for the one era mode that is not 4:3; keep in step.
 */
static double crtcal_display_aspect(int w, int h)
{
    if (w == 1280 && h == 1024) return 5.0 / 4.0;
    return 4.0 / 3.0;
}

/* ---------------------------------------------------------------- patterns */

static void crtcal__grid(uint32_t *fb, int w, int h)
{
    const uint32_t dim = CRTCAL_GREY(0x50), lit = CRTCAL_GREY(0xff);
    const uint32_t ring = CRTCAL_RGB(0xff, 0xff, 0x00);  /* stands out from
                                       the grid in a photograph, and lets a
                                       checker find the circle on its own */
    double par = crtcal_display_aspect(w, h) / ((double)w / h);
    double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0, r = 0.45 * h;
    int x, y, step = (w >= 640) ? 32 : 16, tick = step / 4, major = step * 2;
    int ds = (w >= 1024) ? 2 : 1;               /* digit scale */

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (x = 0; x < w; x += step)
        for (y = 0; y < h; y++)
            crtcal__px(fb, w, h, x, y, (x % major) ? dim : lit);
    for (y = 0; y < h; y += step)
        for (x = 0; x < w; x++)
            crtcal__px(fb, w, h, x, y, (y % major) ? dim : lit);

    /* a circle on the glass: an ellipse here, squeezed by the pixel aspect */
    for (x = 0; x < w; x++) {
        double dx = (x - cx) * par, s = r * r - dx * dx;
        if (s < 0) continue;
        crtcal__px(fb, w, h, x, (int)(cy - sqrt(s) + 0.5), ring);
        crtcal__px(fb, w, h, x, (int)(cy + sqrt(s) + 0.5), ring);
    }
    for (y = 0; y < h; y++) {
        double dy = y - cy, s = r * r - dy * dy;
        if (s < 0) continue;
        crtcal__px(fb, w, h, (int)(cx - sqrt(s) / par + 0.5), y, ring);
        crtcal__px(fb, w, h, (int)(cx + sqrt(s) / par + 0.5), y, ring);
    }

    /* edge rulers: a tick every quarter step, a number every major step, so
       a straight-on photograph says how many pixels fell off each edge */
    for (x = 0; x < w; x += tick) {
        int len = (x % major) ? tick / 2 : tick;
        crtcal__fill(fb, w, h, x, 0, x + 1, len, lit);
        crtcal__fill(fb, w, h, x, h - len, x + 1, h, lit);
        if (x % major == 0 && x > 0) {
            crtcal__num(fb, w, h, x + 2, len + 2, x, ds, lit);
            crtcal__num(fb, w, h, x + 2, h - len - 2 - 5 * ds, x, ds, lit);
        }
    }
    for (y = 0; y < h; y += tick) {
        int len = (y % major) ? tick / 2 : tick;
        crtcal__fill(fb, w, h, 0, y, len, y + 1, lit);
        crtcal__fill(fb, w, h, w - len, y, w, y + 1, lit);
        if (y % major == 0 && y > 0) {
            crtcal__num(fb, w, h, len + 2, y + 2, y, ds, lit);
            crtcal__num(fb, w, h, w - len - 2 - 16 * ds, y + 2, y, ds, lit);
        }
    }
    /* the first and last pixel of every edge, to see what the tube cuts */
    crtcal__fill(fb, w, h, 0, 0, 8, 8, lit);
    crtcal__fill(fb, w, h, w - 8, 0, w, 8, lit);
    crtcal__fill(fb, w, h, 0, h - 8, 8, h, lit);
    crtcal__fill(fb, w, h, w - 8, h - 8, w, h, lit);
}

/*
 * Bands of horizontal line patterns. Band 0 is every other line: on a
 * double-scanned mode the tube draws each of those twice, and the macro
 * shot settles whether 320x200 really is 400 scanlines. The wider spacings
 * show one beam on its own — its thickness against the pitch is the beam
 * profile the shader's scanline parameters stand for.
 */
static void crtcal__scanlines(uint32_t *fb, int w, int h)
{
    const int gaps[6] = { 1, 2, 3, 4, 0, -1 }; /* 0 = solid, -1 = 50% grey */
    int band, y, x, nb = 6, bh = h / nb;

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (band = 0; band < nb; band++) {
        int y0 = band * bh, y1 = (band == nb - 1) ? h : y0 + bh;
        for (y = y0; y < y1; y++) {
            uint32_t c;
            if (gaps[band] == -1)      c = CRTCAL_GREY(0x80);
            else if (gaps[band] == 0)  c = CRTCAL_GREY(0xff);
            else c = ((y - y0) % (gaps[band] + 1) == 0) ? CRTCAL_GREY(0xff) : 0;
            for (x = 0; x < w; x++)
                crtcal__px(fb, w, h, x, y, c);
        }
        /* band number, dim, at the very left so a macro of the middle is clean */
        crtcal__num(fb, w, h, 1, y0 + 2, band, 1, CRTCAL_GREY(0x60));
    }
}

/* Flat fields: white and grey above, the three phosphors below. */
static void crtcal__mask(uint32_t *fb, int w, int h)
{
    int half = h / 2, third = w / 3;
    crtcal__fill(fb, w, h, 0, 0, w / 2, half, CRTCAL_GREY(0xff));
    crtcal__fill(fb, w, h, w / 2, 0, w, half, CRTCAL_GREY(0x80));
    crtcal__fill(fb, w, h, 0, half, third, h, CRTCAL_RGB(0xff, 0, 0));
    crtcal__fill(fb, w, h, third, half, 2 * third, h, CRTCAL_RGB(0, 0xff, 0));
    crtcal__fill(fb, w, h, 2 * third, half, w, h, CRTCAL_RGB(0, 0, 0xff));
}

/*
 * One-pixel lines at rising levels on black. The beam gets wider as it gets
 * brighter; photographed in one exposure, the thickness against the level is
 * what beam_min / beam_max / beam_size are for.
 */
static void crtcal__bloom(uint32_t *fb, int w, int h)
{
    int rows = 12, i, x, step = h / (rows + 1), sq = step / 3;

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (i = 0; i < rows; i++) {
        int v = 255 * (i + 1) / rows, y = step / 2 + i * step;
        uint32_t c = CRTCAL_GREY(v);
        for (x = w / 8; x < w * 5 / 8; x++)      /* the 1px line */
            crtcal__px(fb, w, h, x, y, c);
        crtcal__fill(fb, w, h, w * 11 / 16, y - sq / 2,  /* a solid block */
                     w * 11 / 16 + sq, y - sq / 2 + sq, c);
        crtcal__num(fb, w, h, 4, y - 2, v, 1, CRTCAL_GREY(0x70));
    }
}

/*
 * Vertical bar pairs at falling pitch, then a hard edge. Where the bars stop
 * resolving is the video amplifier giving out; the edge shows the spot's
 * horizontal profile. Run it at every mode: on a monitor of this class the
 * fine bands survive at 640x480 and start smearing near the top mode.
 */
static void crtcal__sharp(uint32_t *fb, int w, int h)
{
    const int pitch[6] = { 2, 4, 6, 8, 12, 16 };
    int band, nb = 6, bh = h / (nb + 1), x, y;

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (band = 0; band < nb; band++) {
        int y0 = band * bh, y1 = y0 + bh;
        for (y = y0; y < y1; y++)
            for (x = 0; x < w; x++)
                crtcal__px(fb, w, h, x, y,
                           ((x % pitch[band]) < pitch[band] / 2)
                               ? CRTCAL_GREY(0xff) : 0);
        crtcal__num(fb, w, h, 2, y0 + 2, pitch[band], 1, CRTCAL_GREY(0x70));
    }
    /* a black-to-white edge down the middle of the last band */
    crtcal__fill(fb, w, h, w / 2, nb * bh, w, h, CRTCAL_GREY(0xff));
}

/*
 * Small white shapes on a large black field, and one on grey. Shot at a
 * fixed exposure, how far the glow reaches is halation; the anti-glare
 * coating on a flat tube of this era keeps it tight.
 */
static void crtcal__halation(uint32_t *fb, int w, int h)
{
    int s = h / 8, cx = w / 2, cy = h / 3, t = s / 8 ? s / 8 : 1;

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    crtcal__fill(fb, w, h, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2,
                 CRTCAL_GREY(0xff));
    /* a cross: the same light with more edge than area */
    crtcal__fill(fb, w, h, w / 5 - t, cy - s, w / 5 + t, cy + s, CRTCAL_GREY(0xff));
    crtcal__fill(fb, w, h, w / 5 - s, cy - t, w / 5 + s, cy + t, CRTCAL_GREY(0xff));
    /* the same square on grey: glow against a lit background, not black */
    crtcal__fill(fb, w, h, 0, h * 5 / 8, w, h, CRTCAL_GREY(0x40));
    crtcal__fill(fb, w, h, cx - s / 2, h * 13 / 16 - s / 2,
                 cx + s / 2, h * 13 / 16 + s / 2, CRTCAL_GREY(0xff));
}

/*
 * A dithered half-and-half block beside a ramp of solid patches. The patch
 * that matches the dither is the tube's mid grey, and gamma follows from
 * log(0.5)/log(v/255). Photograph slightly out of focus so the dither reads
 * as its average.
 */
static void crtcal__gamma(uint32_t *fb, int w, int h)
{
    int steps = 16, i, x, y;

    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (i = 0; i < steps; i++) {
        int v = 255 * (i + 1) / steps;
        int y0 = i * h / steps, y1 = (i + 1) * h / steps;
        /* left: the checkerboard, the same for every row */
        for (y = y0; y < y1; y++)
            for (x = 0; x < w / 2; x++)
                crtcal__px(fb, w, h, x, y,
                           ((x + y) & 1) ? CRTCAL_GREY(0xff) : 0);
        /* right: the solid patch to match it against */
        crtcal__fill(fb, w, h, w / 2, y0, w, y1, CRTCAL_GREY(v));
        crtcal__num(fb, w, h, w / 2 + 4, y0 + 2, v, 1,
                    (v > 128) ? 0 : CRTCAL_GREY(0xff));
    }
}

/* The three phosphors at full, white, and a ramp of each. */
static void crtcal__colour(uint32_t *fb, int w, int h)
{
    int i, q = w / 4, top = h * 2 / 3, rows;
    const uint32_t full[4] = {
        CRTCAL_RGB(0xff, 0, 0), CRTCAL_RGB(0, 0xff, 0),
        CRTCAL_RGB(0, 0, 0xff), CRTCAL_GREY(0xff)
    };
    crtcal__fill(fb, w, h, 0, 0, w, h, 0);
    for (i = 0; i < 4; i++)
        crtcal__fill(fb, w, h, i * q, 0, (i == 3) ? w : (i + 1) * q, top, full[i]);
    rows = 6;
    for (i = 0; i < rows; i++) {
        int v = 255 * (i + 1) / rows;
        int x0 = i * w / rows, x1 = (i + 1) * w / rows, band = (h - top) / 3;
        crtcal__fill(fb, w, h, x0, top, x1, top + band, CRTCAL_RGB(v, 0, 0));
        crtcal__fill(fb, w, h, x0, top + band, x1, top + 2 * band, CRTCAL_RGB(0, v, 0));
        crtcal__fill(fb, w, h, x0, top + 2 * band, x1, h, CRTCAL_RGB(0, 0, v));
    }
}

/* Render one pattern into an XRGB8888 buffer of w*h pixels. */
static void crtcal_render(int pat, int w, int h, uint32_t *fb)
{
    switch (pat) {
    case CRTCAL_GRID:      crtcal__grid(fb, w, h); break;
    case CRTCAL_SCANLINES: crtcal__scanlines(fb, w, h); break;
    case CRTCAL_MASK:      crtcal__mask(fb, w, h); break;
    case CRTCAL_BLOOM:     crtcal__bloom(fb, w, h); break;
    case CRTCAL_SHARP:     crtcal__sharp(fb, w, h); break;
    case CRTCAL_HALATION:  crtcal__halation(fb, w, h); break;
    case CRTCAL_GAMMA:     crtcal__gamma(fb, w, h); break;
    case CRTCAL_COLOUR:    crtcal__colour(fb, w, h); break;
    default:               crtcal__fill(fb, w, h, 0, 0, w, h, 0); break;
    }
}

#endif /* CRTCAL_H */
