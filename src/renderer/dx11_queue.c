/* Render thread (Direct3D 11 mode, [Renderer] RenderThread=1).
   igi2.exe runs everything on one thread. Of a frame's CPU time about half was ours: turning the game's Direct3D 8 calls
   into Direct3D 11 calls, and the Direct3D 11 runtime and the driver behind them (~1000 draws a frame in multiplayer).
   With a render thread the main thread still translates, but records the Direct3D 11 context calls (NxC_*) into a queue;
   a second thread runs them in order, including the frame's Present. So the runtime and driver work runs on another core
   while the game builds the next frame.
   Rules that keep it safe:
   - every call on the device context goes through NxC_* (direct when there is no render thread or when already on it);
   - data the game hands over (constants, vertices, texture rows) is copied into the queue when recorded;
   - a Direct3D 11 object the main thread lets go of is released through the queue, after the commands that use it;
   - anything that reads back from the GPU (screenshots, locks of render targets) first waits until the queue is empty
     (NxQ_Sync) and then uses the context itself; Reset and the device's end stop the thread first;
   - the main thread is at most 2 frames ahead of the render thread.
   Device creation calls (CreateBuffer, CreateTexture2D, ...) stay on the main thread: the device is free-threaded. */
#include "dx11_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define Q_BYTES (16u << 20)        /* the ring; a power of two */
#define Q_BLOB (1u << 20)          /* data larger than this goes to its own allocation */
#define Q_MAX_FRAMES 2

enum {
    C_WRAP = 1, C_PS_SRV, C_VS_SRV, C_PS_CB, C_VS_CB, C_PS_SMP, C_VS, C_PS, C_VIEWPORTS, C_RS, C_TARGETS, C_DEPTH, C_BLEND,
    C_TOPOLOGY, C_LAYOUT, C_VB, C_IB, C_DRAW, C_DRAW_INDEXED, C_DRAW_INSTANCED, C_BEGIN, C_END, C_UPDATE, C_COPY, C_RESOLVE,
    C_CLEAR_RTV, C_CLEAR_DSV, C_WRITE, C_RELEASE, C_CALL, C_FRAME
};

typedef struct Cmd { UINT type, size; } Cmd;     /* size: of the whole command, 8-byte aligned */

/* The two threads' counters sit 64 bytes apart (their own cache lines): sharing one made every recorded command
   bounce that line between the cores. */
struct NxQueue {
    NxDevice *dev;
    BYTE *ring;
    HANDLE work, room, thread;
    DWORD thread_id;
    BYTE pad0[64];
    volatile LONG head;                          /* bytes written (main thread), counting up; the ring position is & mask */
    LONG tail_seen;                              /* the main thread's last look at tail */
    BYTE pad1[64];
    volatile LONG tail;                          /* bytes run (render thread) */
    BYTE pad2[64];
    volatile LONG sleeping, waiting_room;        /* the render thread waits for work / the main thread for room */
    volatile LONG frames;                        /* frames recorded and not yet presented */
    volatile LONG stop;
    unsigned spin;                               /* pauses before the render thread sleeps (0 below 4 processors) */
    BYTE pad3[64];
};

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

static DWORD g_rt_id;                            /* the render thread, while one runs */
static LONGLONG q_bytes, q_cmds, q_room_waits, q_frame_waits, q_frames, q_big;   /* per 5 s, for the log */
static DWORD q_log_at;

BOOL NxQ_OnRenderThread(void) { return g_rt_id && GetCurrentThreadId() == g_rt_id; }

static unsigned Processors(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

/* [Renderer] RenderThread in plugins\\igi2_unified.ini: 1 on, 0 off, missing = on with 4 or more logical processors
   (with fewer the render thread would take time from the game thread, and a 2-core CPU gains little) */
BOOL NxQ_Wanted(void)
{
    char ini[MAX_PATH + 32];
    int v;
    snprintf(ini, sizeof ini, "%s\\plugins\\igi2_unified.ini", Renderer_GameFolder());
    v = (int)GetPrivateProfileIntA("Renderer", "RenderThread", -1, ini);
    if (v < 0) return Processors() >= 4;
    return v != 0 && Processors() >= 2;
}

static BOOL Direct(NxDevice *dev) { return !dev->q || GetCurrentThreadId() == dev->q->thread_id; }

/* ---- the ring (one writer: the main thread; one reader: the render thread) ---- */
static BYTE *Alloc(NxQueue *q, UINT type, UINT size)
{
    size = (size + 7u) & ~7u;
    for (;;) {
        LONG head = q->head, tail = q->tail_seen;
        if (Q_BYTES - (UINT)(head - tail) < size + Q_BYTES / 2) tail = q->tail_seen = q->tail;   /* look again only when room runs short */
        UINT used = (UINT)(head - tail), at = (UINT)head & (Q_BYTES - 1), contig = Q_BYTES - at;
        if (contig < size) {
            /* not enough room before the end: a wrap marker, then the start of the ring */
            if (Q_BYTES - used >= contig + 8) {
                Cmd *w = (Cmd *)(q->ring + at);
                w->type = C_WRAP; w->size = contig;
                COMPILER_BARRIER();
                q->head = head + (LONG)contig;
                continue;
            }
        } else if (Q_BYTES - used >= size + 8) {
            Cmd *c = (Cmd *)(q->ring + at);
            c->type = type; c->size = size;
            return (BYTE *)c;
        }
        /* full: wait for the render thread */
        ++q_room_waits;
        q->tail_seen = q->tail;
        if (q->sleeping) SetEvent(q->work);
        InterlockedExchange(&q->waiting_room, 1);
        if (q->head - q->tail >= (LONG)(Q_BYTES - size - 8)) WaitForSingleObject(q->room, 1);
        InterlockedExchange(&q->waiting_room, 0);
    }
}

/* x86 keeps stores in order, so the command's bytes are there before the new head. No fence here: a fence on every
   command was 3 % of the game thread in a test game. Without it a wake-up can be missed when the render thread goes
   to sleep just as a command lands (its look at head can miss the store still in this core's buffer); the next
   command sees "sleeping" and wakes it, and the frame's end (NxQ_FrameEnd) fences before its check, so the most a
   missed wake-up costs is the render thread's 1 ms sleep while the game thread keeps recording. */
static void Commit(NxQueue *q, BYTE *c)
{
    q_bytes += ((Cmd *)c)->size; ++q_cmds;
    COMPILER_BARRIER();
    q->head = q->head + (LONG)((Cmd *)c)->size;
    COMPILER_BARRIER();
    if (q->sleeping) SetEvent(q->work);
}

#define ARGS(T) T *a = (T *)(c + sizeof(Cmd))

typedef struct { UINT start, n; } A_Slots;           /* followed by n pointers */
typedef struct { void *o; } A_Obj;
typedef struct { void *o; UINT x; } A_ObjX;
typedef struct { UINT n; D3D11_VIEWPORT vp[2]; } A_Viewports;
typedef struct { UINT n; void *rtv[8]; void *dsv; } A_Targets;
typedef struct { void *o; BOOL has_factor; FLOAT factor[4]; UINT mask; } A_Blend;
typedef struct { UINT start, n; void *b[16]; UINT stride[16], offset[16]; } A_VB;
typedef struct { void *b; DXGI_FORMAT f; UINT off; } A_IB;
typedef struct { UINT a, b, c, d, e; INT base; } A_Draw;
typedef struct { void *res; UINT sub; BOOL has_box; D3D11_BOX box; UINT pitch, depth_pitch, bytes; BYTE *blob; } A_Update;
typedef struct { void *dst; UINT dsub, x, y, z; void *src; UINT ssub; BOOL has_box; D3D11_BOX box; } A_Copy;
typedef struct { void *dst; UINT dsub; void *src; UINT ssub; DXGI_FORMAT f; } A_Resolve;
typedef struct { void *v; FLOAT color[4]; UINT flags; FLOAT depth; UINT8 stencil; } A_Clear;
typedef struct { void *b; D3D11_MAP mode; UINT offset, bytes; BYTE *blob; } A_Write;
typedef struct { void (*fn)(NxDevice *, void *); UINT bytes; } A_Call;

static void *Data(BYTE *c, size_t args) { return c + sizeof(Cmd) + ((args + 7u) & ~(size_t)7u); }

/* ---- running a command (the render thread, or directly) ---- */
static void Run(NxDevice *dev, BYTE *c)
{
    ID3D11DeviceContext *x = dev->c;
    switch (((Cmd *)c)->type) {
    case C_PS_SRV: { ARGS(A_Slots); ID3D11DeviceContext_PSSetShaderResources(x, a->start, a->n, (ID3D11ShaderResourceView *const *)Data(c, sizeof *a)); break; }
    case C_VS_SRV: { ARGS(A_Slots); ID3D11DeviceContext_VSSetShaderResources(x, a->start, a->n, (ID3D11ShaderResourceView *const *)Data(c, sizeof *a)); break; }
    case C_PS_CB: { ARGS(A_Slots); ID3D11DeviceContext_PSSetConstantBuffers(x, a->start, a->n, (ID3D11Buffer *const *)Data(c, sizeof *a)); break; }
    case C_VS_CB: { ARGS(A_Slots); ID3D11DeviceContext_VSSetConstantBuffers(x, a->start, a->n, (ID3D11Buffer *const *)Data(c, sizeof *a)); break; }
    case C_PS_SMP: { ARGS(A_Slots); ID3D11DeviceContext_PSSetSamplers(x, a->start, a->n, (ID3D11SamplerState *const *)Data(c, sizeof *a)); break; }
    case C_VS: { ARGS(A_Obj); ID3D11DeviceContext_VSSetShader(x, (ID3D11VertexShader *)a->o, NULL, 0); break; }
    case C_PS: { ARGS(A_Obj); ID3D11DeviceContext_PSSetShader(x, (ID3D11PixelShader *)a->o, NULL, 0); break; }
    case C_VIEWPORTS: { ARGS(A_Viewports); ID3D11DeviceContext_RSSetViewports(x, a->n, a->vp); break; }
    case C_RS: { ARGS(A_Obj); ID3D11DeviceContext_RSSetState(x, (ID3D11RasterizerState *)a->o); break; }
    case C_TARGETS: { ARGS(A_Targets); ID3D11DeviceContext_OMSetRenderTargets(x, a->n, (ID3D11RenderTargetView *const *)a->rtv, (ID3D11DepthStencilView *)a->dsv); break; }
    case C_DEPTH: { ARGS(A_ObjX); ID3D11DeviceContext_OMSetDepthStencilState(x, (ID3D11DepthStencilState *)a->o, a->x); break; }
    case C_BLEND: { ARGS(A_Blend); ID3D11DeviceContext_OMSetBlendState(x, (ID3D11BlendState *)a->o, a->has_factor ? a->factor : NULL, a->mask); break; }
    case C_TOPOLOGY: { ARGS(A_ObjX); ID3D11DeviceContext_IASetPrimitiveTopology(x, (D3D11_PRIMITIVE_TOPOLOGY)a->x); break; }
    case C_LAYOUT: { ARGS(A_Obj); ID3D11DeviceContext_IASetInputLayout(x, (ID3D11InputLayout *)a->o); break; }
    case C_VB: { ARGS(A_VB); ID3D11DeviceContext_IASetVertexBuffers(x, a->start, a->n, (ID3D11Buffer *const *)a->b, a->stride, a->offset); break; }
    case C_IB: { ARGS(A_IB); ID3D11DeviceContext_IASetIndexBuffer(x, (ID3D11Buffer *)a->b, a->f, a->off); break; }
    case C_DRAW: { ARGS(A_Draw); ID3D11DeviceContext_Draw(x, a->a, a->b); break; }
    case C_DRAW_INDEXED: { ARGS(A_Draw); ID3D11DeviceContext_DrawIndexed(x, a->a, a->b, a->base); break; }
    case C_DRAW_INSTANCED: { ARGS(A_Draw); ID3D11DeviceContext_DrawIndexedInstanced(x, a->a, a->b, a->c, a->base, a->d); break; }
    case C_BEGIN: { ARGS(A_Obj); ID3D11DeviceContext_Begin(x, (ID3D11Asynchronous *)a->o); break; }
    case C_END: { ARGS(A_Obj); ID3D11DeviceContext_End(x, (ID3D11Asynchronous *)a->o); break; }
    case C_UPDATE: {
        ARGS(A_Update);
        const void *data = a->blob ? (const void *)a->blob : Data(c, sizeof *a);
        ID3D11DeviceContext_UpdateSubresource(x, (ID3D11Resource *)a->res, a->sub, a->has_box ? &a->box : NULL, data, a->pitch, a->depth_pitch);
        free(a->blob);
        break;
    }
    case C_COPY: {
        ARGS(A_Copy);
        ID3D11DeviceContext_CopySubresourceRegion(x, (ID3D11Resource *)a->dst, a->dsub, a->x, a->y, a->z, (ID3D11Resource *)a->src, a->ssub,
                                                  a->has_box ? &a->box : NULL);
        break;
    }
    case C_RESOLVE: { ARGS(A_Resolve); ID3D11DeviceContext_ResolveSubresource(x, (ID3D11Resource *)a->dst, a->dsub, (ID3D11Resource *)a->src, a->ssub, a->f); break; }
    case C_CLEAR_RTV: { ARGS(A_Clear); ID3D11DeviceContext_ClearRenderTargetView(x, (ID3D11RenderTargetView *)a->v, a->color); break; }
    case C_CLEAR_DSV: { ARGS(A_Clear); ID3D11DeviceContext_ClearDepthStencilView(x, (ID3D11DepthStencilView *)a->v, a->flags, a->depth, a->stencil); break; }
    case C_WRITE: {
        ARGS(A_Write);
        D3D11_MAPPED_SUBRESOURCE m;
        const BYTE *data = a->blob ? a->blob : (const BYTE *)Data(c, sizeof *a);
        if (SUCCEEDED(ID3D11DeviceContext_Map(x, (ID3D11Resource *)a->b, 0, a->mode, 0, &m))) {
            memcpy((BYTE *)m.pData + a->offset, data, a->bytes);
            ID3D11DeviceContext_Unmap(x, (ID3D11Resource *)a->b, 0);
        }
        free(a->blob);
        break;
    }
    case C_RELEASE: { ARGS(A_Obj); IUnknown_Release((IUnknown *)a->o); break; }
    case C_CALL: { ARGS(A_Call); a->fn(dev, Data(c, sizeof *a)); break; }
    case C_FRAME: InterlockedDecrement(&dev->q->frames); SetEvent(dev->q->room); break;
    default: break;
    }
}

static DWORD WINAPI RenderThread(void *arg)
{
    NxQueue *q = (NxQueue *)arg;
    LONG tail = q->tail;
    for (;;) {
        LONG head = q->head;
        COMPILER_BARRIER();
        if (tail == head) {
            unsigned spin;
            if (q->stop) break;
            /* a short spin before sleeping: the main thread records a frame in bursts, and waking a sleeping thread
               costs it a system call (it was most of the recording cost) */
            for (spin = 0; spin < q->spin && q->head == tail && !q->stop; ++spin) __asm__ __volatile__("pause");
            if (q->head != tail) continue;
            InterlockedExchange(&q->sleeping, 1);
            if (q->head == tail && !q->stop) WaitForSingleObject(q->work, 1);
            InterlockedExchange(&q->sleeping, 0);
            continue;
        }
        while (tail != head) {
            BYTE *c = q->ring + ((UINT)tail & (Q_BYTES - 1));
            UINT size = ((Cmd *)c)->size;
            if (((Cmd *)c)->type != C_WRAP) Run(q->dev, c);
            tail += (LONG)size;
            COMPILER_BARRIER();
            q->tail = tail;
            if (q->waiting_room) SetEvent(q->room);
        }
    }
    return 0;
}

/* ---- control ---- */
BOOL NxQ_Start(NxDevice *dev)
{
    NxQueue *q;
    if (dev->q) return TRUE;
    q = (NxQueue *)calloc(1, sizeof *q);
    if (!q) return FALSE;
    q->dev = dev;
    q->spin = Processors() >= 4 ? 4000u : 0u;
    q->ring = (BYTE *)VirtualAlloc(NULL, Q_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    q->work = CreateEventA(NULL, FALSE, FALSE, NULL);
    q->room = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (q->ring && q->work && q->room) q->thread = CreateThread(NULL, 0, RenderThread, q, CREATE_SUSPENDED, &q->thread_id);
    if (!q->thread) {
        if (q->ring) VirtualFree(q->ring, 0, MEM_RELEASE);
        if (q->work) CloseHandle(q->work);
        if (q->room) CloseHandle(q->room);
        free(q);
        return FALSE;
    }
    g_rt_id = q->thread_id;
    dev->q = q;
    ResumeThread(q->thread);
    return TRUE;
}

void NxQ_Sync(NxDevice *dev)
{
    NxQueue *q = dev->q;
    if (!q || NxQ_OnRenderThread()) return;
    while (q->tail != q->head) {
        if (q->sleeping) SetEvent(q->work);
        InterlockedExchange(&q->waiting_room, 1);
        if (q->tail != q->head) WaitForSingleObject(q->room, 1);
        InterlockedExchange(&q->waiting_room, 0);
    }
    MemoryBarrier();
}

void NxQ_Stop(NxDevice *dev)
{
    NxQueue *q = dev->q;
    if (!q || NxQ_OnRenderThread()) return;
    NxQ_Sync(dev);
    InterlockedExchange(&q->stop, 1);
    SetEvent(q->work);
    WaitForSingleObject(q->thread, 5000);
    CloseHandle(q->thread);
    CloseHandle(q->work);
    CloseHandle(q->room);
    VirtualFree(q->ring, 0, MEM_RELEASE);
    dev->q = NULL;
    g_rt_id = 0;
    free(q);
}

/* end of a recorded frame: the main thread waits while it is more than Q_MAX_FRAMES ahead */
void NxQ_FrameEnd(NxDevice *dev)
{
    NxQueue *q = dev->q;
    BYTE *c;
    if (!q) return;
    InterlockedIncrement(&q->frames);
    c = Alloc(q, C_FRAME, sizeof(Cmd));
    Commit(q, c);
    MemoryBarrier();   /* pairs with the render thread's (it sets sleeping, then looks at head): the frame is never left asleep */
    if (q->sleeping) SetEvent(q->work);
    ++q_frames;
    if (q->frames > Q_MAX_FRAMES) ++q_frame_waits;
    if (!q_log_at) q_log_at = GetTickCount();
    if (GetTickCount() - q_log_at >= 5000 && q_frames) {
        Nx_Log("render thread: %.0f commands, %.0f KB a frame (%.0f KB in blobs); waits for room %.1f, for the render thread %.2f a frame",
               (double)q_cmds / q_frames, (double)q_bytes / 1024 / q_frames, (double)q_big / 1024 / q_frames, (double)q_room_waits / q_frames,
               (double)q_frame_waits / q_frames);
        q_bytes = q_cmds = q_room_waits = q_frame_waits = q_frames = q_big = 0;
        q_log_at = GetTickCount();
    }
    while (q->frames > Q_MAX_FRAMES) {
        InterlockedExchange(&q->waiting_room, 1);
        if (q->frames > Q_MAX_FRAMES) WaitForSingleObject(q->room, 2);
        InterlockedExchange(&q->waiting_room, 0);
    }
}

/* ---- recording ---- */
static BYTE *Rec(NxDevice *dev, UINT type, size_t args, size_t extra)
{
    return Alloc(dev->q, type, (UINT)(sizeof(Cmd) + ((args + 7u) & ~(size_t)7u) + extra));
}

static void Slots(NxDevice *dev, UINT type, UINT start, UINT n, void *const *v)
{
    BYTE *c;
    if (n > 128) n = 128;
    c = Rec(dev, type, sizeof(A_Slots), n * sizeof(void *));
    {
        ARGS(A_Slots);
        a->start = start; a->n = n;
        if (v) memcpy(Data(c, sizeof *a), v, n * sizeof *v); else memset(Data(c, sizeof *a), 0, n * sizeof(void *));
    }
    Commit(dev->q, c);
}

static void Obj(NxDevice *dev, UINT type, void *o, UINT xv)
{
    BYTE *c = Rec(dev, type, sizeof(A_ObjX), 0);
    ARGS(A_ObjX);
    a->o = o; a->x = xv;
    Commit(dev->q, c);
}

void NxC_PSSetShaderResources(NxDevice *dev, UINT s, UINT n, ID3D11ShaderResourceView *const *v)
{ if (Direct(dev)) ID3D11DeviceContext_PSSetShaderResources(dev->c, s, n, v); else Slots(dev, C_PS_SRV, s, n, (void *const *)v); }
void NxC_VSSetShaderResources(NxDevice *dev, UINT s, UINT n, ID3D11ShaderResourceView *const *v)
{ if (Direct(dev)) ID3D11DeviceContext_VSSetShaderResources(dev->c, s, n, v); else Slots(dev, C_VS_SRV, s, n, (void *const *)v); }
void NxC_PSSetConstantBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *v)
{ if (Direct(dev)) ID3D11DeviceContext_PSSetConstantBuffers(dev->c, s, n, v); else Slots(dev, C_PS_CB, s, n, (void *const *)v); }
void NxC_VSSetConstantBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *v)
{ if (Direct(dev)) ID3D11DeviceContext_VSSetConstantBuffers(dev->c, s, n, v); else Slots(dev, C_VS_CB, s, n, (void *const *)v); }
void NxC_PSSetSamplers(NxDevice *dev, UINT s, UINT n, ID3D11SamplerState *const *v)
{ if (Direct(dev)) ID3D11DeviceContext_PSSetSamplers(dev->c, s, n, v); else Slots(dev, C_PS_SMP, s, n, (void *const *)v); }
void NxC_VSSetShader(NxDevice *dev, ID3D11VertexShader *o, ID3D11ClassInstance *const *ci, UINT nci)
{ if (Direct(dev)) ID3D11DeviceContext_VSSetShader(dev->c, o, ci, nci); else Obj(dev, C_VS, o, 0); }
void NxC_PSSetShader(NxDevice *dev, ID3D11PixelShader *o, ID3D11ClassInstance *const *ci, UINT nci)
{ if (Direct(dev)) ID3D11DeviceContext_PSSetShader(dev->c, o, ci, nci); else Obj(dev, C_PS, o, 0); }
void NxC_RSSetState(NxDevice *dev, ID3D11RasterizerState *o)
{ if (Direct(dev)) ID3D11DeviceContext_RSSetState(dev->c, o); else Obj(dev, C_RS, o, 0); }
void NxC_OMSetDepthStencilState(NxDevice *dev, ID3D11DepthStencilState *o, UINT ref)
{ if (Direct(dev)) ID3D11DeviceContext_OMSetDepthStencilState(dev->c, o, ref); else Obj(dev, C_DEPTH, o, ref); }
void NxC_IASetPrimitiveTopology(NxDevice *dev, D3D11_PRIMITIVE_TOPOLOGY t)
{ if (Direct(dev)) ID3D11DeviceContext_IASetPrimitiveTopology(dev->c, t); else Obj(dev, C_TOPOLOGY, NULL, (UINT)t); }
void NxC_IASetInputLayout(NxDevice *dev, ID3D11InputLayout *o)
{ if (Direct(dev)) ID3D11DeviceContext_IASetInputLayout(dev->c, o); else Obj(dev, C_LAYOUT, o, 0); }
void NxC_Begin(NxDevice *dev, ID3D11Asynchronous *o)
{ if (Direct(dev)) ID3D11DeviceContext_Begin(dev->c, o); else Obj(dev, C_BEGIN, o, 0); }
void NxC_End(NxDevice *dev, ID3D11Asynchronous *o)
{ if (Direct(dev)) ID3D11DeviceContext_End(dev->c, o); else Obj(dev, C_END, o, 0); }

void NxC_RSSetViewports(NxDevice *dev, UINT n, const D3D11_VIEWPORT *vp)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_RSSetViewports(dev->c, n, vp); return; }
    c = Rec(dev, C_VIEWPORTS, sizeof(A_Viewports), 0);
    {
        ARGS(A_Viewports);
        if (n > 2) n = 2;
        a->n = n;
        memcpy(a->vp, vp, n * sizeof *vp);
    }
    Commit(dev->q, c);
}

void NxC_OMSetRenderTargets(NxDevice *dev, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_OMSetRenderTargets(dev->c, n, rtv, dsv); return; }
    c = Rec(dev, C_TARGETS, sizeof(A_Targets), 0);
    {
        ARGS(A_Targets);
        if (n > 8) n = 8;
        a->n = n;
        if (rtv) memcpy(a->rtv, rtv, n * sizeof *rtv); else memset(a->rtv, 0, sizeof a->rtv);
        a->dsv = dsv;
    }
    Commit(dev->q, c);
}

void NxC_OMSetBlendState(NxDevice *dev, ID3D11BlendState *o, const FLOAT factor[4], UINT mask)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_OMSetBlendState(dev->c, o, factor, mask); return; }
    c = Rec(dev, C_BLEND, sizeof(A_Blend), 0);
    {
        ARGS(A_Blend);
        a->o = o; a->has_factor = factor != NULL; a->mask = mask;
        if (factor) memcpy(a->factor, factor, sizeof a->factor);
    }
    Commit(dev->q, c);
}

void NxC_IASetVertexBuffers(NxDevice *dev, UINT s, UINT n, ID3D11Buffer *const *b, const UINT *stride, const UINT *offset)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_IASetVertexBuffers(dev->c, s, n, b, stride, offset); return; }
    c = Rec(dev, C_VB, sizeof(A_VB), 0);
    {
        ARGS(A_VB);
        if (n > 16) n = 16;
        a->start = s; a->n = n;
        memcpy(a->b, b, n * sizeof *b);
        memcpy(a->stride, stride, n * sizeof *stride);
        memcpy(a->offset, offset, n * sizeof *offset);
    }
    Commit(dev->q, c);
}

void NxC_IASetIndexBuffer(NxDevice *dev, ID3D11Buffer *b, DXGI_FORMAT f, UINT off)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_IASetIndexBuffer(dev->c, b, f, off); return; }
    c = Rec(dev, C_IB, sizeof(A_IB), 0);
    { ARGS(A_IB); a->b = b; a->f = f; a->off = off; }
    Commit(dev->q, c);
}

static void DrawRec(NxDevice *dev, UINT type, UINT x1, UINT x2, UINT x3, INT base, UINT x4)
{
    BYTE *c = Rec(dev, type, sizeof(A_Draw), 0);
    ARGS(A_Draw);
    a->a = x1; a->b = x2; a->c = x3; a->d = x4; a->base = base;
    Commit(dev->q, c);
}
void NxC_Draw(NxDevice *dev, UINT n, UINT start)
{ if (Direct(dev)) ID3D11DeviceContext_Draw(dev->c, n, start); else DrawRec(dev, C_DRAW, n, start, 0, 0, 0); }
void NxC_DrawIndexed(NxDevice *dev, UINT n, UINT start, INT base)
{ if (Direct(dev)) ID3D11DeviceContext_DrawIndexed(dev->c, n, start, base); else DrawRec(dev, C_DRAW_INDEXED, n, start, 0, base, 0); }
void NxC_DrawIndexedInstanced(NxDevice *dev, UINT n, UINT inst, UINT start, INT base, UINT first)
{ if (Direct(dev)) ID3D11DeviceContext_DrawIndexedInstanced(dev->c, n, inst, start, base, first); else DrawRec(dev, C_DRAW_INSTANCED, n, inst, start, base, first); }

/* bytes: how much of data the copy reads (the caller knows its rows) */
void NxC_UpdateSubresource(NxDevice *dev, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT pitch, UINT depth_pitch,
                           UINT bytes)
{
    BYTE *c;
    BOOL blob = bytes > Q_BLOB;
    if (Direct(dev)) { ID3D11DeviceContext_UpdateSubresource(dev->c, r, sub, box, data, pitch, depth_pitch); return; }
    c = Rec(dev, C_UPDATE, sizeof(A_Update), blob ? 0 : bytes);
    {
        ARGS(A_Update);
        a->res = r; a->sub = sub; a->has_box = box != NULL; if (box) a->box = *box;
        a->pitch = pitch; a->depth_pitch = depth_pitch; a->bytes = bytes;
        a->blob = NULL;
        if (blob) { a->blob = (BYTE *)malloc(bytes); if (a->blob) memcpy(a->blob, data, bytes); else ((Cmd *)c)->type = C_WRAP; }   /* no memory: dropped */
        else memcpy(Data(c, sizeof *a), data, bytes);
    }
    Commit(dev->q, c);
}

void NxC_CopySubresourceRegion(NxDevice *dev, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub,
                               const D3D11_BOX *box)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_CopySubresourceRegion(dev->c, dst, dsub, x, y, z, src, ssub, box); return; }
    c = Rec(dev, C_COPY, sizeof(A_Copy), 0);
    {
        ARGS(A_Copy);
        a->dst = dst; a->dsub = dsub; a->x = x; a->y = y; a->z = z; a->src = src; a->ssub = ssub;
        a->has_box = box != NULL; if (box) a->box = *box;
    }
    Commit(dev->q, c);
}

void NxC_ResolveSubresource(NxDevice *dev, ID3D11Resource *dst, UINT dsub, ID3D11Resource *src, UINT ssub, DXGI_FORMAT f)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_ResolveSubresource(dev->c, dst, dsub, src, ssub, f); return; }
    c = Rec(dev, C_RESOLVE, sizeof(A_Resolve), 0);
    { ARGS(A_Resolve); a->dst = dst; a->dsub = dsub; a->src = src; a->ssub = ssub; a->f = f; }
    Commit(dev->q, c);
}

void NxC_ClearRenderTargetView(NxDevice *dev, ID3D11RenderTargetView *v, const FLOAT color[4])
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_ClearRenderTargetView(dev->c, v, color); return; }
    c = Rec(dev, C_CLEAR_RTV, sizeof(A_Clear), 0);
    { ARGS(A_Clear); a->v = v; memcpy(a->color, color, sizeof a->color); }
    Commit(dev->q, c);
}

void NxC_ClearDepthStencilView(NxDevice *dev, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil)
{
    BYTE *c;
    if (Direct(dev)) { ID3D11DeviceContext_ClearDepthStencilView(dev->c, v, flags, depth, stencil); return; }
    c = Rec(dev, C_CLEAR_DSV, sizeof(A_Clear), 0);
    { ARGS(A_Clear); a->v = v; a->flags = flags; a->depth = depth; a->stencil = stencil; }
    Commit(dev->q, c);
}

/* Map (mode), copy bytes to offset, Unmap: a dynamic buffer's new contents */
void NxC_WriteBuffer(NxDevice *dev, ID3D11Buffer *b, D3D11_MAP mode, UINT offset, const void *data, UINT bytes)
{
    BYTE *c;
    BOOL blob = bytes > Q_BLOB;
    if (Direct(dev)) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(dev->c, (ID3D11Resource *)b, 0, mode, 0, &m))) {
            memcpy((BYTE *)m.pData + offset, data, bytes);
            ID3D11DeviceContext_Unmap(dev->c, (ID3D11Resource *)b, 0);
        }
        return;
    }
    c = Rec(dev, C_WRITE, sizeof(A_Write), blob ? 0 : bytes);
    {
        ARGS(A_Write);
        a->b = b; a->mode = mode; a->offset = offset; a->bytes = bytes; a->blob = NULL;
        if (blob) { q_big += bytes; a->blob = (BYTE *)malloc(bytes); if (a->blob) memcpy(a->blob, data, bytes); else ((Cmd *)c)->type = C_WRAP; }
        else memcpy(Data(c, sizeof *a), data, bytes);
    }
    Commit(dev->q, c);
}

/* a Direct3D 11 object the main thread is done with: released after the queued commands that may use it */
void NxC_Release(NxDevice *dev, void *o)
{
    BYTE *c;
    if (!o) return;
    if (!dev || Direct(dev)) { IUnknown_Release((IUnknown *)o); return; }
    c = Rec(dev, C_RELEASE, sizeof(A_Obj), 0);
    { ARGS(A_Obj); a->o = o; }
    Commit(dev->q, c);
}

/* fn(dev, copy of arg) on the render thread (or now, without one) */
void NxC_Call(NxDevice *dev, void (*fn)(NxDevice *, void *), const void *arg, UINT bytes)
{
    BYTE *c;
    if (Direct(dev)) {
        BYTE tmp[256];
        if (bytes > sizeof tmp) bytes = sizeof tmp;
        if (bytes) memcpy(tmp, arg, bytes);
        fn(dev, tmp);
        return;
    }
    c = Rec(dev, C_CALL, sizeof(A_Call), bytes);
    { ARGS(A_Call); a->fn = fn; a->bytes = bytes; if (bytes) memcpy(Data(c, sizeof *a), arg, bytes); }
    Commit(dev->q, c);
}
