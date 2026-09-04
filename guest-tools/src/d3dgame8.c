/*
 * d3dgame8: the Direct3D 8 reference workload (doc 14 P0a), same scene as
 * d3dgame9 (d3dgame.h) on the DX8 API: FVF via SetVertexShader, indices via
 * SetIndices(base), CopyRects for the dump, texture-stage sampler states.
 * Fixed-function only (D3DX8 is a static library; no runtime compiler).
 *
 * Build: i686-w64-mingw32-gcc -O2 -o d3dgame8.exe d3dgame8.c -ld3d8 -lgdi32 -luser32
 */
#define COBJMACROS
#include "d3dgame.h"
#include <d3d8.h>

#define FVF_PNT (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define FVF_PCT (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_RHW (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

struct gfx {
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DVertexBuffer8 *vb_cube, *vb_grid, *vb_ground;
    IDirect3DIndexBuffer8 *ib_cube, *ib_grid;
    IDirect3DTexture8 *tex_checker, *tex_grad, *tex_disc, *tex_rtt;
    IDirect3DSurface8 *rtt_depth;
};

static struct game G;
static struct gfx X;

static const char *hr_str(HRESULT hr) { static char b[32]; sprintf(b, "0x%08lx", (unsigned long)hr); return b; }
#define CHK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { game_log("d3dgame8: %s failed %s", #call, hr_str(hr_)); return 0; } } while (0)

static int make_textures(void)
{
    D3DLOCKED_RECT lr;
    int level, w = 128;
    CHK(IDirect3DDevice8_CreateTexture(X.dev, 128, 128, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &X.tex_checker));
    for (level = 0; w >= 1; level++, w >>= 1) {
        if (FAILED(IDirect3DTexture8_LockRect(X.tex_checker, level, &lr, NULL, 0))) break;
        tex_checker_8888(lr.pBits, lr.Pitch, w, w, level);
        IDirect3DTexture8_UnlockRect(X.tex_checker, level);
    }
    CHK(IDirect3DDevice8_CreateTexture(X.dev, 64, 64, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &X.tex_grad));
    CHK(IDirect3DTexture8_LockRect(X.tex_grad, 0, &lr, NULL, 0));
    tex_gradient_565(lr.pBits, lr.Pitch, 64, 64);
    IDirect3DTexture8_UnlockRect(X.tex_grad, 0);
    if (SUCCEEDED(IDirect3DDevice8_CreateTexture(X.dev, 64, 64, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &X.tex_disc))) {
        CHK(IDirect3DTexture8_LockRect(X.tex_disc, 0, &lr, NULL, 0));
        tex_disc_dxt1(lr.pBits, lr.Pitch, 64, 64);
        IDirect3DTexture8_UnlockRect(X.tex_disc, 0);
    } else {
        game_log("d3dgame8: no DXT1 support, particles use the gradient texture");
        X.tex_disc = X.tex_grad;
        IDirect3DTexture8_AddRef(X.tex_grad);
    }
    CHK(IDirect3DDevice8_CreateTexture(X.dev, RTT_SIZE, RTT_SIZE, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &X.tex_rtt));
    CHK(IDirect3DDevice8_CreateDepthStencilSurface(X.dev, RTT_SIZE, RTT_SIZE, D3DFMT_D16, D3DMULTISAMPLE_NONE, &X.rtt_depth));
    return 1;
}

static int make_geometry(void)
{
    BYTE *p;
    struct vtx_pnt cube[24];
    WORD cidx[36];
    /* strip order a, c, b, d: both triangles wind like the grid's, top face front */
    struct vtx_pnt ground[4] = {
        { -12, -1, -9, 0, 1, 0, 0, 0 }, { -12, -1, 15, 0, 1, 0, 0, 8 },
        { 12, -1, -9, 0, 1, 0, 8, 0 }, { 12, -1, 15, 0, 1, 0, 8, 8 } };
    geo_cube(cube, cidx);
    CHK(IDirect3DDevice8_CreateVertexBuffer(X.dev, sizeof(cube), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, &X.vb_cube));
    CHK(IDirect3DVertexBuffer8_Lock(X.vb_cube, 0, 0, &p, 0)); memcpy(p, cube, sizeof(cube)); IDirect3DVertexBuffer8_Unlock(X.vb_cube);
    CHK(IDirect3DDevice8_CreateIndexBuffer(X.dev, sizeof(cidx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &X.ib_cube));
    CHK(IDirect3DIndexBuffer8_Lock(X.ib_cube, 0, 0, &p, 0)); memcpy(p, cidx, sizeof(cidx)); IDirect3DIndexBuffer8_Unlock(X.ib_cube);
    CHK(IDirect3DDevice8_CreateVertexBuffer(X.dev, sizeof(ground), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, &X.vb_ground));
    CHK(IDirect3DVertexBuffer8_Lock(X.vb_ground, 0, 0, &p, 0)); memcpy(p, ground, sizeof(ground)); IDirect3DVertexBuffer8_Unlock(X.vb_ground);
    CHK(IDirect3DDevice8_CreateVertexBuffer(X.dev, GRID_VERTS * sizeof(struct vtx_pct), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                            FVF_PCT, D3DPOOL_DEFAULT, &X.vb_grid));
    CHK(IDirect3DDevice8_CreateIndexBuffer(X.dev, GRID_INDICES * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &X.ib_grid));
    CHK(IDirect3DIndexBuffer8_Lock(X.ib_grid, 0, 0, &p, 0)); geo_grid_indices((WORD *)p); IDirect3DIndexBuffer8_Unlock(X.ib_grid);
    return 1;
}

static void set_states(void)
{
    D3DLIGHT8 l;
    D3DMATERIAL8 m;
    memset(&l, 0, sizeof(l));
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = 1.0f;
    l.Direction.x = -0.5f; l.Direction.y = -1.0f; l.Direction.z = 0.4f;
    IDirect3DDevice8_SetLight(X.dev, 0, &l);
    IDirect3DDevice8_LightEnable(X.dev, 0, TRUE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_AMBIENT, 0x00404040);
    memset(&m, 0, sizeof(m));
    m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = m.Diffuse.a = 1.0f;
    m.Ambient = m.Diffuse;
    IDirect3DDevice8_SetMaterial(X.dev, &m);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(X.dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(X.dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}

static void set_matrices(const struct mat4 *world, const struct mat4 *view, const struct mat4 *proj)
{
    IDirect3DDevice8_SetTransform(X.dev, D3DTS_WORLD, (const D3DMATRIX *)world);
    IDirect3DDevice8_SetTransform(X.dev, D3DTS_VIEW, (const D3DMATRIX *)view);
    IDirect3DDevice8_SetTransform(X.dev, D3DTS_PROJECTION, (const D3DMATRIX *)proj);
}

static void draw_cubes(const struct mat4 *view, const struct mat4 *proj)
{
    int i;
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice8_SetTexture(X.dev, 0, (IDirect3DBaseTexture8 *)X.tex_checker);
    IDirect3DDevice8_SetVertexShader(X.dev, FVF_PNT);
    IDirect3DDevice8_SetStreamSource(X.dev, 0, X.vb_cube, sizeof(struct vtx_pnt));
    IDirect3DDevice8_SetIndices(X.dev, X.ib_cube, 0);
    for (i = 0; i < NUM_CUBES; i++) {
        struct mat4 w;
        D3DMATERIAL8 m;
        game_cube_world(&G, i, &w);
        set_matrices(&w, view, proj);
        memset(&m, 0, sizeof(m));
        m.Diffuse.r = 0.5f + 0.5f * (i & 1); m.Diffuse.g = 0.5f + 0.25f * (i & 2); m.Diffuse.b = 1.0f - 0.2f * i; m.Diffuse.a = 1.0f;
        m.Ambient = m.Diffuse;
        IDirect3DDevice8_SetMaterial(X.dev, &m);
        IDirect3DDevice8_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, 24, 0, 12);
    }
}

static int render(void)
{
    struct mat4 view, proj, world, rview;
    float eye[3], at[3];
    static const float reye[3] = { 0.0f, 4.0f, -6.0f }, rat[3] = { 0.0f, 0.5f, 3.0f };
    BYTE *p;
    IDirect3DSurface8 *bb, *zs, *rts;
    struct vtx_pct part[NUM_PARTICLES * 6];
    struct vtx_rhw bars[BARS * 6];
    int i, n;

    game_camera(&G, eye, at);
    m_lookat(&view, eye, at);
    m_perspective(&proj, 1.1f, G.o.w / (float)G.o.h, 0.5f, 60.0f);
    m_lookat(&rview, reye, rat);

    IDirect3DDevice8_GetRenderTarget(X.dev, &bb);
    IDirect3DDevice8_GetDepthStencilSurface(X.dev, &zs);
    IDirect3DTexture8_GetSurfaceLevel(X.tex_rtt, 0, &rts);
    IDirect3DDevice8_SetRenderTarget(X.dev, rts, X.rtt_depth);
    IDirect3DDevice8_Clear(X.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff402010, 1.0f, 0);
    IDirect3DDevice8_BeginScene(X.dev);
    {
        struct mat4 rproj;
        m_perspective(&rproj, 0.9f, 1.0f, 0.5f, 60.0f);
        draw_cubes(&rview, &rproj);
    }
    IDirect3DDevice8_EndScene(X.dev);
    IDirect3DDevice8_SetRenderTarget(X.dev, bb, zs);
    IDirect3DSurface8_Release(rts);
    IDirect3DSurface8_Release(bb);
    IDirect3DSurface8_Release(zs);

    IDirect3DDevice8_Clear(X.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1.0f, 0);
    IDirect3DDevice8_BeginScene(X.dev);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_FILLMODE, G.wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, TRUE);

    m_identity(&world);
    set_matrices(&world, &view, &proj);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice8_SetTexture(X.dev, 0, (IDirect3DBaseTexture8 *)X.tex_checker);
    IDirect3DDevice8_SetVertexShader(X.dev, FVF_PNT);
    IDirect3DDevice8_SetStreamSource(X.dev, 0, X.vb_ground, sizeof(struct vtx_pnt));
    IDirect3DDevice8_DrawPrimitive(X.dev, D3DPT_TRIANGLESTRIP, 0, 2);

    draw_cubes(&view, &proj);

    if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(X.vb_grid, 0, 0, &p, D3DLOCK_DISCARD))) {
        geo_grid_fill((struct vtx_pct *)p, G.t);
        IDirect3DVertexBuffer8_Unlock(X.vb_grid);
    }
    m_identity(&world);
    set_matrices(&world, &view, &proj);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetTexture(X.dev, 0, (IDirect3DBaseTexture8 *)X.tex_grad);
    IDirect3DDevice8_SetVertexShader(X.dev, FVF_PCT);
    IDirect3DDevice8_SetStreamSource(X.dev, 0, X.vb_grid, sizeof(struct vtx_pct));
    IDirect3DDevice8_SetIndices(X.dev, X.ib_grid, 0);
    IDirect3DDevice8_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, GRID_VERTS, 0, GRID_INDICES / 3);

    {
        struct vtx_pct q[4] = {
            { -3.0f, 2.5f, 9.0f, 0xffffffff, 0, 0 }, { 3.0f, 2.5f, 9.0f, 0xffffffff, 1, 0 },
            { -3.0f, -0.5f, 9.0f, 0xffffffff, 0, 1 }, { 3.0f, -0.5f, 9.0f, 0xffffffff, 1, 1 } };
        IDirect3DDevice8_SetTexture(X.dev, 0, (IDirect3DBaseTexture8 *)X.tex_rtt);
        IDirect3DDevice8_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    }

    n = 0;
    for (i = 0; i < NUM_PARTICLES; i++) {
        float pos[3], size; DWORD c;
        float rx = view.m[0][0], ry = view.m[1][0], rz = view.m[2][0];
        float ux = view.m[0][1], uy = view.m[1][1], uz = view.m[2][1];
        game_particle(&G, i, pos, &c, &size);
#define PV(sx, sy, tu, tv) do { part[n].x = pos[0] + (rx * (sx) + ux * (sy)) * size; part[n].y = pos[1] + (ry * (sx) + uy * (sy)) * size; \
        part[n].z = pos[2] + (rz * (sx) + uz * (sy)) * size; part[n].color = c; part[n].u = tu; part[n].v = tv; n++; } while (0)
        PV(-1, 1, 0, 0); PV(1, 1, 1, 0); PV(-1, -1, 0, 1);
        PV(1, 1, 1, 0); PV(1, -1, 1, 1); PV(-1, -1, 0, 1);
#undef PV
    }
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetTexture(X.dev, 0, (IDirect3DBaseTexture8 *)X.tex_disc);
    IDirect3DDevice8_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLELIST, n / 3, part, sizeof(part[0]));
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, FALSE);

    n = game_bars(&G, bars, G.o.w, G.o.h);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetTexture(X.dev, 0, NULL);
    IDirect3DDevice8_SetVertexShader(X.dev, FVF_RHW);
    IDirect3DDevice8_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLELIST, n / 3, bars, sizeof(bars[0]));
    IDirect3DDevice8_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice8_EndScene(X.dev);
    return 1;
}

static void dump_frame(void)
{
    IDirect3DSurface8 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice8_GetBackBuffer(X.dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface8_GetDesc(bb, &d);
    if (SUCCEEDED(IDirect3DDevice8_CreateImageSurface(X.dev, d.Width, d.Height, d.Format, &sys))
        && SUCCEEDED(IDirect3DDevice8_CopyRects(X.dev, bb, NULL, 0, sys, NULL))
        && SUCCEEDED(IDirect3DSurface8_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        int ok = bmp_write(G.o.dump_file, lr.pBits, lr.Pitch, d.Width, d.Height, d.Format == D3DFMT_R5G6B5);
        IDirect3DSurface8_UnlockRect(sys);
        game_log("d3dgame8: frame %u -> %s (%s)", G.frame, G.o.dump_file, ok ? "written" : "write failed");
    } else {
        game_log("d3dgame8: dump failed (CopyRects)");
    }
    if (sys) IDirect3DSurface8_Release(sys);
    if (bb) IDirect3DSurface8_Release(bb);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    HWND hwnd;
    D3DADAPTER_IDENTIFIER8 id;
    D3DCAPS8 caps;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    game_init(&G, argc, argv);
    game_log_open(G.o.log_file[0] ? G.o.log_file : "d3dgame8.log", argc, argv);
    hwnd = game_window(&G, "d3dgame8");
    X.d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!X.d3d) { game_log("d3dgame8: Direct3DCreate8 failed"); return 1; }
    IDirect3D8_GetAdapterIdentifier(X.d3d, D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &id);
    IDirect3D8_GetDeviceCaps(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D8_GetAdapterDisplayMode(X.d3d, D3DADAPTER_DEFAULT, &mode);
    game_log("d3dgame8: adapter \"%s\" driver \"%s\" vs %lu.%lu ps %lu.%lu maxtex %lu", id.Description, id.Driver,
           (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
           (unsigned long)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
           (unsigned long)caps.MaxTextureWidth);

    memset(&X.pp, 0, sizeof(X.pp));
    X.pp.BackBufferWidth = G.o.w;
    X.pp.BackBufferHeight = G.o.h;
    X.pp.BackBufferFormat = G.o.fullscreen ? (G.o.bpp16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8) : mode.Format;
    X.pp.BackBufferCount = 1;
    /* D3D8 has no windowed presentation interval: COPY_VSYNC is the way to
     * pace a windowed swap chain at the refresh rate, like d3dgame9's
     * D3DPRESENT_INTERVAL_ONE. -novsync gives DISCARD / immediate on both. */
    X.pp.SwapEffect = (!G.o.fullscreen && !G.o.novsync) ? D3DSWAPEFFECT_COPY_VSYNC : D3DSWAPEFFECT_DISCARD;
    X.pp.hDeviceWindow = hwnd;
    X.pp.Windowed = !G.o.fullscreen;
    X.pp.EnableAutoDepthStencil = TRUE;
    X.pp.AutoDepthStencilFormat = D3DFMT_D16;
    X.pp.FullScreen_PresentationInterval = G.o.fullscreen
        ? (G.o.novsync ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE) : D3DPRESENT_INTERVAL_DEFAULT;
    hr = IDirect3D8_CreateDevice(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                                                                                 : D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &X.pp, &X.dev);
    if (FAILED(hr)) {
        game_log("d3dgame8: CreateDevice failed %s", hr_str(hr));
        return 1;
    }
    game_log("d3dgame8: device %dx%d %s %s, %s vertex processing", G.o.w, G.o.h, G.o.fullscreen ? "fullscreen" : "windowed",
           G.o.bpp16 ? "565" : "8888", (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "hardware" : "software");
    fflush(stdout);
    if (!make_textures() || !make_geometry()) return 1;
    set_states();

    while (!G.quit && game_pump()) {
        float dt = game_step(&G);
        if (!render()) break;
        if (G.o.dump_frame >= 0 && (int)G.frame == G.o.dump_frame) dump_frame();
        hr = IDirect3DDevice8_Present(X.dev, NULL, NULL, NULL, NULL);
        if (hr == D3DERR_DEVICELOST) {
            game_log("d3dgame8: device lost, resetting");
            while (IDirect3DDevice8_TestCooperativeLevel(X.dev) == D3DERR_DEVICELOST) Sleep(50);
            IDirect3DDevice8_Reset(X.dev, &X.pp);
            set_states();
        }
        if (dt < 0) {
            char title[96];
            snprintf(title, sizeof(title), "d3dgame8: %.1f fps, frame %u", G.fps, G.frame);
            SetWindowTextA(hwnd, title);
            game_log("%s", title);
            fflush(stdout);
        }
        if (G.o.frames && (int)G.frame >= G.o.frames) break;
    }
    game_log("d3dgame8: %u frames, %lu ms", G.frame, (unsigned long)(GetTickCount() - G.start_ms));
    if (X.dev) IDirect3DDevice8_Release(X.dev);
    if (X.d3d) IDirect3D8_Release(X.d3d);
    game_log("d3dgame8: exit");
    return 0;
}
