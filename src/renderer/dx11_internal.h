#ifndef NEONIX_DX11_INTERNAL_H
#define NEONIX_DX11_INTERNAL_H
/* The Neonix renderer's Direct3D 11 mode: the game's Direct3D 8 objects implemented on Direct3D 11.
   dx11_d3d.c IDirect3D8, dx11_device.c the device, dx11_res.c textures/surfaces/buffers, dx11_shaders.c the fixed-function
   shaders and shader compiling, dx11_vs.c the game's vs.1.x shaders to HLSL.
   No lost device exists here: Reset only resizes, Present never fails with D3DERR_DEVICELOST. The game's back buffer is our
   own render target; Present draws it into the window's swap chain (scaled, with the gamma ramp). */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef CINTERFACE
#define CINTERFACE
#endif
#include "renderer_internal.h"
#define D3DCOLORVALUE_DEFINED   /* d3d8types.h has it; dxgitype.h would define it again */
#include <d3d11.h>
#include <dxgi1_2.h>
#include "dx11_vs.h"
#include "dx11_ps.h"

typedef struct NxDevice NxDevice;
typedef struct NxQueue NxQueue;
typedef struct NxTexture NxTexture;

enum { NXK_D3D = 1, NXK_DEVICE, NXK_TEXTURE, NXK_CUBE, NXK_SURFACE, NXK_VB, NXK_IB, NXK_SWAP };

typedef struct NxPrivate { GUID id; DWORD size; DWORD flags; struct NxPrivate *next; BYTE data[1]; } NxPrivate;

/* common head of every object */
typedef struct NxHead {
    void *vtbl;
    LONG refs;
    int kind;
    NxDevice *dev;
    NxPrivate *priv;
    DWORD priority;
} NxHead;

/* a D3D8 format and how it is stored on the GPU */
typedef struct NxFormat {
    D3DFORMAT d3d;
    DXGI_FORMAT dxgi;      /* texture format */
    BYTE bpp;              /* bytes per pixel in the D3D8 layout (0 for block formats) */
    BYTE block;            /* bytes per 4x4 block (DXT), else 0 */
    BYTE convert;          /* NXC_*: how the D3D8 bytes become dxgi bytes */
} NxFormat;
enum { NXC_COPY, NXC_A1R5G5B5, NXC_X1R5G5B5, NXC_R5G6B5, NXC_A4R4G4B4, NXC_X8R8G8B8, NXC_A8R8G8B8, NXC_R8G8B8, NXC_L8,
       NXC_A8, NXC_A8L8, NXC_X4R4G4B4, NXC_A8R3G3B2, NXC_R3G3B2 };
const NxFormat *Nx_Format(D3DFORMAT f);    /* NULL: not supported */
void Nx_CheckNativeFormats(ID3D11Device *d);
BOOL Nx_Native(const NxFormat *f);         /* textures of this format keep the D3D8 bytes on the card */

/* what one frame did, for the slow-frame lines in the log */
typedef struct NxStats {
    unsigned draws, tex_uploads, tex_kb, buf_maps, cb_uploads, readbacks, compiles;
    LONGLONG t_draw, t_lock;      /* QueryPerformanceCounter ticks spent in draws and in locks/unlocks this frame */
    LONGLONG t_create;            /* ... and in creating textures and buffers */
    LONGLONG t_terrain;           /* ... and in the GPU terrain (dx11_terrain.c), with its calls */
    unsigned terrain_calls;
} NxStats;
LONGLONG Nx_Now(void);
extern NxStats nx_stats;
extern BOOL nx_bgra;
extern UINT nx_ms_supported;   /* bit n: n samples work for the back buffer and depth */

typedef struct NxSurface {
    NxHead h;
    NxTexture *container;  /* texture or cube owning this level; NULL for standalone surfaces */
    UINT level, face;
    D3DFORMAT format;
    const NxFormat *fmt;
    UINT width, height;
    DWORD usage;
    D3DPOOL pool;
    D3DMULTISAMPLE_TYPE ms;
    UINT samples;          /* D3D11 sample count of tex (multisampled back buffer and depth) */
    ID3D11Texture2D *resolved;            /* single-sample copy of a multisampled target */
    ID3D11ShaderResourceView *resolved_srv;
    BYTE *mem;             /* the D3D8 bytes (system copy), made on the first lock */
    UINT pitch;
    BOOL locked, dirty;
    RECT dirty_rect;
    /* standalone GPU surfaces: back buffer, render targets, depth stencils, image surfaces */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
} NxSurface;

struct NxTexture {
    NxHead h;
    BOOL cube;
    D3DFORMAT format;
    const NxFormat *fmt;
    UINT width, height, levels;
    DWORD usage;
    D3DPOOL pool;
    DWORD lod;
    BOOL native;                   /* tex has the D3D8 format's own layout (see Nx_Native), not B8G8R8A8 */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *rtv;   /* level 0, render target textures */
    NxSurface **surf;              /* levels (x6 faces for a cube): surf[face * levels + level] */
};

typedef struct NxBuffer {
    NxHead h;
    BOOL index;
    UINT size;
    DWORD usage, fvf;
    D3DFORMAT format;              /* index buffers */
    D3DPOOL pool;
    ID3D11Buffer *buf;
    BOOL dynamic;                  /* D3D11 DYNAMIC: locks map the buffer directly (vertex buffers) */
    BOOL rt_shadow;                /* a dynamic buffer given a copy for the render thread */
    BOOL lazy, pending_discard;    /* render thread: written data goes to the card at the draws that use it */
    UINT up_lo, up_hi;             /* ... the byte range the card has since the last discard */
    BOOL mapped, discarded;
    BYTE *shadow;                  /* system copy: static buffers, and every index buffer (fans need the indices) */
    UINT lock_offset, lock_size;
    DWORD lock_flags;
    UINT dirty_lo, dirty_hi;
} NxBuffer;

/* what the fixed-function and game vertex shaders need */
typedef struct NxShaderVS {
    DWORD handle;                  /* D3D8 handle: (index << 1) | 1 */
    ID3D11VertexShader *vs;
    ID3D11InputLayout *layout;
    NxVSDecl decl;
    BOOL used;
} NxShaderVS;

typedef struct NxFFShader {        /* the input layout of one FVF for the shared fixed-function vertex shader */
    DWORD fvf;
    ID3D11VertexShader *vs;        /* NxDevice.vs_ff, not owned */
    ID3D11InputLayout *layout;
    int info[4];                   /* pre-transformed, has normal, has diffuse, has specular */
    int texsize[8];                /* values in each texture coordinate (0: none) */
} NxFFShader;

/* constant buffers, laid out as in dx11_shaders.c's HLSL */
typedef struct NxLightCB {
    float diffuse[4], specular[4], ambient[4];
    float position[4];     /* view space, w = 1 */
    float direction[4];    /* view space, towards the light for directional lights */
    float atten[4];        /* range, att0, att1, att2 */
    float spot[4];         /* falloff, cos(theta/2), cos(phi/2), type */
} NxLightCB;

typedef struct NxObjCB {           /* b0: what changes with each object */
    float wv[16], wvp[16], wvn[16];     /* world*view, world*view*proj, normal matrix (inverse transpose of wv) */
    int fvf[4];                    /* NxFFShader.info of the bound FVF */
    int texsize[8];
} NxObjCB;

typedef struct NxVSCB {            /* b4 */
    float texm[8][16];
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
    float ambient[4];              /* D3DRS_AMBIENT */
    float fog[4];                  /* start, end, density, vertex mode (0 none, 1 exp, 2 exp2, 3 linear) */
    float misc[4];                 /* material power, fog enabled, range fog, table fog */
    int flags[4];                  /* lighting, normalize, color vertex, specular enable */
    int sources[4];                /* diffuse, specular, ambient, emissive material sources */
    int nlights[4];                /* light count */
    int tci[8][4];                 /* stage: coordinate index, texgen (0 passthru, 1 normal, 2 position, 3 reflection), transform count, projected */
    float rhw[4];                  /* XYZRHW to clip: 2/w, -2/h, -1 - 2*x/w (+ half pixel), 1 + 2*y/h */
    float fix[4];                  /* half-pixel: added to pos.xy * pos.w */
    NxLightCB light[8];
} NxVSCB;

typedef struct NxPSCB {            /* b3 */
    int color[8][4];               /* op, arg1, arg2, arg0 */
    int alpha[8][4];               /* op, arg1, arg2, arg0 */
    int stage[8][4];               /* result arg, texture kind (0 none, 1 2D, 2 cube), projected, - */
    float tfactor[4];
    float fogcolor[4];
    float alpharef[4];             /* ref / 255 */
    int misc[4];                   /* alpha func (D3DCMP), fog on, specular add, table fog mode */
    float fogtable[4];             /* start, end, density, - (table fog) */
} NxPSCB;

typedef struct NxStateBlock {
    DWORD token;
    struct NxState *state;
} NxStateBlock;

/* everything the D3D8 device remembers */
typedef struct NxState {
    DWORD rs[256];
    DWORD tss[8][32];
    D3DMATRIX world, view, proj, tex[8];
    D3DVIEWPORT8 viewport;
    D3DMATERIAL8 material;
    D3DLIGHT8 light[8];
    BOOL light_on[8];
    float clip[8][4];
    DWORD vs, ps;
    float vsc[256][4];
    float psc[8][4];
    D3DCLIPSTATUS8 clip_status;
} NxState;

#define NX_MAX_STATEBLOCKS 64
#define NX_MAX_VS 128
#define NX_MAX_PS 64

struct NxDevice {
    NxHead h;
    IDirect3D8 *d3d;
    HWND window;
    D3DPRESENT_PARAMETERS pp;
    DWORD behavior;
    /* Direct3D 11 */
    ID3D11Device *d;
    ID3D11DeviceContext *c;
    NxQueue *q;                    /* the render thread's queue (dx11_queue.c), NULL without one */
    D3D_FEATURE_LEVEL level;
    IDXGIFactory2 *factory2;
    IDXGIFactory1 *factory1;
    IDXGISwapChain *swap;
    IDXGISwapChain1 *swap1;
    BOOL flip, tearing;
    UINT swap_w, swap_h;
    ID3D11RenderTargetView *swap_rtv;
    /* the game's back buffer and depth */
    NxSurface *backbuffer, *depth;
    NxSurface *rt, *zs;            /* bound */
    /* state */
    NxState s;
    NxTexture *tex[8];
    NxBuffer *stream[16];
    UINT stride[16];
    NxBuffer *indices;
    UINT base_vertex;
    BOOL in_scene;
    BOOL recording;
    NxStateBlock blocks[NX_MAX_STATEBLOCKS];
    DWORD next_block;
    NxShaderVS vsh[NX_MAX_VS];
    ID3D11PixelShader *psh[NX_MAX_PS];   /* the game's (and PostFX's) pixel shaders */
    NxFFShader ff[64];
    unsigned ff_count;
    /* D3D11 objects of the pipeline */
    ID3D11PixelShader *ps_ff, *ps_blit;
    ID3D11VertexShader *vs_blit;
    ID3D11Buffer *cb_obj, *cb_vs, *cb_vsc, *cb_fix, *cb_ps, *cb_psc, *cb_blit;
    ID3D11VertexShader *vs_ff;     /* the fixed-function vertex shader, every FVF */
    ID3D11Buffer *ff_defaults;     /* input slot 15: elements an FVF lacks */
    ID3D11VertexShader *vs_clear;
    ID3D11PixelShader *ps_clear;
    ID3D11Buffer *cb_clear;
    ID3D11DepthStencilState *clear_depth[4];
    ID3D11BlendState *clear_blend[2];
    ID3D11Buffer *up_vb, *up_ib;   /* DrawPrimitiveUP data and generated indices (fans) */
    UINT up_vb_size, up_vb_at, up_ib_size, up_ib_at;
    ID3D11Texture2D *gamma_tex;
    ID3D11ShaderResourceView *gamma_srv;
    ID3D11SamplerState *blit_sampler;
    ID3D11ShaderResourceView *white_srv, *white_cube_srv;
    BOOL gamma_set;
    struct { DWORD key[3]; ID3D11BlendState *o; } blend[64];
    struct { DWORD key[3]; ID3D11DepthStencilState *o; } depth_states[64];
    struct { DWORD key[2]; ID3D11RasterizerState *o; } raster[32];
    struct { DWORD key[3]; ID3D11SamplerState *o; } samplers[128];
    unsigned nblend, ndepth, nraster, nsamplers;
    /* dirty tracking: rebuild constant buffers / D3D11 states at the next draw */
    BOOL dirty_obj, dirty_vs, dirty_fix, dirty_vsc, dirty_ps, dirty_psc, dirty_states, dirty_textures, dirty_streams;
    UINT tex_dirty, smp_dirty;   /* stages whose texture / sampler states changed (dirty_textures: all of them) */
    unsigned frames;
    /* last bound D3D11 objects, to skip repeated binds */
    void *bound_vs, *bound_layout, *bound_blend, *bound_depth, *bound_raster, *bound_ps;
    void *bound_srv[16], *bound_sampler[8];
    ID3D11Buffer *bound_vb[16];
    UINT bound_stride[16], bound_offset[16];
    ID3D11Buffer *bound_ib;
    DXGI_FORMAT bound_ib_format;
    UINT bound_ib_offset;
    int bound_topology;
};

/* dx11_d3d.c */
void *Nx_CreateD3D(void);                                /* IDirect3D8, or NULL when Direct3D 11 is not usable here */
extern D3DCAPS8 nx_caps;
HRESULT Nx_D3D11Create(ID3D11Device **d, ID3D11DeviceContext **c, D3D_FEATURE_LEVEL *level);
extern HRESULT (WINAPI *nx_D3DCompile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
                                       ID3DBlob **, ID3DBlob **);

/* dx11_device.c */
HRESULT Nx_CreateDevice(IDirect3D8 *d3d, HWND window, DWORD behavior, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out);
void Nx_Release(void *object);    /* internal reference released */
void Nx_AddRef(void *object);

/* dx11_res.c */
HRESULT Nx_NewTexture(NxDevice *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, BOOL cube, NxTexture **out);
HRESULT Nx_NewSurface(NxDevice *dev, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms, NxSurface **out);
HRESULT Nx_NewBuffer(NxDevice *dev, BOOL index, UINT size, DWORD usage, DWORD fvf, D3DFORMAT f, D3DPOOL pool, NxBuffer **out);
void Nx_FlushTexture(NxTexture *t);          /* uploads dirty levels before a draw */
void Nx_BufferBeforeDraw(NxBuffer *b);       /* static buffer: uploads its dirty range */
void Nx_ScreenshotKey(NxDevice *dev);   /* screenshot.c: [Screenshots] Key saves the frame as a PNG */
HRESULT Nx_ReadSurface(NxSurface *s);        /* GPU surface -> s->mem (screenshots, CopyRects to system surfaces) */
HRESULT Nx_CopyRects(NxDevice *dev, NxSurface *src, const RECT *rects, UINT n, NxSurface *dst, const POINT *points);
ID3D11Texture2D *Nx_SurfaceTexture(NxSurface *s, UINT *sub);   /* the D3D11 texture and subresource holding a surface */
ID3D11Texture2D *Nx_Resolve(NxSurface *s);                    /* single-sample texture of a target (resolved now) */
void Nx_ConvertRow(BYTE convert, const BYTE *src, BYTE *dst, UINT pixels);
extern void *nx_texture_vtbl, *nx_cube_vtbl, *nx_surface_vtbl, *nx_vb_vtbl, *nx_ib_vtbl;
HRESULT Nx_PrivateSet(NxHead *h, REFGUID id, const void *data, DWORD size, DWORD flags);
HRESULT Nx_PrivateGet(NxHead *h, REFGUID id, void *data, DWORD *size);
HRESULT Nx_PrivateFree(NxHead *h, REFGUID id);
void Nx_PrivateClear(NxHead *h);

/* dx11_shaders.c */
BOOL Nx_ShadersInit(NxDevice *dev);
void Nx_ShadersRelease(NxDevice *dev);
NxFFShader *Nx_FFShader(NxDevice *dev, DWORD fvf);
HRESULT Nx_CompileVS(NxDevice *dev, const char *hlsl, ID3D11VertexShader **vs, ID3DBlob **code);
extern HRESULT (WINAPI *nx_D3DCreateBlob)(SIZE_T, ID3DBlob **);   /* the shader cache (dx11_shaders.c) */
void Nx_ShaderCacheReport(void);
HRESULT Nx_CompilePS(NxDevice *dev, const char *hlsl, ID3D11PixelShader **ps, ID3DBlob **code);
UINT Nx_FVFSize(DWORD fvf);

/* dx11_device.c, for dx11_terrain.c */
BOOL Nx_IsDevice(void *p);                                 /* p is one of our IDirect3DDevice8 objects */
BOOL Nx_IsTexture(void *p);                                /* ... one of our 2D textures */
ID3D11SamplerState *Nx_StageSampler(NxDevice *dev, DWORD stage);   /* the sampler of a stage's current states */
void Nx_ForgetBindings(NxDevice *dev);                     /* after drawing with own D3D11 state: rebind everything */
void Nx_BindTargets(NxDevice *dev);

const unsigned char *Nx_TerrainCode(BOOL ps, SIZE_T *size);   /* dx11_shaders.c: nx_terrain.hlsl compiled */

const D3DMATRIX *Nx_DevView(void);    /* dev: the view of nx_view.txt (rotation only), or NULL */
extern float nx_dev_eye[3];           /* ... and its camera position in view units */
void Nx_DevSetView(const float e[3], const float a[3]);   /* dev: a view as nx_view.txt gives it (eye, target; view units) */
extern int nx_scan_active;            /* dev: the terrain hole scan runs (dx11_terrain_scan.inc) */
void Nx_TerrainScanFrame(NxDevice *dev);   /* ... its step at each Present */
extern int nx_dev_terrain_only;       /* dev: nx_terrain_only.on, only the terrain is drawn (terrain comparisons) */
extern int nx_in_terrain;             /* inside the game's own terrain draw */
extern float nx_last_eye[3], nx_last_fwd[3];   /* the camera of the last terrain draw: view units, forward (dev shots log it) */

/* dx11_terrain.c */
void NxTerrain_DeviceGone(NxDevice *dev);

/* dx11_queue.c: the render thread. NxC_* are the device context's calls: run now without a render thread (or on it),
   recorded into its queue otherwise. */
BOOL NxQ_Wanted(void);                     /* [Renderer] RenderThread */
BOOL NxQ_Start(NxDevice *dev);
void NxQ_Stop(NxDevice *dev);              /* runs what is queued, ends the thread */
void NxQ_Sync(NxDevice *dev);              /* runs what is queued; then the main thread may use dev->c until it records again */
void NxQ_FrameEnd(NxDevice *dev);          /* after a frame's Present is recorded: waits while 2 frames are queued */
BOOL NxQ_OnRenderThread(void);
void NxC_PSSetShaderResources(NxDevice *dev, UINT s, UINT n, ID3D11ShaderResourceView *const *v);
void NxC_VSSetShaderResources(NxDevice *dev, UINT s, UINT n, ID3D11ShaderResourceView *const *v);
void NxC_PSSetConstantBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *v);
void NxC_VSSetConstantBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *v);
void NxC_PSSetSamplers(NxDevice *dev, UINT s, UINT n, ID3D11SamplerState *const *v);
void NxC_VSSetShader(NxDevice *dev, ID3D11VertexShader *o, ID3D11ClassInstance *const *ci, UINT nci);
void NxC_PSSetShader(NxDevice *dev, ID3D11PixelShader *o, ID3D11ClassInstance *const *ci, UINT nci);
void NxC_RSSetState(NxDevice *dev, ID3D11RasterizerState *o);
void NxC_RSSetViewports(NxDevice *dev, UINT n, const D3D11_VIEWPORT *vp);
void NxC_OMSetRenderTargets(NxDevice *dev, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv);
void NxC_OMSetDepthStencilState(NxDevice *dev, ID3D11DepthStencilState *o, UINT ref);
void NxC_OMSetBlendState(NxDevice *dev, ID3D11BlendState *o, const FLOAT factor[4], UINT mask);
void NxC_IASetPrimitiveTopology(NxDevice *dev, D3D11_PRIMITIVE_TOPOLOGY t);
void NxC_IASetInputLayout(NxDevice *dev, ID3D11InputLayout *o);
void NxC_IASetVertexBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *b, const UINT *stride, const UINT *offset);
void NxC_IASetIndexBuffer(NxDevice *dev, ID3D11Buffer *b, DXGI_FORMAT f, UINT off);
void NxC_Draw(NxDevice *dev, UINT n, UINT start);
void NxC_DrawIndexed(NxDevice *dev, UINT n, UINT start, INT base);
void NxC_DrawIndexedInstanced(NxDevice *dev, UINT n, UINT inst, UINT start, INT base, UINT first);
void NxC_Begin(NxDevice *dev, ID3D11Asynchronous *o);
void NxC_End(NxDevice *dev, ID3D11Asynchronous *o);
void NxC_UpdateSubresource(NxDevice *dev, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT pitch,
                           UINT depth_pitch, UINT bytes);
void NxC_CopySubresourceRegion(NxDevice *dev, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub,
                               const D3D11_BOX *box);
void NxC_ResolveSubresource(NxDevice *dev, ID3D11Resource *dst, UINT dsub, ID3D11Resource *src, UINT ssub, DXGI_FORMAT f);
void NxC_ClearRenderTargetView(NxDevice *dev, ID3D11RenderTargetView *v, const FLOAT color[4]);
void NxC_ClearDepthStencilView(NxDevice *dev, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil);
void NxC_WriteBuffer(NxDevice *dev, ID3D11Buffer *b, D3D11_MAP mode, UINT offset, const void *data, UINT bytes);
void NxC_Release(NxDevice *dev, void *o);
void NxC_Call(NxDevice *dev, void (*fn)(NxDevice *, void *), const void *arg, UINT bytes);
void Nx_BufferNeed(NxDevice *dev, NxBuffer *b, UINT lo, UINT hi);   /* dx11_res.c: a draw uses bytes lo..hi of b */
void Nx_GpuTerrainMark(NxDevice *dev, BOOL begin);   /* GPU timestamps around a terrain draw */

void Nx_Log(const char *format, ...);
#endif
