/*
 * Drive the embed library's window-less Mesa backend the way the guest
 * wrapper does (mesapt_mm.c activation order), without a guest: init QEMU
 * paused, InitMesaGL → pixel format → context → draw → MGLSwapBuffers, and
 * check that on_3d_active/on_3d_frame arrive with the right pixels (green
 * clear, red quad in the top-left → verifies the bottom-up → top-down flip).
 *
 * Linux only (EGL backend). Build & run from the repo root:
 *   cc -O1 -std=gnu11 -Iembed -o build/embed-3d-test tools/embed-3d-test.c \
 *      -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,$PWD/build/qemu -lepoxy \
 *      && build/embed-3d-test
 */
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "libqemu_embed.h"

/* backend entry points (exported by the .so on Linux; internal API) */
int InitMesaGL(void);
void FiniMesaGL(void);
void MGLTmpContext(void);
int MGLChoosePixelFormat(void);
int MGLSetPixelFormat(int, const void *);
int MGLCreateContext(uint32_t);
int MGLMakeCurrent(uint32_t, int);
int MGLSwapBuffers(void);
void MGLDeleteContext(int);
void MGLWndRelease(void);
int glwnd_ready(void);

#define MESAGL_MAGIC 0x5b5eb5e5

static int actives, frames, fw, fh;
static uint32_t px_tl, px_br, px_c;
/* zero-copy: the dma-bufs the backend offered, mmap'ed for checking */
static struct { int fd; int w, h, stride; void *map; size_t len; } slots[8];
static int dmabufs, readies, last_slot = -1;

static int on_3d_dmabuf(void *ud, int slot, int fd, int w, int h, int stride,
                        uint32_t fourcc, uint64_t modifier)
{
    printf("on_3d_dmabuf(slot %d fd %d %dx%d stride %d fourcc %c%c%c%c modifier 0x%llx)\n",
           slot, fd, w, h, stride, fourcc & 0xff, (fourcc >> 8) & 0xff,
           (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff, (unsigned long long)modifier);
    if (slot < 0 || slot >= 8) {
        return 0;
    }
    if (slots[slot].map) {
        munmap(slots[slot].map, slots[slot].len);
        close(slots[slot].fd);
    }
    slots[slot].fd = fd;
    slots[slot].w = w; slots[slot].h = h; slots[slot].stride = stride;
    slots[slot].len = (size_t)stride * h;
    slots[slot].map = mmap(NULL, slots[slot].len, PROT_READ, MAP_SHARED, fd, 0);
    if (slots[slot].map == MAP_FAILED) {
        printf("  mmap failed; declining zero-copy\n");
        slots[slot].map = NULL;
        return 0;
    }
    dmabufs++;
    return 1;
}

static void on_3d_frame_ready(void *ud, int slot)
{
    readies++;
    last_slot = slot;
    if (slot >= 0 && slot < 8 && slots[slot].map) {
        const uint8_t *p = slots[slot].map;
        int w = slots[slot].w, h = slots[slot].h, st = slots[slot].stride;
        frames++;
        fw = w;
        fh = h;
        px_tl = *(const uint32_t *)(p + 10 * st + 10 * 4);
        px_br = *(const uint32_t *)(p + (h - 10) * st + (w - 10) * 4);
        px_c = *(const uint32_t *)(p + (h / 2) * st + (w / 2) * 4);
    }
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
    px_tl = row[10 * (stride / 4) + 10];
    px_br = row[(h - 10) * (stride / 4) + (w - 10)];
    px_c = row[(h / 2) * (stride / 4) + (w / 2)];
}

int main(int argc, char **argv)
{
    const char *bios = argc > 1 ? argv[1] : "qemu/pc-bios";
    char *qargv[] = {
        "qemu-system-i386", "-machine", "pc", "-m", "32", "-net", "none",
        "-L", (char *)bios, "-nodefaults", "-vga", "std",
    };
    int want_zc = !(argc > 2 && !strcmp(argv[2], "readback"));
    qemu_embed_display_cb cb = {
        .on_3d_active = on_3d_active,
        .on_3d_frame = on_3d_frame,
        .on_3d_dmabuf = want_zc ? on_3d_dmabuf : NULL,
        .on_3d_frame_ready = want_zc ? on_3d_frame_ready : NULL,
    };
    if (qemu_embed_api_version() != QEMU_EMBED_API_VERSION) {
        printf("API version mismatch\n");
        return 1;
    }
    qemu_embed_t *e = qemu_embed_new(sizeof(qargv) / sizeof(qargv[0]), qargv, &cb, NULL);
    if (!e) {
        printf("qemu_embed_new failed\n");
        return 1;
    }

    /* BQL is held by this thread after init; mirror mesapt_mm.c */
    if (InitMesaGL() != 0) {
        printf("InitMesaGL failed (no libGL?)\n");
        return 1;
    }
    MGLTmpContext();
    int pf = MGLChoosePixelFormat();
    uint8_t pfd[64] = {0};
    int spf = MGLSetPixelFormat(pf, pfd);
    printf("pixel format %d set %d wnd_ready %d\n", pf, spf, glwnd_ready());
    int cc = MGLCreateContext(MESAGL_MAGIC);
    printf("MGLCreateContext -> %d (0 = ok)\n", cc);
    if (cc) {
        return 1;
    }
    MGLMakeCurrent(MESAGL_MAGIC, 0);
    printf("GL %s\n", (const char *)glGetString(GL_VERSION));

    glClearColor(0.f, 1.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    /* red quad over the top-left quadrant (GL y up: top half is y > 0) */
    glColor3f(1.f, 0.f, 0.f);
    glBegin(GL_QUADS);
    glVertex2f(-1.f, 0.f);
    glVertex2f(0.f, 0.f);
    glVertex2f(0.f, 1.f);
    glVertex2f(-1.f, 1.f);
    glEnd();
    MGLSwapBuffers();
    printf("frame %dx%d  top-left %08x  bottom-right %08x  center %08x\n",
           fw, fh, px_tl, px_br, px_c);
    int ok = frames == 1 && actives >= 1
          && (px_tl & 0xffffff) == 0xff0000
          && (px_br & 0xffffff) == 0x00ff00
          && (px_c & 0xffffff) == 0x00ff00;

    /* second frame: everything green (the pbuffer persists, no flicker) */
    glClear(GL_COLOR_BUFFER_BIT);
    MGLSwapBuffers();
    ok = ok && frames == 2 && (px_tl & 0xffffff) == 0x00ff00;

    MGLDeleteContext(0);
    MGLWndRelease();
    FiniMesaGL();
    printf("actives %d frames %d dmabufs %d ready %d (last slot %d) -> %s [%s]\n",
           actives, frames, dmabufs, readies, last_slot, ok ? "OK" : "FAIL",
           dmabufs ? "zero-copy" : "readback");
    fflush(stdout);
    /* one VM per process; cleanup is partial — exit without it */
    _exit(ok ? 0 : 1);
}
