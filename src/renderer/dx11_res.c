/* The Neonix renderer, Direct3D 11 mode: textures, surfaces, vertex and index buffers.
   Textures keep the game's bytes in its own D3D8 format (the game locks them); an unlock converts the locked rectangle to
   B8G8R8A8 (or BC1-3 for DXT) and uploads it. Vertex buffers the game only writes (WRITEONLY, default pool) are D3D11 dynamic
   buffers: a lock maps them directly (DISCARD / NOOVERWRITE as asked). Others keep a system copy that is uploaded at unlock.
   Index buffers always keep a system copy: a triangle fan has to be rewritten as a list. */
#include "dx11_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BOOL nx_bgra = TRUE;
UINT nx_ms_supported;   /* B8G8R8A8 textures; FALSE: R8G8B8A8 and R/B swapped in the conversion */

static const NxFormat kFormats[] = {
    {D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4, 0, NXC_A8R8G8B8},
    /* for these four the dxgi format has the D3D8 bit layout: used as is where the card samples it (Nx_Native) */
    {D3DFMT_X8R8G8B8, DXGI_FORMAT_B8G8R8X8_UNORM, 4, 0, NXC_X8R8G8B8},
    {D3DFMT_R5G6B5, DXGI_FORMAT_B5G6R5_UNORM, 2, 0, NXC_R5G6B5},
    {D3DFMT_X1R5G5B5, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 0, NXC_X1R5G5B5},
    {D3DFMT_A1R5G5B5, DXGI_FORMAT_B5G5R5A1_UNORM, 2, 0, NXC_A1R5G5B5},
    {D3DFMT_A4R4G4B4, DXGI_FORMAT_B4G4R4A4_UNORM, 2, 0, NXC_A4R4G4B4},
    {D3DFMT_X4R4G4B4, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 0, NXC_X4R4G4B4},
    {D3DFMT_L8, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, NXC_L8},
    {D3DFMT_A8, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, NXC_A8},
    {D3DFMT_A8L8, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 0, NXC_A8L8},
    {D3DFMT_DXT1, DXGI_FORMAT_BC1_UNORM, 0, 8, NXC_COPY},
    {D3DFMT_DXT2, DXGI_FORMAT_BC2_UNORM, 0, 16, NXC_COPY},
    {D3DFMT_DXT3, DXGI_FORMAT_BC2_UNORM, 0, 16, NXC_COPY},
    {D3DFMT_DXT4, DXGI_FORMAT_BC3_UNORM, 0, 16, NXC_COPY},
    {D3DFMT_DXT5, DXGI_FORMAT_BC3_UNORM, 0, 16, NXC_COPY},
    {D3DFMT_D24S8, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, 0, NXC_COPY},
    {D3DFMT_D24X8, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, 0, NXC_COPY},
    {D3DFMT_D24X4S4, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, 0, NXC_COPY},
    {D3DFMT_D16, DXGI_FORMAT_D16_UNORM, 2, 0, NXC_COPY},
    {D3DFMT_D16_LOCKABLE, DXGI_FORMAT_D16_UNORM, 2, 0, NXC_COPY},
    {D3DFMT_D32, DXGI_FORMAT_D32_FLOAT, 4, 0, NXC_COPY},
    {D3DFMT_D15S1, DXGI_FORMAT_D24_UNORM_S8_UINT, 2, 0, NXC_COPY},
};

static UINT nx_native_mask;   /* bit n: kFormats[n] is uploaded as it is (no conversion to 32 bits) */

void Nx_CheckNativeFormats(ID3D11Device *d)
{
    const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                      D3D11_FORMAT_SUPPORT_MIP;
    unsigned i;
    nx_native_mask = 0;
    for (i = 0; nx_bgra && i < sizeof kFormats / sizeof kFormats[0]; ++i) {
        UINT support = 0;
        if (kFormats[i].block || kFormats[i].convert == NXC_COPY || kFormats[i].dxgi == DXGI_FORMAT_B8G8R8A8_UNORM) continue;
        if (SUCCEEDED(ID3D11Device_CheckFormatSupport(d, kFormats[i].dxgi, &support)) && (support & need) == need)
            nx_native_mask |= 1u << i;
    }
    Nx_Log("textures kept in their own format: %s%s%s%s", nx_native_mask & 2 ? "X8R8G8B8 " : "", nx_native_mask & 4 ? "R5G6B5 " : "",
           nx_native_mask & 16 ? "A1R5G5B5 " : "", nx_native_mask & 32 ? "A4R4G4B4" : "");
}

BOOL Nx_Native(const NxFormat *f)
{
    return f >= kFormats && f < kFormats + sizeof kFormats / sizeof kFormats[0] && ((nx_native_mask >> (f - kFormats)) & 1);
}

const NxFormat *Nx_Format(D3DFORMAT f)
{
    unsigned i;
    for (i = 0; i < sizeof kFormats / sizeof kFormats[0]; ++i)
        if (kFormats[i].d3d == f) return &kFormats[i];
    return NULL;
}

static BOOL IsDepth(D3DFORMAT f)
{
    return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D24X4S4 || f == D3DFMT_D16 || f == D3DFMT_D16_LOCKABLE ||
           f == D3DFMT_D32 || f == D3DFMT_D15S1;
}

static BYTE Five(UINT v) { return (BYTE)((v << 3) | (v >> 2)); }
static BYTE Six(UINT v) { return (BYTE)((v << 2) | (v >> 4)); }

/* one row of D3D8 pixels -> B8G8R8A8 */
void Nx_ConvertRow(BYTE convert, const BYTE *s, BYTE *d, UINT n)
{
    UINT i;
    const WORD *w = (const WORD *)s;
    switch (convert) {
    case NXC_A8R8G8B8: memcpy(d, s, n * 4); break;
    case NXC_X8R8G8B8: for (i = 0; i < n; ++i) { d[4 * i] = s[4 * i]; d[4 * i + 1] = s[4 * i + 1]; d[4 * i + 2] = s[4 * i + 2]; d[4 * i + 3] = 255; } break;
    case NXC_R8G8B8: for (i = 0; i < n; ++i) { d[4 * i] = s[3 * i]; d[4 * i + 1] = s[3 * i + 1]; d[4 * i + 2] = s[3 * i + 2]; d[4 * i + 3] = 255; } break;
    case NXC_R5G6B5:
        for (i = 0; i < n; ++i) { UINT v = w[i]; d[4 * i] = Five(v & 31); d[4 * i + 1] = Six((v >> 5) & 63); d[4 * i + 2] = Five(v >> 11); d[4 * i + 3] = 255; }
        break;
    case NXC_X1R5G5B5:
    case NXC_A1R5G5B5:
        for (i = 0; i < n; ++i) {
            UINT v = w[i];
            d[4 * i] = Five(v & 31); d[4 * i + 1] = Five((v >> 5) & 31); d[4 * i + 2] = Five((v >> 10) & 31);
            d[4 * i + 3] = convert == NXC_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
        }
        break;
    case NXC_A4R4G4B4:
    case NXC_X4R4G4B4:
        for (i = 0; i < n; ++i) {
            UINT v = w[i];
            d[4 * i] = (BYTE)((v & 15) * 17); d[4 * i + 1] = (BYTE)(((v >> 4) & 15) * 17); d[4 * i + 2] = (BYTE)(((v >> 8) & 15) * 17);
            d[4 * i + 3] = convert == NXC_X4R4G4B4 ? 255 : (BYTE)(((v >> 12) & 15) * 17);
        }
        break;
    case NXC_L8: for (i = 0; i < n; ++i) { d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = s[i]; d[4 * i + 3] = 255; } break;
    case NXC_A8: for (i = 0; i < n; ++i) { d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = 0; d[4 * i + 3] = s[i]; } break;
    case NXC_A8L8: for (i = 0; i < n; ++i) { d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = s[2 * i]; d[4 * i + 3] = s[2 * i + 1]; } break;
    default: memcpy(d, s, n * 4); break;
    }
    if (!nx_bgra)
        for (i = 0; i < n; ++i) { BYTE t = d[4 * i]; d[4 * i] = d[4 * i + 2]; d[4 * i + 2] = t; }
}

/* B8G8R8A8 -> one row of the D3D8 format (reading back the back buffer and render targets) */
static void UnconvertRow(D3DFORMAT f, const BYTE *s, BYTE *d, UINT n)
{
    UINT i;
    WORD *w = (WORD *)d;
    switch (f) {
    case D3DFMT_R5G6B5: for (i = 0; i < n; ++i) w[i] = (WORD)(((s[4 * i + 2] >> 3) << 11) | ((s[4 * i + 1] >> 2) << 5) | (s[4 * i] >> 3)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        for (i = 0; i < n; ++i)
            w[i] = (WORD)((s[4 * i + 3] >= 128 ? 0x8000 : 0) | ((s[4 * i + 2] >> 3) << 10) | ((s[4 * i + 1] >> 3) << 5) | (s[4 * i] >> 3));
        break;
    default: memcpy(d, s, n * 4); break;
    }
}

static UINT Pitch(const NxFormat *fmt, UINT w)
{
    if (fmt->block) return ((w + 3) / 4) * fmt->block;
    return (w * fmt->bpp + 3) & ~3u;
}
static UINT Rows(const NxFormat *fmt, UINT h) { return fmt->block ? (h + 3) / 4 : h; }

/* ---- private data ---- */
HRESULT Nx_PrivateSet(NxHead *h, REFGUID id, const void *data, DWORD size, DWORD flags)
{
    NxPrivate *p;
    Nx_PrivateFree(h, id);
    if (flags & D3DSPD_IUNKNOWN) size = sizeof(IUnknown *);
    p = (NxPrivate *)malloc(sizeof *p + size);
    if (!p) return E_OUTOFMEMORY;
    p->id = *id;
    p->size = size;
    p->flags = flags;
    memcpy(p->data, data, size);
    if (flags & D3DSPD_IUNKNOWN) (*(IUnknown **)p->data)->lpVtbl->AddRef(*(IUnknown **)p->data);
    p->next = h->priv;
    h->priv = p;
    return D3D_OK;
}

HRESULT Nx_PrivateGet(NxHead *h, REFGUID id, void *data, DWORD *size)
{
    NxPrivate *p;
    for (p = h->priv; p; p = p->next) {
        if (memcmp(&p->id, id, sizeof(GUID))) continue;
        if (!data) { *size = p->size; return D3D_OK; }
        if (*size < p->size) { *size = p->size; return D3DERR_MOREDATA; }
        memcpy(data, p->data, p->size);
        if (p->flags & D3DSPD_IUNKNOWN) (*(IUnknown **)p->data)->lpVtbl->AddRef(*(IUnknown **)p->data);
        *size = p->size;
        return D3D_OK;
    }
    return D3DERR_NOTFOUND;
}

HRESULT Nx_PrivateFree(NxHead *h, REFGUID id)
{
    NxPrivate **pp;
    for (pp = &h->priv; *pp; pp = &(*pp)->next) {
        NxPrivate *p = *pp;
        if (memcmp(&p->id, id, sizeof(GUID))) continue;
        *pp = p->next;
        if (p->flags & D3DSPD_IUNKNOWN) (*(IUnknown **)p->data)->lpVtbl->Release(*(IUnknown **)p->data);
        free(p);
        return D3D_OK;
    }
    return D3DERR_NOTFOUND;
}

void Nx_PrivateClear(NxHead *h)
{
    while (h->priv) Nx_PrivateFree(h, &h->priv->id);
}

/* ---- surfaces ---- */
ID3D11Texture2D *Nx_SurfaceTexture(NxSurface *s, UINT *sub)
{
    if (s->container) {
        *sub = s->face * s->container->levels + s->level;
        return s->container->tex;
    }
    *sub = 0;
    return s->tex;
}

static BYTE *SurfaceMemory(NxSurface *s)
{
    if (!s->mem) {
        s->pitch = Pitch(s->fmt, s->width);
        s->mem = (BYTE *)calloc(Rows(s->fmt, s->height), s->pitch);
    }
    return s->mem;
}

/* the surface's rectangle from s->mem to its D3D11 texture */
static void Upload(NxSurface *s, const RECT *r)
{
    NxDevice *dev = s->h.dev;
    UINT sub, y, rows, w, h;
    ID3D11Texture2D *tex = Nx_SurfaceTexture(s, &sub);
    D3D11_BOX box;
    RECT all;
    if (!tex || !s->mem) return;
    if (!r) { all.left = 0; all.top = 0; all.right = (LONG)s->width; all.bottom = (LONG)s->height; r = &all; }
    box.left = (UINT)r->left; box.top = (UINT)r->top; box.right = (UINT)r->right; box.bottom = (UINT)r->bottom;
    box.front = 0; box.back = 1;
    if (box.right > s->width) box.right = s->width;
    if (box.bottom > s->height) box.bottom = s->height;
    if (box.left >= box.right || box.top >= box.bottom) return;
    ++nx_stats.tex_uploads;
    nx_stats.tex_kb += (box.right - box.left) * (box.bottom - box.top) * 4 / 1024;
    if (s->fmt->block) {
        box.left &= ~3u; box.top &= ~3u;
        box.right = (box.right + 3) & ~3u; box.bottom = (box.bottom + 3) & ~3u;
        /* a level smaller than a block is a whole block */
        if (box.right > ((s->width + 3) & ~3u)) box.right = (s->width + 3) & ~3u;
        if (box.bottom > ((s->height + 3) & ~3u)) box.bottom = (s->height + 3) & ~3u;
        {
            BOOL whole = s->width < 4 || s->height < 4;
            UINT rows = whole ? (s->height + 3) / 4 : (box.bottom - box.top) / 4;
            UINT row_bytes = whole ? ((s->width + 3) / 4) * s->fmt->block : ((box.right - box.left) / 4) * s->fmt->block;
            const BYTE *from = whole ? s->mem : s->mem + (box.top / 4) * s->pitch + (box.left / 4) * s->fmt->block;
            NxC_UpdateSubresource(dev, (ID3D11Resource *)tex, sub, whole ? NULL : &box, from, s->pitch, 0,
                                  rows ? (rows - 1) * s->pitch + row_bytes : 0);
        }
        return;
    }
    w = box.right - box.left;
    h = box.bottom - box.top;
    if (s->fmt->convert == NXC_COPY || (s->fmt->convert == NXC_A8R8G8B8 && nx_bgra) || (s->container && s->container->native)) {
        NxC_UpdateSubresource(dev, (ID3D11Resource *)tex, sub, &box, s->mem + box.top * s->pitch + box.left * s->fmt->bpp, s->pitch, 0,
                              (h - 1) * s->pitch + w * s->fmt->bpp);
        return;
    }
    {
        BYTE *tmp = (BYTE *)malloc((size_t)w * h * 4);
        if (!tmp) return;
        rows = h;
        for (y = 0; y < rows; ++y)
            Nx_ConvertRow(s->fmt->convert, s->mem + (box.top + y) * s->pitch + box.left * s->fmt->bpp, tmp + (size_t)y * w * 4, w);
        NxC_UpdateSubresource(dev, (ID3D11Resource *)tex, sub, &box, tmp, w * 4, 0, w * 4 * h);
        free(tmp);
    }
}

/* GPU surface (back buffer, render target) -> s->mem in the surface's D3D8 format */
HRESULT Nx_ReadSurface(NxSurface *s)
{
    NxDevice *dev = s->h.dev;
    UINT sub, y;
    ID3D11Texture2D *src = Nx_SurfaceTexture(s, &sub), *staging = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    if (!src || !SurfaceMemory(s) || s->fmt->block || IsDepth(s->format)) return D3DERR_INVALIDCALL;
    ++nx_stats.readbacks;
    NxQ_Sync(dev);   /* everything drawn so far has run; the context is this thread's until it records again */
    if (s->samples > 1) { src = Nx_Resolve(s); sub = 0; NxQ_Sync(dev); }
    if (!src) return D3DERR_INVALIDCALL;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Width = s->width; d.Height = s->height; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &staging))) return D3DERR_INVALIDCALL;
    box.left = 0; box.top = 0; box.front = 0; box.right = s->width; box.bottom = s->height; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(dev->c, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)src, sub, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(dev->c, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < s->height; ++y) {
            const BYTE *row = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
            if (!nx_bgra) {
                BYTE tmp[16384];
                UINT x, n = s->width < 4096 ? s->width : 4096;
                for (x = 0; x < n; ++x) { tmp[4 * x] = row[4 * x + 2]; tmp[4 * x + 1] = row[4 * x + 1]; tmp[4 * x + 2] = row[4 * x]; tmp[4 * x + 3] = row[4 * x + 3]; }
                UnconvertRow(s->format, tmp, s->mem + y * s->pitch, n);
            } else {
                UnconvertRow(s->format, row, s->mem + y * s->pitch, s->width);
            }
        }
        ID3D11DeviceContext_Unmap(dev->c, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return D3D_OK;
}

static void SurfaceDestroy(NxSurface *s)
{
    /* through the render thread's queue when there is one: draws recorded before may still use them */
    NxC_Release(s->h.dev, s->rtv);
    NxC_Release(s->h.dev, s->dsv);
    NxC_Release(s->h.dev, s->srv);
    NxC_Release(s->h.dev, s->resolved_srv);
    NxC_Release(s->h.dev, s->resolved);
    NxC_Release(s->h.dev, s->tex);
    Nx_PrivateClear(&s->h);
    free(s->mem);
    free(s);
}

static HRESULT STDMETHODCALLTYPE Surf_QueryInterface(IDirect3DSurface8 *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(riid, &nxt_IID_IDirect3DSurface8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {
        This->lpVtbl->AddRef(This);
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Surf_AddRef(IDirect3DSurface8 *This)
{
    NxSurface *s = (NxSurface *)This;
    if (s->container) return ((IUnknown *)s->container)->lpVtbl->AddRef((IUnknown *)s->container);
    return (ULONG)InterlockedIncrement(&s->h.refs);
}
static ULONG STDMETHODCALLTYPE Surf_Release(IDirect3DSurface8 *This)
{
    NxSurface *s = (NxSurface *)This;
    LONG r;
    if (s->container) return ((IUnknown *)s->container)->lpVtbl->Release((IUnknown *)s->container);
    r = InterlockedDecrement(&s->h.refs);
    if (r == 0) SurfaceDestroy(s);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE Surf_GetDevice(IDirect3DSurface8 *This, IDirect3DDevice8 **out)
{
    NxSurface *s = (NxSurface *)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DDevice8 *)s->h.dev;
    (*out)->lpVtbl->AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE Surf_SetPrivateData(IDirect3DSurface8 *This, REFGUID id, const void *data, DWORD size, DWORD flags)
{ return Nx_PrivateSet(&((NxSurface *)This)->h, id, data, size, flags); }
static HRESULT STDMETHODCALLTYPE Surf_GetPrivateData(IDirect3DSurface8 *This, REFGUID id, void *data, DWORD *size)
{ return Nx_PrivateGet(&((NxSurface *)This)->h, id, data, size); }
static HRESULT STDMETHODCALLTYPE Surf_FreePrivateData(IDirect3DSurface8 *This, REFGUID id)
{ return Nx_PrivateFree(&((NxSurface *)This)->h, id); }
static HRESULT STDMETHODCALLTYPE Surf_GetContainer(IDirect3DSurface8 *This, REFIID riid, void **out)
{
    NxSurface *s = (NxSurface *)This;
    if (!out) return D3DERR_INVALIDCALL;
    if (s->container) return ((IUnknown *)s->container)->lpVtbl->QueryInterface((IUnknown *)s->container, riid, out);
    return ((IUnknown *)s->h.dev)->lpVtbl->QueryInterface((IUnknown *)s->h.dev, riid, out);
}
static HRESULT STDMETHODCALLTYPE Surf_GetDesc(IDirect3DSurface8 *This, D3DSURFACE_DESC *d)
{
    NxSurface *s = (NxSurface *)This;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = s->format;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = s->usage;
    d->Pool = s->pool;
    d->Size = s->fmt ? Pitch(s->fmt, s->width) * Rows(s->fmt, s->height) : s->width * s->height * 4;
    d->MultiSampleType = s->ms;
    d->Width = s->width;
    d->Height = s->height;
    return D3D_OK;
}
static HRESULT Surf_LockRectImpl(IDirect3DSurface8 *This, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    NxSurface *s = (NxSurface *)This;
    BYTE *mem;
    if (!lr || s->locked || IsDepth(s->format)) return D3DERR_INVALIDCALL;
    /* render targets: only the lockable back buffer and image copies can be read */
    if (!s->container && s->tex && (s->usage & D3DUSAGE_RENDERTARGET)) {
        if (s != s->h.dev->backbuffer && s->pool == D3DPOOL_DEFAULT && !(s->h.dev->pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER))
            return D3DERR_INVALIDCALL;
        if (FAILED(Nx_ReadSurface(s))) return D3DERR_INVALIDCALL;
    } else if (s->container && (s->container->usage & D3DUSAGE_RENDERTARGET)) {
        return D3DERR_INVALIDCALL;
    }
    mem = SurfaceMemory(s);
    if (!mem) return E_OUTOFMEMORY;
    lr->Pitch = (INT)s->pitch;
    if (rect) {
        if (s->fmt->block) lr->pBits = mem + (rect->top / 4) * s->pitch + (rect->left / 4) * s->fmt->block;
        else lr->pBits = mem + rect->top * s->pitch + rect->left * s->fmt->bpp;
        s->dirty_rect = *rect;
    } else {
        lr->pBits = mem;
        s->dirty_rect.left = 0; s->dirty_rect.top = 0; s->dirty_rect.right = (LONG)s->width; s->dirty_rect.bottom = (LONG)s->height;
    }
    s->locked = TRUE;
    s->dirty = !(flags & D3DLOCK_READONLY);
    return D3D_OK;
}
static HRESULT Surf_UnlockRectImpl(IDirect3DSurface8 *This)
{
    NxSurface *s = (NxSurface *)This;
    if (!s->locked) return D3DERR_INVALIDCALL;
    s->locked = FALSE;
    if (s->dirty) {
        s->dirty = FALSE;
        if (s->container || (s->tex && !(s->usage & D3DUSAGE_RENDERTARGET))) Upload(s, &s->dirty_rect);
        else if (s->tex) Upload(s, &s->dirty_rect);   /* written back buffer: rare (game draws into it by locking) */
    }
    return D3D_OK;
}

/* locks timed for the frame report */
static HRESULT STDMETHODCALLTYPE Surf_LockRect(IDirect3DSurface8 *This, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Surf_LockRectImpl(This, lr, rect, flags);
    nx_stats.t_lock += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE Surf_UnlockRect(IDirect3DSurface8 *This)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Surf_UnlockRectImpl(This);
    nx_stats.t_lock += Nx_Now() - t0;
    return hr;
}

static IDirect3DSurface8Vtbl kSurfaceVtbl = {
    .QueryInterface = Surf_QueryInterface, .AddRef = Surf_AddRef, .Release = Surf_Release, .GetDevice = Surf_GetDevice,
    .SetPrivateData = Surf_SetPrivateData, .GetPrivateData = Surf_GetPrivateData, .FreePrivateData = Surf_FreePrivateData,
    .GetContainer = Surf_GetContainer, .GetDesc = Surf_GetDesc, .LockRect = Surf_LockRect, .UnlockRect = Surf_UnlockRect,
};
void *nx_surface_vtbl = &kSurfaceVtbl;

static NxSurface *NewSurfaceObject(NxDevice *dev, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool)
{
    NxSurface *s = (NxSurface *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->h.vtbl = &kSurfaceVtbl;
    s->h.refs = 1;
    s->h.kind = NXK_SURFACE;
    s->h.dev = dev;
    s->format = f;
    s->fmt = Nx_Format(f);
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->usage = usage;
    s->pool = pool;
    return s;
}

/* standalone surfaces: render targets (and the back buffer), depth stencils, image (system memory) surfaces */
HRESULT Nx_NewSurface(NxDevice *dev, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms, NxSurface **out)
{
    NxSurface *s;
    D3D11_TEXTURE2D_DESC d;
    HRESULT hr;
    if (!out || !Nx_Format(f)) return D3DERR_INVALIDCALL;
    s = NewSurfaceObject(dev, w, h, f, usage, pool);
    if (!s) return E_OUTOFMEMORY;
    s->ms = ms;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        memset(&d, 0, sizeof d);
        d.Width = s->width; d.Height = s->height; d.MipLevels = 1; d.ArraySize = 1;
        s->samples = ms >= 2 && ms <= 16 && (nx_ms_supported & (1u << ms)) ? (UINT)ms : 1;
        d.SampleDesc.Count = s->samples;
        d.Usage = D3D11_USAGE_DEFAULT;
        if (usage & D3DUSAGE_DEPTHSTENCIL) {
            d.Format = s->fmt->dxgi;
            d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        } else {
            d.Format = nx_bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | (s->samples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
        }
        hr = ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &s->tex);
        if (SUCCEEDED(hr)) {
            if (usage & D3DUSAGE_DEPTHSTENCIL)
                hr = ID3D11Device_CreateDepthStencilView(dev->d, (ID3D11Resource *)s->tex, NULL, &s->dsv);
            else {
                hr = ID3D11Device_CreateRenderTargetView(dev->d, (ID3D11Resource *)s->tex, NULL, &s->rtv);
                if (SUCCEEDED(hr) && s->samples == 1) hr = ID3D11Device_CreateShaderResourceView(dev->d, (ID3D11Resource *)s->tex, NULL, &s->srv);
            }
        }
        if (FAILED(hr)) {
            Nx_Log("surface %ux%u format %d usage %lx: %08lx", s->width, s->height, (int)f, (unsigned long)usage, (unsigned long)hr);
            SurfaceDestroy(s);
            return D3DERR_OUTOFVIDEOMEMORY;
        }
    }
    *out = s;
    return D3D_OK;
}

/* ---- textures ---- */
static void TextureDestroy(NxTexture *t)
{
    UINT i, n = t->levels * (t->cube ? 6 : 1);
    for (i = 0; i < n; ++i) if (t->surf[i]) SurfaceDestroy(t->surf[i]);
    free(t->surf);
    NxC_Release(t->h.dev, t->rtv);
    NxC_Release(t->h.dev, t->srv);
    NxC_Release(t->h.dev, t->tex);
    Nx_PrivateClear(&t->h);
    free(t);
}

#define TEX_COMMON(prefix, IFACE)                                                                                          \
    static ULONG STDMETHODCALLTYPE prefix##_AddRef(IFACE *This) { return (ULONG)InterlockedIncrement(&((NxTexture *)This)->h.refs); } \
    static ULONG STDMETHODCALLTYPE prefix##_Release(IFACE *This)                                                         \
    {                                                                                                                     \
        LONG r = InterlockedDecrement(&((NxTexture *)This)->h.refs);                                                      \
        if (r == 0) TextureDestroy((NxTexture *)This);                                                                    \
        return (ULONG)r;                                                                                                  \
    }                                                                                                                     \
    static HRESULT STDMETHODCALLTYPE prefix##_GetDevice(IFACE *This, IDirect3DDevice8 **out)                              \
    {                                                                                                                     \
        if (!out) return D3DERR_INVALIDCALL;                                                                              \
        *out = (IDirect3DDevice8 *)((NxTexture *)This)->h.dev;                                                            \
        (*out)->lpVtbl->AddRef(*out);                                                                                     \
        return D3D_OK;                                                                                                    \
    }                                                                                                                     \
    static HRESULT STDMETHODCALLTYPE prefix##_SetPrivateData(IFACE *This, REFGUID id, const void *data, DWORD size, DWORD flags) \
    { return Nx_PrivateSet(&((NxTexture *)This)->h, id, data, size, flags); }                                             \
    static HRESULT STDMETHODCALLTYPE prefix##_GetPrivateData(IFACE *This, REFGUID id, void *data, DWORD *size)            \
    { return Nx_PrivateGet(&((NxTexture *)This)->h, id, data, size); }                                                    \
    static HRESULT STDMETHODCALLTYPE prefix##_FreePrivateData(IFACE *This, REFGUID id)                                    \
    { return Nx_PrivateFree(&((NxTexture *)This)->h, id); }                                                               \
    static DWORD STDMETHODCALLTYPE prefix##_SetPriority(IFACE *This, DWORD p)                                             \
    { DWORD o = ((NxTexture *)This)->h.priority; ((NxTexture *)This)->h.priority = p; return o; }                         \
    static DWORD STDMETHODCALLTYPE prefix##_GetPriority(IFACE *This) { return ((NxTexture *)This)->h.priority; }          \
    static void STDMETHODCALLTYPE prefix##_PreLoad(IFACE *This) { (void)This; }                                           \
    static DWORD STDMETHODCALLTYPE prefix##_SetLOD(IFACE *This, DWORD lod)                                                \
    { DWORD o = ((NxTexture *)This)->lod; ((NxTexture *)This)->lod = lod; return o; }                                     \
    static DWORD STDMETHODCALLTYPE prefix##_GetLOD(IFACE *This) { return ((NxTexture *)This)->lod; }                      \
    static DWORD STDMETHODCALLTYPE prefix##_GetLevelCount(IFACE *This) { return ((NxTexture *)This)->levels; }            \
    static HRESULT STDMETHODCALLTYPE prefix##_GetLevelDesc(IFACE *This, UINT level, D3DSURFACE_DESC *d)                   \
    {                                                                                                                     \
        NxTexture *t = (NxTexture *)This;                                                                                 \
        if (level >= t->levels) return D3DERR_INVALIDCALL;                                                                \
        return Surf_GetDesc((IDirect3DSurface8 *)t->surf[level], d);                                                      \
    }

TEX_COMMON(Tex, IDirect3DTexture8)
TEX_COMMON(Cube, IDirect3DCubeTexture8)

static HRESULT STDMETHODCALLTYPE Tex_QueryInterface(IDirect3DTexture8 *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(riid, &nxt_IID_IDirect3DTexture8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IDirect3DBaseTexture8, sizeof(GUID)) ||
        !memcmp(riid, &nxt_IID_IDirect3DResource8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {
        This->lpVtbl->AddRef(This);
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE Tex_GetType(IDirect3DTexture8 *This) { (void)This; return D3DRTYPE_TEXTURE; }
static HRESULT STDMETHODCALLTYPE Tex_GetSurfaceLevel(IDirect3DTexture8 *This, UINT level, IDirect3DSurface8 **out)
{
    NxTexture *t = (NxTexture *)This;
    if (!out || level >= t->levels) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface8 *)t->surf[level];
    This->lpVtbl->AddRef(This);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE Tex_LockRect(IDirect3DTexture8 *This, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    NxTexture *t = (NxTexture *)This;
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    return Surf_LockRect((IDirect3DSurface8 *)t->surf[level], lr, rect, flags);
}
static HRESULT STDMETHODCALLTYPE Tex_UnlockRect(IDirect3DTexture8 *This, UINT level)
{
    NxTexture *t = (NxTexture *)This;
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    return Surf_UnlockRect((IDirect3DSurface8 *)t->surf[level]);
}
static HRESULT STDMETHODCALLTYPE Tex_AddDirtyRect(IDirect3DTexture8 *This, const RECT *r) { (void)This; (void)r; return D3D_OK; }

static IDirect3DTexture8Vtbl kTextureVtbl = {
    .QueryInterface = Tex_QueryInterface, .AddRef = Tex_AddRef, .Release = Tex_Release, .GetDevice = Tex_GetDevice,
    .SetPrivateData = Tex_SetPrivateData, .GetPrivateData = Tex_GetPrivateData, .FreePrivateData = Tex_FreePrivateData,
    .SetPriority = Tex_SetPriority, .GetPriority = Tex_GetPriority, .PreLoad = Tex_PreLoad, .GetType = Tex_GetType,
    .SetLOD = Tex_SetLOD, .GetLOD = Tex_GetLOD, .GetLevelCount = Tex_GetLevelCount, .GetLevelDesc = Tex_GetLevelDesc,
    .GetSurfaceLevel = Tex_GetSurfaceLevel, .LockRect = Tex_LockRect, .UnlockRect = Tex_UnlockRect, .AddDirtyRect = Tex_AddDirtyRect,
};
void *nx_texture_vtbl = &kTextureVtbl;

static HRESULT STDMETHODCALLTYPE Cube_QueryInterface(IDirect3DCubeTexture8 *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(riid, &nxt_IID_IDirect3DCubeTexture8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IDirect3DBaseTexture8, sizeof(GUID)) ||
        !memcmp(riid, &nxt_IID_IDirect3DResource8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {
        This->lpVtbl->AddRef(This);
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE Cube_GetType(IDirect3DCubeTexture8 *This) { (void)This; return D3DRTYPE_CUBETEXTURE; }
static HRESULT STDMETHODCALLTYPE Cube_GetCubeMapSurface(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8 **out)
{
    NxTexture *t = (NxTexture *)This;
    if (!out || level >= t->levels || (UINT)face >= 6) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface8 *)t->surf[face * t->levels + level];
    This->lpVtbl->AddRef(This);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE Cube_LockRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr,
                                               const RECT *rect, DWORD flags)
{
    NxTexture *t = (NxTexture *)This;
    if (level >= t->levels || (UINT)face >= 6) return D3DERR_INVALIDCALL;
    return Surf_LockRect((IDirect3DSurface8 *)t->surf[face * t->levels + level], lr, rect, flags);
}
static HRESULT STDMETHODCALLTYPE Cube_UnlockRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES face, UINT level)
{
    NxTexture *t = (NxTexture *)This;
    if (level >= t->levels || (UINT)face >= 6) return D3DERR_INVALIDCALL;
    return Surf_UnlockRect((IDirect3DSurface8 *)t->surf[face * t->levels + level]);
}
static HRESULT STDMETHODCALLTYPE Cube_AddDirtyRect(IDirect3DCubeTexture8 *This, D3DCUBEMAP_FACES face, const RECT *r)
{ (void)This; (void)face; (void)r; return D3D_OK; }

static IDirect3DCubeTexture8Vtbl kCubeVtbl = {
    .QueryInterface = Cube_QueryInterface, .AddRef = Cube_AddRef, .Release = Cube_Release, .GetDevice = Cube_GetDevice,
    .SetPrivateData = Cube_SetPrivateData, .GetPrivateData = Cube_GetPrivateData, .FreePrivateData = Cube_FreePrivateData,
    .SetPriority = Cube_SetPriority, .GetPriority = Cube_GetPriority, .PreLoad = Cube_PreLoad, .GetType = Cube_GetType,
    .SetLOD = Cube_SetLOD, .GetLOD = Cube_GetLOD, .GetLevelCount = Cube_GetLevelCount, .GetLevelDesc = Cube_GetLevelDesc,
    .GetCubeMapSurface = Cube_GetCubeMapSurface, .LockRect = Cube_LockRect, .UnlockRect = Cube_UnlockRect,
    .AddDirtyRect = Cube_AddDirtyRect,
};
void *nx_cube_vtbl = &kCubeVtbl;

HRESULT Nx_NewTexture(NxDevice *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, BOOL cube, NxTexture **out)
{
    NxTexture *t;
    const NxFormat *fmt = Nx_Format(f);
    D3D11_TEXTURE2D_DESC d;
    UINT max = 1, x, i, faces = cube ? 6 : 1;
    HRESULT hr;
    if (!out || !fmt || IsDepth(f) || !w || !h) return D3DERR_INVALIDCALL;
    for (x = w > h ? w : h; x > 1; x >>= 1) ++max;
    if (!levels || levels > max) levels = max;
    if (fmt->block && (usage & D3DUSAGE_RENDERTARGET)) return D3DERR_INVALIDCALL;
    t = (NxTexture *)calloc(1, sizeof *t);
    if (!t) return E_OUTOFMEMORY;
    t->h.vtbl = cube ? (void *)&kCubeVtbl : (void *)&kTextureVtbl;
    t->h.refs = 1;
    t->h.kind = cube ? NXK_CUBE : NXK_TEXTURE;
    t->h.dev = dev;
    t->cube = cube;
    t->format = f;
    t->fmt = fmt;
    t->width = w;
    t->height = h;
    t->levels = levels;
    t->usage = usage;
    t->pool = pool;
    t->surf = (NxSurface **)calloc(levels * faces, sizeof *t->surf);
    if (!t->surf) { free(t); return E_OUTOFMEMORY; }
    for (i = 0; i < levels * faces; ++i) {
        UINT lv = i % levels, lw = w >> lv, lh = h >> lv;
        NxSurface *s = NewSurfaceObject(dev, lw ? lw : 1, lh ? lh : 1, f, usage, pool);
        if (!s) { TextureDestroy(t); return E_OUTOFMEMORY; }
        s->container = t;
        s->level = lv;
        s->face = i / levels;
        t->surf[i] = s;
    }
    memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = levels; d.ArraySize = faces;
    t->native = !(usage & D3DUSAGE_RENDERTARGET) && Nx_Native(fmt);
    d.Format = fmt->block || t->native ? fmt->dxgi : (nx_bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((usage & D3DUSAGE_RENDERTARGET) ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (fmt->block && ((w & 3) || (h & 3))) {
        /* BC textures need whole blocks at the top level */
        d.Width = (w + 3) & ~3u;
        d.Height = (h + 3) & ~3u;
    }
    hr = ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &t->tex);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(dev->d, (ID3D11Resource *)t->tex, NULL, &t->srv);
    if (SUCCEEDED(hr) && (usage & D3DUSAGE_RENDERTARGET)) {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = d.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateRenderTargetView(dev->d, (ID3D11Resource *)t->tex, &rd, &t->rtv);
        if (SUCCEEDED(hr) && !cube) { t->surf[0]->rtv = t->rtv; ID3D11RenderTargetView_AddRef(t->rtv); }
    }
    if (FAILED(hr)) {
        Nx_Log("texture %ux%u levels %u format %d usage %lx: %08lx", w, h, levels, (int)f, (unsigned long)usage, (unsigned long)hr);
        TextureDestroy(t);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}

/* CopyRects: render target -> texture or image surface, image -> texture, and plain copies */
HRESULT Nx_CopyRects(NxDevice *dev, NxSurface *src, const RECT *rects, UINT n, NxSurface *dst, const POINT *points)
{
    RECT all;
    UINT i, ss, ds;
    ID3D11Texture2D *st = Nx_SurfaceTexture(src, &ss), *dt = Nx_SurfaceTexture(dst, &ds);
    BOOL src_gpu = st && (src->usage & D3DUSAGE_RENDERTARGET);
    if (!rects || !n) { all.left = 0; all.top = 0; all.right = (LONG)src->width; all.bottom = (LONG)src->height; rects = &all; n = 1; }
    if (src_gpu && src->samples > 1) { st = Nx_Resolve(src); ss = 0; if (!st) return D3DERR_INVALIDCALL; }
    if (src_gpu && dt && (dst->usage & D3DUSAGE_RENDERTARGET || dst->container) && !(dst->container && dst->container->native)) {
        for (i = 0; i < n; ++i) {
            D3D11_BOX box;
            UINT x = points ? (UINT)points[i].x : (UINT)rects[i].left, y = points ? (UINT)points[i].y : (UINT)rects[i].top;
            box.left = (UINT)rects[i].left; box.top = (UINT)rects[i].top; box.right = (UINT)rects[i].right; box.bottom = (UINT)rects[i].bottom;
            box.front = 0; box.back = 1;
            NxC_CopySubresourceRegion(dev, (ID3D11Resource *)dt, ds, x, y, 0, (ID3D11Resource *)st, ss, &box);
        }
        return D3D_OK;
    }
    if (src_gpu && FAILED(Nx_ReadSurface(src))) return D3DERR_INVALIDCALL;
    if (!SurfaceMemory(src) || !SurfaceMemory(dst) || src->fmt != dst->fmt || src->fmt->block) return D3DERR_INVALIDCALL;
    for (i = 0; i < n; ++i) {
        UINT x = points ? (UINT)points[i].x : (UINT)rects[i].left, y = points ? (UINT)points[i].y : (UINT)rects[i].top, row;
        UINT w = (UINT)(rects[i].right - rects[i].left), h = (UINT)(rects[i].bottom - rects[i].top);
        RECT dr;
        if (x + w > dst->width || y + h > dst->height) return D3DERR_INVALIDCALL;
        for (row = 0; row < h; ++row)
            memcpy(dst->mem + (y + row) * dst->pitch + x * dst->fmt->bpp,
                   src->mem + (rects[i].top + row) * src->pitch + rects[i].left * src->fmt->bpp, (size_t)w * src->fmt->bpp);
        dr.left = (LONG)x; dr.top = (LONG)y; dr.right = (LONG)(x + w); dr.bottom = (LONG)(y + h);
        if (dt) Upload(dst, &dr);
    }
    return D3D_OK;
}

/* ---- vertex and index buffers ---- */
static void BufferDestroy(NxBuffer *b)
{
    NxC_Release(b->h.dev, b->buf);
    Nx_PrivateClear(&b->h);
    if (b->rt_shadow) _aligned_free(b->shadow); else free(b->shadow);
    free(b);
}

static HRESULT BufferLockImpl(NxBuffer *b, UINT offset, UINT size, BYTE **data, DWORD flags)
{
    NxDevice *dev = b->h.dev;
    if (!data || b->mapped) return D3DERR_INVALIDCALL;
    if (offset > b->size) return D3DERR_INVALIDCALL;
    ++nx_stats.buf_maps;
    if (!size || offset + size > b->size) size = b->size - offset;
    b->lock_offset = offset;
    b->lock_size = size;
    b->lock_flags = flags;
    if (b->dynamic && dev->q && !b->lazy) {
        /* render thread: the game writes into a copy here; the draws that use the data send their range to the card
           (Nx_BufferNeed). The game locks whole buffers and writes a little: copying whole locks moved 16 MB a frame. */
        if (!b->shadow) {
            /* aligned as Map's memory is: the game writes skinned vertices with aligned SSE stores (movaps, 0x56FB4D) */
            b->shadow = (BYTE *)_aligned_malloc(b->size, 64);
            if (!b->shadow) return D3DERR_OUTOFVIDEOMEMORY;
            b->rt_shadow = TRUE;
        }
        b->lazy = TRUE;
    }
    if (b->dynamic && !b->shadow) {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP mode = (flags & D3DLOCK_NOOVERWRITE) && b->discarded ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
        if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD)) && b->discarded && size != b->size) mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (FAILED(ID3D11DeviceContext_Map(dev->c, (ID3D11Resource *)b->buf, 0, mode, 0, &m))) return D3DERR_INVALIDCALL;
        b->discarded = TRUE;
        b->mapped = TRUE;
        *data = (BYTE *)m.pData + offset;
        return D3D_OK;
    }
    b->mapped = TRUE;
    *data = b->shadow + offset;
    return D3D_OK;
}

static HRESULT BufferUnlockImpl(NxBuffer *b)
{
    NxDevice *dev = b->h.dev;
    if (!b->mapped) return D3DERR_INVALIDCALL;
    b->mapped = FALSE;
    if (b->dynamic && !b->shadow) {
        ID3D11DeviceContext_Unmap(dev->c, (ID3D11Resource *)b->buf, 0);
        return D3D_OK;
    }
    if (b->lock_flags & D3DLOCK_READONLY) return D3D_OK;
    if (b->lazy) {
        /* nothing is sent now: the draws send what they use (Nx_BufferNeed). A discard (or the first lock) starts a new
           buffer on the card; a lock over a range the card has makes it sent again when used. */
        UINT lo = b->lock_offset, hi = b->lock_offset + b->lock_size;
        if ((b->lock_flags & D3DLOCK_DISCARD) || !b->discarded) { b->pending_discard = TRUE; b->up_lo = b->up_hi = 0; }
        else if (lo < b->up_hi && hi > b->up_lo) b->up_lo = b->up_hi = 0;
        b->discarded = TRUE;
        return D3D_OK;
    }
    if (b->dynamic) {
        /* dynamic index buffer: copy the written range into the mapped buffer */
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP mode = (b->lock_flags & D3DLOCK_DISCARD) || !b->discarded ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
        if (SUCCEEDED(ID3D11DeviceContext_Map(dev->c, (ID3D11Resource *)b->buf, 0, mode, 0, &m))) {
            if (mode == D3D11_MAP_WRITE_DISCARD) memcpy(m.pData, b->shadow, b->size);
            else memcpy((BYTE *)m.pData + b->lock_offset, b->shadow + b->lock_offset, b->lock_size);
            ID3D11DeviceContext_Unmap(dev->c, (ID3D11Resource *)b->buf, 0);
            b->discarded = TRUE;
        }
        return D3D_OK;
    }
    {
        D3D11_BOX box;
        box.left = b->lock_offset; box.right = b->lock_offset + b->lock_size;
        box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        if (box.right > box.left)
            NxC_UpdateSubresource(dev, (ID3D11Resource *)b->buf, 0, &box, b->shadow + b->lock_offset, 0, 0, box.right - box.left);
    }
    return D3D_OK;
}

/* render thread: a draw uses bytes lo..hi of a dynamic buffer; the part the card does not have yet goes there now (from
   the copy the game wrote into). The first write after a discard renames the buffer on the card (Map DISCARD). */
void Nx_BufferNeed(NxDevice *dev, NxBuffer *b, UINT lo, UINT hi)
{
    if (!b || !b->lazy || !b->shadow || !b->buf) return;
    if (hi > b->size) hi = b->size;
    if (lo >= hi) return;
    if (b->pending_discard) {
        NxC_WriteBuffer(dev, b->buf, D3D11_MAP_WRITE_DISCARD, lo, b->shadow + lo, hi - lo);
        b->pending_discard = FALSE;
        b->up_lo = lo; b->up_hi = hi;
        return;
    }
    if (b->up_lo == b->up_hi || hi < b->up_lo || lo > b->up_hi) {
        NxC_WriteBuffer(dev, b->buf, D3D11_MAP_WRITE_NO_OVERWRITE, lo, b->shadow + lo, hi - lo);
        b->up_lo = lo; b->up_hi = hi;
        return;
    }
    if (lo < b->up_lo) { NxC_WriteBuffer(dev, b->buf, D3D11_MAP_WRITE_NO_OVERWRITE, lo, b->shadow + lo, b->up_lo - lo); b->up_lo = lo; }
    if (hi > b->up_hi) { NxC_WriteBuffer(dev, b->buf, D3D11_MAP_WRITE_NO_OVERWRITE, b->up_hi, b->shadow + b->up_hi, hi - b->up_hi); b->up_hi = hi; }
}

static HRESULT BufferLock(NxBuffer *b, UINT offset, UINT size, BYTE **data, DWORD flags)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = BufferLockImpl(b, offset, size, data, flags);
    nx_stats.t_lock += Nx_Now() - t0;
    return hr;
}
static HRESULT BufferUnlock(NxBuffer *b)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = BufferUnlockImpl(b);
    nx_stats.t_lock += Nx_Now() - t0;
    return hr;
}

void Nx_BufferBeforeDraw(NxBuffer *b) { (void)b; }

#define BUF_COMMON(prefix, IFACE, IID)                                                                                    \
    static HRESULT STDMETHODCALLTYPE prefix##_QueryInterface(IFACE *This, REFIID riid, void **out)                        \
    {                                                                                                                     \
        if (!out) return E_POINTER;                                                                                       \
        if (!memcmp(riid, &IID, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IDirect3DResource8, sizeof(GUID)) ||             \
            !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {                                                             \
            This->lpVtbl->AddRef(This);                                                                                   \
            *out = This;                                                                                                  \
            return S_OK;                                                                                                  \
        }                                                                                                                 \
        *out = NULL;                                                                                                      \
        return E_NOINTERFACE;                                                                                             \
    }                                                                                                                     \
    static ULONG STDMETHODCALLTYPE prefix##_AddRef(IFACE *This) { return (ULONG)InterlockedIncrement(&((NxBuffer *)This)->h.refs); } \
    static ULONG STDMETHODCALLTYPE prefix##_Release(IFACE *This)                                                         \
    {                                                                                                                     \
        LONG r = InterlockedDecrement(&((NxBuffer *)This)->h.refs);                                                       \
        if (r == 0) BufferDestroy((NxBuffer *)This);                                                                      \
        return (ULONG)r;                                                                                                  \
    }                                                                                                                     \
    static HRESULT STDMETHODCALLTYPE prefix##_GetDevice(IFACE *This, IDirect3DDevice8 **out)                              \
    {                                                                                                                     \
        if (!out) return D3DERR_INVALIDCALL;                                                                              \
        *out = (IDirect3DDevice8 *)((NxBuffer *)This)->h.dev;                                                             \
        (*out)->lpVtbl->AddRef(*out);                                                                                     \
        return D3D_OK;                                                                                                    \
    }                                                                                                                     \
    static HRESULT STDMETHODCALLTYPE prefix##_SetPrivateData(IFACE *This, REFGUID id, const void *data, DWORD size, DWORD flags) \
    { return Nx_PrivateSet(&((NxBuffer *)This)->h, id, data, size, flags); }                                              \
    static HRESULT STDMETHODCALLTYPE prefix##_GetPrivateData(IFACE *This, REFGUID id, void *data, DWORD *size)            \
    { return Nx_PrivateGet(&((NxBuffer *)This)->h, id, data, size); }                                                     \
    static HRESULT STDMETHODCALLTYPE prefix##_FreePrivateData(IFACE *This, REFGUID id)                                    \
    { return Nx_PrivateFree(&((NxBuffer *)This)->h, id); }                                                                \
    static DWORD STDMETHODCALLTYPE prefix##_SetPriority(IFACE *This, DWORD p)                                             \
    { DWORD o = ((NxBuffer *)This)->h.priority; ((NxBuffer *)This)->h.priority = p; return o; }                           \
    static DWORD STDMETHODCALLTYPE prefix##_GetPriority(IFACE *This) { return ((NxBuffer *)This)->h.priority; }           \
    static void STDMETHODCALLTYPE prefix##_PreLoad(IFACE *This) { (void)This; }                                           \
    static HRESULT STDMETHODCALLTYPE prefix##_Lock(IFACE *This, UINT offset, UINT size, BYTE **data, DWORD flags)         \
    { return BufferLock((NxBuffer *)This, offset, size, data, flags); }                                                   \
    static HRESULT STDMETHODCALLTYPE prefix##_Unlock(IFACE *This) { return BufferUnlock((NxBuffer *)This); }

BUF_COMMON(VB, IDirect3DVertexBuffer8, nxt_IID_IDirect3DVertexBuffer8)
BUF_COMMON(IB, IDirect3DIndexBuffer8, nxt_IID_IDirect3DIndexBuffer8)

static D3DRESOURCETYPE STDMETHODCALLTYPE VB_GetType(IDirect3DVertexBuffer8 *This) { (void)This; return D3DRTYPE_VERTEXBUFFER; }
static D3DRESOURCETYPE STDMETHODCALLTYPE IB_GetType(IDirect3DIndexBuffer8 *This) { (void)This; return D3DRTYPE_INDEXBUFFER; }
static HRESULT STDMETHODCALLTYPE VB_GetDesc(IDirect3DVertexBuffer8 *This, D3DVERTEXBUFFER_DESC *d)
{
    NxBuffer *b = (NxBuffer *)This;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size; d->FVF = b->fvf;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE IB_GetDesc(IDirect3DIndexBuffer8 *This, D3DINDEXBUFFER_DESC *d)
{
    NxBuffer *b = (NxBuffer *)This;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = b->format; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size;
    return D3D_OK;
}

static IDirect3DVertexBuffer8Vtbl kVBVtbl = {
    .QueryInterface = VB_QueryInterface, .AddRef = VB_AddRef, .Release = VB_Release, .GetDevice = VB_GetDevice,
    .SetPrivateData = VB_SetPrivateData, .GetPrivateData = VB_GetPrivateData, .FreePrivateData = VB_FreePrivateData,
    .SetPriority = VB_SetPriority, .GetPriority = VB_GetPriority, .PreLoad = VB_PreLoad, .GetType = VB_GetType,
    .Lock = VB_Lock, .Unlock = VB_Unlock, .GetDesc = VB_GetDesc,
};
static IDirect3DIndexBuffer8Vtbl kIBVtbl = {
    .QueryInterface = IB_QueryInterface, .AddRef = IB_AddRef, .Release = IB_Release, .GetDevice = IB_GetDevice,
    .SetPrivateData = IB_SetPrivateData, .GetPrivateData = IB_GetPrivateData, .FreePrivateData = IB_FreePrivateData,
    .SetPriority = IB_SetPriority, .GetPriority = IB_GetPriority, .PreLoad = IB_PreLoad, .GetType = IB_GetType,
    .Lock = IB_Lock, .Unlock = IB_Unlock, .GetDesc = IB_GetDesc,
};
void *nx_vb_vtbl = &kVBVtbl, *nx_ib_vtbl = &kIBVtbl;

HRESULT Nx_NewBuffer(NxDevice *dev, BOOL index, UINT size, DWORD usage, DWORD fvf, D3DFORMAT f, D3DPOOL pool, NxBuffer **out)
{
    NxBuffer *b;
    D3D11_BUFFER_DESC d;
    HRESULT hr;
    if (!out || !size) return D3DERR_INVALIDCALL;
    if (index && f != D3DFMT_INDEX16 && f != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    b = (NxBuffer *)calloc(1, sizeof *b);
    if (!b) return E_OUTOFMEMORY;
    b->h.vtbl = index ? (void *)&kIBVtbl : (void *)&kVBVtbl;
    b->h.refs = 1;
    b->h.kind = index ? NXK_IB : NXK_VB;
    b->h.dev = dev;
    b->index = index;
    b->size = size;
    b->usage = usage;
    b->fvf = fvf;
    b->format = f;
    b->pool = pool;
    b->dynamic = pool == D3DPOOL_DEFAULT && (usage & D3DUSAGE_WRITEONLY);
    /* write-only default-pool buffers (vertex and index) are mapped directly: the game locks whole ring buffers with
       NOOVERWRITE dozens of times a frame and writes a little; a system copy would have to be uploaded whole each time */
    if (!b->dynamic) {
        b->shadow = (BYTE *)calloc(1, size);
        if (!b->shadow) { free(b); return E_OUTOFMEMORY; }
    }
    memset(&d, 0, sizeof d);
    d.ByteWidth = (size + 3) & ~3u;
    d.BindFlags = index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    d.Usage = b->dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = b->dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    hr = ID3D11Device_CreateBuffer(dev->d, &d, NULL, &b->buf);
    if (FAILED(hr)) {
        Nx_Log("buffer %u bytes usage %lx: %08lx", size, (unsigned long)usage, (unsigned long)hr);
        BufferDestroy(b);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = b;
    return D3D_OK;
}

/* a multisampled target resolved into its single-sample copy (made on first use) */
ID3D11Texture2D *Nx_Resolve(NxSurface *s)
{
    NxDevice *dev = s->h.dev;
    D3D11_TEXTURE2D_DESC d;
    if (s->samples <= 1) return s->tex;
    if (!s->resolved) {
        ID3D11Texture2D_GetDesc(s->tex, &d);
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &s->resolved))) return NULL;
        ID3D11Device_CreateShaderResourceView(dev->d, (ID3D11Resource *)s->resolved, NULL, &s->resolved_srv);
    }
    ID3D11Texture2D_GetDesc(s->tex, &d);
    NxC_ResolveSubresource(dev, (ID3D11Resource *)s->resolved, 0, (ID3D11Resource *)s->tex, 0, d.Format);
    return s->resolved;
}
