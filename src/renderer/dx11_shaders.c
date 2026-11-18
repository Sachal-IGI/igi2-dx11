/* The Neonix renderer, Direct3D 11 mode: shaders.
   The renderer's own shaders (nx_ff_vs.hlsl, nx_ff_ps.hlsl, nx_util.hlsl) are compiled ahead of time into
   dx11_shader_code.h (tools/build_renderer_shaders.py); only the game's own vs.1.x shaders are compiled here, once each
   when the game creates them. One fixed-function vertex shader serves every FVF: each FVF gets an input layout in which
   the elements it lacks come from a defaults buffer in input slot 15 (stride 0). */
#include "dx11_internal.h"
#include "dx11_shader_code.h"

const unsigned char *Nx_TerrainCode(BOOL ps, SIZE_T *size)
{
    *size = ps ? sizeof kTerrainPS : sizeof kTerrainVS;
    return ps ? kTerrainPS : kTerrainVS;
}
#include <stdint.h>
#include <stdio.h>
#include <string.h>

HRESULT (WINAPI *nx_D3DCompile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
                                ID3DBlob **, ID3DBlob **);

/* the defaults buffer: what an FVF without the element reads */
#define DEF_TEXCOORD 0    /* float4 0, 0, 0, 1 */
#define DEF_NORMAL 16     /* float3 0, 0, 0 (D3D lights such vertices with ambient and emissive light only) */
#define DEF_DIFFUSE 32    /* B8G8R8A8 white */
#define DEF_SPECULAR 36   /* B8G8R8A8 black */

/* Shader cache. The game's shaders are translated to HLSL and compiled when the game creates them, about 0.25 s of
   D3DCompile at every start (sampled 2026-11-15). The bytecode is kept in plugins\nx_shaders, one file per shader named
   by a 64-bit hash of the target, the flags and the HLSL text, so later starts read it instead of compiling. A file is
   used only when its header matches (magic, format, the text's length and a second, independent hash); bytecode that
   Direct3D refuses is compiled again and the file replaced. Files are written to a temporary name and moved in, so two
   games starting at once never read half a file. Without D3DCreateBlob, or when the folder cannot be written, every
   shader is simply compiled as before. */
HRESULT (WINAPI *nx_D3DCreateBlob)(SIZE_T, ID3DBlob **);
static unsigned nx_cache_hits, nx_cache_compiled;
#define NX_CACHE_FLAGS (1u << 15) /* OPTIMIZATION_LEVEL3 */
typedef struct NxCacheHeader { char magic[4]; uint32_t format, srclen, check, codelen; } NxCacheHeader;

static uint64_t NxHash64(const char *target, const char *src, uint64_t h)
{
    const unsigned char *p;
    for (p = (const unsigned char *)target; *p; ++p) h = (h ^ *p) * 1099511628211ull;
    h = (h ^ 0xFFu) * 1099511628211ull;
    for (p = (const unsigned char *)src; *p; ++p) h = (h ^ *p) * 1099511628211ull;
    return h ^ NX_CACHE_FLAGS;
}

static BOOL NxCachePath(const char *target, const char *src, char *path, size_t cap, uint32_t *check)
{
    static int ready = -1;
    char dir[MAX_PATH + 32];
    uint64_t key = NxHash64(target, src, 14695981039346656037ull);
    *check = (uint32_t)(NxHash64(target, src, 0x84222325CBF29CE4ull) >> 16);
    if (!nx_D3DCreateBlob) return FALSE;
    snprintf(dir, sizeof dir, "%s\\plugins\\nx_shaders", Renderer_GameFolder());
    if (ready < 0) ready = CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (!ready) return FALSE;
    snprintf(path, cap, "%s\\%08lx%08lx.bin", dir, (unsigned long)(key >> 32), (unsigned long)key);
    return TRUE;
}

static BOOL NxCacheRead(const char *path, uint32_t srclen, uint32_t check, ID3DBlob **code)
{
    NxCacheHeader h;
    DWORD got = 0, size;
    BOOL ok = FALSE;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(f, NULL);
    if (ReadFile(f, &h, sizeof h, &got, NULL) && got == sizeof h && !memcmp(h.magic, "NXSC", 4) && h.format == 1 &&
        h.srclen == srclen && h.check == check && h.codelen > 0 && h.codelen < (1u << 20) && size == sizeof h + h.codelen &&
        SUCCEEDED(nx_D3DCreateBlob(h.codelen, code))) {
        ok = ReadFile(f, ID3D10Blob_GetBufferPointer(*code), h.codelen, &got, NULL) && got == h.codelen;
        if (!ok) { ID3D10Blob_Release(*code); *code = NULL; }
    }
    CloseHandle(f);
    return ok;
}

static void NxCacheWrite(const char *path, uint32_t srclen, uint32_t check, ID3DBlob *code)
{
    NxCacheHeader h;
    char temp[MAX_PATH + 112];
    DWORD put = 0;
    BOOL ok;
    HANDLE f;
    memcpy(h.magic, "NXSC", 4);
    h.format = 1; h.srclen = srclen; h.check = check;
    h.codelen = (uint32_t)ID3D10Blob_GetBufferSize(code);
    snprintf(temp, sizeof temp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    f = CreateFileA(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ok = WriteFile(f, &h, sizeof h, &put, NULL) && put == sizeof h &&
         WriteFile(f, ID3D10Blob_GetBufferPointer(code), h.codelen, &put, NULL) && put == h.codelen;
    CloseHandle(f);
    if (!ok || !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(temp);
}

static HRESULT CompileTarget(const char *src, const char *name, const char *target, ID3DBlob **code, BOOL cached, BOOL *hit)
{
    ID3DBlob *errors = NULL;
    DWORD start = GetTickCount();
    char path[MAX_PATH + 64];
    uint32_t check = 0, srclen = (uint32_t)strlen(src);
    BOOL have_path = NxCachePath(target, src, path, sizeof path, &check);
    HRESULT hr;
    *hit = FALSE;
    if (cached && have_path && NxCacheRead(path, srclen, check, code)) { ++nx_cache_hits; *hit = TRUE; return S_OK; }
    ++nx_stats.compiles;
    ++nx_cache_compiled;
    hr = nx_D3DCompile(src, srclen, name, NULL, NULL, "main", target, NX_CACHE_FLAGS, 0, code, &errors);
    if (GetTickCount() - start > 50) Nx_Log("compiling %s took %lu ms", name, (unsigned long)(GetTickCount() - start));
    if (FAILED(hr))
        Nx_Log("shader %s does not compile (%08lx): %.600s", name, (unsigned long)hr,
               errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
    else if (have_path)
        NxCacheWrite(path, srclen, check, *code);
    if (errors) ID3D10Blob_Release(errors);
    return hr;
}

void Nx_ShaderCacheReport(void)
{
    Nx_Log("shader cache: %u read from plugins\\nx_shaders, %u compiled", nx_cache_hits, nx_cache_compiled);
}

HRESULT Nx_CompileVS(NxDevice *dev, const char *hlsl, ID3D11VertexShader **vs, ID3DBlob **code)
{
    BOOL hit;
    HRESULT hr = CompileTarget(hlsl, "game vertex shader", "vs_4_0", code, TRUE, &hit);
    if (FAILED(hr)) return hr;
    hr = ID3D11Device_CreateVertexShader(dev->d, ID3D10Blob_GetBufferPointer(*code), ID3D10Blob_GetBufferSize(*code), NULL, vs);
    if (FAILED(hr) && hit) {   /* a cache file Direct3D refuses: compile it again (and replace the file) */
        ID3D10Blob_Release(*code);
        *code = NULL;
        hr = CompileTarget(hlsl, "game vertex shader", "vs_4_0", code, FALSE, &hit);
        if (FAILED(hr)) return hr;
        hr = ID3D11Device_CreateVertexShader(dev->d, ID3D10Blob_GetBufferPointer(*code), ID3D10Blob_GetBufferSize(*code), NULL, vs);
    }
    if (FAILED(hr)) { ID3D10Blob_Release(*code); *code = NULL; }
    return hr;
}

HRESULT Nx_CompilePS(NxDevice *dev, const char *hlsl, ID3D11PixelShader **ps, ID3DBlob **code)
{
    BOOL hit;
    HRESULT hr = CompileTarget(hlsl, "pixel shader", "ps_4_0", code, TRUE, &hit);
    if (FAILED(hr)) return hr;
    hr = ID3D11Device_CreatePixelShader(dev->d, ID3D10Blob_GetBufferPointer(*code), ID3D10Blob_GetBufferSize(*code), NULL, ps);
    if (FAILED(hr) && hit) {
        ID3D10Blob_Release(*code);
        *code = NULL;
        hr = CompileTarget(hlsl, "pixel shader", "ps_4_0", code, FALSE, &hit);
        if (FAILED(hr)) return hr;
        hr = ID3D11Device_CreatePixelShader(dev->d, ID3D10Blob_GetBufferPointer(*code), ID3D10Blob_GetBufferSize(*code), NULL, ps);
    }
    if (FAILED(hr)) { ID3D10Blob_Release(*code); *code = NULL; }
    return hr;
}

UINT Nx_FVFSize(DWORD fvf)
{
    static const BYTE pos[8] = {0, 12, 16, 16, 20, 24, 28, 32};
    UINT size = pos[(fvf & D3DFVF_POSITION_MASK) >> 1], n, i;
    if (fvf & D3DFVF_NORMAL) size += 12;
    if (fvf & D3DFVF_PSIZE) size += 4;
    if (fvf & D3DFVF_DIFFUSE) size += 4;
    if (fvf & D3DFVF_SPECULAR) size += 4;
    n = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (i = 0; i < n && i < 8; ++i) {
        static const BYTE floats[4] = {2, 3, 4, 1};
        size += 4u * floats[(fvf >> (16 + 2 * i)) & 3];
    }
    return size;
}

/* the input layout of an FVF (and what NxObj tells the shader about it) */
NxFFShader *Nx_FFShader(NxDevice *dev, DWORD fvf)
{
    static const BYTE posbytes[8] = {0, 12, 16, 16, 20, 24, 28, 32};
    static const BYTE floats[4] = {2, 3, 4, 1};
    static const DXGI_FORMAT ffmt[5] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                        DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    D3D11_INPUT_ELEMENT_DESC el[12];
    unsigned ne = 0, i, ntex;
    UINT off;
    NxFFShader *s;
    HRESULT hr;
    for (i = 0; i < dev->ff_count; ++i)
        if (dev->ff[i].fvf == fvf) return dev->ff[i].layout ? &dev->ff[i] : NULL;
    if (dev->ff_count >= sizeof dev->ff / sizeof dev->ff[0]) return NULL;
    s = &dev->ff[dev->ff_count++];
    memset(s, 0, sizeof *s);
    s->fvf = fvf;
    s->vs = dev->vs_ff;
    memset(el, 0, sizeof el);
#define FROM(slot, at) do { el[ne].InputSlot = (slot); el[ne].AlignedByteOffset = (at); } while (0)
    el[ne].SemanticName = "POSITION";
    el[ne].Format = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
    FROM(0, 0);
    ++ne;
    s->info[0] = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    off = posbytes[(fvf & D3DFVF_POSITION_MASK) >> 1];
    el[ne].SemanticName = "NORMAL";
    el[ne].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    if (fvf & D3DFVF_NORMAL) { FROM(0, off); off += 12; s->info[1] = 1; } else FROM(15, DEF_NORMAL);
    ++ne;
    if (fvf & D3DFVF_PSIZE) off += 4;
    el[ne].SemanticName = "COLOR"; el[ne].SemanticIndex = 0; el[ne].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fvf & D3DFVF_DIFFUSE) { FROM(0, off); off += 4; s->info[2] = 1; } else FROM(15, DEF_DIFFUSE);
    ++ne;
    el[ne].SemanticName = "COLOR"; el[ne].SemanticIndex = 1; el[ne].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fvf & D3DFVF_SPECULAR) { FROM(0, off); off += 4; s->info[3] = 1; } else FROM(15, DEF_SPECULAR);
    ++ne;
    ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (ntex > 8) ntex = 8;
    for (i = 0; i < 8; ++i) {
        el[ne].SemanticName = "TEXCOORD";
        el[ne].SemanticIndex = i;
        if (i < ntex) {
            unsigned n = floats[(fvf >> (16 + 2 * i)) & 3];
            el[ne].Format = ffmt[n];
            FROM(0, off);
            off += 4 * n;
            s->texsize[i] = (int)n;
        } else {
            el[ne].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            FROM(15, DEF_TEXCOORD);
        }
        ++ne;
    }
#undef FROM
    hr = ID3D11Device_CreateInputLayout(dev->d, el, ne, kFFVS, sizeof kFFVS, &s->layout);
    if (FAILED(hr)) {
        Nx_Log("fvf %lx: input layout failed (%08lx)", (unsigned long)fvf, (unsigned long)hr);
        s->layout = NULL;
        return NULL;
    }
    return s;
}

static ID3D11Buffer *ConstantBuffer(NxDevice *dev, UINT size)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (size + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev->d, &d, NULL, &b);
    return b;
}

BOOL Nx_ShadersInit(NxDevice *dev)
{
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;
    unsigned k;
    hr = ID3D11Device_CreateVertexShader(dev->d, kFFVS, sizeof kFFVS, NULL, &dev->vs_ff);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev->d, kFFPS, sizeof kFFPS, NULL, &dev->ps_ff);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(dev->d, kBlitVS, sizeof kBlitVS, NULL, &dev->vs_blit);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev->d, kBlitPS, sizeof kBlitPS, NULL, &dev->ps_blit);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(dev->d, kClearVS, sizeof kClearVS, NULL, &dev->vs_clear);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev->d, kClearPS, sizeof kClearPS, NULL, &dev->ps_clear);
    if (FAILED(hr)) { Nx_Log("renderer shaders: %08lx", (unsigned long)hr); return FALSE; }
    {
        BYTE defaults[48];
        static const float tex[4] = {0, 0, 0, 1};
        D3D11_BUFFER_DESC d;
        D3D11_SUBRESOURCE_DATA init;
        memset(defaults, 0, sizeof defaults);
        memcpy(defaults + DEF_TEXCOORD, tex, sizeof tex);
        memset(defaults + DEF_DIFFUSE, 0xFF, 4);
        memset(&d, 0, sizeof d);
        d.ByteWidth = sizeof defaults;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init.pSysMem = defaults;
        init.SysMemPitch = init.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateBuffer(dev->d, &d, &init, &dev->ff_defaults))) return FALSE;
    }
    dev->cb_obj = ConstantBuffer(dev, sizeof(NxObjCB));
    dev->cb_vs = ConstantBuffer(dev, sizeof(NxVSCB));
    dev->cb_vsc = ConstantBuffer(dev, 256 * 16);
    dev->cb_fix = ConstantBuffer(dev, 16);
    dev->cb_ps = ConstantBuffer(dev, sizeof(NxPSCB));
    dev->cb_psc = ConstantBuffer(dev, 8 * 16);
    dev->cb_clear = ConstantBuffer(dev, 48);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11Device_CreateSamplerState(dev->d, &sd, &dev->blit_sampler);
    /* clear_depth: bit 0 depth written, bit 1 stencil written; clear_blend: [0] no colour, [1] colour */
    for (k = 0; k < 4; ++k) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = (k & 1) != 0;
        dd.DepthWriteMask = (k & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = (k & 2) != 0;
        dd.StencilReadMask = 0xFF;
        dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.BackFace = dd.FrontFace;
        ID3D11Device_CreateDepthStencilState(dev->d, &dd, &dev->clear_depth[k]);
    }
    for (k = 0; k < 2; ++k) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = k ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
        ID3D11Device_CreateBlendState(dev->d, &bd, &dev->clear_blend[k]);
    }
    return dev->cb_obj && dev->cb_vs && dev->cb_vsc && dev->cb_fix && dev->cb_ps && dev->cb_psc && dev->cb_clear && dev->blit_sampler;
}

void Nx_ShadersRelease(NxDevice *dev)
{
    unsigned i;
#define REL(type, p) do { if (p) { type##_Release(p); (p) = NULL; } } while (0)
    for (i = 0; i < dev->ff_count; ++i) REL(ID3D11InputLayout, dev->ff[i].layout);
    dev->ff_count = 0;
    REL(ID3D11VertexShader, dev->vs_ff);
    REL(ID3D11PixelShader, dev->ps_ff);
    REL(ID3D11PixelShader, dev->ps_blit);
    REL(ID3D11VertexShader, dev->vs_blit);
    REL(ID3D11VertexShader, dev->vs_clear);
    REL(ID3D11PixelShader, dev->ps_clear);
    REL(ID3D11Buffer, dev->ff_defaults);
    REL(ID3D11Buffer, dev->cb_clear);
    for (i = 0; i < 4; ++i) REL(ID3D11DepthStencilState, dev->clear_depth[i]);
    for (i = 0; i < 2; ++i) REL(ID3D11BlendState, dev->clear_blend[i]);
    REL(ID3D11Buffer, dev->cb_obj);
    REL(ID3D11Buffer, dev->cb_vs);
    REL(ID3D11Buffer, dev->cb_vsc);
    REL(ID3D11Buffer, dev->cb_fix);
    REL(ID3D11Buffer, dev->cb_ps);
    REL(ID3D11Buffer, dev->cb_psc);
    REL(ID3D11SamplerState, dev->blit_sampler);
#undef REL
}
