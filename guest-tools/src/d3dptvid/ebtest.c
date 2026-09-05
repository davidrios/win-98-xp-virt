/*
 * ebtest.c — the DirectX 3 way of drawing, through our driver's HAL: the
 * IDirect3D (v1) device on the back buffer, execute buffers (D3DOP_*
 * instructions, D3DOP_PROCESSVERTICES, D3DOP_TRIANGLE), texture handles
 * (IDirect3DTexture::Load + GetHandle, D3DRENDERSTATE_TEXTUREHANDLE) and
 * the viewport's Clear through a background material — what Moto Racer
 * (1997) does, and what a DX6+ runtime has to emulate on a DrawPrimitives2
 * driver (doc 15 "Execute buffers").
 *
 *   EBTEST [w h bpp] [-rgb]    (default 640 480 16: the mode a 1997 title asks for;
 *                               -rgb: the runtime's RGB software device instead of the HAL, the control)
 *
 * Cases, each read back from the back buffer before the flip:
 *   1. viewport Clear to the background material's colour;
 *   2. two flat-shaded triangles (TL vertices, PROCESSVERTICES_COPY);
 *   3. the same quad textured (R5G6B5 texture loaded the DX3 way,
 *      TEXTUREHANDLE + TEXTUREMAPBLEND);
 *   4. a colour-keyed texture with COLORKEYENABLE: the keyed texels show
 *      the clear colour;
 *   5. untransformed vertices (D3DLVERTEX) through PROCESSVERTICES_TRANSFORM
 *      with identity world / view and an orthographic projection matrix,
 *      executed D3DEXECUTE_CLIPPED.
 * Every HRESULT goes to ebtest.log, which ends with "ebtest: N cases, M failed".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define D3D_OVERLOADS 0
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX 64
#define CLEAR_R 0x00
#define CLEAR_G 0x40
#define CLEAR_B 0x00
#define CLEAR_RGB 0x004000

static FILE *logfile;
static unsigned cases, failed;

static void logp(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logfile) {
        fputs(buf, logfile);
        fflush(logfile);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

static D3DTLVERTEX V(float x, float y, float z, DWORD c, float u, float v)
{
    D3DTLVERTEX r;
    memset(&r, 0, sizeof r);
    r.sx = x; r.sy = y; r.sz = z; r.rhw = 1.0f; r.color = c; r.specular = 0xff000000; r.tu = u; r.tv = v;
    return r;
}

static D3DLVERTEX LV(float x, float y, float z, DWORD c, float u, float v)
{
    D3DLVERTEX r;
    memset(&r, 0, sizeof r);
    r.x = x; r.y = y; r.z = z; r.color = c; r.specular = 0xff000000; r.tu = u; r.tv = v;
    return r;
}

/* the DX3 device enumeration: the HAL is the device with a hardware description */
static int enum_hal, use_rgb;
static const GUID *dev_guid = &IID_IDirect3DHALDevice;
static D3DDEVICEDESC hal_desc;
static HRESULT CALLBACK enum_dev(GUID *guid, char *desc, char *name, LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel, void *ctx)
{
    logp("  device \"%s\" (%s): hw colour model %lu devcaps %08lx / hel colour model %lu devcaps %08lx\n", name ? name : "?", desc ? desc : "?",
         hw ? hw->dcmColorModel : 0, hw ? hw->dwDevCaps : 0, hel ? hel->dcmColorModel : 0, hel ? hel->dwDevCaps : 0);
    if (guid && IsEqualGUID(guid, dev_guid)) {
        enum_hal = 1;
        hal_desc = use_rgb ? *hel : *hw;
    }
    return D3DENUMRET_OK;
}

/* the back buffer at (x, y) as 8-bit RGB */
static DWORD readback(LPDIRECTDRAWSURFACE back, int x, int y)
{
    DDSURFACEDESC sd;
    DWORD px = 0xdeadbeef;
    HRESULT hr;

    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) { logp("  Lock(back) %08lx\n", hr); return px; }
    if (sd.ddpfPixelFormat.dwRGBBitCount == 16) {
        WORD v = *(WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch + x * 2);
        px = ((DWORD)(v >> 11) << 19) | ((DWORD)((v >> 5) & 0x3f) << 10) | ((DWORD)(v & 0x1f) << 3);
    } else {
        px = *(DWORD *)((BYTE *)sd.lpSurface + y * sd.lPitch + x * 4) & 0xffffff;
    }
    back->lpVtbl->Unlock(back, NULL);
    return px;
}

static int near_(DWORD a, DWORD b, int tol)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

/* the whole back buffer as ebN.bmp (24-bit), for the eye when a case fails */
static void dump_bmp(LPDIRECTDRAWSURFACE back, const char *path)
{
    DDSURFACEDESC sd;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    unsigned w, h, bpp, y, x, row_bytes;
    unsigned char *row;

    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    if (FAILED(back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return;
    w = sd.dwWidth; h = sd.dwHeight; bpp = sd.ddpfPixelFormat.dwRGBBitCount; row_bytes = (w * 3 + 3) & ~3u;
    f = fopen(path, "wb");
    row = malloc(row_bytes);
    if (f && row) {
        memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih); memset(row, 0, row_bytes);
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + row_bytes * h;
        ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
        for (y = h; y-- > 0;) {
            const unsigned char *src = (const unsigned char *)sd.lpSurface + y * sd.lPitch;
            for (x = 0; x < w; x++) {
                if (bpp == 32) {
                    row[x * 3 + 0] = src[x * 4 + 0]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2];
                } else {
                    unsigned v = ((const unsigned short *)src)[x];
                    row[x * 3 + 0] = (v & 0x1f) << 3; row[x * 3 + 1] = ((v >> 5) & 0x3f) << 2; row[x * 3 + 2] = ((v >> 11) & 0x1f) << 3;
                }
            }
            fwrite(row, 1, row_bytes, f);
        }
    }
    if (f) fclose(f);
    free(row);
    back->lpVtbl->Unlock(back, NULL);
}

static void check(LPDIRECTDRAWSURFACE back, const char *name, int x1, int y1, DWORD want1, int x2, int y2, DWORD want2)
{
    DWORD p1 = readback(back, x1, y1), p2 = readback(back, x2, y2), p3 = readback(back, 600, 440);
    char path[32];
    int ok = near_(p1, want1, 20) && near_(p2, want2, 20);     /* 16-bit rounding differs between the RGB device and the HAL */
    cases++;
    if (!ok) failed++;
    logp("%-58s (%d,%d) %06lx want %06lx  (%d,%d) %06lx want %06lx  (600,440) %06lx  %s\n", name,
         x1, y1, p1, want1, x2, y2, p2, want2, p3, ok ? "PASS" : "FAIL");
    sprintf(path, "eb%u.bmp", cases);
    dump_bmp(back, path);
}

/* --- the execute buffer: vertices first, then the instruction stream --- */
static BYTE *eb_mem, *eb_ip;
static DWORD eb_size, eb_vertex_bytes;

static void op(BYTE opcode, BYTE size, WORD count)
{
    D3DINSTRUCTION *i = (D3DINSTRUCTION *)eb_ip;
    i->bOpcode = opcode; i->bSize = size; i->wCount = count;
    eb_ip += sizeof *i;
}
static void rstate(D3DRENDERSTATETYPE t, DWORD v)
{
    D3DSTATE *s = (D3DSTATE *)eb_ip;
    s->drstRenderStateType = t; s->dwArg[0] = v;
    eb_ip += sizeof *s;
}
static void tstate(D3DTRANSFORMSTATETYPE t, DWORD v)
{
    D3DSTATE *s = (D3DSTATE *)eb_ip;
    s->dtstTransformStateType = t; s->dwArg[0] = v;
    eb_ip += sizeof *s;
}
static void tri(WORD a, WORD b, WORD c)
{
    D3DTRIANGLE *t = (D3DTRIANGLE *)eb_ip;
    t->v1 = a; t->v2 = b; t->v3 = c; t->wFlags = D3DTRIFLAG_START;
    eb_ip += sizeof *t;
}
static void process(DWORD flags, WORD start, WORD dest, DWORD count)
{
    D3DPROCESSVERTICES *pv = (D3DPROCESSVERTICES *)eb_ip;
    pv->dwFlags = flags; pv->wStart = start; pv->wDest = dest; pv->dwCount = count; pv->dwReserved = 0;
    eb_ip += sizeof *pv;
}

/* lock the buffer, copy the vertices in, start the instructions right after them */
static HRESULT eb_begin(LPDIRECT3DEXECUTEBUFFER eb, const void *verts, DWORD vertex_bytes)
{
    D3DEXECUTEBUFFERDESC d;
    HRESULT hr;
    memset(&d, 0, sizeof d); d.dwSize = sizeof d;
    hr = eb->lpVtbl->Lock(eb, &d);
    if (FAILED(hr)) { logp("  ExecuteBuffer::Lock %08lx\n", hr); return hr; }
    eb_mem = d.lpData; eb_size = d.dwBufferSize;
    memcpy(eb_mem, verts, vertex_bytes);
    eb_vertex_bytes = (vertex_bytes + 15) & ~15u;
    eb_ip = eb_mem + eb_vertex_bytes;
    return hr;
}

static HRESULT eb_end(LPDIRECT3DEXECUTEBUFFER eb, DWORD nverts)
{
    D3DEXECUTEDATA ed;
    HRESULT hr;
    op(D3DOP_EXIT, 0, 0);
    hr = eb->lpVtbl->Unlock(eb);
    if (FAILED(hr)) logp("  ExecuteBuffer::Unlock %08lx\n", hr);
    memset(&ed, 0, sizeof ed); ed.dwSize = sizeof ed;
    ed.dwVertexOffset = 0; ed.dwVertexCount = nverts;
    ed.dwInstructionOffset = eb_vertex_bytes; ed.dwInstructionLength = (DWORD)(eb_ip - eb_mem) - eb_vertex_bytes;
    ed.dwHVertexOffset = 0;
    hr = eb->lpVtbl->SetExecuteData(eb, &ed);
    if (FAILED(hr)) logp("  SetExecuteData %08lx\n", hr);
    return hr;
}

static HRESULT run(LPDIRECT3DDEVICE dev, LPDIRECT3DEXECUTEBUFFER eb, LPDIRECT3DVIEWPORT vp, DWORD flags, const char *what)
{
    HRESULT hb, he, hs;
    D3DEXECUTEDATA ed;
    hb = dev->lpVtbl->BeginScene(dev);
    he = dev->lpVtbl->Execute(dev, eb, vp, flags);
    hs = dev->lpVtbl->EndScene(dev);
    memset(&ed, 0, sizeof ed); ed.dwSize = sizeof ed;
    eb->lpVtbl->GetExecuteData(eb, &ed);
    logp("  %s: BeginScene %08lx Execute(%s) %08lx EndScene %08lx status %04x\n", what, hb,
         (flags & D3DEXECUTE_UNCLIPPED) ? "UNCLIPPED" : "CLIPPED", he, hs, ed.dsStatus.dwStatus);
    return he;
}

/* a DX3 texture: a system-memory surface with the texels, loaded into a
 * video-memory one (ALLOCONLOAD), its handle from IDirect3DTexture */
static LPDIRECTDRAWSURFACE make_texture(LPDIRECTDRAW dd, LPDIRECT3DDEVICE dev, int keyed, D3DTEXTUREHANDLE *handle)
{
    DDSURFACEDESC sd;
    LPDIRECTDRAWSURFACE sys = NULL, vid = NULL;
    LPDIRECT3DTEXTURE tsys = NULL, tvid = NULL;
    DDCOLORKEY ck;
    HRESULT hr;
    int x, y;

    *handle = 0;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = TEX; sd.dwHeight = TEX;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
    sd.ddpfPixelFormat.dwSize = sizeof sd.ddpfPixelFormat;
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 16;
    sd.ddpfPixelFormat.dwRBitMask = 0xf800; sd.ddpfPixelFormat.dwGBitMask = 0x07e0; sd.ddpfPixelFormat.dwBBitMask = 0x001f;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &sys, NULL);
    logp("  CreateSurface(system texture %dx%d R5G6B5) %08lx\n", TEX, TEX, hr);
    if (FAILED(hr)) return NULL;
    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    hr = sys->lpVtbl->Lock(sys, NULL, &sd, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) { logp("  Lock(system texture) %08lx\n", hr); return NULL; }
    for (y = 0; y < TEX; y++) {
        WORD *row = (WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch);
        for (x = 0; x < TEX; x++) {
            /* left half red, right half blue; keyed: an 8x8 checker of magenta over it */
            WORD v = x < TEX / 2 ? 0xf800 : 0x001f;
            if (keyed && (((x / 8) + (y / 8)) & 1)) v = 0xf81f;
            row[x] = v;
        }
    }
    sys->lpVtbl->Unlock(sys, NULL);
    if (keyed) {
        ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = 0xf81f;
        hr = sys->lpVtbl->SetColorKey(sys, DDCKEY_SRCBLT, &ck);
        logp("  SetColorKey(system texture, magenta) %08lx\n", hr);
    }

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = TEX; sd.dwHeight = TEX;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | (use_rgb ? DDSCAPS_SYSTEMMEMORY : DDSCAPS_VIDEOMEMORY | DDSCAPS_ALLOCONLOAD);
    sd.ddpfPixelFormat.dwSize = sizeof sd.ddpfPixelFormat;
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 16;
    sd.ddpfPixelFormat.dwRBitMask = 0xf800; sd.ddpfPixelFormat.dwGBitMask = 0x07e0; sd.ddpfPixelFormat.dwBBitMask = 0x001f;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &vid, NULL);
    logp("  CreateSurface(video texture, ALLOCONLOAD) %08lx\n", hr);
    if (FAILED(hr)) return NULL;
    hr = sys->lpVtbl->QueryInterface(sys, &IID_IDirect3DTexture, (void **)&tsys);
    if (FAILED(hr)) { logp("  QueryInterface(IDirect3DTexture, system) %08lx\n", hr); return NULL; }
    hr = vid->lpVtbl->QueryInterface(vid, &IID_IDirect3DTexture, (void **)&tvid);
    if (FAILED(hr)) { logp("  QueryInterface(IDirect3DTexture, video) %08lx\n", hr); return NULL; }
    hr = tvid->lpVtbl->Load(tvid, tsys);
    logp("  IDirect3DTexture::Load(video <- system) %08lx\n", hr);
    if (keyed) {
        ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = 0xf81f;
        hr = vid->lpVtbl->SetColorKey(vid, DDCKEY_SRCBLT, &ck);
        logp("  SetColorKey(video texture, magenta) %08lx\n", hr);
    }
    hr = tvid->lpVtbl->GetHandle(tvid, dev, handle);
    logp("  IDirect3DTexture::GetHandle %08lx -> %08lx\n", hr, *handle);
    return vid;
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 16;
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE prim = NULL, back = NULL, tex = NULL, ktex = NULL;
    LPDIRECT3D d3d = NULL;
    LPDIRECT3DDEVICE dev = NULL;
    LPDIRECT3DVIEWPORT vp = NULL;
    LPDIRECT3DMATERIAL mat = NULL;
    LPDIRECT3DEXECUTEBUFFER eb = NULL;
    D3DMATERIALHANDLE hmat = 0;
    D3DTEXTUREHANDLE htex = 0, hktex = 0;
    D3DMATRIXHANDLE hworld = 0, hview = 0, hproj = 0;
    DDSURFACEDESC sd;
    DDSCAPS caps;
    D3DVIEWPORT vpd;
    D3DMATERIAL md;
    D3DEXECUTEBUFFERDESC ebd;
    D3DMATRIX m;
    D3DRECT rc;
    D3DTLVERTEX q[6];
    D3DLVERTEX lq[6];
    HRESULT hr;
    WNDCLASSA wc;
    HWND hwnd;

    if (argc > 3 && argv[1][0] != '-') { w = atoi(argv[1]); h = atoi(argv[2]); bpp = atoi(argv[3]); }
    if (!strcmp(argv[argc - 1], "-rgb")) { use_rgb = 1; dev_guid = &IID_IDirect3DRGBDevice; }
    logfile = fopen("ebtest.log", "w");
    logp("ebtest: %dx%d %d bpp (DirectX 3 interfaces: IDirect3D, execute buffers, texture handles) on the %s device\n", w, h, bpp,
         use_rgb ? "RGB software" : "HAL");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ebtest";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "ebtest", "ebtest", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    ShowCursor(FALSE);
    pump();

    hr = DirectDrawCreate(NULL, &dd, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreate failed %08lx\n", hr); return 1; }
    hr = dd->lpVtbl->QueryInterface(dd, &IID_IDirect3D, (void **)&d3d);
    logp("QueryInterface(IDirect3D) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    hr = d3d->lpVtbl->EnumDevices(d3d, enum_dev, NULL);
    logp("EnumDevices %08lx: %s device %s; devcaps %08lx max vertices %lu max buffer %lu texture caps %08lx\n", hr,
         use_rgb ? "RGB" : "HAL", enum_hal ? "present" : "ABSENT", hal_desc.dwDevCaps, hal_desc.dwMaxVertexCount, hal_desc.dwMaxBufferSize,
         hal_desc.dpcTriCaps.dwTextureCaps);
    if (!enum_hal) goto out;

    hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    logp("SetCooperativeLevel %08lx\n", hr);
    hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp);
    logp("SetDisplayMode %dx%dx%d %08lx\n", w, h, bpp, hr);
    if (FAILED(hr)) goto out;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    logp("CreateSurface(primary + back, 3DDEVICE) %08lx\n", hr);
    if (FAILED(hr)) goto out;
    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
    if (FAILED(hr)) { logp("GetAttachedSurface(back) %08lx\n", hr); goto out; }

    /* the DX3 device: QueryInterface on the render target */
    hr = back->lpVtbl->QueryInterface(back, dev_guid, (void **)&dev);
    logp("back->QueryInterface(%s) %08lx\n", use_rgb ? "IID_IDirect3DRGBDevice" : "IID_IDirect3DHALDevice", hr);
    if (FAILED(hr)) goto out;

    hr = d3d->lpVtbl->CreateViewport(d3d, &vp, NULL);
    logp("CreateViewport %08lx\n", hr);
    if (FAILED(hr)) goto out;
    hr = dev->lpVtbl->AddViewport(dev, vp);
    logp("AddViewport %08lx\n", hr);
    memset(&vpd, 0, sizeof vpd);
    vpd.dwSize = sizeof vpd;
    vpd.dwX = 0; vpd.dwY = 0; vpd.dwWidth = w; vpd.dwHeight = h;
    vpd.dvScaleX = w / 2.0f; vpd.dvScaleY = h / 2.0f;
    vpd.dvMaxX = 1.0f; vpd.dvMaxY = 1.0f;
    vpd.dvMinZ = 0.0f; vpd.dvMaxZ = 1.0f;
    hr = vp->lpVtbl->SetViewport(vp, &vpd);
    logp("SetViewport %dx%d %08lx\n", w, h, hr);

    /* the background material: what Clear paints */
    hr = d3d->lpVtbl->CreateMaterial(d3d, &mat, NULL);
    logp("CreateMaterial %08lx\n", hr);
    if (SUCCEEDED(hr)) {
        memset(&md, 0, sizeof md);
        md.dwSize = sizeof md;
        md.diffuse.r = CLEAR_R / 255.0f; md.diffuse.g = CLEAR_G / 255.0f; md.diffuse.b = CLEAR_B / 255.0f; md.diffuse.a = 1.0f;
        md.dwRampSize = 1;
        hr = mat->lpVtbl->SetMaterial(mat, &md);
        logp("SetMaterial %08lx\n", hr);
        hr = mat->lpVtbl->GetHandle(mat, dev, &hmat);
        logp("Material::GetHandle %08lx -> %08lx\n", hr, hmat);
        hr = vp->lpVtbl->SetBackground(vp, hmat);
        logp("SetBackground %08lx\n", hr);
    }

    /* case 1: the viewport's Clear */
    rc.x1 = 0; rc.y1 = 0; rc.x2 = w; rc.y2 = h;
    hr = vp->lpVtbl->Clear(vp, 1, &rc, D3DCLEAR_TARGET);
    logp("Viewport::Clear(TARGET) %08lx\n", hr);
    check(back, "1. Clear to the background material", 100, 100, CLEAR_RGB, 500, 400, CLEAR_RGB);

    /* the execute buffer: 64 KiB, plenty */
    memset(&ebd, 0, sizeof ebd);
    ebd.dwSize = sizeof ebd;
    ebd.dwFlags = D3DDEB_BUFSIZE;
    ebd.dwBufferSize = 65536;
    hr = dev->lpVtbl->CreateExecuteBuffer(dev, &ebd, &eb, NULL);
    logp("CreateExecuteBuffer(64 KiB) %08lx\n", hr);
    if (FAILED(hr)) goto out;

    /* case 2: two flat triangles from TL vertices (the quad 100..420 x 80..320): the upper-left one red, the lower-right one blue */
    q[0] = V(100, 80, 0.5f, 0xffff0000, 0, 0); q[1] = V(420, 80, 0.5f, 0xffff0000, 1, 0); q[2] = V(100, 320, 0.5f, 0xffff0000, 0, 1);
    q[3] = V(100, 320, 0.5f, 0xff0000ff, 0, 1); q[4] = V(420, 80, 0.5f, 0xff0000ff, 1, 0); q[5] = V(420, 320, 0.5f, 0xff0000ff, 1, 1);
    if (SUCCEEDED(eb_begin(eb, q, sizeof q))) {
        op(D3DOP_STATERENDER, sizeof(D3DSTATE), 6);
        rstate(D3DRENDERSTATE_TEXTUREHANDLE, 0);
        rstate(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
        rstate(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        rstate(D3DRENDERSTATE_ZENABLE, FALSE);
        rstate(D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID);
        rstate(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE);
        op(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
        process(D3DPROCESSVERTICES_COPY, 0, 0, 6);
        op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
        tri(0, 1, 2); tri(3, 4, 5);
        eb_end(eb, 6);
        vp->lpVtbl->Clear(vp, 1, &rc, D3DCLEAR_TARGET);
        run(dev, eb, vp, D3DEXECUTE_UNCLIPPED, "flat quad");
        check(back, "2. two flat triangles (TL vertices, PROCESSVERTICES_COPY)", 140, 200, 0xff0000, 400, 300, 0x0000ff);
    }

    /* case 3: the quad textured through a DX3 texture handle */
    tex = make_texture(dd, dev, 0, &htex);
    if (tex && htex) {
        q[0] = V(100, 80, 0.5f, 0xffffffff, 0, 0); q[1] = V(420, 80, 0.5f, 0xffffffff, 1, 0); q[2] = V(100, 320, 0.5f, 0xffffffff, 0, 1);
        q[3] = V(100, 320, 0.5f, 0xffffffff, 0, 1); q[4] = V(420, 80, 0.5f, 0xffffffff, 1, 0); q[5] = V(420, 320, 0.5f, 0xffffffff, 1, 1);
        if (SUCCEEDED(eb_begin(eb, q, sizeof q))) {
            op(D3DOP_STATERENDER, sizeof(D3DSTATE), 6);
            rstate(D3DRENDERSTATE_TEXTUREHANDLE, htex);
            rstate(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE);
            rstate(D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_NEAREST);
            rstate(D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_NEAREST);
            rstate(D3DRENDERSTATE_TEXTUREPERSPECTIVE, TRUE);
            rstate(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            op(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            process(D3DPROCESSVERTICES_COPY, 0, 0, 6);
            op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
            tri(0, 1, 2); tri(3, 4, 5);
            eb_end(eb, 6);
            vp->lpVtbl->Clear(vp, 1, &rc, D3DCLEAR_TARGET);
            run(dev, eb, vp, D3DEXECUTE_UNCLIPPED, "textured quad");
            check(back, "3. the quad textured (TEXTUREHANDLE, R5G6B5 red | blue)", 140, 200, 0xff0000, 400, 300, 0x0000ff);
        }
    } else {
        cases++; failed++;
        logp("3. the quad textured: no texture handle                    FAIL\n");
    }

    /* case 4: a colour-keyed texture: texel (tx, ty) at pixel (100 + tx * 5, 80 + ty * 3.75); the 8x8 cell (0,0) is red, cell (1,0) is keyed */
    ktex = make_texture(dd, dev, 1, &hktex);
    if (ktex && hktex) {
        if (SUCCEEDED(eb_begin(eb, q, sizeof q))) {
            op(D3DOP_STATERENDER, sizeof(D3DSTATE), 3);
            rstate(D3DRENDERSTATE_TEXTUREHANDLE, hktex);
            rstate(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE);
            rstate(D3DRENDERSTATE_COLORKEYENABLE, TRUE);
            op(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            process(D3DPROCESSVERTICES_COPY, 0, 0, 6);
            op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
            tri(0, 1, 2); tri(3, 4, 5);
            eb_end(eb, 6);
            vp->lpVtbl->Clear(vp, 1, &rc, D3DCLEAR_TARGET);
            run(dev, eb, vp, D3DEXECUTE_UNCLIPPED, "keyed quad");
            check(back, "4. a colour-keyed texture with COLORKEYENABLE", 120, 95, 0xff0000, 160, 95, CLEAR_RGB);
        }
    } else {
        cases++; failed++;
        logp("4. the keyed quad: no texture handle                       FAIL\n");
    }

    /* case 5: untransformed vertices through the runtime's transform (identity world / view,
     * an orthographic projection mapping x, y in -1..1 to the viewport), executed CLIPPED */
    hr = dev->lpVtbl->CreateMatrix(dev, &hworld);
    logp("CreateMatrix(world) %08lx -> %08lx\n", hr, hworld);
    hr = dev->lpVtbl->CreateMatrix(dev, &hview);
    logp("CreateMatrix(view) %08lx -> %08lx\n", hr, hview);
    hr = dev->lpVtbl->CreateMatrix(dev, &hproj);
    logp("CreateMatrix(projection) %08lx -> %08lx\n", hr, hproj);
    if (hworld && hview && hproj) {
        memset(&m, 0, sizeof m);
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        dev->lpVtbl->SetMatrix(dev, hworld, &m);
        dev->lpVtbl->SetMatrix(dev, hview, &m);
        /* orthographic: x' = x, y' = y, z' = z * 0.5 + 0.5 (w = 1) */
        m._33 = 0.5f; m._43 = 0.5f;
        hr = dev->lpVtbl->SetMatrix(dev, hproj, &m);
        logp("SetMatrix x3 %08lx\n", hr);
        /* the quad 100..420 x 80..320 in clip space: x = px / 320 - 1, y = 1 - py / 240; the same two colours as case 2 */
        lq[0] = LV(-0.6875f, 0.6667f, 0.5f, 0xffff0000, 0, 0); lq[1] = LV(0.3125f, 0.6667f, 0.5f, 0xffff0000, 1, 0); lq[2] = LV(-0.6875f, -0.3333f, 0.5f, 0xffff0000, 0, 1);
        lq[3] = LV(-0.6875f, -0.3333f, 0.5f, 0xff0000ff, 0, 1); lq[4] = LV(0.3125f, 0.6667f, 0.5f, 0xff0000ff, 1, 0); lq[5] = LV(0.3125f, -0.3333f, 0.5f, 0xff0000ff, 1, 1);
        if (SUCCEEDED(eb_begin(eb, lq, sizeof lq))) {
            op(D3DOP_STATETRANSFORM, sizeof(D3DSTATE), 3);
            tstate(D3DTRANSFORMSTATE_WORLD, hworld);
            tstate(D3DTRANSFORMSTATE_VIEW, hview);
            tstate(D3DTRANSFORMSTATE_PROJECTION, hproj);
            op(D3DOP_STATERENDER, sizeof(D3DSTATE), 2);
            rstate(D3DRENDERSTATE_TEXTUREHANDLE, 0);
            rstate(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            op(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            process(D3DPROCESSVERTICES_TRANSFORM, 0, 0, 6);
            op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
            tri(0, 1, 2); tri(3, 4, 5);
            eb_end(eb, 6);
            vp->lpVtbl->Clear(vp, 1, &rc, D3DCLEAR_TARGET);
            run(dev, eb, vp, D3DEXECUTE_CLIPPED, "transformed quad");
            check(back, "5. LVERTEX through PROCESSVERTICES_TRANSFORM, CLIPPED", 140, 200, 0xff0000, 400, 300, 0x0000ff);
        }
    } else {
        cases++; failed++;
        logp("5. transformed quad: no matrix handles                     FAIL\n");
    }

    prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
    Sleep(500);

out:
    logp("ebtest: %u cases, %u failed\n", cases, failed);
    if (eb) eb->lpVtbl->Release(eb);
    if (ktex) ktex->lpVtbl->Release(ktex);
    if (tex) tex->lpVtbl->Release(tex);
    if (mat) mat->lpVtbl->Release(mat);
    if (vp) vp->lpVtbl->Release(vp);
    if (dev) dev->lpVtbl->Release(dev);
    if (d3d) d3d->lpVtbl->Release(d3d);
    if (back) back->lpVtbl->Release(back);
    if (prim) prim->lpVtbl->Release(prim);
    if (dd) { dd->lpVtbl->RestoreDisplayMode(dd); dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL); dd->lpVtbl->Release(dd); }
    if (logfile) fclose(logfile);
    return failed ? 1 : 0;
}
