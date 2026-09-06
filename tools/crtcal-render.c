/*
 * crtcal-render.c — write the calibration patterns (doc 09) as BMPs.
 *
 *   crtcal-render <outdir> [WxH ...]      (default: the era mode set)
 *
 * The host half of the comparison: the same guest-tools/src/crtcal.h that
 * CRTCAL.EXE puts on the real tube, written to files the player can run
 * through a shader preset (`player --shader <preset> --calib <bmp>`). Shoot
 * the tube, shade the BMP, hold them side by side, adjust the preset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../guest-tools/src/crtcal.h"

static const struct { int w, h; } MODES[] = {
    { 320, 200 }, { 320, 240 }, { 640, 400 }, { 640, 480 },
    { 800, 600 }, { 1024, 768 }, { 1280, 1024 },
};

static int write_bmp(const char *path, int w, int h, const uint32_t *fb)
{
    int row = (w * 3 + 3) & ~3, size = 54 + row * h, y, x;
    unsigned char hdr[54];
    unsigned char *line;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = size; hdr[3] = size >> 8; hdr[4] = size >> 16; hdr[5] = size >> 24;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w; hdr[19] = w >> 8; hdr[20] = w >> 16; hdr[21] = w >> 24;
    hdr[22] = h; hdr[23] = h >> 8; hdr[24] = h >> 16; hdr[25] = h >> 24;
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof(hdr), f);
    line = calloc(1, row);
    if (!line) { fclose(f); return -1; }
    for (y = h - 1; y >= 0; y--) {           /* BMP rows run bottom-up */
        for (x = 0; x < w; x++) {
            uint32_t p = fb[(long)y * w + x];
            line[x * 3 + 0] = p & 0xff;
            line[x * 3 + 1] = (p >> 8) & 0xff;
            line[x * 3 + 2] = (p >> 16) & 0xff;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return 0;
}

/*
 * The one thing about these patterns that can be wrong without anyone
 * noticing: the circle in `grid` is drawn squeezed by the mode's pixel
 * aspect so that it comes out round *on the tube*. Measure the ring we just
 * drew and check that it does — the photographs are useless if the circle
 * was never a circle.
 */
static int check_grid(int w, int h, const uint32_t *fb)
{
    int x, y, x0 = w, x1 = -1, y0 = h, y1 = -1;
    double par = crtcal_display_aspect(w, h) / ((double)w / h), ratio;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if ((fb[(long)y * w + x] & 0xffffff) == 0xffff00) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < x0 || y1 < y0) {
        fprintf(stderr, "  %dx%d: no ring in the grid pattern\n", w, h);
        return -1;
    }
    ratio = (x1 - x0) * par / (y1 - y0);
    if (ratio < 0.98 || ratio > 1.02) {
        fprintf(stderr, "  %dx%d: the ring is %d x %d px, which is %.3f:1 on a "
                        "%.3f:1 tube -- not round\n",
                w, h, x1 - x0, y1 - y0, ratio, crtcal_display_aspect(w, h));
        return -1;
    }
    return 0;
}

static int emit(const char *dir, int w, int h)
{
    uint32_t *fb = malloc((size_t)w * h * 4);
    int pat, rc = 0;
    if (!fb) return -1;
    for (pat = 0; pat < CRTCAL_COUNT; pat++) {
        char path[512];
        crtcal_render(pat, w, h, fb);
        if (pat == CRTCAL_GRID && check_grid(w, h, fb) != 0) rc = -1;
        snprintf(path, sizeof(path), "%s/%s-%dx%d.bmp", dir, crtcal_name(pat), w, h);
        if (write_bmp(path, w, h, fb) != 0) {
            fprintf(stderr, "write %s failed\n", path);
            rc = -1;
        }
    }
    free(fb);
    printf("  %4dx%-4d  %d patterns%s\n", w, h, CRTCAL_COUNT,
           rc == 0 ? ", circle round on the tube" : "");
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir;
    int i, rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: crtcal-render <outdir> [WxH ...]\n\n");
        for (i = 0; i < CRTCAL_COUNT; i++)
            fprintf(stderr, "  %-10s %s\n     shot: %s\n",
                    crtcal_name(i), crtcal_asks(i), crtcal_shot(i));
        return 2;
    }
    dir = argv[1];
    if (argc > 2) {
        for (i = 2; i < argc; i++) {
            int w, h;
            if (sscanf(argv[i], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "bad mode %s\n", argv[i]);
                return 2;
            }
            if (emit(dir, w, h) != 0) rc = 1;
        }
    } else {
        for (i = 0; i < (int)(sizeof MODES / sizeof *MODES); i++)
            if (emit(dir, MODES[i].w, MODES[i].h) != 0) rc = 1;
    }
    return rc;
}
