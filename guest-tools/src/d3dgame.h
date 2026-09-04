/*
 * d3dgame.h: the scene shared by d3dgame9.c and d3dgame8.c (doc 14 P0a).
 * Deterministic, API-free: math, procedural textures, animation state, a
 * BMP writer and the command line. Each program maps it onto its API.
 *
 * Scene: a checker ground (8888 texture, mipmapped), five lit rotating
 * cubes (indexed, materials), a waving grid from a per-frame dynamic
 * vertex buffer (565 texture), additive particles (DXT1 texture,
 * DrawPrimitiveUP), a render-to-texture "monitor" quad showing the cubes
 * from a fixed camera, and a frame-time bar graph in screen space.
 * Camera: WASD/arrows + Q/E height; Esc quits; F1 wireframe; Space pause.
 * -frames N   fixed 1/60 s step, exit after N frames (golden runs)
 * -dump N f   write frame N to f (BMP, 24-bit) then continue
 * -fs         exclusive fullscreen (mode change), -w W -h H size (640x480)
 * -bpp16      16-bit back buffer (565) instead of X8R8G8B8
 * -novsync    D3DPRESENT_INTERVAL_IMMEDIATE
 * -shader     (d3d9) SM1.1 vs/ps for the cubes when D3DX is available
 *             (HLSL; d3dx9_33+ refuse ps_1_1, leaving vs_1_1 + the fixed
 *             pixel stage — kept that way on purpose, the golden set has it)
 * -log f      log file (default d3dgame9.log / d3dgame8.log next to the EXE,
 *             appended); everything printed to the console goes there too
 */
#ifndef D3DGAME_H
#define D3DGAME_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define GAME_W_DEFAULT 640
#define GAME_H_DEFAULT 480
#define GRID_N 32                       /* wave grid vertices per side */
#define GRID_VERTS (GRID_N * GRID_N)
#define GRID_INDICES ((GRID_N - 1) * (GRID_N - 1) * 6)
#define NUM_CUBES 5
#define NUM_PARTICLES 64
#define RTT_SIZE 128
#define BARS 64

struct opts {
    int w, h, fullscreen, bpp16, novsync, shader, frames;
    int dump_frame;
    char dump_file[MAX_PATH];
    char log_file[MAX_PATH];
};

/* ---- logging: console + file, flushed per line (a crash keeps the tail) ---- */

static FILE *g_log;

static void game_log(const char *fmt, ...)
{
    va_list ap;
    char line[512];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

/* a bare file name goes next to the EXE, whatever the shortcut's "Start in" is */
static void game_path_near_exe(char *out, size_t n, const char *name)
{
    char exe[MAX_PATH], *slash;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        strncpy(out, name, n - 1);
        out[n - 1] = 0;
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(out, n, "%s%s", exe, name);
}

static void game_log_open(const char *name, int argc, char **argv)
{
    SYSTEMTIME st;
    int i;
    char path[MAX_PATH];
    game_path_near_exe(path, sizeof(path), name);
    g_log = fopen(path, "at");
    GetLocalTime(&st);
    game_log("---- %04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    game_log("log %s", path);
    for (i = 0; i < argc; i++) game_log("arg[%d] %s", i, argv[i]);
    {
        OSVERSIONINFOA v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);
        game_log("windows %lu.%lu build %lu %s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, v.szCSDVersion);
    }
}

struct vtx_pnt { float x, y, z, nx, ny, nz; float u, v; };            /* XYZ|NORMAL|TEX1 */
struct vtx_pct { float x, y, z; DWORD color; float u, v; };            /* XYZ|DIFFUSE|TEX1 */
struct vtx_rhw { float x, y, z, rhw; DWORD color; };                   /* XYZRHW|DIFFUSE */

struct mat4 { float m[4][4]; };

static void opts_parse(struct opts *o, int argc, char **argv)
{
    int i;
    memset(o, 0, sizeof(*o));
    o->w = GAME_W_DEFAULT;
    o->h = GAME_H_DEFAULT;
    o->dump_frame = -1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-fs")) o->fullscreen = 1;
        else if (!strcmp(argv[i], "-bpp16")) o->bpp16 = 1;
        else if (!strcmp(argv[i], "-novsync")) o->novsync = 1;
        else if (!strcmp(argv[i], "-shader")) o->shader = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) o->w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) o->h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc) o->frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dump") && i + 2 < argc) {
            o->dump_frame = atoi(argv[++i]);
            game_path_near_exe(o->dump_file, MAX_PATH, argv[++i]);
        }
        else if (!strcmp(argv[i], "-log") && i + 1 < argc) strncpy(o->log_file, argv[++i], MAX_PATH - 1);
    }
}

/* ---- math (row-major, D3D conventions: v * M) ---- */

static void m_identity(struct mat4 *r)
{
    memset(r, 0, sizeof(*r));
    r->m[0][0] = r->m[1][1] = r->m[2][2] = r->m[3][3] = 1.0f;
}

static void m_mul(struct mat4 *r, const struct mat4 *a, const struct mat4 *b)
{
    struct mat4 t;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j]
                      + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *r = t;
}

static void m_translate(struct mat4 *r, float x, float y, float z)
{
    m_identity(r);
    r->m[3][0] = x; r->m[3][1] = y; r->m[3][2] = z;
}

static void m_scale(struct mat4 *r, float s)
{
    m_identity(r);
    r->m[0][0] = r->m[1][1] = r->m[2][2] = s;
}

static void m_rot_y(struct mat4 *r, float a)
{
    m_identity(r);
    r->m[0][0] = cosf(a); r->m[0][2] = -sinf(a);
    r->m[2][0] = sinf(a); r->m[2][2] = cosf(a);
}

static void m_rot_x(struct mat4 *r, float a)
{
    m_identity(r);
    r->m[1][1] = cosf(a); r->m[1][2] = sinf(a);
    r->m[2][1] = -sinf(a); r->m[2][2] = cosf(a);
}

static void m_perspective(struct mat4 *r, float fovy, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect;
    memset(r, 0, sizeof(*r));
    r->m[0][0] = xs;
    r->m[1][1] = ys;
    r->m[2][2] = zf / (zf - zn);
    r->m[2][3] = 1.0f;
    r->m[3][2] = -zn * zf / (zf - zn);
}

static void m_lookat(struct mat4 *r, const float *eye, const float *at)
{
    float zx = at[0] - eye[0], zy = at[1] - eye[1], zz = at[2] - eye[2];
    float l = sqrtf(zx * zx + zy * zy + zz * zz);
    float xx, xy, xz, yx, yy, yz;
    zx /= l; zy /= l; zz /= l;
    /* x = up(0,1,0) cross z */
    xx = zz; xy = 0.0f; xz = -zx;
    l = sqrtf(xx * xx + xz * xz);
    if (l < 1e-6f) { xx = 1.0f; xz = 0.0f; l = 1.0f; }
    xx /= l; xz /= l;
    /* y = z cross x */
    yx = zy * xz - zz * xy; yy = zz * xx - zx * xz; yz = zx * xy - zy * xx;
    m_identity(r);
    r->m[0][0] = xx; r->m[1][0] = xy; r->m[2][0] = xz;
    r->m[0][1] = yx; r->m[1][1] = yy; r->m[2][1] = yz;
    r->m[0][2] = zx; r->m[1][2] = zy; r->m[2][2] = zz;
    r->m[3][0] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
    r->m[3][1] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
    r->m[3][2] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
}

/* ---- geometry ---- */

/* 24 vertices, 36 indices, unit cube centred at the origin */
static void geo_cube(struct vtx_pnt *v, WORD *idx)
{
    static const float n[6][3] = { {0,0,-1}, {0,0,1}, {0,-1,0}, {0,1,0}, {-1,0,0}, {1,0,0} };
    static const float u[6][3] = { {1,0,0}, {-1,0,0}, {1,0,0}, {1,0,0}, {0,0,1}, {0,0,-1} };
    int f, i;
    for (f = 0; f < 6; f++) {
        float vx = n[f][1] * u[f][2] - n[f][2] * u[f][1];   /* v = n cross u */
        float vy = n[f][2] * u[f][0] - n[f][0] * u[f][2];
        float vz = n[f][0] * u[f][1] - n[f][1] * u[f][0];
        for (i = 0; i < 4; i++) {
            float su = (i == 1 || i == 2) ? 0.5f : -0.5f;
            float sv = (i >= 2) ? 0.5f : -0.5f;
            struct vtx_pnt *p = &v[f * 4 + i];
            p->x = n[f][0] * 0.5f + u[f][0] * su + vx * sv;
            p->y = n[f][1] * 0.5f + u[f][1] * su + vy * sv;
            p->z = n[f][2] * 0.5f + u[f][2] * su + vz * sv;
            p->nx = n[f][0]; p->ny = n[f][1]; p->nz = n[f][2];
            p->u = (i == 1 || i == 2) ? 1.0f : 0.0f;
            p->v = (i >= 2) ? 1.0f : 0.0f;
        }
        idx[f * 6 + 0] = f * 4 + 0; idx[f * 6 + 1] = f * 4 + 1; idx[f * 6 + 2] = f * 4 + 2;
        idx[f * 6 + 3] = f * 4 + 0; idx[f * 6 + 4] = f * 4 + 2; idx[f * 6 + 5] = f * 4 + 3;
    }
}

static void geo_grid_indices(WORD *idx)
{
    int x, z, k = 0;
    for (z = 0; z < GRID_N - 1; z++) {
        for (x = 0; x < GRID_N - 1; x++) {
            WORD a = z * GRID_N + x, b = a + 1, c = a + GRID_N, d = c + 1;
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }
    }
}

/* the per-frame animated grid: y = wave(t), colour from height */
static void geo_grid_fill(struct vtx_pct *v, float t)
{
    int x, z;
    for (z = 0; z < GRID_N; z++) {
        for (x = 0; x < GRID_N; x++) {
            struct vtx_pct *p = &v[z * GRID_N + x];
            float fx = (x / (float)(GRID_N - 1) - 0.5f) * 6.0f;
            float fz = (z / (float)(GRID_N - 1) - 0.5f) * 6.0f;
            float h = 0.25f * sinf(fx * 1.5f + t * 2.0f) * cosf(fz * 1.3f - t * 1.7f);
            int c = (int)((h + 0.25f) * 2.0f * 255.0f);
            if (c < 0) c = 0;
            if (c > 255) c = 255;
            p->x = fx; p->y = h - 0.9f; p->z = fz + 6.0f;
            p->color = 0xff000000 | (c << 16) | ((255 - c) << 8) | 0x80;
            p->u = x / (float)(GRID_N - 1) * 4.0f;
            p->v = z / (float)(GRID_N - 1) * 4.0f;
        }
    }
}

/* ---- procedural textures (caller locked the level; pitch in bytes) ---- */

static void tex_checker_8888(void *bits, int pitch, int w, int h, int level)
{
    int x, y, cell = 16 >> level;
    if (cell < 1) cell = 1;
    for (y = 0; y < h; y++) {
        DWORD *row = (DWORD *)((BYTE *)bits + y * pitch);
        for (x = 0; x < w; x++) {
            int c = ((x / cell) + (y / cell)) & 1;
            row[x] = c ? 0xffe0e0e0 : 0xff305090;
            if (level == 0 && ((x & 15) == 0 || (y & 15) == 0)) row[x] = 0xff202020;
        }
    }
}

static void tex_gradient_565(void *bits, int pitch, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++) {
        WORD *row = (WORD *)((BYTE *)bits + y * pitch);
        for (x = 0; x < w; x++) {
            int r = x * 31 / (w - 1), g = y * 63 / (h - 1), b = 31 - r;
            row[x] = (WORD)((r << 11) | (g << 5) | b);
        }
    }
}

/* DXT1: a soft white disc on black, 4x4 blocks of colour0 white / colour1 black */
static void tex_disc_dxt1(void *bits, int pitch, int w, int h)
{
    int bx, by, x, y;
    for (by = 0; by < h / 4; by++) {
        BYTE *row = (BYTE *)bits + by * pitch;
        for (bx = 0; bx < w / 4; bx++) {
            BYTE *blk = row + bx * 8;
            DWORD idx = 0;
            blk[0] = 0xff; blk[1] = 0xff;      /* colour0 = white (565) */
            blk[2] = 0x00; blk[3] = 0x00;      /* colour1 = black */
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    float dx = (bx * 4 + x + 0.5f) / w - 0.5f, dy = (by * 4 + y + 0.5f) / h - 0.5f;
                    float d = sqrtf(dx * dx + dy * dy) * 2.0f;   /* 0 centre .. 1 edge */
                    int code = d < 0.4f ? 0 : d < 0.65f ? 2 : d < 0.9f ? 3 : 1;   /* 0 c0, 2 2/3c0, 3 1/3c0, 1 c1 */
                    idx |= (DWORD)code << ((y * 4 + x) * 2);
                }
            }
            blk[4] = (BYTE)idx; blk[5] = (BYTE)(idx >> 8); blk[6] = (BYTE)(idx >> 16); blk[7] = (BYTE)(idx >> 24);
        }
    }
}

/* ---- animation state ---- */

struct game {
    struct opts o;
    float t;                /* scene time */
    unsigned frame;
    float cam_yaw, cam_dist, cam_h;
    int wireframe, paused;
    float bars[BARS];       /* last frame times, ms */
    int bar_i;
    DWORD start_ms, t0_ms, last_ms, fps_frames;
    float fps;
    int quit;
};

static void game_init(struct game *g, int argc, char **argv)
{
    memset(g, 0, sizeof(*g));
    opts_parse(&g->o, argc, argv);
    g->cam_yaw = 0.6f;
    g->cam_dist = 9.0f;
    g->cam_h = 3.5f;
    g->start_ms = g->t0_ms = g->last_ms = GetTickCount();
}

/* keyboard (GetAsyncKeyState: works in menus and exclusive fullscreen alike) */
static void game_input(struct game *g, float dt)
{
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g->quit = 1;
    if ((GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState('A') & 0x8000)) g->cam_yaw -= 1.5f * dt;
    if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState('D') & 0x8000)) g->cam_yaw += 1.5f * dt;
    if ((GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000)) g->cam_dist -= 4.0f * dt;
    if ((GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000)) g->cam_dist += 4.0f * dt;
    if (GetAsyncKeyState('Q') & 0x8000) g->cam_h -= 3.0f * dt;
    if (GetAsyncKeyState('E') & 0x8000) g->cam_h += 3.0f * dt;
    if (g->cam_dist < 2.0f) g->cam_dist = 2.0f;
    if (g->cam_dist > 30.0f) g->cam_dist = 30.0f;
}

/* one step; returns dt. Deterministic in -frames mode. */
static float game_step(struct game *g)
{
    float dt;
    DWORD now = GetTickCount();
    if (g->o.frames) {
        dt = 1.0f / 60.0f;
        g->cam_yaw += 0.4f * dt;           /* auto orbit: golden runs need no keyboard */
    } else {
        dt = (now - g->last_ms) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        game_input(g, dt);
    }
    g->bars[g->bar_i] = (now - g->last_ms) * 1.0f;
    g->bar_i = (g->bar_i + 1) % BARS;
    g->last_ms = now;
    if (!g->paused) g->t += dt;
    g->frame++;
    g->fps_frames++;
    if (now - g->t0_ms >= 1000) {
        g->fps = g->fps_frames * 1000.0f / (now - g->t0_ms);
        g->fps_frames = 0;
        g->t0_ms = now;
        return -dt;                        /* negative: "a second passed, report" */
    }
    return dt;
}

static void game_camera(const struct game *g, float *eye, float *at)
{
    eye[0] = sinf(g->cam_yaw) * g->cam_dist; eye[1] = g->cam_h; eye[2] = cosf(g->cam_yaw) * g->cam_dist + 3.0f;
    at[0] = 0.0f; at[1] = 0.0f; at[2] = 3.0f;
}

static void game_cube_world(const struct game *g, int i, struct mat4 *w)
{
    struct mat4 r1, r2, s, t, tmp;
    float a = g->t * (0.7f + 0.2f * i) + i;
    m_rot_y(&r1, a);
    m_rot_x(&r2, a * 0.6f);
    m_scale(&s, 1.0f + 0.15f * i);
    m_translate(&t, (i - 2) * 2.2f, 0.7f + 0.3f * sinf(g->t * 1.3f + i), 3.0f);
    m_mul(&tmp, &s, &r1);
    m_mul(&tmp, &tmp, &r2);
    m_mul(w, &tmp, &t);
}

/* particle i at time t: position and colour (additive) */
static void game_particle(const struct game *g, int i, float *pos, DWORD *color, float *size)
{
    float ph = fmodf(g->t * 0.5f + i * 0.137f, 1.0f);
    float ang = i * 2.399f + g->t * 0.3f;
    pos[0] = cosf(ang) * (1.0f + ph * 3.0f);
    pos[1] = -0.5f + ph * 4.0f;
    pos[2] = 3.0f + sinf(ang) * (1.0f + ph * 3.0f);
    *size = 0.15f + ph * 0.35f;
    {
        int a = (int)((1.0f - ph) * 200.0f);
        *color = ((DWORD)a << 24) | ((DWORD)(255 - (i * 3 & 127)) << 16) | ((DWORD)(128 + (i * 7 & 127)) << 8) | 40;
    }
}

/* screen-space frame-time bars, bottom-left; fills 6 vertices per bar (2 triangles) */
static int game_bars(const struct game *g, struct vtx_rhw *v, int w, int h)
{
    int i, n = 0;
    for (i = 0; i < BARS; i++) {
        float ms = g->bars[(g->bar_i + i) % BARS];
        float bh = ms * 3.0f;
        float x0 = 8.0f + i * 4.0f, x1 = x0 + 3.0f, y1 = h - 8.0f, y0 = y1 - (bh > 100.0f ? 100.0f : bh);
        DWORD c = ms > 20.0f ? 0xffff4040 : 0xff40ff40;
        v[n].x = x0; v[n].y = y0; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
        v[n].x = x1; v[n].y = y0; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
        v[n].x = x0; v[n].y = y1; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
        v[n].x = x1; v[n].y = y0; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
        v[n].x = x1; v[n].y = y1; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
        v[n].x = x0; v[n].y = y1; v[n].z = 0; v[n].rhw = 1; v[n].color = c; n++;
    }
    (void)w;
    return n;
}

/* ---- BMP writer: 24-bit from a locked 32-bit (X8R8G8B8) or 16-bit (565) surface ---- */

static int bmp_write(const char *path, const void *bits, int pitch, int w, int h, int bpp16)
{
    FILE *f = fopen(path, "wb");
    int y, x, row = (w * 3 + 3) & ~3;
    BYTE hdr[54];
    DWORD size = 54 + row * h;
    BYTE *line;
    if (!f) return 0;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    line = (BYTE *)calloc(row, 1);
    for (y = h - 1; y >= 0; y--) {
        const BYTE *src = (const BYTE *)bits + y * pitch;
        for (x = 0; x < w; x++) {
            if (bpp16) {
                WORD p = ((const WORD *)src)[x];
                line[x * 3 + 0] = (BYTE)((p & 0x1f) << 3);
                line[x * 3 + 1] = (BYTE)(((p >> 5) & 0x3f) << 2);
                line[x * 3 + 2] = (BYTE)(((p >> 11) & 0x1f) << 3);
            } else {
                line[x * 3 + 0] = src[x * 4 + 0];
                line[x * 3 + 1] = src[x * 4 + 1];
                line[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return 1;
}

/* ---- window ---- */

static LRESULT CALLBACK game_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    struct game *g = (struct game *)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (m) {
    case WM_DESTROY:
        if (g) g->quit = 1;
        return 0;
    case WM_KEYDOWN:
        if (g) {
            if (w == VK_F1) g->wireframe ^= 1;
            if (w == VK_SPACE) g->paused ^= 1;
        }
        break;
    case WM_SETCURSOR:
        if (g && g->o.fullscreen) { SetCursor(NULL); return TRUE; }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND game_window(struct game *g, const char *name)
{
    WNDCLASSA wc;
    HWND hwnd;
    DWORD style = g->o.fullscreen ? WS_POPUP : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    RECT r = { 0, 0, g->o.w, g->o.h };
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = game_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = name;
    RegisterClassA(&wc);
    AdjustWindowRect(&r, style, FALSE);
    hwnd = CreateWindowA(name, name, style, g->o.fullscreen ? 0 : CW_USEDEFAULT, g->o.fullscreen ? 0 : CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)g);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return hwnd;
}

static int game_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 0;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 1;
}

#endif /* D3DGAME_H */
