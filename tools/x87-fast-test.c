/*
 * Oracle test for qemu/target/i386/tcg/x87-fast.h: on an x86-64 host, run
 * the fast path and the real x87 (the thing both it and QEMU's softfloat
 * imitate) on the same operands and demand identical results and inexact
 * flags whenever the fast path accepts. Random operands over the whole
 * representable range plus edge cases; both precisions; all rounding modes
 * for the integer conversions.
 *
 * Build & run (Linux x86-64):
 *   cc -O2 -std=gnu11 -Iqemu/target/i386/tcg -o build/x87-fast-test \
 *      tools/x87-fast-test.c && build/x87-fast-test
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t low;
    uint16_t high;
} floatx80;

#include "x87-fast.h"

#if !defined(__x86_64__) && !defined(__i386__)
int main(void) { puts("x87 oracle needs an x86 host"); return 77; }
#else

#define FSW_PE 0x20
#define FSW_IE 0x01

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd64(void)
{
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* control word: precision 0=24, 2=53, 3=64; rounding 0..3; all masked */
static uint16_t make_cw(int pc, int rc)
{
    return 0x003f | (pc << 8) | (rc << 10);
}

typedef struct {
    floatx80 r;
    uint16_t sw;
} x87_result;

/* real x87: a OP b with control word cw */
static x87_result x87_binop(int op, uint16_t cw, floatx80 a, floatx80 b)
{
    x87_result out;
    uint8_t ta[16] = {0}, tb[16] = {0}, tr[16] = {0};
    uint16_t sw = 0;

    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
    memcpy(tb, &b.low, 8); memcpy(tb + 8, &b.high, 2);
#define BINOP_ASM(insn) \
    __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[b]\n\tfldt %[a]\n\t" \
                     insn "\n\tfstpt %[r]\n\tfnstsw %[sw]\n\tfninit" \
                     : [r] "=m"(*(uint8_t(*)[16])tr), [sw] "=m"(sw) \
                     : [a] "m"(*(uint8_t(*)[16])ta), [b] "m"(*(uint8_t(*)[16])tb), \
                       [cw] "m"(cw) : "memory")
    switch (op) {
    case X87F_ADD: BINOP_ASM("faddp"); break;
    case X87F_SUB: BINOP_ASM("fsubp"); break;
    case X87F_MUL: BINOP_ASM("fmulp"); break;
    default:       BINOP_ASM("fdivp"); break;
    }
#undef BINOP_ASM
    memcpy(&out.r.low, tr, 8); memcpy(&out.r.high, tr + 8, 2);
    out.sw = sw;
    return out;
}

static x87_result x87_unop(const char *which, uint16_t cw, floatx80 a)
{
    x87_result out;
    uint8_t ta[16] = {0}, tr[16] = {0};
    uint16_t sw = 0;

    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
    if (which[1] == 's') {
        __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[a]\n\tfsqrt\n\tfstpt %[r]\n\t"
                         "fnstsw %[sw]\n\tfninit"
                         : [r] "=m"(*(uint8_t(*)[16])tr), [sw] "=m"(sw)
                         : [a] "m"(*(uint8_t(*)[16])ta), [cw] "m"(cw) : "memory");
    } else {
        __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[a]\n\tfrndint\n\tfstpt %[r]\n\t"
                         "fnstsw %[sw]\n\tfninit"
                         : [r] "=m"(*(uint8_t(*)[16])tr), [sw] "=m"(sw)
                         : [a] "m"(*(uint8_t(*)[16])ta), [cw] "m"(cw) : "memory");
    }
    memcpy(&out.r.low, tr, 8); memcpy(&out.r.high, tr + 8, 2);
    out.sw = sw;
    return out;
}
#define x87_sqrt(cw, a) x87_unop("fsqrt", cw, a)
#define x87_frndint(cw, a) x87_unop("frndint", cw, a)

/* fistp (32/64) or fisttp */
static int64_t x87_fist(uint16_t cw, floatx80 a, int bits, bool trunc,
                        uint16_t *sw_out)
{
    uint8_t ta[16] = {0};
    int64_t r64 = 0;
    int32_t r32 = 0;
    uint16_t sw = 0;

    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
#define FIST_ASM(insn, dst) \
    __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[a]\n\t" insn " %[r]\n\t" \
                     "fnstsw %[sw]\n\tfninit" \
                     : [r] "=m"(dst), [sw] "=m"(sw) \
                     : [a] "m"(*(uint8_t(*)[16])ta), [cw] "m"(cw) : "memory")
    if (bits == 32) {
        if (trunc) FIST_ASM("fisttpl", r32); else FIST_ASM("fistpl", r32);
        *sw_out = sw;
        return r32;
    }
    if (trunc) FIST_ASM("fisttpq", r64); else FIST_ASM("fistpq", r64);
#undef FIST_ASM
    *sw_out = sw;
    return r64;
}

static uint64_t x87_fstl(floatx80 a, uint16_t *sw_out)
{
    uint8_t ta[16] = {0};
    uint64_t r = 0;
    uint16_t sw = 0, cw = make_cw(3, 0);

    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
    __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[a]\n\tfstpl %[r]\n\tfnstsw %[sw]\n\tfninit"
                     : [r] "=m"(r), [sw] "=m"(sw)
                     : [a] "m"(*(uint8_t(*)[16])ta), [cw] "m"(cw) : "memory");
    *sw_out = sw;
    return r;
}

static uint32_t x87_fsts(floatx80 a, uint16_t *sw_out)
{
    uint8_t ta[16] = {0};
    uint32_t r = 0;
    uint16_t sw = 0, cw = make_cw(3, 0);

    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
    __asm__ volatile("fnclex\n\tfldcw %[cw]\n\tfldt %[a]\n\tfstps %[r]\n\tfnstsw %[sw]\n\tfninit"
                     : [r] "=m"(r), [sw] "=m"(sw)
                     : [a] "m"(*(uint8_t(*)[16])ta), [cw] "m"(cw) : "memory");
    *sw_out = sw;
    return r;
}

static floatx80 x87_fldl(uint64_t bits)
{
    uint8_t tr[16] = {0};
    floatx80 r;
    __asm__ volatile("fldl %[v]\n\tfstpt %[r]" : [r] "=m"(*(uint8_t(*)[16])tr) : [v] "m"(bits) : "memory");
    memcpy(&r.low, tr, 8); memcpy(&r.high, tr + 8, 2);
    return r;
}

static floatx80 x87_flds(uint32_t bits)
{
    uint8_t tr[16] = {0};
    floatx80 r;
    __asm__ volatile("flds %[v]\n\tfstpt %[r]" : [r] "=m"(*(uint8_t(*)[16])tr) : [v] "m"(bits) : "memory");
    memcpy(&r.low, tr, 8); memcpy(&r.high, tr + 8, 2);
    return r;
}

static floatx80 x87_fildq(int64_t v)
{
    uint8_t tr[16] = {0};
    floatx80 r;
    __asm__ volatile("fildq %[v]\n\tfstpt %[r]" : [r] "=m"(*(uint8_t(*)[16])tr) : [v] "m"(v) : "memory");
    memcpy(&r.low, tr, 8); memcpy(&r.high, tr + 8, 2);
    return r;
}

static int x87_fcom(floatx80 a, floatx80 b)
{
    uint8_t ta[16] = {0}, tb[16] = {0};
    uint16_t sw = 0;
    memcpy(ta, &a.low, 8); memcpy(ta + 8, &a.high, 2);
    memcpy(tb, &b.low, 8); memcpy(tb + 8, &b.high, 2);
    __asm__ volatile("fnclex\n\tfldt %[b]\n\tfldt %[a]\n\tfcompp\n\tfnstsw %[sw]\n\tfninit"
                     : [sw] "=m"(sw)
                     : [a] "m"(*(uint8_t(*)[16])ta), [b] "m"(*(uint8_t(*)[16])tb) : "memory");
    /* C3 (0x4000) zero, C0 (0x0100) less, C2 (0x0400) unordered */
    if (sw & 0x0400) return 2;
    if (sw & 0x4000) return 0;
    if (sw & 0x0100) return -1;
    return 1;
}

/* ---- operand generators ---- */

static floatx80 gen_x80(void)
{
    floatx80 a;
    uint64_t r = rnd64();
    int kind = r & 15;
    int e;

    switch (kind) {
    case 0:  /* zero */
        a.high = (r >> 4) & 0x8000; a.low = 0; return a;
    case 1:  /* small integer */
        return x87_fildq((int64_t)(rnd64() % 2001) - 1000);
    case 2:  /* random float32 */
        return x87_flds((uint32_t)rnd64());
    case 3: case 4: case 5:  /* random float64 */
        return x87_fldl(rnd64());
    case 6:  /* double near the window edge */
        e = 1023 + (int)(rnd64() % 200) + 800;
        return x87_fldl(((rnd64() >> 12) & 0x800fffffffffffffULL) | ((uint64_t)e << 52));
    case 7:  /* double tiny, near/below the window */
        e = 1023 - (int)(rnd64() % 200) - 800;
        if (e < 1) e = 1;
        return x87_fldl(((rnd64() >> 12) & 0x800fffffffffffffULL) | ((uint64_t)e << 52));
    case 8:  /* 64-bit mantissa, moderate exponent */
        a.high = ((r >> 4) & 0x8000) | (16383 + (int)(rnd64() % 200) - 100);
        a.low = rnd64() | (1ULL << 63); return a;
    case 9:  /* denormal / pseudo-denormal / unnormal */
        a.high = (r >> 4) & 0x8000; a.low = rnd64() >> (rnd64() % 64); return a;
    case 10: /* inf / nan */
        a.high = ((r >> 4) & 0x8000) | 0x7fff;
        a.low = (1ULL << 63) | ((r >> 5) & 1 ? rnd64() >> 1 : 0); return a;
    case 11: /* subnormal double */
        return x87_fldl(rnd64() & 0x800fffffffffffffULL);
    case 12: /* exact float-ish with float exponent, wide window */
        return x87_flds(((uint32_t)rnd64() & 0x807fffff) | ((uint32_t)(127 + (int)(rnd64() % 200) - 100) << 23));
    default: /* double in the middle */
        e = 1023 + (int)(rnd64() % 400) - 200;
        return x87_fldl(((rnd64() >> 12) & 0x800fffffffffffffULL) | ((uint64_t)e << 52));
    }
}

static int failures;
static long accepted, tried;

static void fail(const char *what, floatx80 a, floatx80 b, floatx80 got, floatx80 want,
                 bool inex, bool want_inex)
{
    if (failures++ < 20) {
        fprintf(stderr, "FAIL %s a=%04x:%016llx b=%04x:%016llx got %04x:%016llx (inexact %d) "
                "want %04x:%016llx (inexact %d)\n", what, a.high, (unsigned long long)a.low,
                b.high, (unsigned long long)b.low, got.high, (unsigned long long)got.low, inex,
                want.high, (unsigned long long)want.low, want_inex);
    }
}

static void test_binops(long n)
{
    static const char *names[] = {"add", "sub", "mul", "div"};
    for (long i = 0; i < n; i++) {
        floatx80 a = gen_x80(), b = gen_x80(), r;
        int op = rnd64() & 3;
        int prec = rnd64() & 1 ? X87F_PREC_D : X87F_PREC_S;
        bool inex;
        tried++;
        if (!x87f_binop(op, prec, a, b, &r, &inex)) {
            continue;
        }
        accepted++;
        x87_result want = x87_binop(op, make_cw(prec == X87F_PREC_D ? 2 : 0, 0), a, b);
        if (r.low != want.r.low || r.high != want.r.high || inex != !!(want.sw & FSW_PE)
            || (want.sw & 0x3f & ~FSW_PE)) {
            fail(names[op], a, b, r, want.r, inex, want.sw & FSW_PE);
        }
    }
}

static void test_sqrt(long n)
{
    for (long i = 0; i < n; i++) {
        floatx80 a = gen_x80(), r, z = {0, 0};
        int prec = rnd64() & 1 ? X87F_PREC_D : X87F_PREC_S;
        bool inex;
        tried++;
        if (!x87f_sqrt(prec, a, &r, &inex)) {
            continue;
        }
        accepted++;
        x87_result want = x87_sqrt(make_cw(prec == X87F_PREC_D ? 2 : 0, 0), a);
        if (r.low != want.r.low || r.high != want.r.high || inex != !!(want.sw & FSW_PE)
            || (want.sw & 0x3f & ~FSW_PE)) {
            fail("sqrt", a, z, r, want.r, inex, want.sw & FSW_PE);
        }
    }
}

static void test_conv(long n)
{
    for (long i = 0; i < n; i++) {
        floatx80 a = gen_x80(), r, z = {0, 0};
        uint64_t b64;
        uint32_t b32;
        uint16_t sw;
        int64_t iv;
        bool inex;
        int rc = rnd64() & 3;

        tried++;
        if (x87f_to_f64(a, &b64)) {
            accepted++;
            uint64_t want = x87_fstl(a, &sw);
            if (want != b64 || (sw & 0x3f)) {
                floatx80 g = {b64, 0}, w = {want, sw};
                fail("fstl", a, z, g, w, 0, 0);
            }
        }
        if (x87f_to_f32(a, &b32)) {
            accepted++;
            uint32_t want = x87_fsts(a, &sw);
            if (want != b32 || (sw & 0x3f)) {
                floatx80 g = {b32, 0}, w = {want, sw};
                fail("fsts", a, z, g, w, 0, 0);
            }
        }
        /* loads */
        b64 = rnd64();
        if (x87f_from_f64(b64, &r)) {
            floatx80 want = x87_fldl(b64);
            if (r.low != want.low || r.high != want.high) {
                floatx80 in = {b64, 0};
                fail("fldl", in, z, r, want, 0, 0);
            }
        }
        b32 = (uint32_t)rnd64();
        if (x87f_from_f32(b32, &r)) {
            floatx80 want = x87_flds(b32);
            if (r.low != want.low || r.high != want.high) {
                floatx80 in = {b32, 0};
                fail("flds", in, z, r, want, 0, 0);
            }
        }
        iv = (int64_t)rnd64() >> (rnd64() % 64);
        r = x87f_from_i64(iv);
        {
            floatx80 want = x87_fildq(iv);
            if (r.low != want.low || r.high != want.high) {
                floatx80 in = {(uint64_t)iv, 0};
                fail("fild", in, z, r, want, 0, 0);
            }
        }
        /* integer stores, all rounding modes, 32 and 64 bit, + truncating */
        if (x87f_to_int(a, rc, -2147483648.0, 2147483648.0, &iv, &inex)) {
            accepted++;
            int64_t want = x87_fist(make_cw(3, rc), a, 32, false, &sw);
            if (want != iv || inex != !!(sw & FSW_PE) || (sw & FSW_IE)) {
                floatx80 g = {(uint64_t)iv, rc}, w = {(uint64_t)want, sw};
                fail("fistl", a, z, g, w, inex, sw & FSW_PE);
            }
        }
        if (x87f_to_int(a, rc, -9223372036854775808.0, 9223372036854775808.0, &iv, &inex)) {
            accepted++;
            int64_t want = x87_fist(make_cw(3, rc), a, 64, false, &sw);
            if (want != iv || inex != !!(sw & FSW_PE) || (sw & FSW_IE)) {
                floatx80 g = {(uint64_t)iv, rc}, w = {(uint64_t)want, sw};
                fail("fistll", a, z, g, w, inex, sw & FSW_PE);
            }
        }
        if (x87f_to_int(a, X87F_RND_ZERO, -2147483648.0, 2147483648.0, &iv, &inex)) {
            int64_t want = x87_fist(make_cw(3, rc), a, 32, true, &sw);
            if (want != iv || inex != !!(sw & FSW_PE) || (sw & FSW_IE)) {
                floatx80 g = {(uint64_t)iv, 9}, w = {(uint64_t)want, sw};
                fail("fisttl", a, z, g, w, inex, sw & FSW_PE);
            }
        }
        /* frndint */
        if (x87f_round_to_int(a, rc, &r, &inex)) {
            accepted++;
            x87_result want = x87_frndint(make_cw(3, rc), a);
            if (r.low != want.r.low || r.high != want.r.high || inex != !!(want.sw & FSW_PE)
                || (want.sw & 0x3f & ~FSW_PE)) {
                floatx80 rcx = {0, rc};
                fail("frndint", a, rcx, r, want.r, inex, want.sw & FSW_PE);
            }
        }
        /* compare */
        {
            floatx80 b = gen_x80();
            int rel;
            if (x87f_compare(a, b, &rel)) {
                accepted++;
                int want = x87_fcom(a, b);
                if (want != rel) {
                    floatx80 g = {0, (uint16_t)rel}, w = {0, (uint16_t)want};
                    fail("fcom", a, b, g, w, 0, 0);
                }
            }
        }
    }
}

/* hand-picked edge cases through the binops */
static void test_edges(void)
{
    static const floatx80 v[] = {
        {0, 0}, {0, 0x8000},                                   /* ±0 */
        {0x8000000000000000ULL, 0x3fff}, {0x8000000000000000ULL, 0xbfff}, /* ±1 */
        {0x8000000000000000ULL, 0x3ffe},                       /* 0.5 */
        {0xc000000000000000ULL, 0x3fff},                       /* 1.5 */
        {0x8000000000000000ULL, 0x3fff + 900},                 /* 2^900 */
        {0x8000000000000000ULL, 0x3fff + 901},                 /* 2^901 */
        {0x8000000000000000ULL, 0x3fff - 900},                 /* 2^-900 */
        {0x8000000000000000ULL, 0x3fff - 901},
        {0x8000000000000000ULL, 0x3fff - 1022},                /* DBL_MIN */
        {0xfffffffffffff800ULL, 0x3fff + 1023},                /* DBL_MAX */
        {0xfffffffffffff800ULL, 0x3fff + 899},
        {0xfffffffffffff800ULL, 0x3fff + 900},
        {0x8000000000000001ULL, 0x3fff},                       /* 1 + 2^-63 */
        {0xffffffffffffffffULL, 0x3fff},
        {0x8000000000000000ULL, 0x7fff}, {0x8000000000000000ULL, 0xffff}, /* ±inf */
        {0xc000000000000000ULL, 0x7fff},                       /* qnan */
        {0xa000000000000000ULL, 0x7fff},                       /* snan */
        {0x0000000000000001ULL, 0x0000},                       /* denormal */
        {0x8000000000000000ULL, 0x0000},                       /* pseudo-denormal */
        {0x4000000000000000ULL, 0x3fff},                       /* unnormal */
        {0xb400000000000000ULL, 0x3fff},                       /* 1.40625 (float-exact) */
        {0xb4000000000000ffULL, 0x3fff},
    };
    int n = sizeof(v) / sizeof(v[0]);
    static const char *names[] = {"add", "sub", "mul", "div"};
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            for (int op = 0; op < 4; op++) {
                for (int prec = 0; prec < 2; prec++) {
                    floatx80 r;
                    bool inex;
                    tried++;
                    if (!x87f_binop(op, prec, v[i], v[j], &r, &inex)) {
                        continue;
                    }
                    accepted++;
                    x87_result want = x87_binop(op, make_cw(prec == X87F_PREC_D ? 2 : 0, 0), v[i], v[j]);
                    if (r.low != want.r.low || r.high != want.r.high || inex != !!(want.sw & FSW_PE)
                        || (want.sw & 0x3f & ~FSW_PE)) {
                        fail(names[op], v[i], v[j], r, want.r, inex, want.sw & FSW_PE);
                    }
                }
            }
        }
        for (int prec = 0; prec < 2; prec++) {
            floatx80 r, z = {0, 0};
            bool inex;
            if (x87f_sqrt(prec, v[i], &r, &inex)) {
                x87_result want = x87_sqrt(make_cw(prec == X87F_PREC_D ? 2 : 0, 0), v[i]);
                if (r.low != want.r.low || r.high != want.r.high || inex != !!(want.sw & FSW_PE)
                    || (want.sw & 0x3f & ~FSW_PE)) {
                    fail("sqrt", v[i], z, r, want.r, inex, want.sw & FSW_PE);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 2000000;

    test_edges();
    test_binops(n);
    test_sqrt(n / 4);
    test_conv(n / 4);
    printf("x87-fast oracle: %ld tried, %ld accepted by the fast path, %d failures\n",
           tried, accepted, failures);
    return failures ? 1 : 0;
}
#endif
