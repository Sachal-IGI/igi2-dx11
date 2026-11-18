#ifndef NEONIX_RENDERER_INTERNAL_H
#define NEONIX_RENDERER_INTERNAL_H
/* The Neonix renderer (modules/renderer). igi2.exe gets Direct3DCreate8 from d3d8.dll at 0x44A06C; renderer.c puts its own
   function in that pointer before the game uses it. Modes: the original Direct3D 8; the trace (d3d8_trace*.c: every call goes
   to the real Direct3D 8 and is counted, to learn what the game really uses); later Direct3D 11. */
#include "host.h"
#include <d3d8.h>
#include <stddef.h>

/* ---- trace: a pass-through copy of every Direct3D 8 object ---- */
typedef struct NxT {
    void *vtbl;      /* first, as in every COM object */
    IUnknown *real;  /* the d3d8.dll object */
    int kind;
} NxT;
enum { NXT_D3D, NXT_DEVICE, NXT_SWAP, NXT_SURF, NXT_VOL, NXT_VB, NXT_IB, NXT_TEX, NXT_CUBE, NXT_VOLTEX, NXT_KINDS };
extern void *nxt_vtbls[NXT_KINDS];                   /* d3d8_trace.c (generated) */
void *NxT_Wrap(int kind, void *real);                /* the one copy of a real object; NULL for NULL */
void *NxT_WrapBaseTexture(void *real);               /* texture, cube or volume texture by its type */
void *NxT_WrapContainer(REFIID riid, void *real);
void *NxT_Unwrap(void *object);                      /* the real object behind a copy; others unchanged */
void NxT_Released(NxT *copy);                        /* the real object's count reached 0 */
void NxT_Hit(const char *key, void *caller);         /* counts one call of this kind */
void NxT_Frame(IDirect3DDevice8 *real);              /* before Present: screenshot requests; the counts now and then */
void NxT_Write(void);
void NxT_Shader(char *key, size_t cap, const char *kind, const DWORD *decl, const DWORD *code, DWORD usage);
const char *NxT_TexFormat(IDirect3DBaseTexture8 *real);
const char *NxT_SurfFormat(IDirect3DSurface8 *real);
void NxT_PresentParams(char *key, size_t cap, const char *what, const D3DPRESENT_PARAMETERS *pp);
void NxT_DrawState(char *key, size_t cap, const char *what, DWORD prim);
void NxT_SetShader(DWORD handle);
void NxT_SetTexture(DWORD stage, IDirect3DBaseTexture8 *real);
void NxT_SetPixelShader(DWORD handle);
void NxT_DumpCaps(IDirect3D8 *d3d);
void NxT_ShaderHandle(DWORD handle);
extern const GUID nxt_IID_IUnknown, nxt_IID_IDirect3D8, nxt_IID_IDirect3DDevice8, nxt_IID_IDirect3DTexture8,
    nxt_IID_IDirect3DCubeTexture8, nxt_IID_IDirect3DVolumeTexture8, nxt_IID_IDirect3DBaseTexture8, nxt_IID_IDirect3DResource8,
    nxt_IID_IDirect3DSurface8, nxt_IID_IDirect3DVertexBuffer8, nxt_IID_IDirect3DIndexBuffer8, nxt_IID_IDirect3DSwapChain8;

/* generated name tables */
const char *NxT_RS(DWORD v);
const char *NxT_TSS(DWORD v);
const char *NxT_Fmt(DWORD v);
const char *NxT_Top(DWORD v);
const char *NxT_Blend(DWORD v);
const char *NxT_Cmp(DWORD v);
const char *NxT_Filter(DWORD v);
const char *NxT_Addr(DWORD v);
const char *NxT_Prim(DWORD v);
const char *NxT_Pool(DWORD v);
void NxT_RSValue(char *out, size_t cap, DWORD state, DWORD value);
void NxT_TSSValue(char *out, size_t cap, DWORD type, DWORD value);

/* dx11_d3d.c: IDirect3D8 on Direct3D 11, or NULL when it cannot run here */
void *Nx_CreateD3D(void);

/* ---- renderer.c ---- */
extern IDirect3D8 *(WINAPI *nx_real_create8)(UINT);
void Renderer_Log(const char *text);
BOOL Renderer_WriteCode(void *at, const void *bytes, SIZE_T size);
void NxScene_Install(void);           /* scene_cache.c: exact reuse of the per-object view test setup */
void NxPace_Install(void);            /* frame_pace.c: wait between capped frames instead of spinning */
void NxPace_Frame(void);
extern volatile LONG nx_present_count;
const char *Renderer_GameFolder(void);
BOOL Renderer_ShotRequest(unsigned frame, char *path, size_t cap);
void Renderer_LogCallers(const char *tag);
void Renderer_FrameTick(void);
extern BOOL nx_trace_quiet;
extern unsigned nx_anisotropy;   /* [Renderer] Anisotropy: 0/1 as the game asks, 2..16 forced for linear filtering */
extern unsigned nx_antialiasing; /* [Renderer] Antialiasing: 0 as the game asks, 2..16 samples for the back buffer */
extern float nx_draw_distance, nx_detail_distance; /* [Renderer] DrawDistance / DetailDistance over 100: 1 = the game's own (draw_distance.h) */
extern int nx_vsync; /* [Renderer] VSync: -1 follow the game (default), 0 off (uncapped, may tear), 1+ on (no tear, caps FPS to the refresh rate) */
extern unsigned nx_alpha_to_coverage; /* [Renderer] AlphaToCoverage (default 1): alpha-tested draws into a multisampled
                                         target take their coverage from alpha; foliage then takes the unsorted path */
void NxTerrain_Install(BOOL dx11);   /* dx11_terrain.c */
BOOL Renderer_WriteBMP(const char *path, unsigned w, unsigned h, const BYTE *pixels, unsigned pitch, unsigned bpp);
#endif
