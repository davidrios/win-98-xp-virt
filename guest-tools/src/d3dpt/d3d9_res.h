/*
 * d3d9_res.h — resources of the paravirtual d3d9.dll (included by d3d9.c):
 * vertex/index buffers, textures, surfaces, shaders, and the device
 * methods that create, bind and draw with them (doc 14 P2).
 *
 * Model: every lockable resource keeps a guest-side shadow of its
 * contents; Lock hands out the shadow, Unlock forwards the dirty range
 * (BUFFER_UPDATE / TEXTURE_UPDATE). Host-side objects are created by sync
 * records and referenced by guest-chosen handles; a resource holds a
 * reference on the device (as native does), so the device outlives it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* ------------------------------------------------------- common header */
struct res_hdr {
    const void *vt;
    LONG ref;
    struct device *dev;
    uint32_t handle;        /* host handle, 0 = guest-only object */
    DWORD priority;
};

struct vbuf {
    struct res_hdr h;
    D3DVERTEXBUFFER_DESC desc;
    uint8_t *mem;
    UINT lock_off, lock_size;
    DWORD lock_flags;
    int locked;
};
struct ibuf {
    struct res_hdr h;
    D3DINDEXBUFFER_DESC desc;
    uint8_t *mem;
    UINT lock_off, lock_size;
    DWORD lock_flags;
    int locked;
};
struct level {
    uint8_t *mem;           /* NULL: not lockable (render target / default pool) */
    UINT w, h, pitch, rows, size;
    int locked;
    RECT lrect;
    DWORD lflags;
};
struct texture {
    struct res_hdr h;
    UINT width, height, levels;
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    DWORD lod;
    int faces;                  /* 1, or 6 for a cube texture */
    struct level lv[6][16];
    struct surface *surf[6][16];   /* cached level surfaces (not referenced by us) */
};
enum surf_kind { SURF_TEXLEVEL, SURF_BACKBUFFER, SURF_AUTODEPTH, SURF_RT, SURF_DS, SURF_SYSMEM };
struct surface {
    struct res_hdr h;
    enum surf_kind kind;
    D3DSURFACE_DESC desc;
    struct texture *tex;    /* SURF_TEXLEVEL: the container (referenced) */
    UINT face, level;
    uint8_t *mem;           /* SURF_SYSMEM */
    UINT pitch;
    int locked;
};
struct shader {
    struct res_hdr h;
    int pixel;
    DWORD *code;
    UINT bytes;
};

/* ------------------------------------------------------------ formats */
static int fmt_block(D3DFORMAT f, UINT *bw, UINT *bh, UINT *bytes)
{
    *bw = *bh = 1;
    switch (f) {
    case D3DFMT_DXT1: *bw = *bh = 4; *bytes = 8; return 1;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *bw = *bh = 4; *bytes = 16; return 1;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_D32:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8:
    case D3DFMT_INDEX32: case D3DFMT_R32F: case D3DFMT_G16R16F:
        *bytes = 4; return 1;
    case D3DFMT_R8G8B8: *bytes = 3; return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L16: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
    case D3DFMT_INDEX16: case D3DFMT_R16F: case D3DFMT_A8R3G3B2: case D3DFMT_L6V5U5: case D3DFMT_A8P8:
        *bytes = 2; return 1;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
        *bytes = 1; return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
        *bytes = 8; return 1;
    case D3DFMT_A32B32G32R32F: *bytes = 16; return 1;
    default: *bytes = 0; return 0;
    }
}
static void level_geometry(D3DFORMAT f, UINT w, UINT h, struct level *l)
{
    UINT bw, bh, bytes;
    fmt_block(f, &bw, &bh, &bytes);
    l->w = w; l->h = h;
    l->pitch = ((w + bw - 1) / bw) * bytes;
    l->rows = (h + bh - 1) / bh;
    l->size = l->pitch * l->rows;
}

/* ------------------------------------------------------- host helpers */
static HRESULT host_release(struct res_hdr *r)
{
    d3dpt_handle *h;
    if (!r->handle || !r->dev->host_alive) return D3D_OK;
    h = d3dpt_enc_cmd(&enc, D3DPT_OP_RELEASE, sizeof *h, 0);
    if (h) { h->handle = r->handle; h->pad = 0; }
    r->handle = 0;
    return D3D_OK;
}
static void res_init(struct res_hdr *r, const void *vt, struct device *dev)
{
    r->vt = vt; r->ref = 1; r->dev = dev; r->handle = d3dpt_enc_handle(&enc); r->priority = 0;
    IDirect3DDevice9_AddRef((IDirect3DDevice9 *)dev);
}
static void res_free(struct res_hdr *r)
{
    struct device *dev = r->dev;
    HeapFree(GetProcessHeap(), 0, r);
    IDirect3DDevice9_Release((IDirect3DDevice9 *)dev);
}
/* the common methods, generated per interface */
#define RES_COMMON(pfx, IFACE, IID_, TYPE_)                                                                  \
HRESULT WINAPI pfx##_QueryInterface(IFACE *This, REFIID riid, void **ppv)                                    \
{                                                                                                            \
    if (!ppv) return E_POINTER;                                                                              \
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_) || IsEqualGUID(riid, &IID_IDirect3DResource9)) { \
        *ppv = This; InterlockedIncrement(&((struct res_hdr *)This)->ref); return S_OK; }                  \
    *ppv = NULL; return E_NOINTERFACE;                                                                       \
}                                                                                                            \
ULONG WINAPI pfx##_AddRef(IFACE *This) { return InterlockedIncrement(&((struct res_hdr *)This)->ref); }     \
HRESULT WINAPI pfx##_GetDevice(IFACE *This, IDirect3DDevice9 **pp)                                           \
{                                                                                                            \
    if (!pp) return D3DERR_INVALIDCALL;                                                                      \
    *pp = (IDirect3DDevice9 *)((struct res_hdr *)This)->dev; IDirect3DDevice9_AddRef(*pp); return D3D_OK;    \
}                                                                                                            \
HRESULT WINAPI pfx##_SetPrivateData(IFACE *This, REFGUID g, const void *d, DWORD n, DWORD f) { return D3D_OK; } \
HRESULT WINAPI pfx##_GetPrivateData(IFACE *This, REFGUID g, void *d, DWORD *n) { return D3DERR_NOTFOUND; }  \
HRESULT WINAPI pfx##_FreePrivateData(IFACE *This, REFGUID g) { return D3DERR_NOTFOUND; }                     \
DWORD WINAPI pfx##_SetPriority(IFACE *This, DWORD p) { struct res_hdr *r = (struct res_hdr *)This; DWORD o = r->priority; r->priority = p; return o; } \
DWORD WINAPI pfx##_GetPriority(IFACE *This) { return ((struct res_hdr *)This)->priority; }                  \
void WINAPI pfx##_PreLoad(IFACE *This) { }                                                                   \
D3DRESOURCETYPE WINAPI pfx##_GetType(IFACE *This) { return TYPE_; }

RES_COMMON(vb, IDirect3DVertexBuffer9, IID_IDirect3DVertexBuffer9, D3DRTYPE_VERTEXBUFFER)
RES_COMMON(ib, IDirect3DIndexBuffer9, IID_IDirect3DIndexBuffer9, D3DRTYPE_INDEXBUFFER)
RES_COMMON(tex, IDirect3DTexture9, IID_IDirect3DTexture9, D3DRTYPE_TEXTURE)
RES_COMMON(surf, IDirect3DSurface9, IID_IDirect3DSurface9, D3DRTYPE_SURFACE)

/* ------------------------------------------------------------ buffers */
static HRESULT send_update(uint32_t handle, UINT off, UINT bytes, DWORD flags, const void *src)
{
    d3dpt_update *u;
    if (!bytes) return D3D_OK;
    u = d3dpt_enc_cmd(&enc, D3DPT_OP_BUFFER_UPDATE, sizeof *u, bytes);
    if (!u) return E_FAIL;
    u->handle = handle; u->offset = off; u->bytes = bytes; u->flags = flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    memcpy(u + 1, src, bytes);
    return D3D_OK;
}

ULONG WINAPI vb_Release(IDirect3DVertexBuffer9 *This)
{
    struct vbuf *b = (struct vbuf *)This;
    LONG r = InterlockedDecrement(&b->h.ref);
    if (r == 0) { host_release(&b->h); HeapFree(GetProcessHeap(), 0, b->mem); res_free(&b->h); }
    return r;
}
HRESULT WINAPI vb_Lock(IDirect3DVertexBuffer9 *This, UINT off, UINT size, void **pp, DWORD flags)
{
    struct vbuf *b = (struct vbuf *)This;
    if (!pp) return D3DERR_INVALIDCALL;
    if (off > b->desc.Size || size > b->desc.Size - off) { *pp = NULL; return D3DERR_INVALIDCALL; }
    if (!size) { off = 0; size = b->desc.Size; }
    b->lock_off = off; b->lock_size = size; b->lock_flags = flags; b->locked = 1;
    *pp = b->mem + off;
    return D3D_OK;
}
HRESULT WINAPI vb_Unlock(IDirect3DVertexBuffer9 *This)
{
    struct vbuf *b = (struct vbuf *)This;
    if (!b->locked) return D3D_OK;
    b->locked = 0;
    if (b->lock_flags & D3DLOCK_READONLY) return D3D_OK;
    return send_update(b->h.handle, b->lock_off, b->lock_size, b->lock_flags, b->mem + b->lock_off);
}
HRESULT WINAPI vb_GetDesc(IDirect3DVertexBuffer9 *This, D3DVERTEXBUFFER_DESC *d) { if (!d) return D3DERR_INVALIDCALL; *d = ((struct vbuf *)This)->desc; return D3D_OK; }

ULONG WINAPI ib_Release(IDirect3DIndexBuffer9 *This)
{
    struct ibuf *b = (struct ibuf *)This;
    LONG r = InterlockedDecrement(&b->h.ref);
    if (r == 0) { host_release(&b->h); HeapFree(GetProcessHeap(), 0, b->mem); res_free(&b->h); }
    return r;
}
HRESULT WINAPI ib_Lock(IDirect3DIndexBuffer9 *This, UINT off, UINT size, void **pp, DWORD flags)
{
    struct ibuf *b = (struct ibuf *)This;
    if (!pp) return D3DERR_INVALIDCALL;
    if (off > b->desc.Size || size > b->desc.Size - off) { *pp = NULL; return D3DERR_INVALIDCALL; }
    if (!size) { off = 0; size = b->desc.Size; }
    b->lock_off = off; b->lock_size = size; b->lock_flags = flags; b->locked = 1;
    *pp = b->mem + off;
    return D3D_OK;
}
HRESULT WINAPI ib_Unlock(IDirect3DIndexBuffer9 *This)
{
    struct ibuf *b = (struct ibuf *)This;
    if (!b->locked) return D3D_OK;
    b->locked = 0;
    if (b->lock_flags & D3DLOCK_READONLY) return D3D_OK;
    return send_update(b->h.handle, b->lock_off, b->lock_size, b->lock_flags, b->mem + b->lock_off);
}
HRESULT WINAPI ib_GetDesc(IDirect3DIndexBuffer9 *This, D3DINDEXBUFFER_DESC *d) { if (!d) return D3DERR_INVALIDCALL; *d = ((struct ibuf *)This)->desc; return D3D_OK; }

static HRESULT create_buffer(struct device *dev, int index, UINT length, DWORD usage, DWORD fvf_or_fmt, D3DPOOL pool, void **out)
{
    uint32_t off;
    d3dpt_create_buffer *c;
    HRESULT hr;
    struct res_hdr *h;
    uint8_t *mem;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!length) return D3DERR_INVALIDCALL;
    mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
    if (!mem) return E_OUTOFMEMORY;
    if (index) {
        struct ibuf *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *b);
        if (!b) { HeapFree(GetProcessHeap(), 0, mem); return E_OUTOFMEMORY; }
        res_init(&b->h, &ib_vtbl, dev);
        b->desc.Format = (D3DFORMAT)fvf_or_fmt; b->desc.Type = D3DRTYPE_INDEXBUFFER; b->desc.Usage = usage; b->desc.Pool = pool; b->desc.Size = length;
        b->mem = mem; h = &b->h;
    } else {
        struct vbuf *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *b);
        if (!b) { HeapFree(GetProcessHeap(), 0, mem); return E_OUTOFMEMORY; }
        res_init(&b->h, &vb_vtbl, dev);
        b->desc.Format = D3DFMT_VERTEXDATA; b->desc.Type = D3DRTYPE_VERTEXBUFFER; b->desc.Usage = usage; b->desc.Pool = pool; b->desc.Size = length; b->desc.FVF = fvf_or_fmt;
        b->mem = mem; h = &b->h;
    }
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, index ? D3DPT_OP_CREATE_INDEX_BUFFER : D3DPT_OP_CREATE_VERTEX_BUFFER, sizeof *c, 0);
    if (!c) { hr = E_FAIL; goto fail; }
    c->handle = h->handle; c->ret_off = off; c->length = length; c->usage = usage; c->fvf_or_format = fvf_or_fmt; c->pool = pool;
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) {
        d3dpt_log("d3dpt: Create%sBuffer(%u bytes, usage 0x%lx, pool %u) -> 0x%08lx", index ? "Index" : "Vertex", length, (unsigned long)usage, pool, (unsigned long)hr);
        h->handle = 0;
        goto fail;
    }
    *out = h;
    return D3D_OK;
fail:
    HeapFree(GetProcessHeap(), 0, mem);
    res_free(h);
    return hr;
}
HRESULT WINAPI dev_CreateVertexBuffer(IDirect3DDevice9 *This, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **pp, HANDLE *pSharedHandle)
{ return create_buffer(DEV(This), 0, Length, Usage, FVF, Pool, (void **)pp); }
HRESULT WINAPI dev_CreateIndexBuffer(IDirect3DDevice9 *This, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **pp, HANDLE *pSharedHandle)
{
    if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    return create_buffer(DEV(This), 1, Length, Usage, Format, Pool, (void **)pp);
}

/* ----------------------------------------------------------- textures */
static HRESULT tex_update_level(struct texture *t, UINT face, UINT level, const RECT *rc)
{
    struct level *l = &t->lv[face][level];
    UINT bw, bh, bytes, x0, y0, x1, y1, pitch, rows, i;
    d3dpt_tex_update *u;
    uint8_t *dst;
    if (!l->mem || !t->h.handle) return D3D_OK;
    fmt_block(t->format, &bw, &bh, &bytes);
    x0 = rc ? (UINT)rc->left : 0; y0 = rc ? (UINT)rc->top : 0;
    x1 = rc ? (UINT)rc->right : l->w; y1 = rc ? (UINT)rc->bottom : l->h;
    if (x1 > l->w) x1 = l->w;
    if (y1 > l->h) y1 = l->h;
    if (x0 >= x1 || y0 >= y1) return D3D_OK;
    x0 -= x0 % bw; y0 -= y0 % bh;                       /* block-align the box */
    pitch = ((x1 - x0 + bw - 1) / bw) * bytes;
    rows = (y1 - y0 + bh - 1) / bh;
    u = d3dpt_enc_cmd(&enc, D3DPT_OP_TEXTURE_UPDATE, sizeof *u, pitch * rows);
    if (!u) return E_FAIL;
    u->handle = t->h.handle; u->level = level | (face << 8); u->x = x0; u->y = y0; u->w = x1 - x0; u->h = y1 - y0;
    u->pitch = pitch; u->bytes = pitch * rows;
    dst = (uint8_t *)(u + 1);
    for (i = 0; i < rows; i++)
        memcpy(dst + i * pitch, l->mem + (y0 / bh + i) * l->pitch + (x0 / bw) * bytes, pitch);
    return D3D_OK;
}
static HRESULT tex_lock(struct texture *t, UINT face, UINT level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags)
{
    struct level *l;
    UINT bw, bh, bytes;
    if (!lr || level >= t->levels || face >= (UINT)t->faces) return D3DERR_INVALIDCALL;
    l = &t->lv[face][level];
    if (!l->mem || l->locked) { lr->pBits = NULL; lr->Pitch = 0; return D3DERR_INVALIDCALL; }
    fmt_block(t->format, &bw, &bh, &bytes);
    if (rc && ((UINT)rc->right > l->w || (UINT)rc->bottom > l->h || rc->left < 0 || rc->top < 0 || rc->left >= rc->right || rc->top >= rc->bottom))
        return D3DERR_INVALIDCALL;
    l->locked = 1; l->lflags = flags;
    if (rc) l->lrect = *rc; else { l->lrect.left = l->lrect.top = 0; l->lrect.right = l->w; l->lrect.bottom = l->h; }
    lr->Pitch = l->pitch;
    lr->pBits = l->mem + (l->lrect.top / bh) * l->pitch + (l->lrect.left / bw) * bytes;
    return D3D_OK;
}
static HRESULT tex_unlock(struct texture *t, UINT face, UINT level)
{
    struct level *l;
    if (level >= t->levels || face >= (UINT)t->faces) return D3DERR_INVALIDCALL;
    l = &t->lv[face][level];
    if (!l->locked) return D3DERR_INVALIDCALL;
    l->locked = 0;
    if (l->lflags & D3DLOCK_READONLY) return D3D_OK;
    return tex_update_level(t, face, level, &l->lrect);
}

ULONG WINAPI tex_Release(IDirect3DTexture9 *This)
{
    struct texture *t = (struct texture *)This;
    LONG r = InterlockedDecrement(&t->h.ref);
    if (r == 0) {
        UINT i, f;
        host_release(&t->h);
        for (f = 0; f < (UINT)t->faces; f++) for (i = 0; i < t->levels; i++) HeapFree(GetProcessHeap(), 0, t->lv[f][i].mem);
        res_free(&t->h);
    }
    return r;
}
DWORD WINAPI tex_SetLOD(IDirect3DTexture9 *This, DWORD lod) { struct texture *t = (struct texture *)This; DWORD o = t->lod; t->lod = lod; return o; }
DWORD WINAPI tex_GetLOD(IDirect3DTexture9 *This) { return ((struct texture *)This)->lod; }
DWORD WINAPI tex_GetLevelCount(IDirect3DTexture9 *This) { return ((struct texture *)This)->levels; }
HRESULT WINAPI tex_SetAutoGenFilterType(IDirect3DTexture9 *This, D3DTEXTUREFILTERTYPE f) { return D3D_OK; }
D3DTEXTUREFILTERTYPE WINAPI tex_GetAutoGenFilterType(IDirect3DTexture9 *This) { return D3DTEXF_LINEAR; }
void WINAPI tex_GenerateMipSubLevels(IDirect3DTexture9 *This) { }
static void level_desc(struct texture *t, UINT level, D3DSURFACE_DESC *d)
{
    d->Format = t->format; d->Type = D3DRTYPE_SURFACE; d->Usage = t->usage; d->Pool = t->pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE; d->MultiSampleQuality = 0;
    d->Width = t->lv[0][level].w; d->Height = t->lv[0][level].h;
}
HRESULT WINAPI tex_GetLevelDesc(IDirect3DTexture9 *This, UINT Level, D3DSURFACE_DESC *d)
{
    struct texture *t = (struct texture *)This;
    if (!d || Level >= t->levels) return D3DERR_INVALIDCALL;
    level_desc(t, Level, d);
    return D3D_OK;
}
HRESULT WINAPI tex_LockRect(IDirect3DTexture9 *This, UINT Level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags) { return tex_lock((struct texture *)This, 0, Level, lr, rc, flags); }
HRESULT WINAPI tex_UnlockRect(IDirect3DTexture9 *This, UINT Level) { return tex_unlock((struct texture *)This, 0, Level); }
HRESULT WINAPI tex_AddDirtyRect(IDirect3DTexture9 *This, const RECT *rc) { return D3D_OK; }

static HRESULT texture_create(struct device *dev, int faces, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, void **pp)
{
    struct texture *t;
    UINT bw, bh, bytes, i, w, h, f;
    int lockable;
    uint32_t off;
    d3dpt_create_texture *c;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!Width || !Height || !fmt_block(Format, &bw, &bh, &bytes)) return D3DERR_INVALIDCALL;
    if (!Levels) { for (w = Width, h = Height, Levels = 1; w > 1 || h > 1; Levels++) { w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; } }
    if (Levels > 16) return D3DERR_INVALIDCALL;
    t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
    if (!t) return E_OUTOFMEMORY;
    res_init(&t->h, faces == 6 ? (const void *)&cube_vtbl : (const void *)&tex_vtbl, dev);
    t->width = Width; t->height = Height; t->levels = Levels; t->format = Format; t->usage = Usage; t->pool = Pool; t->faces = faces;
    /* lockable: anything but a plain DEFAULT-pool / render-target / depth texture */
    lockable = !(Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && (Pool != D3DPOOL_DEFAULT || (Usage & D3DUSAGE_DYNAMIC));
    for (f = 0; f < (UINT)faces; f++) for (i = 0, w = Width, h = Height; i < Levels; i++) {
        level_geometry(Format, w, h, &t->lv[f][i]);
        if (lockable) {
            t->lv[f][i].mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, t->lv[f][i].size);
            if (!t->lv[f][i].mem) { hr = E_OUTOFMEMORY; goto fail; }
        }
        w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1;
    }
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, faces == 6 ? D3DPT_OP_CREATE_CUBE_TEXTURE : D3DPT_OP_CREATE_TEXTURE, sizeof *c, 0);
    if (!c) { hr = E_FAIL; goto fail; }
    memset(c, 0, sizeof *c);
    c->handle = t->h.handle; c->ret_off = off; c->width = Width; c->height = Height; c->levels = Levels;
    c->usage = Usage; c->format = Format; c->pool = Pool;
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) {
        d3dpt_log("d3dpt: CreateTexture(%ux%u, %u levels, usage 0x%lx, fmt %u, pool %u) -> 0x%08lx", Width, Height, Levels, (unsigned long)Usage, Format, Pool, (unsigned long)hr);
        t->h.handle = 0;
        goto fail;
    }
    *pp = t;
    return D3D_OK;
fail:
    for (f = 0; f < (UINT)faces; f++) for (i = 0; i < Levels; i++) HeapFree(GetProcessHeap(), 0, t->lv[f][i].mem);
    res_free(&t->h);
    return hr;
}
HRESULT WINAPI dev_CreateTexture(IDirect3DDevice9 *This, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **pp, HANDLE *pSharedHandle)
{ return texture_create(DEV(This), 1, Width, Height, Levels, Usage, Format, Pool, (void **)pp); }
HRESULT WINAPI dev_CreateCubeTexture(IDirect3DDevice9 *This, UINT Edge, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **pp, HANDLE *pSharedHandle)
{ return texture_create(DEV(This), 6, Edge, Edge, Levels, Usage, Format, Pool, (void **)pp); }

/* ----------------------------------------------------------- surfaces */
static struct surface *surface_new(struct device *dev, enum surf_kind kind, const D3DSURFACE_DESC *desc)
{
    struct surface *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return NULL;
    res_init(&s->h, &surf_vtbl, dev);
    s->kind = kind; s->desc = *desc;
    if (kind == SURF_SYSMEM) {
        struct level l;
        level_geometry(desc->Format, desc->Width, desc->Height, &l);
        s->pitch = l.pitch;
        s->mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l.size);
        if (!s->mem) { res_free(&s->h); return NULL; }
        s->h.handle = 0;    /* guest-only */
    }
    return s;
}
/* ask the host for a surface handle: the device's implicit surfaces or a texture level */
static HRESULT surface_get_host(struct surface *s, uint32_t texture, UINT level)
{
    uint32_t off = d3dpt_enc_ret(&enc, 0);
    d3dpt_get_surface *g = d3dpt_enc_cmd(&enc, D3DPT_OP_GET_SURFACE, sizeof *g, 0);
    HRESULT hr;
    if (!g) return E_FAIL;
    g->handle = s->h.handle; g->ret_off = off; g->texture = texture; g->level = level;
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) d3dpt_log("d3dpt: GetSurface(texture %u, level %u) -> 0x%08lx", texture, level, (unsigned long)hr);
    return hr;
}
ULONG WINAPI surf_Release(IDirect3DSurface9 *This)
{
    struct surface *s = (struct surface *)This;
    LONG r = InterlockedDecrement(&s->h.ref);
    if (r == 0) {
        host_release(&s->h);
        if (s->kind == SURF_TEXLEVEL && s->tex) {
            s->tex->surf[s->face][s->level] = NULL;
            IUnknown_Release((IUnknown *)s->tex);
        }
        HeapFree(GetProcessHeap(), 0, s->mem);
        res_free(&s->h);
    }
    return r;
}
HRESULT WINAPI surf_GetContainer(IDirect3DSurface9 *This, REFIID riid, void **pp)
{
    struct surface *s = (struct surface *)This;
    if (!pp) return D3DERR_INVALIDCALL;
    if (s->kind == SURF_TEXLEVEL && s->tex) return IDirect3DTexture9_QueryInterface((IDirect3DTexture9 *)s->tex, riid, pp);
    return IDirect3DDevice9_QueryInterface((IDirect3DDevice9 *)s->h.dev, riid, pp);
}
HRESULT WINAPI surf_GetDesc(IDirect3DSurface9 *This, D3DSURFACE_DESC *d) { if (!d) return D3DERR_INVALIDCALL; *d = ((struct surface *)This)->desc; return D3D_OK; }
HRESULT WINAPI surf_LockRect(IDirect3DSurface9 *This, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags)
{
    struct surface *s = (struct surface *)This;
    if (!lr) return D3DERR_INVALIDCALL;
    if (s->kind == SURF_TEXLEVEL) return tex_lock(s->tex, s->face, s->level, lr, rc, flags);
    if (s->kind != SURF_SYSMEM || s->locked) { lr->pBits = NULL; lr->Pitch = 0; return D3DERR_INVALIDCALL; }
    {
        UINT bw, bh, bytes;
        fmt_block(s->desc.Format, &bw, &bh, &bytes);
        s->locked = 1;
        lr->Pitch = s->pitch;
        lr->pBits = s->mem + (rc ? (rc->top / bh) * s->pitch + (rc->left / bw) * bytes : 0);
    }
    return D3D_OK;
}
HRESULT WINAPI surf_UnlockRect(IDirect3DSurface9 *This)
{
    struct surface *s = (struct surface *)This;
    if (s->kind == SURF_TEXLEVEL) return tex_unlock(s->tex, s->face, s->level);
    if (!s->locked) return D3DERR_INVALIDCALL;
    s->locked = 0;
    return D3D_OK;
}
static HRESULT tex_level_surface(struct texture *t, UINT face, UINT Level, IDirect3DSurface9 **pp)
{
    struct surface *s;
    D3DSURFACE_DESC d;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (Level >= t->levels || face >= (UINT)t->faces) return D3DERR_INVALIDCALL;
    if (t->surf[face][Level]) { *pp = (IDirect3DSurface9 *)t->surf[face][Level]; IDirect3DSurface9_AddRef(*pp); return D3D_OK; }
    level_desc(t, Level, &d);
    s = surface_new(t->h.dev, SURF_TEXLEVEL, &d);
    if (!s) return E_OUTOFMEMORY;
    s->tex = t; s->face = face; s->level = Level;
    InterlockedIncrement(&t->h.ref);
    hr = surface_get_host(s, t->h.handle, Level | (face << 8));
    if (FAILED(hr)) { s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s); return hr; }
    t->surf[face][Level] = s;
    *pp = (IDirect3DSurface9 *)s;
    return D3D_OK;
}
HRESULT WINAPI tex_GetSurfaceLevel(IDirect3DTexture9 *This, UINT Level, IDirect3DSurface9 **pp) { return tex_level_surface((struct texture *)This, 0, Level, pp); }
static HRESULT implicit_surface(struct device *dev, int depth, struct surface **slot, IDirect3DSurface9 **pp)
{
    D3DSURFACE_DESC d;
    struct surface *s;
    HRESULT hr;
    *pp = NULL;
    if (!*slot) {
        memset(&d, 0, sizeof d);
        d.Format = depth ? dev->pp.AutoDepthStencilFormat : dev->pp.BackBufferFormat;
        d.Type = D3DRTYPE_SURFACE; d.Usage = depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET; d.Pool = D3DPOOL_DEFAULT;
        d.MultiSampleType = dev->pp.MultiSampleType; d.MultiSampleQuality = dev->pp.MultiSampleQuality;
        d.Width = dev->pp.BackBufferWidth; d.Height = dev->pp.BackBufferHeight;
        s = surface_new(dev, depth ? SURF_AUTODEPTH : SURF_BACKBUFFER, &d);
        if (!s) return E_OUTOFMEMORY;
        hr = surface_get_host(s, 0, depth ? 1 : 0);
        if (FAILED(hr)) { s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s); return hr; }
        *slot = s;      /* the device keeps this reference */
    }
    *pp = (IDirect3DSurface9 *)*slot;
    IDirect3DSurface9_AddRef(*pp);
    return D3D_OK;
}
HRESULT WINAPI dev_GetBackBuffer(IDirect3DDevice9 *This, UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    if (iSwapChain || iBackBuffer) { *pp = NULL; return D3DERR_INVALIDCALL; }
    return implicit_surface(DEV(This), 0, &DEV(This)->bb, pp);
}
HRESULT WINAPI dev_GetRenderTarget(IDirect3DDevice9 *This, DWORD idx, IDirect3DSurface9 **pp)
{
    struct device *dev = DEV(This);
    if (!pp) return D3DERR_INVALIDCALL;
    if (idx > 3) { *pp = NULL; return D3DERR_INVALIDCALL; }
    if (dev->rt[idx]) { *pp = (IDirect3DSurface9 *)dev->rt[idx]; IDirect3DSurface9_AddRef(*pp); return D3D_OK; }
    if (idx == 0) return implicit_surface(dev, 0, &dev->bb, pp);
    *pp = NULL;
    return D3DERR_NOTFOUND;
}
HRESULT WINAPI dev_GetDepthStencilSurface(IDirect3DDevice9 *This, IDirect3DSurface9 **pp)
{
    struct device *dev = DEV(This);
    if (!pp) return D3DERR_INVALIDCALL;
    if (dev->ds) { *pp = (IDirect3DSurface9 *)dev->ds; IDirect3DSurface9_AddRef(*pp); return D3D_OK; }
    if (dev->pp.EnableAutoDepthStencil) return implicit_surface(dev, 1, &dev->auto_ds, pp);
    *pp = NULL;
    return D3DERR_NOTFOUND;
}
static void bind_surface(struct surface **slot, struct surface *s)
{
    if (s) IDirect3DSurface9_AddRef((IDirect3DSurface9 *)s);
    if (*slot) IDirect3DSurface9_Release((IDirect3DSurface9 *)*slot);
    *slot = s;
}
HRESULT WINAPI dev_SetRenderTarget(IDirect3DDevice9 *This, DWORD idx, IDirect3DSurface9 *pS)
{
    struct device *dev = DEV(This);
    struct surface *s = (struct surface *)pS;
    if (idx > 3) return D3DERR_INVALIDCALL;
    if (idx == 0 && !s) return D3DERR_INVALIDCALL;
    if (s && !s->h.handle) return D3DERR_INVALIDCALL;   /* system memory surfaces are not render targets */
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_RENDER_TARGET, idx, s ? s->h.handle : 0);
    bind_surface(&dev->rt[idx], s);
    if (idx == 0 && s) {
        dev->st.vp.X = dev->st.vp.Y = 0; dev->st.vp.Width = s->desc.Width; dev->st.vp.Height = s->desc.Height; dev->st.vp.MinZ = 0.0f; dev->st.vp.MaxZ = 1.0f;
        dev->st.scissor.left = dev->st.scissor.top = 0; dev->st.scissor.right = s->desc.Width; dev->st.scissor.bottom = s->desc.Height;
    }
    return D3D_OK;
}
HRESULT WINAPI dev_SetDepthStencilSurface(IDirect3DDevice9 *This, IDirect3DSurface9 *pS)
{
    struct device *dev = DEV(This);
    struct surface *s = (struct surface *)pS;
    if (s && !s->h.handle) return D3DERR_INVALIDCALL;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_DEPTH_STENCIL, s ? s->h.handle : 0, 0);
    bind_surface(&dev->ds, s);
    return D3D_OK;
}
static HRESULT create_host_surface(struct device *dev, uint32_t op, enum surf_kind kind, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, WINBOOL lockable, IDirect3DSurface9 **pp)
{
    D3DSURFACE_DESC d;
    struct surface *s;
    uint32_t off;
    d3dpt_create_texture *c;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!w || !h) return D3DERR_INVALIDCALL;
    memset(&d, 0, sizeof d);
    d.Format = fmt; d.Type = D3DRTYPE_SURFACE; d.Usage = kind == SURF_DS ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    d.Pool = D3DPOOL_DEFAULT; d.MultiSampleType = ms; d.MultiSampleQuality = msq; d.Width = w; d.Height = h;
    s = surface_new(dev, kind, &d);
    if (!s) return E_OUTOFMEMORY;
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, op, sizeof *c, 0);
    if (!c) { s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s); return E_FAIL; }
    memset(c, 0, sizeof *c);
    c->handle = s->h.handle; c->ret_off = off; c->width = w; c->height = h; c->levels = 1; c->format = fmt;
    c->multisample = ms; c->ms_quality = msq; c->lockable = lockable ? 1 : 0;
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) {
        d3dpt_log("d3dpt: Create%s(%ux%u fmt %u) -> 0x%08lx", kind == SURF_DS ? "DepthStencilSurface" : "RenderTarget", w, h, fmt, (unsigned long)hr);
        s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s);
        return hr;
    }
    *pp = (IDirect3DSurface9 *)s;
    return D3D_OK;
}
HRESULT WINAPI dev_CreateRenderTarget(IDirect3DDevice9 *This, UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, WINBOOL Lockable, IDirect3DSurface9 **pp, HANDLE *sh)
{ return create_host_surface(DEV(This), D3DPT_OP_CREATE_RENDER_TARGET, SURF_RT, W, H, F, MS, MSQ, Lockable, pp); }
HRESULT WINAPI dev_CreateDepthStencilSurface(IDirect3DDevice9 *This, UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, WINBOOL Discard, IDirect3DSurface9 **pp, HANDLE *sh)
{ return create_host_surface(DEV(This), D3DPT_OP_CREATE_DEPTH_STENCIL, SURF_DS, W, H, F, MS, MSQ, FALSE, pp); }
HRESULT WINAPI dev_CreateOffscreenPlainSurface(IDirect3DDevice9 *This, UINT W, UINT H, D3DFORMAT F, D3DPOOL Pool, IDirect3DSurface9 **pp, HANDLE *sh)
{
    D3DSURFACE_DESC d;
    UINT bw, bh, bytes;
    struct surface *s;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!W || !H || !fmt_block(F, &bw, &bh, &bytes)) return D3DERR_INVALIDCALL;
    if (Pool != D3DPOOL_SYSTEMMEM && Pool != D3DPOOL_SCRATCH) {
        uint32_t off;
        d3dpt_create_texture *c;
        HRESULT hr;
        memset(&d, 0, sizeof d);
        d.Format = F; d.Type = D3DRTYPE_SURFACE; d.Pool = Pool; d.Width = W; d.Height = H;
        s = surface_new(DEV(This), SURF_RT, &d);      /* host-only, not lockable from the guest yet */
        if (!s) return E_OUTOFMEMORY;
        off = d3dpt_enc_ret(&enc, 0);
        c = d3dpt_enc_cmd(&enc, D3DPT_OP_CREATE_OFFSCREEN, sizeof *c, 0);
        if (!c) { s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s); return E_FAIL; }
        memset(c, 0, sizeof *c);
        c->handle = s->h.handle; c->ret_off = off; c->width = W; c->height = H; c->levels = 1; c->format = F; c->pool = Pool;
        d3dpt_enc_flush(&enc);
        hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
        if (FAILED(hr)) { s->h.handle = 0; IDirect3DSurface9_Release((IDirect3DSurface9 *)s); return hr; }
        *pp = (IDirect3DSurface9 *)s;
        return D3D_OK;
    }
    memset(&d, 0, sizeof d);
    d.Format = F; d.Type = D3DRTYPE_SURFACE; d.Pool = Pool; d.Width = W; d.Height = H;
    s = surface_new(DEV(This), SURF_SYSMEM, &d);
    if (!s) return E_OUTOFMEMORY;
    *pp = (IDirect3DSurface9 *)s;
    return D3D_OK;
}
HRESULT WINAPI dev_GetRenderTargetData(IDirect3DDevice9 *This, IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst)
{
    struct surface *src = (struct surface *)pSrc, *dst = (struct surface *)pDst;
    UINT bw, bh, bytes, off, y;
    d3dpt_sync *p;
    d3dpt_ret *r;
    if (!src || !dst || !src->h.handle || dst->kind != SURF_SYSMEM) return D3DERR_INVALIDCALL;
    if (src->desc.Width != dst->desc.Width || src->desc.Height != dst->desc.Height || src->desc.Format != dst->desc.Format) return D3DERR_INVALIDCALL;
    fmt_block(dst->desc.Format, &bw, &bh, &bytes);
    off = d3dpt_enc_ret(&enc, dst->desc.Width * dst->desc.Height * bytes);
    p = d3dpt_enc_cmd(&enc, D3DPT_OP_GET_RENDER_TARGET_DATA, sizeof *p, 0);
    if (!p) return E_FAIL;
    p->handle = src->h.handle; p->ret_off = off;
    d3dpt_enc_flush(&enc);
    if (enc.last_status) return E_FAIL;
    r = d3dpt_enc_result(&enc, off);
    if (FAILED(r->hr)) { d3dpt_log("d3dpt: GetRenderTargetData -> 0x%08lx", (unsigned long)r->hr); return r->hr; }
    if (r->bytes < dst->desc.Width * dst->desc.Height * bytes) return E_FAIL;
    for (y = 0; y < dst->desc.Height; y++)
        memcpy(dst->mem + y * dst->pitch, (const uint8_t *)(r + 1) + y * dst->desc.Width * bytes, dst->desc.Width * bytes);
    return D3D_OK;
}
HRESULT WINAPI dev_StretchRect(IDirect3DDevice9 *This, IDirect3DSurface9 *pSrc, const RECT *sr, IDirect3DSurface9 *pDst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    struct surface *src = (struct surface *)pSrc, *dst = (struct surface *)pDst;
    d3dpt_stretch_rect *p;
    if (!src || !dst || !src->h.handle || !dst->h.handle) return D3DERR_INVALIDCALL;
    p = d3dpt_enc_cmd(&enc, D3DPT_OP_STRETCH_RECT, sizeof *p, 0);
    if (!p) return E_FAIL;
    memset(p, 0, sizeof *p);
    p->src = src->h.handle; p->dst = dst->h.handle; p->filter = f;
    if (sr) { p->has_rects |= 1; p->src_rect[0] = sr->left; p->src_rect[1] = sr->top; p->src_rect[2] = sr->right; p->src_rect[3] = sr->bottom; }
    if (dr) { p->has_rects |= 2; p->dst_rect[0] = dr->left; p->dst_rect[1] = dr->top; p->dst_rect[2] = dr->right; p->dst_rect[3] = dr->bottom; }
    return D3D_OK;
}
HRESULT WINAPI dev_GetSwapChain(IDirect3DDevice9 *This, UINT i, IDirect3DSwapChain9 **pp) { if (pp) *pp = NULL; D3DPT_STUB("IDirect3DDevice9::GetSwapChain"); return D3DERR_INVALIDCALL; }

/* ------------------------------------------------------------ shaders */
/* token stream length: skip comment blocks by their length, stop at END */
static UINT shader_bytes(const DWORD *code)
{
    UINT i = 1;
    if (!code) return 0;
    for (;;) {
        DWORD t = code[i];
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + ((t >> 16) & 0x7FFF);
        else i++;
        if (i > 65536) return 0;
    }
}
#define SHADER_COMMON(pfx, IFACE, IID_)                                                                       \
HRESULT WINAPI pfx##_QueryInterface(IFACE *This, REFIID riid, void **ppv)                                     \
{                                                                                                             \
    if (!ppv) return E_POINTER;                                                                               \
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_)) { *ppv = This; InterlockedIncrement(&((struct res_hdr *)This)->ref); return S_OK; } \
    *ppv = NULL; return E_NOINTERFACE;                                                                        \
}                                                                                                             \
ULONG WINAPI pfx##_AddRef(IFACE *This) { return InterlockedIncrement(&((struct res_hdr *)This)->ref); }      \
ULONG WINAPI pfx##_Release(IFACE *This)                                                                       \
{                                                                                                             \
    struct shader *s = (struct shader *)This;                                                                 \
    LONG r = InterlockedDecrement(&s->h.ref);                                                                 \
    if (r == 0) { host_release(&s->h); HeapFree(GetProcessHeap(), 0, s->code); res_free(&s->h); }            \
    return r;                                                                                                 \
}                                                                                                             \
HRESULT WINAPI pfx##_GetDevice(IFACE *This, IDirect3DDevice9 **pp)                                            \
{                                                                                                             \
    if (!pp) return D3DERR_INVALIDCALL;                                                                       \
    *pp = (IDirect3DDevice9 *)((struct res_hdr *)This)->dev; IDirect3DDevice9_AddRef(*pp); return D3D_OK;     \
}                                                                                                             \
HRESULT WINAPI pfx##_GetFunction(IFACE *This, void *data, UINT *size)                                         \
{                                                                                                             \
    struct shader *s = (struct shader *)This;                                                                 \
    if (!size) return D3DERR_INVALIDCALL;                                                                     \
    if (data) { if (*size < s->bytes) return D3DERR_INVALIDCALL; memcpy(data, s->code, s->bytes); }           \
    *size = s->bytes; return D3D_OK;                                                                          \
}
SHADER_COMMON(vs, IDirect3DVertexShader9, IID_IDirect3DVertexShader9)
SHADER_COMMON(ps, IDirect3DPixelShader9, IID_IDirect3DPixelShader9)

static HRESULT create_shader(struct device *dev, int pixel, const DWORD *code, void **pp)
{
    struct shader *s;
    UINT bytes;
    uint32_t off;
    d3dpt_create_shader *c;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    bytes = shader_bytes(code);
    if (!bytes || bytes > (256u << 10)) return D3DERR_INVALIDCALL;
    if ((code[0] & 0xFFFF0000) != (pixel ? 0xFFFF0000 : 0xFFFE0000)) return D3DERR_INVALIDCALL;
    s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return E_OUTOFMEMORY;
    res_init(&s->h, pixel ? (const void *)&ps_vtbl : (const void *)&vs_vtbl, dev);
    s->pixel = pixel; s->bytes = bytes;
    s->code = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!s->code) { res_free(&s->h); return E_OUTOFMEMORY; }
    memcpy(s->code, code, bytes);
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, pixel ? D3DPT_OP_CREATE_PIXEL_SHADER : D3DPT_OP_CREATE_VERTEX_SHADER, sizeof *c, bytes);
    if (!c) { hr = E_FAIL; goto fail; }
    c->handle = s->h.handle; c->ret_off = off; c->bytes = bytes; c->pad = 0;
    memcpy(c + 1, code, bytes);
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) {
        d3dpt_log("d3dpt: Create%sShader(%u bytes, version %08lx) -> 0x%08lx", pixel ? "Pixel" : "Vertex", bytes, (unsigned long)code[0], (unsigned long)hr);
        goto fail;
    }
    d3dpt_log("d3dpt: Create%sShader(%u bytes, version %08lx) ok", pixel ? "Pixel" : "Vertex", bytes, (unsigned long)code[0]);
    *pp = s;
    return D3D_OK;
fail:
    s->h.handle = 0;
    HeapFree(GetProcessHeap(), 0, s->code);
    res_free(&s->h);
    return hr;
}
HRESULT WINAPI dev_CreateVertexShader(IDirect3DDevice9 *This, const DWORD *code, IDirect3DVertexShader9 **pp) { return create_shader(DEV(This), 0, code, (void **)pp); }
HRESULT WINAPI dev_CreatePixelShader(IDirect3DDevice9 *This, const DWORD *code, IDirect3DPixelShader9 **pp) { return create_shader(DEV(This), 1, code, (void **)pp); }

/* ----------------------------------------------------------- bindings */
#define BIND(slot, obj, IFACE) do { \
    if (obj) IFACE##_AddRef((IFACE *)(obj)); \
    if (slot) IFACE##_Release((IFACE *)(slot)); \
    (slot) = (obj); } while (0)

HRESULT WINAPI dev_SetTexture(IDirect3DDevice9 *This, DWORD Stage, IDirect3DBaseTexture9 *pTex)
{
    struct device *dev = DEV(This);
    struct texture *t = (struct texture *)pTex;
    if (Stage >= 16) return D3DERR_INVALIDCALL;
    if (t && t->h.vt != &tex_vtbl && t->h.vt != &cube_vtbl) { D3DPT_STUB("SetTexture(volume) [later]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_TEXTURE, Stage, t ? t->h.handle : 0);
    BIND(dev->st.tex_bound[Stage], t, IUnknown);
    SB_MARK(dev, m_->textures |= 1u << Stage);
    return D3D_OK;
}
HRESULT WINAPI dev_GetTexture(IDirect3DDevice9 *This, DWORD Stage, IDirect3DBaseTexture9 **pp)
{
    struct device *dev = DEV(This);
    if (!pp) return D3DERR_INVALIDCALL;
    if (Stage >= 16) { *pp = NULL; return D3DERR_INVALIDCALL; }
    *pp = (IDirect3DBaseTexture9 *)dev->st.tex_bound[Stage];
    if (*pp) IDirect3DBaseTexture9_AddRef(*pp);
    return D3D_OK;
}
HRESULT WINAPI dev_SetStreamSource(IDirect3DDevice9 *This, UINT n, IDirect3DVertexBuffer9 *pVB, UINT off, UINT stride)
{
    struct device *dev = DEV(This);
    struct vbuf *b = (struct vbuf *)pVB;
    if (n >= 16) return D3DERR_INVALIDCALL;
    d3dpt_enc_u32x4(&enc, D3DPT_OP_SET_STREAM_SOURCE, n, b ? b->h.handle : 0, off, stride);
    BIND(dev->st.stream[n], b, IDirect3DVertexBuffer9);
    dev->st.stream_off[n] = off; dev->st.stream_stride[n] = stride;
    SB_MARK(dev, m_->streams |= 1u << n);
    return D3D_OK;
}
HRESULT WINAPI dev_GetStreamSource(IDirect3DDevice9 *This, UINT n, IDirect3DVertexBuffer9 **pp, UINT *off, UINT *stride)
{
    struct device *dev = DEV(This);
    if (!pp || n >= 16) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DVertexBuffer9 *)dev->st.stream[n];
    if (*pp) IDirect3DVertexBuffer9_AddRef(*pp);
    if (off) *off = dev->st.stream_off[n];
    if (stride) *stride = dev->st.stream_stride[n];
    return D3D_OK;
}
HRESULT WINAPI dev_SetIndices(IDirect3DDevice9 *This, IDirect3DIndexBuffer9 *pIB)
{
    struct device *dev = DEV(This);
    struct ibuf *b = (struct ibuf *)pIB;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_INDICES, b ? b->h.handle : 0, 0);
    BIND(dev->st.indices, b, IDirect3DIndexBuffer9);
    SB_MARK(dev, m_->misc |= SB_INDICES);
    return D3D_OK;
}
HRESULT WINAPI dev_GetIndices(IDirect3DDevice9 *This, IDirect3DIndexBuffer9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DIndexBuffer9 *)DEV(This)->st.indices;
    if (*pp) IDirect3DIndexBuffer9_AddRef(*pp);
    return D3D_OK;
}
HRESULT WINAPI dev_SetVertexShader(IDirect3DDevice9 *This, IDirect3DVertexShader9 *pS)
{
    struct device *dev = DEV(This);
    struct shader *s = (struct shader *)pS;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_VERTEX_SHADER, s ? s->h.handle : 0, 0);
    BIND(dev->st.vs, s, IDirect3DVertexShader9);
    SB_MARK(dev, m_->misc |= SB_VS);
    return D3D_OK;
}
HRESULT WINAPI dev_GetVertexShader(IDirect3DDevice9 *This, IDirect3DVertexShader9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DVertexShader9 *)DEV(This)->st.vs;
    if (*pp) IDirect3DVertexShader9_AddRef(*pp);
    return D3D_OK;
}
HRESULT WINAPI dev_SetPixelShader(IDirect3DDevice9 *This, IDirect3DPixelShader9 *pS)
{
    struct device *dev = DEV(This);
    struct shader *s = (struct shader *)pS;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_PIXEL_SHADER, s ? s->h.handle : 0, 0);
    BIND(dev->st.ps, s, IDirect3DPixelShader9);
    SB_MARK(dev, m_->misc |= SB_PS);
    return D3D_OK;
}
HRESULT WINAPI dev_GetPixelShader(IDirect3DDevice9 *This, IDirect3DPixelShader9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DPixelShader9 *)DEV(This)->st.ps;
    if (*pp) IDirect3DPixelShader9_AddRef(*pp);
    return D3D_OK;
}
static void device_unbind_all(struct device *dev)
{
    UINT i;
    for (i = 0; i < 16; i++) { BIND(dev->st.tex_bound[i], NULL, IUnknown); BIND(dev->st.stream[i], NULL, IDirect3DVertexBuffer9); }
    BIND(dev->st.decl, NULL, IDirect3DVertexDeclaration9);
    BIND(dev->st.indices, NULL, IDirect3DIndexBuffer9);
    BIND(dev->st.vs, NULL, IDirect3DVertexShader9);
    BIND(dev->st.ps, NULL, IDirect3DPixelShader9);
    for (i = 0; i < 4; i++) bind_surface(&dev->rt[i], NULL);
    bind_surface(&dev->ds, NULL);
    /* the implicit surfaces belong to the device; the host recreates them on Reset */
    if (dev->bb) { IDirect3DSurface9_Release((IDirect3DSurface9 *)dev->bb); dev->bb = NULL; }
    if (dev->auto_ds) { IDirect3DSurface9_Release((IDirect3DSurface9 *)dev->auto_ds); dev->auto_ds = NULL; }
}

/* -------------------------------------------------------------- draws */
HRESULT WINAPI dev_DrawPrimitive(IDirect3DDevice9 *This, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    if (!prim_vertex_count(t, count)) return D3DERR_INVALIDCALL;
    d3dpt_enc_u32x3(&enc, D3DPT_OP_DRAW_PRIMITIVE, t, start, count);
    return D3D_OK;
}
HRESULT WINAPI dev_DrawIndexedPrimitive(IDirect3DDevice9 *This, D3DPRIMITIVETYPE t, INT base, UINT minidx, UINT numv, UINT startidx, UINT count)
{
    d3dpt_draw_indexed *d;
    if (!prim_vertex_count(t, count)) return D3DERR_INVALIDCALL;
    d = d3dpt_enc_cmd(&enc, D3DPT_OP_DRAW_INDEXED_PRIMITIVE, sizeof *d, 0);
    if (!d) return E_FAIL;
    d->type = t; d->base_vertex = (uint32_t)base; d->min_index = minidx; d->num_vertices = numv; d->start_index = startidx; d->prim_count = count;
    return D3D_OK;
}
HRESULT WINAPI dev_DrawIndexedPrimitiveUP(IDirect3DDevice9 *This, D3DPRIMITIVETYPE t, UINT minidx, UINT numv, UINT count, const void *idx, D3DFORMAT ifmt, const void *vtx, UINT stride)
{
    UINT ni = prim_vertex_count(t, count), isz = ifmt == D3DFMT_INDEX32 ? 4 : ifmt == D3DFMT_INDEX16 ? 2 : 0, ib, vb;
    d3dpt_draw_indexed_up *d;
    if (!ni || !isz || !idx || !vtx || !stride || !numv) return D3DERR_INVALIDCALL;
    ib = ni * isz; vb = numv * stride;
    d = d3dpt_enc_cmd(&enc, D3DPT_OP_DRAW_INDEXED_PRIMITIVE_UP, sizeof *d, D3DPT_ALIGN8(ib) + vb);
    if (!d) return D3DERR_INVALIDCALL;
    d->type = t; d->min_index = minidx; d->num_vertices = numv; d->prim_count = count;
    d->index_format = ifmt; d->index_bytes = ib; d->stride = stride; d->vertex_bytes = vb;
    memcpy(d + 1, idx, ib);
    memcpy((uint8_t *)(d + 1) + D3DPT_ALIGN8(ib), vtx, vb);
    return D3D_OK;
}
