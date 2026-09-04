/*
 * d3d9_p3.h — vertex declarations, queries, state blocks, cube textures,
 * surface/texture updates, colour fill, clip planes and the shader
 * constant getters of the paravirtual d3d9.dll (doc 14 P3). Included by
 * d3d9.c after d3d9_res.h.
 *
 * State blocks are guest-side: a block is a snapshot of the device's
 * shadow state plus marks saying which items it holds; Apply re-issues
 * the marked items through the ordinary setters (which forward to the
 * host), Capture copies them from the shadow. The host never sees a
 * state block, which keeps the protocol driver-neutral (ADR-008).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* ------------------------------------------------- vertex declarations */
struct vdecl {
    struct res_hdr h;
    UINT count;                 /* elements including D3DDECL_END */
    D3DVERTEXELEMENT9 el[65];
};
HRESULT WINAPI decl_QueryInterface(IDirect3DVertexDeclaration9 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DVertexDeclaration9)) { *ppv = This; InterlockedIncrement(&((struct res_hdr *)This)->ref); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
ULONG WINAPI decl_AddRef(IDirect3DVertexDeclaration9 *This) { return InterlockedIncrement(&((struct res_hdr *)This)->ref); }
ULONG WINAPI decl_Release(IDirect3DVertexDeclaration9 *This)
{
    struct vdecl *d = (struct vdecl *)This;
    LONG r = InterlockedDecrement(&d->h.ref);
    if (r == 0) { host_release(&d->h); res_free(&d->h); }
    return r;
}
HRESULT WINAPI decl_GetDevice(IDirect3DVertexDeclaration9 *This, IDirect3DDevice9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DDevice9 *)((struct res_hdr *)This)->dev; IDirect3DDevice9_AddRef(*pp); return D3D_OK;
}
HRESULT WINAPI decl_GetDeclaration(IDirect3DVertexDeclaration9 *This, D3DVERTEXELEMENT9 *out, UINT *n)
{
    struct vdecl *d = (struct vdecl *)This;
    if (!n) return D3DERR_INVALIDCALL;
    if (out) { if (*n < d->count) return D3DERR_INVALIDCALL; memcpy(out, d->el, d->count * sizeof *out); }
    *n = d->count;
    return D3D_OK;
}
HRESULT WINAPI dev_CreateVertexDeclaration(IDirect3DDevice9 *This, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **pp)
{
    struct device *dev = DEV(This);
    struct vdecl *d;
    UINT n = 0, off;
    d3dpt_create_shader *c;
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!el) return D3DERR_INVALIDCALL;
    while (el[n].Stream != 0xFF) { if (++n > 64) return D3DERR_INVALIDCALL; }
    n++;
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    res_init(&d->h, &decl_vtbl, dev);
    d->count = n;
    memcpy(d->el, el, n * sizeof *el);
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, D3DPT_OP_CREATE_VERTEX_DECL, sizeof *c, n * sizeof *el);
    if (!c) { res_free(&d->h); return E_FAIL; }
    c->handle = d->h.handle; c->ret_off = off; c->bytes = n * sizeof *el; c->pad = 0;
    memcpy(c + 1, el, n * sizeof *el);
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) { d3dpt_log("d3dpt: CreateVertexDeclaration(%u elements) -> 0x%08lx", n, (unsigned long)hr); d->h.handle = 0; res_free(&d->h); return hr; }
    *pp = (IDirect3DVertexDeclaration9 *)d;
    return D3D_OK;
}
HRESULT WINAPI dev_SetVertexDeclaration(IDirect3DDevice9 *This, IDirect3DVertexDeclaration9 *pD)
{
    struct device *dev = DEV(This);
    struct vdecl *d = (struct vdecl *)pD;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_VERTEX_DECL, d ? d->h.handle : 0, 0);
    BIND(dev->st.decl, d, IDirect3DVertexDeclaration9);
    if (d) dev->st.fvf = 0;
    SB_MARK(dev, m_->misc |= SB_DECL);
    return D3D_OK;
}
HRESULT WINAPI dev_GetVertexDeclaration(IDirect3DDevice9 *This, IDirect3DVertexDeclaration9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DVertexDeclaration9 *)DEV(This)->st.decl;
    if (*pp) IDirect3DVertexDeclaration9_AddRef(*pp);
    return D3D_OK;
}

/* ------------------------------------------------------------ queries */
struct query {
    struct res_hdr h;
    D3DQUERYTYPE type;
    DWORD size;
};
static DWORD query_data_size(D3DQUERYTYPE t)
{
    switch (t) {
    case D3DQUERYTYPE_EVENT: return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP: return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ: return sizeof(UINT64);
    case D3DQUERYTYPE_VERTEXSTATS: return sizeof(D3DDEVINFO_D3DVERTEXSTATS);
    default: return 0;
    }
}
HRESULT WINAPI query_QueryInterface(IDirect3DQuery9 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DQuery9)) { *ppv = This; InterlockedIncrement(&((struct res_hdr *)This)->ref); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
ULONG WINAPI query_AddRef(IDirect3DQuery9 *This) { return InterlockedIncrement(&((struct res_hdr *)This)->ref); }
ULONG WINAPI query_Release(IDirect3DQuery9 *This)
{
    struct query *q = (struct query *)This;
    LONG r = InterlockedDecrement(&q->h.ref);
    if (r == 0) { host_release(&q->h); res_free(&q->h); }
    return r;
}
HRESULT WINAPI query_GetDevice(IDirect3DQuery9 *This, IDirect3DDevice9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DDevice9 *)((struct res_hdr *)This)->dev; IDirect3DDevice9_AddRef(*pp); return D3D_OK;
}
D3DQUERYTYPE WINAPI query_GetType(IDirect3DQuery9 *This) { return ((struct query *)This)->type; }
DWORD WINAPI query_GetDataSize(IDirect3DQuery9 *This) { return ((struct query *)This)->size; }
HRESULT WINAPI query_Issue(IDirect3DQuery9 *This, DWORD flags)
{
    struct query *q = (struct query *)This;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_QUERY_ISSUE, q->h.handle, flags);
    return D3D_OK;
}
HRESULT WINAPI query_GetData(IDirect3DQuery9 *This, void *data, DWORD size, DWORD flags)
{
    struct query *q = (struct query *)This;
    uint32_t off;
    d3dpt_query_get *g;
    d3dpt_ret *r;
    if (data && size < q->size) return D3DERR_INVALIDCALL;
    off = d3dpt_enc_ret(&enc, data ? q->size : 0);
    g = d3dpt_enc_cmd(&enc, D3DPT_OP_QUERY_GET_DATA, sizeof *g, 0);
    if (!g) return E_FAIL;
    g->handle = q->h.handle; g->ret_off = off; g->flags = flags; g->size = data ? q->size : 0;
    d3dpt_enc_flush(&enc);
    if (enc.last_status) return E_FAIL;
    r = d3dpt_enc_result(&enc, off);
    if (r->hr == S_OK && data && r->bytes >= q->size) memcpy(data, r + 1, q->size);
    return (HRESULT)r->hr;
}
HRESULT WINAPI dev_CreateQuery(IDirect3DDevice9 *This, D3DQUERYTYPE Type, IDirect3DQuery9 **pp)
{
    struct device *dev = DEV(This);
    struct query *q;
    uint32_t off;
    d3dpt_create_query *c;
    HRESULT hr;
    if (!query_data_size(Type)) return D3DERR_NOTAVAILABLE;
    if (!pp) return D3D_OK;                     /* "is this query type supported" probe */
    *pp = NULL;
    q = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *q);
    if (!q) return E_OUTOFMEMORY;
    res_init(&q->h, &query_vtbl, dev);
    q->type = Type; q->size = query_data_size(Type);
    off = d3dpt_enc_ret(&enc, 0);
    c = d3dpt_enc_cmd(&enc, D3DPT_OP_CREATE_QUERY, sizeof *c, 0);
    if (!c) { res_free(&q->h); return E_FAIL; }
    c->handle = q->h.handle; c->ret_off = off; c->type = Type; c->pad = 0;
    d3dpt_enc_flush(&enc);
    hr = enc.last_status ? E_FAIL : d3dpt_enc_result(&enc, off)->hr;
    if (FAILED(hr)) { q->h.handle = 0; res_free(&q->h); return hr; }
    *pp = (IDirect3DQuery9 *)q;
    return D3D_OK;
}

/* ------------------------------------------------------ cube textures */
RES_COMMON(cube, IDirect3DCubeTexture9, IID_IDirect3DCubeTexture9, D3DRTYPE_CUBETEXTURE)
ULONG WINAPI cube_Release(IDirect3DCubeTexture9 *This) { return tex_Release((IDirect3DTexture9 *)This); }
DWORD WINAPI cube_SetLOD(IDirect3DCubeTexture9 *This, DWORD lod) { return tex_SetLOD((IDirect3DTexture9 *)This, lod); }
DWORD WINAPI cube_GetLOD(IDirect3DCubeTexture9 *This) { return tex_GetLOD((IDirect3DTexture9 *)This); }
DWORD WINAPI cube_GetLevelCount(IDirect3DCubeTexture9 *This) { return ((struct texture *)This)->levels; }
HRESULT WINAPI cube_SetAutoGenFilterType(IDirect3DCubeTexture9 *This, D3DTEXTUREFILTERTYPE f) { return D3D_OK; }
D3DTEXTUREFILTERTYPE WINAPI cube_GetAutoGenFilterType(IDirect3DCubeTexture9 *This) { return D3DTEXF_LINEAR; }
void WINAPI cube_GenerateMipSubLevels(IDirect3DCubeTexture9 *This) { }
HRESULT WINAPI cube_GetLevelDesc(IDirect3DCubeTexture9 *This, UINT Level, D3DSURFACE_DESC *d) { return tex_GetLevelDesc((IDirect3DTexture9 *)This, Level, d); }
HRESULT WINAPI cube_GetCubeMapSurface(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES face, UINT Level, IDirect3DSurface9 **pp) { return tex_level_surface((struct texture *)This, (UINT)face, Level, pp); }
HRESULT WINAPI cube_LockRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES face, UINT Level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags) { return tex_lock((struct texture *)This, (UINT)face, Level, lr, rc, flags); }
HRESULT WINAPI cube_UnlockRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES face, UINT Level) { return tex_unlock((struct texture *)This, (UINT)face, Level); }
HRESULT WINAPI cube_AddDirtyRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES face, const RECT *rc) { return D3D_OK; }

/* ---------------------------------------- surface / texture uploads */
/* upload a box of a guest-shadowed source into a host surface */
static HRESULT surface_upload(uint32_t handle, D3DFORMAT fmt, const uint8_t *src, UINT src_pitch, UINT sx, UINT sy, UINT w, UINT h, UINT dx, UINT dy)
{
    UINT bw, bh, bytes, pitch, rows, i;
    d3dpt_tex_update *u;
    uint8_t *dst;
    fmt_block(fmt, &bw, &bh, &bytes);
    pitch = ((w + bw - 1) / bw) * bytes;
    rows = (h + bh - 1) / bh;
    u = d3dpt_enc_cmd(&enc, D3DPT_OP_SURFACE_UPDATE, sizeof *u, pitch * rows);
    if (!u) return E_FAIL;
    u->handle = handle; u->level = 0; u->x = dx; u->y = dy; u->w = w; u->h = h; u->pitch = pitch; u->bytes = pitch * rows;
    dst = (uint8_t *)(u + 1);
    for (i = 0; i < rows; i++) memcpy(dst + i * pitch, src + (sy / bh + i) * src_pitch + (sx / bw) * bytes, pitch);
    return D3D_OK;
}
static int surface_shadow(struct surface *s, const uint8_t **mem, UINT *pitch)
{
    if (s->kind == SURF_SYSMEM) { *mem = s->mem; *pitch = s->pitch; return s->mem != NULL; }
    if (s->kind == SURF_TEXLEVEL && s->tex) { struct level *l = &s->tex->lv[s->face][s->level]; *mem = l->mem; *pitch = l->pitch; return l->mem != NULL; }
    return 0;
}
HRESULT WINAPI dev_UpdateSurface(IDirect3DDevice9 *This, IDirect3DSurface9 *pSrc, const RECT *sr, IDirect3DSurface9 *pDst, const POINT *dp)
{
    struct surface *src = (struct surface *)pSrc, *dst = (struct surface *)pDst;
    const uint8_t *mem;
    UINT pitch, sx = 0, sy = 0, w, h, dx = 0, dy = 0;
    if (!src || !dst || src->desc.Format != dst->desc.Format) return D3DERR_INVALIDCALL;
    if (!surface_shadow(src, &mem, &pitch)) { D3DPT_STUB("UpdateSurface from a host-only surface"); return D3DERR_INVALIDCALL; }
    w = src->desc.Width; h = src->desc.Height;
    if (sr) { sx = sr->left; sy = sr->top; w = sr->right - sr->left; h = sr->bottom - sr->top; }
    if (dp) { dx = dp->x; dy = dp->y; }
    if (sx + w > src->desc.Width || sy + h > src->desc.Height || dx + w > dst->desc.Width || dy + h > dst->desc.Height) return D3DERR_INVALIDCALL;
    if (dst->kind == SURF_TEXLEVEL && dst->tex) {
        /* keep the destination's own shadow current too, then upload through the texture path */
        struct level *l = &dst->tex->lv[dst->face][dst->level];
        if (l->mem) {
            UINT bw, bh, bytes, i, rows, row;
            RECT rc = { (LONG)dx, (LONG)dy, (LONG)(dx + w), (LONG)(dy + h) };
            fmt_block(dst->desc.Format, &bw, &bh, &bytes);
            rows = (h + bh - 1) / bh; row = ((w + bw - 1) / bw) * bytes;
            for (i = 0; i < rows; i++) memcpy(l->mem + (dy / bh + i) * l->pitch + (dx / bw) * bytes, mem + (sy / bh + i) * pitch + (sx / bw) * bytes, row);
            return tex_update_level(dst->tex, dst->face, dst->level, &rc);
        }
        return surface_upload(dst->h.handle, dst->desc.Format, mem, pitch, sx, sy, w, h, dx, dy);
    }
    if (!dst->h.handle) return D3DERR_INVALIDCALL;
    return surface_upload(dst->h.handle, dst->desc.Format, mem, pitch, sx, sy, w, h, dx, dy);
}
HRESULT WINAPI dev_UpdateTexture(IDirect3DDevice9 *This, IDirect3DBaseTexture9 *pSrc, IDirect3DBaseTexture9 *pDst)
{
    struct texture *src = (struct texture *)pSrc, *dst = (struct texture *)pDst;
    UINT f, i, skip;
    if (!src || !dst || src->h.vt != dst->h.vt || src->format != dst->format || src->faces != dst->faces) return D3DERR_INVALIDCALL;
    if (src->h.vt != &tex_vtbl && src->h.vt != &cube_vtbl) { D3DPT_STUB("UpdateTexture(volume)"); return D3DERR_INVALIDCALL; }
    if (src->levels < dst->levels) return D3DERR_INVALIDCALL;
    skip = src->levels - dst->levels;       /* the source's smaller levels map onto the destination */
    for (f = 0; f < (UINT)src->faces; f++) for (i = 0; i < dst->levels; i++) {
        struct level *sl = &src->lv[f][i + skip], *dl = &dst->lv[f][i];
        if (!sl->mem || sl->w != dl->w || sl->h != dl->h) return D3DERR_INVALIDCALL;
        if (dl->mem) memcpy(dl->mem, sl->mem, dl->size);
        {
            HRESULT hr = dl->mem ? tex_update_level(dst, f, i, NULL)
                                 : surface_upload(0, dst->format, sl->mem, sl->pitch, 0, 0, sl->w, sl->h, 0, 0);
            if (!dl->mem) {
                /* host-only destination level: upload straight into it */
                d3dpt_tex_update *u;
                UINT bw, bh, bytes;
                fmt_block(dst->format, &bw, &bh, &bytes);
                u = d3dpt_enc_cmd(&enc, D3DPT_OP_TEXTURE_UPDATE, sizeof *u, sl->size);
                if (!u) return E_FAIL;
                u->handle = dst->h.handle; u->level = i | (f << 8); u->x = 0; u->y = 0; u->w = sl->w; u->h = sl->h; u->pitch = sl->pitch; u->bytes = sl->size;
                memcpy(u + 1, sl->mem, sl->size);
                hr = D3D_OK;
            }
            if (FAILED(hr)) return hr;
        }
    }
    return D3D_OK;
}
HRESULT WINAPI dev_ColorFill(IDirect3DDevice9 *This, IDirect3DSurface9 *pS, const RECT *rc, D3DCOLOR color)
{
    struct surface *s = (struct surface *)pS;
    d3dpt_color_fill *f;
    if (!s) return D3DERR_INVALIDCALL;
    if (!s->h.handle) {
        /* system-memory surface: fill the shadow (32-bit formats only for now) */
        UINT bw, bh, bytes, x, y, x0 = 0, y0 = 0, x1 = s->desc.Width, y1 = s->desc.Height;
        fmt_block(s->desc.Format, &bw, &bh, &bytes);
        if (bytes != 4 || bw != 1) return D3DERR_INVALIDCALL;
        if (rc) { x0 = rc->left; y0 = rc->top; x1 = rc->right; y1 = rc->bottom; }
        for (y = y0; y < y1 && y < s->desc.Height; y++) for (x = x0; x < x1 && x < s->desc.Width; x++) ((uint32_t *)(s->mem + y * s->pitch))[x] = color;
        return D3D_OK;
    }
    f = d3dpt_enc_cmd(&enc, D3DPT_OP_COLOR_FILL, sizeof *f, 0);
    if (!f) return E_FAIL;
    memset(f, 0, sizeof *f);
    f->handle = s->h.handle; f->color = color;
    if (rc) { f->has_rect = 1; f->rect[0] = rc->left; f->rect[1] = rc->top; f->rect[2] = rc->right; f->rect[3] = rc->bottom; }
    return D3D_OK;
}

/* --------------------------------------------- clip planes, constants */
HRESULT WINAPI dev_SetClipPlane(IDirect3DDevice9 *This, DWORD i, const float *p)
{
    d3dpt_clip_plane *c;
    if (i > 5 || !p) return D3DERR_INVALIDCALL;
    memcpy(DEV(This)->st.clip[i], p, 16);
    SB_MARK(DEV(This), m_->clip |= 1u << i);
    c = d3dpt_enc_cmd(&enc, D3DPT_OP_SET_CLIP_PLANE, sizeof *c, 0);
    if (!c) return E_FAIL;
    c->index = i; c->pad = 0; memcpy(c->plane, p, 16);
    return D3D_OK;
}
HRESULT WINAPI dev_GetClipPlane(IDirect3DDevice9 *This, DWORD i, float *p)
{
    if (i > 5 || !p) return D3DERR_INVALIDCALL;
    memcpy(p, DEV(This)->st.clip[i], 16);
    return D3D_OK;
}
HRESULT WINAPI dev_GetVertexShaderConstantF(IDirect3DDevice9 *This, UINT start, float *out, UINT n)
{
    if (!out || start > 256 || n > 256 - start) return D3DERR_INVALIDCALL;
    memcpy(out, DEV(This)->st.vs_f[start], n * 16);
    return D3D_OK;
}
HRESULT WINAPI dev_GetPixelShaderConstantF(IDirect3DDevice9 *This, UINT start, float *out, UINT n)
{
    if (!out || start > 224 || n > 224 - start) return D3DERR_INVALIDCALL;
    memcpy(out, DEV(This)->st.ps_f[start], n * 16);
    return D3D_OK;
}
static HRESULT set_const_ib(struct device *dev, uint32_t op, UINT start, const void *data, UINT n, UINT elem)
{
    d3dpt_u32x2 *p;
    UINT i;
    if (!data || start > 16 || n > 16 - start) return D3DERR_INVALIDCALL;
    if (!n) return D3D_OK;
    switch (op) {
    case D3DPT_OP_SET_VS_CONST_I: memcpy(dev->st.vs_i[start], data, n * 16); SB_MARK(dev, for (i = start; i < start + n; i++) m_->vs_i |= 1u << i); break;
    case D3DPT_OP_SET_PS_CONST_I: memcpy(dev->st.ps_i[start], data, n * 16); SB_MARK(dev, for (i = start; i < start + n; i++) m_->ps_i |= 1u << i); break;
    case D3DPT_OP_SET_VS_CONST_B: memcpy(&dev->st.vs_b[start], data, n * 4); SB_MARK(dev, for (i = start; i < start + n; i++) m_->vs_b |= 1u << i); break;
    default:                      memcpy(&dev->st.ps_b[start], data, n * 4); SB_MARK(dev, for (i = start; i < start + n; i++) m_->ps_b |= 1u << i); break;
    }
    p = d3dpt_enc_cmd(&enc, op, sizeof *p, n * elem);
    if (!p) return E_FAIL;
    p->a = start; p->b = n;
    memcpy(p + 1, data, n * elem);
    return D3D_OK;
}
HRESULT WINAPI dev_SetVertexShaderConstantI(IDirect3DDevice9 *This, UINT s, const int *d, UINT n) { return set_const_ib(DEV(This), D3DPT_OP_SET_VS_CONST_I, s, d, n, 16); }
HRESULT WINAPI dev_SetPixelShaderConstantI(IDirect3DDevice9 *This, UINT s, const int *d, UINT n) { return set_const_ib(DEV(This), D3DPT_OP_SET_PS_CONST_I, s, d, n, 16); }
HRESULT WINAPI dev_SetVertexShaderConstantB(IDirect3DDevice9 *This, UINT s, const WINBOOL *d, UINT n) { return set_const_ib(DEV(This), D3DPT_OP_SET_VS_CONST_B, s, d, n, 4); }
HRESULT WINAPI dev_SetPixelShaderConstantB(IDirect3DDevice9 *This, UINT s, const WINBOOL *d, UINT n) { return set_const_ib(DEV(This), D3DPT_OP_SET_PS_CONST_B, s, d, n, 4); }
HRESULT WINAPI dev_GetVertexShaderConstantI(IDirect3DDevice9 *This, UINT s, int *d, UINT n) { if (!d || s > 16 || n > 16 - s) return D3DERR_INVALIDCALL; memcpy(d, DEV(This)->st.vs_i[s], n * 16); return D3D_OK; }
HRESULT WINAPI dev_GetPixelShaderConstantI(IDirect3DDevice9 *This, UINT s, int *d, UINT n) { if (!d || s > 16 || n > 16 - s) return D3DERR_INVALIDCALL; memcpy(d, DEV(This)->st.ps_i[s], n * 16); return D3D_OK; }
HRESULT WINAPI dev_GetVertexShaderConstantB(IDirect3DDevice9 *This, UINT s, WINBOOL *d, UINT n) { if (!d || s > 16 || n > 16 - s) return D3DERR_INVALIDCALL; memcpy(d, &DEV(This)->st.vs_b[s], n * 4); return D3D_OK; }
HRESULT WINAPI dev_GetPixelShaderConstantB(IDirect3DDevice9 *This, UINT s, WINBOOL *d, UINT n) { if (!d || s > 16 || n > 16 - s) return D3DERR_INVALIDCALL; memcpy(d, &DEV(This)->st.ps_b[s], n * 4); return D3D_OK; }

/* ------------------------------------------------------- state blocks */
struct stateblock {
    struct res_hdr h;
    struct sb_marks marks;
    struct shadow_state st;     /* bound objects inside are referenced */
};
static struct sb_marks *rec_marks(struct device *dev) { return dev->recording ? &dev->recording->marks : NULL; }

static void sb_release_objects(struct shadow_state *st)
{
    UINT i;
    for (i = 0; i < 16; i++) { if (st->tex_bound[i]) IUnknown_Release((IUnknown *)st->tex_bound[i]); if (st->stream[i]) IUnknown_Release((IUnknown *)st->stream[i]); }
    if (st->indices) IUnknown_Release((IUnknown *)st->indices);
    if (st->vs) IUnknown_Release((IUnknown *)st->vs);
    if (st->ps) IUnknown_Release((IUnknown *)st->ps);
    if (st->decl) IUnknown_Release((IUnknown *)st->decl);
}
/* copy the marked items of `from` into `to` (object references adjusted) */
static void sb_copy(const struct sb_marks *m, const struct shadow_state *from, struct shadow_state *to)
{
    UINT i, j;
    for (i = 0; i < MAX_RS; i++) if (m->rs[i / 32] & (1u << (i % 32))) to->rs[i] = from->rs[i];
    for (i = 0; i < MAX_TSS_STAGES; i++) for (j = 0; j < 33; j++) if (m->tss[i] & (1ull << j)) to->tss[i][j] = from->tss[i][j];
    for (i = 0; i < MAX_SAMPLERS; i++) for (j = 0; j < 14; j++) if (m->samp[i] & (1u << j)) to->samp[i][j] = from->samp[i][j];
    if (m->xform & 1) to->world = from->world;
    if (m->xform & 2) to->view = from->view;
    if (m->xform & 4) to->proj = from->proj;
    for (i = 0; i < 8; i++) if (m->xform & (8u << i)) to->tex[i] = from->tex[i];
    if (m->misc & SB_VP) to->vp = from->vp;
    if (m->misc & SB_MATERIAL) to->material = from->material;
    if (m->misc & SB_FVF) to->fvf = from->fvf;
    if (m->misc & SB_SCISSOR) to->scissor = from->scissor;
    for (i = 0; i < 8; i++) { if (m->lights & (1u << i)) to->lights[i] = from->lights[i]; if (m->light_on & (1u << i)) to->light_on[i] = from->light_on[i]; }
    for (i = 0; i < 6; i++) if (m->clip & (1u << i)) memcpy(to->clip[i], from->clip[i], 16);
    for (i = 0; i < 256; i++) if (m->vs_f[i / 32] & (1u << (i % 32))) memcpy(to->vs_f[i], from->vs_f[i], 16);
    for (i = 0; i < 224; i++) if (m->ps_f[i / 32] & (1u << (i % 32))) memcpy(to->ps_f[i], from->ps_f[i], 16);
    for (i = 0; i < 16; i++) {
        if (m->vs_i & (1u << i)) memcpy(to->vs_i[i], from->vs_i[i], 16);
        if (m->ps_i & (1u << i)) memcpy(to->ps_i[i], from->ps_i[i], 16);
        if (m->vs_b & (1u << i)) to->vs_b[i] = from->vs_b[i];
        if (m->ps_b & (1u << i)) to->ps_b[i] = from->ps_b[i];
    }
    for (i = 0; i < 16; i++) {
        if (m->textures & (1u << i)) { if (from->tex_bound[i]) IUnknown_AddRef((IUnknown *)from->tex_bound[i]); if (to->tex_bound[i]) IUnknown_Release((IUnknown *)to->tex_bound[i]); to->tex_bound[i] = from->tex_bound[i]; }
        if (m->streams & (1u << i)) {
            if (from->stream[i]) IUnknown_AddRef((IUnknown *)from->stream[i]);
            if (to->stream[i]) IUnknown_Release((IUnknown *)to->stream[i]);
            to->stream[i] = from->stream[i]; to->stream_off[i] = from->stream_off[i]; to->stream_stride[i] = from->stream_stride[i];
        }
    }
#define SB_COPY_OBJ(bit, field) if (m->misc & (bit)) { if (from->field) IUnknown_AddRef((IUnknown *)from->field); if (to->field) IUnknown_Release((IUnknown *)to->field); to->field = from->field; }
    SB_COPY_OBJ(SB_INDICES, indices)
    SB_COPY_OBJ(SB_VS, vs)
    SB_COPY_OBJ(SB_PS, ps)
    SB_COPY_OBJ(SB_DECL, decl)
#undef SB_COPY_OBJ
}
/* re-issue the marked items of `st` through the device's setters */
static void sb_apply(struct device *dev, const struct sb_marks *m, const struct shadow_state *st)
{
    IDirect3DDevice9 *D = (IDirect3DDevice9 *)dev;
    UINT i, j;
    for (i = 0; i < MAX_RS; i++) if (m->rs[i / 32] & (1u << (i % 32))) dev_SetRenderState(D, (D3DRENDERSTATETYPE)i, st->rs[i]);
    for (i = 0; i < MAX_TSS_STAGES; i++) for (j = 0; j < 33; j++) if (m->tss[i] & (1ull << j)) dev_SetTextureStageState(D, i, (D3DTEXTURESTAGESTATETYPE)j, st->tss[i][j]);
    for (i = 0; i < MAX_SAMPLERS; i++) for (j = 0; j < 14; j++) if (m->samp[i] & (1u << j)) dev_SetSamplerState(D, i, (D3DSAMPLERSTATETYPE)j, st->samp[i][j]);
    if (m->xform & 1) dev_SetTransform(D, D3DTS_WORLD, &st->world);
    if (m->xform & 2) dev_SetTransform(D, D3DTS_VIEW, &st->view);
    if (m->xform & 4) dev_SetTransform(D, D3DTS_PROJECTION, &st->proj);
    for (i = 0; i < 8; i++) if (m->xform & (8u << i)) dev_SetTransform(D, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &st->tex[i]);
    if (m->misc & SB_VP) dev_SetViewport(D, &st->vp);
    if (m->misc & SB_MATERIAL) dev_SetMaterial(D, &st->material);
    if (m->misc & SB_SCISSOR) dev_SetScissorRect(D, &st->scissor);
    for (i = 0; i < 8; i++) { if (m->lights & (1u << i)) dev_SetLight(D, i, &st->lights[i]); if (m->light_on & (1u << i)) dev_LightEnable(D, i, st->light_on[i]); }
    for (i = 0; i < 6; i++) if (m->clip & (1u << i)) dev_SetClipPlane(D, i, st->clip[i]);
    for (i = 0; i < 256; i++) if (m->vs_f[i / 32] & (1u << (i % 32))) dev_SetVertexShaderConstantF(D, i, st->vs_f[i], 1);
    for (i = 0; i < 224; i++) if (m->ps_f[i / 32] & (1u << (i % 32))) dev_SetPixelShaderConstantF(D, i, st->ps_f[i], 1);
    for (i = 0; i < 16; i++) {
        if (m->vs_i & (1u << i)) dev_SetVertexShaderConstantI(D, i, st->vs_i[i], 1);
        if (m->ps_i & (1u << i)) dev_SetPixelShaderConstantI(D, i, st->ps_i[i], 1);
        if (m->vs_b & (1u << i)) dev_SetVertexShaderConstantB(D, i, &st->vs_b[i], 1);
        if (m->ps_b & (1u << i)) dev_SetPixelShaderConstantB(D, i, &st->ps_b[i], 1);
    }
    for (i = 0; i < 16; i++) {
        if (m->textures & (1u << i)) dev_SetTexture(D, i, (IDirect3DBaseTexture9 *)st->tex_bound[i]);
        if (m->streams & (1u << i)) dev_SetStreamSource(D, i, (IDirect3DVertexBuffer9 *)st->stream[i], st->stream_off[i], st->stream_stride[i]);
    }
    if (m->misc & SB_INDICES) dev_SetIndices(D, (IDirect3DIndexBuffer9 *)st->indices);
    if (m->misc & SB_DECL) dev_SetVertexDeclaration(D, (IDirect3DVertexDeclaration9 *)st->decl);
    if (m->misc & SB_FVF) dev_SetFVF(D, st->fvf);      /* after the declaration: FVF wins when both are recorded, like native */
    if (m->misc & SB_VS) dev_SetVertexShader(D, (IDirect3DVertexShader9 *)st->vs);
    if (m->misc & SB_PS) dev_SetPixelShader(D, (IDirect3DPixelShader9 *)st->ps);
}
static void sb_mark_all(struct sb_marks *m, D3DSTATEBLOCKTYPE type)
{
    UINT i;
    memset(m, 0, sizeof *m);
    if (type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE) {
        static const DWORD pixel_rs[] = { D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL,
            D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
            D3DRS_ALPHABLENDENABLE, D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC,
            D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4,
            D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15,
            D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE,
            D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
            D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA };
        for (i = 0; i < sizeof pixel_rs / sizeof pixel_rs[0]; i++) m->rs[pixel_rs[i] / 32] |= 1u << (pixel_rs[i] % 32);
        for (i = 0; i < MAX_TSS_STAGES; i++) m->tss[i] = (1ull << 33) - 1;
        for (i = 0; i < MAX_SAMPLERS; i++) m->samp[i] = 0x3fff;
        m->misc |= SB_PS; memset(m->ps_f, 0xff, sizeof m->ps_f); m->ps_i = m->ps_b = 0xffff;
    }
    if (type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE) {
        static const DWORD vertex_rs[] = { D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
            D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_LOCALVIEWER, D3DRS_NORMALIZENORMALS,
            D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE,
            D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C,
            D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR,
            D3DRS_POSITIONDEGREE, D3DRS_NORMALDEGREE, D3DRS_MINTESSELLATIONLEVEL, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y,
            D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W, D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_SHADEMODE, D3DRS_SPECULARENABLE };
        for (i = 0; i < sizeof vertex_rs / sizeof vertex_rs[0]; i++) m->rs[vertex_rs[i] / 32] |= 1u << (vertex_rs[i] % 32);
        for (i = 0; i < MAX_TSS_STAGES; i++) m->tss[i] |= (1ull << D3DTSS_TEXCOORDINDEX) | (1ull << D3DTSS_TEXTURETRANSFORMFLAGS);
        m->lights = m->light_on = 0xff; m->clip = 0x3f;
        m->misc |= SB_VS | SB_DECL | SB_FVF; memset(m->vs_f, 0xff, sizeof m->vs_f); m->vs_i = m->vs_b = 0xffff;
        for (i = 0; i < 16; i++) m->samp[i] |= 1u << D3DSAMP_DMAPOFFSET;
    }
    if (type == D3DSBT_ALL) {
        memset(m->rs, 0xff, sizeof m->rs);
        m->xform = 0x7ff; m->misc |= SB_VP | SB_MATERIAL | SB_SCISSOR | SB_INDICES;
        m->textures = m->streams = 0xffff;
    }
}
HRESULT WINAPI sb_QueryInterface(IDirect3DStateBlock9 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DStateBlock9)) { *ppv = This; InterlockedIncrement(&((struct res_hdr *)This)->ref); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
ULONG WINAPI sb_AddRef(IDirect3DStateBlock9 *This) { return InterlockedIncrement(&((struct res_hdr *)This)->ref); }
ULONG WINAPI sb_Release(IDirect3DStateBlock9 *This)
{
    struct stateblock *b = (struct stateblock *)This;
    LONG r = InterlockedDecrement(&b->h.ref);
    if (r == 0) { sb_release_objects(&b->st); res_free(&b->h); }
    return r;
}
HRESULT WINAPI sb_GetDevice(IDirect3DStateBlock9 *This, IDirect3DDevice9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3DDevice9 *)((struct res_hdr *)This)->dev; IDirect3DDevice9_AddRef(*pp); return D3D_OK;
}
HRESULT WINAPI sb_Capture(IDirect3DStateBlock9 *This)
{
    struct stateblock *b = (struct stateblock *)This;
    if (b->h.dev->recording) return D3DERR_INVALIDCALL;
    sb_copy(&b->marks, &b->h.dev->st, &b->st);
    return D3D_OK;
}
HRESULT WINAPI sb_Apply(IDirect3DStateBlock9 *This)
{
    struct stateblock *b = (struct stateblock *)This;
    if (b->h.dev->recording) return D3DERR_INVALIDCALL;
    sb_apply(b->h.dev, &b->marks, &b->st);
    return D3D_OK;
}
static struct stateblock *sb_new(struct device *dev)
{
    struct stateblock *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *b);
    if (!b) return NULL;
    res_init(&b->h, &sb_vtbl, dev);
    b->h.handle = 0;        /* guest-only */
    return b;
}
HRESULT WINAPI dev_CreateStateBlock(IDirect3DDevice9 *This, D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **pp)
{
    struct device *dev = DEV(This);
    struct stateblock *b;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (dev->recording || (Type != D3DSBT_ALL && Type != D3DSBT_PIXELSTATE && Type != D3DSBT_VERTEXSTATE)) return D3DERR_INVALIDCALL;
    b = sb_new(dev);
    if (!b) return E_OUTOFMEMORY;
    sb_mark_all(&b->marks, Type);
    sb_copy(&b->marks, &dev->st, &b->st);
    *pp = (IDirect3DStateBlock9 *)b;
    return D3D_OK;
}
HRESULT WINAPI dev_BeginStateBlock(IDirect3DDevice9 *This)
{
    struct device *dev = DEV(This);
    if (dev->recording) return D3DERR_INVALIDCALL;
    dev->recording = sb_new(dev);
    return dev->recording ? D3D_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI dev_EndStateBlock(IDirect3DDevice9 *This, IDirect3DStateBlock9 **pp)
{
    struct device *dev = DEV(This);
    struct stateblock *b = dev->recording;
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!b) return D3DERR_INVALIDCALL;
    dev->recording = NULL;
    sb_copy(&b->marks, &dev->st, &b->st);   /* the values set while recording are the block's contents */
    *pp = (IDirect3DStateBlock9 *)b;
    return D3D_OK;
}
