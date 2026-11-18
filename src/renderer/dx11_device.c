/* The Neonix renderer, Direct3D 11 mode: IDirect3DDevice8.
   Set* calls only store D3D8 state and mark what changed; a draw turns the changed parts into D3D11 constant buffers, state
   objects (cached by their D3D8 values) and bindings. */
#include "dx11_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UP_VB_SIZE (4u << 20)
#define UP_IB_SIZE (1u << 20)

void Nx_Log(const char *format, ...)
{
    char text[900];
    va_list a;
    va_start(a, format);
    vsnprintf(text, sizeof text, format, a);
    va_end(a);
    Renderer_Log(text);
}

void Nx_AddRef(void *o) { if (o) ((IUnknown *)o)->lpVtbl->AddRef((IUnknown *)o); }
void Nx_Release(void *o) { if (o) ((IUnknown *)o)->lpVtbl->Release((IUnknown *)o); }

static NxDevice *Dev(IDirect3DDevice8 *This) { return (NxDevice *)This; }
static BOOL g_view_override;       /* dev: nx_view.txt (LoadViewOverride) */
static D3DMATRIX g_view_m;
float nx_dev_eye[3];
const D3DMATRIX *Nx_DevView(void) { return g_view_override ? &g_view_m : NULL; }

/* ---- matrices (D3D row vectors: v * M) ---- */
static void Mul(const D3DMATRIX *a, const D3DMATRIX *b, float *out)
{
    int i, j, k;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            float v = 0;
            for (k = 0; k < 4; ++k) v += a->m[i][k] * b->m[k][j];
            out[i * 4 + j] = v;
        }
}

static void NormalMatrix(const float *m, float *out)
{
    /* inverse transpose of the upper 3x3 */
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C, r;
    memset(out, 0, 16 * sizeof(float));
    if (fabs(det) < 1e-30) { out[0] = out[5] = out[10] = out[15] = 1; return; }
    r = 1.0f / det;
    /* inverse = adj^T / det; its transpose = cofactor / det */
    out[0] = A * r; out[1] = B * r; out[2] = C * r;
    out[4] = -(b * i - c * h) * r; out[5] = (a * i - c * g) * r; out[6] = -(a * h - b * g) * r;
    out[8] = (b * f - c * e) * r; out[9] = -(a * f - c * d) * r; out[10] = (a * e - b * d) * r;
    out[15] = 1;
}

static void Identity(D3DMATRIX *m)
{
    memset(m, 0, sizeof *m);
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1;
}

static float F(DWORD v) { float f; memcpy(&f, &v, 4); return f; }
static void Color(DWORD c, float *out)
{
    out[0] = ((c >> 16) & 255) / 255.0f; out[1] = ((c >> 8) & 255) / 255.0f; out[2] = (c & 255) / 255.0f; out[3] = (c >> 24) / 255.0f;
}

/* ---- default state ---- */
static void DefaultState(NxDevice *dev)
{
    NxState *s = &dev->s;
    DWORD i;
    float one = 1.0f, zero = 0.0f, big = 64.0f;
    memset(s, 0, sizeof *s);
    s->rs[D3DRS_ZENABLE] = dev->pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    s->rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    s->rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    s->rs[D3DRS_ZWRITEENABLE] = TRUE;
    s->rs[D3DRS_LASTPIXEL] = TRUE;
    s->rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    s->rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    s->rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    s->rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    s->rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    memcpy(&s->rs[D3DRS_FOGEND], &one, 4);
    memcpy(&s->rs[D3DRS_FOGDENSITY], &one, 4);
    memcpy(&s->rs[D3DRS_FOGSTART], &zero, 4);
    s->rs[D3DRS_STENCILFAIL] = s->rs[D3DRS_STENCILZFAIL] = s->rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    s->rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    s->rs[D3DRS_STENCILMASK] = s->rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    s->rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    s->rs[D3DRS_CLIPPING] = TRUE;
    s->rs[D3DRS_LIGHTING] = TRUE;
    s->rs[D3DRS_COLORVERTEX] = TRUE;
    s->rs[D3DRS_LOCALVIEWER] = TRUE;
    s->rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    s->rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    s->rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
    s->rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    memcpy(&s->rs[D3DRS_POINTSIZE], &one, 4);
    memcpy(&s->rs[D3DRS_POINTSIZE_MIN], &one, 4);
    memcpy(&s->rs[D3DRS_POINTSCALE_A], &one, 4);
    memcpy(&s->rs[D3DRS_POINTSIZE_MAX], &big, 4);
    s->rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    s->rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    s->rs[D3DRS_COLORWRITEENABLE] = 0xF;
    s->rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    for (i = 0; i < 8; ++i) {
        s->tss[i][D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        s->tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        s->tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        s->tss[i][D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        s->tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        s->tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        s->tss[i][D3DTSS_TEXCOORDINDEX] = i;
        s->tss[i][D3DTSS_ADDRESSU] = s->tss[i][D3DTSS_ADDRESSV] = s->tss[i][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
        s->tss[i][D3DTSS_MAGFILTER] = s->tss[i][D3DTSS_MINFILTER] = D3DTEXF_POINT;
        s->tss[i][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
        s->tss[i][D3DTSS_MAXANISOTROPY] = 1;
        s->tss[i][D3DTSS_COLORARG0] = s->tss[i][D3DTSS_ALPHAARG0] = s->tss[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
        Identity(&s->tex[i]);
    }
    Identity(&s->world);
    Identity(&s->view);
    Identity(&s->proj);
    s->clip_status.ClipUnion = 0;
    s->clip_status.ClipIntersection = 0xFFFFFFFF;
    dev->dirty_obj = dev->dirty_vs = dev->dirty_fix = dev->dirty_vsc = dev->dirty_ps = dev->dirty_states = dev->dirty_textures = TRUE;
}

static void FullViewport(NxDevice *dev)
{
    NxSurface *rt = dev->rt ? dev->rt : dev->backbuffer;
    dev->s.viewport.X = 0;
    dev->s.viewport.Y = 0;
    dev->s.viewport.Width = rt ? rt->width : 1;
    dev->s.viewport.Height = rt ? rt->height : 1;
    dev->s.viewport.MinZ = 0;
    dev->s.viewport.MaxZ = 1;
    dev->dirty_vs = dev->dirty_fix = dev->dirty_states = TRUE;
}

/* ---- swap chain and back buffer ---- */
static void ClientSize(HWND w, UINT *cx, UINT *cy)
{
    RECT r;
    if (w && GetClientRect(w, &r) && r.right > r.left && r.bottom > r.top) { *cx = (UINT)(r.right - r.left); *cy = (UINT)(r.bottom - r.top); }
    else { *cx = 640; *cy = 480; }
}

static void ReleaseSwap(NxDevice *dev)
{
    if (dev->swap_rtv) { ID3D11RenderTargetView_Release(dev->swap_rtv); dev->swap_rtv = NULL; }
    if (dev->swap1) { IDXGISwapChain1_Release(dev->swap1); dev->swap1 = NULL; }
    if (dev->swap) { IDXGISwapChain_Release(dev->swap); dev->swap = NULL; }
}

static HRESULT SwapRTV(NxDevice *dev)
{
    ID3D11Texture2D *b = NULL;
    static const GUID iid_tex = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
    HRESULT hr = IDXGISwapChain_GetBuffer(dev->swap, 0, &iid_tex, (void **)&b);
    if (FAILED(hr)) return hr;
    hr = ID3D11Device_CreateRenderTargetView(dev->d, (ID3D11Resource *)b, NULL, &dev->swap_rtv);
    ID3D11Texture2D_Release(b);
    return hr;
}

static unsigned nx_dbg;   /* dev: 1 no window association, 2 no Present */
static HRESULT CreateSwap(NxDevice *dev)
{
    HRESULT hr = E_FAIL;
    UINT w, h;
    ClientSize(dev->window, &w, &h);
    dev->swap_w = w;
    dev->swap_h = h;
    char flag[MAX_PATH + 32];
    snprintf(flag, sizeof flag, "%s\\nx_dx11_blit.on", Renderer_GameFolder());
    if (dev->factory2 && GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES) {
        DXGI_SWAP_CHAIN_DESC1 d;
        static const DXGI_SWAP_EFFECT effects[2] = {(DXGI_SWAP_EFFECT)4 /* FLIP_DISCARD */, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
        int k;
        for (k = 0; k < 2 && FAILED(hr); ++k) {
            memset(&d, 0, sizeof d);
            d.Width = w; d.Height = h;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            /* three: with the desktop compositor holding one buffer until the screen's next refresh, a second free one
               keeps the game from waiting for it (frames bounced between 7 and 16.7 ms at 144 FPS on a 60 Hz screen) */
            d.BufferCount = 3;
            d.Scaling = DXGI_SCALING_STRETCH;
            d.SwapEffect = effects[k];
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            d.Flags = dev->tearing ? 2048 /* ALLOW_TEARING */ : 0;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(dev->factory2, (IUnknown *)dev->d, dev->window, &d, NULL, NULL, &dev->swap1);
        }
        if (SUCCEEDED(hr)) {
            static const GUID iid_swap = {0x310d36a0, 0xd2e7, 0x4c0a, {0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a}};
            dev->flip = TRUE;
            hr = IDXGISwapChain1_QueryInterface(dev->swap1, &iid_swap, (void **)&dev->swap);
        }
    }
    if (FAILED(hr) && dev->factory1) {
        DXGI_SWAP_CHAIN_DESC d;
        memset(&d, 0, sizeof d);
        d.BufferDesc.Width = w; d.BufferDesc.Height = h;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = dev->window;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        dev->flip = FALSE;
        dev->tearing = FALSE;
        hr = IDXGIFactory1_CreateSwapChain(dev->factory1, (IUnknown *)dev->d, &d, &dev->swap);
    }
    if (FAILED(hr)) { Nx_Log("swap chain for %ux%u: %08lx", w, h, (unsigned long)hr); return hr; }
    if (dev->factory1 && !(nx_dbg & 1)) IDXGIFactory1_MakeWindowAssociation(dev->factory1, dev->window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = SwapRTV(dev);
    Nx_Log("swap chain %ux%u, %s%s", w, h, dev->flip ? "flip model" : "blit model", dev->tearing ? ", tearing allowed" : "");
    return hr;
}

/* the window covers its monitor when the game asks for full screen: borderless, no display mode change */
static void FullScreenWindow(NxDevice *dev)
{
    MONITORINFO mi;
    HMONITOR m;
    if (dev->pp.Windowed || !dev->window) return;
    m = MonitorFromWindow(dev->window, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(m, &mi)) return;
    SetWindowLongA(dev->window, GWL_STYLE, (LONG)((DWORD)GetWindowLongA(dev->window, GWL_STYLE) & ~(DWORD)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | (LONG)WS_POPUP);
    SetWindowPos(dev->window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

static HRESULT MakeBackBuffer(NxDevice *dev)
{
    UINT w = dev->pp.BackBufferWidth, h = dev->pp.BackBufferHeight;
    D3DFORMAT f = dev->pp.BackBufferFormat;
    /* [Renderer] Antialiasing overrides the game's own choice */
    D3DMULTISAMPLE_TYPE ms = nx_antialiasing >= 2 ? (D3DMULTISAMPLE_TYPE)nx_antialiasing : dev->pp.MultiSampleType;
    HRESULT hr;
    if (!w || !h) ClientSize(dev->window, &w, &h);
    dev->pp.BackBufferWidth = w;
    dev->pp.BackBufferHeight = h;
    if (f == D3DFMT_UNKNOWN || !Nx_Format(f)) f = D3DFMT_X8R8G8B8;
    dev->pp.BackBufferFormat = f;
    if (dev->pp.BackBufferCount == 0) dev->pp.BackBufferCount = 1;
    hr = Nx_NewSurface(dev, w, h, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ms, &dev->backbuffer);
    if (FAILED(hr)) return hr;
    if (dev->backbuffer->samples > 1) Nx_Log("back buffer: %ux anti-aliasing", dev->backbuffer->samples);
    if (dev->pp.EnableAutoDepthStencil) {
        D3DFORMAT z = Nx_Format(dev->pp.AutoDepthStencilFormat) ? dev->pp.AutoDepthStencilFormat : D3DFMT_D24S8;
        hr = Nx_NewSurface(dev, w, h, z, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ms, &dev->depth);
        if (FAILED(hr)) return hr;
    }
    dev->rt = dev->backbuffer;
    Nx_AddRef(dev->rt);
    dev->zs = dev->depth;
    Nx_AddRef(dev->zs);
    return D3D_OK;
}

static void BindTargets(NxDevice *dev)
{
    ID3D11RenderTargetView *rtv = dev->rt ? dev->rt->rtv : NULL;
    ID3D11DepthStencilView *dsv = dev->zs ? dev->zs->dsv : NULL;
    /* a depth buffer smaller than the target cannot be bound in D3D11; D3D8 allowed it */
    if (dsv && dev->rt && (dev->zs->width < dev->rt->width || dev->zs->height < dev->rt->height)) dsv = NULL;
    /* and one with another sample count (a texture target while the back buffer is multisampled) */
    if (dsv && dev->rt && dev->zs->samples != (dev->rt->samples ? dev->rt->samples : 1)) dsv = NULL;
    NxC_OMSetRenderTargets(dev, 1, &rtv, dsv);
}

/* ---- state objects ---- */
static const D3D11_BLEND kBlend[14] = {D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR,
    D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA,
    D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA};

static D3D11_BLEND BlendForAlpha(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

/* Alpha to coverage: an alpha-tested draw (GREATER / GREATEREQUAL, no blending) into a multisampled target gets its
   coverage from alpha around the test's threshold instead of a hard discard (the pixel shaders, pmisc.x >= 16): the
   same shape as the alpha test, its edges smoothed over the samples, and no draw order needed. */
extern volatile int nx_dev_ab;   /* DEV A/B 32: alpha to coverage off */
static BOOL A2C(const NxDevice *dev)
{
    const DWORD *rs = dev->s.rs;
    DWORD f = rs[D3DRS_ALPHAFUNC];
    return nx_alpha_to_coverage && !(nx_dev_ab & 32) && dev->rt && dev->rt->samples > 1 && rs[D3DRS_ALPHATESTENABLE] && !rs[D3DRS_ALPHABLENDENABLE] &&
           (f == D3DCMP_GREATER || f == D3DCMP_GREATEREQUAL);
}

static ID3D11BlendState *BlendState(NxDevice *dev)
{
    DWORD *rs = dev->s.rs;
    DWORD src = rs[D3DRS_SRCBLEND] < 14 ? rs[D3DRS_SRCBLEND] : 2, dst = rs[D3DRS_DESTBLEND] < 14 ? rs[D3DRS_DESTBLEND] : 1;
    DWORD op = rs[D3DRS_BLENDOP] >= 1 && rs[D3DRS_BLENDOP] <= 5 ? rs[D3DRS_BLENDOP] : 1;
    BOOL alpha_target = dev->rt && (dev->rt->format == D3DFMT_A8R8G8B8 || dev->rt->format == D3DFMT_A1R5G5B5 || dev->rt->format == D3DFMT_A4R4G4B4);
    DWORD mask = rs[D3DRS_COLORWRITEENABLE] & (alpha_target ? 0xF : 0x7);
    DWORD key[3];
    unsigned i;
    D3D11_BLEND_DESC d;
    D3D11_BLEND s11, d11;
    if (src == D3DBLEND_BOTHSRCALPHA) { src = D3DBLEND_SRCALPHA; dst = D3DBLEND_INVSRCALPHA; }
    else if (src == D3DBLEND_BOTHINVSRCALPHA) { src = D3DBLEND_INVSRCALPHA; dst = D3DBLEND_SRCALPHA; }
    key[0] = (rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0) | (A2C(dev) ? 2 : 0);
    key[1] = src | (dst << 8) | (op << 16);
    key[2] = mask;
    for (i = 0; i < dev->nblend; ++i)
        if (!memcmp(dev->blend[i].key, key, sizeof key)) return dev->blend[i].o;
    if (dev->nblend >= sizeof dev->blend / sizeof dev->blend[0]) return dev->blend[0].o;
    s11 = kBlend[src]; d11 = kBlend[dst];
    memset(&d, 0, sizeof d);
    d.AlphaToCoverageEnable = (key[0] & 2) != 0;
    d.RenderTarget[0].BlendEnable = key[0] & 1;
    d.RenderTarget[0].SrcBlend = s11;
    d.RenderTarget[0].DestBlend = d11;
    d.RenderTarget[0].BlendOp = (D3D11_BLEND_OP)op;
    d.RenderTarget[0].SrcBlendAlpha = BlendForAlpha(s11);
    d.RenderTarget[0].DestBlendAlpha = BlendForAlpha(d11);
    d.RenderTarget[0].BlendOpAlpha = (D3D11_BLEND_OP)op;
    d.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    memcpy(dev->blend[dev->nblend].key, key, sizeof key);
    dev->blend[dev->nblend].o = NULL;
    ID3D11Device_CreateBlendState(dev->d, &d, &dev->blend[dev->nblend].o);
    return dev->blend[dev->nblend++].o;
}

static ID3D11DepthStencilState *DepthState(NxDevice *dev)
{
    DWORD *rs = dev->s.rs;
    DWORD key[3];
    unsigned i;
    D3D11_DEPTH_STENCIL_DESC d;
    DWORD zf = rs[D3DRS_ZFUNC] >= 1 && rs[D3DRS_ZFUNC] <= 8 ? rs[D3DRS_ZFUNC] : D3DCMP_LESSEQUAL;
    DWORD sf = rs[D3DRS_STENCILFUNC] >= 1 && rs[D3DRS_STENCILFUNC] <= 8 ? rs[D3DRS_STENCILFUNC] : D3DCMP_ALWAYS;
#define SOP(x) ((x) >= 1 && (x) <= 8 ? (x) : 1)
    key[0] = (rs[D3DRS_ZENABLE] ? 1u : 0u) | (rs[D3DRS_ZWRITEENABLE] ? 2u : 0u) | (zf << 2) | (rs[D3DRS_STENCILENABLE] ? 64u : 0u);
    key[1] = SOP(rs[D3DRS_STENCILFAIL]) | (SOP(rs[D3DRS_STENCILZFAIL]) << 4) | (SOP(rs[D3DRS_STENCILPASS]) << 8) | (sf << 12);
    key[2] = (rs[D3DRS_STENCILMASK] & 255) | ((rs[D3DRS_STENCILWRITEMASK] & 255) << 8);
    for (i = 0; i < dev->ndepth; ++i)
        if (!memcmp(dev->depth_states[i].key, key, sizeof key)) return dev->depth_states[i].o;
    if (dev->ndepth >= sizeof dev->depth_states / sizeof dev->depth_states[0]) return dev->depth_states[0].o;
    memset(&d, 0, sizeof d);
    d.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    d.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)zf;
    d.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    d.StencilReadMask = (UINT8)rs[D3DRS_STENCILMASK];
    d.StencilWriteMask = (UINT8)rs[D3DRS_STENCILWRITEMASK];
    d.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)SOP(rs[D3DRS_STENCILFAIL]);
    d.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)SOP(rs[D3DRS_STENCILZFAIL]);
    d.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)SOP(rs[D3DRS_STENCILPASS]);
    d.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)sf;
    d.BackFace = d.FrontFace;
#undef SOP
    memcpy(dev->depth_states[dev->ndepth].key, key, sizeof key);
    dev->depth_states[dev->ndepth].o = NULL;
    ID3D11Device_CreateDepthStencilState(dev->d, &d, &dev->depth_states[dev->ndepth].o);
    return dev->depth_states[dev->ndepth++].o;
}

static ID3D11RasterizerState *RasterState(NxDevice *dev)
{
    DWORD *rs = dev->s.rs;
    DWORD key[2];
    unsigned i;
    D3D11_RASTERIZER_DESC d;
    key[0] = (rs[D3DRS_CULLMODE] & 3) | ((rs[D3DRS_FILLMODE] & 3) << 2);
    key[1] = rs[D3DRS_ZBIAS] & 31;
    for (i = 0; i < dev->nraster; ++i)
        if (!memcmp(dev->raster[i].key, key, sizeof key)) return dev->raster[i].o;
    if (dev->nraster >= sizeof dev->raster / sizeof dev->raster[0]) return dev->raster[0].o;
    memset(&d, 0, sizeof d);
    d.FillMode = rs[D3DRS_FILLMODE] == D3DFILL_SOLID ? D3D11_FILL_SOLID : D3D11_FILL_WIREFRAME;
    d.CullMode = rs[D3DRS_CULLMODE] == D3DCULL_CW ? D3D11_CULL_FRONT : rs[D3DRS_CULLMODE] == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    d.FrontCounterClockwise = FALSE;
    d.DepthBias = -(INT)(rs[D3DRS_ZBIAS] & 31) * 16;
    d.DepthClipEnable = TRUE;
    memcpy(dev->raster[dev->nraster].key, key, sizeof key);
    dev->raster[dev->nraster].o = NULL;
    ID3D11Device_CreateRasterizerState(dev->d, &d, &dev->raster[dev->nraster].o);
    return dev->raster[dev->nraster++].o;
}

static D3D11_FILTER Filter(DWORD mn, DWORD mg, DWORD mp)
{
    UINT lin_min = mn >= D3DTEXF_LINEAR, lin_mag = mg >= D3DTEXF_LINEAR, lin_mip = mp >= D3DTEXF_LINEAR;
    if (mn == D3DTEXF_ANISOTROPIC || mg == D3DTEXF_ANISOTROPIC) return D3D11_FILTER_ANISOTROPIC;
    return (D3D11_FILTER)((lin_min << 4) | (lin_mag << 2) | lin_mip);
}

static ID3D11SamplerState *SamplerState(NxDevice *dev, DWORD stage)
{
    DWORD *t = dev->s.tss[stage];
    DWORD key[3];
    unsigned i;
    D3D11_SAMPLER_DESC d;
    DWORD mn = t[D3DTSS_MINFILTER] & 7, mg = t[D3DTSS_MAGFILTER] & 7, mp = t[D3DTSS_MIPFILTER] & 3;
    DWORD aniso = t[D3DTSS_MAXANISOTROPY] < 1 ? 1 : t[D3DTSS_MAXANISOTROPY] > 16 ? 16 : t[D3DTSS_MAXANISOTROPY];
#define ADDR(x) ((x) >= 1 && (x) <= 5 ? (x) : 1)
    key[0] = mn | (mg << 3) | (mp << 6) | (ADDR(t[D3DTSS_ADDRESSU]) << 8) | (ADDR(t[D3DTSS_ADDRESSV]) << 11) |
             (ADDR(t[D3DTSS_ADDRESSW]) << 14) | (aniso << 17) | ((t[D3DTSS_MAXMIPLEVEL] & 31) << 22);
    key[1] = t[D3DTSS_MIPMAPLODBIAS];
    key[2] = t[D3DTSS_BORDERCOLOR];
    for (i = 0; i < dev->nsamplers; ++i)
        if (!memcmp(dev->samplers[i].key, key, sizeof key)) return dev->samplers[i].o;
    if (dev->nsamplers >= sizeof dev->samplers / sizeof dev->samplers[0]) return dev->samplers[0].o;
    memset(&d, 0, sizeof d);
    d.Filter = Filter(mn, mg, mp);
    /* [Renderer] Anisotropy: sharper textures at a slant, for every linearly filtered, mipmapped texture */
    if (nx_anisotropy >= 2 && mn >= D3DTEXF_LINEAR && mp != D3DTEXF_NONE) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        aniso = nx_anisotropy;
    }
    d.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)ADDR(t[D3DTSS_ADDRESSU]);
    d.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)ADDR(t[D3DTSS_ADDRESSV]);
    d.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)ADDR(t[D3DTSS_ADDRESSW]);
#undef ADDR
    d.MipLODBias = F(t[D3DTSS_MIPMAPLODBIAS]);
    d.MaxAnisotropy = aniso;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Color(t[D3DTSS_BORDERCOLOR], d.BorderColor);
    {
        /* BorderColor is RGBA; Color() gave RGBA too */
    }
    d.MinLOD = (float)(t[D3DTSS_MAXMIPLEVEL] & 31);
    d.MaxLOD = mp == D3DTEXF_NONE ? d.MinLOD : D3D11_FLOAT32_MAX;
    memcpy(dev->samplers[dev->nsamplers].key, key, sizeof key);
    dev->samplers[dev->nsamplers].o = NULL;
    ID3D11Device_CreateSamplerState(dev->d, &d, &dev->samplers[dev->nsamplers].o);
    return dev->samplers[dev->nsamplers++].o;
}

/* ---- constant buffers ---- */
NxStats nx_stats;

/* the stats clock (draws, locks, terrain, creation): the CPU's time stamp counter. QueryPerformanceCounter around every
   draw and lock was some 5000 calls a frame in a 1300-draw view; the counter's rate is measured against it in Stats */
LONGLONG Nx_Now(void)
{
    return (LONGLONG)__builtin_ia32_rdtsc();
}

static void Upload(NxDevice *dev, ID3D11Buffer *b, const void *data, UINT size)
{
    ++nx_stats.cb_uploads;
    NxC_WriteBuffer(dev, b, D3D11_MAP_WRITE_DISCARD, 0, data, size);
}

static void Vec(const D3DCOLORVALUE *c, float *out) { out[0] = c->r; out[1] = c->g; out[2] = c->b; out[3] = c->a; }

/* b0: world*view, world*view*proj and the normal matrix; rebuilt when one of the three transforms changes */
static void BuildObj(NxDevice *dev)
{
    NxState *s = &dev->s;
    NxObjCB cb;
    D3DMATRIX wv;
    Mul(&s->world, &s->view, cb.wv);
    memcpy(&wv, cb.wv, sizeof wv);
    Mul(&wv, &s->proj, cb.wvp);
    NormalMatrix(cb.wv, cb.wvn);
    {
        NxFFShader *f = Nx_FFShader(dev, s->vs);
        memset(cb.fvf, 0, sizeof cb.fvf + sizeof cb.texsize);
        if (f) { memcpy(cb.fvf, f->info, sizeof cb.fvf); memcpy(cb.texsize, f->texsize, sizeof cb.texsize); }
    }
    Upload(dev, dev->cb_obj, &cb, sizeof cb);
}

static void BuildFix(NxDevice *dev)
{
    const D3DVIEWPORT8 *v = &dev->s.viewport;
    float fix[4];
    fix[0] = 1 / (v->Width ? (float)v->Width : 1);
    fix[1] = -1 / (v->Height ? (float)v->Height : 1);
    fix[2] = fix[3] = 0;
    Upload(dev, dev->cb_fix, fix, sizeof fix);
}

/* b4: everything else the fixed-function vertex shader reads */
static void BuildVS(NxDevice *dev)
{
    NxState *s = &dev->s;
    NxVSCB cb;
    unsigned i, n = 0;
    memset(&cb, 0, sizeof cb);
    for (i = 0; i < 8; ++i) memcpy(cb.texm[i], &s->tex[i], 64);
    Vec(&s->material.Diffuse, cb.mat_diffuse);
    Vec(&s->material.Ambient, cb.mat_ambient);
    Vec(&s->material.Specular, cb.mat_specular);
    Vec(&s->material.Emissive, cb.mat_emissive);
    Color(s->rs[D3DRS_AMBIENT], cb.ambient);
    cb.fog[0] = F(s->rs[D3DRS_FOGSTART]);
    cb.fog[1] = F(s->rs[D3DRS_FOGEND]);
    cb.fog[2] = F(s->rs[D3DRS_FOGDENSITY]);
    cb.fog[3] = (float)(s->rs[D3DRS_FOGVERTEXMODE] & 3);
    cb.misc[0] = s->material.Power;
    cb.misc[1] = s->rs[D3DRS_FOGENABLE] ? 1.0f : 0.0f;
    cb.misc[2] = s->rs[D3DRS_RANGEFOGENABLE] ? 1.0f : 0.0f;
    cb.misc[3] = (float)(s->rs[D3DRS_FOGTABLEMODE] & 3);
    cb.flags[0] = s->rs[D3DRS_LIGHTING] != 0;
    cb.flags[1] = s->rs[D3DRS_NORMALIZENORMALS] != 0;
    cb.flags[2] = s->rs[D3DRS_COLORVERTEX] != 0;
    cb.flags[3] = s->rs[D3DRS_SPECULARENABLE] != 0;
    cb.sources[0] = (int)(s->rs[D3DRS_DIFFUSEMATERIALSOURCE] & 3);
    cb.sources[1] = (int)(s->rs[D3DRS_SPECULARMATERIALSOURCE] & 3);
    cb.sources[2] = (int)(s->rs[D3DRS_AMBIENTMATERIALSOURCE] & 3);
    cb.sources[3] = (int)(s->rs[D3DRS_EMISSIVEMATERIALSOURCE] & 3);
    for (i = 0; i < 8; ++i) {
        const D3DLIGHT8 *l = &s->light[i];
        NxLightCB *o;
        float x, y, z, len;
        if (!s->light_on[i]) continue;
        o = &cb.light[n++];
        Vec(&l->Diffuse, o->diffuse);
        Vec(&l->Specular, o->specular);
        Vec(&l->Ambient, o->ambient);
        x = l->Position.x; y = l->Position.y; z = l->Position.z;
        o->position[0] = x * s->view.m[0][0] + y * s->view.m[1][0] + z * s->view.m[2][0] + s->view.m[3][0];
        o->position[1] = x * s->view.m[0][1] + y * s->view.m[1][1] + z * s->view.m[2][1] + s->view.m[3][1];
        o->position[2] = x * s->view.m[0][2] + y * s->view.m[1][2] + z * s->view.m[2][2] + s->view.m[3][2];
        o->position[3] = 1;
        x = l->Direction.x; y = l->Direction.y; z = l->Direction.z;
        o->direction[0] = -(x * s->view.m[0][0] + y * s->view.m[1][0] + z * s->view.m[2][0]);
        o->direction[1] = -(x * s->view.m[0][1] + y * s->view.m[1][1] + z * s->view.m[2][1]);
        o->direction[2] = -(x * s->view.m[0][2] + y * s->view.m[1][2] + z * s->view.m[2][2]);
        len = (float)sqrt(o->direction[0] * o->direction[0] + o->direction[1] * o->direction[1] + o->direction[2] * o->direction[2]);
        if (len > 0) { o->direction[0] /= len; o->direction[1] /= len; o->direction[2] /= len; }
        o->atten[0] = l->Range > 0 ? l->Range : 1e30f;
        o->atten[1] = l->Attenuation0; o->atten[2] = l->Attenuation1; o->atten[3] = l->Attenuation2;
        o->spot[0] = l->Falloff;
        o->spot[1] = (float)cos(l->Theta * 0.5);
        o->spot[2] = (float)cos(l->Phi * 0.5);
        o->spot[3] = (float)l->Type;
    }
    cb.nlights[0] = (int)n;
    for (i = 0; i < 8; ++i) {
        DWORD tci = s->tss[i][D3DTSS_TEXCOORDINDEX], tt = s->tss[i][D3DTSS_TEXTURETRANSFORMFLAGS];
        cb.tci[i][0] = (int)(tci & 7);
        cb.tci[i][1] = (int)((tci >> 16) & 3);
        cb.tci[i][2] = (int)(tt & 7);
        cb.tci[i][3] = (tt & D3DTTFF_PROJECTED) != 0;
    }
    {
        const D3DVIEWPORT8 *v = &s->viewport;
        float w = v->Width ? (float)v->Width : 1, h = v->Height ? (float)v->Height : 1;
        cb.rhw[0] = 2 / w;
        cb.rhw[1] = -2 / h;
        cb.rhw[2] = (0.5f - (float)v->X) * 2 / w - 1;
        cb.rhw[3] = 1 - (0.5f - (float)v->Y) * 2 / h;
        cb.fix[0] = 1 / w;
        cb.fix[1] = -1 / h;
    }
    Upload(dev, dev->cb_vs, &cb, sizeof cb);
}

static void BuildPS(NxDevice *dev)
{
    NxState *s = &dev->s;
    NxPSCB cb;
    unsigned i;
    memset(&cb, 0, sizeof cb);
    for (i = 0; i < 8; ++i) {
        DWORD *t = s->tss[i];
        DWORD tt = t[D3DTSS_TEXTURETRANSFORMFLAGS];
        cb.color[i][0] = (int)t[D3DTSS_COLOROP]; cb.color[i][1] = (int)t[D3DTSS_COLORARG1];
        cb.color[i][2] = (int)t[D3DTSS_COLORARG2]; cb.color[i][3] = (int)t[D3DTSS_COLORARG0];
        cb.alpha[i][0] = (int)t[D3DTSS_ALPHAOP]; cb.alpha[i][1] = (int)t[D3DTSS_ALPHAARG1];
        cb.alpha[i][2] = (int)t[D3DTSS_ALPHAARG2]; cb.alpha[i][3] = (int)t[D3DTSS_ALPHAARG0];
        cb.stage[i][0] = (int)(t[D3DTSS_RESULTARG] & 15);
        cb.stage[i][1] = dev->tex[i] ? (dev->tex[i]->cube ? 2 : 1) : 0;
        cb.stage[i][2] = (tt & D3DTTFF_PROJECTED) && dev->s.vs && !(dev->s.vs & 1) ? (int)(tt & 7) : 0;
    }
    Color(s->rs[D3DRS_TEXTUREFACTOR], cb.tfactor);
    Color(s->rs[D3DRS_FOGCOLOR], cb.fogcolor);
    cb.alpharef[0] = (float)(s->rs[D3DRS_ALPHAREF] & 255);
    cb.misc[0] = A2C(dev) ? 16 + (int)(s->rs[D3DRS_ALPHAFUNC] & 15) : s->rs[D3DRS_ALPHATESTENABLE] ? (int)(s->rs[D3DRS_ALPHAFUNC] & 15) : 8;
    cb.misc[1] = s->rs[D3DRS_FOGENABLE] != 0;
    cb.misc[2] = s->rs[D3DRS_SPECULARENABLE] != 0;
    cb.misc[3] = (int)(s->rs[D3DRS_FOGTABLEMODE] & 3);
    cb.fogtable[0] = F(s->rs[D3DRS_FOGSTART]);
    cb.fogtable[1] = F(s->rs[D3DRS_FOGEND]);
    cb.fogtable[2] = F(s->rs[D3DRS_FOGDENSITY]);
    Upload(dev, dev->cb_ps, &cb, sizeof cb);
}

static NxShaderVS *GameShader(NxDevice *dev, DWORD handle)
{
    DWORD i = handle >> 1;
    if (!(handle & 1) || i >= NX_MAX_VS || !dev->vsh[i].used) return NULL;
    return &dev->vsh[i];
}

static ID3D11PixelShader *PixelShader(NxDevice *dev, DWORD h);

/* everything a draw needs; FALSE: skip the draw */
static BOOL Prepare(NxDevice *dev)
{
    unsigned i;
    void *vs, *layout;
    ID3D11Buffer *cbs[5];
    if (dev->s.vs & 1) {
        NxShaderVS *g = GameShader(dev, dev->s.vs);
        if (!g || !g->vs) return FALSE;
        vs = g->vs;
        layout = g->layout;
        if (dev->dirty_vsc) { Upload(dev, dev->cb_vsc, dev->s.vsc, sizeof dev->s.vsc); dev->dirty_vsc = FALSE; }
        if (dev->dirty_fix) { BuildFix(dev); dev->dirty_fix = FALSE; }
    } else {
        NxFFShader *f = Nx_FFShader(dev, dev->s.vs);
        if (!f) return FALSE;
        vs = f->vs;
        layout = f->layout;
        if (dev->dirty_obj) { BuildObj(dev); dev->dirty_obj = FALSE; }
        if (dev->dirty_vs) { BuildVS(dev); dev->dirty_vs = FALSE; }
    }
    if (dev->dirty_ps) { BuildPS(dev); dev->dirty_ps = FALSE; }
    if (dev->s.ps && dev->dirty_psc) { Upload(dev, dev->cb_psc, dev->s.psc, sizeof dev->s.psc); dev->dirty_psc = FALSE; }
    {
        ID3D11PixelShader *ps = dev->s.ps ? PixelShader(dev, dev->s.ps) : dev->ps_ff;
        if (!ps) ps = dev->ps_ff;
        if (ps != dev->bound_ps) { NxC_PSSetShader(dev, ps, NULL, 0); dev->bound_ps = ps; }
    }
    if (vs != dev->bound_vs) { NxC_VSSetShader(dev, (ID3D11VertexShader *)vs, NULL, 0); dev->bound_vs = vs; }
    if (layout != dev->bound_layout) { NxC_IASetInputLayout(dev, (ID3D11InputLayout *)layout); dev->bound_layout = layout; }
    if (dev->dirty_states) {
        ID3D11BlendState *b = BlendState(dev);
        ID3D11DepthStencilState *z = DepthState(dev);
        ID3D11RasterizerState *r = RasterState(dev);
        D3D11_VIEWPORT vp;
        static const float factor[4] = {0, 0, 0, 0};
        NxC_OMSetBlendState(dev, b, factor, 0xFFFFFFFF);
        NxC_OMSetDepthStencilState(dev, z, dev->s.rs[D3DRS_STENCILREF] & 255);
        NxC_RSSetState(dev, r);
        vp.TopLeftX = (float)dev->s.viewport.X;
        vp.TopLeftY = (float)dev->s.viewport.Y;
        vp.Width = (float)dev->s.viewport.Width;
        vp.Height = (float)dev->s.viewport.Height;
        vp.MinDepth = dev->s.viewport.MinZ;
        vp.MaxDepth = dev->s.viewport.MaxZ;
        NxC_RSSetViewports(dev, 1, &vp);
        dev->dirty_states = FALSE;
    }
    if (dev->dirty_textures) { dev->tex_dirty = dev->smp_dirty = 0xFF; dev->dirty_textures = FALSE; }
    if (dev->tex_dirty | dev->smp_dirty) {
        /* only the stages that changed: a texture swap rebinds its view, a sampler state change looks up its sampler */
        UINT m = dev->tex_dirty | dev->smp_dirty;
        for (i = 0; i < 8; ++i) {
            if (!(m & (1u << i))) continue;
            if (dev->tex_dirty & (1u << i)) {
                NxTexture *t = dev->tex[i];
                ID3D11ShaderResourceView *s2 = t && !t->cube ? t->srv : NULL, *sc = t && t->cube ? t->srv : NULL;
                if (s2 != dev->bound_srv[i]) { NxC_PSSetShaderResources(dev, i, 1, &s2); dev->bound_srv[i] = s2; }
                if (sc != dev->bound_srv[8 + i]) { NxC_PSSetShaderResources(dev, 8 + i, 1, &sc); dev->bound_srv[8 + i] = sc; }
            }
            if (dev->smp_dirty & (1u << i)) {
                ID3D11SamplerState *smp = SamplerState(dev, i);
                if (smp != dev->bound_sampler[i]) { NxC_PSSetSamplers(dev, i, 1, &smp); dev->bound_sampler[i] = smp; }
            }
        }
        dev->tex_dirty = dev->smp_dirty = 0;
    }
    if (!dev->bound_blend) {
        /* constant buffers, pixel shader and targets once per frame (Present changes them for the blit) */
        cbs[0] = dev->cb_obj; cbs[1] = dev->cb_vsc; cbs[2] = dev->cb_fix; cbs[3] = NULL; cbs[4] = dev->cb_vs;
        NxC_VSSetConstantBuffers(dev, 0, 5, cbs);
        NxC_PSSetConstantBuffers(dev, 3, 1, &dev->cb_ps);
        NxC_PSSetConstantBuffers(dev, 5, 1, &dev->cb_psc);
        BindTargets(dev);
        dev->bound_blend = (void *)1;
    }
    return TRUE;
}

/* vertex buffers: only slots that changed */
static void SetStream(NxDevice *dev, UINT slot, ID3D11Buffer *b, UINT stride, UINT offset)
{
    if (dev->bound_vb[slot] == b && dev->bound_stride[slot] == stride && dev->bound_offset[slot] == offset) return;
    NxC_IASetVertexBuffers(dev, slot, 1, &b, &stride, &offset);
    dev->bound_vb[slot] = b;
    dev->bound_stride[slot] = stride;
    dev->bound_offset[slot] = offset;
}

static void BindStreams(NxDevice *dev)
{
    unsigned i;
    BOOL ff = !(dev->s.vs & 1);
    for (i = 0; i < 16; ++i) {
        if (ff && i == 15) { SetStream(dev, 15, dev->ff_defaults, 0, 0); continue; }   /* what the FVF lacks */
        if (!dev->stream[i] && !dev->bound_vb[i]) continue;
        SetStream(dev, i, dev->stream[i] ? dev->stream[i]->buf : NULL, dev->stride[i], 0);
    }
}

static void SetIndices11(NxDevice *dev, ID3D11Buffer *b, DXGI_FORMAT f, UINT offset)
{
    if (dev->bound_ib == b && dev->bound_ib_format == f && dev->bound_ib_offset == offset) return;
    NxC_IASetIndexBuffer(dev, b, f, offset);
    dev->bound_ib = b;
    dev->bound_ib_format = f;
    dev->bound_ib_offset = offset;
}

/* after Present's blit: nothing of the game's is bound */
static void ForgetBindings(NxDevice *dev)
{
    dev->bound_vs = dev->bound_layout = dev->bound_blend = dev->bound_ps = NULL;
    memset(dev->bound_srv, 0xFF, sizeof dev->bound_srv);
    memset(dev->bound_sampler, 0xFF, sizeof dev->bound_sampler);
    memset(dev->bound_vb, 0xFF, sizeof dev->bound_vb);
    dev->bound_ib = (ID3D11Buffer *)(~(uintptr_t)0);
    dev->bound_topology = -1;
    dev->dirty_states = dev->dirty_textures = TRUE;
}

static UINT VertexCount(D3DPRIMITIVETYPE t, UINT n)
{
    switch (t) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}

/* data into a ring buffer; returns the byte offset */
static BOOL Ring(NxDevice *dev, BOOL index, const void *data, UINT size, UINT align, UINT *offset)
{
    ID3D11Buffer *b = index ? dev->up_ib : dev->up_vb;
    UINT cap = index ? dev->up_ib_size : dev->up_vb_size, *at = index ? &dev->up_ib_at : &dev->up_vb_at, start;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (size > cap) return FALSE;
    start = (*at + align - 1) / align * align;
    if (start + size > cap || !*at) { start = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    NxC_WriteBuffer(dev, b, mode, start, data, size);
    *at = start + size;
    *offset = start;
    return TRUE;
}

static void Topology(NxDevice *dev, D3DPRIMITIVETYPE t)
{
    int topo = t == D3DPT_TRIANGLEFAN ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : (int)t;
    if (topo == dev->bound_topology) return;
    NxC_IASetPrimitiveTopology(dev, (D3D11_PRIMITIVE_TOPOLOGY)topo);
    dev->bound_topology = topo;
}

/* a fan as a list of 16-bit indices from first..first+n+1 (relative indices) */
static BOOL FanIndices(NxDevice *dev, UINT tris, UINT *offset)
{
    WORD tmp[3 * 2048];
    UINT i;
    if (tris > 2048) return FALSE;
    for (i = 0; i < tris; ++i) { tmp[3 * i] = 0; tmp[3 * i + 1] = (WORD)(i + 1); tmp[3 * i + 2] = (WORD)(i + 2); }
    return Ring(dev, TRUE, tmp, tris * 6, 2, offset);
}

/* ---- IUnknown ---- */
static HRESULT STDMETHODCALLTYPE D_QueryInterface(IDirect3DDevice8 *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(riid, &nxt_IID_IDirect3DDevice8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {
        This->lpVtbl->AddRef(This);
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE D_AddRef(IDirect3DDevice8 *This) { return (ULONG)InterlockedIncrement(&Dev(This)->h.refs); }

static void ReleaseState(NxDevice *dev)
{
    unsigned i;
    for (i = 0; i < 8; ++i) { Nx_Release(dev->tex[i]); dev->tex[i] = NULL; }
    for (i = 0; i < 16; ++i) { Nx_Release(dev->stream[i]); dev->stream[i] = NULL; }
    Nx_Release(dev->indices); dev->indices = NULL;
}

static void GpuRelease(void);

static void DeviceDestroy(NxDevice *dev)
{
    unsigned i;
    NxQ_Stop(dev);   /* the render thread first: it may still be using what follows */
    NxTerrain_DeviceGone(dev);
    GpuRelease();
    if (dev->c) ID3D11DeviceContext_ClearState(dev->c);
    ReleaseState(dev);
    Nx_Release(dev->rt); Nx_Release(dev->zs);
    Nx_Release(dev->backbuffer); Nx_Release(dev->depth);
    for (i = 0; i < NX_MAX_STATEBLOCKS; ++i) free(dev->blocks[i].state);
    for (i = 0; i < NX_MAX_PS; ++i) if (dev->psh[i]) ID3D11PixelShader_Release(dev->psh[i]);
    for (i = 0; i < NX_MAX_VS; ++i) {
        if (dev->vsh[i].vs) ID3D11VertexShader_Release(dev->vsh[i].vs);
        if (dev->vsh[i].layout) ID3D11InputLayout_Release(dev->vsh[i].layout);
    }
    for (i = 0; i < dev->nblend; ++i) if (dev->blend[i].o) ID3D11BlendState_Release(dev->blend[i].o);
    for (i = 0; i < dev->ndepth; ++i) if (dev->depth_states[i].o) ID3D11DepthStencilState_Release(dev->depth_states[i].o);
    for (i = 0; i < dev->nraster; ++i) if (dev->raster[i].o) ID3D11RasterizerState_Release(dev->raster[i].o);
    for (i = 0; i < dev->nsamplers; ++i) if (dev->samplers[i].o) ID3D11SamplerState_Release(dev->samplers[i].o);
    if (dev->up_vb) ID3D11Buffer_Release(dev->up_vb);
    if (dev->up_ib) ID3D11Buffer_Release(dev->up_ib);
    if (dev->gamma_srv) ID3D11ShaderResourceView_Release(dev->gamma_srv);
    if (dev->gamma_tex) ID3D11Texture2D_Release(dev->gamma_tex);
    if (dev->cb_blit) ID3D11Buffer_Release(dev->cb_blit);
    Nx_ShadersRelease(dev);
    ReleaseSwap(dev);
    if (dev->factory2) IDXGIFactory2_Release(dev->factory2);
    if (dev->factory1) IDXGIFactory1_Release(dev->factory1);
    if (dev->c) ID3D11DeviceContext_Release(dev->c);
    if (dev->d) ID3D11Device_Release(dev->d);
    Nx_Release(dev->d3d);
    Nx_PrivateClear(&dev->h);
    Nx_Log("device released after %u frames", dev->frames);
    free(dev);
}

static ULONG STDMETHODCALLTYPE D_Release(IDirect3DDevice8 *This)
{
    LONG r = InterlockedDecrement(&Dev(This)->h.refs);
    if (r == 0) DeviceDestroy(Dev(This));
    return (ULONG)r;
}

/* ---- device ---- */
static HRESULT STDMETHODCALLTYPE D_TestCooperativeLevel(IDirect3DDevice8 *This) { (void)This; return D3D_OK; }
static UINT STDMETHODCALLTYPE D_GetAvailableTextureMem(IDirect3DDevice8 *This) { (void)This; return 0x7FF00000u; }
static HRESULT STDMETHODCALLTYPE D_ResourceManagerDiscardBytes(IDirect3DDevice8 *This, DWORD n) { (void)This; (void)n; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE D_GetDirect3D(IDirect3DDevice8 *This, IDirect3D8 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    *out = Dev(This)->d3d;
    Nx_AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetDeviceCaps(IDirect3DDevice8 *This, D3DCAPS8 *caps)
{
    (void)This;
    if (!caps) return D3DERR_INVALIDCALL;
    *caps = nx_caps;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetDisplayMode(IDirect3DDevice8 *This, D3DDISPLAYMODE *m)
{
    NxDevice *dev = Dev(This);
    DEVMODEA dm;
    if (!m) return D3DERR_INVALIDCALL;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    m->Width = dev->pp.Windowed ? dm.dmPelsWidth : dev->pp.BackBufferWidth;
    m->Height = dev->pp.Windowed ? dm.dmPelsHeight : dev->pp.BackBufferHeight;
    m->RefreshRate = dm.dmDisplayFrequency;
    m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetCreationParameters(IDirect3DDevice8 *This, D3DDEVICE_CREATION_PARAMETERS *p)
{
    NxDevice *dev = Dev(This);
    if (!p) return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = dev->window;
    p->BehaviorFlags = dev->behavior;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetCursorProperties(IDirect3DDevice8 *This, UINT x, UINT y, IDirect3DSurface8 *s)
{ (void)This; (void)x; (void)y; (void)s; return D3D_OK; }
static void STDMETHODCALLTYPE D_SetCursorPosition(IDirect3DDevice8 *This, UINT x, UINT y, DWORD f) { (void)This; (void)x; (void)y; (void)f; }
static WINBOOL STDMETHODCALLTYPE D_ShowCursor(IDirect3DDevice8 *This, WINBOOL show) { (void)This; (void)show; return FALSE; }
static HRESULT STDMETHODCALLTYPE D_CreateAdditionalSwapChain(IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain8 **out)
{ (void)This; (void)pp; if (out) *out = NULL; return D3DERR_INVALIDCALL; }

static HRESULT STDMETHODCALLTYPE D_Reset(IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS *pp)
{
    NxDevice *dev = Dev(This);
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    NxQ_Stop(dev);   /* a reset runs on this thread alone; the render thread starts again at the end */
    Unified_PostFxDeviceLost();   /* as before a native Reset: PostFX lets go of its frame-sized textures */
    NxC_OMSetRenderTargets(dev, 0, NULL, NULL);
    Nx_Release(dev->rt); dev->rt = NULL;
    Nx_Release(dev->zs); dev->zs = NULL;
    Nx_Release(dev->backbuffer); dev->backbuffer = NULL;
    Nx_Release(dev->depth); dev->depth = NULL;
    ReleaseState(dev);
    dev->pp = *pp;
    FullScreenWindow(dev);
    hr = MakeBackBuffer(dev);
    if (FAILED(hr)) return hr;
    *pp = dev->pp;
    DefaultState(dev);
    FullViewport(dev);
    ForgetBindings(dev);
    if (NxQ_Wanted()) NxQ_Start(dev);
    Nx_Log("reset: %ux%u %s", dev->pp.BackBufferWidth, dev->pp.BackBufferHeight, dev->pp.Windowed ? "window" : "full screen");
    return D3D_OK;
}

static void Shot(NxDevice *dev);

/* ---- GPU time: timestamp queries from one Present to the next and around the terrain draws, read back a few frames
   later without waiting. "start to end" is the GPU time of a frame when the GPU is the limit; when the CPU is, it also
   holds the GPU's idle time. ---- */
#define GQ_SETS 5
#define GQ_MARKS 10      /* 0 frame start, 1..8 terrain begin/end pairs, 9 frame end */
typedef struct GpuSet { ID3D11Query *dj, *ts[GQ_MARKS], *st[2]; unsigned n; BOOL open, pending, st_terrain; } GpuSet;
static GpuSet g_gq[GQ_SETS];
static int g_gq_cur = -1;
static BOOL g_gq_failed;
static double g_gq_span, g_gq_terrain, g_gq_stat[2][3];   /* per frame and terrain: vertices, triangles, pixels */
static unsigned g_gq_frames, g_gq_stat_frames;

static void GpuRelease(void)
{
    int i, k;
    for (i = 0; i < GQ_SETS; ++i) {
        if (g_gq[i].dj) ID3D11Query_Release(g_gq[i].dj);
        for (k = 0; k < GQ_MARKS; ++k) if (g_gq[i].ts[k]) ID3D11Query_Release(g_gq[i].ts[k]);
        for (k = 0; k < 2; ++k) if (g_gq[i].st[k]) ID3D11Query_Release(g_gq[i].st[k]);
    }
    memset(g_gq, 0, sizeof g_gq);
    g_gq_cur = -1;
}

static void GpuTerrainMarkCall(NxDevice *dev, void *arg) { Nx_GpuTerrainMark(dev, *(BOOL *)arg); }
void Nx_GpuTerrainMark(NxDevice *dev, BOOL begin)
{
    GpuSet *s;
    if (dev->q && !NxQ_OnRenderThread()) { NxC_Call(dev, GpuTerrainMarkCall, &begin, sizeof begin); return; }   /* the timer lives there */
    s = g_gq_cur >= 0 ? &g_gq[g_gq_cur] : NULL;
    if (!s || !s->open) return;
    if (begin ? !(s->n & 1) || s->n > GQ_MARKS - 3 : (s->n & 1)) return;
    if (s->n == 1) NxC_Begin(dev, (ID3D11Asynchronous *)s->st[1]);       /* the first terrain draw */
    else if (s->n == 2) { NxC_End(dev, (ID3D11Asynchronous *)s->st[1]); s->st_terrain = TRUE; }
    NxC_End(dev, (ID3D11Asynchronous *)s->ts[s->n++]);
}

static void GpuFrame(NxDevice *dev)
{
    D3D11_QUERY_DESC qd;
    GpuSet *s;
    int i, k;
    if (g_gq_failed) return;
    if (!g_gq[0].dj) {
        HRESULT hr = S_OK;
        for (i = 0; i < GQ_SETS && SUCCEEDED(hr); ++i) {
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; qd.MiscFlags = 0;
            hr = ID3D11Device_CreateQuery(dev->d, &qd, &g_gq[i].dj);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (k = 0; k < GQ_MARKS && SUCCEEDED(hr); ++k) hr = ID3D11Device_CreateQuery(dev->d, &qd, &g_gq[i].ts[k]);
            qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
            for (k = 0; k < 2 && SUCCEEDED(hr); ++k) hr = ID3D11Device_CreateQuery(dev->d, &qd, &g_gq[i].st[k]);
        }
        if (FAILED(hr)) { GpuRelease(); g_gq_failed = TRUE; return; }
    }
    if (g_gq_cur >= 0 && g_gq[g_gq_cur].open) {
        s = &g_gq[g_gq_cur];
        if (!(s->n & 1)) --s->n;   /* a terrain begin without its end */
        if (s->n == 2) NxC_End(dev, (ID3D11Asynchronous *)s->st[1]);
        NxC_End(dev, (ID3D11Asynchronous *)s->st[0]);
        NxC_End(dev, (ID3D11Asynchronous *)s->ts[GQ_MARKS - 1]);
        NxC_End(dev, (ID3D11Asynchronous *)s->dj);
        s->open = FALSE; s->pending = TRUE;
    }
    for (i = 0; i < GQ_SETS; ++i) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t[GQ_MARKS];
        BOOL ok = TRUE;
        s = &g_gq[i];
        if (!s->pending) continue;
        if (ID3D11DeviceContext_GetData(dev->c, (ID3D11Asynchronous *)s->dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        s->pending = FALSE;
        if (dj.Disjoint || !dj.Frequency) continue;
        for (k = 0; k < GQ_MARKS && ok; ++k) {
            if (k >= (int)s->n && k != GQ_MARKS - 1) continue;
            ok = ID3D11DeviceContext_GetData(dev->c, (ID3D11Asynchronous *)s->ts[k], &t[k], sizeof t[k], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }
        if (!ok) continue;
        {
            D3D11_QUERY_DATA_PIPELINE_STATISTICS ps[2];
            if (ID3D11DeviceContext_GetData(dev->c, (ID3D11Asynchronous *)s->st[0], &ps[0], sizeof ps[0], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                int j, nst = s->st_terrain && ID3D11DeviceContext_GetData(dev->c, (ID3D11Asynchronous *)s->st[1], &ps[1], sizeof ps[1],
                                                                          D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK ? 2 : 1;
                if (nst == 1) memset(&ps[1], 0, sizeof ps[1]);
                for (j = 0; j < 2; ++j) {
                    g_gq_stat[j][0] += (double)ps[j].VSInvocations;
                    g_gq_stat[j][1] += (double)ps[j].CInvocations;
                    g_gq_stat[j][2] += (double)ps[j].PSInvocations;
                }
                ++g_gq_stat_frames;
            }
        }
        g_gq_span += (double)(t[GQ_MARKS - 1] - t[0]) * 1000.0 / (double)dj.Frequency;
        for (k = 1; k + 1 < (int)s->n; k += 2) g_gq_terrain += (double)(t[k + 1] - t[k]) * 1000.0 / (double)dj.Frequency;
        ++g_gq_frames;
    }
    k = (g_gq_cur + 1) % GQ_SETS;
    if (g_gq[k].pending) { g_gq_cur = -1; return; }   /* still waiting for the GPU: this frame is not timed */
    g_gq_cur = k;
    s = &g_gq[k];
    NxC_Begin(dev, (ID3D11Asynchronous *)s->dj);
    NxC_Begin(dev, (ID3D11Asynchronous *)s->st[0]);
    NxC_End(dev, (ID3D11Asynchronous *)s->ts[0]);
    s->n = 1; s->open = TRUE; s->st_terrain = FALSE;
}

/* slow frames (over 12 ms after the first 300, any over 50 ms): what the frame did, at most 20 lines in 10 s */
static void FrameReport(NxDevice *dev, LONGLONG before, LONGLONG after, UINT sync, UINT flags)
{
    static LARGE_INTEGER freq;
    static LONGLONG last, last_tsc;
    static double tsc_hz;           /* time stamp counter ticks a second, from the last frame against QueryPerformanceCounter */
    static DWORD window_start;
    static unsigned lines, logged_mode;
    double frame_ms, present_ms;
    LONGLONG tsc = Nx_Now();
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    frame_ms = last ? (double)(after - last) * 1000.0 / (double)freq.QuadPart : 0;
    present_ms = (double)(after - before) * 1000.0 / (double)freq.QuadPart;
    if (last && after > last && tsc > last_tsc) {
        double hz = (double)(tsc - last_tsc) * (double)freq.QuadPart / (double)(after - last);
        tsc_hz = tsc_hz > 0 ? tsc_hz * 0.99 + hz * 0.01 : hz;
    }
    last = after;
    last_tsc = tsc;
    if (tsc_hz <= 0) { memset(&nx_stats, 0, sizeof nx_stats); return; }
    if (!logged_mode || logged_mode != (sync | (flags << 4) | (dev->pp.Windowed ? 0x100u : 0) | 0x1000u)) {
        logged_mode = sync | (flags << 4) | (dev->pp.Windowed ? 0x100u : 0) | 0x1000u;
        Nx_Log("present: %s, interval %08lx asked, sync %u, %s", dev->pp.Windowed ? "window" : "full screen",
               (unsigned long)dev->pp.FullScreen_PresentationInterval, sync, flags ? "tearing" : "no tearing");
    }
    {
        /* every 5 s: where the CPU time of a frame went (the game's own work is the rest) */
        static LONGLONG span_start, sum_draw, sum_lock, sum_present, sum_terrain;
        static unsigned span_frames, span_draws, span_locks, span_uploads;
        if (!span_start) span_start = after;
        sum_draw += nx_stats.t_draw; sum_lock += nx_stats.t_lock; sum_present += after - before; sum_terrain += nx_stats.t_terrain;
        span_draws += nx_stats.draws; span_locks += nx_stats.buf_maps; span_uploads += nx_stats.tex_uploads; ++span_frames;
        if ((double)(after - span_start) >= 5.0 * (double)freq.QuadPart && span_frames) {
            double k = 1000.0 / (double)freq.QuadPart / span_frames, total = (double)(after - span_start) * k;
            double kt = 1000.0 / tsc_hz / span_frames;   /* draws, locks and terrain are counted in time stamp ticks */
            Nx_Log("CPU per frame: %.2f ms = draws %.2f (%u draws) + locks %.2f (%u buffer locks, %.1f texture uploads) + terrain %.2f + "
                   "Present %.2f + the game %.2f", total, sum_draw * kt, span_draws / span_frames, sum_lock * kt, span_locks / span_frames,
                   (double)span_uploads / span_frames, sum_terrain * kt, sum_present * k,
                   total - (sum_draw + sum_lock + sum_terrain) * kt - sum_present * k);
            if (g_gq_frames)
                Nx_Log("GPU per frame: start to end %.2f ms, terrain %.2f ms (%u frames timed)", g_gq_span / g_gq_frames,
                       g_gq_terrain / g_gq_frames, g_gq_frames);
            if (g_gq_stat_frames) {
                double f = 1.0 / g_gq_stat_frames, px = (double)dev->swap_w * (double)dev->swap_h;
                Nx_Log("GPU work per frame: %.0f k vertices, %.0f k triangles, %.0f k pixels shaded (%.1f x the screen); "
                       "terrain %.0f k vertices, %.0f k triangles, %.0f k pixels", g_gq_stat[0][0] * f / 1000, g_gq_stat[0][1] * f / 1000,
                       g_gq_stat[0][2] * f / 1000, px > 0 ? g_gq_stat[0][2] * f / px : 0, g_gq_stat[1][0] * f / 1000,
                       g_gq_stat[1][1] * f / 1000, g_gq_stat[1][2] * f / 1000);
            }
            g_gq_span = g_gq_terrain = 0; g_gq_frames = g_gq_stat_frames = 0;
            memset(g_gq_stat, 0, sizeof g_gq_stat);
            span_start = after; sum_draw = sum_lock = sum_present = sum_terrain = 0; span_frames = span_draws = span_locks = span_uploads = 0;
        }
    }
    if (GetTickCount() - window_start > 10000) { window_start = GetTickCount(); lines = 0; }
    if (((dev->frames > 300 && frame_ms > 12) || frame_ms > 50) && lines < 20) {
        ++lines;
        double k = 1000.0 / tsc_hz;
        Nx_Log("slow frame %u: %.1f ms, renderer %.1f ms (draws %.1f, locks %.1f, creating %.1f, terrain %.1f in %u calls, Present %.2f); "
               "%u draws, %u texture uploads %u KB, %u buffer locks, %u constant uploads, %u read-backs, %u compiles",
               dev->frames, frame_ms, (double)(nx_stats.t_draw + nx_stats.t_lock + nx_stats.t_create + nx_stats.t_terrain) * k + present_ms,
               nx_stats.t_draw * k, nx_stats.t_lock * k, nx_stats.t_create * k, nx_stats.t_terrain * k, nx_stats.terrain_calls, present_ms,
               nx_stats.draws, nx_stats.tex_uploads, nx_stats.tex_kb, nx_stats.buf_maps, nx_stats.cb_uploads, nx_stats.readbacks,
               nx_stats.compiles);
    }
    memset(&nx_stats, 0, sizeof nx_stats);
}

/* the frame onto the swap chain and Present: on the render thread when there is one (it owns the context then) */
typedef struct PresentArgs { UINT w, h, sync, flags; } PresentArgs;
static volatile LONG g_present_hr;
static void PresentBlock(NxDevice *dev, void *arg)
{
    const PresentArgs *a = (const PresentArgs *)arg;
    UINT w = a->w, h = a->h;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srv[2];
    ID3D11ShaderResourceView *none[16];
    float opt[4];
    if (w != dev->swap_w || h != dev->swap_h) {
        if (dev->swap_rtv) { ID3D11RenderTargetView_Release(dev->swap_rtv); dev->swap_rtv = NULL; }
        NxC_OMSetRenderTargets(dev, 0, NULL, NULL);
        if (SUCCEEDED(IDXGISwapChain_ResizeBuffers(dev->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, dev->tearing ? 2048 : 0))) {
            dev->swap_w = w;
            dev->swap_h = h;
        }
        SwapRTV(dev);
    }
    if (dev->swap_rtv && dev->backbuffer) {
        NxC_OMSetRenderTargets(dev, 1, &dev->swap_rtv, NULL);
        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)dev->swap_w; vp.Height = (float)dev->swap_h; vp.MinDepth = 0; vp.MaxDepth = 1;
        NxC_RSSetViewports(dev, 1, &vp);
        NxC_RSSetState(dev, NULL);
        NxC_OMSetBlendState(dev, NULL, NULL, 0xFFFFFFFF);
        NxC_OMSetDepthStencilState(dev, NULL, 0);
        NxC_IASetInputLayout(dev, NULL);
        NxC_IASetPrimitiveTopology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        NxC_VSSetShader(dev, dev->vs_blit, NULL, 0);
        NxC_PSSetShader(dev, dev->ps_blit, NULL, 0);
        if (dev->backbuffer->samples > 1) { Nx_Resolve(dev->backbuffer); srv[0] = dev->backbuffer->resolved_srv; }
        else srv[0] = dev->backbuffer->srv;
        srv[1] = dev->gamma_srv;
        NxC_PSSetShaderResources(dev, 0, 2, srv);
        NxC_PSSetSamplers(dev, 0, 1, &dev->blit_sampler);
        opt[0] = dev->gamma_set ? 1.0f : 0.0f; opt[1] = opt[2] = opt[3] = 0;
        Upload(dev, dev->cb_blit, opt, sizeof opt);
        NxC_PSSetConstantBuffers(dev, 0, 1, &dev->cb_blit);
        NxC_Draw(dev, 3, 0);
        memset(none, 0, sizeof none);
        NxC_PSSetShaderResources(dev, 0, 16, none);
    }
    GpuFrame(dev);
    InterlockedExchange(&g_present_hr, (LONG)((nx_dbg & 2) ? S_OK : IDXGISwapChain_Present(dev->swap, a->sync, a->flags)));
}

/* With alpha to coverage the game draws its foliage on the path it uses below the top detail preset
   (ForestRender_isAlphaBlend 0x6D366C = 0, set by Config_SetGameToCurrentPerformanceLevel from the preset): leaves
   alpha-tested with depth writes (smoothed by alpha to coverage) instead of a depth-sorted alpha list with a
   per-triangle sort for every tree. That sort was about 30 % of the game thread outdoors on Jungle. What is
   lost is the top preset's cross-fade between a tree's detail levels. Checked by the two reads of the switch in
   ForestRender_Draw; the game's value comes back when alpha to coverage stops (no anti-aliasing, setting off). */
static void FoliagePath(NxDevice *dev)
{
    static int checked, ok, saved = -1;
    static const BYTE cmp_site[] = {0x39, 0x15, 0x6C, 0x36, 0x6D, 0x00}, mov_site[] = {0x8B, 0x15, 0x6C, 0x36, 0x6D, 0x00};
    volatile int *blend = (volatile int *)(uintptr_t)0x6D366C;
    BOOL on;
    if (!checked) {
        checked = 1;
        ok = !memcmp((const void *)(uintptr_t)0x59A5D6, cmp_site, sizeof cmp_site) && !memcmp((const void *)(uintptr_t)0x59A979, mov_site, sizeof mov_site);
        Nx_Log(ok ? "foliage: alpha to coverage path available" : "foliage: switch reads not found, foliage path left alone");
    }
    if (!ok) return;
    on = nx_alpha_to_coverage && !(nx_dev_ab & 32) && dev->backbuffer && dev->backbuffer->samples > 1;
    if (on) {
        if (*blend) { saved = *blend; *blend = 0; }
    } else if (saved >= 0) {
        *blend = saved;
        saved = -1;
    }
}

static HRESULT STDMETHODCALLTYPE D_Present(IDirect3DDevice8 *This, const RECT *src, const RECT *dst, HWND over, const RGNDATA *dirty)
{
    NxDevice *dev = Dev(This);
    UINT sync, flags = 0;
    PresentArgs args;
    HRESULT hr;
    (void)src; (void)dst; (void)over; (void)dirty;
    /* what the engine module does before a native device's Present (graphics_states.inc G_Present): the high-resolution
       terrain light (TerrainLight_Frame) and the PostFX test path; its hook only goes on d3d8.dll's own devices */
    Unified_PostFxPresentTest(This);
    FoliagePath(dev);
    Nx_ScreenshotKey(dev);
    Nx_TerrainScanFrame(dev);   /* dev: nx_terrain_scan.txt */
    Shot(dev);
    if (nx_vsync >= 0) sync = (UINT)nx_vsync;  /* [Renderer] VSync override: 0 off, 1+ on */
    else sync = dev->pp.Windowed || dev->pp.FullScreen_PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1;
    if (!sync && dev->tearing && !dev->pp.Windowed) flags = 0x200; /* DXGI_PRESENT_ALLOW_TEARING */
    ClientSize(dev->window, &args.w, &args.h);
    args.sync = sync; args.flags = flags;
    {
        LARGE_INTEGER a, b2;
        QueryPerformanceCounter(&a);
        NxC_Call(dev, PresentBlock, &args, sizeof args);
        NxQ_FrameEnd(dev);   /* with a render thread: waits while 2 frames are queued */
        QueryPerformanceCounter(&b2);
        hr = (HRESULT)g_present_hr;
        FrameReport(dev, a.QuadPart, b2.QuadPart, sync, flags);
    }
    if (FAILED(hr) && (dev->frames < 3 || (dev->frames & 1023) == 0)) Nx_Log("Present: %08lx", (unsigned long)hr);
    ++dev->frames;
    if (dev->frames == 1) { Nx_Log("first frame presented (%08lx)", (unsigned long)hr); Nx_ShaderCacheReport(); }
    ++nx_present_count;
    NxPace_Frame();
    Renderer_FrameTick();
    if (g_view_override) { dev->s.view = g_view_m; dev->dirty_obj = dev->dirty_vs = TRUE; }
    /* everything is bound again by the next draw */
    ForgetBindings(dev);
    BindTargets(dev);
    if (nx_scan_active && dev->rt && dev->rt->rtv) {
        /* dev: the hole scan's frames start magenta even where the game draws its sky instead of clearing */
        static const float magenta[4] = {1, 0, 1, 1};
        NxC_ClearRenderTargetView(dev, dev->rt->rtv, magenta);
    }
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE D_GetBackBuffer(IDirect3DDevice8 *This, UINT n, D3DBACKBUFFER_TYPE t, IDirect3DSurface8 **out)
{
    NxDevice *dev = Dev(This);
    (void)t;
    if (!out || n) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface8 *)dev->backbuffer;
    Nx_AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetRasterStatus(IDirect3DDevice8 *This, D3DRASTER_STATUS *r)
{
    (void)This;
    if (!r) return D3DERR_INVALIDCALL;
    r->InVBlank = TRUE;
    r->ScanLine = 0;
    return D3D_OK;
}
static void STDMETHODCALLTYPE D_SetGammaRamp(IDirect3DDevice8 *This, DWORD flags, const D3DGAMMARAMP *ramp)
{
    NxDevice *dev = Dev(This);
    WORD data[256 * 4];
    unsigned i;
    BOOL identity = TRUE;
    (void)flags;
    if (!ramp) return;
    for (i = 0; i < 256; ++i) {
        data[4 * i] = ramp->red[i]; data[4 * i + 1] = ramp->green[i]; data[4 * i + 2] = ramp->blue[i]; data[4 * i + 3] = 0xFFFF;
        if (abs((int)ramp->red[i] - (int)(i * 257)) > 64 || abs((int)ramp->green[i] - (int)(i * 257)) > 64 ||
            abs((int)ramp->blue[i] - (int)(i * 257)) > 64) identity = FALSE;
    }
    if (!dev->gamma_tex) {
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &dev->gamma_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev->d, (ID3D11Resource *)dev->gamma_tex, NULL, &dev->gamma_srv))) return;
    }
    NxC_UpdateSubresource(dev, (ID3D11Resource *)dev->gamma_tex, 0, NULL, data, sizeof data, 0, sizeof data);
    dev->gamma_set = !identity;
}
static void STDMETHODCALLTYPE D_GetGammaRamp(IDirect3DDevice8 *This, D3DGAMMARAMP *ramp)
{
    unsigned i;
    (void)This;
    if (!ramp) return;
    for (i = 0; i < 256; ++i) ramp->red[i] = ramp->green[i] = ramp->blue[i] = (WORD)(i * 257);
}

static HRESULT STDMETHODCALLTYPE D_CreateTexture(IDirect3DDevice8 *This, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                                 D3DPOOL pool, IDirect3DTexture8 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Nx_NewTexture(Dev(This), w, h, levels, usage, f, pool, FALSE, (NxTexture **)out);
    nx_stats.t_create += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_CreateVolumeTexture(IDirect3DDevice8 *This, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                                       D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture8 **out)
{
    (void)This; (void)w; (void)h; (void)d; (void)levels; (void)usage; (void)f; (void)pool;
    if (out) *out = NULL;
    return D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE D_CreateCubeTexture(IDirect3DDevice8 *This, UINT edge, UINT levels, DWORD usage, D3DFORMAT f,
                                                     D3DPOOL pool, IDirect3DCubeTexture8 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Nx_NewTexture(Dev(This), edge, edge, levels, usage, f, pool, TRUE, (NxTexture **)out);
    nx_stats.t_create += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_CreateVertexBuffer(IDirect3DDevice8 *This, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                      IDirect3DVertexBuffer8 **out)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Nx_NewBuffer(Dev(This), FALSE, size, usage, fvf, D3DFMT_VERTEXDATA, pool, (NxBuffer **)out);
    nx_stats.t_create += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_CreateIndexBuffer(IDirect3DDevice8 *This, UINT size, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                                     IDirect3DIndexBuffer8 **out)
{
    LONGLONG t0 = Nx_Now();
    HRESULT hr = Nx_NewBuffer(Dev(This), TRUE, size, usage, 0, f, pool, (NxBuffer **)out);
    nx_stats.t_create += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_CreateRenderTarget(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                      WINBOOL lockable, IDirect3DSurface8 **out)
{
    (void)lockable;
    return Nx_NewSurface(Dev(This), w, h, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ms, (NxSurface **)out);
}
static HRESULT STDMETHODCALLTYPE D_CreateDepthStencilSurface(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                             IDirect3DSurface8 **out)
{
    return Nx_NewSurface(Dev(This), w, h, f, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ms, (NxSurface **)out);
}
static HRESULT STDMETHODCALLTYPE D_CreateImageSurface(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface8 **out)
{
    return Nx_NewSurface(Dev(This), w, h, f, 0, D3DPOOL_SYSTEMMEM, D3DMULTISAMPLE_NONE, (NxSurface **)out);
}
static HRESULT STDMETHODCALLTYPE D_CopyRects(IDirect3DDevice8 *This, IDirect3DSurface8 *src, const RECT *rects, UINT n,
                                             IDirect3DSurface8 *dst, const POINT *points)
{
    if (!src || !dst) return D3DERR_INVALIDCALL;
    return Nx_CopyRects(Dev(This), (NxSurface *)src, rects, n, (NxSurface *)dst, points);
}
static HRESULT STDMETHODCALLTYPE D_UpdateTexture(IDirect3DDevice8 *This, IDirect3DBaseTexture8 *src, IDirect3DBaseTexture8 *dst)
{
    NxTexture *s = (NxTexture *)src, *d = (NxTexture *)dst;
    UINT i, n;
    if (!s || !d || s->cube != d->cube || s->format != d->format) return D3DERR_INVALIDCALL;
    n = (s->levels < d->levels ? s->levels : d->levels) * (s->cube ? 6 : 1);
    for (i = 0; i < n; ++i) {
        NxSurface *a = s->surf[i], *b = d->surf[i];
        if (a->mem && a->width == b->width && a->height == b->height) Nx_CopyRects(Dev(This), a, NULL, 0, b, NULL);
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetFrontBuffer(IDirect3DDevice8 *This, IDirect3DSurface8 *dst)
{
    NxDevice *dev = Dev(This);
    NxSurface *d = (NxSurface *)dst, *b = dev->backbuffer;
    D3DLOCKED_RECT lr;
    UINT y, w, h;
    if (!d || !b || FAILED(Nx_ReadSurface(b))) return D3DERR_INVALIDCALL;
    if (FAILED(dst->lpVtbl->LockRect(dst, &lr, NULL, 0))) return D3DERR_INVALIDCALL;
    w = d->width < b->width ? d->width : b->width;
    h = d->height < b->height ? d->height : b->height;
    for (y = 0; y < h; ++y) {
        if (b->fmt->bpp == 4) {
            BYTE *row = (BYTE *)lr.pBits + y * (UINT)lr.Pitch;
            memcpy(row, b->mem + y * b->pitch, (size_t)w * 4);
        }
    }
    dst->lpVtbl->UnlockRect(dst);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetRenderTarget(IDirect3DDevice8 *This, IDirect3DSurface8 *rt, IDirect3DSurface8 *zs)
{
    NxDevice *dev = Dev(This);
    NxSurface *r = (NxSurface *)rt, *z = (NxSurface *)zs;
    if (r && !r->rtv) {
        /* a render target texture level other than 0: make its view now */
        if (r->container && r->container->tex && (r->container->usage & D3DUSAGE_RENDERTARGET)) {
            D3D11_RENDER_TARGET_VIEW_DESC d;
            memset(&d, 0, sizeof d);
            d.Format = nx_bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            if (r->container->cube) {
                d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray.MipSlice = r->level; d.Texture2DArray.FirstArraySlice = r->face; d.Texture2DArray.ArraySize = 1;
            } else {
                d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                d.Texture2D.MipSlice = r->level;
            }
            ID3D11Device_CreateRenderTargetView(dev->d, (ID3D11Resource *)r->container->tex, &d, &r->rtv);
        }
        if (!r->rtv) return D3DERR_INVALIDCALL;
    }
    if (r) { Nx_AddRef(r); Nx_Release(dev->rt); dev->rt = r; }
    /* D3D8: a NULL depth stencil means none */
    Nx_AddRef(z);
    Nx_Release(dev->zs);
    dev->zs = z;
    BindTargets(dev);
    FullViewport(dev);
    dev->dirty_states = TRUE;
    dev->dirty_ps = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetRenderTarget(IDirect3DDevice8 *This, IDirect3DSurface8 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface8 *)Dev(This)->rt;
    Nx_AddRef(*out);
    return *out ? D3D_OK : D3DERR_NOTFOUND;
}
static HRESULT STDMETHODCALLTYPE D_GetDepthStencilSurface(IDirect3DDevice8 *This, IDirect3DSurface8 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface8 *)Dev(This)->zs;
    Nx_AddRef(*out);
    return *out ? D3D_OK : D3DERR_NOTFOUND;
}
static HRESULT STDMETHODCALLTYPE D_BeginScene(IDirect3DDevice8 *This) { Dev(This)->in_scene = TRUE; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE D_EndScene(IDirect3DDevice8 *This) { Dev(This)->in_scene = FALSE; return D3D_OK; }

static HRESULT STDMETHODCALLTYPE D_Clear(IDirect3DDevice8 *This, DWORD n, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                                         DWORD stencil)
{
    NxDevice *dev = Dev(This);
    const D3DVIEWPORT8 *v = &dev->s.viewport;
    UINT tw = dev->rt ? dev->rt->width : 0, th = dev->rt ? dev->rt->height : 0, i, count = n && rects ? n : 1;
    BOOL color_on = (flags & D3DCLEAR_TARGET) && dev->rt && dev->rt->rtv;
    BOOL depth_on = (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && dev->zs && dev->zs->dsv;
    BOOL quads = FALSE;
    float c[4];
    if (!tw || !th) return D3D_OK;
    c[0] = ((color >> 16) & 255) / 255.0f; c[1] = ((color >> 8) & 255) / 255.0f; c[2] = (color & 255) / 255.0f;
    c[3] = (color >> 24) / 255.0f;
    if (nx_scan_active) { c[0] = 1; c[1] = 0; c[2] = 1; c[3] = 1; }   /* dev: the terrain hole scan's magenta */
    for (i = 0; i < count; ++i) {
        /* D3D8 clears the viewport, or the given rectangles inside it */
        LONG l = (LONG)v->X, t = (LONG)v->Y, r = (LONG)(v->X + v->Width), b = (LONG)(v->Y + v->Height);
        if (n && rects) {
            if (rects[i].x1 > l) l = rects[i].x1;
            if (rects[i].y1 > t) t = rects[i].y1;
            if (rects[i].x2 < r) r = rects[i].x2;
            if (rects[i].y2 < b) b = rects[i].y2;
        }
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (LONG)tw) r = (LONG)tw;
        if (b > (LONG)th) b = (LONG)th;
        if (l >= r || t >= b) continue;
        if (l == 0 && t == 0 && r == (LONG)tw && b == (LONG)th) {
            if (color_on) NxC_ClearRenderTargetView(dev, dev->rt->rtv, c);
            if (depth_on)
                NxC_ClearDepthStencilView(dev, dev->zs->dsv,
                                                          ((flags & D3DCLEAR_ZBUFFER) ? D3D11_CLEAR_DEPTH : 0) |
                                                          ((flags & D3DCLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0), z, (UINT8)stencil);
            continue;
        }
        {
            /* part of the target: a quad that writes the colour, depth and stencil */
            float cb[12];
            D3D11_VIEWPORT vp;
            static const float zero[4] = {0, 0, 0, 0};
            unsigned zk = ((flags & D3DCLEAR_ZBUFFER) && depth_on ? 1u : 0u) | ((flags & D3DCLEAR_STENCIL) && depth_on ? 2u : 0u);
            if (!color_on && !zk) continue;
            if (!quads) {
                vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)tw; vp.Height = (float)th; vp.MinDepth = 0; vp.MaxDepth = 1;
                NxC_RSSetViewports(dev, 1, &vp);
                NxC_RSSetState(dev, NULL);
                NxC_IASetInputLayout(dev, NULL);
                NxC_IASetPrimitiveTopology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                NxC_VSSetShader(dev, dev->vs_clear, NULL, 0);
                NxC_PSSetShader(dev, dev->ps_clear, NULL, 0);
                NxC_VSSetConstantBuffers(dev, 1, 1, &dev->cb_clear);
                NxC_PSSetConstantBuffers(dev, 1, 1, &dev->cb_clear);
                quads = TRUE;
            }
            cb[0] = (float)l / tw * 2 - 1; cb[1] = 1 - (float)t / th * 2; cb[2] = (float)r / tw * 2 - 1; cb[3] = 1 - (float)b / th * 2;
            memcpy(cb + 4, c, sizeof c);
            cb[8] = z; cb[9] = cb[10] = cb[11] = 0;
            Upload(dev, dev->cb_clear, cb, sizeof cb);
            NxC_OMSetBlendState(dev, dev->clear_blend[color_on ? 1 : 0], zero, 0xFFFFFFFF);
            NxC_OMSetDepthStencilState(dev, dev->clear_depth[zk], (UINT)(stencil & 255));
            NxC_Draw(dev, 4, 0);
        }
    }
    if (quads) ForgetBindings(dev);   /* the next draw binds the game's state again */
    return D3D_OK;
}

/* dev: nx_view.txt in the game folder ("ex ey ez ax ay az": eye and target, z up) replaces the view transform, so hidden
   tests without a player camera can look at the ground */
static void SetDevView(const float e[3], const float a[3])
{
    float x[3], y[3], z[3], n;
    z[0] = a[0] - e[0]; z[1] = a[1] - e[1]; z[2] = a[2] - e[2];
    n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]); z[0] /= n; z[1] /= n; z[2] /= n;
    x[0] = z[1]; x[1] = -z[0]; x[2] = 0;   /* up (0, 0, 1) x z */
    n = sqrtf(x[0] * x[0] + x[1] * x[1]); x[0] /= n; x[1] /= n;
    y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
    memset(&g_view_m, 0, sizeof g_view_m);
    g_view_m._11 = x[0]; g_view_m._21 = x[1]; g_view_m._31 = x[2];
    g_view_m._12 = y[0]; g_view_m._22 = y[1]; g_view_m._32 = y[2];
    g_view_m._13 = z[0]; g_view_m._23 = z[1]; g_view_m._33 = z[2];
    /* as in real play: the view only turns; the camera's position reaches the terrain through its eye (ctx + 4) */
    g_view_m._44 = 1;
    memcpy(nx_dev_eye, e, sizeof nx_dev_eye);
    g_view_override = TRUE;
}

/* dev: the terrain hole scan's views (dx11_terrain_scan.inc) */
void Nx_DevSetView(const float e[3], const float a[3]) { SetDevView(e, a); }

static void LoadViewOverride(void)
{
    char path[MAX_PATH + 32];
    float e[3], a[3], x[3], y[3], z[3], n;
    FILE *f;
    snprintf(path, sizeof path, "%s\\nx_view.txt", Renderer_GameFolder());
    f = fopen(path, "r");
    if (!f) return;
    if (fscanf(f, "%f %f %f %f %f %f", &e[0], &e[1], &e[2], &a[0], &a[1], &a[2]) == 6) {
        z[0] = a[0] - e[0]; z[1] = a[1] - e[1]; z[2] = a[2] - e[2];
        n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]); z[0] /= n; z[1] /= n; z[2] /= n;
        x[0] = z[1]; x[1] = -z[0]; x[2] = 0;   /* up (0, 0, 1) x z */
        n = sqrtf(x[0] * x[0] + x[1] * x[1]); x[0] /= n; x[1] /= n;
        y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
        memset(&g_view_m, 0, sizeof g_view_m);
        g_view_m._11 = x[0]; g_view_m._21 = x[1]; g_view_m._31 = x[2];
        g_view_m._12 = y[0]; g_view_m._22 = y[1]; g_view_m._32 = y[2];
        g_view_m._13 = z[0]; g_view_m._23 = z[1]; g_view_m._33 = z[2];
        /* as in real play: the view only turns; the camera's position reaches the terrain through its eye (ctx + 4) */
        g_view_m._44 = 1;
        memcpy(nx_dev_eye, e, sizeof nx_dev_eye);
        g_view_override = TRUE;
        Nx_Log("dev: view from nx_view.txt");
    }
    fclose(f);
}

static HRESULT STDMETHODCALLTYPE D_SetTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    NxDevice *dev = Dev(This);
    if (!m) return D3DERR_INVALIDCALL;
    if (t == D3DTS_VIEW && g_view_override) m = &g_view_m;
    if (t == D3DTS_VIEW) { dev->s.view = *m; dev->dirty_obj = dev->dirty_vs = TRUE; }   /* lights are in view space */
    else if (t == D3DTS_PROJECTION) { dev->s.proj = *m; dev->dirty_obj = TRUE; }
    else if (t == D3DTS_WORLD) { dev->s.world = *m; dev->dirty_obj = TRUE; }
    else if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) { dev->s.tex[t - D3DTS_TEXTURE0] = *m; dev->dirty_vs = TRUE; }
    return D3D_OK;
}
static D3DMATRIX *Transform(NxDevice *dev, D3DTRANSFORMSTATETYPE t)
{
    if (t == D3DTS_VIEW) return &dev->s.view;
    if (t == D3DTS_PROJECTION) return &dev->s.proj;
    if (t == D3DTS_WORLD) return &dev->s.world;
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) return &dev->s.tex[t - D3DTS_TEXTURE0];
    return NULL;
}
static HRESULT STDMETHODCALLTYPE D_GetTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m)
{
    D3DMATRIX *s = Transform(Dev(This), t);
    if (!m) return D3DERR_INVALIDCALL;
    if (s) *m = *s; else Identity(m);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_MultiplyTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    D3DMATRIX *s = Transform(Dev(This), t), r;
    if (!m) return D3DERR_INVALIDCALL;
    if (!s) return D3D_OK;
    Mul(m, s, &r.m[0][0]);
    *s = r;
    Dev(This)->dirty_obj = Dev(This)->dirty_vs = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetViewport(IDirect3DDevice8 *This, const D3DVIEWPORT8 *v)
{
    NxDevice *dev = Dev(This);
    if (!v) return D3DERR_INVALIDCALL;
    dev->s.viewport = *v;
    dev->dirty_vs = dev->dirty_fix = dev->dirty_states = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetViewport(IDirect3DDevice8 *This, D3DVIEWPORT8 *v)
{
    if (!v) return D3DERR_INVALIDCALL;
    *v = Dev(This)->s.viewport;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetMaterial(IDirect3DDevice8 *This, const D3DMATERIAL8 *m)
{
    if (!m) return D3DERR_INVALIDCALL;
    Dev(This)->s.material = *m;
    Dev(This)->dirty_vs = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetMaterial(IDirect3DDevice8 *This, D3DMATERIAL8 *m)
{
    if (!m) return D3DERR_INVALIDCALL;
    *m = Dev(This)->s.material;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetLight(IDirect3DDevice8 *This, DWORD i, const D3DLIGHT8 *l)
{
    if (!l || i >= 8) return D3DERR_INVALIDCALL;
    Dev(This)->s.light[i] = *l;
    Dev(This)->dirty_vs = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetLight(IDirect3DDevice8 *This, DWORD i, D3DLIGHT8 *l)
{
    if (!l || i >= 8) return D3DERR_INVALIDCALL;
    *l = Dev(This)->s.light[i];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_LightEnable(IDirect3DDevice8 *This, DWORD i, WINBOOL on)
{
    NxDevice *dev = Dev(This);
    if (i >= 8) return D3DERR_INVALIDCALL;
    if (on && !dev->s.light[i].Type) {
        /* D3D8: enabling an unset light gives a white directional light down +z */
        memset(&dev->s.light[i], 0, sizeof dev->s.light[i]);
        dev->s.light[i].Type = D3DLIGHT_DIRECTIONAL;
        dev->s.light[i].Diffuse.r = dev->s.light[i].Diffuse.g = dev->s.light[i].Diffuse.b = 1;
        dev->s.light[i].Direction.z = 1;
    }
    dev->s.light_on[i] = on != 0;
    dev->dirty_vs = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetLightEnable(IDirect3DDevice8 *This, DWORD i, WINBOOL *on)
{
    if (!on || i >= 8) return D3DERR_INVALIDCALL;
    *on = Dev(This)->s.light_on[i];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetClipPlane(IDirect3DDevice8 *This, DWORD i, const float *p)
{
    if (!p || i >= 8) return D3DERR_INVALIDCALL;
    memcpy(Dev(This)->s.clip[i], p, 16);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetClipPlane(IDirect3DDevice8 *This, DWORD i, float *p)
{
    if (!p || i >= 8) return D3DERR_INVALIDCALL;
    memcpy(p, Dev(This)->s.clip[i], 16);
    return D3D_OK;
}

static void StateChanged(NxDevice *dev, DWORD s)
{
    switch (s) {
    case D3DRS_ALPHABLENDENABLE: case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC:   /* alpha to coverage: both */
        dev->dirty_states = dev->dirty_ps = TRUE;
        break;
    case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
    case D3DRS_CULLMODE: case D3DRS_FILLMODE: case D3DRS_ZBIAS: case D3DRS_COLORWRITEENABLE:
    case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC:
    case D3DRS_STENCILREF: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK:
        dev->dirty_states = TRUE;
        break;
    case D3DRS_TEXTUREFACTOR: case D3DRS_ALPHAREF: case D3DRS_FOGCOLOR:
        dev->dirty_ps = TRUE;
        break;
    case D3DRS_FOGENABLE: case D3DRS_SPECULARENABLE: case D3DRS_FOGTABLEMODE: case D3DRS_FOGSTART: case D3DRS_FOGEND:
    case D3DRS_FOGDENSITY:
        dev->dirty_ps = dev->dirty_vs = TRUE;
        break;
    /* what BuildVS reads (fog states are above); other states feed nothing that is cached */
    case D3DRS_AMBIENT: case D3DRS_LIGHTING: case D3DRS_NORMALIZENORMALS: case D3DRS_COLORVERTEX: case D3DRS_RANGEFOGENABLE:
    case D3DRS_FOGVERTEXMODE: case D3DRS_DIFFUSEMATERIALSOURCE: case D3DRS_SPECULARMATERIALSOURCE:
    case D3DRS_AMBIENTMATERIALSOURCE: case D3DRS_EMISSIVEMATERIALSOURCE: case D3DRS_LOCALVIEWER:
        dev->dirty_vs = TRUE;
        break;
    default:
        break;
    }
}

static HRESULT STDMETHODCALLTYPE D_SetRenderState(IDirect3DDevice8 *This, D3DRENDERSTATETYPE s, DWORD v)
{
    NxDevice *dev = Dev(This);
    if ((DWORD)s >= 256) return D3DERR_INVALIDCALL;
    if (dev->s.rs[s] == v) return D3D_OK;
    dev->s.rs[s] = v;
    StateChanged(dev, s);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetRenderState(IDirect3DDevice8 *This, D3DRENDERSTATETYPE s, DWORD *v)
{
    if (!v || (DWORD)s >= 256) return D3DERR_INVALIDCALL;
    *v = Dev(This)->s.rs[s];
    return D3D_OK;
}

/* ---- state blocks: a copy of the state and which bytes the block holds (recorded blocks: those that were set) ---- */
typedef struct NxBlock {
    NxState state;
    BYTE mask[sizeof(NxState)];
    NxTexture *tex[8];
    NxBuffer *stream[16];
    UINT stride[16];
    NxBuffer *indices;
    UINT base_vertex;
    BOOL bindings;          /* textures and streams are part of the block */
} NxBlock;
static NxBlock *g_recording_from;   /* state when BeginStateBlock was called */

static NxBlock *Block(NxDevice *dev, DWORD token)
{
    if (!token || token > NX_MAX_STATEBLOCKS || !dev->blocks[token - 1].state) return NULL;
    return (NxBlock *)dev->blocks[token - 1].state;
}
static DWORD NewBlock(NxDevice *dev, NxBlock **out)
{
    DWORD i;
    for (i = 0; i < NX_MAX_STATEBLOCKS; ++i)
        if (!dev->blocks[i].state) {
            NxBlock *b = (NxBlock *)calloc(1, sizeof *b);
            if (!b) return 0;
            dev->blocks[i].state = (struct NxState *)b;
            dev->blocks[i].token = i + 1;
            *out = b;
            return i + 1;
        }
    return 0;
}
static void CaptureBindings(NxDevice *dev, NxBlock *b)
{
    unsigned i;
    for (i = 0; i < 8; ++i) { Nx_AddRef(dev->tex[i]); Nx_Release(b->tex[i]); b->tex[i] = dev->tex[i]; }
    for (i = 0; i < 16; ++i) { Nx_AddRef(dev->stream[i]); Nx_Release(b->stream[i]); b->stream[i] = dev->stream[i]; b->stride[i] = dev->stride[i]; }
    Nx_AddRef(dev->indices); Nx_Release(b->indices); b->indices = dev->indices;
    b->base_vertex = dev->base_vertex;
}
static HRESULT STDMETHODCALLTYPE D_BeginStateBlock(IDirect3DDevice8 *This)
{
    NxDevice *dev = Dev(This);
    if (dev->recording) return D3DERR_INVALIDCALL;
    g_recording_from = (NxBlock *)calloc(1, sizeof *g_recording_from);
    if (!g_recording_from) return E_OUTOFMEMORY;
    g_recording_from->state = dev->s;
    dev->recording = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_EndStateBlock(IDirect3DDevice8 *This, DWORD *token)
{
    NxDevice *dev = Dev(This);
    NxBlock *b = NULL;
    size_t i;
    if (!dev->recording || !token) return D3DERR_INVALIDCALL;
    dev->recording = FALSE;
    *token = NewBlock(dev, &b);
    if (b) {
        const BYTE *before = (const BYTE *)&g_recording_from->state, *now = (const BYTE *)&dev->s;
        b->state = dev->s;
        for (i = 0; i < sizeof(NxState); ++i) b->mask[i] = before[i] != now[i];
    }
    /* recording does not change the device in D3D8 */
    dev->s = g_recording_from->state;
    dev->dirty_obj = dev->dirty_vs = dev->dirty_fix = dev->dirty_vsc = dev->dirty_ps = dev->dirty_states = dev->dirty_textures = TRUE;
    free(g_recording_from);
    g_recording_from = NULL;
    return *token ? D3D_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE D_ApplyStateBlock(IDirect3DDevice8 *This, DWORD token)
{
    NxDevice *dev = Dev(This);
    NxBlock *b = Block(dev, token);
    size_t i;
    if (!b) return D3DERR_INVALIDCALL;
    for (i = 0; i < sizeof(NxState); ++i)
        if (b->mask[i]) ((BYTE *)&dev->s)[i] = ((const BYTE *)&b->state)[i];
    if (b->bindings) {
        unsigned k;
        for (k = 0; k < 8; ++k) { Nx_AddRef(b->tex[k]); Nx_Release(dev->tex[k]); dev->tex[k] = b->tex[k]; }
        for (k = 0; k < 16; ++k) { Nx_AddRef(b->stream[k]); Nx_Release(dev->stream[k]); dev->stream[k] = b->stream[k]; dev->stride[k] = b->stride[k]; }
        Nx_AddRef(b->indices); Nx_Release(dev->indices); dev->indices = b->indices;
        dev->base_vertex = b->base_vertex;
    }
    dev->dirty_obj = dev->dirty_vs = dev->dirty_fix = dev->dirty_vsc = dev->dirty_ps = dev->dirty_states = dev->dirty_textures = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_CaptureStateBlock(IDirect3DDevice8 *This, DWORD token)
{
    NxDevice *dev = Dev(This);
    NxBlock *b = Block(dev, token);
    size_t i;
    if (!b) return D3DERR_INVALIDCALL;
    for (i = 0; i < sizeof(NxState); ++i)
        if (b->mask[i]) ((BYTE *)&b->state)[i] = ((const BYTE *)&dev->s)[i];
    if (b->bindings) CaptureBindings(dev, b);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_DeleteStateBlock(IDirect3DDevice8 *This, DWORD token)
{
    NxDevice *dev = Dev(This);
    NxBlock *b = Block(dev, token);
    unsigned k;
    if (!b) return D3DERR_INVALIDCALL;
    for (k = 0; k < 8; ++k) Nx_Release(b->tex[k]);
    for (k = 0; k < 16; ++k) Nx_Release(b->stream[k]);
    Nx_Release(b->indices);
    free(b);
    dev->blocks[token - 1].state = NULL;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_CreateStateBlock(IDirect3DDevice8 *This, D3DSTATEBLOCKTYPE type, DWORD *token)
{
    NxDevice *dev = Dev(This);
    NxBlock *b = NULL;
    (void)type;   /* pixel and vertex blocks hold everything too */
    if (!token) return D3DERR_INVALIDCALL;
    *token = NewBlock(dev, &b);
    if (!b) return E_OUTOFMEMORY;
    b->state = dev->s;
    memset(b->mask, 1, sizeof b->mask);
    b->bindings = TRUE;
    CaptureBindings(dev, b);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE D_SetClipStatus(IDirect3DDevice8 *This, const D3DCLIPSTATUS8 *c)
{
    if (!c) return D3DERR_INVALIDCALL;
    Dev(This)->s.clip_status = *c;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetClipStatus(IDirect3DDevice8 *This, D3DCLIPSTATUS8 *c)
{
    if (!c) return D3DERR_INVALIDCALL;
    *c = Dev(This)->s.clip_status;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetTexture(IDirect3DDevice8 *This, DWORD stage, IDirect3DBaseTexture8 **out)
{
    if (!out || stage >= 8) return D3DERR_INVALIDCALL;
    *out = (IDirect3DBaseTexture8 *)Dev(This)->tex[stage];
    Nx_AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetTexture(IDirect3DDevice8 *This, DWORD stage, IDirect3DBaseTexture8 *t)
{
    NxDevice *dev = Dev(This);
    NxTexture *n = (NxTexture *)t;
    if (stage >= 8) return D3D_OK;
    if (n && n->h.vtbl != nx_texture_vtbl && n->h.vtbl != nx_cube_vtbl) return D3DERR_INVALIDCALL;
    if (dev->tex[stage] == n) return D3D_OK;
    /* the pixel constants only say whether a stage has a texture and whether it is a cube (BuildPS): most swaps
       keep that, and rebuilding them on every swap was an upload on nearly every draw */
    if (!dev->tex[stage] != !n || (dev->tex[stage] && n && dev->tex[stage]->cube != n->cube)) dev->dirty_ps = TRUE;
    Nx_AddRef(n);
    Nx_Release(dev->tex[stage]);
    dev->tex[stage] = n;
    dev->tex_dirty |= 1u << stage;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetTextureStageState(IDirect3DDevice8 *This, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
{
    if (!v || stage >= 8 || (DWORD)t >= 32) return D3DERR_INVALIDCALL;
    *v = Dev(This)->s.tss[stage][t];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetTextureStageState(IDirect3DDevice8 *This, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    NxDevice *dev = Dev(This);
    if (stage >= 8 || (DWORD)t >= 32) return D3DERR_INVALIDCALL;
    if (dev->s.tss[stage][t] == v) return D3D_OK;
    dev->s.tss[stage][t] = v;
    switch (t) {
    case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV: case D3DTSS_ADDRESSW: case D3DTSS_BORDERCOLOR: case D3DTSS_MAGFILTER:
    case D3DTSS_MINFILTER: case D3DTSS_MIPFILTER: case D3DTSS_MIPMAPLODBIAS: case D3DTSS_MAXMIPLEVEL: case D3DTSS_MAXANISOTROPY:
        dev->smp_dirty |= 1u << stage;
        break;
    case D3DTSS_TEXCOORDINDEX: case D3DTSS_TEXTURETRANSFORMFLAGS:
        dev->dirty_vs = dev->dirty_ps = TRUE;
        break;
    default:
        dev->dirty_ps = TRUE;
        break;
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_ValidateDevice(IDirect3DDevice8 *This, DWORD *passes)
{
    (void)This;
    if (passes) *passes = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetInfo(IDirect3DDevice8 *This, DWORD id, void *data, DWORD size)
{ (void)This; (void)id; (void)data; (void)size; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE D_SetPaletteEntries(IDirect3DDevice8 *This, UINT i, const PALETTEENTRY *e) { (void)This; (void)i; (void)e; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE D_GetPaletteEntries(IDirect3DDevice8 *This, UINT i, PALETTEENTRY *e) { (void)This; (void)i; (void)e; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE D_SetCurrentTexturePalette(IDirect3DDevice8 *This, UINT i) { (void)This; (void)i; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE D_GetCurrentTexturePalette(IDirect3DDevice8 *This, UINT *i) { (void)This; if (i) *i = 0; return D3D_OK; }

/* render thread: the vertices first .. first + count of every stream go to the card (dynamic buffers send what draws use) */
static void NeedStreams(NxDevice *dev, UINT first, UINT count)
{
    UINT i;
    for (i = 0; i < 16; ++i)
        if (dev->stream[i] && dev->stream[i]->lazy)
            Nx_BufferNeed(dev, dev->stream[i], first * dev->stride[i], count ? (first + count) * dev->stride[i] : dev->stream[i]->size);
}

static HRESULT D_DrawPrimitiveImpl(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    NxDevice *dev = Dev(This);
    UINT count = VertexCount(t, n), off;
    if (!count || !Prepare(dev)) return D3D_OK;
    NeedStreams(dev, start, count);
    BindStreams(dev);
    Topology(dev, t);
    if (t == D3DPT_TRIANGLEFAN) {
        if (!FanIndices(dev, n, &off)) return D3D_OK;
        SetIndices11(dev, dev->up_ib, DXGI_FORMAT_R16_UINT, off);
        NxC_DrawIndexed(dev, n * 3, 0, (INT)start);
        return D3D_OK;
    }
    NxC_Draw(dev, count, start);
    return D3D_OK;
}

static HRESULT D_DrawIndexedPrimitiveImpl(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT min, UINT nv, UINT start, UINT n)
{
    NxDevice *dev = Dev(This);
    UINT count = VertexCount(t, n);
    NxBuffer *ib = dev->indices;
    DXGI_FORMAT f;
    if (!count || !ib || !Prepare(dev)) return D3D_OK;
    f = ib->format == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    NeedStreams(dev, dev->base_vertex + min, nv);   /* nv 0 (not given): the whole buffer */
    if (ib->lazy && t != D3DPT_TRIANGLEFAN) {
        UINT isize = ib->format == D3DFMT_INDEX32 ? 4 : 2;
        Nx_BufferNeed(dev, ib, start * isize, (start + count) * isize);
    }
    BindStreams(dev);
    Topology(dev, t);
    if (t == D3DPT_TRIANGLEFAN) {
        UINT size = ib->format == D3DFMT_INDEX32 ? 4 : 2, i, off;
        BYTE tmp[3 * 2048 * 4];
        if (!ib->shadow) {   /* a mapped (write-only) index buffer cannot be read back */
            static int told;
            if (!told) { told = 1; Nx_Log("indexed triangle fan from a write-only index buffer: not drawn"); }
            return D3D_OK;
        }
        if (n > 2048 || (start + n + 2) * size > ib->size) return D3D_OK;
        for (i = 0; i < n; ++i) {
            memcpy(tmp + (3 * i) * size, ib->shadow + start * size, size);
            memcpy(tmp + (3 * i + 1) * size, ib->shadow + (start + i + 1) * size, size);
            memcpy(tmp + (3 * i + 2) * size, ib->shadow + (start + i + 2) * size, size);
        }
        if (!Ring(dev, TRUE, tmp, n * 3 * size, size, &off)) return D3D_OK;
        SetIndices11(dev, dev->up_ib, f, off);
        NxC_DrawIndexed(dev, n * 3, 0, (INT)dev->base_vertex);
        return D3D_OK;
    }
    SetIndices11(dev, ib->buf, f, 0);
    ++nx_stats.draws;
    NxC_DrawIndexed(dev, count, start, (INT)dev->base_vertex);
    return D3D_OK;
}

static void AfterUP(NxDevice *dev)
{
    /* D3D8: the UP draws leave no stream 0 and no indices */
    Nx_Release(dev->stream[0]); dev->stream[0] = NULL; dev->stride[0] = 0;
    Nx_Release(dev->indices); dev->indices = NULL;
}

static HRESULT D_DrawPrimitiveUPImpl(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride)
{
    NxDevice *dev = Dev(This);
    UINT count = VertexCount(t, n), off, ioff;
    if (!count || !data || !stride) return D3D_OK;
    AfterUP(dev);
    if (!Prepare(dev) || !Ring(dev, FALSE, data, count * stride, stride, &off)) return D3D_OK;
    SetStream(dev, 0, dev->up_vb, stride, off);
    if (!(dev->s.vs & 1)) SetStream(dev, 15, dev->ff_defaults, 0, 0);
    Topology(dev, t);
    if (t == D3DPT_TRIANGLEFAN) {
        if (!FanIndices(dev, n, &ioff)) return D3D_OK;
        SetIndices11(dev, dev->up_ib, DXGI_FORMAT_R16_UINT, ioff);
        NxC_DrawIndexed(dev, n * 3, 0, 0);
        return D3D_OK;
    }
    NxC_Draw(dev, count, 0);
    return D3D_OK;
}

static HRESULT D_DrawIndexedPrimitiveUPImpl(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT min, UINT nv, UINT n,
                                                          const void *idx, D3DFORMAT f, const void *data, UINT stride)
{
    NxDevice *dev = Dev(This);
    UINT count = VertexCount(t, n), size = f == D3DFMT_INDEX32 ? 4 : 2, voff, ioff;
    if (!count || !data || !idx || !stride) return D3D_OK;
    AfterUP(dev);
    if (!Prepare(dev) || !Ring(dev, FALSE, data, (min + nv) * stride, stride, &voff)) return D3D_OK;
    SetStream(dev, 0, dev->up_vb, stride, voff);
    if (!(dev->s.vs & 1)) SetStream(dev, 15, dev->ff_defaults, 0, 0);
    Topology(dev, t);
    if (t == D3DPT_TRIANGLEFAN) {
        BYTE tmp[3 * 2048 * 4];
        UINT i;
        if (n > 2048) return D3D_OK;
        for (i = 0; i < n; ++i) {
            memcpy(tmp + (3 * i) * size, idx, size);
            memcpy(tmp + (3 * i + 1) * size, (const BYTE *)idx + (i + 1) * size, size);
            memcpy(tmp + (3 * i + 2) * size, (const BYTE *)idx + (i + 2) * size, size);
        }
        if (!Ring(dev, TRUE, tmp, n * 3 * size, size, &ioff)) return D3D_OK;
        count = n * 3;
    } else if (!Ring(dev, TRUE, idx, count * size, size, &ioff)) {
        return D3D_OK;
    }
    SetIndices11(dev, dev->up_ib, size == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, ioff);
    NxC_DrawIndexed(dev, count, 0, 0);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE D_ProcessVertices(IDirect3DDevice8 *This, UINT a, UINT b, UINT c, IDirect3DVertexBuffer8 *d, DWORD f)
{ (void)This; (void)a; (void)b; (void)c; (void)d; (void)f; return D3DERR_INVALIDCALL; }

static HRESULT STDMETHODCALLTYPE D_CreateVertexShader(IDirect3DDevice8 *This, const DWORD *decl, const DWORD *code, DWORD *handle, DWORD usage)
{
    NxDevice *dev = Dev(This);
    NxShaderVS *s = NULL;
    D3D11_INPUT_ELEMENT_DESC el[16];
    ID3DBlob *blob = NULL;
    char *hlsl;
    unsigned i;
    HRESULT hr;
    (void)usage;
    if (!decl || !handle) return D3DERR_INVALIDCALL;
    if (!code) { Nx_Log("vertex shader without code (fixed function with a declaration) is not supported"); return D3DERR_INVALIDCALL; }
    for (i = 0; i < NX_MAX_VS; ++i) if (!dev->vsh[i].used) { s = &dev->vsh[i]; break; }
    if (!s) return D3DERR_OUTOFVIDEOMEMORY;
    memset(s, 0, sizeof *s);
    if (!NxVS_ParseDeclaration(decl, &s->decl)) { Nx_Log("vertex shader declaration not supported"); return D3DERR_INVALIDCALL; }
    hlsl = (char *)malloc(65536);
    if (!hlsl) return E_OUTOFMEMORY;
    if (!NxVS_Translate(code, &s->decl, hlsl, 65536)) { free(hlsl); Nx_Log("vertex shader not translated"); return D3DERR_INVALIDCALL; }
    hr = Nx_CompileVS(dev, hlsl, &s->vs, &blob);
    free(hlsl);
    if (FAILED(hr)) return D3DERR_INVALIDCALL;
    memset(el, 0, sizeof el);
    for (i = 0; i < s->decl.count && i < 16; ++i) {
        el[i].SemanticName = "ATTR";
        el[i].SemanticIndex = s->decl.e[i].reg;
        el[i].Format = NxVS_Format(s->decl.e[i].type);
        el[i].InputSlot = s->decl.e[i].stream;
        el[i].AlignedByteOffset = s->decl.e[i].offset;
        el[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    }
    hr = ID3D11Device_CreateInputLayout(dev->d, el, s->decl.count, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &s->layout);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) {
        Nx_Log("vertex shader input layout: %08lx", (unsigned long)hr);
        ID3D11VertexShader_Release(s->vs);
        s->vs = NULL;
        return D3DERR_INVALIDCALL;
    }
    s->used = TRUE;
    s->handle = (DWORD)((s - dev->vsh) << 1) | 1;
    *handle = s->handle;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetVertexShader(IDirect3DDevice8 *This, DWORD handle)
{
    NxDevice *dev = Dev(This);
    NxShaderVS *s;
    unsigned i;
    if (!(handle & 1) && handle != dev->s.vs) dev->dirty_obj = TRUE;   /* NxObj carries the FVF */
    dev->s.vs = handle;
    if ((s = GameShader(dev, handle)) != NULL) {
        for (i = 0; i < s->decl.consts; ++i)
            if (s->decl.c[i].reg < 256) memcpy(dev->s.vsc[s->decl.c[i].reg], s->decl.c[i].v, 16);
        if (s->decl.consts) dev->dirty_vsc = TRUE;
    }
    dev->dirty_ps = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetVertexShader(IDirect3DDevice8 *This, DWORD *h)
{
    if (!h) return D3DERR_INVALIDCALL;
    *h = Dev(This)->s.vs;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_DeleteVertexShader(IDirect3DDevice8 *This, DWORD handle)
{
    NxShaderVS *s = GameShader(Dev(This), handle);
    if (!s) return D3DERR_INVALIDCALL;
    NxC_Release(Dev(This), s->vs);
    NxC_Release(Dev(This), s->layout);
    if (Dev(This)->bound_vs == s->vs) Dev(This)->bound_vs = NULL;
    if (Dev(This)->bound_layout == s->layout) Dev(This)->bound_layout = NULL;
    memset(s, 0, sizeof *s);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetVertexShaderConstant(IDirect3DDevice8 *This, DWORD reg, const void *data, DWORD count)
{
    NxDevice *dev = Dev(This);
    if (!data || reg >= 256) return D3DERR_INVALIDCALL;
    if (reg + count > 256) count = 256 - reg;
    memcpy(dev->s.vsc[reg], data, count * 16);
    dev->dirty_vsc = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetVertexShaderConstant(IDirect3DDevice8 *This, DWORD reg, void *data, DWORD count)
{
    NxDevice *dev = Dev(This);
    if (!data || reg >= 256) return D3DERR_INVALIDCALL;
    if (reg + count > 256) count = 256 - reg;
    memcpy(data, dev->s.vsc[reg], count * 16);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetVertexShaderDeclaration(IDirect3DDevice8 *This, DWORD h, void *d, DWORD *size)
{ (void)This; (void)h; (void)d; (void)size; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE D_GetVertexShaderFunction(IDirect3DDevice8 *This, DWORD h, void *d, DWORD *size)
{ (void)This; (void)h; (void)d; (void)size; return D3DERR_INVALIDCALL; }

static HRESULT STDMETHODCALLTYPE D_SetStreamSource(IDirect3DDevice8 *This, UINT n, IDirect3DVertexBuffer8 *vb, UINT stride)
{
    NxDevice *dev = Dev(This);
    NxBuffer *b = (NxBuffer *)vb;
    if (n >= 16) return D3DERR_INVALIDCALL;
    if (b && b->h.vtbl != nx_vb_vtbl) return D3DERR_INVALIDCALL;
    Nx_AddRef(b);
    Nx_Release(dev->stream[n]);
    dev->stream[n] = b;
    dev->stride[n] = stride;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetStreamSource(IDirect3DDevice8 *This, UINT n, IDirect3DVertexBuffer8 **vb, UINT *stride)
{
    NxDevice *dev = Dev(This);
    if (n >= 16 || !vb || !stride) return D3DERR_INVALIDCALL;
    *vb = (IDirect3DVertexBuffer8 *)dev->stream[n];
    Nx_AddRef(*vb);
    *stride = dev->stride[n];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetIndices(IDirect3DDevice8 *This, IDirect3DIndexBuffer8 *ib, UINT base)
{
    NxDevice *dev = Dev(This);
    NxBuffer *b = (NxBuffer *)ib;
    if (b && b->h.vtbl != nx_ib_vtbl) return D3DERR_INVALIDCALL;
    Nx_AddRef(b);
    Nx_Release(dev->indices);
    dev->indices = b;
    dev->base_vertex = base;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetIndices(IDirect3DDevice8 *This, IDirect3DIndexBuffer8 **ib, UINT *base)
{
    NxDevice *dev = Dev(This);
    if (!ib || !base) return D3DERR_INVALIDCALL;
    *ib = (IDirect3DIndexBuffer8 *)dev->indices;
    Nx_AddRef(*ib);
    *base = dev->base_vertex;
    return D3D_OK;
}
/* pixel shaders ps.1.0-1.3 (PostFX): translated to HLSL (dx11_ps.c) and compiled when created; handle = index + 1 */
static ID3D11PixelShader *PixelShader(NxDevice *dev, DWORD h)
{
    return h >= 1 && h <= NX_MAX_PS ? dev->psh[h - 1] : NULL;
}
static HRESULT STDMETHODCALLTYPE D_CreatePixelShader(IDirect3DDevice8 *This, const DWORD *code, DWORD *h)
{
    NxDevice *dev = Dev(This);
    char *hlsl;
    ID3DBlob *blob = NULL;
    HRESULT hr;
    unsigned i;
    if (!code || !h) return D3DERR_INVALIDCALL;
    *h = 0;
    for (i = 0; i < NX_MAX_PS && dev->psh[i]; ++i) {}
    if (i == NX_MAX_PS) return D3DERR_OUTOFVIDEOMEMORY;
    hlsl = (char *)malloc(32768);
    if (!hlsl) return E_OUTOFMEMORY;
    if (!NxPS_Translate(code, hlsl, 32768)) { free(hlsl); Nx_Log("pixel shader not translated (version %08lx)", (unsigned long)code[0]); return D3DERR_INVALIDCALL; }
    hr = Nx_CompilePS(dev, hlsl, &dev->psh[i], &blob);
    free(hlsl);
    if (blob) ID3D10Blob_Release(blob);
    if (FAILED(hr)) return D3DERR_INVALIDCALL;
    *h = i + 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetPixelShader(IDirect3DDevice8 *This, DWORD h)
{
    NxDevice *dev = Dev(This);
    if (h && !PixelShader(dev, h)) return D3DERR_INVALIDCALL;
    dev->s.ps = h;
    dev->dirty_ps = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetPixelShader(IDirect3DDevice8 *This, DWORD *h)
{
    if (!h) return D3DERR_INVALIDCALL;
    *h = Dev(This)->s.ps;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_DeletePixelShader(IDirect3DDevice8 *This, DWORD h)
{
    NxDevice *dev = Dev(This);
    ID3D11PixelShader *ps = PixelShader(dev, h);
    if (!ps) return D3DERR_INVALIDCALL;
    if (dev->bound_ps == ps) dev->bound_ps = NULL;
    NxC_Release(dev, ps);
    dev->psh[h - 1] = NULL;
    if (dev->s.ps == h) dev->s.ps = 0;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_SetPixelShaderConstant(IDirect3DDevice8 *This, DWORD reg, const void *data, DWORD count)
{
    NxDevice *dev = Dev(This);
    if (!data || reg >= 8) return D3DERR_INVALIDCALL;
    if (reg + count > 8) count = 8 - reg;
    memcpy(dev->s.psc[reg], data, count * 16);
    dev->dirty_psc = TRUE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetPixelShaderConstant(IDirect3DDevice8 *This, DWORD reg, void *data, DWORD count)
{
    NxDevice *dev = Dev(This);
    if (!data || reg >= 8) return D3DERR_INVALIDCALL;
    if (reg + count > 8) count = 8 - reg;
    memcpy(data, dev->s.psc[reg], count * 16);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE D_GetPixelShaderFunction(IDirect3DDevice8 *This, DWORD h, void *d, DWORD *size)
{ (void)This; (void)h; (void)d; (void)size; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE D_DrawRectPatch(IDirect3DDevice8 *This, UINT h, const float *s, const D3DRECTPATCH_INFO *i)
{ (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE D_DrawTriPatch(IDirect3DDevice8 *This, UINT h, const float *s, const D3DTRIPATCH_INFO *i)
{ (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE D_DeletePatch(IDirect3DDevice8 *This, UINT h) { (void)This; (void)h; return D3DERR_INVALIDCALL; }

/* draws timed for the frame report */
static HRESULT STDMETHODCALLTYPE D_DrawPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    LONGLONG t0;
    if (nx_dev_terrain_only && !nx_in_terrain) return D3D_OK;
    t0 = Nx_Now();
    HRESULT hr = D_DrawPrimitiveImpl(This, t, start, n);
    nx_stats.t_draw += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_DrawIndexedPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT min, UINT nv, UINT start, UINT n)
{
    LONGLONG t0;
    if (nx_dev_terrain_only && !nx_in_terrain) return D3D_OK;
    t0 = Nx_Now();
    HRESULT hr = D_DrawIndexedPrimitiveImpl(This, t, min, nv, start, n);
    nx_stats.t_draw += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_DrawPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride)
{
    LONGLONG t0;
    if (nx_dev_terrain_only && !nx_in_terrain) return D3D_OK;
    t0 = Nx_Now();
    HRESULT hr = D_DrawPrimitiveUPImpl(This, t, n, data, stride);
    nx_stats.t_draw += Nx_Now() - t0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE D_DrawIndexedPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT min, UINT nv, UINT n, const void *idx, D3DFORMAT f, const void *data, UINT stride)
{
    LONGLONG t0;
    if (nx_dev_terrain_only && !nx_in_terrain) return D3D_OK;
    t0 = Nx_Now();
    HRESULT hr = D_DrawIndexedPrimitiveUPImpl(This, t, min, nv, n, idx, f, data, stride);
    nx_stats.t_draw += Nx_Now() - t0;
    return hr;
}

static IDirect3DDevice8Vtbl kDeviceVtbl = {
    .QueryInterface = D_QueryInterface, .AddRef = D_AddRef, .Release = D_Release,
    .TestCooperativeLevel = D_TestCooperativeLevel, .GetAvailableTextureMem = D_GetAvailableTextureMem,
    .ResourceManagerDiscardBytes = D_ResourceManagerDiscardBytes, .GetDirect3D = D_GetDirect3D, .GetDeviceCaps = D_GetDeviceCaps,
    .GetDisplayMode = D_GetDisplayMode, .GetCreationParameters = D_GetCreationParameters,
    .SetCursorProperties = D_SetCursorProperties, .SetCursorPosition = D_SetCursorPosition, .ShowCursor = D_ShowCursor,
    .CreateAdditionalSwapChain = D_CreateAdditionalSwapChain, .Reset = D_Reset, .Present = D_Present,
    .GetBackBuffer = D_GetBackBuffer, .GetRasterStatus = D_GetRasterStatus, .SetGammaRamp = D_SetGammaRamp,
    .GetGammaRamp = D_GetGammaRamp, .CreateTexture = D_CreateTexture, .CreateVolumeTexture = D_CreateVolumeTexture,
    .CreateCubeTexture = D_CreateCubeTexture, .CreateVertexBuffer = D_CreateVertexBuffer, .CreateIndexBuffer = D_CreateIndexBuffer,
    .CreateRenderTarget = D_CreateRenderTarget, .CreateDepthStencilSurface = D_CreateDepthStencilSurface,
    .CreateImageSurface = D_CreateImageSurface, .CopyRects = D_CopyRects, .UpdateTexture = D_UpdateTexture,
    .GetFrontBuffer = D_GetFrontBuffer, .SetRenderTarget = D_SetRenderTarget, .GetRenderTarget = D_GetRenderTarget,
    .GetDepthStencilSurface = D_GetDepthStencilSurface, .BeginScene = D_BeginScene, .EndScene = D_EndScene, .Clear = D_Clear,
    .SetTransform = D_SetTransform, .GetTransform = D_GetTransform, .MultiplyTransform = D_MultiplyTransform,
    .SetViewport = D_SetViewport, .GetViewport = D_GetViewport, .SetMaterial = D_SetMaterial, .GetMaterial = D_GetMaterial,
    .SetLight = D_SetLight, .GetLight = D_GetLight, .LightEnable = D_LightEnable, .GetLightEnable = D_GetLightEnable,
    .SetClipPlane = D_SetClipPlane, .GetClipPlane = D_GetClipPlane, .SetRenderState = D_SetRenderState,
    .GetRenderState = D_GetRenderState, .BeginStateBlock = D_BeginStateBlock, .EndStateBlock = D_EndStateBlock,
    .ApplyStateBlock = D_ApplyStateBlock, .CaptureStateBlock = D_CaptureStateBlock, .DeleteStateBlock = D_DeleteStateBlock,
    .CreateStateBlock = D_CreateStateBlock, .SetClipStatus = D_SetClipStatus, .GetClipStatus = D_GetClipStatus,
    .GetTexture = D_GetTexture, .SetTexture = D_SetTexture, .GetTextureStageState = D_GetTextureStageState,
    .SetTextureStageState = D_SetTextureStageState, .ValidateDevice = D_ValidateDevice, .GetInfo = D_GetInfo,
    .SetPaletteEntries = D_SetPaletteEntries, .GetPaletteEntries = D_GetPaletteEntries,
    .SetCurrentTexturePalette = D_SetCurrentTexturePalette, .GetCurrentTexturePalette = D_GetCurrentTexturePalette,
    .DrawPrimitive = D_DrawPrimitive, .DrawIndexedPrimitive = D_DrawIndexedPrimitive, .DrawPrimitiveUP = D_DrawPrimitiveUP,
    .DrawIndexedPrimitiveUP = D_DrawIndexedPrimitiveUP, .ProcessVertices = D_ProcessVertices,
    .CreateVertexShader = D_CreateVertexShader, .SetVertexShader = D_SetVertexShader, .GetVertexShader = D_GetVertexShader,
    .DeleteVertexShader = D_DeleteVertexShader, .SetVertexShaderConstant = D_SetVertexShaderConstant,
    .GetVertexShaderConstant = D_GetVertexShaderConstant, .GetVertexShaderDeclaration = D_GetVertexShaderDeclaration,
    .GetVertexShaderFunction = D_GetVertexShaderFunction, .SetStreamSource = D_SetStreamSource,
    .GetStreamSource = D_GetStreamSource, .SetIndices = D_SetIndices, .GetIndices = D_GetIndices,
    .CreatePixelShader = D_CreatePixelShader, .SetPixelShader = D_SetPixelShader, .GetPixelShader = D_GetPixelShader,
    .DeletePixelShader = D_DeletePixelShader, .SetPixelShaderConstant = D_SetPixelShaderConstant,
    .GetPixelShaderConstant = D_GetPixelShaderConstant, .GetPixelShaderFunction = D_GetPixelShaderFunction,
    .DrawRectPatch = D_DrawRectPatch, .DrawTriPatch = D_DrawTriPatch, .DeletePatch = D_DeletePatch,
};

/* dev test: a file nx_shot.req in the game folder holding a BMP path: the next frame is written there */
static void Shot(NxDevice *dev)
{
    char path[MAX_PATH];
    NxSurface *b = dev->backbuffer;
    if (!b || !Renderer_ShotRequest(dev->frames, path, sizeof path) || FAILED(Nx_ReadSurface(b))) return;
    if (Renderer_WriteBMP(path, b->width, b->height, b->mem, b->pitch, b->fmt->bpp))
    {
        Nx_Log("frame %u written to %s", dev->frames, path);
        /* the camera of that frame as an nx_view.txt line (eye, a point 1000 ahead), to rebuild the view in a hidden test */
        Nx_Log("frame %u camera, nx_view: %.1f %.1f %.1f %.1f %.1f %.1f", dev->frames, nx_last_eye[0], nx_last_eye[1], nx_last_eye[2],
               nx_last_eye[0] + 1000.0f * nx_last_fwd[0], nx_last_eye[1] + 1000.0f * nx_last_fwd[1],
               nx_last_eye[2] + 1000.0f * nx_last_fwd[2]);
    }
}

static ID3D11Buffer *RingBuffer(NxDevice *dev, UINT size, UINT bind)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    memset(&d, 0, sizeof d);
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = bind;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev->d, &d, NULL, &b);
    return b;
}

HRESULT Nx_CreateDevice(IDirect3D8 *d3d, HWND window, DWORD behavior, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    NxDevice *dev;
    HRESULT hr;
    if (!pp || !out) return D3DERR_INVALIDCALL;
    *out = NULL;
    dev = (NxDevice *)calloc(1, sizeof *dev);
    if (!dev) return E_OUTOFMEMORY;
    dev->h.vtbl = &kDeviceVtbl;
    LoadViewOverride();
    dev->h.refs = 1;
    dev->h.kind = NXK_DEVICE;
    dev->h.dev = dev;
    dev->d3d = d3d;
    Nx_AddRef(d3d);
    dev->window = pp->hDeviceWindow ? pp->hDeviceWindow : window;
    dev->behavior = behavior;
    dev->pp = *pp;
    hr = Nx_D3D11Create(&dev->d, &dev->c, &dev->level);
    if (FAILED(hr)) { DeviceDestroy(dev); return D3DERR_NOTAVAILABLE; }
    {
        UINT support = 0;
        nx_bgra = SUCCEEDED(ID3D11Device_CheckFormatSupport(dev->d, DXGI_FORMAT_B8G8R8A8_UNORM, &support)) &&
                  (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
        Nx_CheckNativeFormats(dev->d);
    }
    {
        /* the factory that made the device's adapter: swap chains must come from it */
        static const GUID iid_f1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
        static const GUID iid_f2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
        static const GUID iid_f5 = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
        static const GUID iid_dxgidev = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev->d, &iid_dxgidev, (void **)&xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)))
            IDXGIAdapter_GetParent(ad, &iid_f1, (void **)&dev->factory1);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (dev->factory1) {
            IUnknown *f5 = NULL;
            IDXGIFactory1_QueryInterface(dev->factory1, &iid_f2, (void **)&dev->factory2);
            if (SUCCEEDED(IDXGIFactory1_QueryInterface(dev->factory1, &iid_f5, (void **)&f5)) && f5) {
                /* IDXGIFactory5::CheckFeatureSupport is the slot after the 4 IDXGIFactory4 methods */
                typedef HRESULT (STDMETHODCALLTYPE *Check)(IUnknown *, UINT, void *, UINT);
                BOOL allow = FALSE;
                Check check = (Check)((void **)*(void **)f5)[28];
                if (SUCCEEDED(check(f5, 0 /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */, &allow, sizeof allow)) && allow) dev->tearing = TRUE;
                f5->lpVtbl->Release(f5);
            }
        }
    }
    if (!Nx_ShadersInit(dev)) { Nx_Log("shaders did not compile"); DeviceDestroy(dev); return D3DERR_NOTAVAILABLE; }
    {
        D3D11_BUFFER_DESC d;
        memset(&d, 0, sizeof d);
        d.ByteWidth = 16; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(dev->d, &d, NULL, &dev->cb_blit);
    }
    dev->up_vb_size = UP_VB_SIZE;
    dev->up_ib_size = UP_IB_SIZE;
    dev->up_vb = RingBuffer(dev, UP_VB_SIZE, D3D11_BIND_VERTEX_BUFFER);
    dev->up_ib = RingBuffer(dev, UP_IB_SIZE, D3D11_BIND_INDEX_BUFFER);
    FullScreenWindow(dev);
    {
        char f[MAX_PATH + 32];
        FILE *t;
        snprintf(f, sizeof f, "%s/nx_dx11_dbg.txt", Renderer_GameFolder());
        if ((t = fopen(f, "r")) != NULL) { if (fscanf(t, "%u", &nx_dbg) != 1) nx_dbg = 0; fclose(t); Nx_Log("debug flags %u", nx_dbg); }
    }
    if (!dev->up_vb || !dev->up_ib || !dev->cb_blit || FAILED(CreateSwap(dev)) || FAILED(MakeBackBuffer(dev))) {
        DeviceDestroy(dev);
        return D3DERR_NOTAVAILABLE;
    }
    *pp = dev->pp;
    DefaultState(dev);
    FullViewport(dev);
    ForgetBindings(dev);
    BindTargets(dev);
    Nx_Log("Direct3D 11 device (feature level %x): %ux%u %s, back buffer format %d", (unsigned)dev->level, dev->pp.BackBufferWidth,
           dev->pp.BackBufferHeight, dev->pp.Windowed ? "window" : "full screen (borderless)", (int)dev->pp.BackBufferFormat);
    if (NxQ_Wanted()) Nx_Log(NxQ_Start(dev) ? "render thread: on (Direct3D 11 calls run on a second core)" : "render thread: could not start");
    *out = (IDirect3DDevice8 *)dev;
    return D3D_OK;
}

/* ---- for dx11_terrain.c ---- */
BOOL Nx_IsDevice(void *p) { return p && ((NxHead *)p)->vtbl == (void *)&kDeviceVtbl && ((NxHead *)p)->kind == NXK_DEVICE; }
BOOL Nx_IsTexture(void *p) { return p && ((NxHead *)p)->vtbl == nx_texture_vtbl && ((NxHead *)p)->kind == NXK_TEXTURE; }
ID3D11SamplerState *Nx_StageSampler(NxDevice *dev, DWORD stage) { return SamplerState(dev, stage); }
void Nx_ForgetBindings(NxDevice *dev) { ForgetBindings(dev); }
void Nx_BindTargets(NxDevice *dev) { BindTargets(dev); }
