/*
 * ssebench: SSE throughput of the machine this runs on, real or emulated.
 * Runs the same handful of kernels every Direct3D-era game does in its
 * math library (D3DX-style vector transforms, normalizes, clamps,
 * float-to-int conversion) with SSE1 intrinsics, plus the same work in
 * plain C floats (x87 on this compiler) as the baseline, and prints
 * ns per operation for each. Run it on the reference rig (doc 09) and in
 * the guests with and without `-cpu ...,sse-fast=off` / `x87-fast=off`.
 *
 *   SSEBENCH [-iter N] [-quick] [-only prefix] [-pc24|-pc53|-pc64]
 *   N: repeat multiplier (default 1); -only runs the kernels whose name starts
 *   with prefix (e.g. -only convert); -pcNN sets the x87 precision control for
 *   the C kernels (default 53, what MSVC-built games run at; Direct3D sets 24;
 *   mingw's CRT startup leaves 64, where no x87 fast path applies).
 *
 * Output goes to the console and ssebench.log. Build (guest-tools/
 * build-wrappers.sh): i686-w64-mingw32-gcc -O2 -o ssebench.exe ssebench.c
 * (-march=pentium3: SSE1 only, x87 for scalar C, msvcrt).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <mmintrin.h>
#include <xmmintrin.h>

/*
 * The "C" kernels must stay scalar x87: gcc -O2 auto-vectorizes them into
 * SSE otherwise, and the x87-vs-SSE comparison is the point of them.
 */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

#define N 1024

static FILE *logfile;
static double freq;

static void out(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logfile) {
        fputs(buf, logfile);
        fflush(logfile);
    }
}

static double now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / freq;
}

typedef struct { float x, y, z, w; } vec4;

static vec4 in[N] __attribute__((aligned(16)));
static vec4 outv[N] __attribute__((aligned(16)));
static float fin[N] __attribute__((aligned(16)));
static int iout[N];
static float mat[16] __attribute__((aligned(16)));
static unsigned char pixa[N * 4] __attribute__((aligned(8)));
static unsigned char pixb[N * 4] __attribute__((aligned(8)));

static float lcg(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static void init_data(void)
{
    unsigned seed = 12345;
    int i;
    for (i = 0; i < N; i++) {
        in[i].x = lcg(&seed) * 200.0f - 100.0f;
        in[i].y = lcg(&seed) * 200.0f - 100.0f;
        in[i].z = lcg(&seed) * 200.0f - 100.0f;
        in[i].w = 1.0f;
        fin[i] = lcg(&seed) * 60000.0f - 30000.0f;
    }
    for (i = 0; i < 16; i++) {
        mat[i] = lcg(&seed) * 2.0f - 1.0f;
    }
    for (i = 0; i < N * 4; i++) {
        pixa[i] = (unsigned char)(lcg(&seed) * 256.0f);
        pixb[i] = (unsigned char)(lcg(&seed) * 256.0f);
    }
    mat[15] = 1.0f;
}

/* 1. packed transform: 4 mulps + 3 addps + 4 shuffles per vertex (D3DXVec4Transform) */
static float k_xform(int reps)
{
    __m128 sum = _mm_setzero_ps();
    int r, i;
    for (r = 0; r < reps; r++) {
        __m128 m0 = _mm_load_ps(mat), m1 = _mm_load_ps(mat + 4);
        __m128 m2 = _mm_load_ps(mat + 8), m3 = _mm_load_ps(mat + 12);
        mat[0] += 1e-7f;
        for (i = 0; i < N; i++) {
            __m128 v = _mm_load_ps(&in[i].x);
            __m128 x = _mm_shuffle_ps(v, v, 0x00);
            __m128 y = _mm_shuffle_ps(v, v, 0x55);
            __m128 z = _mm_shuffle_ps(v, v, 0xaa);
            __m128 w = _mm_shuffle_ps(v, v, 0xff);
            __m128 o = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m0), _mm_mul_ps(y, m1)),
                                  _mm_add_ps(_mm_mul_ps(z, m2), _mm_mul_ps(w, m3)));
            _mm_store_ps(&outv[i].x, o);
        }
        sum = _mm_add_ps(sum, _mm_load_ps(&outv[N - 1].x));
    }
    return _mm_cvtss_f32(sum);
}

/* 2. packed normalize: dot product, rsqrtps, one Newton step (D3DXVec3Normalize x4) */
static float k_normalize(int reps)
{
    __m128 half = _mm_set1_ps(0.5f), three = _mm_set1_ps(3.0f);
    __m128 sum = _mm_setzero_ps();
    int r, i;
    for (r = 0; r < reps; r++) {
        for (i = 0; i < N; i++) {
            __m128 v = _mm_load_ps(&in[i].x);
            __m128 sq = _mm_mul_ps(v, v);
            __m128 t = _mm_add_ps(sq, _mm_shuffle_ps(sq, sq, 0x4e));
            __m128 d = _mm_add_ps(t, _mm_shuffle_ps(t, t, 0xb1));
            __m128 rs = _mm_rsqrt_ps(d);
            rs = _mm_mul_ps(_mm_mul_ps(half, rs),
                            _mm_sub_ps(three, _mm_mul_ps(d, _mm_mul_ps(rs, rs))));
            _mm_store_ps(&outv[i].x, _mm_mul_ps(v, rs));
        }
        sum = _mm_add_ps(sum, _mm_load_ps(&outv[r & (N - 1)].x));
        in[r & (N - 1)].x += 1e-3f;
    }
    return _mm_cvtss_f32(sum);
}

/* 3. scalar chain: mulss/addss/divss/sqrtss/comiss, the shape of compiled scalar SSE code */
static float k_scalar(int reps)
{
    float acc = 0.0f;
    int r, i, hits = 0;
    for (r = 0; r < reps; r++) {
        __m128 a = _mm_set_ss(1.0001f), c = _mm_set_ss(0.25f);
        __m128 lim = _mm_set_ss(1000.0f);
        for (i = 0; i < N; i++) {
            __m128 v = _mm_load_ss(&fin[i]);
            v = _mm_add_ss(_mm_mul_ss(v, a), c);
            v = _mm_div_ss(v, _mm_add_ss(_mm_sqrt_ss(_mm_mul_ss(v, v)), a));
            if (_mm_comilt_ss(v, lim)) {
                hits++;
            }
            _mm_store_ss(&fin[i], _mm_min_ss(_mm_max_ss(v, _mm_set_ss(-30000.0f)), _mm_set_ss(30000.0f)));
        }
        acc += fin[r & (N - 1)];
    }
    return acc + hits;
}

/* 4. clamp and compare: minps/maxps, cmpps + movmskps (culling / bounds tests) */
static float k_clamp(int reps)
{
    __m128 lo = _mm_set1_ps(-50.0f), hi = _mm_set1_ps(50.0f);
    int r, i, inside = 0;
    for (r = 0; r < reps; r++) {
        __m128 th = _mm_set1_ps(10.0f + r * 1e-3f);
        for (i = 0; i < N; i++) {
            __m128 v = _mm_load_ps(&in[i].x);
            __m128 c = _mm_min_ps(_mm_max_ps(v, lo), hi);
            __m128 m = _mm_cmplt_ps(_mm_mul_ps(c, c), _mm_mul_ps(th, th));
            inside += _mm_movemask_ps(m);
            _mm_store_ps(&outv[i].x, c);
        }
    }
    return (float)inside;
}

/* 5. float <-> int: cvttss2si and cvtsi2ss (vertex quantization, fixed-point setup) */
static float k_convert(int reps)
{
    int r, i, acc = 0;
    for (r = 0; r < reps; r++) {
        float s = 1.0f + (r & 7) * 1e-4f;   /* 0.999 * s < 1: values decay slowly, stay in int range */
        for (i = 0; i < N; i++) {
            __m128 v = _mm_mul_ss(_mm_load_ss(&fin[i]), _mm_set_ss(s));
            int q = _mm_cvttss_si32(v);
            acc += q;
            iout[i] = q;
        }
        for (i = 0; i < N; i++) {
            _mm_store_ss(&fin[i], _mm_mul_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), iout[i]), _mm_set_ss(0.999f)));
        }
    }
    return (float)acc;
}

/* 6. the transform in plain C floats: x87 on this compiler (patch 06's path) */
static float k_c_xform(int reps)
{
    float sum = 0.0f;
    int r, i;
    for (r = 0; r < reps; r++) {
        mat[5] += 1e-7f;
        for (i = 0; i < N; i++) {
            float x = in[i].x, y = in[i].y, z = in[i].z, w = in[i].w;
            outv[i].x = x * mat[0] + y * mat[4] + z * mat[8] + w * mat[12];
            outv[i].y = x * mat[1] + y * mat[5] + z * mat[9] + w * mat[13];
            outv[i].z = x * mat[2] + y * mat[6] + z * mat[10] + w * mat[14];
            outv[i].w = x * mat[3] + y * mat[7] + z * mat[11] + w * mat[15];
        }
        sum += outv[N - 1].y;
    }
    return sum;
}

/* 7. scalar C normalize with sqrtf and a divide: x87 fsqrt/fdiv */
static float k_c_normalize(int reps)
{
    float sum = 0.0f;
    int r, i;
    for (r = 0; r < reps; r++) {
        for (i = 0; i < N; i++) {
            float x = in[i].x, y = in[i].y, z = in[i].z;
            float inv = 1.0f / (float)sqrt(x * x + y * y + z * z + 1e-30f);
            outv[i].x = x * inv;
            outv[i].y = y * inv;
            outv[i].z = z * inv;
        }
        sum += outv[r & (N - 1)].z;
    }
    return sum;
}

/* 9. MMX alpha blend of 32-bit pixels, two per iteration: the shape of a software blitter */
static float k_mmx_blend(int reps)
{
    __m64 zero = _mm_setzero_si64();
    unsigned sum = 0;
    int r, i;
    for (r = 0; r < reps; r++) {
        __m64 alpha = _mm_set1_pi16((short)(64 + (r & 127)));
        for (i = 0; i < N * 4; i += 8) {
            __m64 a = *(__m64 *)&pixa[i], b = *(__m64 *)&pixb[i];
            __m64 al = _mm_unpacklo_pi8(a, zero), ah = _mm_unpackhi_pi8(a, zero);
            __m64 bl = _mm_unpacklo_pi8(b, zero), bh = _mm_unpackhi_pi8(b, zero);
            __m64 dl = _mm_srai_pi16(_mm_mullo_pi16(_mm_sub_pi16(al, bl), alpha), 8);
            __m64 dh = _mm_srai_pi16(_mm_mullo_pi16(_mm_sub_pi16(ah, bh), alpha), 8);
            __m64 o = _mm_packs_pu16(_mm_add_pi16(bl, dl), _mm_add_pi16(bh, dh));
            o = _mm_avg_pu8(o, a);
            *(__m64 *)&pixb[i] = o;
        }
        sum += _mm_cvtsi64_si32(_mm_sad_pu8(*(__m64 *)&pixb[r & 255], *(__m64 *)&pixa[r & 255]));
    }
    _mm_empty();
    return (float)sum;
}

/* 8. decay into denormals: the slow-path cost when results go tiny (not in the score) */
static float k_denormal(int reps)
{
    __m128 f = _mm_set1_ps(0.75f);
    float sum = 0.0f;
    int r, i;
    for (r = 0; r < reps; r++) {
        for (i = 0; i < N; i++) {
            outv[i].x = 1e-38f * (i + 1) / N;
            outv[i].y = outv[i].z = outv[i].w = outv[i].x;
        }
        for (i = 0; i < 80; i++) {
            int j;
            for (j = 0; j < N; j++) {
                _mm_store_ps(&outv[j].x, _mm_mul_ps(_mm_load_ps(&outv[j].x), f));
            }
        }
        sum += outv[0].x;
    }
    return sum;
}

struct kernel {
    const char *name;
    float (*fn)(int reps);
    int reps;           /* ~0.3 s on a Pentium 4 1.7 */
    int ops;            /* SSE/x87 instructions per inner iteration */
    int score;
};

static struct kernel kernels[] = {
    { "xform (packed mul/add)",      k_xform,       400, 7,  1 },
    { "normalize (packed, rsqrtps)", k_normalize,   300, 12, 1 },
    { "scalar chain (ss ops)",       k_scalar,      300, 8,  1 },
    { "clamp+cmp (min/max/cmpps)",   k_clamp,       600, 4,  1 },
    { "convert (cvttss2si/cvtsi2ss)", k_convert,    600, 4,  1 },
    { "C xform (x87)",               k_c_xform,     300, 28, 0 },
    { "C normalize (x87 fsqrt/fdiv)", k_c_normalize, 300, 12, 0 },
    { "denormal decay (slow path)",  k_denormal,    3,   80, 0 },
    { "MMX blend (unpck/mul/pack/avg)", k_mmx_blend, 300, 14, 0 },
};

int main(int argc, char **argv)
{
    LARGE_INTEGER f;
    int mult = 1, quick = 0, i;
    const char *only = NULL;
    unsigned pc = _PC_53;
    double score = 0.0;
    int nscore = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-iter") && i + 1 < argc) {
            mult = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-quick")) {
            quick = 1;
        } else if (!strcmp(argv[i], "-only") && i + 1 < argc) {
            only = argv[++i];
        } else if (!strcmp(argv[i], "-pc24")) {
            pc = _PC_24;
        } else if (!strcmp(argv[i], "-pc53")) {
            pc = _PC_53;
        } else if (!strcmp(argv[i], "-pc64")) {
            pc = _PC_64;
        }
    }
    logfile = fopen("ssebench.log", "w");
    QueryPerformanceFrequency(&f);
    freq = (double)f.QuadPart;
    _controlfp(pc, _MCW_PC);
    out("ssebench: SSE %s, SSE2 %s, x87 CW %04x (PC=%s), %d KB working set, mult %d\n",
        IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE) ? "yes" : "NO",
        IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) ? "yes" : "no",
        _control87(0, 0) & 0xffff, pc == _PC_24 ? "24" : pc == _PC_53 ? "53" : "64",
        (int)((sizeof(in) + sizeof(outv) + sizeof(fin)) / 1024), mult);
    if (!IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE)) {
        out("no SSE, giving up\n");
        return 1;
    }
    out("%-32s %10s %9s %9s %s\n", "kernel", "ms", "ns/iter", "ns/op", "check");
    for (i = 0; i < (int)(sizeof(kernels) / sizeof(kernels[0])); i++) {
        struct kernel *k = &kernels[i];
        int reps = k->reps * mult / (quick ? 4 : 1);
        double t0, t1, iters;
        float chk;
        if (only && strncmp(k->name, only, strlen(only))) {
            continue;
        }
        if (reps < 1) {
            reps = 1;
        }
        init_data();
        k->fn(1);                       /* warm: translate, fault pages in */
        t0 = now();
        chk = k->fn(reps);
        t1 = now();
        iters = (double)reps * N;
        out("%-32s %10.1f %9.1f %9.2f %g\n", k->name, (t1 - t0) * 1e3,
            (t1 - t0) * 1e9 / iters, (t1 - t0) * 1e9 / iters / k->ops, chk);
        if (k->score) {
            score += (t1 - t0) * 1e9 / iters / k->ops;
            nscore++;
        }
    }
    if (nscore) {
        out("SSE score: %.2f ns per SSE op (mean of the %d SSE kernels; lower is better)\n",
            score / nscore, nscore);
    }
    out("done\n");
    if (logfile) {
        fclose(logfile);
    }
    return 0;
}
