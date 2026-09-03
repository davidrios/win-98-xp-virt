/*
 * QEMU MESA GL Pass-Through — window-less context backend for the embed
 * library (win98-xp-virt, M3; docs/12-m3-context-provider.md).
 *
 * Port of hw/mesa/mglcntx_linux.c (GLX on an X11 window) to run without a
 * window or display server. Everything the guest wrapper needs (WGL
 * pixel formats, extension string, context levels, pbuffer emulation,
 * gamma/swap-interval stubs) is shared; a small platform layer provides
 * the context and the drawable that stands in for the window:
 *
 *   Linux  — EGL on the surfaceless platform, pbuffer surface = FBO 0.
 *   macOS  — CGL context with no drawable; an FBO (renderbuffers) plays the
 *            default framebuffer and framebuffer binding 0 is redirected to
 *            it in the dispatch table (MesaGLSetFunc, patch 32), since CGL
 *            pbuffers are deprecated and no window exists.
 *
 * All GL runs on the vCPU thread under the BQL exactly as upstream. The
 * presented frame is read back after each swap and handed to the frontend
 * (embedfx.c → libqemu_embed.c on_3d_frame) — the bring-up path; the
 * zero-copy dma-buf / IOSurface export replaces the readback later.
 *
 * The native backends in hw/mesa are linked weak (patch 31) so these
 * definitions take over in libqemu-embed while qemu-system keeps its own.
 *
 * Copyright (c) 2020 (upstream backend)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "hw/mesa/mesagl_impl.h"
#include "embedfx.h"

#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "glcntx: " fmt "\n" , ## __VA_ARGS__); } while(0)
#define DPRINTF_COND(cond,fmt, ...) \
    if (cond) { fprintf(stderr, "glcntx: " fmt "\n" , ## __VA_ARGS__); }

#if defined(CONFIG_LINUX) || defined(CONFIG_DARWIN)

/* ----------------------------------------------------- shared WGL layer */

#define GL_CONTEXTALPHA 1

typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
typedef struct tagPIXELFORMATDESCRIPTOR {
  WORD  nSize;
  WORD  nVersion;
  DWORD dwFlags;
  BYTE  iPixelType;
  BYTE  cColorBits;
  BYTE  cRedBits;
  BYTE  cRedShift;
  BYTE  cGreenBits;
  BYTE  cGreenShift;
  BYTE  cBlueBits;
  BYTE  cBlueShift;
  BYTE  cAlphaBits;
  BYTE  cAlphaShift;
  BYTE  cAccumBits;
  BYTE  cAccumRedBits;
  BYTE  cAccumGreenBits;
  BYTE  cAccumBlueBits;
  BYTE  cAccumAlphaBits;
  BYTE  cDepthBits;
  BYTE  cStencilBits;
  BYTE  cAuxBuffers;
  BYTE  iLayerType;
  BYTE  bReserved;
  DWORD dwLayerMask;
  DWORD dwVisibleMask;
  DWORD dwDamageMask;
} PIXELFORMATDESCRIPTOR;

#define WGL_NUMBER_PIXEL_FORMATS_ARB            0x2000
#define WGL_DRAW_TO_WINDOW_ARB                  0x2001
#define WGL_ACCELERATION_ARB                    0x2003
#define WGL_SWAP_METHOD_ARB                     0x2007
#define WGL_SUPPORT_OPENGL_ARB                  0x2010
#define WGL_DOUBLE_BUFFER_ARB                   0x2011
#define WGL_PIXEL_TYPE_ARB                      0x2013
#define WGL_COLOR_BITS_ARB                      0x2014
#define WGL_RED_BITS_ARB                        0x2015
#define WGL_RED_SHIFT_ARB                       0x2016
#define WGL_GREEN_BITS_ARB                      0x2017
#define WGL_GREEN_SHIFT_ARB                     0x2018
#define WGL_BLUE_BITS_ARB                       0x2019
#define WGL_BLUE_SHIFT_ARB                      0x201A
#define WGL_ALPHA_BITS_ARB                      0x201B
#define WGL_ALPHA_SHIFT_ARB                     0x201C
#define WGL_DEPTH_BITS_ARB                      0x2022
#define WGL_STENCIL_BITS_ARB                    0x2023
#define WGL_AUX_BUFFERS_ARB                     0x2024
#define WGL_FULL_ACCELERATION_ARB               0x2027
#define WGL_SWAP_EXCHANGE_ARB                   0x2028
#define WGL_TYPE_RGBA_ARB                       0x202B
#define WGL_DRAW_TO_PBUFFER_ARB                 0x202D
#define WGL_PBUFFER_WIDTH_ARB                   0x2034
#define WGL_PBUFFER_HEIGHT_ARB                  0x2035
#define WGL_SAMPLE_BUFFERS_ARB                  0x2041
#define WGL_SAMPLES_ARB                         0x2042
/* WGL_ARB_create_context(_profile): same values as GLX_*_ARB */
#define WGL_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB           0x2092
#define WGL_CONTEXT_FLAGS_ARB                   0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB            0x9126
#define WGL_CONTEXT_DEBUG_BIT_ARB               0x0001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB  0x0002
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB        0x0001
/* WGL_ARB_render_texture / WGL_NV_render_texture_rectangle */
#define WGL_TEXTURE_FORMAT_ARB                  0x2072
#define WGL_TEXTURE_TARGET_ARB                  0x2073
#define WGL_TEXTURE_RGB_ARB                     0x2075
#define WGL_TEXTURE_RGBA_ARB                    0x2076
#define WGL_TEXTURE_2D_ARB                      0x207A
#define WGL_MIPMAP_LEVEL_ARB                    0x207B
#define WGL_TEXTURE_RECTANGLE_NV                0x20A2

typedef struct tagFakePBuffer {
    int width;
    int height;
    int target, format, level;
} HPBUFFERARB;

static const PIXELFORMATDESCRIPTOR pfd = {
    .nSize = sizeof(PIXELFORMATDESCRIPTOR),
    .nVersion = 1,
    .dwFlags = 0x225,
    .cColorBits = 32,
    .cRedBits = 8, .cGreenBits = 8, .cBlueBits = 8, .cAlphaBits = 8,
    .cRedShift = 16, .cGreenShift = 8, .cBlueShift = 0, .cAlphaShift = 24,
    .cDepthBits = 24,
    .cStencilBits = 8,
    .cAuxBuffers = 0,
};
static const int iAttribs[] = {
    WGL_NUMBER_PIXEL_FORMATS_ARB, 1,
    WGL_DRAW_TO_WINDOW_ARB, 1,
    WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
    WGL_SWAP_METHOD_ARB, WGL_SWAP_EXCHANGE_ARB,
    WGL_SUPPORT_OPENGL_ARB, 1,
    WGL_DOUBLE_BUFFER_ARB, 1,
    WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
    WGL_COLOR_BITS_ARB, 32,
    WGL_RED_BITS_ARB, 8,
    WGL_RED_SHIFT_ARB, 16,
    WGL_GREEN_BITS_ARB, 8,
    WGL_GREEN_SHIFT_ARB, 8,
    WGL_BLUE_BITS_ARB, 8,
    WGL_BLUE_SHIFT_ARB, 0,
    WGL_ALPHA_BITS_ARB, 8,
    WGL_ALPHA_SHIFT_ARB, 24,
    WGL_DEPTH_BITS_ARB, 24,
    WGL_STENCIL_BITS_ARB, 8,
    WGL_AUX_BUFFERS_ARB, 0,
    WGL_SAMPLE_BUFFERS_ARB, 0,
    WGL_SAMPLES_ARB, 0,
    0,0
};

/* platform-independent state */
static void        *ctx[MAX_LVLCNTX];      /* context levels (0 = main) */
static int          win_w, win_h;           /* drawable = guest 2D surface */
static int          win_ready;              /* provider said go */
static int          wnd_ready;              /* handshake flag for the guest */
static const char  *xstr, *xcstr;
static HPBUFFERARB  hPbuffer[MAX_PBUFFER];
static int          cAlphaBits, cDepthBits, cStencilBits;
static int          cAuxBuffers, cSampleBuf[2];
static int          swap_interval = 1;
static uint32_t    *readback;               /* win_w * win_h, bottom-up */
static struct wgamma { uint16_t r[0x100], g[0x100], b[0x100]; } gamma_ramp;

/* platform layer (one implementation per OS below) */
static int   plat_open(void);                          /* library / display */
static int   plat_choose(int msaa);                    /* fills c*Bits */
static void *plat_ctx_create(void *share, const int *wgl_attribs);
static void  plat_ctx_destroy(void *c);
static void  plat_unbind(void);
static int   plat_make_current(void *c);               /* + main drawable */
static void *plat_get_current(void);
static int   plat_drawable(int w, int h);              /* (re)size FBO 0 */
static void  plat_drawable_release(void);
static void  plat_readback(uint32_t *dst);             /* FBO 0, bottom-up */
static int   plat_pbuffer_create(int i, int w, int h);
static void  plat_pbuffer_destroy(int i);
static void  plat_pbuffer_current(int i);
static void  plat_pbuffer_bind_tex(int i, int gl_target, int gl_format);
static void *plat_get_proc(const char *name);
static void  plat_set_func_ptr(void *hdll);

/* guest 2D surface size = drawable size (the SDL window follows it too) */
static void guest_size(int *w, int *h)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *s = con ? qemu_console_surface(con) : NULL;
    *w = s ? surface_width(s) : 640;
    *h = s ? surface_height(s) : 480;
}

int embed_gl_available(void)
{
    return plat_open();
}

void embed_gl_drawable_size(int *w, int *h)
{
    *w = win_w ? win_w : 640;
    *h = win_h ? win_h : 480;
}

static int drawable_resize(int w, int h)
{
    if (win_ready && win_w == w && win_h == h) {
        return 1;
    }
    if (!plat_drawable(w, h)) {
        return 0;
    }
    win_w = w;
    win_h = h;
    readback = g_realloc(readback, (size_t)w * h * sizeof(uint32_t));
    DPRINTF("drawable %dx%d", w, h);
    return 1;
}

/* ------------------------------------------------------------ backend API */

int glwnd_ready(void) { return qatomic_read(&wnd_ready); }

int MGLExtIsAvail(const char *xstr_, const char *str)
{ return find_xstr(xstr_, str); }

static void cwnd_mesagl(void *swnd, void *nwnd, void *opaque)
{
    win_ready = nwnd ? 1 : 0;
    DPRINTF("MESAGL drawable %s", win_ready ? "ready" : "refused");
    qatomic_set(&wnd_ready, 1);
}

static void TmpContextPurge(void)
{
    int n;
    for (n = MAX_LVLCNTX; ((n > 1) && !ctx[--n]););
    if ((n == 1) && ctx[--n]) {
        plat_ctx_destroy(ctx[n]);
        DPRINTF("MESAGL curr %d cntx [%p] purge %d", n, ctx[n], 1);
        ctx[n] = 0;
    }
}

void SetMesaFuncPtr(void *p)
{
    plat_set_func_ptr(p);
}

void *MesaGLGetProc(const char *proc)
{
    return plat_get_proc(proc);
}

void MGLTmpContext(void)
{
    plat_open();
}

void MGLDeleteContext(int level)
{
    int n = (level)? ((level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1):level;
    plat_unbind();
    if (n) {
        plat_ctx_destroy(ctx[n]);
        ctx[n] = 0;
    }
    else {
        for (int i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                plat_ctx_destroy(ctx[i]);
                ctx[i] = 0;
            }
        }
        MesaBlitFree();
        MGLActivateHandler(0, 0);
    }
}

void MGLWndRelease(void)
{
    if (win_ready) {
        plat_unbind();
        if (ctx[0]) {
            plat_ctx_destroy(ctx[0]);
        }
        plat_drawable_release();
        mesa_release_window();
        CompareAttribArray(NULL);
        ctx[0] = 0;
        win_w = win_h = 0;
        win_ready = 0;
    }
}

int MGLCreateContext(uint32_t gDC)
{
    int i, ret;
    if (!win_ready) {
        DPRINTF("no drawable: GL context refused");
        return 1;
    }
    i = gDC & (MAX_PBUFFER - 1);
    if (gDC == ((MESAGL_HPBDC & 0xFFFFFFF0U) | i)) {
        ret = 0;
    }
    else {
        plat_unbind();
        for (i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                plat_ctx_destroy(ctx[i]);
                ctx[i] = 0;
            }
        }
        if (!ctx[0])
            ctx[0] = plat_ctx_create(NULL, NULL);
        ret = (ctx[0])? 0:1;
    }
    return ret;
}

int MGLMakeCurrent(uint32_t cntxRC, int level)
{
    if (!win_ready)
        return 0;
    int n = (level)? ((level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1):level;
    uint32_t i = cntxRC & (MAX_PBUFFER - 1);
    if (cntxRC == (MESAGL_MAGIC - n)) {
        int w, h;
        guest_size(&w, &h);
        drawable_resize(w, h);
        plat_make_current(ctx[n]);
        InitMesaGLExt();
        wrContextSRGB(ContextUseSRGB());
        if (!n)
            MGLActivateHandler(1, 0);
    }
    if (cntxRC == (((MESAGL_MAGIC & 0xFFFFFFFU) << 4) | i))
        plat_pbuffer_current(i);

    return 0;
}

int MGLSwapBuffers(void)
{
    MGLActivateHandler(1, 0);
    MesaBlitScale();
    /* present = read FBO 0 back and hand it to the frontend (bottom-up) */
    if (win_ready && readback && plat_get_current() == ctx[0]) {
        plat_readback(readback);
        embed_fx_frame(readback, win_w, win_h, win_w * 4, /* bottom_up */ 1);
    }
    /* size follows the guest's 2D mode for the next frame */
    int w, h;
    guest_size(&w, &h);
    if (w != win_w || h != win_h) {
        drawable_resize(w, h);
    }
    return 1;
}

static int MGLPresetPixelFormat(void)
{
    qatomic_set(&wnd_ready, 0);
    ImplMesaGLReset();
    if (!plat_open() || !plat_choose(GetContextMSAA())) {
        if (GetContextMSAA() && plat_choose(0)) {
            DPRINTF("MSAA %d unavailable, using no MSAA", GetContextMSAA());
        } else {
            DPRINTF("no usable GL configuration");
            cwnd_mesagl(NULL, NULL, NULL);
            return 1;
        }
    }
    /* the provider decides whether a drawable may exist (it calls cwnd) */
    mesa_prepare_window(GetContextMSAA(), GL_CONTEXTALPHA, 0, &cwnd_mesagl);
    if (!win_ready) {
        return 1;
    }
    int w, h;
    guest_size(&w, &h);
    win_w = win_h = 0;
    drawable_resize(w, h);
    for (int i = 0; i < 0x100; i++) {
        gamma_ramp.r[i] = gamma_ramp.g[i] = gamma_ramp.b[i] = (i << 8) | i;
    }
    DPRINTF("config depth %d stencil %d nSamples %d %d %s",
        cDepthBits, cStencilBits, cSampleBuf[0], cSampleBuf[1],
        ContextUseSRGB()? "sRGB":"");
    return 1;
}

int MGLChoosePixelFormat(void)
{
    DPRINTF("ChoosePixelFormat()");
    if (!win_ready)
        return MGLPresetPixelFormat();
    return 1;
}

int MGLSetPixelFormat(int fmt, const void *p)
{
    int ret;
    ret = (!win_ready)? MGLPresetPixelFormat():1;
    TmpContextPurge();
    DPRINTF("SetPixelFormat() ret %d", ret);
    return ret;
}

int MGLDescribePixelFormat(int fmt, unsigned int sz, void *p)
{
    if (!win_ready)
        MGLPresetPixelFormat();
    memcpy(p, &pfd, sizeof(PIXELFORMATDESCRIPTOR));
    ((PIXELFORMATDESCRIPTOR *)p)->cDepthBits = cDepthBits;
    ((PIXELFORMATDESCRIPTOR *)p)->cStencilBits = cStencilBits;
    ((PIXELFORMATDESCRIPTOR *)p)->cAuxBuffers = cAuxBuffers;
    return 1;
}

int NumPbuffer(void)
{
    int i, c;
    for (i = 0, c = 0; i < MAX_PBUFFER;)
        if (hPbuffer[i++].width) c++;
    return c;
}

int DrawableContext(void)
{
    return (ctx[0] == plat_get_current());
}

static int PbufferGLBinding(const int target)
{
    switch (target) {
        case WGL_TEXTURE_2D_ARB:        return 0x8069; /* GL_TEXTURE_BINDING_2D */
        case WGL_TEXTURE_RECTANGLE_NV:  return 0x84F6; /* GL_TEXTURE_BINDING_RECTANGLE */
        default:                        return 0;
    }
}
static int PbufferGLAttrib(const int attr)
{
    switch (attr) {
        case WGL_TEXTURE_2D_ARB:        return 0x0DE1; /* GL_TEXTURE_2D */
        case WGL_TEXTURE_RECTANGLE_NV:  return 0x84F5; /* GL_TEXTURE_RECTANGLE */
        case WGL_TEXTURE_RGB_ARB:       return 0x1907; /* GL_RGB */
        case WGL_TEXTURE_RGBA_ARB:      return 0x1908; /* GL_RGBA */
        default:                        return 0;
    }
}
static int LookupAttribArray(const int *attrib, const int attr)
{
    int ret = 0;
    for (int i = 0; attrib[i]; i+=2) {
        if (attrib[i] == attr) {
            switch (attr) {
                case WGL_DEPTH_BITS_ARB:    ret = cDepthBits;    break;
                case WGL_STENCIL_BITS_ARB:  ret = cStencilBits;  break;
                case WGL_AUX_BUFFERS_ARB:   ret = cAuxBuffers;   break;
                case WGL_SAMPLE_BUFFERS_ARB: ret = cSampleBuf[0]; break;
                case WGL_SAMPLES_ARB:       ret = cSampleBuf[1]; break;
                default:                    ret = attrib[i+1];   break;
            }
            break;
        }
    }
    return ret;
}

void MGLFuncHandler(const char *name)
{
    char fname[64];
    uint32_t *argsp = (uint32_t *)(name + ALIGNED((strnlen(name, sizeof(fname))+1)));
    strncpy(fname, name, sizeof(fname)-1);

#define FUNCP_HANDLER(a) \
    if (!memcmp(fname, a, sizeof(a)))

    FUNCP_HANDLER("wglShareLists") {
        uint32_t i, ret = 0;
        i = argsp[1] & (MAX_PBUFFER - 1);
        if ((argsp[0] == MESAGL_MAGIC) && (argsp[1] == ((MESAGL_MAGIC & 0xFFFFFFFU) << 4 | i)))
            ret = 1;
        else {
            DPRINTF("  *WARN* ShareLists called with unknown contexts, %x %x", argsp[0], argsp[1]);
        }
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglUseFontBitmapsA") {
        /* no X font path without a display; the guest falls back */
        argsp[0] = 0;
        return;
    }
    FUNCP_HANDLER("wglSwapIntervalEXT") {
        /* the frontend paces presentation; remember the guest's wish */
        swap_interval = argsp[0];
        DPRINTF("wglSwapIntervalEXT(%u) ret %-24u", argsp[0], 1);
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetSwapIntervalEXT") {
        argsp[0] = swap_interval;
        return;
    }
    FUNCP_HANDLER("wglGetExtensionsStringARB") {
        const char wglext[] = "WGL_3DFX_gamma_control "
            "WGL_ARB_create_context "
            "WGL_ARB_create_context_profile "
            "WGL_ARB_extensions_string "
            "WGL_ARB_multisample "
            "WGL_ARB_pixel_format "
            "WGL_ARB_pbuffer WGL_ARB_render_texture WGL_NV_render_texture_rectangle "
            "WGL_EXT_extensions_string "
            "WGL_EXT_swap_control "
            ;
        memcpy((char *)name, wglext, sizeof(wglext));
        *((char *)name + sizeof(wglext) - 2) = '\0';
        return;
    }
    FUNCP_HANDLER("wglCreateContextAttribsARB") {
        uint32_t i, ret;
        const int *attribs = (const int *)&argsp[2];
        for (i = 0; ((i < MAX_LVLCNTX) && ctx[i]); i++);
        argsp[1] = (argsp[0])? i:0;
        if (argsp[1] == 0) {
            plat_unbind();
            if (CompareAttribArray(attribs)) {
                for (i = MAX_LVLCNTX; i > 0;) {
                    if (ctx[--i]) {
                        plat_ctx_destroy(ctx[i]);
                        ctx[i] = 0;
                    }
                }
                MGLActivateHandler(0, 0);
                ctx[0] = plat_ctx_create(NULL, attribs);
            }
            ret = (ctx[0])? 1:0;
        }
        else {
            if (i == MAX_LVLCNTX) {
                plat_ctx_destroy(ctx[1]);
                for (i = 1; i < (MAX_LVLCNTX - 1); i++)
                    ctx[i] = ctx[i + 1];
                argsp[1] = i;
            }
            ctx[i] = plat_ctx_create(ctx[i-1], attribs);
            ret = (ctx[i])? 1:0;
        }
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribfvARB") {
        const int *ia = (const int *)&argsp[4], n = argsp[2];
        float pf[64];
        for (int i = 0; i < n; i++)
            pf[i] = (float)LookupAttribArray(iAttribs, ia[i]);
        memcpy(&argsp[2], pf, n*sizeof(float));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribivARB") {
        const int *ia = (const int *)&argsp[4], n = argsp[2];
        int pi[64];
        for (int i = 0; i < n; i++)
            pi[i] = LookupAttribArray(iAttribs, ia[i]);
        memcpy(&argsp[2], pi, n*sizeof(int));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglChoosePixelFormatARB") {
        const int *ia = (const int *)argsp;
        if (LookupAttribArray(ia, WGL_DRAW_TO_PBUFFER_ARB)) {
            argsp[1] = 0x02;
        }
        else {
            DPRINTF("%-32s", "wglChoosePixelFormatARB()");
            argsp[1] = MGLChoosePixelFormat();
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglBindTexImageARB") {
        uint32_t i = argsp[0] & (MAX_PBUFFER - 1);
        if (PbufferGLBinding(hPbuffer[i].target) && PbufferGLAttrib(hPbuffer[i].format)) {
            plat_pbuffer_bind_tex(i, hPbuffer[i].target, hPbuffer[i].format);
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglReleaseTexImageARB") {
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglCreatePbufferARB") {
        uint32_t i;
        for (i = 0; ((i < MAX_PBUFFER) && hPbuffer[i].width); i++);
        if (MAX_PBUFFER == i) {
            DPRINTF("MAX_PBUFFER reached %-24u", i);
            argsp[0] = 0;
            return;
        }
        hPbuffer[i].width = argsp[1];
        hPbuffer[i].height = argsp[2];
        const int *pattr = (const int *)&argsp[4];
        hPbuffer[i].target = LookupAttribArray(pattr, WGL_TEXTURE_TARGET_ARB);
        hPbuffer[i].format = LookupAttribArray(pattr, WGL_TEXTURE_FORMAT_ARB);
        hPbuffer[i].level = LookupAttribArray(pattr, WGL_MIPMAP_LEVEL_ARB);
        if (plat_pbuffer_create(i, hPbuffer[i].width, hPbuffer[i].height)) {
            argsp[0] = 1;
        } else {
            memset(&hPbuffer[i], 0, sizeof(HPBUFFERARB));
            argsp[0] = 0;
        }
        argsp[1] = i;
        return;
    }
    FUNCP_HANDLER("wglDestroyPbufferARB") {
        uint32_t i;
        i = argsp[0] & (MAX_PBUFFER - 1);
        plat_pbuffer_destroy(i);
        argsp[0] = 1;
        memset(&hPbuffer[i], 0, sizeof(HPBUFFERARB));
        return;
    }
    FUNCP_HANDLER("wglQueryPbufferARB") {
        uint32_t i = argsp[0] & (MAX_PBUFFER - 1);
        switch(argsp[1]) {
            case WGL_PBUFFER_WIDTH_ARB:   argsp[2] = hPbuffer[i].width;  break;
            case WGL_PBUFFER_HEIGHT_ARB:  argsp[2] = hPbuffer[i].height; break;
            case WGL_TEXTURE_TARGET_ARB:  argsp[2] = hPbuffer[i].target; break;
            case WGL_TEXTURE_FORMAT_ARB:  argsp[2] = hPbuffer[i].format; break;
            case WGL_MIPMAP_LEVEL_ARB:    argsp[2] = hPbuffer[i].level;  break;
            default:
                argsp[0] = 0;
                return;
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetDeviceGammaRamp3DFX") {
        /* no display to apply it to: hand back what the guest last set */
        struct wgamma *wRamp = (struct wgamma *)&argsp[2];
        memcpy(wRamp, &gamma_ramp, sizeof(gamma_ramp));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglSetDeviceGammaRamp3DFX") {
        struct wgamma *wRamp = (struct wgamma *)&argsp[0];
        memcpy(&gamma_ramp, wRamp, sizeof(gamma_ramp));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglSetDeviceCursor3DFX") {
        return;
    }

    DPRINTF("  *WARN* Unhandled GLFunc %s", name);
    argsp[0] = 0;
}

#endif /* CONFIG_LINUX || CONFIG_DARWIN */

/* ============================================================== Linux */
#if defined(CONFIG_LINUX)
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <linux/version.h>
#include <sys/utsname.h>
#include "sysemu/kvm.h"

static int bufo_accel_en(void)
{
    struct utsname buf;
    if (!uname(&buf)) {
        int major, patch, sub, i = sscanf(buf.release, "%d.%d.%d", &major, &patch, &sub);
        if (i == 3) {
            return (KERNEL_VERSION(major, patch, sub) >=
                    KERNEL_VERSION(6, 13, 0))? 1:0;
        }
    }
    return 0;
}
int MGLUpdateGuestBufo(mapbufo_t *bufo, const int add)
{
    int ret = (GetBufOAccelEN()
            || (bufo_accel_en() &&
                (bufo && bufo->tgt == GL_PIXEL_UNPACK_BUFFER))
            )? kvm_enabled():0;
    if (ret && bufo) {
        bufo->lvl = (add)? MapBufObjGpa(bufo):0;
        kvm_update_guest_pa_range(MBUFO_BASE | (bufo->gpa & ((MBUFO_SIZE - 1) - (qemu_real_host_page_size() - 1))),
            bufo->mapsz + (bufo->hva & (qemu_real_host_page_size() - 1)),
            (void *)(bufo->hva & qemu_real_host_page_mask()),
            (bufo->acc & GL_MAP_WRITE_BIT)? 0:1, add);
    }
    return ret;
}

static EGLDisplay   dpy = EGL_NO_DISPLAY;
static EGLConfig    cfg;
static EGLSurface   win = EGL_NO_SURFACE;     /* the pbuffer = FBO 0 */
static EGLSurface   PBDC[MAX_PBUFFER];
static EGLContext   PBRC[MAX_PBUFFER];

static const EGLint ctx_compat[] = {
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
    EGL_NONE
};

static int plat_open(void)
{
    if (dpy != EGL_NO_DISPLAY) {
        return 1;
    }
    if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        DPRINTF("EGL_MESA_platform_surfaceless missing");
        return 0;
    }
    dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                   EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        DPRINTF("no surfaceless EGL display");
        return 0;
    }
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        DPRINTF("eglInitialize failed 0x%x", eglGetError());
        dpy = EGL_NO_DISPLAY;
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        DPRINTF("EGL_OPENGL_API not supported");
        eglTerminate(dpy);
        dpy = EGL_NO_DISPLAY;
        return 0;
    }
    xcstr = eglQueryString(dpy, EGL_VENDOR);
    xstr = eglQueryString(dpy, EGL_EXTENSIONS);
    DPRINTF("EGL %d.%d %s (surfaceless)", major, minor, xcstr);
    return 1;
}

static int plat_choose(int msaa)
{
    EGLint ia[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE,   24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, msaa ? 1 : 0,
        EGL_SAMPLES,        msaa ? msaa : 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ia, &cfg, 1, &n) || n < 1) {
        return 0;
    }
    eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &cAlphaBits);
    eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &cDepthBits);
    eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &cStencilBits);
    cAuxBuffers = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLE_BUFFERS, &cSampleBuf[0]);
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLES, &cSampleBuf[1]);
    return 1;
}

/* WGL_ARB_create_context attribs (== GLX values) -> EGL attribs */
static void *plat_ctx_create(void *share, const int *wgl)
{
    EGLint ea[16];
    int n = 0;
    if (wgl) {
        for (int i = 0; wgl[i] && n + 2 < 16; i += 2) {
            switch (wgl[i]) {
            case WGL_CONTEXT_MAJOR_VERSION_ARB:
                ea[n++] = EGL_CONTEXT_MAJOR_VERSION; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_MINOR_VERSION_ARB:
                ea[n++] = EGL_CONTEXT_MINOR_VERSION; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_PROFILE_MASK_ARB:
                /* core = 1, compatibility = 2 in both APIs */
                ea[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_FLAGS_ARB:
                if (wgl[i + 1] & WGL_CONTEXT_DEBUG_BIT_ARB) {
                    ea[n++] = EGL_CONTEXT_OPENGL_DEBUG; ea[n++] = EGL_TRUE;
                }
                if (wgl[i + 1] & WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) {
                    ea[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE; ea[n++] = EGL_TRUE;
                }
                break;
            default:
                break;
            }
        }
        ea[n] = EGL_NONE;
    }
    EGLContext c = eglCreateContext(dpy, cfg, share ? (EGLContext)share : EGL_NO_CONTEXT,
                                    wgl ? ea : ctx_compat);
    if (c == EGL_NO_CONTEXT) {
        DPRINTF("eglCreateContext failed 0x%x", eglGetError());
        return NULL;
    }
    return c;
}

static void plat_ctx_destroy(void *c)
{
    eglDestroyContext(dpy, (EGLContext)c);
}

static void plat_unbind(void)
{
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static int plat_make_current(void *c)
{
    if (!eglMakeCurrent(dpy, win, win, (EGLContext)c)) {
        DPRINTF("eglMakeCurrent failed 0x%x", eglGetError());
        return 0;
    }
    return 1;
}

static void *plat_get_current(void)
{
    return eglGetCurrentContext();
}

/* (re)create the pbuffer that plays the window; keeps the context */
static int plat_drawable(int w, int h)
{
    EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pa);
    if (s == EGL_NO_SURFACE) {
        DPRINTF("pbuffer %dx%d failed 0x%x", w, h, eglGetError());
        return 0;
    }
    EGLContext cur = eglGetCurrentContext();
    if (win != EGL_NO_SURFACE) {
        if (cur != EGL_NO_CONTEXT && eglGetCurrentSurface(EGL_DRAW) == win) {
            eglMakeCurrent(dpy, s, s, cur);
        }
        eglDestroySurface(dpy, win);
    }
    win = s;
    return 1;
}

static void plat_drawable_release(void)
{
    if (win != EGL_NO_SURFACE) {
        eglDestroySurface(dpy, win);
        win = EGL_NO_SURFACE;
    }
}

static void plat_readback(uint32_t *dst)
{
    GLint prev_fbo = 0, prev_pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_fbo);
}

static int plat_pbuffer_create(int i, int w, int h)
{
    const EGLint ia[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, cAlphaBits,
        EGL_DEPTH_SIZE, cDepthBits,
        EGL_NONE,
    };
    EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLConfig pbcfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ia, &pbcfg, 1, &n) || n < 1) {
        return 0;
    }
    PBDC[i] = eglCreatePbufferSurface(dpy, pbcfg, pa);
    PBRC[i] = eglCreateContext(dpy, pbcfg, eglGetCurrentContext(), ctx_compat);
    return PBDC[i] != EGL_NO_SURFACE && PBRC[i] != EGL_NO_CONTEXT;
}

static void plat_pbuffer_destroy(int i)
{
    if (PBRC[i])
        eglDestroyContext(dpy, PBRC[i]);
    if (PBDC[i])
        eglDestroySurface(dpy, PBDC[i]);
    PBRC[i] = 0; PBDC[i] = 0;
}

static void plat_pbuffer_current(int i)
{
    eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);
}

static void plat_pbuffer_bind_tex(int i, int wgl_target, int wgl_format)
{
    int prev_binded_texture = 0;
    EGLContext prev_context = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    glGetIntegerv(PbufferGLBinding(wgl_target), &prev_binded_texture);
    eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);
    glBindTexture(PbufferGLAttrib(wgl_target), prev_binded_texture);
    glCopyTexImage2D(PbufferGLAttrib(wgl_target), hPbuffer[i].level,
        PbufferGLAttrib(wgl_format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
    eglMakeCurrent(dpy, prev_draw, prev_read, prev_context);
}

static void *plat_get_proc(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

static void plat_set_func_ptr(void *hdll)
{
}

/* ============================================================== macOS */
#elif defined(CONFIG_DARWIN)
#define GL_SILENCE_DEPRECATION 1
#include <dlfcn.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

/* mesagl_impl.c dlopens this for the dispatch table; the framework umbrella
 * re-exports libGL, so CGL and GL come from one handle */
const char dllname[] = "/System/Library/Frameworks/OpenGL.framework/OpenGL";

int MGLUpdateGuestBufo(mapbufo_t *bufo, int add) { return 0; }

static CGLPixelFormatObj pix;
static int      cgl_open;
/*
 * The default framebuffer stand-in: an FBO per context level (FBO names
 * are not shared between contexts) over shared renderbuffers. Binding 0
 * through the guest's dispatch is redirected here (MesaGLSetFunc).
 */
static GLuint   dfbo[MAX_LVLCNTX];
static void    *dfbo_ctx[MAX_LVLCNTX];     /* the context each FBO belongs to */
static GLuint   rb_color, rb_depth;
static GLuint   cur_dfbo;                   /* FBO of the current context */
static int      rb_w, rb_h;
/* WGL pbuffer emulation: FBO + texture in the main context */
static GLuint   pb_fbo[MAX_PBUFFER], pb_tex[MAX_PBUFFER], pb_depth[MAX_PBUFFER];
static void (*real_bind_fb)(GLenum, GLuint);
static void (*real_bind_fb_ext)(GLenum, GLuint);

static void fx_glBindFramebuffer(GLenum target, GLuint fb)
{
    real_bind_fb(target, fb ? fb : cur_dfbo);
}
static void fx_glBindFramebufferEXT(GLenum target, GLuint fb)
{
    real_bind_fb_ext(target, fb ? fb : cur_dfbo);
}

static int plat_open(void)
{
    if (cgl_open) {
        return 1;
    }
    /* any accelerated renderer will do; probing needs no context */
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj p = NULL;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &p, &npix);
    if (err != kCGLNoError || !p) {
        DPRINTF("CGLChoosePixelFormat: %s", CGLErrorString(err));
        return 0;
    }
    CGLDestroyPixelFormat(p);
    cgl_open = 1;
    xcstr = "Apple";
    xstr = "";
    DPRINTF("CGL (window-less)");
    return 1;
}

static CGLPixelFormatObj choose_pix(int msaa, int profile)
{
    CGLPixelFormatAttribute attrs[24];
    int n = 0;
#define A(x) ((CGLPixelFormatAttribute)(x))
    attrs[n++] = kCGLPFAAccelerated;
    attrs[n++] = kCGLPFAColorSize;   attrs[n++] = A(24);
    attrs[n++] = kCGLPFAAlphaSize;   attrs[n++] = A(8);
    attrs[n++] = kCGLPFADepthSize;   attrs[n++] = A(24);
    attrs[n++] = kCGLPFAStencilSize; attrs[n++] = A(8);
    attrs[n++] = kCGLPFAOpenGLProfile; attrs[n++] = A(profile);
    if (msaa) {
        attrs[n++] = kCGLPFASampleBuffers; attrs[n++] = A(1);
        attrs[n++] = kCGLPFASamples;       attrs[n++] = A(msaa);
        attrs[n++] = kCGLPFAMultisample;
    }
    attrs[n] = A(0);
#undef A
    CGLPixelFormatObj p = NULL;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &p, &npix);
    if (err != kCGLNoError || !p) {
        DPRINTF("CGLChoosePixelFormat(profile 0x%x msaa %d): %s", profile, msaa,
                CGLErrorString(err));
        return NULL;
    }
    return p;
}

static int plat_choose(int msaa)
{
    /* legacy = compatibility 2.1: the guests are fixed-function GL */
    CGLPixelFormatObj p = choose_pix(msaa, kCGLOGLPVersion_Legacy);
    if (!p) {
        return 0;
    }
    if (pix) {
        CGLDestroyPixelFormat(pix);
    }
    pix = p;
    GLint v = 0;
    CGLDescribePixelFormat(pix, 0, kCGLPFAAlphaSize, &v);   cAlphaBits = v;
    CGLDescribePixelFormat(pix, 0, kCGLPFADepthSize, &v);   cDepthBits = v;
    CGLDescribePixelFormat(pix, 0, kCGLPFAStencilSize, &v); cStencilBits = v;
    cAuxBuffers = 0;
    /* the FBO stand-in is single-sampled for now */
    cSampleBuf[0] = 0;
    cSampleBuf[1] = 0;
    return 1;
}

static void *plat_ctx_create(void *share, const int *wgl)
{
    int profile = kCGLOGLPVersion_Legacy;
    if (wgl) {
        int major = 0, mask = 0;
        for (int i = 0; wgl[i]; i += 2) {
            if (wgl[i] == WGL_CONTEXT_MAJOR_VERSION_ARB) major = wgl[i + 1];
            if (wgl[i] == WGL_CONTEXT_PROFILE_MASK_ARB)  mask = wgl[i + 1];
        }
        if (major >= 4 && (mask & WGL_CONTEXT_CORE_PROFILE_BIT_ARB)) {
            profile = kCGLOGLPVersion_GL4_Core;
        } else if (major >= 3 && (mask & WGL_CONTEXT_CORE_PROFILE_BIT_ARB)) {
            profile = kCGLOGLPVersion_3_2_Core;
        }
    }
    CGLPixelFormatObj p = pix;
    int own = 0;
    if (profile != kCGLOGLPVersion_Legacy) {
        p = choose_pix(GetContextMSAA(), profile);
        own = p != NULL;
        if (!p) {
            p = pix;    /* fall back to legacy rather than fail */
        }
    }
    CGLContextObj c = NULL;
    CGLError err = CGLCreateContext(p, (CGLContextObj)share, &c);
    if (own) {
        CGLDestroyPixelFormat(p);
    }
    if (err != kCGLNoError) {
        DPRINTF("CGLCreateContext: %s", CGLErrorString(err));
        return NULL;
    }
    return c;
}

static void ctx_fbo_forget(void *c)
{
    for (int i = 0; i < MAX_LVLCNTX; i++) {
        if (dfbo_ctx[i] == c) {
            dfbo[i] = 0;
            dfbo_ctx[i] = NULL;
        }
    }
}

static void plat_ctx_destroy(void *c)
{
    ctx_fbo_forget(c);
    if (c == ctx[0]) {
        /* renderbuffers live in the share group rooted at ctx[0] */
        rb_color = rb_depth = 0;
        rb_w = rb_h = 0;
        for (int i = 0; i < MAX_PBUFFER; i++) {
            pb_fbo[i] = pb_tex[i] = pb_depth[i] = 0;
        }
    }
    if (CGLGetCurrentContext() == (CGLContextObj)c) {
        CGLSetCurrentContext(NULL);
        cur_dfbo = 0;
    }
    CGLDestroyContext((CGLContextObj)c);
}

static void plat_unbind(void)
{
    CGLSetCurrentContext(NULL);
    cur_dfbo = 0;
}

static void *plat_get_current(void)
{
    return CGLGetCurrentContext();
}

/* renderbuffers at the drawable size (shared objects, created once) */
static int rb_ensure(int w, int h)
{
    if (!rb_color) {
        glGenRenderbuffersEXT(1, &rb_color);
        glGenRenderbuffersEXT(1, &rb_depth);
        rb_w = rb_h = 0;
    }
    if (rb_w != w || rb_h != h) {
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb_color);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb_depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
        rb_w = w;
        rb_h = h;
    }
    return 1;
}

/* the current context's FBO 0 stand-in, created on first use */
static int dfbo_ensure(void)
{
    CGLContextObj c = CGLGetCurrentContext();
    int slot = -1;
    if (!c) {
        return 0;
    }
    rb_ensure(win_w ? win_w : 640, win_h ? win_h : 480);
    for (int i = 0; i < MAX_LVLCNTX; i++) {
        if (dfbo_ctx[i] == c && dfbo[i]) {
            cur_dfbo = dfbo[i];
            real_bind_fb_ext(GL_FRAMEBUFFER_EXT, cur_dfbo);
            return 1;
        }
        if (slot < 0 && !dfbo[i]) {
            slot = i;
        }
    }
    if (slot < 0) {
        return 0;
    }
    glGenFramebuffersEXT(1, &dfbo[slot]);
    dfbo_ctx[slot] = c;
    real_bind_fb_ext(GL_FRAMEBUFFER_EXT, dfbo[slot]);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, rb_color);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, rb_depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, rb_depth);
    GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
        DPRINTF("default FBO incomplete 0x%x", st);
    }
    cur_dfbo = dfbo[slot];
    glViewport(0, 0, rb_w, rb_h);
    return 1;
}

static int plat_make_current(void *c)
{
    CGLError err = CGLSetCurrentContext((CGLContextObj)c);
    if (err != kCGLNoError) {
        DPRINTF("CGLSetCurrentContext: %s", CGLErrorString(err));
        return 0;
    }
    return dfbo_ensure();
}

static int plat_drawable(int w, int h)
{
    /* storage follows lazily in dfbo_ensure()/rb_ensure() on the vCPU thread */
    if (CGLGetCurrentContext()) {
        rb_ensure(w, h);
    }
    return 1;
}

static void plat_drawable_release(void)
{
    /* objects die with the share group in plat_ctx_destroy(ctx[0]) */
}

static void plat_readback(uint32_t *dst)
{
    GLint prev_read = 0, prev_pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &prev_read);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    real_bind_fb_ext(GL_READ_FRAMEBUFFER_EXT, cur_dfbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
    real_bind_fb_ext(GL_READ_FRAMEBUFFER_EXT, prev_read ? prev_read : cur_dfbo);
}

/* WGL pbuffers: FBO + texture in the current (main) context */
static int plat_pbuffer_create(int i, int w, int h)
{
    if (!CGLGetCurrentContext()) {
        return 0;
    }
    GLint prev_fb = 0, prev_tex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fb);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(1, &pb_tex[i]);
    glBindTexture(GL_TEXTURE_2D, pb_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffersEXT(1, &pb_depth[i]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, pb_depth[i]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
    glGenFramebuffersEXT(1, &pb_fbo[i]);
    real_bind_fb_ext(GL_FRAMEBUFFER_EXT, pb_fbo[i]);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, pb_tex[i], 0);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, pb_depth[i]);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, pb_depth[i]);
    GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    real_bind_fb_ext(GL_FRAMEBUFFER_EXT, prev_fb ? prev_fb : cur_dfbo);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
        DPRINTF("pbuffer FBO incomplete 0x%x", st);
        plat_pbuffer_destroy(i);
        return 0;
    }
    return 1;
}

static void plat_pbuffer_destroy(int i)
{
    if (pb_fbo[i]) {
        glDeleteFramebuffersEXT(1, &pb_fbo[i]);
    }
    if (pb_depth[i]) {
        glDeleteRenderbuffersEXT(1, &pb_depth[i]);
    }
    if (pb_tex[i]) {
        glDeleteTextures(1, &pb_tex[i]);
    }
    pb_fbo[i] = pb_depth[i] = pb_tex[i] = 0;
}

static void plat_pbuffer_current(int i)
{
    /* same context: "making the pbuffer current" = drawing into its FBO */
    if (pb_fbo[i]) {
        real_bind_fb_ext(GL_FRAMEBUFFER_EXT, pb_fbo[i]);
        glViewport(0, 0, hPbuffer[i].width, hPbuffer[i].height);
    }
}

static void plat_pbuffer_bind_tex(int i, int wgl_target, int wgl_format)
{
    /* copy the pbuffer's colour into the texture the guest has bound */
    GLint prev_read = 0, prev_tex = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &prev_read);
    glGetIntegerv(PbufferGLBinding(wgl_target), &prev_tex);
    real_bind_fb_ext(GL_READ_FRAMEBUFFER_EXT, pb_fbo[i]);
    glBindTexture(PbufferGLAttrib(wgl_target), prev_tex);
    glCopyTexImage2D(PbufferGLAttrib(wgl_target), hPbuffer[i].level,
        PbufferGLAttrib(wgl_format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
    real_bind_fb_ext(GL_READ_FRAMEBUFFER_EXT, prev_read ? prev_read : cur_dfbo);
}

static void *hdll;
static void *plat_get_proc(const char *name)
{
    if (!hdll) {
        hdll = dlopen(dllname, RTLD_NOW);
    }
    return hdll ? dlsym(hdll, name) : NULL;
}

static void plat_set_func_ptr(void *h)
{
    hdll = h;
    /* redirect framebuffer 0 to the stand-in FBO in the guest's dispatch */
    real_bind_fb = MesaGLSetFunc(FEnum_glBindFramebuffer, (void *)fx_glBindFramebuffer);
    real_bind_fb_ext = MesaGLSetFunc(FEnum_glBindFramebufferEXT, (void *)fx_glBindFramebufferEXT);
    if (!real_bind_fb) {
        real_bind_fb = (void (*)(GLenum, GLuint))glBindFramebufferEXT;
    }
    if (!real_bind_fb_ext) {
        real_bind_fb_ext = (void (*)(GLenum, GLuint))glBindFramebufferEXT;
    }
}

#else /* neither: no embed backend, the native (weak) one stays */

int embed_gl_available(void)
{
    return 0;
}

void embed_gl_drawable_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

#endif
