/*
 * QEMU MESA GL Pass-Through — window-less context backend for the embed
 * library (win98-xp-virt, M3; docs/12-m3-context-provider.md).
 *
 * Port of hw/mesa/mglcntx_linux.c (GLX on an X11 window) to EGL on the
 * surfaceless platform with a pbuffer as the default framebuffer: FBO 0
 * stays "the screen" for MesaBlitScale/MesaRenderScaler, no window and no
 * display server are involved, and every GL call keeps running on the vCPU
 * thread under the BQL exactly as upstream. The presented frame is read
 * back after each swap and handed to the embedding frontend
 * (embedfx.c → libqemu_embed.c on_3d_frame) — the bring-up path; the
 * zero-copy dma-buf export replaces the readback later.
 *
 * Linux only for now (macOS gets a CGL/IOSurface port). The native backend
 * in hw/mesa is linked weak (patch 31) so these definitions take over in
 * libqemu-embed while qemu-system keeps GLX.
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

/* used by MGLPresetPixelFormat() below */
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
} PIXELFORMATDESCRIPTOR, *PPIXELFORMATDESCRIPTOR, *LPPIXELFORMATDESCRIPTOR;

#define WGL_NUMBER_PIXEL_FORMATS_ARB            0x2000
#define WGL_DRAW_TO_WINDOW_ARB                  0x2001
#define WGL_DRAW_TO_BITMAP_ARB                  0x2002
#define WGL_ACCELERATION_ARB                    0x2003
#define WGL_NEED_PALETTE_ARB                    0x2004
#define WGL_NEED_SYSTEM_PALETTE_ARB             0x2005
#define WGL_SWAP_LAYER_BUFFERS_ARB              0x2006
#define WGL_SWAP_METHOD_ARB                     0x2007
#define WGL_NUMBER_OVERLAYS_ARB                 0x2008
#define WGL_NUMBER_UNDERLAYS_ARB                0x2009
#define WGL_TRANSPARENT_ARB                     0x200A
#define WGL_SHARE_DEPTH_ARB                     0x200C
#define WGL_SHARE_STENCIL_ARB                   0x200D
#define WGL_SHARE_ACCUM_ARB                     0x200E
#define WGL_SUPPORT_GDI_ARB                     0x200F
#define WGL_SUPPORT_OPENGL_ARB                  0x2010
#define WGL_DOUBLE_BUFFER_ARB                   0x2011
#define WGL_STEREO_ARB                          0x2012
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
#define WGL_ACCUM_BITS_ARB                      0x201D
#define WGL_ACCUM_RED_BITS_ARB                  0x201E
#define WGL_ACCUM_GREEN_BITS_ARB                0x201F
#define WGL_ACCUM_BLUE_BITS_ARB                 0x2020
#define WGL_ACCUM_ALPHA_BITS_ARB                0x2021
#define WGL_DEPTH_BITS_ARB                      0x2022
#define WGL_STENCIL_BITS_ARB                    0x2023
#define WGL_AUX_BUFFERS_ARB                     0x2024
#define WGL_NO_ACCELERATION_ARB                 0x2025
#define WGL_GENERIC_ACCELERATION_ARB            0x2026
#define WGL_FULL_ACCELERATION_ARB               0x2027
#define WGL_SWAP_EXCHANGE_ARB                   0x2028
#define WGL_SWAP_COPY_ARB                       0x2029
#define WGL_SWAP_UNDEFINED_ARB                  0x202A
#define WGL_TYPE_RGBA_ARB                       0x202B
#define WGL_TYPE_COLORINDEX_ARB                 0x202C
#define WGL_SAMPLE_BUFFERS_ARB                  0x2041
#define WGL_SAMPLES_ARB                         0x2042
/* WGL_ARB_create_context(_profile): same values as GLX_*_ARB */
#define WGL_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB           0x2092
#define WGL_CONTEXT_FLAGS_ARB                   0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB            0x9126
#define WGL_CONTEXT_DEBUG_BIT_ARB               0x0001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB  0x0002
/* WGL_ARB_render_texture / WGL_NV_render_texture_rectangle */
#define WGL_TEXTURE_FORMAT_ARB                  0x2072
#define WGL_TEXTURE_RGB_ARB                     0x2075
#define WGL_TEXTURE_RGBA_ARB                    0x2076
#define WGL_TEXTURE_TARGET_ARB                  0x2073
#define WGL_TEXTURE_2D_ARB                      0x207A
#define WGL_TEXTURE_RECTANGLE_NV                0x20A2
#define WGL_MIPMAP_LEVEL_ARB                    0x207B

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

/* ------------------------------------------------------------- EGL state */

static EGLDisplay   dpy = EGL_NO_DISPLAY;
static EGLConfig    cfg;
static EGLSurface   win = EGL_NO_SURFACE;     /* the pbuffer = FBO 0 */
static int          win_w, win_h;
static int          win_ready;                /* provider said go */
static const char  *xstr, *xcstr;
static EGLContext   ctx[MAX_LVLCNTX];
static EGLSurface   PBDC[MAX_PBUFFER];
static EGLContext   PBRC[MAX_PBUFFER];

static HPBUFFERARB hPbuffer[MAX_PBUFFER];
static int wnd_ready;
static int cAlphaBits, cDepthBits, cStencilBits;
static int cAuxBuffers, cSampleBuf[2];
static int swap_interval = 1;
static uint32_t *readback;      /* win_w * win_h XRGB, top-down */
static struct wgamma { uint16_t r[0x100], g[0x100], b[0x100]; } gamma_ramp;

static const EGLint ctx_compat[] = {
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
    EGL_NONE
};

static int egl_open(void)
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

int embed_gl_available(void)
{
    return egl_open();
}

static int choose_config(const int do_msaa, EGLConfig *out)
{
    EGLint ia[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE,   24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, do_msaa ? 1 : 0,
        EGL_SAMPLES,        do_msaa ? do_msaa : 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ia, out, 1, &n) || n < 1) {
        return 0;
    }
    return 1;
}

/* (re)create the pbuffer that plays the window; keeps the context */
static int pbuffer_resize(int w, int h)
{
    if (win != EGL_NO_SURFACE && win_w == w && win_h == h) {
        return 1;
    }
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
    win_w = w;
    win_h = h;
    readback = g_realloc(readback, (size_t)w * h * sizeof(uint32_t));
    DPRINTF("drawable %dx%d", w, h);
    return 1;
}

/* guest 2D surface size = drawable size (the SDL window follows it too) */
static void guest_size(int *w, int *h)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *s = con ? qemu_console_surface(con) : NULL;
    *w = s ? surface_width(s) : 640;
    *h = s ? surface_height(s) : 480;
}

void embed_gl_drawable_size(int *w, int *h)
{
    *w = win_w ? win_w : 640;
    *h = win_h ? win_h : 480;
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
        eglDestroyContext(dpy, ctx[n]);
        DPRINTF("MESAGL curr %d cntx [%p] purge %d", n, ctx[n], 1);
        ctx[n] = 0;
    }
}

void SetMesaFuncPtr(void *p)
{
}

void *MesaGLGetProc(const char *proc)
{
    return (void *)eglGetProcAddress(proc);
}

void MGLTmpContext(void)
{
    egl_open();
}

static void unbind(void)
{
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void MGLDeleteContext(int level)
{
    int n = (level)? ((level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1):level;
    unbind();
    if (n) {
        eglDestroyContext(dpy, ctx[n]);
        ctx[n] = 0;
    }
    else {
        for (int i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                eglDestroyContext(dpy, ctx[i]);
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
        if (ctx[0]) {
            unbind();
            eglDestroyContext(dpy, ctx[0]);
        }
        if (win != EGL_NO_SURFACE) {
            eglDestroySurface(dpy, win);
        }
        mesa_release_window();
        CompareAttribArray(NULL);
        ctx[0] = 0;
        win = EGL_NO_SURFACE;
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
        unbind();
        for (i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                eglDestroyContext(dpy, ctx[i]);
                ctx[i] = 0;
            }
        }
        if (!ctx[0])
            ctx[0] = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_compat);
        ret = (ctx[0])? 0:1;
        if (ret) {
            DPRINTF("eglCreateContext failed 0x%x", eglGetError());
        }
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
        pbuffer_resize(w, h);
        if (!eglMakeCurrent(dpy, win, win, ctx[n])) {
            DPRINTF("eglMakeCurrent failed 0x%x", eglGetError());
        }
        InitMesaGLExt();
        wrContextSRGB(ContextUseSRGB());
        if (!n)
            MGLActivateHandler(1, 0);
    }
    if (cntxRC == (((MESAGL_MAGIC & 0xFFFFFFFU) << 4) | i))
        eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);

    return 0;
}

int MGLSwapBuffers(void)
{
    MGLActivateHandler(1, 0);
    MesaBlitScale();
    /*
     * Present = read FBO 0 back and hand it to the frontend. GL rows are
     * bottom-up; the frontend wants top-down XRGB8888.
     */
    if (win != EGL_NO_SURFACE && readback && eglGetCurrentContext() == ctx[0]) {
        GLint prev_fbo = 0, prev_pack = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, readback);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_fbo);
        embed_fx_frame(readback, win_w, win_h, win_w * 4, /* bottom_up */ 1);
    }
    /* size follows the guest's 2D mode for the next frame */
    int w, h;
    guest_size(&w, &h);
    if (w != win_w || h != win_h) {
        pbuffer_resize(w, h);
    }
    return 1;
}

static int MGLPresetPixelFormat(void)
{
    qatomic_set(&wnd_ready, 0);
    ImplMesaGLReset();
    if (!egl_open() || !choose_config(GetContextMSAA(), &cfg)) {
        if (dpy != EGL_NO_DISPLAY && choose_config(0, &cfg)) {
            DPRINTF("MSAA %d unavailable, using no MSAA", GetContextMSAA());
        } else {
            DPRINTF("no usable EGL config");
            cwnd_mesagl(NULL, NULL, NULL);
            return 1;
        }
    }
    /* the provider decides whether a drawable may exist (it calls cwnd) */
    mesa_prepare_window(GetContextMSAA(), GL_CONTEXTALPHA, 0, &cwnd_mesagl);
    if (!win_ready) {
        return 1;
    }
    eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &cAlphaBits);
    eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &cDepthBits);
    eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &cStencilBits);
    cAuxBuffers = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLE_BUFFERS, &cSampleBuf[0]);
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLES, &cSampleBuf[1]);
    int w, h;
    guest_size(&w, &h);
    pbuffer_resize(w, h);
    for (int i = 0; i < 0x100; i++) {
        gamma_ramp.r[i] = gamma_ramp.g[i] = gamma_ramp.b[i] = (i << 8) | i;
    }
    DPRINTF("EGLConfig depth %d stencil %d nSamples %d %d %s",
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
    return (ctx[0] == eglGetCurrentContext());
}

static int PbufferGLBinding(const int target)
{
    int ret;
    switch (target) {
        case WGL_TEXTURE_2D_ARB:
            ret = GL_TEXTURE_BINDING_2D;
            break;
        case WGL_TEXTURE_RECTANGLE_NV:
            ret = GL_TEXTURE_BINDING_RECTANGLE_NV;
            break;
        default:
            return 0;
    }
    return ret;
}
static int PbufferGLAttrib(const int attr)
{
    int ret;
    switch (attr) {
        case WGL_TEXTURE_2D_ARB:
            ret = GL_TEXTURE_2D;
            break;
        case WGL_TEXTURE_RECTANGLE_NV:
            ret = GL_TEXTURE_RECTANGLE_NV;
            break;
        case WGL_TEXTURE_RGB_ARB:
            ret = GL_RGB;
            break;
        case WGL_TEXTURE_RGBA_ARB:
            ret = GL_RGBA;
            break;
        default:
            return 0;
    }
    return ret;
}
static int LookupAttribArray(const int *attrib, const int attr)
{
    int ret = 0;
    for (int i = 0; attrib[i]; i+=2) {
        if (attrib[i] == attr) {
            switch (attr) {
                case WGL_DEPTH_BITS_ARB:
                    ret = cDepthBits;
                    break;
                case WGL_STENCIL_BITS_ARB:
                    ret = cStencilBits;
                    break;
                case WGL_AUX_BUFFERS_ARB:
                    ret = cAuxBuffers;
                    break;
                case WGL_SAMPLE_BUFFERS_ARB:
                    ret = cSampleBuf[0];
                    break;
                case WGL_SAMPLES_ARB:
                    ret = cSampleBuf[1];
                    break;
                default:
                    ret = attrib[i+1];
                    break;
            }
            break;
        }
    }
    return ret;
}

/* WGL_ARB_create_context attribs (== GLX values) -> EGL attribs */
static void translate_ctx_attribs(const int *wgl, EGLint *egl, int max)
{
    int n = 0;
    for (int i = 0; wgl[i] && n + 2 < max; i += 2) {
        switch (wgl[i]) {
        case WGL_CONTEXT_MAJOR_VERSION_ARB:
            egl[n++] = EGL_CONTEXT_MAJOR_VERSION; egl[n++] = wgl[i + 1];
            break;
        case WGL_CONTEXT_MINOR_VERSION_ARB:
            egl[n++] = EGL_CONTEXT_MINOR_VERSION; egl[n++] = wgl[i + 1];
            break;
        case WGL_CONTEXT_PROFILE_MASK_ARB:
            /* core = 1, compatibility = 2 in both APIs */
            egl[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK; egl[n++] = wgl[i + 1];
            break;
        case WGL_CONTEXT_FLAGS_ARB:
            if (wgl[i + 1] & WGL_CONTEXT_DEBUG_BIT_ARB) {
                egl[n++] = EGL_CONTEXT_OPENGL_DEBUG; egl[n++] = EGL_TRUE;
            }
            if (wgl[i + 1] & WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) {
                egl[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE; egl[n++] = EGL_TRUE;
            }
            break;
        default:
            break;
        }
    }
    egl[n] = EGL_NONE;
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
        EGLint ea[16];
        translate_ctx_attribs((const int *)&argsp[2], ea, 16);
        for (i = 0; ((i < MAX_LVLCNTX) && ctx[i]); i++);
        argsp[1] = (argsp[0])? i:0;
        if (argsp[1] == 0) {
            unbind();
            if (CompareAttribArray((const int *)&argsp[2])) {
                for (i = MAX_LVLCNTX; i > 0;) {
                    if (ctx[--i]) {
                        eglDestroyContext(dpy, ctx[i]);
                        ctx[i] = 0;
                    }
                }
                MGLActivateHandler(0, 0);
                ctx[0] = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ea);
            }
            ret = (ctx[0])? 1:0;
        }
        else {
            if (i == MAX_LVLCNTX) {
                eglDestroyContext(dpy, ctx[1]);
                for (i = 1; i < (MAX_LVLCNTX - 1); i++)
                    ctx[i] = ctx[i + 1];
                argsp[1] = i;
            }
            ctx[i] = eglCreateContext(dpy, cfg, ctx[i-1], ea);
            ret = (ctx[i])? 1:0;
        }
        if (!ret) {
            DPRINTF("wglCreateContextAttribsARB: eglCreateContext failed 0x%x", eglGetError());
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
#define WGL_DRAW_TO_PBUFFER_ARB 0x202D
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
            int prev_binded_texture = 0;
            EGLContext prev_context = eglGetCurrentContext();
            EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
            EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
            glGetIntegerv(PbufferGLBinding(hPbuffer[i].target), &prev_binded_texture);
            eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);
            glBindTexture(PbufferGLAttrib(hPbuffer[i].target), prev_binded_texture);
            glCopyTexImage2D(PbufferGLAttrib(hPbuffer[i].target), hPbuffer[i].level,
                PbufferGLAttrib(hPbuffer[i].format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
            eglMakeCurrent(dpy, prev_draw, prev_read, prev_context);
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
        const EGLint ia[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, cAlphaBits,
            EGL_DEPTH_SIZE, cDepthBits,
            EGL_NONE,
        };
        EGLint pa[] = {
            EGL_WIDTH, hPbuffer[i].width,
            EGL_HEIGHT, hPbuffer[i].height,
            EGL_NONE,
        };
        EGLConfig pbcfg;
        EGLint n = 0;
        if (!eglChooseConfig(dpy, ia, &pbcfg, 1, &n) || n < 1)
            argsp[0] = 0;
        else {
            PBDC[i] = eglCreatePbufferSurface(dpy, pbcfg, pa);
            PBRC[i] = eglCreateContext(dpy, pbcfg, eglGetCurrentContext(), ctx_compat);
            argsp[0] = (PBDC[i] != EGL_NO_SURFACE && PBRC[i] != EGL_NO_CONTEXT) ? 1 : 0;
        }
        argsp[1] = i;
        return;
    }
    FUNCP_HANDLER("wglDestroyPbufferARB") {
        uint32_t i;
        i = argsp[0] & (MAX_PBUFFER - 1);
        if (PBRC[i])
            eglDestroyContext(dpy, PBRC[i]);
        if (PBDC[i])
            eglDestroySurface(dpy, PBDC[i]);
        PBRC[i] = 0; PBDC[i] = 0;
        argsp[0] = 1;
        memset(&hPbuffer[i], 0, sizeof(HPBUFFERARB));
        return;
    }
    FUNCP_HANDLER("wglQueryPbufferARB") {
        uint32_t i = argsp[0] & (MAX_PBUFFER - 1);
#define WGL_PBUFFER_WIDTH_ARB   0x2034
#define WGL_PBUFFER_HEIGHT_ARB  0x2035
        switch(argsp[1]) {
            case WGL_PBUFFER_WIDTH_ARB:
                argsp[2] = hPbuffer[i].width;
                break;
            case WGL_PBUFFER_HEIGHT_ARB:
                argsp[2] = hPbuffer[i].height;
                break;
            case WGL_TEXTURE_TARGET_ARB:
                argsp[2] = hPbuffer[i].target;
                break;
            case WGL_TEXTURE_FORMAT_ARB:
                argsp[2] = hPbuffer[i].format;
                break;
            case WGL_MIPMAP_LEVEL_ARB:
                argsp[2] = hPbuffer[i].level;
                break;
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

#else /* !CONFIG_LINUX: no embed backend yet, the native (weak) one stays */

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
