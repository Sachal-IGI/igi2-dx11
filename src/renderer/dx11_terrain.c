/* The terrain on the GPU (Direct3D 11 mode).
   igi2.exe 1.3 rebuilds its terrain mesh on the CPU every frame (TerrainRender_Draw 0x609610: a view-dependent bintree with
   morphing, fog per vertex and per-layer blend weights, then one pass per layer and a lightmap pass): about half of the
   game's main thread. In Direct3D 11 mode that draw is replaced: the height maps and material bytes go to the card once,
   and every frame each map is drawn in one pass (nx_terrain.hlsl): full detail near the camera, coarser with distance
   (Select below), in nodes of 16 x 16 cells that are in view. The game's own setup still runs (its state methods and TerrainRender_SetupView, which sets the vertex shader
   constants and the matrices other code reads), and afterwards the render states its last pass would leave.
   [Renderer] Terrain = cpu keeps the game's own terrain; dev: nx_terrain_cpu.on, nx_terrain_dump.on (game folder).

   Engine facts (igi2 1.3, from IDA and the team's IGI2-RE terrain.c; checked against a dump of a real draw):
   - the draw is a render method, cdecl (ctx, ?, ?); ctx is a per-draw context on the caller's stack (TvTerrain):
     +0x30 layer per block [16], +0x70 layer count, +0x94 lightmap holder per map [4] (texture at +0x24),
     +0xA4 / +0xC4 lightmap uv = (p - A) * B per map, +0xE4 no lightmap pass, +0x668 single pass,
     +236 / +276 map 0 size x / y, +316 heights per map, +1396 material bytes per map, +1236 / +1240 map origin in fine
     units (x, y; 8 bytes per map), +1316 shift (cell = fine unit << shift), +1436 / +1440 samples w, h (8 bytes per map),
     +1516 / +1520 centre offset. Map 0's sample (0, 0) is at -max(w, h) * cell / 2 + offset + cell / 2.
   - layer: +0x30 has texture 0, +0x58 texture 0, +0x260 its scale, +0x264 has texture 1, +0x28C texture 1, +0x494 its
     scale, +0x4AC planar axis (0 xy, 1 xz, 2 yz); uv = p.axes / (scale * 4096).
   - fog = 1 / e^(dot(plane at 0x834B068, (x, y, z, 1)) * density 0x8368D00); fog on 0x6D36FC; colour 0x84AA3A0 (rgb).
   - DiscardTask holes: *(0x834BFA8) = {count, ?, tasks}, rectangle +0x148 xmin, +0x14C xmax, +0x154 ymin, +0x150 ymax. */
#include "dx11_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRAW_ADDR 0x609610u
#define DRAW_RESUME 0x609616u
static const BYTE kDrawPrologue[6] = {0x81, 0xEC, 0x74, 0x01, 0x00, 0x00}; /* sub esp, 174h */

#define G_DEVICE 0x8368744u
#define G_CTX 0x834B094u
#define G_TERRAIN_ON 0x6D385Cu
#define G_WIREFRAME 0x836DAD8u
#define G_FOG_ON 0x6D36FCu
#define G_ZENABLE 0x853EE38u
#define G_FOG_COLOR 0x84AA3A0u
#define G_FOG_PLANE 0x834B068u
extern volatile int t_fog_exp2;    /* [TerrainFog] Exp2Levels, set per level by hud/zonefog.inc; the CPU path squares in terrain_abi.S */
#define G_DENSITY 0x8368D00u
#define G_DISCARD 0x834BFA8u
#define F_SETUPVIEW 0x607EA0u
#define F_PASSSTATES 0x606570u

#define MAX_MAPS 10
#define NODE 16                               /* cells a node side */
#define NODE_INDICES (NODE * NODE * 6)
#define HALF_INDICES (NODE * NODE / 4 * 6)    /* a quarter node (8 x 8 cells) */
#define MAX_LEVELS 14
#define MAX_INSTANCES (2 * 8192 * MAX_MAPS)   /* every map's two node lists full (SEL_CAP) */

void *nx_terrain_tramp;            /* the original draw: its first instruction, then a jump back */
extern void NxTerrain_Entry(void);
extern IDirect3DBaseTexture8 *__cdecl Unified_TlSubstitute(IDirect3DBaseTexture8 *t);   /* HD terrain light (hud) */
static BYTE g_jump[5];
static BOOL g_hooked, g_dump, g_gpu, g_logged_fallback;
static unsigned g_calls;
static BOOL g_eye_set;             /* dev: nx_terrain_eye.txt, a fixed eye (ctx + 4) for hidden tests */
static BOOL g_info_due;            /* dev: log what the terrain draw sees once after new maps */
static BOOL g_lod_off;             /* dev: nx_terrain_nolod.on, material weights at full detail (sharp edges) */
static int g_debug_view;           /* dev: nx_terrain_debug.txt 1 materials, 2 layer colour, 3 fog, 4 lightmap, 5/6 tex 0/1 */
static float g_eye[3];
int nx_dev_terrain_only, nx_in_terrain;
float nx_last_eye[3], nx_last_fwd[3];

__asm__(".intel_syntax noprefix\n"
        ".globl _NxTerrain_Entry\n"
        "_NxTerrain_Entry:\n"
        "    push dword ptr [esp+12]\n"
        "    push dword ptr [esp+12]\n"
        "    push dword ptr [esp+12]\n"
        "    call _NxTerrain_Draw\n"
        "    add esp, 12\n"
        "    test eax, eax\n"
        "    jz 1f\n"
        "    ret\n"
        "1:  jmp dword ptr [_nx_terrain_tramp]\n"
        /* TerrainRender_SetupView(ebx = ctx, eye): cdecl with ebx */
        ".globl _NxTerrain_SetupView\n"
        "_NxTerrain_SetupView:\n"
        "    push ebx\n"
        "    mov ebx, [esp+8]\n"
        "    push dword ptr [esp+12]\n"
        "    mov eax, 0x607EA0\n"
        "    call eax\n"
        "    add esp, 4\n"
        "    pop ebx\n"
        "    ret\n"
        /* TerrainRender_SetPassStates(edx = pass) */
        ".globl _NxTerrain_PassStates\n"
        "_NxTerrain_PassStates:\n"
        "    push ebx\n"
        "    push esi\n"
        "    push edi\n"
        "    mov edx, [esp+16]\n"
        "    mov eax, 0x606570\n"
        "    call eax\n"
        "    pop edi\n"
        "    pop esi\n"
        "    pop ebx\n"
        "    ret\n"
        ".att_syntax prefix\n");
extern void __cdecl NxTerrain_SetupView(BYTE *ctx, const float *eye);
extern void __cdecl NxTerrain_PassStates(int pass);

typedef int(__cdecl *DrawFn)(void *, void *, void *);

#define I32(p, o) (*(int32_t *)((BYTE *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((BYTE *)(p) + (o)))
#define F32(p, o) (*(float *)((BYTE *)(p) + (o)))
#define PTR(p, o) (*(BYTE **)((BYTE *)(p) + (o)))

/* ---- what the card keeps: per map its heights and material bytes ---- */
typedef struct TMap {
    const void *heights, *mats;    /* the game's arrays these were made from */
    int w, h;
    DWORD sum;                     /* a few samples, to notice new data at the same address */
    ID3D11Texture2D *ht, *mt;
    ID3D11ShaderResourceView *hsrv, *msrv;
    float *zr[MAX_LEVELS];         /* per level, per node: lowest and highest height */
    int nw[MAX_LEVELS], nh[MAX_LEVELS], levels;   /* nodes per level; the last level is one node */
} TMap;

typedef struct TerrainCB {
    float c[4][4], c14[4], fix[4], fogp[4], foghalf[4], misc[4], grid[4], size[4], pgrid[4], psize[4], region[4];
    float later[MAX_MAPS][4], lm[4], layer[16][4], holes[64][4], lod[4], cam[4];
} TerrainCB;

static struct {
    NxDevice *dev;
    ID3D11Device *d;               /* our reference: the resources below belong to it */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *ib, *inst, *cb;
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *depth[2];
    ID3D11RasterizerState *raster;
    ID3D11RasterizerState *raster_scan;   /* dev: the hole scan's (no depth clip) */
    ID3D11Texture2D *white_tex;
    ID3D11ShaderResourceView *white;   /* a layer texture the game does not have: white (D3D8's untextured stage), so
                                          t0 * t1 = t0. Was the device's white_srv, which is never made (NULL reads black): a
                                          layer with one texture drew black (level 13 hillsides) */
    TMap map[MAX_MAPS];
    BOOL failed;
} T;

static void ReleaseMap(TMap *m)
{
    NxC_Release(T.dev, m->hsrv);   /* through the render thread when there is one */
    NxC_Release(T.dev, m->msrv);
    NxC_Release(T.dev, m->ht);
    NxC_Release(T.dev, m->mt);
    int i;
    for (i = 0; i < MAX_LEVELS; ++i) free(m->zr[i]);
    memset(m, 0, sizeof *m);
}

static void ReleaseAll(void)
{
    unsigned i;
    for (i = 0; i < MAX_MAPS; ++i) ReleaseMap(&T.map[i]);
#define REL(x, kind) if (x) { kind##_Release(x); x = NULL; }
    REL(T.vs, ID3D11VertexShader) REL(T.ps, ID3D11PixelShader) REL(T.layout, ID3D11InputLayout)
    REL(T.ib, ID3D11Buffer) REL(T.inst, ID3D11Buffer) REL(T.cb, ID3D11Buffer) REL(T.blend, ID3D11BlendState)
    REL(T.depth[0], ID3D11DepthStencilState) REL(T.depth[1], ID3D11DepthStencilState) REL(T.raster, ID3D11RasterizerState)
    REL(T.raster_scan, ID3D11RasterizerState)
    REL(T.white, ID3D11ShaderResourceView) REL(T.white_tex, ID3D11Texture2D)
    REL(T.d, ID3D11Device)
#undef REL
    T.dev = NULL;
    T.failed = FALSE;
}

void NxTerrain_DeviceGone(NxDevice *dev)
{
    if (T.dev == dev) ReleaseAll();
}

/* shaders, index buffer, states: once per device */
static BOOL Setup(NxDevice *dev)
{
    ID3D11Device *d = dev->d;
    const unsigned char *code;
    SIZE_T n;
    D3D11_INPUT_ELEMENT_DESC el = {"CHUNK", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1};
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_BLEND_DESC blend;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rs;
    WORD *idx;
    unsigned x, y, k = 0;
    HRESULT hr;
    if (T.dev == dev && T.d == d) return !T.failed;
    if (T.dev) ReleaseAll();
    T.dev = dev;
    T.d = d;
    ID3D11Device_AddRef(d);
    code = Nx_TerrainCode(FALSE, &n);
    hr = ID3D11Device_CreateVertexShader(d, code, n, NULL, &T.vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(d, &el, 1, code, n, &T.layout);
    code = Nx_TerrainCode(TRUE, &n);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(d, code, n, NULL, &T.ps);
    /* a node: 16 x 16 cells, then a quarter node of 8 x 8; two triangles a cell, every cell split from (x + 1, y) to
       (x, y + 1) as the game's ground height is (Terrain_GetZFromWorldCoordinates; Linux DS 0x815470a): what players,
       trees and objects stand on. The alternating diagonal drawn before left half the cells below that surface on bumps
       (user: "floating ~5 cm"). With one direction a level's odd vertices sliding onto the even ones give exactly the
       next level's cells. */
    idx = (WORD *)malloc((NODE_INDICES + HALF_INDICES) * sizeof(WORD));
    if (!idx) hr = E_OUTOFMEMORY;
    {
        unsigned g;
        for (g = NODE; idx && g >= NODE / 2; g /= 2)
            for (y = 0; y < g; ++y)
                for (x = 0; x < g; ++x) {
                    WORD a = (WORD)(y * (g + 1) + x), b = (WORD)(a + 1), c = (WORD)(a + g + 1), e = (WORD)(c + 1);
                    idx[k++] = a; idx[k++] = c; idx[k++] = b; idx[k++] = b; idx[k++] = c; idx[k++] = e;
                }
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (NODE_INDICES + HALF_INDICES) * sizeof(WORD);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = idx; init.SysMemPitch = 0; init.SysMemSlicePitch = 0;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(d, &bd, &init, &T.ib);
    free(idx);
    bd.ByteWidth = MAX_INSTANCES * 8;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(d, &bd, NULL, &T.inst);
    bd.ByteWidth = sizeof(TerrainCB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(d, &bd, NULL, &T.cb);
    memset(&blend, 0, sizeof blend);
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBlendState(d, &blend, &T.blend);
    memset(&ds, 0, sizeof ds);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilState(d, &ds, &T.depth[0]);
    ds.DepthEnable = TRUE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilState(d, &ds, &T.depth[1]);
    memset(&rs, 0, sizeof rs);
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;    /* the game culls D3DCULL_CW; our triangles wind the other way */
    rs.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(d, &rs, &T.raster);
    /* dev, the hole scan: no near or far clipping (the ground past the far plane is drawn at the far depth, the ground
       before the near plane at the near one), so nothing vanishes there; culling as in play */
    rs.DepthClipEnable = FALSE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(d, &rs, &T.raster_scan);
    if (SUCCEEDED(hr)) {
        static const DWORD white = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = &white; init.SysMemPitch = 4; init.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateTexture2D(d, &td, &init, &T.white_tex);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource *)T.white_tex, NULL, &T.white);
    }
    if (FAILED(hr)) {
        Nx_Log("terrain on the GPU unavailable (%08lx): the game's own terrain", (unsigned long)hr);
        T.failed = TRUE;
        return FALSE;
    }
    Nx_Log("terrain on the GPU ready");
    return TRUE;
}

static DWORD Checksum(const float *h, int w, int hh)
{
    DWORD s = (DWORD)w * 31u + (DWORD)hh;
    int i, n = w * hh;
    for (i = 0; i < 64; ++i) {
        DWORD v;
        memcpy(&v, &h[(int)((long long)n * i / 64)], 4);
        s = s * 16777619u ^ v;
    }
    return s;
}

static void ForgetReadable(void);

/* the map's heights and materials on the card (again when the game's arrays changed) */
static BOOL UploadMap(TMap *m, const float *heights, const BYTE *mats, int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    DWORD sum = Checksum(heights, w, h);
    int cx, cy, x, y, L;
    HRESULT hr;
    float *clean = NULL;
    const float *orig = heights;   /* the game's array: what the next frame compares with */
    if (m->heights == heights && m->mats == mats && m->w == w && m->h == h && m->sum == sum && m->hsrv) return TRUE;
    ReleaseMap(m);
    ForgetReadable();
    {
        /* a height that is not a number (a broken export) would take its triangles with it: such samples get the last
           good height before them (the card's copy and the node ranges only; the game's own array is left alone) */
        size_t i, n = (size_t)w * h, bad = 0;
        for (i = 0; i < n; ++i) if (!isfinite(heights[i]) || fabsf(heights[i]) > 1e12f) ++bad;
        if (bad && (clean = (float *)malloc(n * sizeof(float))) != NULL) {
            float last = 0;
            for (i = 0; i < n; ++i) {
                float v = heights[i];
                if (!isfinite(v) || fabsf(v) > 1e12f) v = last;
                clean[i] = last = v;
            }
            Renderer_Log("terrain: a height map has samples that are not numbers; drawn with the height before them");
            heights = clean;
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    init.pSysMem = heights; init.SysMemPitch = (UINT)w * 4; init.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(T.d, &td, &init, &m->ht);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(T.d, (ID3D11Resource *)m->ht, NULL, &m->hsrv);
    td.Format = DXGI_FORMAT_R8_UINT;
    init.pSysMem = mats; init.SysMemPitch = (UINT)w;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(T.d, &td, &init, &m->mt);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(T.d, (ID3D11Resource *)m->mt, NULL, &m->msrv);
    /* height range per node, level 0 from the samples (0..w, the last extrapolated: the range is widened a little for
       it), each next level from four nodes of the one below, up to a single node */
    if (FAILED(hr)) { ReleaseMap(m); free(clean); return FALSE; }
    m->nw[0] = (w + NODE - 1) / NODE;
    m->nh[0] = (h + NODE - 1) / NODE;
    for (L = 0; L < MAX_LEVELS; ++L) {
        if (L) { m->nw[L] = (m->nw[L - 1] + 1) / 2; m->nh[L] = (m->nh[L - 1] + 1) / 2; }
        m->zr[L] = (float *)malloc(sizeof(float) * 2 * (size_t)m->nw[L] * (size_t)m->nh[L]);
        if (!m->zr[L]) { ReleaseMap(m); free(clean); return FALSE; }
        for (cy = 0; cy < m->nh[L]; ++cy)
            for (cx = 0; cx < m->nw[L]; ++cx) {
                float lo = 1e30f, hi = -1e30f, *z = m->zr[L] + 2 * (cy * m->nw[L] + cx);
                if (!L) {
                    for (y = cy * NODE; y <= cy * NODE + NODE && y < h; ++y)
                        for (x = cx * NODE; x <= cx * NODE + NODE && x < w; ++x) {
                            float v = heights[y * w + x];
                            if (v < lo) lo = v;
                            if (v > hi) hi = v;
                        }
                    z[0] = lo - (hi - lo) - 4096.0f;
                    z[1] = hi + (hi - lo) + 4096.0f;
                    continue;
                }
                for (y = 2 * cy; y <= 2 * cy + 1 && y < m->nh[L - 1]; ++y)
                    for (x = 2 * cx; x <= 2 * cx + 1 && x < m->nw[L - 1]; ++x) {
                        const float *c = m->zr[L - 1] + 2 * (y * m->nw[L - 1] + x);
                        if (c[0] < lo) lo = c[0];
                        if (c[1] > hi) hi = c[1];
                    }
                z[0] = lo; z[1] = hi;
            }
        m->levels = L + 1;
        if (m->nw[L] == 1 && m->nh[L] == 1) break;
    }
    m->heights = orig; m->mats = mats; m->w = w; m->h = h; m->sum = sum;
    free(clean);
    Nx_Log("terrain map %dx%d on the card", w, h);
    {
        /* dev: which material bytes the map uses */
        int hist[256], i, n = 0;
        char line[1024];
        memset(hist, 0, sizeof hist);
        for (i = 0; i < w * h; ++i) ++hist[mats[i]];
        line[0] = 0;
        for (i = 0; i < 256 && n < 900; ++i)
            if (hist[i]) n += snprintf(line + n, sizeof line - (size_t)n, " %d:%d", i, hist[i]);
        Nx_Log("terrain map materials%s", line);
        g_info_due = TRUE;
    }
    return TRUE;
}


/* the whole range is committed, readable memory (the context is on the caller's stack: only real maps are used).
   VirtualQuery is a system call: ranges found readable are remembered (the game's pointers stay the same between frames;
   the list is cleared when the maps change) */
static struct { const void *p; SIZE_T n; } g_ok[64];
static unsigned g_ok_next;
static BOOL ReadableNow(const void *p, SIZE_T n);
static BOOL Readable(const void *p, SIZE_T n)
{
    unsigned i;
    for (i = 0; i < 64; ++i) if (g_ok[i].p == p && g_ok[i].n >= n) return TRUE;
    if (!ReadableNow(p, n)) return FALSE;
    g_ok[g_ok_next].p = p; g_ok[g_ok_next].n = n;
    g_ok_next = (g_ok_next + 1) & 63;
    return TRUE;
}
static void ForgetReadable(void) { memset(g_ok, 0, sizeof g_ok); g_ok_next = 0; }

static BOOL ReadableNow(const void *p, SIZE_T n)
{
    const BYTE *at = (const BYTE *)p, *end = at + n;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            return FALSE;
        at = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return TRUE;
}

static ID3D11ShaderResourceView *TexSRV(NxDevice *dev, void *t)
{
    (void)dev;
    if (t && Readable(t, sizeof(NxTexture)) && Nx_IsTexture(t) && ((NxTexture *)t)->srv) return ((NxTexture *)t)->srv;
    return T.white;
}

typedef struct MapInfo {
    BOOL used;
    float x0, y0, cx, cy;          /* sample (0, 0), cell */
    float region[4];
    int parent;
} MapInfo;

/* a point of the chunk box in clip space: the game's position math (m3x4 r1, v0, c0; add oPos, r1, c14) */
static void Clip(const TerrainCB *cb, float x, float y, float z, float out[4])
{
    unsigned i;
    for (i = 0; i < 4; ++i) out[i] = x * cb->c[i][0] + y * cb->c[i][1] + z * cb->c[i][2] + cb->c14[i];
}

static BOOL ChunkVisible(const TerrainCB *cb, float x0, float y0, float x1, float y1, float z0, float z1)
{
    unsigned i, out[6] = {0, 0, 0, 0, 0, 0};
    for (i = 0; i < 8; ++i) {
        float p[4];
        Clip(cb, i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0, p);
        out[0] += p[0] < -p[3]; out[1] += p[0] > p[3];
        out[2] += p[1] < -p[3]; out[3] += p[1] > p[3];
        out[4] += p[2] < 0; out[5] += p[2] > p[3];
    }
    for (i = 0; i < 6; ++i) if (out[i] == 8 && !(nx_scan_active && i >= 4)) return FALSE;   /* the hole scan: no near/far */
    return TRUE;
}

/* ---- levels of detail (CDLOD). A node of level L is NODE x NODE cells with a vertex every 2^L samples. Level L is drawn
   out to range(L) = R0 * 2^L from the camera; between 0.85 range(L) (0.7 at level 0) and range(L) its odd vertices slide
   onto the even ones, which is level L + 1 (nx_terrain.hlsl), so neighbouring levels meet without cracks or popping.
   A node is split while it reaches into the range of the level below; children out of that range are drawn as quarter
   nodes of the parent's level. R0 follows the game's own detail factor K (ctx + 36; TerrainRender_Draw splits an edge of
   length E when E > K d) with four times the game's detail, and is at least 48 cells: a node must fit in its morph zone.
   The game's material weights keep their own level of detail (MaterialWeights in the shader). ---- */
typedef struct LodSel {
    const TerrainCB *cb;
    const TMap *m;
    const MapInfo *mi;
    const float *later;            /* regions of later (finer) maps: nodes inside one are not drawn */
    int nlater;
    float range[MAX_LEVELS];
    UINT *full, *half;
    int nfull, nhalf;
} LodSel;

#define SEL_CAP 8192
static UINT g_inst_data[2 * MAX_INSTANCES];   /* this frame's node list, sent to the card in one write */
static UINT g_sel_full[2 * SEL_CAP], g_sel_half[2 * SEL_CAP];
static BOOL g_full_detail;         /* dev: nx_terrain_full.on, every node at full detail */
static float g_dev_zoom = 1;       /* dev: nx_terrain_zoom.txt, the terrain magnified around the middle (binocular-like test shots) */

static void NodeBox(const LodSel *s, int L, int nx, int ny, float b[6])
{
    float size = (float)(NODE << L);
    const float *z = s->m->zr[L] + 2 * (ny * s->m->nw[L] + nx);
    b[0] = s->mi->x0 + (float)nx * size * s->mi->cx; b[1] = b[0] + size * s->mi->cx;
    b[2] = s->mi->y0 + (float)ny * size * s->mi->cy; b[3] = b[2] + size * s->mi->cy;
    b[4] = z[0]; b[5] = z[1];
}

static float BoxDistance(const float b[6], const float *cam)
{
    float dx = cam[0] < b[0] ? b[0] - cam[0] : cam[0] > b[1] ? cam[0] - b[1] : 0.0f;
    float dy = cam[1] < b[2] ? b[2] - cam[1] : cam[1] > b[3] ? cam[1] - b[3] : 0.0f;
    float dz = cam[2] < b[4] ? b[4] - cam[2] : cam[2] > b[5] ? cam[2] - b[5] : 0.0f;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static BOOL BoxDrawn(const LodSel *s, const float b[6])
{
    int i;
    for (i = 0; i < s->nlater; ++i) {
        const float *r = s->later + 4 * i;
        if (b[0] >= r[0] && b[1] <= r[1] && b[2] >= r[2] && b[3] <= r[3]) return FALSE;
    }
    return ChunkVisible(s->cb, b[0], b[2], b[1], b[3], b[4], b[5]);
}

static void Emit(UINT *list, int *n, int x, int y, int L, int half)
{
    if (*n >= SEL_CAP) return;
    list[2 * *n] = (UINT)x | ((UINT)L << 16);
    list[2 * *n + 1] = (UINT)y | ((UINT)half << 16);
    ++*n;
}

/* FALSE: the node is out of its level's range (its parent draws that part) */
static BOOL Select(LodSel *s, int L, int nx, int ny)
{
    float b[6], cd[4];
    int i, j, order[4];
    NodeBox(s, L, nx, ny, b);
    {
        float dist = BoxDistance(b, s->cb->cam);
        if (L < s->m->levels - 1 && dist > s->range[L]) return FALSE;
        if (!BoxDrawn(s, b)) return TRUE;
        if (!L || dist > s->range[L - 1]) { Emit(s->full, &s->nfull, nx * (NODE << L), ny * (NODE << L), L, 0); return TRUE; }
    }
    /* a node list nearly full (a huge map with full detail far out): this node is drawn whole, coarser, rather than
       its children dropped, which would leave a hole */
    if (s->nfull + 4 > SEL_CAP || s->nhalf + 4 > SEL_CAP) {
        static BOOL said;
        if (!said) { said = TRUE; Renderer_Log("terrain: a node list is full; some ground is drawn coarser"); }
        Emit(s->full, &s->nfull, nx * (NODE << L), ny * (NODE << L), L, 0);
        return TRUE;
    }
    /* children, the nearest first (front to back helps the depth test) */
    for (i = 0; i < 4; ++i) {
        int cx = 2 * nx + (i & 1), cy = 2 * ny + (i >> 1);
        float c[6];
        order[i] = i;
        cd[i] = -1.0f;
        if (cx >= s->m->nw[L - 1] || cy >= s->m->nh[L - 1]) continue;
        NodeBox(s, L - 1, cx, cy, c);
        cd[i] = BoxDistance(c, s->cb->cam);
    }
    for (i = 1; i < 4; ++i)
        for (j = i; j > 0 && cd[order[j]] < cd[order[j - 1]]; --j) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (i = 0; i < 4; ++i) {
        int c = order[i], cx = 2 * nx + (c & 1), cy = 2 * ny + (c >> 1);
        float cb6[6];
        if (cd[c] < 0) continue;
        if (Select(s, L - 1, cx, cy)) continue;
        NodeBox(s, L - 1, cx, cy, cb6);
        if (BoxDrawn(s, cb6)) Emit(s->half, &s->nhalf, cx * (NODE << (L - 1)), cy * (NODE << (L - 1)), L, 1);
    }
    return TRUE;
}

#include "dx11_terrain_scan.inc"   /* dev: nx_terrain_scan.txt, the hole scan */

/* TRUE when map m lies in map k (k is its parent, or its parent's parent ...). Only such a map takes k's place where it
   lies. Two maps under the same outer map can overlap side by side (level 32: maps 1 and 2 share a strip 32 cells wide);
   each one's border follows the outer map's surface, not its neighbour's, so cutting map 1 away along map 2's border
   left a crack there (user, 2026-11-15, steep hillside). The game's own terrain shows both maps in that strip. */
static BOOL InsideMap(const MapInfo *mi, int m, int k)
{
    int p, guard;
    for (p = mi[m].parent, guard = 0; p >= 0 && guard < MAX_MAPS; p = mi[p].parent, ++guard)
        if (p == k) return TRUE;
    return FALSE;
}

/* draws the terrain of ctx with the D3D11 device; FALSE: nothing drawn (the game's own path runs) */
static BOOL DrawGPU(NxDevice *dev, BYTE *ctx)
{
    TerrainCB cb;
    MapInfo mi[MAX_MAPS];
    ID3D11ShaderResourceView *srv[33], *lsrv, *vsrv[3], *none[48];
    ID3D11SamplerState *smp[3];
    ID3D11Buffer *cbs[1];
    UINT stride = 8, offset = 0, first[MAX_MAPS], count[MAX_MAPS], hfirst[MAX_MAPS], hcount[MAX_MAPS], total = 0;
    float r0[MAX_MAPS];
    int maxlevel[MAX_MAPS];
    BYTE *list;
    int k, b, w0, h0, maxdim, shift0, nmaps = 0, maxmaps;
    float sizex, sizey, cx0, cy0, ux, uy, xl, yl;
    DWORD saved_u, saved_v, saved_u1, saved_v1, used;
    BOOL lightmap = !I32(ctx, 0xE4);
    D3D11_VIEWPORT vp;

    memset(&cb, 0, sizeof cb);
    memset(mi, 0, sizeof mi);
    /* maps */
    w0 = I32(ctx, 1436); h0 = I32(ctx, 1440);
    shift0 = I32(ctx, 1316);
    sizex = F32(ctx, 236); sizey = F32(ctx, 276);
    if (w0 < 2 || h0 < 2 || w0 > 8192 || h0 > 8192 || shift0 < 0 || shift0 > 16) return FALSE;
    maxdim = w0 > h0 ? w0 : h0;
    cx0 = sizex / (float)w0;
    cy0 = sizey / (float)h0;
    xl = -((float)maxdim * cx0) * 0.5f + F32(ctx, 1516) + cx0 * 0.5f;
    yl = -((float)maxdim * cy0) * 0.5f + F32(ctx, 1520) + cy0 * 0.5f;
    ux = cx0 / (float)(1 << shift0);
    uy = cy0 / (float)(1 << shift0);
    {
        /* the number of maps: their lightmaps (+0x664), when it is a sensible count */
        int n = I32(ctx, 0x664);
        if (n < 1 || n > MAX_MAPS) n = MAX_MAPS;
        maxmaps = n;
    }
    for (k = 0; k < maxmaps; ++k) {
        int w = I32(ctx, 1436 + 8 * k), h = I32(ctx, 1440 + 8 * k), sh = I32(ctx, 1316 + 4 * k);
        const float *hp = (const float *)PTR(ctx, 316 + 4 * k);
        const BYTE *mp = PTR(ctx, 1396 + 4 * k);
        if (w < 2 || h < 2 || w > 8192 || h > 8192 || !hp || !mp || sh < 0 || sh > 16) break;
        if (!Readable(hp, (SIZE_T)w * (SIZE_T)h * 4) || !Readable(mp, (SIZE_T)w * (SIZE_T)h)) break;
        if (!UploadMap(&T.map[k], hp, mp, w, h)) return FALSE;
        mi[k].used = TRUE;
        mi[k].cx = ux * (float)(1 << sh);
        mi[k].cy = uy * (float)(1 << sh);
        mi[k].x0 = xl + (float)I32(ctx, 1236 + 8 * k) * ux;
        mi[k].y0 = yl + (float)I32(ctx, 1240 + 8 * k) * uy;
        mi[k].region[0] = mi[k].x0; mi[k].region[1] = mi[k].x0 + (float)w * mi[k].cx;
        mi[k].region[2] = mi[k].y0; mi[k].region[3] = mi[k].y0 + (float)h * mi[k].cy;
        mi[k].parent = -1;
        nmaps = k + 1;
    }
    if (!nmaps) return FALSE;
    for (k = 1; k < nmaps; ++k) {
        /* the map it lies in: the last earlier one containing its centre */
        float px = (mi[k].region[0] + mi[k].region[1]) * 0.5f, py = (mi[k].region[2] + mi[k].region[3]) * 0.5f;
        int m;
        for (m = k - 1; m >= 0; --m)
            if (px > mi[m].region[0] && px < mi[m].region[1] && py > mi[m].region[2] && py < mi[m].region[3]) { mi[k].parent = m; break; }
    }
    NxScan_Maps(mi, nmaps);   /* dev: the hole scan (nx_terrain_scan.txt) */
    /* constants: the game's terrain vertex shader constants as TerrainRender_SetupView left them */
    memcpy(cb.c, dev->s.vsc[0], sizeof cb.c);
    memcpy(cb.c14, dev->s.vsc[14], sizeof cb.c14);
    cb.fix[0] = 1 / (dev->s.viewport.Width ? (float)dev->s.viewport.Width : 1);
    cb.fix[1] = -1 / (dev->s.viewport.Height ? (float)dev->s.viewport.Height : 1);
    cb.fix[2] = g_dev_zoom;
    {
        const float *plane = (const float *)G_FOG_PLANE, *fogc = (const float *)G_FOG_COLOR;
        float k2 = F32(G_DENSITY, 0) * 1.442695040888963f;
        for (b = 0; b < 4; ++b) cb.fogp[b] = plane[b] * k2;
        /* the layer passes' fog colour: round(127 * colour) / 255 (TerrainRender_SetPassStates, passes 1 and 2) */
        for (b = 0; b < 3; ++b) cb.foghalf[b] = (float)(int)(fogc[b] * 127.0f + 0.5f) / 255.0f;
        cb.foghalf[3] = I32(G_FOG_ON, 0) ? 1.0f : 0.0f;
        /* [TerrainFog] Exp2Levels (Zarya-9, 58): the squared exponent, as the CPU path's terrain_abi.S does before e^-x */
        cb.fix[3] = t_fog_exp2 ? 1.0f : 0.0f;
    }
    cb.misc[0] = lightmap ? 1.0f : 0.0f;
    cb.lod[3] = (float)g_debug_view;
    /* the game's level of detail for the material weights (nx_terrain.hlsl MaterialWeights): split factor ctx + 36,
       height error factor ctx + 28, camera = minus the terrain eye (ctx + 4) */
    cb.lod[0] = F32(ctx, 36);
    if (nx_detail_distance > 1.0f) cb.lod[0] /= nx_detail_distance;   /* [Renderer] DetailDistance: finer material edges further */
    cb.lod[1] = F32(ctx, 28) * 0.5f;
    cb.lod[2] = cb.lod[0] > 0 && cb.lod[0] < 1e6f && cb.lod[1] >= 0 && cb.lod[1] < 1e9f && !g_lod_off ? 1.0f : 0.0f;
    cb.cam[0] = -F32(ctx, 4); cb.cam[1] = -F32(ctx, 8); cb.cam[2] = -F32(ctx, 12);
    {
        float s = F32(0x853EFF0u, 0);
        nx_last_eye[0] = cb.cam[0] * s; nx_last_eye[1] = cb.cam[1] * s; nx_last_eye[2] = cb.cam[2] * s;
        nx_last_fwd[0] = dev->s.view._13; nx_last_fwd[1] = dev->s.view._23; nx_last_fwd[2] = dev->s.view._33;
    }
    list = PTR(G_DISCARD, 0);
    if (list) {
        int n = I32(list, 0), i, m = 0;
        for (i = 0; i < n && m < 64; ++i) {
            BYTE *t = PTR(list, 8 + 4 * i);
            if (!t) continue;
            cb.holes[m][0] = F32(t, 0x148); cb.holes[m][1] = F32(t, 0x14C);
            cb.holes[m][2] = F32(t, 0x154); cb.holes[m][3] = F32(t, 0x150);
            ++m;
        }
        cb.misc[1] = nx_scan_active ? 0.0f : (float)m;   /* the hole scan: tunnels and shafts closed */
    }
    /* layers: per block its textures and planar scales. A block is a material byte: the game keeps the layer of
       material m at ctx + 0x30 + 4 m and lists the materials in use at ctx + 0x74 (+0x70 of them), so material ids can
       be above the count (level 17: count 5, materials 0, 1, 2, 4, 5); TerrainRender 0x6063B0 / 0x606DC0 */
    {
        int i, n = I32(ctx, 0x70);
        used = 0;
        for (i = 0; i < n && i < 16; ++i) if (ctx[0x74 + i] < 16) used |= 1u << ctx[0x74 + i];
    }
    for (b = 0; b < 16; ++b) {
        BYTE *l = (used >> b) & 1 ? PTR(ctx, 0x30 + 4 * b) : NULL;
        srv[b] = srv[16 + b] = T.white;
        if (!l || !Readable(l, 0x4B0)) continue;
        cb.layer[b][0] = 1.0f / (F32(l, 0x260) * 4096.0f);
        cb.layer[b][1] = 1.0f / (F32(l, 0x494) * 4096.0f);
        cb.layer[b][2] = (float)I32(l, 0x4AC);
        cb.layer[b][3] = 1.0f;
        srv[b] = TexSRV(dev, I32(l, 0x30) ? PTR(l, 0x58) : NULL);
        srv[16 + b] = TexSRV(dev, I32(l, 0x264) ? PTR(l, 0x28C) : NULL);
    }
    if (g_info_due) {
        g_info_due = FALSE;
        Nx_Log("terrain info: layers %d, no-lightmap %d, maps %d (0x664 %d), single %d, fog on %d, foghalf %g %g %g, "
               "lod k %g error %g, camera %g %g %g", I32(ctx, 0x70), I32(ctx, 0xE4), nmaps, I32(ctx, 0x664), I32(ctx, 0x668),
               I32(G_FOG_ON, 0), cb.foghalf[0], cb.foghalf[1], cb.foghalf[2], F32(ctx, 36), F32(ctx, 28),
               cb.cam[0], cb.cam[1], cb.cam[2]);
        Nx_Log("terrain info: bytes 0x74 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
               ctx[0x74], ctx[0x75], ctx[0x76], ctx[0x77], ctx[0x78], ctx[0x79], ctx[0x7A], ctx[0x7B], ctx[0x7C], ctx[0x7D],
               ctx[0x7E], ctx[0x7F], ctx[0x80], ctx[0x81], ctx[0x82], ctx[0x83]);
        Nx_Log("terrain info: bytes 0x84 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
               ctx[0x84], ctx[0x85], ctx[0x86], ctx[0x87], ctx[0x88], ctx[0x89], ctx[0x8A], ctx[0x8B], ctx[0x8C], ctx[0x8D],
               ctx[0x8E], ctx[0x8F], ctx[0x90], ctx[0x91], ctx[0x92], ctx[0x93]);
        for (k = 0; k < nmaps; ++k) {
            BYTE *holder = k < 4 ? PTR(ctx, 0x94 + 4 * k) : NULL;
            void *lt = holder && Readable(holder, 0x28) ? PTR(holder, 0x24) : NULL;
            {
                const float *hp = (const float *)T.map[k].heights;
                int i, n = T.map[k].w * T.map[k].h;
                float lo = 1e30f, hi = -1e30f;
                for (i = 0; i < n; ++i) { if (hp[i] < lo) lo = hp[i]; if (hp[i] > hi) hi = hp[i]; }
                Nx_Log("terrain info: map %d heights %g..%g, centre %g", k, lo, hi, hp[(T.map[k].h / 2) * T.map[k].w + T.map[k].w / 2]);
            }
            Nx_Log("terrain info: map %d %dx%d at %g %g cell %g %g parent %d, lightmap %p ours %d, lm %g %g %g %g", k,
                   T.map[k].w, T.map[k].h, mi[k].x0, mi[k].y0, mi[k].cx, mi[k].cy, mi[k].parent, lt,
                   lt ? (int)(TexSRV(dev, lt) != T.white) : -1,
                   F32(ctx, 0xA4 + 8 * k), F32(ctx, 0xA8 + 8 * k), F32(ctx, 0xC4 + 8 * k), F32(ctx, 0xC8 + 8 * k));
        }
        for (b = 0; b < 16; ++b) {
            BYTE *l = PTR(ctx, 0x30 + 4 * b);
            if (!((used >> b) & 1)) continue;
            if (!l || !Readable(l, 0x4B0)) { Nx_Log("terrain info: layer %d none (%p)", b, l); continue; }
            Nx_Log("terrain info: layer %d has %d/%d tex %p %p ours %d %d scale %g %g type %d",
                   b, I32(l, 0x30), I32(l, 0x264), PTR(l, 0x58), PTR(l, 0x28C), (int)(srv[b] != T.white),
                   (int)(srv[16 + b] != T.white), F32(l, 0x260), F32(l, 0x494), I32(l, 0x4AC));
        }
    }
    /* chunks in view, per map */
    for (k = 0; k < nmaps; ++k) {
        LodSel s;
        float later[MAX_MAPS][4], kf = F32(ctx, 36);
        UINT *out = g_inst_data;
        int i, L;
        memset(&s, 0, sizeof s);
        s.cb = &cb; s.m = &T.map[k]; s.mi = &mi[k];
        /* nodes wholly inside a finer map are not drawn; that map's area shrunk by the same 1 % of its cell as in the pixel
           test, so the ring of nodes along its border is drawn and the overlap there exists (on level 16 the border lies
           on node boundaries: without this the outer map had no triangles inside it) */
        for (i = k + 1; i < nmaps; ++i) {
            float e = 0.01f * mi[i].cx;
            if (!InsideMap(mi, i, k)) continue;   /* a map beside this one, not in it: both are drawn (InsideMap) */
            later[s.nlater][0] = mi[i].region[0] + e; later[s.nlater][1] = mi[i].region[1] - e;
            later[s.nlater][2] = mi[i].region[2] + e; later[s.nlater][3] = mi[i].region[3] - e;
            ++s.nlater;
        }
        s.later = &later[0][0];
        r0[k] = 48.0f * mi[k].cx;
        if (kf > 0 && kf < 1e6f && 8.0f * mi[k].cx / kf > r0[k]) r0[k] = 8.0f * mi[k].cx / kf;
        if (nx_detail_distance > 1.0f) {
            /* [Renderer] DetailDistance: full detail further out, at most 400 cells (a level-0 disc of about 2000 nodes,
               well inside SEL_CAP, so no node is ever dropped) */
            float wide = r0[k] * nx_detail_distance, cap = 400.0f * mi[k].cx;
            if (wide > cap) wide = cap;
            if (wide > r0[k]) r0[k] = wide;
        }
        {
            /* a finer map's border follows this map's full-detail surface: keep full detail out past every such border
               (its far corners, before the slide to level 1 starts at 0.7 R0), or the seam opens */
            const float *zt = T.map[k].zr[T.map[k].levels - 1];
            float dz = fabsf(cb.cam[2] - zt[0]) > fabsf(cb.cam[2] - zt[1]) ? fabsf(cb.cam[2] - zt[0]) : fabsf(cb.cam[2] - zt[1]);
            for (i = 0; i < s.nlater; ++i) {
                int c;
                for (c = 0; c < 4; ++c) {
                    float dx = later[i][c & 1] - cb.cam[0], dy = later[i][2 + (c >> 1)] - cb.cam[1];
                    float need = sqrtf(dx * dx + dy * dy + dz * dz) / 0.7f + 2.0f * NODE * mi[k].cx;
                    if (need > r0[k]) r0[k] = need;
                }
            }
        }
        if (g_full_detail) r0[k] = 1e30f;
        /* a map inside another: its border follows the outer map's surface, straight between that map's samples, so its
           grid never gets coarser than one cell of the outer map */
        maxlevel[k] = MAX_LEVELS;
        if (mi[k].parent >= 0)
            for (maxlevel[k] = 0; maxlevel[k] < MAX_LEVELS - 1 && (float)(2 << maxlevel[k]) * mi[k].cx <= mi[mi[k].parent].cx * 1.001f;)
                ++maxlevel[k];
        for (L = 0; L < MAX_LEVELS; ++L) s.range[L] = L >= maxlevel[k] ? 3e38f : r0[k] * (float)(1 << L);
        s.full = g_sel_full; s.half = g_sel_half;
        Select(&s, T.map[k].levels - 1, 0, 0);
        first[k] = total;
        for (i = 0; i < s.nfull && total < MAX_INSTANCES; ++i, ++total) { out[2 * total] = s.full[2 * i]; out[2 * total + 1] = s.full[2 * i + 1]; }
        count[k] = total - first[k];
        hfirst[k] = total;
        for (i = 0; i < s.nhalf && total < MAX_INSTANCES; ++i, ++total) { out[2 * total] = s.half[2 * i]; out[2 * total + 1] = s.half[2 * i + 1]; }
        hcount[k] = total - hfirst[k];
    }
    if (total) NxC_WriteBuffer(dev, T.inst, D3D11_MAP_WRITE_DISCARD, 0, g_inst_data, total * 8);

    /* samplers: the layer stages with the game's filters, wrapped as its layer passes set them (TerrainRender_SetPassStates
       0x606570 passes 1, 2: stage 0 ADDRESSU / V = WRAP); the address mode left from the last draw (the sky's clamp, or
       our own pass 3 below) made a layer read only its edge texel (level 13: a whole hillside black). The lightmap with
       stage 0's filter and clamped (pass 3). */
    saved_u = dev->s.tss[0][D3DTSS_ADDRESSU]; saved_v = dev->s.tss[0][D3DTSS_ADDRESSV];
    saved_u1 = dev->s.tss[1][D3DTSS_ADDRESSU]; saved_v1 = dev->s.tss[1][D3DTSS_ADDRESSV];
    dev->s.tss[0][D3DTSS_ADDRESSU] = dev->s.tss[0][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    dev->s.tss[1][D3DTSS_ADDRESSU] = dev->s.tss[1][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    smp[0] = Nx_StageSampler(dev, 0);
    smp[1] = Nx_StageSampler(dev, 1);
    dev->s.tss[1][D3DTSS_ADDRESSU] = saved_u1; dev->s.tss[1][D3DTSS_ADDRESSV] = saved_v1;
    dev->s.tss[0][D3DTSS_ADDRESSU] = dev->s.tss[0][D3DTSS_ADDRESSV] = D3DTADDRESS_CLAMP;
    smp[2] = Nx_StageSampler(dev, 0);
    dev->s.tss[0][D3DTSS_ADDRESSU] = saved_u; dev->s.tss[0][D3DTSS_ADDRESSV] = saved_v;

    /* pipeline */
    Nx_BindTargets(dev);
    vp.TopLeftX = (float)dev->s.viewport.X; vp.TopLeftY = (float)dev->s.viewport.Y;
    vp.Width = (float)dev->s.viewport.Width; vp.Height = (float)dev->s.viewport.Height;
    vp.MinDepth = dev->s.viewport.MinZ; vp.MaxDepth = dev->s.viewport.MaxZ;
    NxC_RSSetViewports(dev, 1, &vp);
    NxC_RSSetState(dev, nx_scan_active && T.raster_scan ? T.raster_scan : T.raster);
    NxC_OMSetBlendState(dev, T.blend, NULL, 0xFFFFFFFF);
    NxC_OMSetDepthStencilState(dev, T.depth[I32(G_ZENABLE, 0) ? 1 : 0], 0);
    NxC_IASetInputLayout(dev, T.layout);
    NxC_IASetPrimitiveTopology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NxC_IASetVertexBuffers(dev, 0, 1, &T.inst, &stride, &offset);
    NxC_IASetIndexBuffer(dev, T.ib, DXGI_FORMAT_R16_UINT, 0);
    NxC_VSSetShader(dev, T.vs, NULL, 0);
    NxC_PSSetShader(dev, T.ps, NULL, 0);
    cbs[0] = T.cb;
    NxC_VSSetConstantBuffers(dev, 6, 1, cbs);
    NxC_PSSetConstantBuffers(dev, 6, 1, cbs);
    NxC_PSSetShaderResources(dev, 16, 32, srv);
    NxC_PSSetSamplers(dev, 8, 3, smp);
    {
        /* the HD terrain light sees every lightmap first (it picks the largest), then hands out its own for that one */
        for (k = 0; k < nmaps && k < 4; ++k) {
            BYTE *holder = PTR(ctx, 0x94 + 4 * k);
            if (holder && Readable(holder, 0x28) && PTR(holder, 0x24)) Unified_TlSubstitute((IDirect3DBaseTexture8 *)PTR(holder, 0x24));
        }
    }
    for (k = 0; k < nmaps; ++k) {
        int m, n = 0;
        BYTE *holder = k < 4 ? PTR(ctx, 0x94 + 4 * k) : NULL;
        void *lt = holder && Readable(holder, 0x28) ? PTR(holder, 0x24) : NULL;
        if (!count[k] && !hcount[k]) continue;
        if (lt) lt = Unified_TlSubstitute((IDirect3DBaseTexture8 *)lt);
        cb.grid[0] = mi[k].x0; cb.grid[1] = mi[k].y0; cb.grid[2] = mi[k].cx; cb.grid[3] = mi[k].cy;
        cb.size[0] = (float)T.map[k].w; cb.size[1] = (float)T.map[k].h;
        {
            /* levels of detail: the root square has max(w, h) samples a side */
            int top = 0, mx = T.map[k].w > T.map[k].h ? T.map[k].w : T.map[k].h;
            while ((1 << top) < mx) ++top;
            cb.size[2] = (float)top;
        }
        cb.size[3] = r0[k];
        cb.misc[3] = (float)maxlevel[k];
        /* Where this map draws, and the finer maps it leaves to them, each with a small overlap (1 % of a cell): with the
           exact border both maps could reject a pixel on it (rounding of the interpolated position), and the sky showed
           through as a thin light line along the seam (user, level 16, binoculars). The surfaces meet at the border,
           so the overlap is not visible. */
        {
            float e = 0.01f * mi[k].cx;
            cb.region[0] = mi[k].region[0] - e; cb.region[1] = mi[k].region[1] + e;
            cb.region[2] = mi[k].region[2] - e; cb.region[3] = mi[k].region[3] + e;
        }
        memset(cb.pgrid, 0, sizeof cb.pgrid);
        memset(cb.psize, 0, sizeof cb.psize);
        vsrv[2] = NULL;
        if (mi[k].parent >= 0) {
            int p = mi[k].parent;
            cb.pgrid[0] = mi[p].x0; cb.pgrid[1] = mi[p].y0; cb.pgrid[2] = mi[p].cx; cb.pgrid[3] = mi[p].cy;
            cb.psize[0] = (float)T.map[p].w; cb.psize[1] = (float)T.map[p].h; cb.psize[2] = 1;
            vsrv[2] = T.map[p].hsrv;
        }
        for (m = k + 1; m < nmaps; ++m) {
            float e = 0.01f * mi[m].cx;
            if (!InsideMap(mi, m, k)) continue;   /* the same maps as the node skip above */
            cb.later[n][0] = mi[m].region[0] + e; cb.later[n][1] = mi[m].region[1] - e;
            cb.later[n][2] = mi[m].region[2] + e; cb.later[n][3] = mi[m].region[3] - e;
            ++n;
        }
        cb.misc[2] = (float)n;
        cb.lm[0] = F32(ctx, 0xA4 + 8 * k); cb.lm[1] = F32(ctx, 0xA8 + 8 * k);
        cb.lm[2] = F32(ctx, 0xC4 + 8 * k); cb.lm[3] = F32(ctx, 0xC8 + 8 * k);
        cb.misc[0] = lightmap && k < 4 && lt ? 1.0f : 0.0f;
        NxC_WriteBuffer(dev, T.cb, D3D11_MAP_WRITE_DISCARD, 0, &cb, sizeof cb);
        vsrv[0] = T.map[k].hsrv;
        vsrv[1] = T.map[k].msrv;
        NxC_VSSetShaderResources(dev, 0, 3, vsrv);
        NxC_PSSetShaderResources(dev, 0, 3, vsrv);   /* heights and materials for the weights */
        lsrv = TexSRV(dev, lt);
        NxC_PSSetShaderResources(dev, 48, 1, &lsrv);
        if (count[k]) { NxC_DrawIndexedInstanced(dev, NODE_INDICES, count[k], 0, 0, first[k]); ++nx_stats.draws; }
        if (hcount[k]) { NxC_DrawIndexedInstanced(dev, HALF_INDICES, hcount[k], NODE_INDICES, 0, hfirst[k]); ++nx_stats.draws; }
    }
    /* leave nothing of ours bound that the device does not rebind */
    memset(none, 0, sizeof none);
    NxC_VSSetShaderResources(dev, 0, 3, none);
    NxC_PSSetShaderResources(dev, 0, 3, none);
    NxC_PSSetShaderResources(dev, 16, 33, none);
    Nx_ForgetBindings(dev);
    return TRUE;
}

/* the render state methods TerrainRender_Draw calls first: holder (cache index at +4, methods from +20), wanted index */
static void StateMethod(uintptr_t holder_at, uintptr_t index_at)
{
    BYTE *h = PTR(holder_at, 0);
    int i = I32(index_at, 0);
    if (!h || I32(h, 4) == i) return;
    I32(h, 4) = i;
    ((void(__cdecl *)(int))(void *)PTR(h, 4 * i + 20))(0);
}

/* ---- dev dump (nx_terrain_dump.on) ---- */
static void ReadSafe(const void *addr, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    memset(out, 0, n);
    if (addr) ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got);
}

static void Put(FILE *f, const char *tag, const void *addr, SIZE_T n)
{
    BYTE *buf = (BYTE *)malloc(n ? n : 1);
    DWORD hdr[3];
    if (!buf) return;
    ReadSafe(addr, buf, n);
    memcpy(hdr, tag, 4);
    hdr[1] = (DWORD)(uintptr_t)addr;
    hdr[2] = (DWORD)n;
    fwrite(hdr, 4, 3, f);
    fwrite(buf, 1, n, f);
    free(buf);
}

static void Dump(BYTE *ctx)
{
    char path[MAX_PATH + 40];
    FILE *f;
    unsigned k;
    snprintf(path, sizeof path, "%s\\nx_terrain_dump.bin", Renderer_GameFolder());
    f = fopen(path, "wb");
    if (!f) return;
    Put(f, "CTX ", ctx, 0x800);
    Put(f, "ARG1", NULL, 16);
    Put(f, "ARG2", NULL, 16);
    Put(f, "VERT", (void *)0x7F09D68u, 30000 * 32);
    Put(f, "NVRT", (void *)0x7F09D0Cu, 4);
    Put(f, "G834", (void *)0x834B000u, 0x100);
    for (k = 0; k < MAX_MAPS; ++k) {
        DWORD w = 0, h = 0, hp = 0, mp = 0;
        ReadSafe(ctx + 1436 + 8 * k, &w, 4);
        ReadSafe(ctx + 1440 + 8 * k, &h, 4);
        ReadSafe(ctx + 316 + 4 * k, &hp, 4);
        ReadSafe(ctx + 1396 + 4 * k, &mp, 4);
        if (w < 2 || h < 2 || w > 4096 || h > 4096 || !hp) break;
        Put(f, "HGT ", (void *)(uintptr_t)hp, (SIZE_T)w * h * 4);
        Put(f, "MAT ", (void *)(uintptr_t)mp, (SIZE_T)w * h);
    }
    fclose(f);
    Renderer_Log("terrain dump written");
}

int __cdecl NxTerrain_Draw(BYTE *ctx, BYTE *a1, BYTE *a2)
{
    NxDevice *dev = (NxDevice *)PTR(G_DEVICE, 0);
    ++g_calls;
    if (g_dump && g_calls == 600) {
        ((DrawFn)nx_terrain_tramp)(ctx, a1, a2);
        Dump(ctx);
        return 1;
    }
    if (Nx_IsDevice(dev) && Nx_DevView() && ctx) {
        /* dev camera (nx_view.txt): the terrain's eye and the game's camera turn, as the game sets them in real play */
        const D3DMATRIX *v = Nx_DevView();
        float s = F32(0x853EFF0u, 0), *tc = (float *)0x8408FC0u;
        if (s != 0) { F32(ctx, 4) = -nx_dev_eye[0] / s; F32(ctx, 8) = -nx_dev_eye[1] / s; F32(ctx, 12) = -nx_dev_eye[2] / s; }
        tc[0] = v->_11; tc[1] = v->_21; tc[2] = v->_31;
        tc[3] = v->_12; tc[4] = v->_22; tc[5] = v->_32;
        tc[6] = v->_13; tc[7] = v->_23; tc[8] = v->_33;
        if (!g_gpu) {
            nx_in_terrain = 1;
            if (Nx_IsDevice(dev)) Nx_GpuTerrainMark(dev, TRUE);
            ((DrawFn)nx_terrain_tramp)(ctx, a1, a2);
            if (Nx_IsDevice(dev)) Nx_GpuTerrainMark(dev, FALSE);
            nx_in_terrain = 0;
            return 1;
        }
    }
    if ((g_eye_set & 1) && ctx) {
        static unsigned logged;
        F32(ctx, 4) = g_eye[0]; F32(ctx, 8) = g_eye[1]; F32(ctx, 12) = g_eye[2];
        if (!g_gpu) {
            ((DrawFn)nx_terrain_tramp)(ctx, a1, a2);
            if (logged < 2 && Nx_IsDevice(dev) && ++logged) {
                int i;
                for (i = 0; i < 16; ++i) if (i < 5 || i == 14)
                    Nx_Log("terrain eye orig c%d %g %g %g %g", i, dev->s.vsc[i][0], dev->s.vsc[i][1], dev->s.vsc[i][2], dev->s.vsc[i][3]);
                Nx_Log("terrain eye orig vert0 %g %g %g %g", F32(0x7F09D68u, 0), F32(0x7F09D68u, 4), F32(0x7F09D68u, 8), F32(0x7F09D68u, 12));
            }
            return 1;
        }
    }
    if (!g_gpu || !ctx || !I32(G_TERRAIN_ON, 0) || I32(G_WIREFRAME, 0) || I32(ctx, 0x668)) return 0;
    if (!Nx_IsDevice(dev) || !Setup(dev)) {
        if (!g_logged_fallback) { g_logged_fallback = TRUE; Renderer_Log("terrain: not our Direct3D 11 device, the game's own terrain"); }
        return 0;
    }
    /* what TerrainRender_Draw does before building its mesh */
    *(BYTE **)G_CTX = ctx;
    StateMethod(0x853EFE4u, 0x834B090u);
    StateMethod(0x853F014u, 0x814A03Cu);
    StateMethod(0x853F494u, 0x814A038u);
    StateMethod(0x853EF74u, 0x853F48Cu);
    NxTerrain_SetupView(ctx, (const float *)(ctx + 4));
    {
        LONGLONG t0 = Nx_Now();
        BOOL drawn;
        Nx_GpuTerrainMark(dev, TRUE);
        drawn = DrawGPU(dev, ctx);
        Nx_GpuTerrainMark(dev, FALSE);
        nx_stats.t_terrain += Nx_Now() - t0;
        ++nx_stats.terrain_calls;
        if (!drawn) return 0;   /* the game draws it (setup above is what it does too) */
    }
    /* the render states the game's last pass leaves */
    NxTerrain_PassStates(I32(ctx, 0xE4) ? 2 : 3);
    return 1;
}

void NxTerrain_Install(BOOL dx11)
{
    char path[MAX_PATH + 40], ini[MAX_PATH + 32], mode[16];
    BYTE *site = (BYTE *)(uintptr_t)DRAW_ADDR, *t;
    DWORD old, unused;
    int32_t rel;
    snprintf(path, sizeof path, "%s\\nx_terrain_dump.on", Renderer_GameFolder());
    g_dump = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    {
        FILE *f;
        snprintf(path, sizeof path, "%s\\nx_terrain_debug.txt", Renderer_GameFolder());
        if ((f = fopen(path, "r")) != NULL) {
            if (fscanf(f, "%d", &g_debug_view) != 1) g_debug_view = 0;
            fclose(f);
        }
    }
    {
        FILE *zf;
        snprintf(path, sizeof path, "%s\\nx_terrain_zoom.txt", Renderer_GameFolder());
        if ((zf = fopen(path, "r")) != NULL) { if (fscanf(zf, "%f", &g_dev_zoom) != 1 || g_dev_zoom < 1 || g_dev_zoom > 64) g_dev_zoom = 1; fclose(zf); }
    }
    snprintf(path, sizeof path, "%s\\nx_terrain_full.on", Renderer_GameFolder());
    g_full_detail = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(path, sizeof path, "%s\\nx_terrain_only.on", Renderer_GameFolder());
    nx_dev_terrain_only = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(path, sizeof path, "%s\\nx_terrain_nolod.on", Renderer_GameFolder());
    g_lod_off = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(ini, sizeof ini, "%s\\plugins\\igi2_unified.ini", Renderer_GameFolder());
    GetPrivateProfileStringA("Renderer", "Terrain", "gpu", mode, sizeof mode, ini);
    snprintf(path, sizeof path, "%s\\nx_terrain_cpu.on", Renderer_GameFolder());
    g_gpu = dx11 && _stricmp(mode, "cpu") && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
    {
        FILE *f;
        snprintf(path, sizeof path, "%s\\nx_terrain_eye.txt", Renderer_GameFolder());
        if ((f = fopen(path, "r")) != NULL) {
            g_eye_set = fscanf(f, "%f %f %f", &g_eye[0], &g_eye[1], &g_eye[2]) == 3;
            fclose(f);
        }
    }
    snprintf(path, sizeof path, "%s\\nx_view.txt", Renderer_GameFolder());
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_eye_set |= 2;   /* hook for the dev camera too */
    if ((!g_dump && !g_gpu && !g_eye_set) || g_hooked) {
        if (dx11 && !g_gpu) Renderer_Log("terrain: the game's own (CPU) terrain");
        return;
    }
    if (memcmp(site, kDrawPrologue, sizeof kDrawPrologue)) { Renderer_Log("terrain: draw is not the 1.3 code, left alone"); return; }
    t = (BYTE *)VirtualAlloc(NULL, 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kDrawPrologue, 6);
    t[6] = 0xE9;
    rel = (int32_t)(DRAW_RESUME - ((uintptr_t)t + 11));
    memcpy(t + 7, &rel, 4);
    nx_terrain_tramp = t;
    g_jump[0] = 0xE9;
    rel = (int32_t)((uintptr_t)NxTerrain_Entry - (DRAW_ADDR + 5));
    memcpy(g_jump + 1, &rel, 4);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, g_jump, 5);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    VirtualProtect(site, 5, old, &unused);
    g_hooked = TRUE;
    Renderer_Log(g_gpu ? "terrain: drawn on the GPU (Direct3D 11)" : "terrain: draw hooked for the dev dump");
}
