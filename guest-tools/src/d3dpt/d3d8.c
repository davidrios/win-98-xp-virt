/*
 * d3d8.c — the guest d3d8.dll of the paravirtual Direct3D device (doc 14
 * P4): Direct3D 8 as thin wrappers over the d3d9 object model of d3d9.c,
 * compiled into one translation unit (the d3d8to9 shape, in C). The
 * device, resources, encoder and transport are the d3d9 ones; this file
 * only converts the API differences: caps/identifier/present-parameter
 * layouts, texture-stage sampler states, D3DRS_ZBIAS, vertex shader
 * declarations (D3DVSD_* tokens -> D3DVERTEXELEMENT9 + dcl instructions),
 * DWORD shader and state-block handles, SetIndices' base vertex, CopyRects.
 *
 * mingw's d3d8.h cannot be included next to d3d9.h, so the interface
 * structs come from gen_vtbl8.py (d3d8_vtbl.h) and the D3D8-only types are
 * defined here.
 *
 * Build: guest-tools/build-wrappers.sh (msvcrt, -march=pentium3).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d9.c"

/* ------------------------------------------------------ D3D8-only types */
/* mingw's d3d8types.h / d3d8caps.h pack these to 4 bytes on i386; a
 * naturally aligned D3DADAPTER_IDENTIFIER8 is 4 bytes longer and
 * GetAdapterIdentifier then clears past the caller's buffer (found the
 * hard way: D3DGAME8 hung on return from main) */
#pragma pack(push, 4)
typedef struct _D3DPRESENT_PARAMETERS8 {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    WINBOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS8;
typedef struct _D3DSURFACE_DESC8 {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType; UINT Width, Height;
} D3DSURFACE_DESC8;
typedef struct _D3DVOLUME_DESC8 {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size, Width, Height, Depth;
} D3DVOLUME_DESC8;
typedef struct _D3DCLIPSTATUS8 { DWORD ClipUnion, ClipIntersection; } D3DCLIPSTATUS8;
typedef struct _D3DADAPTER_IDENTIFIER8 {
    char Driver[512], Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER8;
typedef struct _D3DCAPS8 {
    D3DDEVTYPE DeviceType; UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps, PrimitiveMiscCaps, RasterCaps, ZCmpCaps,
          SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps, CubeTextureFilterCaps,
          VolumeTextureFilterCaps, TextureAddressCaps, VolumeTextureAddressCaps, LineCaps, MaxTextureWidth, MaxTextureHeight,
          MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    DWORD StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps,
          MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion, MaxVertexShaderConst, PixelShaderVersion;
    float MaxPixelShaderValue;
} D3DCAPS8;
#pragma pack(pop)
typedef char d3d8_size_check_id[sizeof(D3DADAPTER_IDENTIFIER8) == 1068 ? 1 : -1];
typedef char d3d8_size_check_caps[sizeof(D3DCAPS8) == 212 ? 1 : -1];
typedef char d3d8_size_check_pp[sizeof(D3DPRESENT_PARAMETERS8) == 52 ? 1 : -1];
typedef char d3d8_size_check_desc[sizeof(D3DSURFACE_DESC8) == 32 ? 1 : -1];
#define D3D8_SDK_VERSION 220
#define D3DSWAPEFFECT_COPY_VSYNC8 4
#define D3DRS8_LINEPATTERN 10
#define D3DRS8_ZVISIBLE 30
#define D3DRS8_EDGEANTIALIAS 40
#define D3DRS8_ZBIAS 47
#define D3DRS8_SOFTWAREVERTEXPROCESSING 153
#define D3DRS8_PATCHSEGMENTS 164
#define SHADER_HANDLE_BIT 0x80000000u

DEFINE_GUID(IID_IDirect3D8,             0x1dd9e8da, 0x1c77, 0x4d40, 0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12);
DEFINE_GUID(IID_IDirect3DDevice8,       0x7385e5df, 0x8fe8, 0x41d5, 0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf);
DEFINE_GUID(IID_IDirect3DResource8,     0x1b36bb7b, 0x09b7, 0x410a, 0xb4, 0x45, 0x7d, 0x14, 0x30, 0xd7, 0xb3, 0x3f);
DEFINE_GUID(IID_IDirect3DBaseTexture8,  0xb4211cfa, 0x51b9, 0x4a9f, 0xab, 0x78, 0xdb, 0x99, 0xb2, 0xbb, 0x67, 0x8e);
DEFINE_GUID(IID_IDirect3DTexture8,      0xe4cdd575, 0x2866, 0x4f01, 0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58);
DEFINE_GUID(IID_IDirect3DCubeTexture8,  0x3ee5b968, 0x2aca, 0x4c34, 0x8b, 0xb5, 0x7e, 0x0c, 0x3d, 0x19, 0xb7, 0x50);
DEFINE_GUID(IID_IDirect3DVertexBuffer8, 0x8aeeeac7, 0x05f9, 0x44d4, 0xb5, 0x91, 0x00, 0x0b, 0x0d, 0xf1, 0xcb, 0x95);
DEFINE_GUID(IID_IDirect3DIndexBuffer8,  0x0e689c9a, 0x053d, 0x44a0, 0x9d, 0x92, 0xdb, 0x0e, 0x3d, 0x75, 0x0f, 0x86);
DEFINE_GUID(IID_IDirect3DSurface8,      0xb96eebca, 0xb326, 0x4ea5, 0x88, 0x2f, 0x2f, 0xf5, 0xba, 0xe0, 0x21, 0xdd);

#include "d3d8_vtbl.h"

/* ------------------------------------------------------------ objects */
struct d3d8 { const IDirect3D8Vtbl *vt; LONG ref; struct d3d9 *d9; };
struct vs8 { struct vdecl *decl; struct shader *vs; float *consts; UINT const_start, const_count; DWORD *decl_tokens; UINT decl_bytes; };
struct dev8 {
    const IDirect3DDevice8Vtbl *vt;
    LONG ref;
    struct d3d8 *d8;
    struct device *dev;
    UINT base_vertex;
    DWORD cur_vs, cur_ps;               /* D3D8 handles */
    struct vs8 *vs; UINT vs_n;
    struct shader **ps; UINT ps_n;
    struct stateblock **sb; UINT sb_n;
    DWORD rs8[256];                      /* the 8-only render states, shadowed */
};
/* one wrapper shape for every resource: inner is the d3d9 object (referenced) */
struct w8 { const void *vt; LONG ref; void *inner; struct dev8 *dev8; };

static void *w8_new(const void *vt, void *inner, struct dev8 *d)
{
    struct w8 *w;
    if (!inner) return NULL;
    w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    if (!w) { IUnknown_Release((IUnknown *)inner); return NULL; }
    w->vt = vt; w->ref = 1; w->inner = inner; w->dev8 = d;
    InterlockedIncrement(&d->ref);
    return w;
}
#define W8(This) ((struct w8 *)(This))
#define INNER(This, T) ((T *)W8(This)->inner)
static ULONG w8_release(struct w8 *w)
{
    LONG r = InterlockedDecrement(&w->ref);
    if (r == 0) {
        IUnknown_Release((IUnknown *)w->inner);
        dev8_Release((IDirect3DDevice8 *)w->dev8);
        HeapFree(GetProcessHeap(), 0, w);
    }
    return r;
}
#define W8_COMMON(pfx, IFACE, IID_)                                                                          \
HRESULT WINAPI pfx##_QueryInterface(IFACE *This, REFIID riid, void **ppv)                                    \
{                                                                                                            \
    if (!ppv) return E_POINTER;                                                                              \
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_)) { *ppv = This; InterlockedIncrement(&W8(This)->ref); return S_OK; } \
    *ppv = NULL; return E_NOINTERFACE;                                                                       \
}                                                                                                            \
ULONG WINAPI pfx##_AddRef(IFACE *This) { return InterlockedIncrement(&W8(This)->ref); }                     \
ULONG WINAPI pfx##_Release(IFACE *This) { return w8_release(W8(This)); }                                     \
HRESULT WINAPI pfx##_GetDevice(IFACE *This, IDirect3DDevice8 **pp)                                           \
{                                                                                                            \
    if (!pp) return D3DERR_INVALIDCALL;                                                                      \
    *pp = (IDirect3DDevice8 *)W8(This)->dev8; InterlockedIncrement(&W8(This)->dev8->ref); return D3D_OK;     \
}                                                                                                            \
HRESULT WINAPI pfx##_SetPrivateData(IFACE *This, REFGUID g, const void *d, DWORD n, DWORD f) { return D3D_OK; } \
HRESULT WINAPI pfx##_GetPrivateData(IFACE *This, REFGUID g, void *d, DWORD *n) { return D3DERR_NOTFOUND; }  \
HRESULT WINAPI pfx##_FreePrivateData(IFACE *This, REFGUID g) { return D3DERR_NOTFOUND; }
#define W8_RESOURCE(pfx, IFACE, T, RTYPE)                                                                    \
DWORD WINAPI pfx##_SetPriority(IFACE *This, DWORD p) { return ((struct res_hdr *)W8(This)->inner)->priority = p; } \
DWORD WINAPI pfx##_GetPriority(IFACE *This) { return ((struct res_hdr *)W8(This)->inner)->priority; }         \
void WINAPI pfx##_PreLoad(IFACE *This) { }                                                                    \
D3DRESOURCETYPE WINAPI pfx##_GetType(IFACE *This) { return RTYPE; }

static void desc8(const D3DSURFACE_DESC *d, D3DSURFACE_DESC8 *o)
{
    struct level l;
    level_geometry(d->Format, d->Width, d->Height, &l);
    o->Format = d->Format; o->Type = d->Type; o->Usage = d->Usage; o->Pool = d->Pool; o->Size = l.size;
    o->MultiSampleType = d->MultiSampleType; o->Width = d->Width; o->Height = d->Height;
}

/* --- surfaces --- */
W8_COMMON(surf8, IDirect3DSurface8, IID_IDirect3DSurface8)
HRESULT WINAPI surf8_GetContainer(IDirect3DSurface8 *This, REFIID riid, void **pp)
{
    struct surface *s = INNER(This, struct surface);
    if (!pp) return D3DERR_INVALIDCALL;
    if (s->kind == SURF_TEXLEVEL && s->tex) {
        InterlockedIncrement(&s->tex->h.ref);
        *pp = w8_new(s->tex->faces == 6 ? (const void *)&cube8_vtbl : (const void *)&tex8_vtbl, s->tex, W8(This)->dev8);
        return *pp ? D3D_OK : E_OUTOFMEMORY;
    }
    *pp = W8(This)->dev8; InterlockedIncrement(&W8(This)->dev8->ref);
    return D3D_OK;
}
HRESULT WINAPI surf8_GetDesc(IDirect3DSurface8 *This, D3DSURFACE_DESC8 *d) { if (!d) return D3DERR_INVALIDCALL; desc8(&INNER(This, struct surface)->desc, d); return D3D_OK; }
HRESULT WINAPI surf8_LockRect(IDirect3DSurface8 *This, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) { return surf_LockRect(INNER(This, IDirect3DSurface9), lr, rc, f); }
HRESULT WINAPI surf8_UnlockRect(IDirect3DSurface8 *This) { return surf_UnlockRect(INNER(This, IDirect3DSurface9)); }
static IDirect3DSurface8 *wrap_surface(struct dev8 *d, IDirect3DSurface9 *s) { return (IDirect3DSurface8 *)w8_new(&surf8_vtbl, s, d); }

/* --- textures --- */
W8_COMMON(tex8, IDirect3DTexture8, IID_IDirect3DTexture8)
W8_RESOURCE(tex8, IDirect3DTexture8, struct texture, D3DRTYPE_TEXTURE)
DWORD WINAPI tex8_SetLOD(IDirect3DTexture8 *This, DWORD l) { return tex_SetLOD(INNER(This, IDirect3DTexture9), l); }
DWORD WINAPI tex8_GetLOD(IDirect3DTexture8 *This) { return tex_GetLOD(INNER(This, IDirect3DTexture9)); }
DWORD WINAPI tex8_GetLevelCount(IDirect3DTexture8 *This) { return INNER(This, struct texture)->levels; }
HRESULT WINAPI tex8_GetLevelDesc(IDirect3DTexture8 *This, UINT Level, D3DSURFACE_DESC8 *d)
{
    D3DSURFACE_DESC d9;
    HRESULT hr = tex_GetLevelDesc(INNER(This, IDirect3DTexture9), Level, &d9);
    if (SUCCEEDED(hr) && d) desc8(&d9, d);
    return hr;
}
HRESULT WINAPI tex8_GetSurfaceLevel(IDirect3DTexture8 *This, UINT Level, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = tex_GetSurfaceLevel(INNER(This, IDirect3DTexture9), Level, &s);
    *pp = SUCCEEDED(hr) ? wrap_surface(W8(This)->dev8, s) : NULL;
    return hr;
}
HRESULT WINAPI tex8_LockRect(IDirect3DTexture8 *This, UINT l, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) { return tex_LockRect(INNER(This, IDirect3DTexture9), l, lr, rc, f); }
HRESULT WINAPI tex8_UnlockRect(IDirect3DTexture8 *This, UINT l) { return tex_UnlockRect(INNER(This, IDirect3DTexture9), l); }
HRESULT WINAPI tex8_AddDirtyRect(IDirect3DTexture8 *This, const RECT *rc) { return D3D_OK; }

W8_COMMON(cube8, IDirect3DCubeTexture8, IID_IDirect3DCubeTexture8)
W8_RESOURCE(cube8, IDirect3DCubeTexture8, struct texture, D3DRTYPE_CUBETEXTURE)
DWORD WINAPI cube8_SetLOD(IDirect3DCubeTexture8 *This, DWORD l) { return tex_SetLOD(INNER(This, IDirect3DTexture9), l); }
DWORD WINAPI cube8_GetLOD(IDirect3DCubeTexture8 *This) { return tex_GetLOD(INNER(This, IDirect3DTexture9)); }
DWORD WINAPI cube8_GetLevelCount(IDirect3DCubeTexture8 *This) { return INNER(This, struct texture)->levels; }
HRESULT WINAPI cube8_GetLevelDesc(IDirect3DCubeTexture8 *This, UINT Level, D3DSURFACE_DESC8 *d)
{
    D3DSURFACE_DESC d9;
    HRESULT hr = tex_GetLevelDesc(INNER(This, IDirect3DTexture9), Level, &d9);
    if (SUCCEEDED(hr) && d) desc8(&d9, d);
    return hr;
}
HRESULT WINAPI cube8_GetCubeMapSurface(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES face, UINT Level, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = cube_GetCubeMapSurface(INNER(This, IDirect3DCubeTexture9), face, Level, &s);
    *pp = SUCCEEDED(hr) ? wrap_surface(W8(This)->dev8, s) : NULL;
    return hr;
}
HRESULT WINAPI cube8_LockRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT *lr, const RECT *rc, DWORD fl) { return cube_LockRect(INNER(This, IDirect3DCubeTexture9), f, l, lr, rc, fl); }
HRESULT WINAPI cube8_UnlockRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES f, UINT l) { return cube_UnlockRect(INNER(This, IDirect3DCubeTexture9), f, l); }
HRESULT WINAPI cube8_AddDirtyRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES f, const RECT *rc) { return D3D_OK; }

/* --- buffers --- */
W8_COMMON(vb8, IDirect3DVertexBuffer8, IID_IDirect3DVertexBuffer8)
W8_RESOURCE(vb8, IDirect3DVertexBuffer8, struct vbuf, D3DRTYPE_VERTEXBUFFER)
HRESULT WINAPI vb8_Lock(IDirect3DVertexBuffer8 *This, UINT off, UINT size, BYTE **pp, DWORD f) { return vb_Lock(INNER(This, IDirect3DVertexBuffer9), off, size, (void **)pp, f); }
HRESULT WINAPI vb8_Unlock(IDirect3DVertexBuffer8 *This) { return vb_Unlock(INNER(This, IDirect3DVertexBuffer9)); }
HRESULT WINAPI vb8_GetDesc(IDirect3DVertexBuffer8 *This, D3DVERTEXBUFFER_DESC *d) { return vb_GetDesc(INNER(This, IDirect3DVertexBuffer9), d); }
W8_COMMON(ib8, IDirect3DIndexBuffer8, IID_IDirect3DIndexBuffer8)
W8_RESOURCE(ib8, IDirect3DIndexBuffer8, struct ibuf, D3DRTYPE_INDEXBUFFER)
HRESULT WINAPI ib8_Lock(IDirect3DIndexBuffer8 *This, UINT off, UINT size, BYTE **pp, DWORD f) { return ib_Lock(INNER(This, IDirect3DIndexBuffer9), off, size, (void **)pp, f); }
HRESULT WINAPI ib8_Unlock(IDirect3DIndexBuffer8 *This) { return ib_Unlock(INNER(This, IDirect3DIndexBuffer9)); }
HRESULT WINAPI ib8_GetDesc(IDirect3DIndexBuffer8 *This, D3DINDEXBUFFER_DESC *d) { return ib_GetDesc(INNER(This, IDirect3DIndexBuffer9), d); }

/* ------------------------------------------------------------ IDirect3D8 */
HRESULT WINAPI d8_QueryInterface(IDirect3D8 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D8)) { *ppv = This; InterlockedIncrement(&((struct d3d8 *)This)->ref); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
ULONG WINAPI d8_AddRef(IDirect3D8 *This) { return InterlockedIncrement(&((struct d3d8 *)This)->ref); }
ULONG WINAPI d8_Release(IDirect3D8 *This)
{
    struct d3d8 *d = (struct d3d8 *)This;
    LONG r = InterlockedDecrement(&d->ref);
    d3dpt_log("d3d8: IDirect3D8 Release -> %ld", (long)r);
    if (r == 0) { IDirect3D9_Release((IDirect3D9 *)d->d9); HeapFree(GetProcessHeap(), 0, d); }
    return r;
}
#define D9(This) ((IDirect3D9 *)((struct d3d8 *)(This))->d9)
HRESULT WINAPI d8_RegisterSoftwareDevice(IDirect3D8 *This, void *p) { return D3D_OK; }
UINT WINAPI d8_GetAdapterCount(IDirect3D8 *This) { return 1; }
HRESULT WINAPI d8_GetAdapterIdentifier(IDirect3D8 *This, UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *id)
{
    D3DADAPTER_IDENTIFIER9 id9;
    HRESULT hr;
    if (!id) return D3DERR_INVALIDCALL;
    hr = d3d_GetAdapterIdentifier(D9(This), Adapter, 0, &id9);
    if (FAILED(hr)) return hr;
    memset(id, 0, sizeof *id);
    memcpy(id->Driver, id9.Driver, sizeof id->Driver);
    memcpy(id->Description, id9.Description, sizeof id->Description);
    id->DriverVersion = id9.DriverVersion; id->VendorId = id9.VendorId; id->DeviceId = id9.DeviceId;
    id->SubSysId = id9.SubSysId; id->Revision = id9.Revision; id->DeviceIdentifier = id9.DeviceIdentifier; id->WHQLLevel = id9.WHQLLevel;
    return D3D_OK;
}
UINT WINAPI d8_GetAdapterModeCount(IDirect3D8 *This, UINT Adapter) { return d3d_GetAdapterModeCount(D9(This), Adapter, D3DFMT_UNKNOWN); }
HRESULT WINAPI d8_EnumAdapterModes(IDirect3D8 *This, UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode) { return d3d_EnumAdapterModes(D9(This), Adapter, D3DFMT_UNKNOWN, Mode, pMode); }
HRESULT WINAPI d8_GetAdapterDisplayMode(IDirect3D8 *This, UINT Adapter, D3DDISPLAYMODE *pMode) { return d3d_GetAdapterDisplayMode(D9(This), Adapter, pMode); }
HRESULT WINAPI d8_CheckDeviceType(IDirect3D8 *This, UINT A, D3DDEVTYPE T, D3DFORMAT df, D3DFORMAT bf, WINBOOL w) { return d3d_CheckDeviceType(D9(This), A, T, df, bf, w); }
HRESULT WINAPI d8_CheckDeviceFormat(IDirect3D8 *This, UINT A, D3DDEVTYPE T, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT cf) { return d3d_CheckDeviceFormat(D9(This), A, T, af, u, rt, cf); }
HRESULT WINAPI d8_CheckDeviceMultiSampleType(IDirect3D8 *This, UINT A, D3DDEVTYPE T, D3DFORMAT f, WINBOOL w, D3DMULTISAMPLE_TYPE ms) { return d3d_CheckDeviceMultiSampleType(D9(This), A, T, f, w, ms, NULL); }
HRESULT WINAPI d8_CheckDepthStencilMatch(IDirect3D8 *This, UINT A, D3DDEVTYPE T, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) { return d3d_CheckDepthStencilMatch(D9(This), A, T, af, rf, df); }
static void caps8(const D3DCAPS9 *c, D3DCAPS8 *o)
{
    memset(o, 0, sizeof *o);
    o->DeviceType = c->DeviceType; o->AdapterOrdinal = c->AdapterOrdinal;
    o->Caps = c->Caps; o->Caps2 = c->Caps2; o->Caps3 = c->Caps3; o->PresentationIntervals = c->PresentationIntervals;
    o->CursorCaps = c->CursorCaps; o->DevCaps = c->DevCaps; o->PrimitiveMiscCaps = c->PrimitiveMiscCaps; o->RasterCaps = c->RasterCaps;
    o->ZCmpCaps = c->ZCmpCaps; o->SrcBlendCaps = c->SrcBlendCaps; o->DestBlendCaps = c->DestBlendCaps; o->AlphaCmpCaps = c->AlphaCmpCaps;
    o->ShadeCaps = c->ShadeCaps; o->TextureCaps = c->TextureCaps; o->TextureFilterCaps = c->TextureFilterCaps;
    o->CubeTextureFilterCaps = c->CubeTextureFilterCaps; o->VolumeTextureFilterCaps = c->VolumeTextureFilterCaps;
    o->TextureAddressCaps = c->TextureAddressCaps; o->VolumeTextureAddressCaps = c->VolumeTextureAddressCaps; o->LineCaps = c->LineCaps;
    o->MaxTextureWidth = c->MaxTextureWidth; o->MaxTextureHeight = c->MaxTextureHeight; o->MaxVolumeExtent = c->MaxVolumeExtent;
    o->MaxTextureRepeat = c->MaxTextureRepeat; o->MaxTextureAspectRatio = c->MaxTextureAspectRatio; o->MaxAnisotropy = c->MaxAnisotropy;
    o->MaxVertexW = c->MaxVertexW; o->GuardBandLeft = c->GuardBandLeft; o->GuardBandTop = c->GuardBandTop;
    o->GuardBandRight = c->GuardBandRight; o->GuardBandBottom = c->GuardBandBottom; o->ExtentsAdjust = c->ExtentsAdjust;
    o->StencilCaps = c->StencilCaps; o->FVFCaps = c->FVFCaps; o->TextureOpCaps = c->TextureOpCaps;
    o->MaxTextureBlendStages = c->MaxTextureBlendStages; o->MaxSimultaneousTextures = c->MaxSimultaneousTextures;
    o->VertexProcessingCaps = c->VertexProcessingCaps; o->MaxActiveLights = c->MaxActiveLights; o->MaxUserClipPlanes = c->MaxUserClipPlanes;
    o->MaxVertexBlendMatrices = c->MaxVertexBlendMatrices; o->MaxVertexBlendMatrixIndex = c->MaxVertexBlendMatrixIndex;
    o->MaxPointSize = c->MaxPointSize; o->MaxPrimitiveCount = c->MaxPrimitiveCount; o->MaxVertexIndex = c->MaxVertexIndex;
    o->MaxStreams = c->MaxStreams; o->MaxStreamStride = c->MaxStreamStride;
    /* D3D8 tops out at vs_1_1 / ps_1_4 */
    o->VertexShaderVersion = D3DVS_VERSION(1, 1); o->MaxVertexShaderConst = c->MaxVertexShaderConst > 96 ? 96 : c->MaxVertexShaderConst;
    o->PixelShaderVersion = D3DPS_VERSION(1, 4); o->MaxPixelShaderValue = c->PixelShader1xMaxValue;
}
HRESULT WINAPI d8_GetDeviceCaps(IDirect3D8 *This, UINT Adapter, D3DDEVTYPE T, D3DCAPS8 *pCaps)
{
    D3DCAPS9 c;
    HRESULT hr;
    if (!pCaps) return D3DERR_INVALIDCALL;
    hr = d3d_GetDeviceCaps(D9(This), Adapter, T, &c);
    if (SUCCEEDED(hr)) caps8(&c, pCaps);
    return hr;
}
HMONITOR WINAPI d8_GetAdapterMonitor(IDirect3D8 *This, UINT Adapter) { return d3d_GetAdapterMonitor(D9(This), Adapter); }
static void pp9_from_pp8(D3DPRESENT_PARAMETERS *o, const D3DPRESENT_PARAMETERS8 *p)
{
    memset(o, 0, sizeof *o);
    o->BackBufferWidth = p->BackBufferWidth; o->BackBufferHeight = p->BackBufferHeight; o->BackBufferFormat = p->BackBufferFormat;
    o->BackBufferCount = p->BackBufferCount; o->MultiSampleType = p->MultiSampleType;
    o->SwapEffect = (DWORD)p->SwapEffect == D3DSWAPEFFECT_COPY_VSYNC8 ? D3DSWAPEFFECT_COPY : p->SwapEffect;
    o->hDeviceWindow = p->hDeviceWindow; o->Windowed = p->Windowed; o->EnableAutoDepthStencil = p->EnableAutoDepthStencil;
    o->AutoDepthStencilFormat = p->AutoDepthStencilFormat; o->Flags = p->Flags; o->FullScreen_RefreshRateInHz = p->FullScreen_RefreshRateInHz;
    o->PresentationInterval = p->Windowed ? ((DWORD)p->SwapEffect == D3DSWAPEFFECT_COPY_VSYNC8 ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE)
                                          : p->FullScreen_PresentationInterval;
}
static void pp8_from_pp9(D3DPRESENT_PARAMETERS8 *p, const D3DPRESENT_PARAMETERS *o, const D3DPRESENT_PARAMETERS8 *orig)
{
    p->BackBufferWidth = o->BackBufferWidth; p->BackBufferHeight = o->BackBufferHeight; p->BackBufferFormat = o->BackBufferFormat;
    p->BackBufferCount = o->BackBufferCount; p->hDeviceWindow = o->hDeviceWindow;
    (void)orig;
}
HRESULT WINAPI d8_CreateDevice(IDirect3D8 *This, UINT Adapter, D3DDEVTYPE T, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS8 *pp, IDirect3DDevice8 **out)
{
    struct dev8 *d;
    D3DPRESENT_PARAMETERS pp9;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!pp) return D3DERR_INVALIDCALL;
    pp9_from_pp8(&pp9, pp);
    hr = d3d_CreateDevice(D9(This), Adapter, T, focus, flags, &pp9, &dev);
    if (FAILED(hr)) return hr;
    pp8_from_pp9(pp, &pp9, pp);
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) { IDirect3DDevice9_Release(dev); return E_OUTOFMEMORY; }
    d->vt = &dev8_vtbl; d->ref = 1; d->d8 = (struct d3d8 *)This; d->dev = (struct device *)dev;
    InterlockedIncrement(&d->d8->ref);
    d3dpt_log("d3d8: device created over the d3d9 device");
    *out = (IDirect3DDevice8 *)d;
    return D3D_OK;
}

/* ------------------------------------------------------- IDirect3DDevice8 */
#define D8(This) ((struct dev8 *)(This))
#define DEV9(This) ((IDirect3DDevice9 *)D8(This)->dev)
HRESULT WINAPI dev8_QueryInterface(IDirect3DDevice8 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DDevice8)) { *ppv = This; InterlockedIncrement(&D8(This)->ref); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
ULONG WINAPI dev8_AddRef(IDirect3DDevice8 *This) { return InterlockedIncrement(&D8(This)->ref); }
static void vs8_free(struct vs8 *v)
{
    if (v->decl) IDirect3DVertexDeclaration9_Release((IDirect3DVertexDeclaration9 *)v->decl);
    if (v->vs) IDirect3DVertexShader9_Release((IDirect3DVertexShader9 *)v->vs);
    HeapFree(GetProcessHeap(), 0, v->consts);
    HeapFree(GetProcessHeap(), 0, v->decl_tokens);
    memset(v, 0, sizeof *v);
}
ULONG WINAPI dev8_Release(IDirect3DDevice8 *This)
{
    struct dev8 *d = D8(This);
    LONG r = InterlockedDecrement(&d->ref);
    if (r < 2) d3dpt_log("d3d8: device Release -> %ld", (long)r);
    if (r == 0) {
        UINT i;
        d3dpt_log("d3d8: device teardown: %u vs, %u ps, %u sb slots", d->vs_n, d->ps_n, d->sb_n);
        for (i = 0; i < d->vs_n; i++) vs8_free(&d->vs[i]);
        for (i = 0; i < d->ps_n; i++) if (d->ps[i]) IDirect3DPixelShader9_Release((IDirect3DPixelShader9 *)d->ps[i]);
        for (i = 0; i < d->sb_n; i++) if (d->sb[i]) IDirect3DStateBlock9_Release((IDirect3DStateBlock9 *)d->sb[i]);
        HeapFree(GetProcessHeap(), 0, d->vs); HeapFree(GetProcessHeap(), 0, d->ps); HeapFree(GetProcessHeap(), 0, d->sb);
        d3dpt_log("d3d8: device teardown: releasing the d3d9 device");
        IDirect3DDevice9_Release(DEV9(This));
        d3dpt_log("d3d8: device teardown: done");
        d8_Release((IDirect3D8 *)d->d8);
        HeapFree(GetProcessHeap(), 0, d);
    }
    return r;
}
HRESULT WINAPI dev8_TestCooperativeLevel(IDirect3DDevice8 *This) { return dev_TestCooperativeLevel(DEV9(This)); }
UINT WINAPI dev8_GetAvailableTextureMem(IDirect3DDevice8 *This) { return dev_GetAvailableTextureMem(DEV9(This)); }
HRESULT WINAPI dev8_ResourceManagerDiscardBytes(IDirect3DDevice8 *This, DWORD b) { return D3D_OK; }
HRESULT WINAPI dev8_GetDirect3D(IDirect3DDevice8 *This, IDirect3D8 **pp) { if (!pp) return D3DERR_INVALIDCALL; *pp = (IDirect3D8 *)D8(This)->d8; InterlockedIncrement(&D8(This)->d8->ref); return D3D_OK; }
HRESULT WINAPI dev8_GetDeviceCaps(IDirect3DDevice8 *This, D3DCAPS8 *c) { return d8_GetDeviceCaps((IDirect3D8 *)D8(This)->d8, 0, D3DDEVTYPE_HAL, c); }
HRESULT WINAPI dev8_GetDisplayMode(IDirect3DDevice8 *This, D3DDISPLAYMODE *m) { return dev_GetDisplayMode(DEV9(This), 0, m); }
HRESULT WINAPI dev8_GetCreationParameters(IDirect3DDevice8 *This, D3DDEVICE_CREATION_PARAMETERS *p) { return dev_GetCreationParameters(DEV9(This), p); }
HRESULT WINAPI dev8_SetCursorProperties(IDirect3DDevice8 *This, UINT x, UINT y, IDirect3DSurface8 *s) { return D3D_OK; }
void WINAPI dev8_SetCursorPosition(IDirect3DDevice8 *This, UINT x, UINT y, DWORD f) { dev_SetCursorPosition(DEV9(This), (int)x, (int)y, f); }
WINBOOL WINAPI dev8_ShowCursor(IDirect3DDevice8 *This, WINBOOL b) { return dev_ShowCursor(DEV9(This), b); }
HRESULT WINAPI dev8_Reset(IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS8 *pp)
{
    D3DPRESENT_PARAMETERS pp9;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    pp9_from_pp8(&pp9, pp);
    hr = dev_Reset(DEV9(This), &pp9);
    if (SUCCEEDED(hr)) pp8_from_pp9(pp, &pp9, pp);
    return hr;
}
HRESULT WINAPI dev8_Present(IDirect3DDevice8 *This, const RECT *s, const RECT *d, HWND w, const RGNDATA *r) { return dev_Present(DEV9(This), s, d, w, r); }
HRESULT WINAPI dev8_GetBackBuffer(IDirect3DDevice8 *This, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetBackBuffer(DEV9(This), 0, i, t, &s);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateTexture(IDirect3DDevice8 *This, UINT W, UINT H, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DTexture8 **pp)
{
    IDirect3DTexture9 *t = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateTexture(DEV9(This), W, H, L, U, F, P, &t, NULL);
    *pp = SUCCEEDED(hr) ? (IDirect3DTexture8 *)w8_new(&tex8_vtbl, t, D8(This)) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateCubeTexture(IDirect3DDevice8 *This, UINT E, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DCubeTexture8 **pp)
{
    IDirect3DCubeTexture9 *t = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateCubeTexture(DEV9(This), E, L, U, F, P, &t, NULL);
    *pp = SUCCEEDED(hr) ? (IDirect3DCubeTexture8 *)w8_new(&cube8_vtbl, t, D8(This)) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateVertexBuffer(IDirect3DDevice8 *This, UINT L, DWORD U, DWORD FVF, D3DPOOL P, IDirect3DVertexBuffer8 **pp)
{
    IDirect3DVertexBuffer9 *b = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateVertexBuffer(DEV9(This), L, U, FVF, P, &b, NULL);
    *pp = SUCCEEDED(hr) ? (IDirect3DVertexBuffer8 *)w8_new(&vb8_vtbl, b, D8(This)) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateIndexBuffer(IDirect3DDevice8 *This, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DIndexBuffer8 **pp)
{
    IDirect3DIndexBuffer9 *b = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateIndexBuffer(DEV9(This), L, U, F, P, &b, NULL);
    *pp = SUCCEEDED(hr) ? (IDirect3DIndexBuffer8 *)w8_new(&ib8_vtbl, b, D8(This)) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateRenderTarget(IDirect3DDevice8 *This, UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, WINBOOL L, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateRenderTarget(DEV9(This), W, H, F, MS, 0, L, &s, NULL);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateDepthStencilSurface(IDirect3DDevice8 *This, UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateDepthStencilSurface(DEV9(This), W, H, F, MS, 0, FALSE, &s, NULL);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CreateImageSurface(IDirect3DDevice8 *This, UINT W, UINT H, D3DFORMAT F, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_CreateOffscreenPlainSurface(DEV9(This), W, H, F, D3DPOOL_SYSTEMMEM, &s, NULL);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_CopyRects(IDirect3DDevice8 *This, IDirect3DSurface8 *pSrc, const RECT *rects, UINT n, IDirect3DSurface8 *pDst, const POINT *pts)
{
    struct surface *src, *dst;
    UINT i;
    if (!pSrc || !pDst) return D3DERR_INVALIDCALL;
    src = INNER(pSrc, struct surface); dst = INNER(pDst, struct surface);
    if (src->desc.Format != dst->desc.Format) return D3DERR_INVALIDCALL;
    if (!n) {
        /* whole surface */
        if (src->h.handle && dst->kind == SURF_SYSMEM) return dev_GetRenderTargetData(DEV9(This), (IDirect3DSurface9 *)src, (IDirect3DSurface9 *)dst);
        if (!src->h.handle && dst->h.handle) return dev_UpdateSurface(DEV9(This), (IDirect3DSurface9 *)src, NULL, (IDirect3DSurface9 *)dst, NULL);
        if (src->h.handle && dst->h.handle) return dev_StretchRect(DEV9(This), (IDirect3DSurface9 *)src, NULL, (IDirect3DSurface9 *)dst, NULL, D3DTEXF_NONE);
        if (src->mem && dst->mem && src->desc.Width == dst->desc.Width && src->desc.Height == dst->desc.Height) {
            UINT y, bw, bh, bytes;
            fmt_block(src->desc.Format, &bw, &bh, &bytes);
            for (y = 0; y < (dst->desc.Height + bh - 1) / bh; y++) memcpy(dst->mem + y * dst->pitch, src->mem + y * src->pitch, src->pitch < dst->pitch ? src->pitch : dst->pitch);
            return D3D_OK;
        }
        return D3DERR_INVALIDCALL;
    }
    for (i = 0; i < n; i++) {
        RECT r = rects[i];
        POINT p = pts ? pts[i] : (POINT){ 0, 0 };
        RECT dr = { p.x, p.y, p.x + (r.right - r.left), p.y + (r.bottom - r.top) };
        HRESULT hr;
        if (!src->h.handle && dst->h.handle) hr = dev_UpdateSurface(DEV9(This), (IDirect3DSurface9 *)src, &r, (IDirect3DSurface9 *)dst, &p);
        else if (src->h.handle && dst->h.handle) hr = dev_StretchRect(DEV9(This), (IDirect3DSurface9 *)src, &r, (IDirect3DSurface9 *)dst, &dr, D3DTEXF_NONE);
        else { D3DPT_STUB("CopyRects with rects into system memory"); return D3DERR_INVALIDCALL; }
        if (FAILED(hr)) return hr;
    }
    return D3D_OK;
}
HRESULT WINAPI dev8_UpdateTexture(IDirect3DDevice8 *This, IDirect3DBaseTexture8 *s, IDirect3DBaseTexture8 *d)
{
    if (!s || !d) return D3DERR_INVALIDCALL;
    return dev_UpdateTexture(DEV9(This), INNER(s, IDirect3DBaseTexture9), INNER(d, IDirect3DBaseTexture9));
}
HRESULT WINAPI dev8_SetRenderTarget(IDirect3DDevice8 *This, IDirect3DSurface8 *rt, IDirect3DSurface8 *ds)
{
    HRESULT hr = D3D_OK;
    if (rt) hr = dev_SetRenderTarget(DEV9(This), 0, INNER(rt, IDirect3DSurface9));
    if (SUCCEEDED(hr)) hr = dev_SetDepthStencilSurface(DEV9(This), ds ? INNER(ds, IDirect3DSurface9) : NULL);
    return hr;
}
HRESULT WINAPI dev8_GetRenderTarget(IDirect3DDevice8 *This, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetRenderTarget(DEV9(This), 0, &s);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_GetDepthStencilSurface(IDirect3DDevice8 *This, IDirect3DSurface8 **pp)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetDepthStencilSurface(DEV9(This), &s);
    *pp = SUCCEEDED(hr) ? wrap_surface(D8(This), s) : NULL;
    return hr;
}
HRESULT WINAPI dev8_BeginScene(IDirect3DDevice8 *This) { return dev_BeginScene(DEV9(This)); }
HRESULT WINAPI dev8_EndScene(IDirect3DDevice8 *This) { return dev_EndScene(DEV9(This)); }
HRESULT WINAPI dev8_Clear(IDirect3DDevice8 *This, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s) { return dev_Clear(DEV9(This), n, r, f, c, z, s); }
HRESULT WINAPI dev8_SetTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) { return dev_SetTransform(DEV9(This), s, m); }
HRESULT WINAPI dev8_GetTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE s, D3DMATRIX *m) { return dev_GetTransform(DEV9(This), s, m); }
HRESULT WINAPI dev8_MultiplyTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) { return dev_MultiplyTransform(DEV9(This), s, m); }
HRESULT WINAPI dev8_SetViewport(IDirect3DDevice8 *This, const D3DVIEWPORT9 *v) { return dev_SetViewport(DEV9(This), v); }
HRESULT WINAPI dev8_GetViewport(IDirect3DDevice8 *This, D3DVIEWPORT9 *v) { return dev_GetViewport(DEV9(This), v); }
HRESULT WINAPI dev8_SetMaterial(IDirect3DDevice8 *This, const D3DMATERIAL9 *m) { return dev_SetMaterial(DEV9(This), m); }
HRESULT WINAPI dev8_GetMaterial(IDirect3DDevice8 *This, D3DMATERIAL9 *m) { return dev_GetMaterial(DEV9(This), m); }
HRESULT WINAPI dev8_SetLight(IDirect3DDevice8 *This, DWORD i, const D3DLIGHT9 *l) { return dev_SetLight(DEV9(This), i, l); }
HRESULT WINAPI dev8_GetLight(IDirect3DDevice8 *This, DWORD i, D3DLIGHT9 *l) { return dev_GetLight(DEV9(This), i, l); }
HRESULT WINAPI dev8_LightEnable(IDirect3DDevice8 *This, DWORD i, WINBOOL e) { return dev_LightEnable(DEV9(This), i, e); }
HRESULT WINAPI dev8_GetLightEnable(IDirect3DDevice8 *This, DWORD i, WINBOOL *e) { return dev_GetLightEnable(DEV9(This), i, e); }
HRESULT WINAPI dev8_SetClipPlane(IDirect3DDevice8 *This, DWORD i, const float *p) { return dev_SetClipPlane(DEV9(This), i, p); }
HRESULT WINAPI dev8_GetClipPlane(IDirect3DDevice8 *This, DWORD i, float *p) { return dev_GetClipPlane(DEV9(This), i, p); }
HRESULT WINAPI dev8_SetRenderState(IDirect3DDevice8 *This, D3DRENDERSTATETYPE s, DWORD v)
{
    struct dev8 *d = D8(This);
    if ((DWORD)s < 256) d->rs8[s] = v;
    switch ((DWORD)s) {
    case D3DRS8_ZBIAS: {
        float bias = (float)v * -0.000005f;      /* the d3d8to9 mapping */
        DWORD bits; memcpy(&bits, &bias, 4);
        return dev_SetRenderState(DEV9(This), D3DRS_DEPTHBIAS, bits);
    }
    case D3DRS8_EDGEANTIALIAS: return dev_SetRenderState(DEV9(This), D3DRS_ANTIALIASEDLINEENABLE, v);
    case D3DRS8_SOFTWAREVERTEXPROCESSING: return dev_SetSoftwareVertexProcessing(DEV9(This), v);
    case D3DRS8_LINEPATTERN: case D3DRS8_ZVISIBLE: case D3DRS8_PATCHSEGMENTS: return D3D_OK;
    default: return dev_SetRenderState(DEV9(This), s, v);
    }
}
HRESULT WINAPI dev8_GetRenderState(IDirect3DDevice8 *This, D3DRENDERSTATETYPE s, DWORD *v)
{
    switch ((DWORD)s) {
    case D3DRS8_ZBIAS: case D3DRS8_EDGEANTIALIAS: case D3DRS8_SOFTWAREVERTEXPROCESSING: case D3DRS8_LINEPATTERN: case D3DRS8_ZVISIBLE: case D3DRS8_PATCHSEGMENTS:
        if (!v || (DWORD)s >= 256) return D3DERR_INVALIDCALL;
        *v = D8(This)->rs8[s];
        return D3D_OK;
    default: return dev_GetRenderState(DEV9(This), s, v);
    }
}
/* state blocks: DWORD tokens into a table of d3d9 blocks */
static DWORD sb8_add(struct dev8 *d, IDirect3DStateBlock9 *b)
{
    UINT i;
    for (i = 0; i < d->sb_n; i++) if (!d->sb[i]) { d->sb[i] = (struct stateblock *)b; return i + 1; }
    {
        struct stateblock **n = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (d->sb_n + 16) * sizeof *n);
        if (!n) return 0;
        if (d->sb) { memcpy(n, d->sb, d->sb_n * sizeof *n); HeapFree(GetProcessHeap(), 0, d->sb); }
        d->sb = n; d->sb[d->sb_n] = (struct stateblock *)b; d->sb_n += 16;
        return d->sb_n - 16 + 1;
    }
}
static IDirect3DStateBlock9 *sb8_get(struct dev8 *d, DWORD tok) { return tok && tok <= d->sb_n ? (IDirect3DStateBlock9 *)d->sb[tok - 1] : NULL; }
HRESULT WINAPI dev8_BeginStateBlock(IDirect3DDevice8 *This) { return dev_BeginStateBlock(DEV9(This)); }
HRESULT WINAPI dev8_EndStateBlock(IDirect3DDevice8 *This, DWORD *tok)
{
    IDirect3DStateBlock9 *b = NULL;
    HRESULT hr;
    if (!tok) return D3DERR_INVALIDCALL;
    hr = dev_EndStateBlock(DEV9(This), &b);
    if (FAILED(hr)) return hr;
    *tok = sb8_add(D8(This), b);
    return *tok ? D3D_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI dev8_ApplyStateBlock(IDirect3DDevice8 *This, DWORD tok) { IDirect3DStateBlock9 *b = sb8_get(D8(This), tok); return b ? sb_Apply(b) : D3DERR_INVALIDCALL; }
HRESULT WINAPI dev8_CaptureStateBlock(IDirect3DDevice8 *This, DWORD tok) { IDirect3DStateBlock9 *b = sb8_get(D8(This), tok); return b ? sb_Capture(b) : D3DERR_INVALIDCALL; }
HRESULT WINAPI dev8_DeleteStateBlock(IDirect3DDevice8 *This, DWORD tok)
{
    IDirect3DStateBlock9 *b = sb8_get(D8(This), tok);
    if (!b) return D3DERR_INVALIDCALL;
    sb_Release(b);
    D8(This)->sb[tok - 1] = NULL;
    return D3D_OK;
}
HRESULT WINAPI dev8_CreateStateBlock(IDirect3DDevice8 *This, D3DSTATEBLOCKTYPE t, DWORD *tok)
{
    IDirect3DStateBlock9 *b = NULL;
    HRESULT hr;
    if (!tok) return D3DERR_INVALIDCALL;
    hr = dev_CreateStateBlock(DEV9(This), t, &b);
    if (FAILED(hr)) return hr;
    *tok = sb8_add(D8(This), b);
    return *tok ? D3D_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI dev8_GetTexture(IDirect3DDevice8 *This, DWORD stage, IDirect3DBaseTexture8 **pp)
{
    IDirect3DBaseTexture9 *t = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetTexture(DEV9(This), stage, &t);
    *pp = NULL;
    if (SUCCEEDED(hr) && t) *pp = (IDirect3DBaseTexture8 *)w8_new(((struct texture *)t)->faces == 6 ? (const void *)&cube8_vtbl : (const void *)&tex8_vtbl, t, D8(This));
    return hr;
}
HRESULT WINAPI dev8_SetTexture(IDirect3DDevice8 *This, DWORD stage, IDirect3DBaseTexture8 *t) { return dev_SetTexture(DEV9(This), stage, t ? INNER(t, IDirect3DBaseTexture9) : NULL); }
static int tss8_to_samp(DWORD type)
{
    switch (type) {
    case 13: return D3DSAMP_ADDRESSU; case 14: return D3DSAMP_ADDRESSV; case 15: return D3DSAMP_BORDERCOLOR;
    case 16: return D3DSAMP_MAGFILTER; case 17: return D3DSAMP_MINFILTER; case 18: return D3DSAMP_MIPFILTER;
    case 19: return D3DSAMP_MIPMAPLODBIAS; case 20: return D3DSAMP_MAXMIPLEVEL; case 21: return D3DSAMP_MAXANISOTROPY;
    case 25: return D3DSAMP_ADDRESSW;
    default: return 0;
    }
}
HRESULT WINAPI dev8_GetTextureStageState(IDirect3DDevice8 *This, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *v)
{
    int s = tss8_to_samp(type);
    return s ? dev_GetSamplerState(DEV9(This), stage, (D3DSAMPLERSTATETYPE)s, v) : dev_GetTextureStageState(DEV9(This), stage, type, v);
}
HRESULT WINAPI dev8_SetTextureStageState(IDirect3DDevice8 *This, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD v)
{
    int s = tss8_to_samp(type);
    return s ? dev_SetSamplerState(DEV9(This), stage, (D3DSAMPLERSTATETYPE)s, v) : dev_SetTextureStageState(DEV9(This), stage, type, v);
}
HRESULT WINAPI dev8_ValidateDevice(IDirect3DDevice8 *This, DWORD *n) { return dev_ValidateDevice(DEV9(This), n); }
HRESULT WINAPI dev8_GetInfo(IDirect3DDevice8 *This, DWORD id, void *p, DWORD n) { return S_FALSE; }
HRESULT WINAPI dev8_DrawPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT s, UINT n) { return dev_DrawPrimitive(DEV9(This), t, s, n); }
HRESULT WINAPI dev8_DrawIndexedPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT si, UINT pc) { return dev_DrawIndexedPrimitive(DEV9(This), t, (INT)D8(This)->base_vertex, mi, nv, si, pc); }
HRESULT WINAPI dev8_DrawPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT n, const void *d, UINT s) { return dev_DrawPrimitiveUP(DEV9(This), t, n, d, s); }
HRESULT WINAPI dev8_DrawIndexedPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *id, D3DFORMAT f, const void *vd, UINT s) { return dev_DrawIndexedPrimitiveUP(DEV9(This), t, mi, nv, pc, id, f, vd, s); }

/* --- vertex shaders: D3DVSD_* declaration tokens -> elements + dcl instructions --- */
static const struct { BYTE usage, index; } vsde_usage[17] = {
    { D3DDECLUSAGE_POSITION, 0 }, { D3DDECLUSAGE_BLENDWEIGHT, 0 }, { D3DDECLUSAGE_BLENDINDICES, 0 }, { D3DDECLUSAGE_NORMAL, 0 },
    { D3DDECLUSAGE_PSIZE, 0 }, { D3DDECLUSAGE_COLOR, 0 }, { D3DDECLUSAGE_COLOR, 1 }, { D3DDECLUSAGE_TEXCOORD, 0 },
    { D3DDECLUSAGE_TEXCOORD, 1 }, { D3DDECLUSAGE_TEXCOORD, 2 }, { D3DDECLUSAGE_TEXCOORD, 3 }, { D3DDECLUSAGE_TEXCOORD, 4 },
    { D3DDECLUSAGE_TEXCOORD, 5 }, { D3DDECLUSAGE_TEXCOORD, 6 }, { D3DDECLUSAGE_TEXCOORD, 7 }, { D3DDECLUSAGE_POSITION, 1 }, { D3DDECLUSAGE_NORMAL, 1 },
};
static const BYTE vsdt_size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
static UINT vsd_tokens(const DWORD *d)
{
    UINT i = 0;
    while (d[i] != 0xFFFFFFFF) {
        DWORD type = (d[i] >> 29) & 7;
        if (type == 4) i += 1 + 4 * ((d[i] >> 25) & 0xF); else i++;
        if (i > 4096) return 0;
    }
    return i + 1;
}
/* returns the element count (END included); regs: bitmask of registers used; consts appended */
static int vsd_convert(const DWORD *d, D3DVERTEXELEMENT9 *el, UINT *regs, float **consts, UINT *cstart, UINT *ccount)
{
    UINT i = 0, n = 0, stream = 0, offset = 0;
    *regs = 0; *consts = NULL; *cstart = *ccount = 0;
    for (;;) {
        DWORD t = d[i];
        DWORD type = (t >> 29) & 7;
        if (t == 0xFFFFFFFF) break;
        if (type == 1) { stream = t & 0xF; offset = 0; i++; }                 /* D3DVSD_STREAM */
        else if (type == 2) {
            if (t & (1u << 28)) { offset += 4 * ((t >> 16) & 0xF); i++; continue; }   /* D3DVSD_SKIP */
            {
                DWORD reg = t & 0x1F, dt = (t >> 16) & 0xF;
                if (reg > 16 || dt > 7 || n >= 64) return -1;
                el[n].Stream = (WORD)stream; el[n].Offset = (WORD)offset; el[n].Type = (BYTE)dt; el[n].Method = D3DDECLMETHOD_DEFAULT;
                el[n].Usage = vsde_usage[reg].usage; el[n].UsageIndex = vsde_usage[reg].index;
                n++; offset += vsdt_size[dt]; *regs |= 1u << reg;
                i++;
            }
        } else if (type == 4) {                                              /* D3DVSD_CONST: count float4s from register */
            UINT cnt = (t >> 25) & 0xF, reg = t & 0x7F, j;
            float *c = HeapAlloc(GetProcessHeap(), 0, (*ccount + cnt) * 16);
            if (!c) return -1;
            if (*consts) { memcpy(c, *consts, *ccount * 16); HeapFree(GetProcessHeap(), 0, *consts); }
            else *cstart = reg;
            for (j = 0; j < cnt * 4; j++) memcpy(&c[*ccount * 4 + j], &d[i + 1 + j], 4);
            *consts = c; *ccount += cnt;
            i += 1 + cnt * 4;
        } else i++;                                                          /* tessellator and unknown tokens */
        if (i > 4096) return -1;
    }
    el[n].Stream = 0xFF; el[n].Offset = 0; el[n].Type = D3DDECLTYPE_UNUSED; el[n].Method = 0; el[n].Usage = 0; el[n].UsageIndex = 0;
    return (int)n + 1;
}
HRESULT WINAPI dev8_CreateVertexShader(IDirect3DDevice8 *This, const DWORD *decl, const DWORD *func, DWORD *handle, DWORD usage)
{
    struct dev8 *d = D8(This);
    D3DVERTEXELEMENT9 el[65];
    UINT regs, i, slot;
    struct vs8 v;
    HRESULT hr;
    int n;
    if (!handle) return D3DERR_INVALIDCALL;
    *handle = 0;
    if (!decl) return D3DERR_INVALIDCALL;
    memset(&v, 0, sizeof v);
    n = vsd_convert(decl, el, &regs, &v.consts, &v.const_start, &v.const_count);
    if (n < 0) return D3DERR_INVALIDCALL;
    hr = dev_CreateVertexDeclaration(DEV9(This), el, (IDirect3DVertexDeclaration9 **)&v.decl);
    if (FAILED(hr)) { HeapFree(GetProcessHeap(), 0, v.consts); return hr; }
    if (func) {
        /* prepend a dcl per input register the declaration feeds (D3D9 wants them, D3D8 did not have them) */
        UINT bytes = shader_bytes(func), extra = 0, w = 0;
        DWORD *code;
        if (!bytes) { vs8_free(&v); return D3DERR_INVALIDCALL; }
        for (i = 0; i <= 16; i++) if (regs & (1u << i)) extra += 3;
        code = HeapAlloc(GetProcessHeap(), 0, bytes + extra * 4);
        if (!code) { vs8_free(&v); return E_OUTOFMEMORY; }
        code[w++] = func[0];
        for (i = 0; i <= 16; i++) if (regs & (1u << i)) {
            code[w++] = 0x0000001F;
            code[w++] = 0x80000000u | vsde_usage[i].usage | ((DWORD)vsde_usage[i].index << 16);
            code[w++] = 0x900F0000u | i;
        }
        memcpy(&code[w], &func[1], bytes - 4);
        hr = dev_CreateVertexShader(DEV9(This), code, (IDirect3DVertexShader9 **)&v.vs);
        HeapFree(GetProcessHeap(), 0, code);
        if (FAILED(hr)) { vs8_free(&v); return hr; }
    }
    v.decl_bytes = vsd_tokens(decl) * 4;
    v.decl_tokens = HeapAlloc(GetProcessHeap(), 0, v.decl_bytes);
    if (v.decl_tokens) memcpy(v.decl_tokens, decl, v.decl_bytes);
    for (slot = 0; slot < d->vs_n; slot++) if (!d->vs[slot].decl) break;
    if (slot == d->vs_n) {
        struct vs8 *nv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (d->vs_n + 16) * sizeof *nv);
        if (!nv) { vs8_free(&v); return E_OUTOFMEMORY; }
        if (d->vs) { memcpy(nv, d->vs, d->vs_n * sizeof *nv); HeapFree(GetProcessHeap(), 0, d->vs); }
        d->vs = nv; d->vs_n += 16;
    }
    d->vs[slot] = v;
    *handle = SHADER_HANDLE_BIT | (slot + 1);
    d3dpt_log("d3d8: CreateVertexShader -> handle %08lx (%d elements, %s function, %u constants)", (unsigned long)*handle, n - 1, func ? "with" : "no", v.const_count);
    return D3D_OK;
}
static struct vs8 *vs8_get(struct dev8 *d, DWORD h)
{
    UINT i = (h & ~SHADER_HANDLE_BIT);
    if (!(h & SHADER_HANDLE_BIT) || !i || i > d->vs_n || !d->vs[i - 1].decl) return NULL;
    return &d->vs[i - 1];
}
HRESULT WINAPI dev8_SetVertexShader(IDirect3DDevice8 *This, DWORD h)
{
    struct dev8 *d = D8(This);
    if (h & SHADER_HANDLE_BIT) {
        struct vs8 *v = vs8_get(d, h);
        if (!v) return D3DERR_INVALIDCALL;
        dev_SetVertexDeclaration(DEV9(This), (IDirect3DVertexDeclaration9 *)v->decl);
        dev_SetVertexShader(DEV9(This), (IDirect3DVertexShader9 *)v->vs);
        if (v->const_count) dev_SetVertexShaderConstantF(DEV9(This), v->const_start, v->consts, v->const_count);
    } else {
        dev_SetVertexShader(DEV9(This), NULL);
        dev_SetFVF(DEV9(This), h);
    }
    d->cur_vs = h;
    return D3D_OK;
}
HRESULT WINAPI dev8_GetVertexShader(IDirect3DDevice8 *This, DWORD *h) { if (!h) return D3DERR_INVALIDCALL; *h = D8(This)->cur_vs; return D3D_OK; }
HRESULT WINAPI dev8_DeleteVertexShader(IDirect3DDevice8 *This, DWORD h)
{
    struct vs8 *v = vs8_get(D8(This), h);
    if (!v) return D3DERR_INVALIDCALL;
    if (D8(This)->cur_vs == h) { dev_SetVertexShader(DEV9(This), NULL); dev_SetVertexDeclaration(DEV9(This), NULL); D8(This)->cur_vs = 0; }
    vs8_free(v);
    return D3D_OK;
}
HRESULT WINAPI dev8_SetVertexShaderConstant(IDirect3DDevice8 *This, DWORD reg, const void *data, DWORD n) { return dev_SetVertexShaderConstantF(DEV9(This), reg, data, n); }
HRESULT WINAPI dev8_GetVertexShaderConstant(IDirect3DDevice8 *This, DWORD reg, void *data, DWORD n) { return dev_GetVertexShaderConstantF(DEV9(This), reg, data, n); }
HRESULT WINAPI dev8_GetVertexShaderDeclaration(IDirect3DDevice8 *This, DWORD h, void *data, DWORD *size)
{
    struct vs8 *v = vs8_get(D8(This), h);
    if (!v || !size) return D3DERR_INVALIDCALL;
    if (data) { if (*size < v->decl_bytes) return D3DERR_INVALIDCALL; memcpy(data, v->decl_tokens, v->decl_bytes); }
    *size = v->decl_bytes;
    return D3D_OK;
}
HRESULT WINAPI dev8_GetVertexShaderFunction(IDirect3DDevice8 *This, DWORD h, void *data, DWORD *size)
{
    struct vs8 *v = vs8_get(D8(This), h);
    if (!v || !size) return D3DERR_INVALIDCALL;
    if (!v->vs) { *size = 0; return D3D_OK; }
    return vs_GetFunction((IDirect3DVertexShader9 *)v->vs, data, (UINT *)size);
}
HRESULT WINAPI dev8_SetStreamSource(IDirect3DDevice8 *This, UINT n, IDirect3DVertexBuffer8 *vb, UINT stride) { return dev_SetStreamSource(DEV9(This), n, vb ? INNER(vb, IDirect3DVertexBuffer9) : NULL, 0, stride); }
HRESULT WINAPI dev8_GetStreamSource(IDirect3DDevice8 *This, UINT n, IDirect3DVertexBuffer8 **pp, UINT *stride)
{
    IDirect3DVertexBuffer9 *b = NULL;
    UINT off;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetStreamSource(DEV9(This), n, &b, &off, stride);
    *pp = (SUCCEEDED(hr) && b) ? (IDirect3DVertexBuffer8 *)w8_new(&vb8_vtbl, b, D8(This)) : NULL;
    return hr;
}
HRESULT WINAPI dev8_SetIndices(IDirect3DDevice8 *This, IDirect3DIndexBuffer8 *ib, UINT base)
{
    D8(This)->base_vertex = base;
    return dev_SetIndices(DEV9(This), ib ? INNER(ib, IDirect3DIndexBuffer9) : NULL);
}
HRESULT WINAPI dev8_GetIndices(IDirect3DDevice8 *This, IDirect3DIndexBuffer8 **pp, UINT *base)
{
    IDirect3DIndexBuffer9 *b = NULL;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    hr = dev_GetIndices(DEV9(This), &b);
    *pp = (SUCCEEDED(hr) && b) ? (IDirect3DIndexBuffer8 *)w8_new(&ib8_vtbl, b, D8(This)) : NULL;
    if (base) *base = D8(This)->base_vertex;
    return hr;
}
HRESULT WINAPI dev8_CreatePixelShader(IDirect3DDevice8 *This, const DWORD *func, DWORD *handle)
{
    struct dev8 *d = D8(This);
    IDirect3DPixelShader9 *ps = NULL;
    UINT slot;
    HRESULT hr;
    if (!handle) return D3DERR_INVALIDCALL;
    *handle = 0;
    hr = dev_CreatePixelShader(DEV9(This), func, &ps);
    if (FAILED(hr)) return hr;
    for (slot = 0; slot < d->ps_n; slot++) if (!d->ps[slot]) break;
    if (slot == d->ps_n) {
        struct shader **np = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (d->ps_n + 16) * sizeof *np);
        if (!np) { IDirect3DPixelShader9_Release(ps); return E_OUTOFMEMORY; }
        if (d->ps) { memcpy(np, d->ps, d->ps_n * sizeof *np); HeapFree(GetProcessHeap(), 0, d->ps); }
        d->ps = np; d->ps_n += 16;
    }
    d->ps[slot] = (struct shader *)ps;
    *handle = SHADER_HANDLE_BIT | (slot + 1);
    return D3D_OK;
}
static IDirect3DPixelShader9 *ps8_get(struct dev8 *d, DWORD h)
{
    UINT i = h & ~SHADER_HANDLE_BIT;
    if (!(h & SHADER_HANDLE_BIT) || !i || i > d->ps_n) return NULL;
    return (IDirect3DPixelShader9 *)d->ps[i - 1];
}
HRESULT WINAPI dev8_SetPixelShader(IDirect3DDevice8 *This, DWORD h)
{
    IDirect3DPixelShader9 *ps = NULL;
    if (h && !(ps = ps8_get(D8(This), h))) return D3DERR_INVALIDCALL;
    D8(This)->cur_ps = h;
    return dev_SetPixelShader(DEV9(This), ps);
}
HRESULT WINAPI dev8_GetPixelShader(IDirect3DDevice8 *This, DWORD *h) { if (!h) return D3DERR_INVALIDCALL; *h = D8(This)->cur_ps; return D3D_OK; }
HRESULT WINAPI dev8_DeletePixelShader(IDirect3DDevice8 *This, DWORD h)
{
    IDirect3DPixelShader9 *ps = ps8_get(D8(This), h);
    if (!ps) return D3DERR_INVALIDCALL;
    if (D8(This)->cur_ps == h) { dev_SetPixelShader(DEV9(This), NULL); D8(This)->cur_ps = 0; }
    IDirect3DPixelShader9_Release(ps);
    D8(This)->ps[(h & ~SHADER_HANDLE_BIT) - 1] = NULL;
    return D3D_OK;
}
HRESULT WINAPI dev8_SetPixelShaderConstant(IDirect3DDevice8 *This, DWORD reg, const void *data, DWORD n) { return dev_SetPixelShaderConstantF(DEV9(This), reg, data, n); }
HRESULT WINAPI dev8_GetPixelShaderConstant(IDirect3DDevice8 *This, DWORD reg, void *data, DWORD n) { return dev_GetPixelShaderConstantF(DEV9(This), reg, data, n); }
HRESULT WINAPI dev8_GetPixelShaderFunction(IDirect3DDevice8 *This, DWORD h, void *data, DWORD *size)
{
    IDirect3DPixelShader9 *ps = ps8_get(D8(This), h);
    if (!ps || !size) return D3DERR_INVALIDCALL;
    return ps_GetFunction(ps, data, (UINT *)size);
}

/* ---------------------------------------------------------------- export */
__declspec(dllexport) IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion)
{
    struct d3d8 *d;
    IDirect3D9 *d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d9) return NULL;
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) { IDirect3D9_Release(d9); return NULL; }
    d->vt = &d8_vtbl; d->ref = 1; d->d9 = (struct d3d9 *)d9;
    d3dpt_log("d3d8: Direct3DCreate8(sdk %u)", SDKVersion);
    return (IDirect3D8 *)d;
}
__declspec(dllexport) HRESULT WINAPI ValidateVertexShader(const DWORD *vs, const DWORD *decl, void *caps, DWORD flags, void *log) { return S_OK; }
__declspec(dllexport) HRESULT WINAPI ValidatePixelShader(const DWORD *ps, void *caps, DWORD flags, void *log) { return S_OK; }
