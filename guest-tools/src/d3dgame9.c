/*
 * d3dgame9: the Direct3D 9 reference workload (doc 14 P0a). Scene and
 * options in d3dgame.h. Golden on the rig (P4 + GeForce 6200) first; then
 * WineD3D-in-guest and the paravirtual device are diffed against its BMPs.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o d3dgame9.exe d3dgame9.c -ld3d9 -lgdi32 -luser32
 */
#define COBJMACROS
#include "d3dgame.h"
#include <d3d9.h>

#define FVF_PNT (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define FVF_PCT (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_RHW (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* optional SM1.1 path: D3DXCompileShader from whatever d3dx9_NN.dll exists */
typedef HRESULT (WINAPI *pfn_compile)(LPCSTR src, UINT len, const void *defines, void *include,
                                      LPCSTR fn, LPCSTR profile, DWORD flags, void **code, void **err, void **ct);
static const char vs_src[] =
    "float4x4 wvp : register(c0);\n"
    "float3 ldir : register(c4);\n"
    "struct VS_OUT { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(float4 pos : POSITION, float3 n : NORMAL, float2 uv : TEXCOORD0) {\n"
    "  VS_OUT o; o.pos = mul(pos, wvp);\n"
    "  float d = saturate(dot(normalize(n), -ldir)) * 0.8 + 0.2;\n"
    "  o.col = float4(d, d, d, 1); o.uv = uv; return o; }\n";
static const char ps_src[] =
    "sampler s0 : register(s0);\n"
    "float4 main(float4 col : COLOR0, float2 uv : TEXCOORD0) : COLOR { return tex2D(s0, uv) * col; }\n";

struct gfx {
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DVertexBuffer9 *vb_cube, *vb_grid, *vb_ground;
    IDirect3DIndexBuffer9 *ib_cube, *ib_grid;
    IDirect3DTexture9 *tex_checker, *tex_grad, *tex_disc, *tex_rtt;
    IDirect3DSurface9 *rtt_depth;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
};

static struct game G;
static struct gfx X;

static const char *hr_str(HRESULT hr) { static char b[32]; sprintf(b, "0x%08lx", (unsigned long)hr); return b; }
#define CHK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { printf("d3dgame9: %s failed %s\n", #call, hr_str(hr_)); fflush(stdout); return 0; } } while (0)

static int make_textures(void)
{
    D3DLOCKED_RECT lr;
    int level, w = 128;
    CHK(IDirect3DDevice9_CreateTexture(X.dev, 128, 128, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &X.tex_checker, NULL));
    for (level = 0; w >= 1; level++, w >>= 1) {
        if (FAILED(IDirect3DTexture9_LockRect(X.tex_checker, level, &lr, NULL, 0))) break;
        tex_checker_8888(lr.pBits, lr.Pitch, w, w, level);
        IDirect3DTexture9_UnlockRect(X.tex_checker, level);
    }
    CHK(IDirect3DDevice9_CreateTexture(X.dev, 64, 64, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &X.tex_grad, NULL));
    CHK(IDirect3DTexture9_LockRect(X.tex_grad, 0, &lr, NULL, 0));
    tex_gradient_565(lr.pBits, lr.Pitch, 64, 64);
    IDirect3DTexture9_UnlockRect(X.tex_grad, 0);
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(X.dev, 64, 64, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &X.tex_disc, NULL))) {
        CHK(IDirect3DTexture9_LockRect(X.tex_disc, 0, &lr, NULL, 0));
        tex_disc_dxt1(lr.pBits, lr.Pitch, 64, 64);
        IDirect3DTexture9_UnlockRect(X.tex_disc, 0);
    } else {
        printf("d3dgame9: no DXT1 support, particles use the gradient texture\n");
        X.tex_disc = X.tex_grad;
        IDirect3DTexture9_AddRef(X.tex_grad);
    }
    CHK(IDirect3DDevice9_CreateTexture(X.dev, RTT_SIZE, RTT_SIZE, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                       D3DPOOL_DEFAULT, &X.tex_rtt, NULL));
    CHK(IDirect3DDevice9_CreateDepthStencilSurface(X.dev, RTT_SIZE, RTT_SIZE, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0,
                                                   TRUE, &X.rtt_depth, NULL));
    return 1;
}

static int make_geometry(void)
{
    void *p;
    struct vtx_pnt cube[24];
    WORD cidx[36];
    struct vtx_pnt ground[4] = {
        { -12, -1, -9, 0, 1, 0, 0, 0 }, { 12, -1, -9, 0, 1, 0, 8, 0 },
        { -12, -1, 15, 0, 1, 0, 0, 8 }, { 12, -1, 15, 0, 1, 0, 8, 8 } };
    geo_cube(cube, cidx);
    CHK(IDirect3DDevice9_CreateVertexBuffer(X.dev, sizeof(cube), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, &X.vb_cube, NULL));
    CHK(IDirect3DVertexBuffer9_Lock(X.vb_cube, 0, 0, &p, 0)); memcpy(p, cube, sizeof(cube)); IDirect3DVertexBuffer9_Unlock(X.vb_cube);
    CHK(IDirect3DDevice9_CreateIndexBuffer(X.dev, sizeof(cidx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &X.ib_cube, NULL));
    CHK(IDirect3DIndexBuffer9_Lock(X.ib_cube, 0, 0, &p, 0)); memcpy(p, cidx, sizeof(cidx)); IDirect3DIndexBuffer9_Unlock(X.ib_cube);
    CHK(IDirect3DDevice9_CreateVertexBuffer(X.dev, sizeof(ground), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, &X.vb_ground, NULL));
    CHK(IDirect3DVertexBuffer9_Lock(X.vb_ground, 0, 0, &p, 0)); memcpy(p, ground, sizeof(ground)); IDirect3DVertexBuffer9_Unlock(X.vb_ground);
    CHK(IDirect3DDevice9_CreateVertexBuffer(X.dev, GRID_VERTS * sizeof(struct vtx_pct), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                            FVF_PCT, D3DPOOL_DEFAULT, &X.vb_grid, NULL));
    CHK(IDirect3DDevice9_CreateIndexBuffer(X.dev, GRID_INDICES * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &X.ib_grid, NULL));
    CHK(IDirect3DIndexBuffer9_Lock(X.ib_grid, 0, 0, &p, 0)); geo_grid_indices((WORD *)p); IDirect3DIndexBuffer9_Unlock(X.ib_grid);
    return 1;
}

static void make_shaders(void)
{
    static const char *names[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_36.dll", "d3dx9_30.dll", "d3dx9_24.dll", NULL };
    HMODULE h = NULL;
    pfn_compile compile;
    int i;
    struct blob { IUnknownVtbl *vt; } *code = NULL;
    /* ID3DXBuffer: vtable slot 3 = GetBufferPointer */
    typedef void *(WINAPI *pfn_ptr)(void *);
    for (i = 0; names[i] && !h; i++) h = LoadLibraryA(names[i]);
    if (!h) { printf("d3dgame9: no d3dx9 DLL, fixed-function only\n"); return; }
    compile = (pfn_compile)GetProcAddress(h, "D3DXCompileShader");
    if (!compile) return;
    if (SUCCEEDED(compile(vs_src, sizeof(vs_src) - 1, NULL, NULL, "main", "vs_1_1", 0, (void **)&code, NULL, NULL)) && code) {
        pfn_ptr gp = (pfn_ptr)((void **)code->vt)[3];
        IDirect3DDevice9_CreateVertexShader(X.dev, (const DWORD *)gp(code), &X.vs);
        ((void (WINAPI *)(void *))((void **)code->vt)[2])(code);
        code = NULL;
    }
    if (SUCCEEDED(compile(ps_src, sizeof(ps_src) - 1, NULL, NULL, "main", "ps_1_1", 0, (void **)&code, NULL, NULL)) && code) {
        pfn_ptr gp = (pfn_ptr)((void **)code->vt)[3];
        IDirect3DDevice9_CreatePixelShader(X.dev, (const DWORD *)gp(code), &X.ps);
        ((void (WINAPI *)(void *))((void **)code->vt)[2])(code);
    }
    printf("d3dgame9: shaders %s\n", X.vs && X.ps ? "vs_1_1 + ps_1_1 compiled" : "failed, fixed-function");
}

static void set_states(void)
{
    D3DLIGHT9 l;
    D3DMATERIAL9 m;
    memset(&l, 0, sizeof(l));
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = 1.0f;
    l.Direction.x = -0.5f; l.Direction.y = -1.0f; l.Direction.z = 0.4f;
    IDirect3DDevice9_SetLight(X.dev, 0, &l);
    IDirect3DDevice9_LightEnable(X.dev, 0, TRUE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_AMBIENT, 0x00404040);
    memset(&m, 0, sizeof(m));
    m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = m.Diffuse.a = 1.0f;
    m.Ambient = m.Diffuse;
    IDirect3DDevice9_SetMaterial(X.dev, &m);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}

static void set_matrices(const struct mat4 *world, const struct mat4 *view, const struct mat4 *proj, int shader)
{
    if (shader && X.vs) {
        struct mat4 wvp, t;
        float ldir[4] = { -0.5f, -1.0f, 0.4f, 0.0f };
        float l = sqrtf(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
        int i, j;
        float wvp_t[16];
        ldir[0] /= l; ldir[1] /= l; ldir[2] /= l;
        m_mul(&t, world, view);
        m_mul(&wvp, &t, proj);
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) wvp_t[i * 4 + j] = wvp.m[j][i];   /* HLSL column-major */
        IDirect3DDevice9_SetVertexShaderConstantF(X.dev, 0, wvp_t, 4);
        IDirect3DDevice9_SetVertexShaderConstantF(X.dev, 4, ldir, 1);
    }
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_WORLD, (const D3DMATRIX *)world);
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_VIEW, (const D3DMATRIX *)view);
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_PROJECTION, (const D3DMATRIX *)proj);
}

static void draw_cubes(const struct mat4 *view, const struct mat4 *proj, int shader)
{
    int i;
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_LIGHTING, shader && X.vs ? FALSE : TRUE);
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.tex_checker);
    IDirect3DDevice9_SetFVF(X.dev, FVF_PNT);
    IDirect3DDevice9_SetStreamSource(X.dev, 0, X.vb_cube, 0, sizeof(struct vtx_pnt));
    IDirect3DDevice9_SetIndices(X.dev, X.ib_cube);
    if (shader && X.vs) {
        IDirect3DDevice9_SetVertexShader(X.dev, X.vs);
        IDirect3DDevice9_SetPixelShader(X.dev, X.ps);
    }
    for (i = 0; i < NUM_CUBES; i++) {
        struct mat4 w;
        D3DMATERIAL9 m;
        game_cube_world(&G, i, &w);
        set_matrices(&w, view, proj, shader);
        memset(&m, 0, sizeof(m));
        m.Diffuse.r = 0.5f + 0.5f * (i & 1); m.Diffuse.g = 0.5f + 0.25f * (i & 2); m.Diffuse.b = 1.0f - 0.2f * i; m.Diffuse.a = 1.0f;
        m.Ambient = m.Diffuse;
        IDirect3DDevice9_SetMaterial(X.dev, &m);
        IDirect3DDevice9_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    }
    if (shader && X.vs) {
        IDirect3DDevice9_SetVertexShader(X.dev, NULL);
        IDirect3DDevice9_SetPixelShader(X.dev, NULL);
    }
}

static int render(void)
{
    struct mat4 view, proj, world, rview;
    float eye[3], at[3];
    static const float reye[3] = { 0.0f, 4.0f, -6.0f }, rat[3] = { 0.0f, 0.5f, 3.0f };
    void *p;
    IDirect3DSurface9 *bb, *zs, *rts;
    struct vtx_pct part[NUM_PARTICLES * 6];
    struct vtx_rhw bars[BARS * 6];
    int i, n;

    game_camera(&G, eye, at);
    m_lookat(&view, eye, at);
    m_perspective(&proj, 1.1f, G.o.w / (float)G.o.h, 0.5f, 60.0f);
    m_lookat(&rview, reye, rat);

    /* 1. render-to-texture: the cubes from a fixed camera */
    IDirect3DDevice9_GetRenderTarget(X.dev, 0, &bb);
    IDirect3DDevice9_GetDepthStencilSurface(X.dev, &zs);
    IDirect3DTexture9_GetSurfaceLevel(X.tex_rtt, 0, &rts);
    IDirect3DDevice9_SetRenderTarget(X.dev, 0, rts);
    IDirect3DDevice9_SetDepthStencilSurface(X.dev, X.rtt_depth);
    IDirect3DDevice9_Clear(X.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff402010, 1.0f, 0);
    IDirect3DDevice9_BeginScene(X.dev);
    {
        struct mat4 rproj;
        m_perspective(&rproj, 0.9f, 1.0f, 0.5f, 60.0f);
        draw_cubes(&rview, &rproj, 0);
    }
    IDirect3DDevice9_EndScene(X.dev);
    IDirect3DDevice9_SetRenderTarget(X.dev, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(X.dev, zs);
    IDirect3DSurface9_Release(rts);
    IDirect3DSurface9_Release(bb);
    IDirect3DSurface9_Release(zs);

    /* 2. main scene */
    IDirect3DDevice9_Clear(X.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1.0f, 0);
    IDirect3DDevice9_BeginScene(X.dev);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_FILLMODE, G.wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, TRUE);

    /* ground */
    m_identity(&world);
    set_matrices(&world, &view, &proj, 0);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.tex_checker);
    IDirect3DDevice9_SetFVF(X.dev, FVF_PNT);
    IDirect3DDevice9_SetStreamSource(X.dev, 0, X.vb_ground, 0, sizeof(struct vtx_pnt));
    IDirect3DDevice9_DrawPrimitive(X.dev, D3DPT_TRIANGLESTRIP, 0, 2);

    /* cubes */
    draw_cubes(&view, &proj, G.o.shader);

    /* wave grid: dynamic VB, discard each frame, vertex colours, 565 texture */
    if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(X.vb_grid, 0, 0, &p, D3DLOCK_DISCARD))) {
        geo_grid_fill((struct vtx_pct *)p, G.t);
        IDirect3DVertexBuffer9_Unlock(X.vb_grid);
    }
    m_identity(&world);
    set_matrices(&world, &view, &proj, 0);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.tex_grad);
    IDirect3DDevice9_SetFVF(X.dev, FVF_PCT);
    IDirect3DDevice9_SetStreamSource(X.dev, 0, X.vb_grid, 0, sizeof(struct vtx_pct));
    IDirect3DDevice9_SetIndices(X.dev, X.ib_grid);
    IDirect3DDevice9_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, 0, GRID_VERTS, 0, GRID_INDICES / 3);

    /* monitor quad showing the render target */
    {
        struct vtx_pct q[4] = {
            { -3.0f, 2.5f, 9.0f, 0xffffffff, 0, 0 }, { 3.0f, 2.5f, 9.0f, 0xffffffff, 1, 0 },
            { -3.0f, -0.5f, 9.0f, 0xffffffff, 0, 1 }, { 3.0f, -0.5f, 9.0f, 0xffffffff, 1, 1 } };
        IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.tex_rtt);
        IDirect3DDevice9_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    }

    /* particles: additive, no z-write, camera-facing quads */
    n = 0;
    for (i = 0; i < NUM_PARTICLES; i++) {
        float pos[3], size; DWORD c;
        float rx = view.m[0][0], ry = view.m[1][0], rz = view.m[2][0];   /* camera right */
        float ux = view.m[0][1], uy = view.m[1][1], uz = view.m[2][1];   /* camera up */
        game_particle(&G, i, pos, &c, &size);
#define PV(sx, sy, tu, tv) do { part[n].x = pos[0] + (rx * (sx) + ux * (sy)) * size; part[n].y = pos[1] + (ry * (sx) + uy * (sy)) * size; \
        part[n].z = pos[2] + (rz * (sx) + uz * (sy)) * size; part[n].color = c; part[n].u = tu; part[n].v = tv; n++; } while (0)
        PV(-1, 1, 0, 0); PV(1, 1, 1, 0); PV(-1, -1, 0, 1);
        PV(1, 1, 1, 0); PV(1, -1, 1, 1); PV(-1, -1, 0, 1);
#undef PV
    }
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.tex_disc);
    IDirect3DDevice9_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLELIST, n / 3, part, sizeof(part[0]));
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ALPHABLENDENABLE, FALSE);

    /* frame-time bars (screen space, no texture, no z) */
    n = game_bars(&G, bars, G.o.w, G.o.h);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetTexture(X.dev, 0, NULL);
    IDirect3DDevice9_SetFVF(X.dev, FVF_RHW);
    IDirect3DDevice9_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLELIST, n / 3, bars, sizeof(bars[0]));
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_EndScene(X.dev);
    return 1;
}

static void dump_frame(void)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(X.dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(X.dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))
        && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(X.dev, bb, sys))
        && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        int ok = bmp_write(G.o.dump_file, lr.pBits, lr.Pitch, d.Width, d.Height, d.Format == D3DFMT_R5G6B5);
        IDirect3DSurface9_UnlockRect(sys);
        printf("d3dgame9: frame %u -> %s (%s)\n", G.frame, G.o.dump_file, ok ? "written" : "write failed");
    } else {
        printf("d3dgame9: dump failed (GetRenderTargetData)\n");
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (bb) IDirect3DSurface9_Release(bb);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    HWND hwnd;
    D3DADAPTER_IDENTIFIER9 id;
    D3DCAPS9 caps;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    game_init(&G, argc, argv);
    hwnd = game_window(&G, "d3dgame9");
    X.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!X.d3d) { printf("d3dgame9: Direct3DCreate9 failed\n"); return 1; }
    IDirect3D9_GetAdapterIdentifier(X.d3d, D3DADAPTER_DEFAULT, 0, &id);
    IDirect3D9_GetDeviceCaps(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D9_GetAdapterDisplayMode(X.d3d, D3DADAPTER_DEFAULT, &mode);
    printf("d3dgame9: adapter \"%s\" driver \"%s\" vs %lu.%lu ps %lu.%lu maxtex %lu\n", id.Description, id.Driver,
           (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
           (unsigned long)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
           (unsigned long)caps.MaxTextureWidth);

    memset(&X.pp, 0, sizeof(X.pp));
    X.pp.BackBufferWidth = G.o.w;
    X.pp.BackBufferHeight = G.o.h;
    X.pp.BackBufferFormat = G.o.fullscreen ? (G.o.bpp16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8) : mode.Format;
    X.pp.BackBufferCount = 1;
    X.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    X.pp.hDeviceWindow = hwnd;
    X.pp.Windowed = !G.o.fullscreen;
    X.pp.EnableAutoDepthStencil = TRUE;
    X.pp.AutoDepthStencilFormat = D3DFMT_D16;
    X.pp.PresentationInterval = G.o.novsync ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    hr = IDirect3D9_CreateDevice(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                                                                                 : D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &X.pp, &X.dev);
    if (FAILED(hr)) {
        printf("d3dgame9: CreateDevice failed %s\n", hr_str(hr));
        return 1;
    }
    printf("d3dgame9: device %dx%d %s %s, %s vertex processing\n", G.o.w, G.o.h, G.o.fullscreen ? "fullscreen" : "windowed",
           G.o.bpp16 ? "565" : "8888", (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "hardware" : "software");
    fflush(stdout);
    if (!make_textures() || !make_geometry()) return 1;
    if (G.o.shader) make_shaders();
    set_states();

    while (!G.quit && game_pump()) {
        float dt = game_step(&G);
        if (!render()) break;
        if (G.o.dump_frame >= 0 && (int)G.frame == G.o.dump_frame) dump_frame();
        hr = IDirect3DDevice9_Present(X.dev, NULL, NULL, NULL, NULL);
        if (hr == D3DERR_DEVICELOST) {
            printf("d3dgame9: device lost, resetting\n");
            while (IDirect3DDevice9_TestCooperativeLevel(X.dev) == D3DERR_DEVICELOST) Sleep(50);
            IDirect3DDevice9_Reset(X.dev, &X.pp);
            set_states();
        }
        if (dt < 0) {
            char title[96];
            snprintf(title, sizeof(title), "d3dgame9: %.1f fps, frame %u", G.fps, G.frame);
            SetWindowTextA(hwnd, title);
            printf("%s\n", title);
            fflush(stdout);
        }
        if (G.o.frames && (int)G.frame >= G.o.frames) break;
    }
    printf("d3dgame9: %u frames, %lu ms\n", G.frame, (unsigned long)(GetTickCount() - G.t0_ms));
    if (X.dev) IDirect3DDevice9_Release(X.dev);
    if (X.d3d) IDirect3D9_Release(X.d3d);
    return 0;
}
