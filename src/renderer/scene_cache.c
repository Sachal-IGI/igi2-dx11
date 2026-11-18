/* Exact reuse of the view-test setup in Mesh3D_ExcludeObject (0x5A7A10), the game's per-object view test.
   Before it tests an object it turns the clip window and the field of view into four view-plane slopes and their
   cosines (four atan2 and four cos on the x87) whenever the active transform context changed (TransContext_
   SetActiveTransContext counts 0x8368C0C up) or the render context's time stamp moved. The game sets a transform
   context for every object it draws, so this ran for nearly every object: about 8 % of the game thread looking at
   the buildings of Chinese Temple. The eight results depend only on the clip window (0x853D698/69C, 0x853D6A8..6BC),
   the field of view (0x8409000/004) and the FPU control word, and only this function writes them (Mesh3D_Open sets
   them at load). When the inputs match one of the last 8 setups it computed (the game switches between several
   views), the eight values computed from them then are written back and the function's own "up to date" pair
   (0x8368C00 = 0x8368C0C, 0x8368BFC = time stamp) is set: it goes on with them without computing them again.
   Inputs are compared as bit patterns and no FPU instruction runs here, so the values are the same to the last bit;
   a dev check that let it compute anyway found 0 differences. */
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXCLUDE_ADDR 0x5A7A10u
#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))

typedef int (__cdecl *ExcludeFn)(int, int, int);

static const uintptr_t kIn[10] = {0x853D698, 0x853D69C, 0x853D6A8, 0x853D6AC, 0x853D6B0, 0x853D6B4, 0x853D6B8, 0x853D6BC,
                                  0x8409000, 0x8409004};
static const uintptr_t kOut[8] = {0x84AA4A0, 0x84AA63C, 0x84AA4C0, 0x846AB08, 0x84AA620, 0x846AB04, 0x84A5500, 0x846AB64};
static BYTE *g_tramp;
/* the game switches between several views (the main view, the weapon, others), each its own clip window / field of
   view: one stored setup missed at every switch, so the last 8 are kept */
#define SETUPS 8
typedef struct Setup { uint32_t in[11], out[8]; int used; } Setup;
static Setup g_setup[SETUPS];
static unsigned g_next, g_last;
static unsigned g_reused, g_computed;
static DWORD g_log_at;

static void ReadIn(uint32_t *in)
{
    unsigned short cw;
    unsigned i;
    for (i = 0; i < 10; ++i) in[i] = U32(kIn[i]);
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    in[10] = cw;
}

static void ReadOut(uint32_t *out)
{
    unsigned i;
    for (i = 0; i < 8; ++i) out[i] = U32(kOut[i]);
}

static void WriteOut(const uint32_t *out)
{
    unsigned i;
    for (i = 0; i < 8; ++i) U32(kOut[i]) = out[i];
}

static int __cdecl Exclude(int a, int b, int c)
{
    int stale = U32(0x8368C00) != U32(0x8368C0C) || U32(0x8368BFC) != U32(0x8368768), r;
    uint32_t in[11];
    if (stale) {
        Setup *s = NULL;
        unsigned i;
        ReadIn(in);
        if (g_setup[g_last].used && !memcmp(in, g_setup[g_last].in, sizeof in)) s = &g_setup[g_last];
        else for (i = 0; i < SETUPS; ++i)
            if (g_setup[i].used && !memcmp(in, g_setup[i].in, sizeof in)) { s = &g_setup[i]; g_last = i; break; }
        if (s) {   /* the values it would compute from these inputs */
            WriteOut(s->out);
            U32(0x8368C00) = U32(0x8368C0C);
            U32(0x8368BFC) = U32(0x8368768);
            stale = 0;
            ++g_reused;
        }
    }
    r = ((ExcludeFn)(void *)g_tramp)(a, b, c);
    if (stale) {   /* the function computed them from these inputs */
        Setup *s = &g_setup[g_next];
        memcpy(s->in, in, sizeof s->in);
        ReadOut(s->out);
        s->used = 1;
        g_last = g_next;
        g_next = (g_next + 1) % SETUPS;
        ++g_computed;
        if (GetTickCount() - g_log_at >= 30000) {
            char line[160];
            g_log_at = GetTickCount();
            snprintf(line, sizeof line, "view test setup in 30 s: %u reused, %u computed", g_reused, g_computed);
            Renderer_Log(line);
            g_reused = g_computed = 0;
        }
    }
    return r;
}

void NxScene_Install(void)
{
    static const BYTE expect[14] = {0x8B, 0x0D, 0x00, 0x8C, 0x36, 0x08, 0xA1, 0x0C, 0x8C, 0x36, 0x08, 0x83, 0xEC, 0x10};
    BYTE *site = (BYTE *)(uintptr_t)EXCLUDE_ADDR, jump[6];
    DWORD old;
    int32_t rel;
    if (memcmp(site, expect, sizeof expect)) { Renderer_Log("view test setup: not the 1.3 code, left alone"); return; }
    g_tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tramp) return;
    memcpy(g_tramp, site, 6);   /* mov ecx, [0x8368C00]: an absolute address, runs anywhere */
    g_tramp[6] = 0xE9;
    rel = (int32_t)((EXCLUDE_ADDR + 6) - ((uintptr_t)g_tramp + 11));
    memcpy(g_tramp + 7, &rel, 4);
    if (!VirtualProtect(g_tramp, 16, PAGE_EXECUTE_READ, &old)) { VirtualFree(g_tramp, 0, MEM_RELEASE); g_tramp = NULL; return; }
    FlushInstructionCache(GetCurrentProcess(), g_tramp, 16);
    jump[0] = 0xE9;
    rel = (int32_t)((uintptr_t)Exclude - (EXCLUDE_ADDR + 5));
    memcpy(jump + 1, &rel, 4);
    jump[5] = 0x90;
    if (!Renderer_WriteCode(site, jump, sizeof jump)) { Renderer_Log("view test setup: cannot patch"); return; }
    g_log_at = GetTickCount();
    Renderer_Log("view test setup: reused while the clip window and field of view stay the same");
}
