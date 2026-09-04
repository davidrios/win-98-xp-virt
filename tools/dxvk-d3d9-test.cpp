/*
 * dxvk-d3d9-test: drive the native DXVK d3d9 library (the D3D executor,
 * doc 14 P0b / ADR-007) without a guest: create the device on a hidden
 * SDL2 window, clear, draw a lit textured triangle with the fixed
 * function pipeline, read the back buffer back through GetRenderTargetData
 * and write it as a 24-bit BMP (the d3dgame -dump format, diffable with
 * tools/bmpdiff.py). Prints DXVK's adapter line and per-frame timing.
 *
 * Build (macOS and Linux alike; add -ldl on Linux):
 *   c++ -std=c++17 -O2 -o build/dxvk-d3d9-test tools/dxvk-d3d9-test.cpp \
 *     -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *     -Ithird_party/dxvk/include/native/directx $(pkg-config --cflags --libs sdl2) \
 *     -Wl,-rpath,$PWD/build/dxvk/src/d3d9
 * Run (Linux: just DXVK_WSI_DRIVER=SDL2. macOS; DXVK dlopens SDL2 and the Vulkan loader by bare name):
 *   DYLD_LIBRARY_PATH=/opt/homebrew/lib SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib \
 *   VK_ICD_FILENAMES=<icd.json> DXVK_WSI_DRIVER=SDL2 DXVK_LOG_LEVEL=info \
 *   build/dxvk-d3d9-test [out.bmp] [frames]
 * NOWINDOW=1 DXVK_WSI_DRIVER=Headless: no SDL window at all (patch 04's driver).
 * A refused device prints DXVK's reason (e.g. "Device does not support
 * required feature 'nullDescriptor'" on MoltenVK). Passes on KosmicKrisp
 * (LunarG SDK, macOS 26): <icd.json> =
 * ~/VulkanSDK/<ver>/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json.
 */
#include <windows.h>
#include <d3d9.h>
#include <SDL.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>

struct Vtx { float x, y, z; float nx, ny, nz; float u, v; };
#define FVF_PNT (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

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

static void matrix(D3DMATRIX &m, const float *v) { memcpy(&m, v, 64); }

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "dxvk-d3d9-test.bmp";
  int frames = argc > 2 ? atoi(argv[2]) : 60;
  const int W = 640, H = 480;

  const char *lib = getenv("DXVK_D3D9_LIB");
  if (!lib) lib =
#ifdef __APPLE__
    "libdxvk_d3d9.0.dylib";
#else
    "libdxvk_d3d9.so.0";
#endif
  void *h = dlopen(lib, RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
  auto create = (IDirect3D9 *(*)(UINT))dlsym(h, "Direct3DCreate9");
  if (!create) { fprintf(stderr, "no Direct3DCreate9 in %s\n", lib); return 1; }

  /* NOWINDOW=1: no SDL, NULL device window (DXVK_WSI_DRIVER=Headless, patch 04):
   * DXVK creates no presenter and Present is a no-op, the path the paravirtual
   * device's executor uses; the frame still comes out through GetRenderTargetData */
  const bool nowindow = getenv("NOWINDOW") && atoi(getenv("NOWINDOW"));
  SDL_Window *win = nullptr;
  if (!nowindow) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    win = SDL_CreateWindow("dxvk-d3d9-test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                           SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  }

  IDirect3D9 *d3d = nullptr;
  try { d3d = create(D3D_SDK_VERSION); } catch (...) { d3d = nullptr; }
  if (!d3d) { printf("Direct3DCreate9 failed: no usable Vulkan device (see DXVK's info/warn lines above)\n"); return 2; }
  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
  D3DCAPS9 caps = {};
  HRESULT hr = d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
  printf("adapter \"%s\" driver \"%s\" caps 0x%08x vs %u.%u ps %u.%u\n", id.Description, id.Driver, (unsigned)hr,
         (unsigned)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
         (unsigned)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = (HWND)win; pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D16;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = nullptr;
  hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) { printf("CreateDevice failed 0x%08x\n", (unsigned)hr); return 2; }
  printf("device %dx%d windowed X8R8G8B8 created\n", W, H);

  /* checker texture with mipmaps */
  IDirect3DTexture9 *tex = nullptr;
  if (FAILED(dev->CreateTexture(64, 64, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) { printf("CreateTexture failed\n"); return 3; }
  for (int level = 0, w = 64; w >= 1; level++, w >>= 1) {
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(level, &lr, nullptr, 0))) break;
    for (int y = 0; y < w; y++) for (int x = 0; x < w; x++) {
      int c = ((x * 8 / w) ^ (y * 8 / w)) & 1;
      ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = c ? 0xffe0d040 : 0xff203040;
    }
    tex->UnlockRect(level);
  }
  IDirect3DVertexBuffer9 *vb = nullptr;
  Vtx tri[3] = { { -1.5f, -1.0f, 0, 0, 0, -1, 0, 1 }, { 0.0f, 1.2f, 0, 0, 0, -1, 0.5f, 0 }, { 1.5f, -1.0f, 0, 0, 0, -1, 1, 1 } };
  if (FAILED(dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, &vb, nullptr))) { printf("CreateVertexBuffer failed\n"); return 3; }
  void *p; vb->Lock(0, 0, &p, 0); memcpy(p, tri, sizeof(tri)); vb->Unlock();

  D3DLIGHT9 l = {}; l.Type = D3DLIGHT_DIRECTIONAL; l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = 1; l.Direction.x = 0.3f; l.Direction.y = -0.5f; l.Direction.z = 1;
  dev->SetLight(0, &l); dev->LightEnable(0, TRUE);
  D3DMATERIAL9 m = {}; m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = m.Diffuse.a = 1; m.Ambient = m.Diffuse; dev->SetMaterial(&m);
  dev->SetRenderState(D3DRS_AMBIENT, 0x00404040);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  float view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1 };
  float n = 0.5f, f = 60.0f, asp = W / (float)H, fov = 1.1f, ys = 1.0f / tanf(fov / 2), xs = ys / asp;
  float proj[16] = { xs,0,0,0, 0,ys,0,0, 0,0,f/(f-n),1, 0,0,-n*f/(f-n),0 };
  D3DMATRIX mv, mp, mw; matrix(mv, view); matrix(mp, proj);
  dev->SetTransform(D3DTS_VIEW, &mv); dev->SetTransform(D3DTS_PROJECTION, &mp);
  dev->SetTexture(0, tex); dev->SetFVF(FVF_PNT); dev->SetStreamSource(0, vb, 0, sizeof(Vtx));

  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; i++) {
    float a = i * (1.0f / 60.0f) * 0.8f;
    float world[16] = { cosf(a),0,-sinf(a),0, 0,1,0,0, sinf(a),0,cosf(a),0, 0,0,0,1 };
    matrix(mw, world); dev->SetTransform(D3DTS_WORLD, &mw);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1.0f, 0);
    dev->BeginScene();
    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    dev->EndScene();
    if (i == frames - 1) {
      IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
      dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
      dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
      hr = dev->GetRenderTargetData(bb, sys);
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        printf("frame %d -> %s (%s)\n", i, out, bmp_write(out, lr.pBits, lr.Pitch, W, H) ? "written" : "write failed");
        sys->UnlockRect();
      } else printf("GetRenderTargetData 0x%08x\n", (unsigned)hr);
      if (sys) sys->Release(); if (bb) bb->Release();
    }
    hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) { printf("Present 0x%08x at frame %d\n", (unsigned)hr, i); break; }
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("%d frames, %.1f ms, %.0f fps\n", frames, ms, frames * 1000.0 / ms);
  vb->Release(); tex->Release(); dev->Release(); d3d->Release();
  if (win) { SDL_DestroyWindow(win); SDL_Quit(); }
  return 0;
}
