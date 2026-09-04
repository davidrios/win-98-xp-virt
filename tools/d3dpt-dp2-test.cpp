/*
 * d3dpt-dp2-test: drives the display driver's records of the paravirtual
 * Direct3D executor (libd3dpt_exec, doc 15 M7c) without a guest. A
 * malloc'ed stand-in for the d3dpt-vga adapter's VRAM holds a render
 * target, a Z buffer and a texture exactly as dxg's heap would; the test
 * registers them (VRAM_SURFACE), opens a context (CTX_CREATE), sends the
 * D3D7TEST scene as the DP2 token stream the DX7 runtime emits for a
 * non-T&L HAL (guest-tools/src/d3dptvid/d3d7test.c draws the same scene
 * through IDirect3DDevice7 on XP), reads the frame back into "VRAM"
 * (READBACK) and checks pixels; then it feeds hostile records (a surface
 * outside VRAM, a truncated token stream, a lying record size) and
 * expects them refused. The frame is written as a BMP for the eye and for
 * the guest-side diff.
 *
 * Build: c++ -std=c++17 -O2 -o build/d3dpt-dp2-test tools/d3dpt-dp2-test.cpp \
 *          -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *          -Ithird_party/dxvk/include/native/directx -ldl
 * Run:   build/d3dpt-dp2-test [out.bmp]   (from the repo root, or D3DPT_EXEC_LIB / D3DPT_DXVK_LIB)
 */
#include <windows.h>
#include <d3d9.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../d3dpt/d3dpt_enc.h"
#include "../d3dpt/exec/d3dpt_exec.h"

static d3dpt_exec_t *X;
static uint32_t (*p_submit)(d3dpt_exec_t *, void *, uint32_t);
static uint8_t *vram;
static uint32_t dirty_calls, dirty_bytes;
static int failures;

static void cb_log(void *, const char *m) { printf("exec: %s\n", m); }
static void cb_active(void *, int on) { printf("exec: 3D %s\n", on ? "on" : "off"); }
static void cb_frame(void *, const void *, int, int, int) {}
static void cb_dirty(void *, uint32_t off, uint32_t bytes) { dirty_calls++; dirty_bytes += bytes; printf("vram dirty: %u +%u\n", off, bytes); }
static void doorbell(d3dpt_enc *e) { p_submit(X, e->shm, D3DPT_SHM_SIZE); }

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* --- the scene, shared with guest-tools/src/d3dptvid/d3d7test.c --- */
enum { W = 640, H = 480, TEX = 64 };
enum { RT_OFF = 0, RT_PITCH = W * 4, Z_OFF = 0x200000, Z_PITCH = W * 2, TEX_OFF = 0x300000, TEX_PITCH = TEX * 4, VRAM_SIZE = 0x400000 };
enum { H_RT = 1, H_Z = 2, H_TEX = 3, CTX = 1 };
#define CLEAR_COLOR 0xff203040u

struct tlv { float x, y, z, rhw; uint32_t diffuse, specular; float tu, tv; };
#define FVF_TLVERTEX 0x1c4   /* XYZRHW | DIFFUSE | SPECULAR | TEX1 */

static void fill_texture(uint8_t *p, int pitch) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            uint32_t c = ((x / 8) + (y / 8)) & 1 ? 0xffffffffu : 0xff2040ffu;
            if (y >= 24 && y < 40) c = (c & 0x00ffffffu) | 0x80000000u;      /* a half-transparent band */
            memcpy(p + y * pitch + x * 4, &c, 4);
        }
}

/* the DP2 token stream */
struct Dp2Buf {
    std::vector<uint8_t> b;
    void cmd(uint8_t op, uint16_t count) { b.push_back(op); b.push_back(0); b.push_back(count & 0xff); b.push_back(count >> 8); }
    void u16(uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xff); }
    void f32(float f) { uint32_t v; memcpy(&v, &f, 4); u32(v); }
    void rs(uint32_t s, uint32_t v) { cmd(8, 1); u32(s); u32(v); }
    void tss(uint16_t stage, uint16_t st, uint32_t v) { cmd(25, 1); u16(stage); u16(st); u32(v); }
    void viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { cmd(28, 1); u32(x); u32(y); u32(w); u32(h); }
    void zrange(float a, float z) { cmd(32, 1); f32(a); f32(z); }
    void clear(uint32_t flags, uint32_t color, float z) { cmd(42, 0); u32(flags); u32(color); f32(z); u32(0); }
    void trilist(uint16_t start, uint16_t count) { cmd(18, count); u16(start); }
    void trifan(uint16_t start, uint16_t count) { cmd(21, count); u16(start); }
    void tristrip(uint16_t start, uint16_t count) { cmd(19, count); u16(start); }
    void indexed_trilist2(uint16_t base, const std::vector<uint16_t> &idx) {
        cmd(26, idx.size() / 3); u16(base); for (uint16_t i : idx) u16(i);
    }
};

static tlv V(float x, float y, float z, uint32_t c, float u, float v) { return { x, y, z, 1.0f, c, 0xff000000u, u, v }; }

static void build_scene(Dp2Buf &d, std::vector<tlv> &vtx) {
    /* vertex buffer: quad (0..5), fan (6..11), triangle (12..14), alpha strip (15..18) */
    vtx = {
        V(100, 80, 0.5f, 0xffffffff, 0, 0), V(420, 80, 0.5f, 0xffffffff, 2, 0), V(100, 320, 0.5f, 0xffffffff, 0, 2),
        V(100, 320, 0.5f, 0xffffffff, 0, 2), V(420, 80, 0.5f, 0xffffffff, 2, 0), V(420, 320, 0.5f, 0xffffffff, 2, 2),
    };
    for (int i = 0; i < 6; i++) {
        float a = i ? (float)(i - 1) * 6.2831853f / 4.0f : 0.0f;
        uint32_t col[] = { 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff0000 };
        vtx.push_back(i == 0 ? V(480, 240, 0.3f, col[0], 0, 0)
                             : V(480 + 100 * cosf(a), 240 + 100 * sinf(a), 0.3f, col[i], 0, 0));
    }
    vtx.push_back(V(300, 40, 0.7f, 0xff00ffff, 0, 0)); vtx.push_back(V(600, 420, 0.7f, 0xff00ffff, 0, 0)); vtx.push_back(V(60, 460, 0.7f, 0xff00ffff, 0, 0));
    vtx.push_back(V(20, 360, 0.1f, 0x80ff0000, 0, 0)); vtx.push_back(V(220, 360, 0.1f, 0x80ff0000, 0, 0));
    vtx.push_back(V(20, 470, 0.1f, 0x80ff0000, 0, 0)); vtx.push_back(V(220, 470, 0.1f, 0x80ff0000, 0, 0));

    d.viewport(0, 0, W, H);
    d.zrange(0.0f, 1.0f);
    d.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
    d.rs(D3DRS_ZENABLE, 1); d.rs(D3DRS_ZWRITEENABLE, 1); d.rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d.rs(D3DRS_LIGHTING, 0); d.rs(D3DRS_CULLMODE, D3DCULL_NONE); d.rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    d.rs(D3DRS_ALPHABLENDENABLE, 0); d.rs(D3DRS_DITHERENABLE, 0); d.rs(D3DRS_SPECULARENABLE, 0);
    d.rs(3, 1); d.rs(41, 0);                                    /* DX7-only states: dropped by the host */
    /* the cyan triangle first, behind the quad (z 0.7) */
    d.tss(0, 0, 0); d.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d.indexed_trilist2(12, { 0, 1, 2 });
    /* the textured quad */
    d.tss(0, 0, H_TEX); d.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d.tss(0, 16, 1); d.tss(0, 17, 1); d.tss(0, 18, 1); d.tss(0, 12, D3DTADDRESS_WRAP);   /* point sampling, no mips */
    d.trilist(0, 2);
    /* the coloured fan in front */
    d.tss(0, 0, 0); d.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d.trifan(6, 4);
    /* a half-transparent red strip */
    d.rs(D3DRS_ALPHABLENDENABLE, 1); d.rs(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d.rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d.tristrip(15, 2);
    d.rs(D3DRS_ALPHABLENDENABLE, 0);
}

static bool bmp_write(const char *path, const uint8_t *bits, int pitch, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3;
    unsigned char hdr[54] = {}; unsigned size = 54 + row * h;
    hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> line(row);
    for (int y = h - 1; y >= 0; y--) {
        const unsigned char *src = bits + y * pitch;
        for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

static uint32_t px(int x, int y) { uint32_t v; memcpy(&v, vram + RT_OFF + y * RT_PITCH + x * 4, 4); return v & 0xffffff; }
static bool near_(uint32_t a, uint32_t b, int tol) {
    for (int i = 0; i < 24; i += 8) if (abs((int)((a >> i) & 255) - (int)((b >> i) & 255)) > tol) return false;
    return true;
}

static void vram_surface(d3dpt_enc *e, uint32_t handle, uint32_t off, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt, uint32_t caps) {
    d3dpt_vram_surface *s = (d3dpt_vram_surface *)d3dpt_enc_cmd(e, D3DPT_OP_VRAM_SURFACE, sizeof *s, 0);
    *s = { handle, off, w, h, pitch, fmt, caps, 1 };
}

static uint32_t send_dp2(d3dpt_enc *e, const Dp2Buf &d, const std::vector<tlv> &vtx, uint32_t cmd_bytes_claim = 0, uint32_t *err_off = nullptr) {
    uint32_t cb = (uint32_t)d.b.size(), vb = (uint32_t)(vtx.size() * sizeof(tlv));
    uint32_t off = d3dpt_enc_ret(e, 0);
    d3dpt_dp2 *a = (d3dpt_dp2 *)d3dpt_enc_cmd(e, D3DPT_OP_DP2, sizeof *a, D3DPT_ALIGN8(cb) + vb);
    *a = { CTX, off, 0, FVF_TLVERTEX, sizeof(tlv), cmd_bytes_claim ? cmd_bytes_claim : cb, vb, 0 };
    memcpy(a + 1, d.b.data(), cb);
    memcpy((uint8_t *)(a + 1) + D3DPT_ALIGN8(cb), vtx.data(), vb);
    d3dpt_enc_flush(e);
    if (e->last_status) return 0xffff0000u | e->last_status;
    d3dpt_ret *r = d3dpt_enc_result(e, off);
    if (err_off) *err_off = r->bytes;
    return r->hr;
}

static uint32_t readback(d3dpt_enc *e, uint32_t handle) { return d3dpt_enc_sync(e, D3DPT_OP_READBACK, handle); }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "d3dpt-dp2-test.bmp";
    const char *lib = getenv("D3DPT_EXEC_LIB");
    if (!lib) lib = "build/d3dpt/libd3dpt_exec.so";
    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
    auto p_version = (uint32_t (*)(void))dlsym(h, "d3dpt_exec_version");
    auto p_create = (d3dpt_exec_t *(*)(const d3dpt_exec_ops *))dlsym(h, "d3dpt_exec_create");
    auto p_attach = (void (*)(d3dpt_exec_t *, int))dlsym(h, "d3dpt_exec_attach");
    auto p_destroy = (void (*)(d3dpt_exec_t *))dlsym(h, "d3dpt_exec_destroy");
    auto p_set_vram = (void (*)(d3dpt_exec_t *, void *, uint32_t))dlsym(h, "d3dpt_exec_set_vram");
    p_submit = (uint32_t (*)(d3dpt_exec_t *, void *, uint32_t))dlsym(h, "d3dpt_exec_submit");
    if (!p_version || !p_create || !p_attach || !p_destroy || !p_submit || !p_set_vram) { fprintf(stderr, "bad executor library\n"); return 1; }
    printf("executor protocol %u (header %u)\n", p_version(), D3DPT_PROTO_VERSION);
    if (p_version() != D3DPT_PROTO_VERSION) { fprintf(stderr, "protocol mismatch\n"); return 1; }
    d3dpt_exec_ops ops = { nullptr, cb_log, cb_active, cb_frame, cb_dirty };
    X = p_create(&ops);
    if (!X) { printf("no executor (no DXVK / Vulkan device)\n"); return 77; }
    p_attach(X, 1);

    uint8_t *shm = (uint8_t *)calloc(1, D3DPT_SHM_SIZE);
    vram = (uint8_t *)calloc(1, VRAM_SIZE);
    p_set_vram(X, vram, VRAM_SIZE);
    fill_texture(vram + TEX_OFF, TEX_PITCH);
    d3dpt_enc enc;
    d3dpt_enc_init(&enc, shm, doorbell);

    /* --- the scene --- */
    vram_surface(&enc, H_RT, RT_OFF, W, H, RT_PITCH, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY);
    vram_surface(&enc, H_Z, Z_OFF, W, H, Z_PITCH, D3DFMT_D16, D3DPT_VS_ZBUFFER);
    vram_surface(&enc, H_TEX, TEX_OFF, TEX, TEX, TEX_PITCH, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE);
    {
        uint32_t off = d3dpt_enc_ret(&enc, 0);
        d3dpt_ctx_create *c = (d3dpt_ctx_create *)d3dpt_enc_cmd(&enc, D3DPT_OP_CTX_CREATE, sizeof *c, 0);
        *c = { CTX, off, H_RT, H_Z };
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == 0 && d3dpt_enc_result(&enc, off)->hr == 0, "context created (status %u hr 0x%08x)", enc.last_status, d3dpt_enc_result(&enc, off)->hr);
    }
    Dp2Buf d; std::vector<tlv> vtx;
    build_scene(d, vtx);
    uint32_t hr = send_dp2(&enc, d, vtx);
    CHECK(hr == 0, "DP2 scene (%zu token bytes, %zu vertices) -> 0x%08x", d.b.size(), vtx.size(), hr);
    hr = readback(&enc, H_RT);
    CHECK(hr == 0, "readback -> 0x%08x, vram_dirty %u calls %u bytes", hr, dirty_calls, dirty_bytes);
    CHECK(dirty_calls == 1 && dirty_bytes == RT_PITCH * H, "readback marked the whole target dirty");
    bmp_write(out, vram + RT_OFF, RT_PITCH, W, H);
    printf("frame -> %s\n", out);

    /* pixels: clear colour, the texture (white / blue cells, 8 px each, wrapped twice over 320 px = 2.5 px per texel),
     * the fan centre (white), the cyan triangle where nothing covers it, the blended strip */
    CHECK(px(620, 20) == (CLEAR_COLOR & 0xffffff), "background is the clear colour (0x%06x)", px(620, 20));
    uint32_t q1 = px(104, 84), q2 = px(124, 84);                 /* first cell blue, second cell white */
    CHECK(near_(q1, 0x2040ff, 2) && near_(q2, 0xffffff, 2), "textured quad cells 0x%06x 0x%06x", q1, q2);
    CHECK(near_(px(480, 240), 0xffffff, 2), "fan centre 0x%06x", px(480, 240));
    CHECK(near_(px(560, 400), 0x00ffff, 2), "cyan triangle 0x%06x", px(560, 400));
    CHECK(near_(px(380, 300), 0xffffff, 2) || near_(px(380, 300), 0x2040ff, 2), "quad hides the triangle (z test) 0x%06x", px(380, 300));
    uint32_t bl = px(40, 365);                                    /* 50% red over the clear colour */
    CHECK(near_(bl, 0x8f1820, 6), "alpha-blended strip 0x%06x", bl);

    /* the second readback without new drawing is a no-op (S_FALSE) */
    hr = readback(&enc, H_RT);
    CHECK(hr == 1, "readback without drawing -> S_FALSE (0x%08x)", hr);

    /* VRAM_DIRTY on the texture + a re-render picks up new texels */
    memset(vram + TEX_OFF, 0x40, TEX * TEX_PITCH);                /* dark grey, alpha 0x40 */
    { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY, sizeof *hh, 0); *hh = { H_TEX, 0 }; }
    Dp2Buf d2; d2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
    d2.tss(0, 0, H_TEX); d2.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d2.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d2.trilist(0, 2);
    hr = send_dp2(&enc, d2, vtx);
    hr |= readback(&enc, H_RT);
    CHECK(hr == 0 && near_(px(200, 200), 0x404040, 2), "texture re-read after VRAM_DIRTY (0x%06x)", px(200, 200));

    /* --- hostile records --- */
    vram_surface(&enc, 9, VRAM_SIZE - 4096, 64, 64, 256, D3DFMT_X8R8G8B8, D3DPT_VS_TEXTURE);
    d3dpt_enc_flush(&enc);
    CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "surface beyond VRAM refused (status %u)", enc.last_status);
    hr = send_dp2(&enc, d, vtx, (uint32_t)d.b.size() + 4096);
    CHECK(hr == (0xffff0000u | D3DPT_ERR_BAD_ARG), "DP2 record lying about its command bytes refused (0x%08x)", hr);
    Dp2Buf bad; bad.rs(D3DRS_ZENABLE, 1); bad.cmd(18, 3);           /* TRIANGLELIST without its start vertex */
    uint32_t err_off = 0;
    hr = send_dp2(&enc, bad, vtx, 0, &err_off);
    CHECK(hr == 0x88760BB8u && err_off == 12, "truncated token stream -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
    Dp2Buf oob; oob.trilist(100, 4);                                 /* vertices beyond the buffer: skipped, not fatal */
    hr = send_dp2(&enc, oob, vtx);
    CHECK(hr == 0, "out-of-range vertices skipped (0x%08x)", hr);
    { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_RELEASE, sizeof *hh, 0); *hh = { H_TEX, 0 }; }
    Dp2Buf gone; gone.tss(0, 0, H_TEX); gone.trilist(0, 2);
    hr = send_dp2(&enc, gone, vtx);
    CHECK(hr == 0, "released texture bound: drawn untextured, not fatal (0x%08x)", hr);

    p_attach(X, 0);
    p_destroy(X);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
