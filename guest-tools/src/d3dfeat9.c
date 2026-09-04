/*
 * d3dfeat9: the Direct3D 9 feature test of the paravirtual device (doc 14
 * P3). Where D3DGAME9 is the fixed-function reference scene golden on the
 * rig, this one exercises what games of the era do beyond it, with no
 * D3DX: hand-assembled vs_1_1 / ps_1_1 bytecode, a vertex declaration,
 * shader constants (float, int, bool), a cube texture sampled by the
 * pixel shader, state blocks (recorded and D3DSBT_ALL), an occlusion
 * query, a DEFAULT-pool offscreen surface with ColorFill + StretchRect
 * into a render-target texture, UpdateSurface from system memory into a
 * DEFAULT texture, a clip plane, DrawIndexedPrimitiveUP, scissor.
 *
 * Deterministic: -frames N -dump N file.bmp like D3DGAME9. The same source
 * builds natively over DXVK (tools/d3dfeat9-native.cpp); the guest frame
 * through the device must be byte-identical to the native frame, and the
 * log lines (query result, HRESULTs) must match.
 *
 * Build (guest): i686-w64-mingw32-gcc -O2 -o d3dfeat9.exe d3dfeat9.c -ld3d9 -lgdi32 -luser32
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "d3dgame.h"

#define LOG_NAME "d3dfeat9.log"

static const char *hr_str(HRESULT hr) { static char b[32]; sprintf(b, "0x%08lx", (unsigned long)hr); return b; }
#define CHK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { game_log("d3dfeat9: %s failed %s", #call, hr_str(hr_)); return 0; } } while (0)

/* --- hand-assembled shaders (SM1.1 token stream, see d3dpt docs) --- */
static const DWORD vs_code[] = {
    0xFFFE0101,                                  /* vs_1_1 */
    0x0000001F, 0x80000000, 0x900F0000,          /* dcl_position v0 */
    0x0000001F, 0x80000003, 0x900F0001,          /* dcl_normal   v1 */
    0x0000001F, 0x8000000A, 0x900F0002,          /* dcl_color    v2 */
    0x0000001F, 0x80000005, 0x900F0003,          /* dcl_texcoord v3 */
    0x00000014, 0xC00F0000, 0x90E40000, 0xA0E40000, /* m4x4 oPos, v0, c0 (D3DSIO_M4x4 = 20) */
    0x00000001, 0xD00F0000, 0x90E40002,          /* mov oD0, v2 */
    0x00000001, 0xE00F0000, 0x90E40003,          /* mov oT0, v3 */
    0x00000001, 0xE00F0001, 0x90E40001,          /* mov oT1, v1 */
    0x0000FFFF
};
static const DWORD ps_tex_code[] = {
    0xFFFF0101,                                  /* ps_1_1 */
    0x00000042, 0xB00F0000,                      /* tex t0 */
    0x00000005, 0x800F0000, 0xB0E40000, 0x90E40000, /* mul r0, t0, v0 */
    0x00000005, 0x800F0000, 0x80E40000, 0xA0E40000, /* mul r0, r0, c0 */
    0x0000FFFF
};
static const DWORD ps_cube_code[] = {
    0xFFFF0101,                                  /* ps_1_1 */
    0x00000042, 0xB00F0001,                      /* tex t1 (cube, 3 coords from oT1) */
    0x00000001, 0x800F0000, 0xB0E40001,          /* mov r0, t1 */
    0x0000FFFF
};

struct vtx_decl { float x, y, z, nx, ny, nz; DWORD color; float u, v; };
static const D3DVERTEXELEMENT9 decl_elems[] = {
    { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
    { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    { 0, 28, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};

struct gfx {
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib;
    IDirect3DTexture9 *checker, *rtt, *def_tex;
    IDirect3DCubeTexture9 *cube;
    IDirect3DSurface9 *offscreen, *sysmem;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps_tex, *ps_cube;
    IDirect3DStateBlock9 *sb_quad_a, *sb_all;
    IDirect3DQuery9 *occ;
};
static struct game G;
static struct gfx X;

static int make_resources(void)
{
    D3DLOCKED_RECT lr;
    int level, w, i;
    void *p;
    struct vtx_decl *v;
    WORD *idx;
    IDirect3DSurface9 *rts;
    RECT half;
    static const DWORD face_colors[6] = { 0xffff4040, 0xff40ff40, 0xff4040ff, 0xffffff40, 0xffff40ff, 0xff40ffff };

    CHK(IDirect3DDevice9_CreateVertexDeclaration(X.dev, decl_elems, &X.decl));
    /* two quads in one VB: A (shader + checker) at x<0, B (cube) at x>0 */
    CHK(IDirect3DDevice9_CreateVertexBuffer(X.dev, 8 * sizeof(struct vtx_decl), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &X.vb, NULL));
    CHK(IDirect3DVertexBuffer9_Lock(X.vb, 0, 0, &p, 0));
    v = (struct vtx_decl *)p;
    for (i = 0; i < 8; i++) {
        int q = i / 4, c = i % 4;
        float cx = q ? 1.3f : -1.3f;
        v[i].x = cx + (c == 1 || c == 2 ? 1.0f : -1.0f);
        v[i].y = 0.9f + (c >= 2 ? 1.0f : -1.0f);
        v[i].z = 0.0f;
        v[i].nx = (c == 1 || c == 2 ? 1.0f : -1.0f); v[i].ny = (c >= 2 ? 1.0f : -1.0f); v[i].nz = 1.0f;
        v[i].color = c == 0 ? 0xffffffff : c == 1 ? 0xffff8080 : c == 2 ? 0xff80ff80 : 0xff8080ff;
        v[i].u = (c == 1 || c == 2) ? 1.0f : 0.0f; v[i].v = c >= 2 ? 0.0f : 1.0f;
    }
    CHK(IDirect3DVertexBuffer9_Unlock(X.vb));
    CHK(IDirect3DDevice9_CreateIndexBuffer(X.dev, 12 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &X.ib, NULL));
    CHK(IDirect3DIndexBuffer9_Lock(X.ib, 0, 0, &p, 0));
    idx = (WORD *)p;
    for (i = 0; i < 2; i++) { idx[i * 6 + 0] = i * 4; idx[i * 6 + 1] = i * 4 + 2; idx[i * 6 + 2] = i * 4 + 1; idx[i * 6 + 3] = i * 4; idx[i * 6 + 4] = i * 4 + 3; idx[i * 6 + 5] = i * 4 + 2; }
    CHK(IDirect3DIndexBuffer9_Unlock(X.ib));

    CHK(IDirect3DDevice9_CreateTexture(X.dev, 64, 64, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &X.checker, NULL));
    for (level = 0, w = 64; w >= 1; level++, w >>= 1) {
        if (FAILED(IDirect3DTexture9_LockRect(X.checker, level, &lr, NULL, 0))) break;
        tex_checker_8888(lr.pBits, lr.Pitch, w, w, level);
        IDirect3DTexture9_UnlockRect(X.checker, level);
    }
    /* cube: six flat colours with a diagonal gradient, 2 mip levels */
    CHK(IDirect3DDevice9_CreateCubeTexture(X.dev, 32, 2, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &X.cube, NULL));
    for (i = 0; i < 6; i++) for (level = 0, w = 32; level < 2; level++, w >>= 1) {
        int x, y;
        CHK(IDirect3DCubeTexture9_LockRect(X.cube, (D3DCUBEMAP_FACES)i, level, &lr, NULL, 0));
        for (y = 0; y < w; y++) for (x = 0; x < w; x++) {
            DWORD c = face_colors[i], k = (DWORD)((x + y) * 255 / (2 * w - 2));
            ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xff000000 | (((c >> 16 & 255) * k / 255) << 16) | (((c >> 8 & 255) * k / 255) << 8) | ((c & 255) * k / 255);
        }
        IDirect3DCubeTexture9_UnlockRect(X.cube, (D3DCUBEMAP_FACES)i, level);
    }
    /* offscreen DEFAULT surface: ColorFill two halves, StretchRect into a render-target texture */
    CHK(IDirect3DDevice9_CreateOffscreenPlainSurface(X.dev, 64, 64, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &X.offscreen, NULL));
    CHK(IDirect3DDevice9_ColorFill(X.dev, X.offscreen, NULL, 0xff204080));
    half.left = 0; half.top = 32; half.right = 64; half.bottom = 64;
    CHK(IDirect3DDevice9_ColorFill(X.dev, X.offscreen, &half, 0xffe0a020));
    CHK(IDirect3DDevice9_CreateTexture(X.dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &X.rtt, NULL));
    CHK(IDirect3DTexture9_GetSurfaceLevel(X.rtt, 0, &rts));
    CHK(IDirect3DDevice9_StretchRect(X.dev, X.offscreen, NULL, rts, NULL, D3DTEXF_NONE));
    IDirect3DSurface9_Release(rts);
    /* system-memory surface with a pattern, UpdateSurface'd into a DEFAULT texture */
    CHK(IDirect3DDevice9_CreateOffscreenPlainSurface(X.dev, 32, 32, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &X.sysmem, NULL));
    CHK(IDirect3DSurface9_LockRect(X.sysmem, &lr, NULL, 0));
    {
        int x, y;
        for (y = 0; y < 32; y++) for (x = 0; x < 32; x++)
            ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = ((x / 4 + y / 4) & 1) ? 0xffffffff : 0xff800000 | (DWORD)(x * 8) << 8 | (DWORD)(y * 8);
    }
    CHK(IDirect3DSurface9_UnlockRect(X.sysmem));
    CHK(IDirect3DDevice9_CreateTexture(X.dev, 64, 64, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &X.def_tex, NULL));
    CHK(IDirect3DTexture9_GetSurfaceLevel(X.def_tex, 0, &rts));
    CHK(IDirect3DDevice9_ColorFill(X.dev, rts, NULL, 0xff303030));
    {
        POINT pt = { 16, 16 };
        CHK(IDirect3DDevice9_UpdateSurface(X.dev, X.sysmem, NULL, rts, &pt));
    }
    IDirect3DSurface9_Release(rts);

    CHK(IDirect3DDevice9_CreateVertexShader(X.dev, vs_code, &X.vs));
    CHK(IDirect3DDevice9_CreatePixelShader(X.dev, ps_tex_code, &X.ps_tex));
    CHK(IDirect3DDevice9_CreatePixelShader(X.dev, ps_cube_code, &X.ps_cube));
    CHK(IDirect3DDevice9_CreateQuery(X.dev, D3DQUERYTYPE_OCCLUSION, &X.occ));
    return 1;
}

static int set_states(void)
{
    static const float tint[4] = { 1.0f, 0.9f, 0.8f, 1.0f };
    static const int ic[4] = { 3, 0, 0, 0 };
    static const BOOL bc[1] = { TRUE };
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(X.dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(X.dev, 1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(X.dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(X.dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetVertexShaderConstantI(X.dev, 0, ic, 1);
    IDirect3DDevice9_SetVertexShaderConstantB(X.dev, 0, bc, 1);
    /* the recorded block: everything quad A needs except the per-frame matrix */
    CHK(IDirect3DDevice9_BeginStateBlock(X.dev));
    IDirect3DDevice9_SetVertexDeclaration(X.dev, X.decl);
    IDirect3DDevice9_SetStreamSource(X.dev, 0, X.vb, 0, sizeof(struct vtx_decl));
    IDirect3DDevice9_SetIndices(X.dev, X.ib);
    IDirect3DDevice9_SetVertexShader(X.dev, X.vs);
    IDirect3DDevice9_SetPixelShader(X.dev, X.ps_tex);
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.checker);
    IDirect3DDevice9_SetPixelShaderConstantF(X.dev, 0, tint, 1);
    CHK(IDirect3DDevice9_EndStateBlock(X.dev, &X.sb_quad_a));
    /* undo what the recording applied, then capture "everything" as the frame's baseline */
    IDirect3DDevice9_SetVertexShader(X.dev, NULL);
    IDirect3DDevice9_SetPixelShader(X.dev, NULL);
    IDirect3DDevice9_SetTexture(X.dev, 0, NULL);
    IDirect3DDevice9_SetVertexDeclaration(X.dev, NULL);
    IDirect3DDevice9_SetStreamSource(X.dev, 0, NULL, 0, 0);
    IDirect3DDevice9_SetIndices(X.dev, NULL);
    CHK(IDirect3DDevice9_CreateStateBlock(X.dev, D3DSBT_ALL, &X.sb_all));
    return 1;
}

static void set_matrix(const struct mat4 *world, const struct mat4 *view, const struct mat4 *proj)
{
    struct mat4 wv, wvp;
    float t[16];
    int i, j;
    m_mul(&wv, world, view);
    m_mul(&wvp, &wv, proj);
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) t[i * 4 + j] = wvp.m[j][i];   /* column-major for m4x4 */
    IDirect3DDevice9_SetVertexShaderConstantF(X.dev, 0, t, 4);
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_WORLD, (const D3DMATRIX *)world);
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_VIEW, (const D3DMATRIX *)view);
    IDirect3DDevice9_SetTransform(X.dev, D3DTS_PROJECTION, (const D3DMATRIX *)proj);
}

static int render(void)
{
    struct mat4 world, view, proj, rot;
    float eye[3] = { 0.0f, 0.4f, -6.0f }, at[3] = { 0.0f, 0.4f, 0.0f };
    float plane[4] = { -1.0f, 0.0f, 0.0f, 2.8f };   /* keeps x <= 2.8: cuts the right quarter of quad D */
    struct vtx_pct q[4];
    WORD qi[6] = { 0, 2, 1, 0, 3, 2 };
    RECT sc = { 0, 0, G.o.w, G.o.h };
    DWORD pixels = 0;
    int i;

    m_perspective(&proj, 1.0f, (float)G.o.w / (float)G.o.h, 0.5f, 50.0f);
    m_lookat(&view, eye, at);
    m_rot_y(&rot, (float)G.frame * 0.01f);
    m_identity(&world);
    m_mul(&world, &rot, &world);

    IDirect3DDevice9_Clear(X.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff101828, 1.0f, 0);
    IDirect3DDevice9_BeginScene(X.dev);
    IDirect3DStateBlock9_Apply(X.sb_all);
    IDirect3DDevice9_SetScissorRect(X.dev, &sc);
    set_matrix(&world, &view, &proj);

    /* quad A: recorded block + shaders */
    IDirect3DStateBlock9_Apply(X.sb_quad_a);
    IDirect3DDevice9_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    /* quad B: cube map through ps_1_1 */
    IDirect3DDevice9_SetPixelShader(X.dev, X.ps_cube);
    IDirect3DDevice9_SetTexture(X.dev, 1, (IDirect3DBaseTexture9 *)X.cube);
    IDirect3DDevice9_DrawIndexedPrimitive(X.dev, D3DPT_TRIANGLELIST, 0, 4, 4, 6, 2);
    IDirect3DDevice9_SetTexture(X.dev, 1, NULL);

    /* fixed function from here */
    IDirect3DDevice9_SetVertexShader(X.dev, NULL);
    IDirect3DDevice9_SetPixelShader(X.dev, NULL);
    IDirect3DDevice9_SetFVF(X.dev, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    m_identity(&world);
    set_matrix(&world, &view, &proj);
    /* quad C (bottom left): the render-target texture filled through StretchRect, inside an occlusion query */
    for (i = 0; i < 4; i++) {
        q[i].x = -2.3f + (i == 1 || i == 2 ? 1.0f : -1.0f); q[i].y = -1.3f + (i >= 2 ? 1.0f : -1.0f); q[i].z = 0.0f;
        q[i].color = 0xffffffff; q[i].u = (i == 1 || i == 2) ? 1.0f : 0.0f; q[i].v = i >= 2 ? 0.0f : 1.0f;
    }
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.rtt);
    IDirect3DQuery9_Issue(X.occ, D3DISSUE_BEGIN);
    IDirect3DDevice9_DrawPrimitiveUP(X.dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(q[0]));
    IDirect3DQuery9_Issue(X.occ, D3DISSUE_END);
    /* quad D (bottom right): the DEFAULT texture patched by UpdateSurface, clipped by a plane, indexed UP */
    for (i = 0; i < 4; i++) { q[i].x += 4.6f; q[i].color = 0xffc0e0ff; }
    IDirect3DDevice9_SetTexture(X.dev, 0, (IDirect3DBaseTexture9 *)X.def_tex);
    IDirect3DDevice9_SetClipPlane(X.dev, 0, plane);
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CLIPPLANEENABLE, 1);
    IDirect3DDevice9_DrawIndexedPrimitiveUP(X.dev, D3DPT_TRIANGLELIST, 0, 4, 2, qi, D3DFMT_INDEX16, q, sizeof(q[0]));
    IDirect3DDevice9_SetRenderState(X.dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetTexture(X.dev, 0, NULL);
    IDirect3DDevice9_EndScene(X.dev);

    if (G.o.dump_frame >= 0 && (int)G.frame == G.o.dump_frame) {
        HRESULT hr;
        int spins = 0;
        while ((hr = IDirect3DQuery9_GetData(X.occ, &pixels, sizeof(pixels), D3DGETDATA_FLUSH)) == S_FALSE && spins++ < 100000) ;
        game_log("d3dfeat9: occlusion query at frame %u: %s, %lu pixels (quad C is 2 triangles at 640x480: expect ~13000)", G.frame, hr_str(hr), (unsigned long)pixels);
    }
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
        game_log("d3dfeat9: frame %u -> %s (%s)", G.frame, G.o.dump_file, ok ? "written" : "write failed");
    } else {
        game_log("d3dfeat9: dump failed");
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (bb) IDirect3DSurface9_Release(bb);
}

int main(int argc, char **argv)
{
    HWND hwnd;
    HRESULT hr;
    D3DCAPS9 caps;
    D3DDISPLAYMODE mode;
    D3DADAPTER_IDENTIFIER9 id;
    float readback[4];
    int ic[4];
    BOOL bc[1];

    game_init(&G, argc, argv);
    game_log_open(LOG_NAME, argc, argv);
    hwnd = game_window(&G, "d3dfeat9");
    X.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!X.d3d) { game_log("d3dfeat9: Direct3DCreate9 failed"); return 1; }
    IDirect3D9_GetAdapterIdentifier(X.d3d, D3DADAPTER_DEFAULT, 0, &id);
    IDirect3D9_GetDeviceCaps(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D9_GetAdapterDisplayMode(X.d3d, D3DADAPTER_DEFAULT, &mode);
    game_log("d3dfeat9: adapter \"%s\" vs %lu.%lu ps %lu.%lu", id.Description,
             (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
             (unsigned long)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));
    memset(&X.pp, 0, sizeof(X.pp));
    X.pp.BackBufferWidth = G.o.w; X.pp.BackBufferHeight = G.o.h;
    X.pp.BackBufferFormat = G.o.fullscreen ? D3DFMT_X8R8G8B8 : mode.Format;
    X.pp.BackBufferCount = 1; X.pp.SwapEffect = D3DSWAPEFFECT_DISCARD; X.pp.hDeviceWindow = hwnd; X.pp.Windowed = !G.o.fullscreen;
    X.pp.EnableAutoDepthStencil = TRUE; X.pp.AutoDepthStencilFormat = D3DFMT_D16;
    X.pp.PresentationInterval = G.o.novsync ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    hr = IDirect3D9_CreateDevice(X.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &X.pp, &X.dev);
    if (FAILED(hr)) { game_log("d3dfeat9: CreateDevice failed %s", hr_str(hr)); return 1; }
    game_log("d3dfeat9: device %dx%d %s", G.o.w, G.o.h, G.o.fullscreen ? "fullscreen" : "windowed");
    if (!make_resources() || !set_states()) return 1;
    /* shadowed getters must return what was set */
    IDirect3DDevice9_GetVertexShaderConstantI(X.dev, 0, ic, 1);
    IDirect3DDevice9_GetVertexShaderConstantB(X.dev, 0, bc, 1);
    IDirect3DStateBlock9_Apply(X.sb_quad_a);
    IDirect3DDevice9_GetPixelShaderConstantF(X.dev, 0, readback, 1);
    game_log("d3dfeat9: getters: vs int c0 = %d, vs bool b0 = %d, ps float c0 = %.2f %.2f %.2f %.2f", ic[0], bc[0], readback[0], readback[1], readback[2], readback[3]);

    while (!G.quit && game_pump()) {
        float dt = game_step(&G);
        if (!render()) break;
        if (G.o.dump_frame >= 0 && (int)G.frame == G.o.dump_frame) dump_frame();
        IDirect3DDevice9_Present(X.dev, NULL, NULL, NULL, NULL);
        if (dt < 0) { game_log("d3dfeat9: %.1f fps, frame %u", G.fps, G.frame); }
        if (G.o.frames && (int)G.frame >= G.o.frames) break;
    }
    game_log("d3dfeat9: %u frames, %lu ms", G.frame, (unsigned long)(GetTickCount() - G.start_ms));
    IDirect3DDevice9_Release(X.dev);
    IDirect3D9_Release(X.d3d);
    game_log("d3dfeat9: exit");
    return 0;
}
