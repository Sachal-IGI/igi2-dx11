/* Trace mode of modules/renderer: the copies' bookkeeping and the call counts (plugins\nx_d3d8_trace.txt).
   Every real Direct3D 8 object has exactly one copy: resources and surfaces keep a pointer to theirs in their own private
   data (so it goes away with them), the few Direct3D, device and swap chain objects are in a small table. */
#include "renderer_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID nxt_private = {0x6e786e78, 0x7472, 0x6163, {0x65, 0x38, 0x31, 0x2d, 0x6e, 0x65, 0x6f, 0x6e}};

static CRITICAL_SECTION nxt_lock;
static LONG nxt_lock_ready;

static void Lock(void)
{
    if (InterlockedCompareExchange(&nxt_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&nxt_lock);
        InterlockedExchange(&nxt_lock_ready, 2);
    }
    while (nxt_lock_ready != 2) Sleep(0);
    EnterCriticalSection(&nxt_lock);
}
static void Unlock(void) { LeaveCriticalSection(&nxt_lock); }

/* ---- copies ---- */
static struct { void *real; NxT *copy; } nxt_table[64];

static BOOL InPrivateData(int kind) { return kind == NXT_SURF || kind == NXT_VOL || kind >= NXT_VB; }

static NxT *Find(int kind, void *real)
{
    NxT *copy = NULL;
    DWORD size = sizeof copy;
    unsigned i;
    if (InPrivateData(kind)) {
        /* Set/GetPrivateData are slots 4 and 5 of every resource, surface and volume */
        IDirect3DResource8 *r = (IDirect3DResource8 *)real;
        if (SUCCEEDED(r->lpVtbl->GetPrivateData(r, &nxt_private, &copy, &size)) && size == sizeof copy) return copy;
        return NULL;
    }
    for (i = 0; i < sizeof nxt_table / sizeof nxt_table[0]; ++i)
        if (nxt_table[i].real == real) return nxt_table[i].copy;
    return NULL;
}

void *NxT_Wrap(int kind, void *real)
{
    NxT *copy;
    unsigned i;
    if (!real) return NULL;
    if (kind < 0 || kind >= NXT_KINDS) return real;
    Lock();
    copy = Find(kind, real);
    if (!copy) {
        copy = (NxT *)calloc(1, sizeof *copy);
        if (!copy) { Unlock(); return real; }
        copy->vtbl = nxt_vtbls[kind];
        copy->real = (IUnknown *)real;
        copy->kind = kind;
        if (InPrivateData(kind)) {
            IDirect3DResource8 *r = (IDirect3DResource8 *)real;
            r->lpVtbl->SetPrivateData(r, &nxt_private, &copy, sizeof copy, 0);
        } else {
            for (i = 0; i < sizeof nxt_table / sizeof nxt_table[0] && nxt_table[i].real; ++i) {}
            if (i < sizeof nxt_table / sizeof nxt_table[0]) { nxt_table[i].real = real; nxt_table[i].copy = copy; }
        }
    }
    Unlock();
    return copy;
}

void *NxT_WrapBaseTexture(void *real)
{
    IDirect3DBaseTexture8 *t = (IDirect3DBaseTexture8 *)real;
    if (!t) return NULL;
    switch (t->lpVtbl->GetType(t)) {
    case D3DRTYPE_TEXTURE: return NxT_Wrap(NXT_TEX, real);
    case D3DRTYPE_CUBETEXTURE: return NxT_Wrap(NXT_CUBE, real);
    case D3DRTYPE_VOLUMETEXTURE: return NxT_Wrap(NXT_VOLTEX, real);
    case D3DRTYPE_VERTEXBUFFER: return NxT_Wrap(NXT_VB, real);
    case D3DRTYPE_INDEXBUFFER: return NxT_Wrap(NXT_IB, real);
    default: return real;
    }
}

void *NxT_WrapContainer(REFIID riid, void *real)
{
    if (!memcmp(riid, &nxt_IID_IDirect3DTexture8, sizeof(GUID))) return NxT_Wrap(NXT_TEX, real);
    if (!memcmp(riid, &nxt_IID_IDirect3DCubeTexture8, sizeof(GUID))) return NxT_Wrap(NXT_CUBE, real);
    if (!memcmp(riid, &nxt_IID_IDirect3DVolumeTexture8, sizeof(GUID))) return NxT_Wrap(NXT_VOLTEX, real);
    if (!memcmp(riid, &nxt_IID_IDirect3DDevice8, sizeof(GUID))) return NxT_Wrap(NXT_DEVICE, real);
    if (!memcmp(riid, &nxt_IID_IDirect3DBaseTexture8, sizeof(GUID))) return NxT_WrapBaseTexture(real);
    return real;
}

void *NxT_Unwrap(void *object)
{
    NxT *copy = (NxT *)object;
    int k;
    if (!copy) return NULL;
    for (k = 0; k < NXT_KINDS; ++k)
        if (copy->vtbl == nxt_vtbls[k]) return copy->real;
    NxT_Hit("foreign object passed in", __builtin_return_address(0));
    return object;
}

void NxT_Released(NxT *copy)
{
    unsigned i;
    Lock();
    if (!InPrivateData(copy->kind))
        for (i = 0; i < sizeof nxt_table / sizeof nxt_table[0]; ++i)
            if (nxt_table[i].copy == copy) { nxt_table[i].real = NULL; nxt_table[i].copy = NULL; }
    if (copy->kind == NXT_DEVICE) NxT_Write();
    copy->vtbl = NULL;
    Unlock();
    free(copy);
}

/* ---- call counts ---- */
#define NXT_SLOTS 16384u
#define NXT_KEY 200
typedef struct { char key[NXT_KEY]; unsigned count; unsigned callers; } Hit;
static Hit *nxt_hits;
static unsigned nxt_used;
static struct { const BYTE *base; DWORD size; char tag; } nxt_mods[4];

static void ModuleRange(int i, HMODULE m, char tag)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS *nt;
    if (!m) return;
    nt = (const IMAGE_NT_HEADERS *)((const BYTE *)m + dos->e_lfanew);
    nxt_mods[i].base = (const BYTE *)m;
    nxt_mods[i].size = nt->OptionalHeader.SizeOfImage;
    nxt_mods[i].tag = tag;
}

static unsigned CallerBit(void *caller)
{
    static const char tags[] = "EPUO";
    int i;
    if (!nxt_mods[0].base) {
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)CallerBit, &self);
        ModuleRange(0, GetModuleHandleA(NULL), 'E');
        ModuleRange(1, GetModuleHandleA("IGI2Patch.dll"), 'P');
        ModuleRange(2, self, 'U');
    }
    for (i = 0; i < 3; ++i)
        if (nxt_mods[i].base && (const BYTE *)caller >= nxt_mods[i].base &&
            (const BYTE *)caller < nxt_mods[i].base + nxt_mods[i].size)
            return 1u << (strchr(tags, nxt_mods[i].tag) - tags);
    return 8u;
}

void NxT_Hit(const char *key, void *caller)
{
    unsigned h = 2166136261u, i;
    const char *p;
    if (nx_trace_quiet) return;
    for (p = key; *p; ++p) h = (h ^ (BYTE)*p) * 16777619u;
    Lock();
    if (!nxt_hits) nxt_hits = (Hit *)calloc(NXT_SLOTS, sizeof(Hit));
    if (nxt_hits) {
        for (i = h & (NXT_SLOTS - 1);; i = (i + 1) & (NXT_SLOTS - 1)) {
            Hit *e = &nxt_hits[i];
            if (!e->count) {
                if (nxt_used >= NXT_SLOTS - 64) break;
                snprintf(e->key, sizeof e->key, "%s", key);
                ++nxt_used;
            } else if (strcmp(e->key, key)) {
                continue;
            }
            ++e->count;
            e->callers |= CallerBit(caller);
            break;
        }
    }
    Unlock();
}

static int ByKey(const void *a, const void *b)
{
    return strcmp((*(const Hit *const *)a)->key, (*(const Hit *const *)b)->key);
}

void NxT_Write(void)
{
    char path[MAX_PATH];
    FILE *f;
    Hit **list;
    unsigned i, n = 0;
    Lock();
    if (!nxt_hits) { Unlock(); return; }
    list = (Hit **)malloc(nxt_used * sizeof *list + 1);
    if (!list) { Unlock(); return; }
    for (i = 0; i < NXT_SLOTS; ++i)
        if (nxt_hits[i].count) list[n++] = &nxt_hits[i];
    qsort(list, n, sizeof *list, ByKey);
    snprintf(path, sizeof path, "%s\\plugins\\nx_d3d8_trace.txt", Renderer_GameFolder());
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "# Neonix renderer trace: count, callers (E igi2.exe, P IGI2Patch, U igi2_unified, O other), call\n");
        for (i = 0; i < n; ++i)
            fprintf(f, "%10u %c%c%c%c %s\n", list[i]->count, list[i]->callers & 1 ? 'E' : '-', list[i]->callers & 2 ? 'P' : '-',
                    list[i]->callers & 4 ? 'U' : '-', list[i]->callers & 8 ? 'O' : '-', list[i]->key);
        fclose(f);
    }
    free(list);
    Unlock();
}

static unsigned nxt_frames;
void NxT_Frame(IDirect3DDevice8 *real)
{
    char path[MAX_PATH];
    IDirect3DSurface8 *b = NULL;
    if (real && Renderer_ShotRequest(nxt_frames, path, sizeof path) &&
        SUCCEEDED(real->lpVtbl->GetBackBuffer(real, 0, D3DBACKBUFFER_TYPE_MONO, &b)) && b) {
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(b->lpVtbl->GetDesc(b, &d)) && SUCCEEDED(b->lpVtbl->LockRect(b, &lr, NULL, D3DLOCK_READONLY))) {
            unsigned bpp = d.Format == D3DFMT_R5G6B5 ? 2 : 4;
            if (Renderer_WriteBMP(path, d.Width, d.Height, (const BYTE *)lr.pBits, (unsigned)lr.Pitch, bpp)) {
                char text[MAX_PATH + 64];
                snprintf(text, sizeof text, "trace: frame %u written to %s", nxt_frames, path);
                Renderer_Log(text);
            }
            b->lpVtbl->UnlockRect(b);
        }
        b->lpVtbl->Release(b);
    }
    ++nxt_frames;
    Renderer_FrameTick();
    if (!nx_trace_quiet && (nxt_frames == 300 || nxt_frames == 1800 || nxt_frames % 3600 == 0)) NxT_Write();
}

/* ---- what is drawn with ---- */
static DWORD nxt_shader, nxt_pixel_shader, nxt_textures;
static struct { DWORD handle; unsigned hash; } nxt_shader_names[256];
static unsigned nxt_last_hash, nxt_shader_count;

void NxT_SetShader(DWORD handle) { nxt_shader = handle; }
void NxT_SetPixelShader(DWORD handle) { nxt_pixel_shader = handle; }
void NxT_SetTexture(DWORD stage, IDirect3DBaseTexture8 *real)
{
    if (stage >= 8) return;
    if (real) nxt_textures |= 1u << stage; else nxt_textures &= ~(1u << stage);
}
void NxT_ShaderHandle(DWORD handle)
{
    Lock();
    if (nxt_shader_count < 256) {
        nxt_shader_names[nxt_shader_count].handle = handle;
        nxt_shader_names[nxt_shader_count].hash = nxt_last_hash;
        ++nxt_shader_count;
    }
    Unlock();
}

void NxT_DrawState(char *key, size_t cap, const char *what, DWORD prim)
{
    char shader[24];
    unsigned i;
    if (nxt_shader & 1) {
        snprintf(shader, sizeof shader, "vs?");
        for (i = 0; i < nxt_shader_count; ++i)
            if (nxt_shader_names[i].handle == nxt_shader) snprintf(shader, sizeof shader, "vs_%08x", nxt_shader_names[i].hash);
    } else {
        snprintf(shader, sizeof shader, "fvf=%lx", (unsigned long)nxt_shader);
    }
    snprintf(key, cap, "%s %s %s tex=%lx%s", what, NxT_Prim(prim), shader, (unsigned long)nxt_textures,
             nxt_pixel_shader ? " ps" : "");
}

/* the shader's tokens to plugins\nx_trace\vs_<hash>.txt, once */
void NxT_Shader(char *key, size_t cap, const char *kind, const DWORD *decl, const DWORD *code, DWORD usage)
{
    unsigned h = 2166136261u, nd = 0, nc = 0, i;
    char path[MAX_PATH];
    FILE *f;
    if (decl) while (nd < 4096 && decl[nd] != 0xFFFFFFFFu) ++nd;
    if (code) while (nc < 65536 && code[nc] != 0x0000FFFFu) ++nc;
    for (i = 0; decl && i <= nd; ++i) h = (h ^ decl[i]) * 16777619u;
    for (i = 0; code && i <= nc; ++i) h = (h ^ code[i]) * 16777619u;
    nxt_last_hash = h;
    snprintf(key, cap, "Dev.Create%sShader %s_%08x decl=%u code=%u%s usage=%lx", kind[0] == 'v' ? "Vertex" : "Pixel", kind, h,
             nd, nc, code ? "" : " fixed", (unsigned long)usage);
    snprintf(path, sizeof path, "%s\\plugins\\nx_trace", Renderer_GameFolder());
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\plugins\\nx_trace\\%s_%08x.txt", Renderer_GameFolder(), kind, h);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "decl %u\n", nd);
    for (i = 0; decl && i <= nd; ++i) fprintf(f, "%08lx%c", (unsigned long)decl[i], i % 8 == 7 ? '\n' : ' ');
    fprintf(f, "\ncode %u\n", nc);
    for (i = 0; code && i <= nc; ++i) fprintf(f, "%08lx%c", (unsigned long)code[i], i % 8 == 7 ? '\n' : ' ');
    fprintf(f, "\n");
    fclose(f);
}

/* ---- names ---- */
static char *Ring(void)
{
    static char ring[8][48];
    static unsigned at;
    return ring[at++ & 7];
}

const char *NxT_TexFormat(IDirect3DBaseTexture8 *t)
{
    char *out = Ring();
    D3DSURFACE_DESC d;
    D3DVOLUME_DESC v;
    if (!t) return "NULL";
    switch (t->lpVtbl->GetType(t)) {
    case D3DRTYPE_TEXTURE:
        if (FAILED(((IDirect3DTexture8 *)t)->lpVtbl->GetLevelDesc((IDirect3DTexture8 *)t, 0, &d))) return "tex?";
        snprintf(out, 48, "%s/%s", NxT_Fmt(d.Format), NxT_Pool(d.Pool));
        return out;
    case D3DRTYPE_CUBETEXTURE:
        if (FAILED(((IDirect3DCubeTexture8 *)t)->lpVtbl->GetLevelDesc((IDirect3DCubeTexture8 *)t, 0, &d))) return "cube?";
        snprintf(out, 48, "cube %s/%s", NxT_Fmt(d.Format), NxT_Pool(d.Pool));
        return out;
    case D3DRTYPE_VOLUMETEXTURE:
        if (FAILED(((IDirect3DVolumeTexture8 *)t)->lpVtbl->GetLevelDesc((IDirect3DVolumeTexture8 *)t, 0, &v))) return "vol?";
        snprintf(out, 48, "volume %s/%s", NxT_Fmt(v.Format), NxT_Pool(v.Pool));
        return out;
    default:
        return "texture?";
    }
}

const char *NxT_SurfFormat(IDirect3DSurface8 *s)
{
    char *out = Ring();
    D3DSURFACE_DESC d;
    if (!s) return "NULL";
    if (FAILED(s->lpVtbl->GetDesc(s, &d))) return "surface?";
    snprintf(out, 48, "%s/%s%s", NxT_Fmt(d.Format), NxT_Pool(d.Pool),
             d.Usage & D3DUSAGE_RENDERTARGET ? " rt" : d.Usage & D3DUSAGE_DEPTHSTENCIL ? " z" : "");
    return out;
}

void NxT_PresentParams(char *key, size_t cap, const char *what, const D3DPRESENT_PARAMETERS *pp)
{
    if (!pp) { snprintf(key, cap, "%s NULL", what); return; }
    snprintf(key, cap, "%s bb=%s x%u ms=%d swap=%d windowed=%d autodepth=%d %s flags=%lx hz=%u interval=%lx", what,
             NxT_Fmt(pp->BackBufferFormat), pp->BackBufferCount, (int)pp->MultiSampleType, (int)pp->SwapEffect,
             (int)pp->Windowed, (int)pp->EnableAutoDepthStencil, NxT_Fmt(pp->AutoDepthStencilFormat),
             (unsigned long)pp->Flags, pp->FullScreen_RefreshRateInHz, (unsigned long)pp->FullScreen_PresentationInterval);
}

static const char *Arg(char *out, size_t cap, DWORD v)
{
    static const char *const sel[] = {"DIFFUSE", "CURRENT", "TEXTURE", "TFACTOR", "SPECULAR", "TEMP"};
    snprintf(out, cap, "%s%s%s", (v & 0xF) < 6 ? sel[v & 0xF] : "?", v & D3DTA_COMPLEMENT ? "|COMPLEMENT" : "",
             v & D3DTA_ALPHAREPLICATE ? "|ALPHAREPLICATE" : "");
    return out;
}

void NxT_RSValue(char *out, size_t cap, DWORD s, DWORD v)
{
    switch (s) {
    case D3DRS_FOGSTART: case D3DRS_FOGEND: case D3DRS_FOGDENSITY: case D3DRS_POINTSIZE: case D3DRS_POINTSIZE_MIN:
    case D3DRS_POINTSIZE_MAX: case D3DRS_POINTSCALE_A: case D3DRS_POINTSCALE_B: case D3DRS_POINTSCALE_C:
    case D3DRS_TWEENFACTOR: case D3DRS_PATCHSEGMENTS:
        snprintf(out, cap, "float");
        return;
    case D3DRS_FOGCOLOR: case D3DRS_AMBIENT: case D3DRS_TEXTUREFACTOR:
        snprintf(out, cap, "color");
        return;
    case D3DRS_SRCBLEND: case D3DRS_DESTBLEND:
        snprintf(out, cap, "%s", NxT_Blend(v));
        return;
    case D3DRS_ZFUNC: case D3DRS_ALPHAFUNC: case D3DRS_STENCILFUNC:
        snprintf(out, cap, "%s", NxT_Cmp(v));
        return;
    default:
        snprintf(out, cap, v > 255 ? "%lx" : "%lu", (unsigned long)v);
    }
}

void NxT_TSSValue(char *out, size_t cap, DWORD t, DWORD v)
{
    switch (t) {
    case D3DTSS_COLOROP: case D3DTSS_ALPHAOP:
        snprintf(out, cap, "%s", NxT_Top(v));
        return;
    case D3DTSS_COLORARG0: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2: case D3DTSS_ALPHAARG0: case D3DTSS_ALPHAARG1:
    case D3DTSS_ALPHAARG2: case D3DTSS_RESULTARG:
        Arg(out, cap, v);
        return;
    case D3DTSS_MAGFILTER: case D3DTSS_MINFILTER: case D3DTSS_MIPFILTER:
        snprintf(out, cap, "%s", NxT_Filter(v));
        return;
    case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV: case D3DTSS_ADDRESSW:
        snprintf(out, cap, "%s", NxT_Addr(v));
        return;
    case D3DTSS_BUMPENVMAT00: case D3DTSS_BUMPENVMAT01: case D3DTSS_BUMPENVMAT10: case D3DTSS_BUMPENVMAT11:
    case D3DTSS_BUMPENVLSCALE: case D3DTSS_BUMPENVLOFFSET: case D3DTSS_MIPMAPLODBIAS:
        snprintf(out, cap, "float");
        return;
    case D3DTSS_BORDERCOLOR:
        snprintf(out, cap, "color");
        return;
    default:
        snprintf(out, cap, "%lx", (unsigned long)v);
    }
}

/* the real driver's caps and adapter, once, to plugins
x_trace\caps8.bin (D3DCAPS8 bytes) and caps8.txt */
void NxT_DumpCaps(IDirect3D8 *d3d)
{
    char path[MAX_PATH];
    D3DCAPS8 caps;
    D3DADAPTER_IDENTIFIER8 id;
    FILE *f;
    if (FAILED(d3d->lpVtbl->GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps))) return;
    snprintf(path, sizeof path, "%s\\plugins\\nx_trace", Renderer_GameFolder());
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\plugins\\nx_trace\\caps8.bin", Renderer_GameFolder());
    if ((f = fopen(path, "wb")) != NULL) { fwrite(&caps, sizeof caps, 1, f); fclose(f); }
    snprintf(path, sizeof path, "%s\\plugins\\nx_trace\\caps8.txt", Renderer_GameFolder());
    if ((f = fopen(path, "w")) == NULL) return;
    memset(&id, 0, sizeof id);
    d3d->lpVtbl->GetAdapterIdentifier(d3d, 0, 0, &id);
    fprintf(f, "adapter %s / %s vendor %04lx device %04lx\n", id.Description, id.Driver, (unsigned long)id.VendorId,
            (unsigned long)id.DeviceId);
    fprintf(f, "Caps %lx Caps2 %lx Caps3 %lx PresentationIntervals %lx CursorCaps %lx DevCaps %lx\n", caps.Caps, caps.Caps2,
            caps.Caps3, caps.PresentationIntervals, caps.CursorCaps, caps.DevCaps);
    fprintf(f, "PrimitiveMiscCaps %lx RasterCaps %lx ZCmpCaps %lx SrcBlendCaps %lx DestBlendCaps %lx AlphaCmpCaps %lx\n",
            caps.PrimitiveMiscCaps, caps.RasterCaps, caps.ZCmpCaps, caps.SrcBlendCaps, caps.DestBlendCaps, caps.AlphaCmpCaps);
    fprintf(f, "ShadeCaps %lx TextureCaps %lx TextureFilterCaps %lx CubeTextureFilterCaps %lx VolumeTextureFilterCaps %lx\n",
            caps.ShadeCaps, caps.TextureCaps, caps.TextureFilterCaps, caps.CubeTextureFilterCaps, caps.VolumeTextureFilterCaps);
    fprintf(f, "TextureAddressCaps %lx LineCaps %lx MaxTexture %lux%lu aspect %lu aniso %lu StencilCaps %lx FVFCaps %lx\n",
            caps.TextureAddressCaps, caps.LineCaps, caps.MaxTextureWidth, caps.MaxTextureHeight, caps.MaxTextureAspectRatio,
            caps.MaxAnisotropy, caps.StencilCaps, caps.FVFCaps);
    fprintf(f, "TextureOpCaps %lx MaxTextureBlendStages %lu MaxSimultaneousTextures %lu VertexProcessingCaps %lx MaxActiveLights %lu\n",
            caps.TextureOpCaps, caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures, caps.VertexProcessingCaps,
            caps.MaxActiveLights);
    fprintf(f, "MaxUserClipPlanes %lu MaxVertexBlendMatrices %lu MaxPrimitiveCount %lu MaxVertexIndex %lu MaxStreams %lu\n",
            caps.MaxUserClipPlanes, caps.MaxVertexBlendMatrices, caps.MaxPrimitiveCount, caps.MaxVertexIndex, caps.MaxStreams);
    fprintf(f, "VertexShaderVersion %lx MaxVertexShaderConst %lu PixelShaderVersion %lx MaxPixelShaderValue %g MaxPointSize %g\n",
            caps.VertexShaderVersion, caps.MaxVertexShaderConst, caps.PixelShaderVersion, (double)caps.MaxPixelShaderValue,
            (double)caps.MaxPointSize);
    fclose(f);
}
