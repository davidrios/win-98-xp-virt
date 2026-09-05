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
enum { TEX16_OFF = 0x310000, TEX16_PITCH = TEX * 2, TEXP8_OFF = 0x320000, TEXP8_PITCH = TEX };   /* v8: a 16-bit keyed texture, a palettized one */
enum { H_RT = 1, H_Z = 2, H_TEX = 3, H_TEX16 = 4, H_TEXP8 = 5, CTX = 1 };
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
    /* the DX8 DDI's self-contained draw (the display driver's rewrite of the
     * runtime's DRAWPRIMITIVE / DRAWINDEXEDPRIMITIVE): vertices and 16-bit
     * indices inline, indices relative to min_index */
    template <class T> void draw8(uint32_t prim, uint32_t prims, uint32_t fvf, const std::vector<T> &v,
                                  const std::vector<uint16_t> &idx = {}, uint32_t min_index = 0, uint32_t stride_lie = 0) {
        cmd(D3DPT_DP2_DRAW8, 0);
        u32(prim); u32(prims); u32(fvf); u32(stride_lie ? stride_lie : (uint32_t)sizeof(T));
        u32((uint32_t)v.size()); u32((uint32_t)idx.size()); u32(min_index); u32(0);
        for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); }
        while (b.size() % 4) b.push_back(0xcc);
        for (uint16_t i : idx) u16(i);
        while (b.size() % 4) b.push_back(0xcc);
    }
    void stateset(uint32_t op, uint32_t handle, uint32_t type = 0) { cmd(39, 1); u32(op); u32(handle); u32(type); }
    /* the DX8 shader tokens as the runtime lays them out (protocol v7: the
     * driver forwards them; the host keeps the shaders per context) */
    void create_vs(uint32_t handle, const std::vector<uint32_t> &decl, const std::vector<uint32_t> &code, uint32_t decl_lie = 0) {
        cmd(45, 1); u32(handle); u32(decl_lie ? decl_lie : (uint32_t)decl.size() * 4); u32((uint32_t)code.size() * 4);
        for (uint32_t t : decl) u32(t);
        for (uint32_t t : code) u32(t);
    }
    void create_ps(uint32_t handle, const std::vector<uint32_t> &code) { cmd(54, 1); u32(handle); u32((uint32_t)code.size() * 4); for (uint32_t t : code) u32(t); }
    void set_vs(uint32_t h) { cmd(47, 1); u32(h); }
    void set_ps(uint32_t h) { cmd(56, 1); u32(h); }
    void delete_vs(uint32_t h) { cmd(46, 1); u32(h); }
    void delete_ps(uint32_t h) { cmd(55, 1); u32(h); }
    void vs_const(uint32_t reg, float a, float b, float c, float d) { cmd(48, 1); u32(reg); u32(1); f32(a); f32(b); f32(c); f32(d); }
    void ps_const(uint32_t reg, float a, float b, float c, float d) { cmd(57, 1); u32(reg); u32(1); f32(a); f32(b); f32(c); f32(d); }
    void transform(uint32_t t, const float *m) { cmd(36, 1); u32(t); for (int i = 0; i < 16; i++) f32(m[i]); }
    /* the runtime's palette tokens (v8): SETPALETTE binds palette handle ->
     * surface, UPDATEPALETTE carries PALETTEENTRYs (r, g, b, flags) */
    void set_palette(uint32_t pal, uint32_t flags, uint32_t surface) { cmd(30, 1); u32(pal); u32(flags); u32(surface); }
    void update_palette(uint32_t pal, uint16_t start, const std::vector<uint32_t> &rgb) {
        cmd(31, 1); u32(pal); u16(start); u16((uint16_t)rgb.size());
        for (uint32_t c : rgb) { b.push_back((c >> 16) & 0xff); b.push_back((c >> 8) & 0xff); b.push_back(c & 0xff); b.push_back((c >> 24) & 0xff); }
    }
    /* TRIANGLEFAN_IMM as the runtime lays it out: the 4-byte header (which
     * may sit at offset 2 mod 4), padding to a DWORD boundary, the edge
     * flags, the vertices inline, padding so the next token starts at a
     * DWORD-aligned offset */
    template <class T> void trifan_imm(const std::vector<T> &v) {
        cmd(23, v.size() - 2);
        while (b.size() % 4) b.push_back(0xcc);                 /* the pad bytes are not data */
        u32(0);
        for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); }
        while (b.size() % 4) b.push_back(0xcc);
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
    /* twice, so the next token lands at offset 2 mod 4 (2 + 6 × 2 index words) */
    d.indexed_trilist2(12, { 0, 1, 2, 0, 1, 2 });
    /* the coloured fan in front, as inline vertices at that odd offset (the
     * DX8 runtime's legacy path does this; the parser must realign after it) */
    d.trifan_imm(std::vector<tlv>(vtx.begin() + 6, vtx.begin() + 12));
    /* the textured quad */
    d.tss(0, 0, H_TEX); d.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d.tss(0, 16, 1); d.tss(0, 17, 1); d.tss(0, 18, 1); d.tss(0, 12, D3DTADDRESS_WRAP);   /* point sampling, no mips */
    d.trilist(0, 2);
    d.tss(0, 0, 0); d.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
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

    uint32_t err_off = 0;

    /* --- the DX8 DDI: the driver's self-contained draws, state sets --- */
    {
        /* the same scene through DRAW8 tokens: the quad as an indexed
         * triangle list whose indices are relative to a MinIndex of 10, the
         * fan as a plain draw; then a recorded state set that switches the
         * quad to its diffuse colour, executed, captured back, deleted */
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6), fan(vtx.begin() + 6, vtx.begin() + 12);
        Dp2Buf e8;
        e8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e8.tss(0, 0, H_TEX); e8.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); e8.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        e8.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); e8.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); e8.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        e8.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        e8.tss(0, 0, 0); e8.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); e8.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        e8.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, e8, vtx);
        hr |= readback(&enc, H_RT);
        /* the texture is the re-read case's grey by now */
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(124, 84), 0x404040, 2) && near_(px(480, 240), 0xffffff, 2),
              "DRAW8 tokens: indexed quad (0x%06x 0x%06x) and fan (0x%06x)", px(104, 84), px(124, 84), px(480, 240));
        Dp2Buf sb;
        sb.stateset(0, 77);                                     /* BEGIN handle 77: record */
        sb.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);           /* the quad's colour from its diffuse (white) */
        sb.stateset(1, 77);                                     /* END */
        sb.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        sb.tss(0, 0, H_TEX); sb.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        sb.stateset(3, 77);                                     /* EXECUTE: SELECTARG2 again */
        sb.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        sb.stateset(4, 77);                                     /* CAPTURE and DELETE: accepted */
        sb.stateset(2, 77);
        sb.stateset(3, 77);                                     /* EXECUTE of a deleted set: logged, not fatal */
        hr = send_dp2(&enc, sb, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0xffffff, 2) && near_(px(124, 84), 0xffffff, 2),
              "state set recorded, executed: quad drawn from diffuse (0x%06x 0x%06x)", px(104, 84), px(124, 84));
        Dp2Buf bad8;
        bad8.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 99 }, 10);    /* an index beyond the copied range */
        hr = send_dp2(&enc, bad8, vtx);
        CHECK(hr == 0, "DRAW8 with an index beyond its vertices: skipped, not fatal (0x%08x)", hr);
        Dp2Buf lie8;
        lie8.draw8(4, 2, FVF_TLVERTEX, quad, {}, 0, 20);                        /* a stride that is not the FVF's */
        hr = send_dp2(&enc, lie8, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u, "DRAW8 lying about its stride -> D3DERR_COMMAND_UNPARSED (0x%08x)", hr);
    }

    /* --- the DX8 DDI: vertex and pixel shaders 1.x (protocol v7) --- */
    {
        /* hand-assembled shader models 1.x: a version token, instruction
         * tokens (opcode), parameter tokens (bit 31, register type in bits
         * 28..30, write mask / swizzle in 16..23, the register number) */
        const uint32_t VS11 = 0xFFFE0101u, PS11 = 0xFFFF0101u, END = 0x0000FFFFu, MOV = 1, MUL = 5, TEX = 66;
        auto dst = [](uint32_t type, uint32_t n) { return 0x80000000u | (type << 28) | (0xFu << 16) | n; };
        auto src = [](uint32_t type, uint32_t n) { return 0x80000000u | (type << 28) | (0xE4u << 16) | n; };
        enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5 };
        /* D3DVSD_* declaration tokens: STREAM(n) = 1 << 29 | n, REG(reg, type) = 2 << 29 | type << 16 | reg,
         * CONST(reg, count) = 4 << 29 | count << 25 | reg (+ count float4s), END = 0xFFFFFFFF */
        auto STREAM = [](uint32_t n) { return (1u << 29) | n; };
        auto REG = [](uint32_t reg, uint32_t type) { return (2u << 29) | (type << 16) | reg; };
        auto VSCONST = [](uint32_t reg, uint32_t count) { return (4u << 29) | (count << 25) | reg; };
        const uint32_t T_FLOAT3 = 2, T_FLOAT4 = 3, T_D3DCOLOR = 4;
        auto F = [](float f) { uint32_t v; memcpy(&v, &f, 4); return v; };
        /* vs 1.1: oPos = v0 (clip space as given), oD0 = v5 * c0 */
        std::vector<uint32_t> vs_code = { VS11, MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0), MUL, dst(R_ATTROUT, 0), src(R_INPUT, 5), src(R_CONST, 0), END };
        std::vector<uint32_t> decl_pc = { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), 0xFFFFFFFFu };
        /* ps 1.1: r0 = c0; and r0 = t0 * v0 (the texture times the diffuse) */
        std::vector<uint32_t> ps_const = { PS11, MOV, dst(R_TEMP, 0), src(R_CONST, 0), END };
        std::vector<uint32_t> ps_tex = { PS11, TEX, dst(R_TEXTURE, 0), MUL, dst(R_TEMP, 0), src(R_TEXTURE, 0), src(R_INPUT, 0), END };
        struct svtx { float x, y, z, w; uint32_t color; };                     /* 20 bytes: the declaration above */
        /* a white quad over the top-left quadrant in clip space */
        std::vector<svtx> sq = { { -1, 1, 0.5f, 1, 0xffffffffu }, { 0, 1, 0.5f, 1, 0xffffffffu }, { -1, 0, 0.5f, 1, 0xffffffffu },
                                 { -1, 0, 0.5f, 1, 0xffffffffu }, { 0, 1, 0.5f, 1, 0xffffffffu }, { 0, 0, 0.5f, 1, 0xffffffffu } };
        const uint32_t H_VS = 0x101, H_VS_FF = 0x103, H_VS_CONST = 0x105, H_PS = 0x201, H_PS_TEX = 0x203;
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6), fan(vtx.begin() + 6, vtx.begin() + 12);
        Dp2Buf s;
        s.create_vs(H_VS, decl_pc, vs_code);
        s.vs_const(0, 1, 0, 0, 1);                                             /* c0 = red */
        s.set_vs(H_VS);
        s.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        s.tss(0, 0, 0); s.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); s.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        s.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); s.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        s.draw8(4, 2, H_VS, sq);
        hr = send_dp2(&enc, s, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0xff0000, 2) && px(500, 400) == (CLEAR_COLOR & 0xffffff),
              "vs 1.1 through its declaration, c0 red: quad 0x%06x, outside 0x%06x", px(100, 100), px(500, 400));
        Dp2Buf s2;
        s2.vs_const(0, 0, 1, 0, 1);                                            /* c0 = green: the constant alone changes the frame */
        s2.draw8(4, 2, H_VS, sq);
        hr = send_dp2(&enc, s2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x00ff00, 2), "vertex shader constant c0 green: 0x%06x", px(100, 100));
        /* a declaration-only shader: the fixed function on a layout that is
         * no FVF (the colour before the position), identity transforms */
        struct ffvtx { uint32_t color; float x, y, z; };
        std::vector<ffvtx> fq;
        for (const svtx &v : sq) fq.push_back({ 0xff00ffffu, v.x, v.y, v.z });
        static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        Dp2Buf s3;
        s3.create_vs(H_VS_FF, { STREAM(0), REG(5, T_D3DCOLOR), REG(0, T_FLOAT3), 0xFFFFFFFFu }, {});
        s3.transform(256, ident); s3.transform(2, ident); s3.transform(3, ident);
        s3.rs(D3DRS_LIGHTING, 0);
        s3.set_vs(H_VS_FF);
        s3.draw8(4, 2, H_VS_FF, fq);
        hr = send_dp2(&enc, s3, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x00ffff, 2), "declaration-only shader (fixed function, colour first): 0x%06x", px(100, 100));
        /* D3DVSD_CONST in the declaration: loaded when the shader is set */
        Dp2Buf s4;
        s4.create_vs(H_VS_CONST, { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), VSCONST(0, 1), F(0), F(0), F(1), F(1), 0xFFFFFFFFu }, vs_code);
        s4.set_vs(H_VS_CONST);
        s4.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s4, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2), "declaration constant c0 blue: 0x%06x", px(100, 100));
        /* a pixel shader over it: r0 = c0 (yellow); then off again */
        Dp2Buf s5;
        s5.create_ps(H_PS, ps_const);
        s5.ps_const(0, 1, 1, 0, 1);
        s5.set_ps(H_PS);
        s5.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s5, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0xffff00, 2), "ps 1.1 r0 = c0 yellow: 0x%06x", px(100, 100));
        Dp2Buf s6;
        s6.set_ps(0);
        s6.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s6, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2), "pixel shader off: the vertex colour again 0x%06x", px(100, 100));
        /* a texturing pixel shader on fixed-function (XYZRHW) vertices: the
         * texture is the grey of the re-read case by now */
        Dp2Buf s7;
        s7.create_ps(H_PS_TEX, ps_tex);
        s7.set_ps(H_PS_TEX);
        s7.tss(0, 0, H_TEX);
        s7.set_vs(FVF_TLVERTEX);
        s7.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        s7.set_ps(0); s7.tss(0, 0, 0);
        hr = send_dp2(&enc, s7, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(300, 200), 0x404040, 2), "ps 1.1 tex t0 * v0 on FVF vertices: 0x%06x 0x%06x", px(104, 84), px(300, 200));
        /* hostile: a function with no END and garbage tokens (refused, the
         * draw with it skipped), a declaration reading stream 1 (skipped), a
         * declaration wider than the stride (skipped), constants off the
         * scale (dropped), a CREATEVERTEXSHADER lying about its sizes
         * (COMMAND_UNPARSED); then the FVF path still draws */
        Dp2Buf s8;
        s8.create_vs(0x107, decl_pc, { VS11, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu });
        s8.set_vs(0x107);
        s8.draw8(4, 2, 0x107, sq);
        /* garbage with a proper END: an unknown opcode, an instruction without
         * its operands, a register off the file (DXVK's compiler asserts on
         * the first two — the process would die — so the executor's own
         * validator must refuse them first) */
        s8.create_vs(0x10f, decl_pc, { VS11, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, END });
        s8.create_ps(0x20f, { PS11, 0xDEADBEEFu, 0xDEADBEEFu, END });
        s8.set_vs(0x10f); s8.set_ps(0x20f);
        s8.draw8(4, 2, 0x10f, sq);
        s8.create_vs(0x111, decl_pc, { VS11, MOV, END });
        s8.create_ps(0x211, { PS11, MOV, dst(R_TEMP, 0), src(R_RASTOUT, 0), END });
        s8.set_vs(0x111); s8.set_ps(0x211);
        s8.draw8(4, 2, 0x111, sq);
        s8.create_vs(0x113, decl_pc, { VS11, MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0), MUL, dst(R_CONST, 0), src(R_INPUT, 5), src(R_CONST, 0), END });
        s8.set_vs(0x113);
        s8.draw8(4, 2, 0x113, sq);
        s8.set_ps(0);
        s8.create_vs(0x109, { STREAM(1), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), 0xFFFFFFFFu }, vs_code);
        s8.set_vs(0x109);
        s8.draw8(4, 2, 0x109, sq);
        s8.create_vs(0x10b, { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), REG(7, T_FLOAT4), 0xFFFFFFFFu }, vs_code);
        s8.set_vs(0x10b);
        s8.draw8(4, 2, 0x10b, sq);
        s8.vs_const(300, 1, 1, 1, 1);
        s8.ps_const(250, 1, 1, 1, 1);
        s8.set_vs(0x1ff);                                                     /* never created */
        s8.draw8(4, 2, 0x1ff, sq);
        s8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        s8.set_vs(FVF_TLVERTEX);
        s8.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, s8, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && px(100, 100) == (CLEAR_COLOR & 0xffffff) && near_(px(480, 240), 0xffffff, 2),
              "garbage function / stream 1 / wide declaration / unknown handle / wild constants: skipped, not fatal; the fan still draws (0x%06x 0x%06x)", px(100, 100), px(480, 240));
        Dp2Buf s9;
        s9.create_vs(0x10d, decl_pc, vs_code, 4096);                          /* the declaration size lies */
        hr = send_dp2(&enc, s9, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u && err_off == 0, "CREATEVERTEXSHADER lying about its declaration size -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
        Dp2Buf s10;
        s10.delete_vs(H_VS); s10.delete_vs(H_VS_FF); s10.delete_vs(H_VS_CONST); s10.delete_vs(0x109); s10.delete_vs(0x10b);
        s10.delete_ps(H_PS); s10.delete_ps(H_PS_TEX); s10.delete_ps(0x2ff);
        s10.set_vs(FVF_TLVERTEX);
        s10.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, s10, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(480, 240), 0xffffff, 2), "shaders deleted, the fan still draws (0x%06x)", px(480, 240));
    }

    /* --- palettized textures and colour keying (v8) --- */
    {
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        /* a P8 texture: index = (x / 8 + y / 8) & 3, so the quad's cells cycle
         * through four palette entries; a R5G6B5 checker of blue / white */
        for (int y = 0; y < TEX; y++)
            for (int x = 0; x < TEX; x++) {
                vram[TEXP8_OFF + y * TEXP8_PITCH + x] = (uint8_t)(((x / 8) + (y / 8)) & 3);
                uint16_t c = ((x / 8) + (y / 8)) & 1 ? 0xffff : 0x001f;
                memcpy(vram + TEX16_OFF + y * TEX16_PITCH + x * 2, &c, 2);
            }
        vram_surface(&enc, H_TEXP8, TEXP8_OFF, TEX, TEX, TEXP8_PITCH, D3DFMT_P8, D3DPT_VS_TEXTURE);
        vram_surface(&enc, H_TEX16, TEX16_OFF, TEX, TEX, TEX16_PITCH, D3DFMT_R5G6B5, D3DPT_VS_TEXTURE);
        Dp2Buf p1;
        std::vector<uint32_t> pal(256, 0);
        pal[0] = 0xffff0000u; pal[1] = 0xff00ff00u; pal[2] = 0xff0000ffu; pal[3] = 0xffffff00u;
        p1.update_palette(77, 0, pal);
        p1.set_palette(77, 1, H_TEXP8);
        p1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        p1.tss(0, 0, H_TEXP8); p1.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); p1.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        p1.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); p1.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        p1.tss(0, 16, 1); p1.tss(0, 17, 1); p1.tss(0, 18, 1);
        p1.set_vs(FVF_TLVERTEX);
        p1.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, p1, vtx);
        hr |= readback(&enc, H_RT);
        /* the quad maps 2 texels per 2.5 px: cell 0 at (104, 84), cell 1 at (124, 84), cell 2 at (144, 84), cell 3 at (164, 84) */
        CHECK(hr == 0 && near_(px(104, 84), 0xff0000, 2) && near_(px(124, 84), 0x00ff00, 2) && near_(px(144, 84), 0x0000ff, 2) && near_(px(164, 84), 0xffff00, 2),
              "P8 texture through its palette: 0x%06x 0x%06x 0x%06x 0x%06x", px(104, 84), px(124, 84), px(144, 84), px(164, 84));
        Dp2Buf p2;
        p2.update_palette(77, 1, { 0xff00ffffu });                          /* entry 1 becomes cyan: the texture is re-expanded */
        p2.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, p2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(124, 84), 0x00ffff, 2) && near_(px(104, 84), 0xff0000, 2), "UPDATEPALETTE changes the texels: 0x%06x 0x%06x", px(124, 84), px(104, 84));
        /* colour key: blue (0x001f) keyed on the 16-bit checker; render state
         * 41 on -> the blue cells show the clear colour, off -> blue again */
        { d3dpt_u32x4 *k = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_COLORKEY, sizeof *k, 0); *k = { H_TEX16, 0x001f, 0x001f, 1 }; }
        Dp2Buf k1;
        k1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k1.rs(41, 1);
        k1.tss(0, 0, H_TEX16);
        k1.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, k1, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && px(104, 84) == (CLEAR_COLOR & 0xffffff) && near_(px(124, 84), 0xffffff, 2),
              "colour-keyed R5G6B5 texture, COLORKEYENABLE on: keyed cell 0x%06x, other 0x%06x", px(104, 84), px(124, 84));
        Dp2Buf k2;
        k2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k2.rs(41, 0);
        k2.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, k2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "COLORKEYENABLE off: the keyed cell draws again 0x%06x", px(104, 84));
        /* the app's own alpha test survives the key: ALPHATESTENABLE on with
         * ALWAYS, key on -> the keyed cell still draws (the app's test wins),
         * then the key removed and the alpha test off */
        Dp2Buf k3;
        k3.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k3.rs(15, 1); k3.rs(25, D3DCMP_ALWAYS); k3.rs(41, 1);
        k3.draw8(4, 2, FVF_TLVERTEX, quad);
        k3.rs(15, 0); k3.rs(41, 0);
        hr = send_dp2(&enc, k3, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "the app's own alpha test (ALWAYS) wins over the key: 0x%06x", px(104, 84));
        { d3dpt_u32x4 *k = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_COLORKEY, sizeof *k, 0); *k = { H_TEX16, 0, 0, 0 }; }
        Dp2Buf k4;
        k4.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k4.rs(41, 1);
        k4.draw8(4, 2, FVF_TLVERTEX, quad);
        k4.rs(41, 0); k4.tss(0, 0, 0);
        hr = send_dp2(&enc, k4, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "key removed: the cell draws with COLORKEYENABLE on 0x%06x", px(104, 84));
        /* hostile: a palette beyond 256 entries, a SETPALETTE on an unknown surface */
        Dp2Buf p3; p3.update_palette(78, 250, std::vector<uint32_t>(10, 0));
        hr = send_dp2(&enc, p3, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u, "UPDATEPALETTE beyond 256 entries -> D3DERR_COMMAND_UNPARSED (0x%08x)", hr);
        Dp2Buf p4; p4.set_palette(77, 1, 999); p4.draw8(6, 4, FVF_TLVERTEX, std::vector<tlv>(vtx.begin() + 6, vtx.begin() + 12));
        hr = send_dp2(&enc, p4, vtx);
        CHECK(hr == 0, "SETPALETTE on an unknown surface: ignored (0x%08x)", hr);
    }

    /* --- hostile records --- */
    vram_surface(&enc, 9, VRAM_SIZE - 4096, 64, 64, 256, D3DFMT_X8R8G8B8, D3DPT_VS_TEXTURE);
    d3dpt_enc_flush(&enc);
    CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "surface beyond VRAM refused (status %u)", enc.last_status);
    hr = send_dp2(&enc, d, vtx, (uint32_t)d.b.size() + 4096);
    CHECK(hr == (0xffff0000u | D3DPT_ERR_BAD_ARG), "DP2 record lying about its command bytes refused (0x%08x)", hr);
    Dp2Buf bad; bad.rs(D3DRS_ZENABLE, 1); bad.cmd(18, 3);           /* TRIANGLELIST without its start vertex */
    hr = send_dp2(&enc, bad, vtx, 0, &err_off);
    CHECK(hr == 0x88760BB8u && err_off == 12, "truncated token stream -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
    /* a light index / transform id off the scale: DXVK would grow its light
     * array to the index (std::bad_alloc, uncatchable across its own
     * unwinder: the process aborts) or index past its transform array */
    Dp2Buf wild; wild.cmd(34, 2); wild.u32(0xfffffff0u); wild.u32(0); wild.u32(0xfffffff0u); wild.u32(2); for (int i = 0; i < 26; i++) wild.u32(0);
    wild.cmd(36, 1); wild.u32(0x7fffffffu); for (int i = 0; i < 16; i++) wild.f32(i % 5 == 0 ? 1.0f : 0.0f);
    wild.trilist(0, 2);
    hr = send_dp2(&enc, wild, vtx);
    CHECK(hr == 0, "wild light index / transform id dropped, the draw still runs (0x%08x)", hr);
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
