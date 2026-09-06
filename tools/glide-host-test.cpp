/*
 * Glide pass-through end to end without a guest (doc 12 §5): the real
 * host-side wrapper (build/glide/libglide2x.so), loaded by hw/3dfx's own
 * dispatcher, opened through glidewnd.c's own handshake, rendering into
 * the embed library's window-less context, with the frame checked where
 * the player would receive it.
 *
 * What each stage proves:
 *
 *   1. init_glide2x()  — the wrapper is found (QEMU_GLIDE_LIB), its 121
 *      Glide entry points resolve undecorated, and patch 33 hands it the
 *      GlideHostOps table the embed provider returns.
 *   2. init_window() + stat_window() — glidewnd.c's resolution encoding
 *      and the provider's window_stat run the wrapper's real
 *      grSstWinOpen through cwnd_glide2x, on a context nobody has a
 *      window for. This is the exact sequence glidept_mm.c performs when
 *      a guest calls grSstWinOpen; only the MMIO decode is missing.
 *   3. a clear and a triangle through the wrapper, then grBufferSwap —
 *      on_3d_frame arrives at the frontend with the drawn pixels, so the
 *      whole path from a Glide call to the player's texture is closed.
 *   4. fini_window()/fini_glide2x() — the context is given back and the
 *      frontend is told 3D is over.
 *
 * C++ because the Glide SDK header is (sdk2_3dfx.h includes <cstdint>).
 * Linux only (EGL backend). Build & run from the repo root:
 *   c++ -O1 -std=c++17 -Iembed -Ithird_party/openglide -Iqemu/hw/3dfx \
 *      -o build/glide-host-test tools/glide-host-test.cpp \
 *      -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,$PWD/build/qemu -ldl \
 *   && QEMU_GLIDE_LIB=$PWD/build/glide/libglide2x.so build/glide-host-test
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdk2_glide.h"   /* the wrapper's own ABI, from third_party/openglide */
#include "g2xfuncs.h"     /* FEnum_*, from the prepared qemu/hw/3dfx */
#include "libqemu_embed.h"

/* hw/3dfx internals, exported by the embed library on Linux */
typedef struct {
    int activate;
    uint32_t *arg;
    uint32_t FEnum;
    uintptr_t GrContext;
} window_cb;
extern "C" {
int init_glide2x(const char *dllname);
void fini_glide2x(void);
void init_window(const int res, const char *title, void *opaque);
int stat_window(const int res, void *opaque);
void fini_window(void *opaque);
const void *glide_host_ops(void);
}

#define GR_RESOLUTION_640x480 0x7

static int actives, frames, fw, fh;
static uint32_t px_c, px_tl, px_br;
static uint32_t *last_frame;

/* GLIDE_TEST_BMP=<path> writes the frame out, for looking at a failure */
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t n = (uint32_t)w * h * 4, sz = n + 54;
    FILE *f = fopen(path, "wb");
    int y, x;

    if (!f) {
        return;
    }
    memcpy(hdr + 2, &sz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 32;
    memcpy(hdr + 34, &n, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = h - 1; y >= 0; y--) {          /* BMP rows run bottom-up */
        for (x = 0; x < w; x++) {
            uint32_t p = px[y * w + x] | 0xff000000u;
            fwrite(&p, 4, 1, f);
        }
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void on_3d_active(void *ud, bool on)
{
    actives++;
    printf("on_3d_active(%d)\n", on);
}

static void on_3d_frame(void *ud, const uint8_t *p, int w, int h, int stride)
{
    const uint32_t *row = (const uint32_t *)p;
    frames++;
    fw = w;
    fh = h;
    px_c = row[(h / 2) * (stride / 4) + (w / 2)];
    px_tl = row[10 * (stride / 4) + 10];
    px_br = row[(h - 10) * (stride / 4) + (w - 10)];
    last_frame = (uint32_t *)realloc(last_frame, (size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        memcpy(last_frame + (size_t)y * w, p + (size_t)y * stride, (size_t)w * 4);
    }
}

/* the wrapper's entry points, looked up the way hw/3dfx looks them up */
static void *wrapper;
static void *sym(const char *name)
{
    void *p = dlsym(wrapper, name);
    if (!p) {
        printf("  wrapper has no %s\n", name);
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *bios = argc > 1 ? argv[1] : "qemu/pc-bios";
    const char *lib = getenv("QEMU_GLIDE_LIB");
    char *qargv[] = {
        "qemu-system-i386", "-machine", "pc", "-m", "32", "-net", "none",
        "-L", (char *)bios, "-nodefaults", "-vga", "std",
    };
    qemu_embed_display_cb cb = {
        .on_3d_active = on_3d_active,
        .on_3d_frame = on_3d_frame,
        /* readback only: the check is the pixels, not the transport, and
         * tools/embed-3d-test already guards the dma-buf ring */
    };
    window_cb disp = { };
    uint32_t arg[16] = { 0 };
    int ok = 1, stat;

    /* Unset is the interesting case: hw/3dfx's own candidate list has to
     * find the build tree by itself. We still need the path to reach the
     * wrapper's gr* directly below, and dlopen of an already-loaded library
     * returns the same handle, so use the same relative name it does. */
    if (!lib || !*lib) {
        lib = "build/glide/libglide2x.so";
        printf("QEMU_GLIDE_LIB unset: expecting hw/3dfx to find %s\n", lib);
    }
    if (qemu_embed_api_version() != QEMU_EMBED_API_VERSION) {
        printf("API version mismatch\n");
        return 1;
    }
    if (!qemu_embed_new(sizeof(qargv) / sizeof(qargv[0]), qargv, &cb, NULL)) {
        printf("qemu_embed_new failed\n");
        return 1;
    }
    /* BQL is held by this thread after init, as on a vCPU thread */

    /* 1. the dispatcher loads the wrapper and gives it our context */
    if (glide_host_ops() == NULL) {
        printf("the embed provider offers no host ops (no EGL?)\n");
        return 1;
    }
    if (init_glide2x("glide2x.dll") != 0) {
        printf("init_glide2x failed: %s not loadable\n", lib);
        return 1;
    }
    printf("wrapper loaded: %s\n", lib);
    wrapper = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
    if (!wrapper) {
        printf("the wrapper is loaded but not by this name?\n");
        return 1;
    }

    auto grGlideInit_ = (void (*)(void))sym("grGlideInit");
    auto grSstSelect_ = (void (*)(int))sym("grSstSelect");
    auto grBufferClear_ =
        (void (*)(GrColor_t, GrAlpha_t, FxU32))sym("grBufferClear");
    auto grBufferSwap_ = (void (*)(int))sym("grBufferSwap");
    auto grDrawTriangle_ =
        (void (*)(const void *, const void *, const void *))sym("grDrawTriangle");
    auto grColorCombine_ =
        (void (*)(GrCombineFunction_t, GrCombineFactor_t, GrCombineLocal_t,
                  GrCombineOther_t, FxBool))sym("grColorCombine");
    if (!grGlideInit_ || !grSstSelect_ || !grBufferClear_ || !grBufferSwap_
        || !grDrawTriangle_ || !grColorCombine_) {
        return 1;
    }
    grGlideInit_();
    grSstSelect_(0);

    /* 2. glidewnd.c's own handshake: what glidept_mm.c does for a guest's
     *    grSstWinOpen, then the guest's poll of MMIO 0xFB8. */
    disp.FEnum = FEnum_grSstWinOpen;
    disp.arg = arg;
    arg[1] = GR_RESOLUTION_640x480;
    arg[2] = GR_REFRESH_60Hz;
    arg[3] = GR_COLORFORMAT_ABGR;
    arg[4] = GR_ORIGIN_UPPER_LEFT;
    arg[5] = 2;   /* double buffered */
    arg[6] = 1;   /* one aux buffer */
    init_window(GR_RESOLUTION_640x480, "Glide2x", &disp);
    stat = stat_window(GR_RESOLUTION_640x480, &disp);
    printf("stat_window -> %d (0 = the window is up), GrContext %p\n",
           stat, (void *)disp.GrContext);
    if (stat != 0 || !disp.GrContext) {
        printf("grSstWinOpen refused\n");
        return 1;
    }
    ok = ok && actives == 1;

    /* 3. draw through Glide and swap */
    grColorCombine_(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                    GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    /* 0x0000ff in ABGR is red; the clear colour is the frame's background */
    grBufferClear_(0x0000ff, 0, 0);
    /*
     * A green triangle over the *upper-left half* of the 640x480 Glide
     * window. Glide was opened with GR_ORIGIN_UPPER_LEFT, so this is the
     * orientation check as much as the drawing one: get the flip wrong
     * anywhere between OpenGLide, the FBO and the readback and the two
     * corners below simply swap. A centred shape would pass either way.
     */
    {
        GrVertex a = { }, b = { }, c = { };
        a.x = 0.f;   a.y = 0.f;
        b.x = 639.f; b.y = 0.f;
        c.x = 0.f;   c.y = 479.f;
        a.r = b.r = c.r = 0.f;
        a.g = b.g = c.g = 255.f;
        a.b = b.b = c.b = 0.f;
        a.a = b.a = c.a = 255.f;
        a.oow = b.oow = c.oow = 1.f;
        grDrawTriangle_(&a, &b, &c);
    }
    grBufferSwap_(0);
    printf("frame %d: %dx%d top-left %08x bottom-right %08x centre %08x\n",
           frames, fw, fh, px_tl, px_br, px_c);
    if (getenv("GLIDE_TEST_BMP") && last_frame) {
        write_bmp(getenv("GLIDE_TEST_BMP"), last_frame, fw, fh);
    }
    ok = ok && frames == 1 && fw == 640 && fh == 480
            && (px_tl & 0xffffff) == 0x00ff00     /* the triangle, top-left */
            && (px_br & 0xffffff) == 0xff0000;    /* the clear, bottom-right */

    /* a second swap with nothing drawn: the clear colour everywhere */
    grBufferClear_(0x0000ff, 0, 0);
    grBufferSwap_(0);
    ok = ok && frames == 2 && (px_tl & 0xffffff) == 0xff0000
            && (px_c & 0xffffff) == 0xff0000;

    /* 4. close, exactly as processFRet does for grSstWinClose */
    disp.FEnum = FEnum_grSstWinClose;
    fini_window(&disp);
    fini_glide2x();
    ok = ok && actives == 2;

    printf("actives %d frames %d -> %s\n", actives, frames, ok ? "OK" : "FAIL");
    fflush(stdout);
    /* one VM per process; cleanup is partial — exit without it */
    _exit(ok ? 0 : 1);
}
