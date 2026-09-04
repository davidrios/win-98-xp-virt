/*
 * d3dpt-exec-test: drives the paravirtual Direct3D executor (libd3dpt_exec,
 * doc 14) without a guest. Encodes the D3D9TEST scene (guest-tools/src/
 * d3d9test.c: spinning colored triangle) with the guest's own encoder
 * (d3dpt/d3dpt_enc.h) into a malloc'ed stand-in for the shared window and
 * submits it batch by batch, exactly as the guest d3d9.dll does; the
 * frame callback writes frame N as a BMP. Proves decoder + executor +
 * readback before a guest exists, and stays the regression test after.
 *
 * Build: c++ -std=c++17 -O2 -o build/d3dpt-exec-test tools/d3dpt-exec-test.cpp \
 *          -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *          -Ithird_party/dxvk/include/native/directx -ldl
 * Run:   build/d3dpt-exec-test [out.bmp] [frames] [dump_frame]   (from the repo root,
 *        so build/d3dpt and build/dxvk are found; or D3DPT_EXEC_LIB / D3DPT_DXVK_LIB)
 */
#include <windows.h>
#include <d3d9.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include "../d3dpt/d3dpt_enc.h"
#include "../d3dpt/exec/d3dpt_exec.h"

static d3dpt_exec_t *X;
static uint32_t (*p_submit)(d3dpt_exec_t *, void *, uint32_t);
static const char *out_path = "d3dpt-exec-test.bmp";
static int dump_frame = -1, frame_no = 0, frames_seen = 0;

static bool bmp_write(const char *path, const void *bits, int pitch, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  int row = (w * 3 + 3) & ~3;
  unsigned char hdr[54] = {}; unsigned size = 54 + row * h;
  hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
  memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24;
  fwrite(hdr, 1, 54, f);
  std::vector<unsigned char> line(row);
  for (int y = h - 1; y >= 0; y--) {
    const unsigned char *src = (const unsigned char *)bits + y * pitch;
    for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
    fwrite(line.data(), 1, row, f);
  }
  fclose(f);
  return true;
}

static void cb_log(void *, const char *m) { printf("exec: %s\n", m); }
static void cb_active(void *, int on) { printf("exec: 3D %s\n", on ? "on" : "off"); }
static void cb_frame(void *, const void *px, int w, int h, int stride) {
  frames_seen++;
  if (frame_no == dump_frame)
    printf("frame %d -> %s (%s)\n", frame_no, out_path, bmp_write(out_path, px, stride, w, h) ? "written" : "write failed");
}

static void doorbell(d3dpt_enc *e) { p_submit(X, e->shm, D3DPT_SHM_SIZE); }

struct vtx { float x, y, z, rhw; DWORD color; };

int main(int argc, char **argv) {
  if (argc > 1) out_path = argv[1];
  int frames = argc > 2 ? atoi(argv[2]) : 60;
  dump_frame = argc > 3 ? atoi(argv[3]) : frames - 1;

  const char *lib = getenv("D3DPT_EXEC_LIB");
  if (!lib) lib = "build/d3dpt/libd3dpt_exec.so";
  void *h = dlopen(lib, RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
  auto p_version = (uint32_t (*)(void))dlsym(h, "d3dpt_exec_version");
  auto p_create = (d3dpt_exec_t *(*)(const d3dpt_exec_ops *))dlsym(h, "d3dpt_exec_create");
  auto p_attach = (void (*)(d3dpt_exec_t *, int))dlsym(h, "d3dpt_exec_attach");
  auto p_destroy = (void (*)(d3dpt_exec_t *))dlsym(h, "d3dpt_exec_destroy");
  p_submit = (uint32_t (*)(d3dpt_exec_t *, void *, uint32_t))dlsym(h, "d3dpt_exec_submit");
  if (!p_version || !p_create || !p_attach || !p_destroy || !p_submit) { fprintf(stderr, "bad executor library\n"); return 1; }
  printf("executor protocol %u (header %u)\n", p_version(), D3DPT_PROTO_VERSION);
  d3dpt_exec_ops ops = { nullptr, cb_log, cb_active, cb_frame };
  X = p_create(&ops);
  if (!X) { printf("executor refused (no DXVK / no Vulkan device)\n"); return 2; }
  p_attach(X, 1);

  std::vector<uint8_t> shm(D3DPT_SHM_SIZE);
  d3dpt_enc enc;
  d3dpt_enc_init(&enc, shm.data(), doorbell);

  /* adapter identifier + caps, as d3d9test prints them */
  {
    uint32_t off = d3dpt_enc_ret(&enc, sizeof(d3dpt_adapter_info) + 64 * sizeof(d3dpt_mode));
    d3dpt_get_adapter *a = (d3dpt_get_adapter *)d3dpt_enc_cmd(&enc, D3DPT_OP_GET_ADAPTER, sizeof(d3dpt_get_adapter), 0);
    a->adapter = 0; a->ret_off = off;
    d3dpt_enc_flush(&enc);
    d3dpt_ret *r = d3dpt_enc_result(&enc, off);
    auto *info = (const d3dpt_adapter_info *)(r + 1);
    const D3DCAPS9 *caps = (const D3DCAPS9 *)info->caps;
    printf("adapter \"%s\" driver \"%s\" vendor %04x device %04x (hr 0x%08x, %u modes)\n", info->identifier.description,
           info->identifier.driver, info->identifier.vendor_id, info->identifier.device_id, r->hr, info->mode_count);
    printf("caps: vs %u.%u ps %u.%u max texture %ux%u HW T&L %s\n",
           D3DSHADER_VERSION_MAJOR(caps->VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps->VertexShaderVersion),
           D3DSHADER_VERSION_MAJOR(caps->PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps->PixelShaderVersion),
           caps->MaxTextureWidth, caps->MaxTextureHeight, (caps->DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "yes" : "no");
  }
  /* CreateDevice as d3d9test does: 640x480 windowed, DISCARD, software VP */
  const uint32_t dev = d3dpt_enc_handle(&enc);
  {
    uint32_t off = d3dpt_enc_ret(&enc, 0);
    d3dpt_create_device *c = (d3dpt_create_device *)d3dpt_enc_cmd(&enc, D3DPT_OP_CREATE_DEVICE, sizeof(d3dpt_create_device), 0);
    memset(c, 0, sizeof *c);
    c->handle = dev; c->ret_off = off; c->adapter = 0; c->devtype = D3DDEVTYPE_HAL; c->behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    c->pp.width = 640; c->pp.height = 480; c->pp.format = D3DFMT_X8R8G8B8; c->pp.backbuffer_count = 1;
    c->pp.swap_effect = D3DSWAPEFFECT_DISCARD; c->pp.windowed = 1; c->pp.interval = D3DPRESENT_INTERVAL_IMMEDIATE;
    /* Max Payne's 32-bit ask: D3DFMT_D32 auto depth. DXVK has no D32; the
     * executor's depth_norm must make this succeed anyway. */
    c->pp.auto_depth = 1; c->pp.depth_format = D3DFMT_D32;
    d3dpt_enc_flush(&enc);
    uint32_t hr = d3dpt_enc_result(&enc, off)->hr;
    printf("CreateDevice -> 0x%08x (batch status %u)\n", hr, enc.last_status);
    if (FAILED(hr)) return 3;
  }

  auto t0 = std::chrono::steady_clock::now();
  float ang = 0.0f;
  for (frame_no = 0; frame_no < frames; frame_no++) {
    vtx v[3];
    for (int i = 0; i < 3; i++) {
      float a = ang + (float)i * 2.0943951f;
      v[i].x = 320.0f + 180.0f * cosf(a); v[i].y = 240.0f + 180.0f * sinf(a); v[i].z = 0.5f; v[i].rhw = 1.0f;
      v[i].color = i == 0 ? 0xffff0000 : i == 1 ? 0xff00ff00 : 0xff0000ff;
    }
    ang += 0.02f;
    d3dpt_clear *cl = (d3dpt_clear *)d3dpt_enc_cmd(&enc, D3DPT_OP_CLEAR, sizeof(d3dpt_clear), 0);
    cl->count = 0; cl->flags = D3DCLEAR_TARGET; cl->color = 0xff202040; cl->z = 1.0f; cl->stencil = 0; cl->pad = 0;
    d3dpt_enc_nobody(&enc, D3DPT_OP_BEGIN_SCENE);
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_RENDER_STATE, D3DRS_LIGHTING, FALSE);
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_RENDER_STATE, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_FVF, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, 0);
    d3dpt_draw_up *d = (d3dpt_draw_up *)d3dpt_enc_cmd(&enc, D3DPT_OP_DRAW_PRIMITIVE_UP, sizeof(d3dpt_draw_up), sizeof v);
    d->type = D3DPT_TRIANGLELIST; d->prim_count = 1; d->stride = sizeof v[0]; d->bytes = sizeof v;
    memcpy(d + 1, v, sizeof v);
    d3dpt_enc_nobody(&enc, D3DPT_OP_END_SCENE);
    uint32_t hr = d3dpt_enc_sync(&enc, D3DPT_OP_PRESENT, dev);
    if (FAILED(hr) || enc.last_status) { printf("Present 0x%08x status %u at frame %d\n", hr, enc.last_status, frame_no); break; }
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("%d frames, %d delivered, %.1f ms, %.0f fps, %u batches\n", frames, frames_seen, ms, frames * 1000.0 / ms, d3dpt_enc_hdr(&enc)->batches);

  /* a hostile batch must be refused, not executed */
  {
    d3dpt_draw_up *d = (d3dpt_draw_up *)d3dpt_enc_cmd(&enc, D3DPT_OP_DRAW_PRIMITIVE_UP, sizeof(d3dpt_draw_up), 16);
    d->type = D3DPT_TRIANGLELIST; d->prim_count = 1000000; d->stride = 20; d->bytes = 16;
    d3dpt_enc_flush(&enc);
    printf("oversized DrawPrimitiveUP -> status %u (expect %u)\n", enc.last_status, D3DPT_ERR_BAD_ARG);
    if (enc.last_status != D3DPT_ERR_BAD_ARG) return 4;
  }
  d3dpt_handle *rel = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_RELEASE, sizeof *rel, 0);
  rel->handle = dev; rel->pad = 0;
  d3dpt_enc_flush(&enc);
  p_attach(X, 0);
  p_destroy(X);
  return frames_seen == frames ? 0 : 5;
}
