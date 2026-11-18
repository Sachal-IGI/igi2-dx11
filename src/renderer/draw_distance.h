/* [Renderer] DrawDistance and DetailDistance, in percent of the game's own distances (100, the default, is the game).
 * Never below 100, so they only ever add cover, and characters (bone meshes) keep the game's distances, so nobody sees
 * players further than anyone else. Game-side, so they work with both renderers. Every site is checked byte for byte
 * first (all or nothing) and restored on unload.
 *
 * DrawDistance R (100..400): how far bushes, trees and objects stay visible. Only the cutoffs move; past the game's
 * cutoff a model is drawn with its last (cheapest) level of detail, so the cost stays low.
 *   Forest_UpdateInternalDataHandler 0x5CBEBE: a forest's view cutoff (m) times 4096 uses 4096 R (its cutoff planes).
 *   ModelObj_GetLOD (rigid meshes, forests without high quality, sector objects): the last-LOD cutoff at 0x589D21
 *     (scale * model cutoff) and the rigid cutoff test at 0x589C8D (cutoff^2 against the distance^2) become calls that
 *     also multiply by R and R^2. The rigid LOD steps keep the game's spacing.
 *   LODSettings handler 0x44029D: the spline mesh cutoff uses 0.02 R; the game's default (10 at 0x6D3730) is scaled too.
 * DetailDistance D (100..300): how far the detailed levels of detail reach (costs more).
 *   ForestRender_Draw 0x59AC8F / 0x59AC9F: ForestRender_vLODBiasSquared = (bias * 16 + 1)^2 is made with 16/D and 1/D.
 *   Config_SetGameToCurrentPerformanceLevel 0x427C41 / 0x427C47: ModelObj_vActualLODBiasSquared = (bias * 4 + 1)^2 is
 *     made with 4/D and 1/D (the current value is divided by D^2 at once). ModelObj_vLODBias (characters) is unchanged.
 *   Direct3D 11 terrain (dx11_terrain.c): full-detail radius and material detail D times further (at most 400 cells).
 * Included by renderer.c (uses its WriteCode). */
#ifndef NX_DRAW_DISTANCE_H
#define NX_DRAW_DISTANCE_H

float nx_draw_distance = 1.0f, nx_detail_distance = 1.0f;

typedef struct DD_OPERAND {
    uint32_t ea;            /* the instruction: two opcode bytes, then the absolute address of a float */
    unsigned char op[2];
    uint32_t addr;          /* the game's float (default base) */
    float *mine;
    int detail;             /* 0: DrawDistance, 1: DetailDistance */
    int done;
} DD_OPERAND;
typedef struct DD_CALL {
    uint32_t ea;
    unsigned char orig[8];
    unsigned size;          /* 7 or 8: a 5-byte call and nops */
    unsigned stub;          /* offset of its target in dd_stub */
    unsigned char now[8];
    int done;
} DD_CALL;

static float dd_forest_cut = 4096.0f, dd_spline_k = 0.02f, dd_cut = 1.0f, dd_cut2 = 1.0f;
static float dd_fbias_mul = 16.0f, dd_fbias_add = 1.0f, dd_obias_mul = 4.0f, dd_obias_add = 1.0f;
static DD_OPERAND dd_ops[] = {
    {0x5CBEBEu, {0xD8, 0x0D}, 0x6BF5E0u, &dd_forest_cut, 0, 0},  /* fmul [4096.0]: forest view cutoff */
    {0x44029Du, {0xD8, 0x0D}, 0x6BF94Cu, &dd_spline_k, 0, 0},    /* fmul [0.02]: spline mesh cutoff (LODSettings) */
    {0x59AC8Fu, {0xD8, 0x0D}, 0x6BFBD8u, &dd_fbias_mul, 1, 0},   /* fmul [16.0]: forest LOD bias */
    {0x59AC9Fu, {0xD8, 0x05}, 0x6BF54Cu, &dd_fbias_add, 1, 0},   /* fadd [1.0]: forest LOD bias */
    {0x427C41u, {0xD8, 0x0D}, 0x6BF598u, &dd_obias_mul, 1, 0},   /* fmul [4.0]: object LOD bias */
    {0x427C47u, {0xD8, 0x05}, 0x6BF54Cu, &dd_obias_add, 1, 0}    /* fadd [1.0]: object LOD bias */
};
static DD_CALL dd_calls[] = {
    {0x589D21u, {0xD9, 0x44, 0x24, 0x0C, 0xD8, 0x4F, 0x38}, 7, 0, {0}, 0},          /* fld [esp+0Ch]; fmul [edi+38h] */
    {0x589C8Du, {0xD9, 0xC0, 0xD8, 0xC9, 0xD8, 0x5C, 0x24, 0x08}, 8, 32, {0}, 0}    /* fld st; fmul st,st1; fcomp [esp+8] */
};
#define DD_NOPS (sizeof dd_ops / sizeof dd_ops[0])
#define DD_NCALLS (sizeof dd_calls / sizeof dd_calls[0])
#define DD_SPLINE_DEFAULT 0x6D3730u
#define DD_OBJ_BIAS_SQ 0x7ADC878u
static BYTE *dd_stub;
static int dd_spline_done, dd_bias_done;
static UINT dd_reloc;

static int DD_Percent(HMODULE self, const char *key, int high)
{
    char ini[MAX_PATH + 32], *slash;
    int p;
    if (!self || !GetModuleFileNameA(self, ini, MAX_PATH)) return 100;
    slash = strrchr(ini, 92);   /* the DLL's folder (92 = backslash) */
    if (!slash) return 100;
    snprintf(slash + 1, sizeof ini - (size_t)(slash + 1 - ini), "igi2_unified.ini");
    p = (int)GetPrivateProfileIntA("Renderer", key, 100, ini);
    return p < 100 ? 100 : p > high ? high : p;
}

static BOOL DD_Scale(uint32_t ea, float factor)
{
    float *v = (float *)(uintptr_t)(ea + dd_reloc);
    DWORD old, unused;
    if (!VirtualProtect(v, 4, PAGE_READWRITE, &old)) return FALSE;
    if (*v > 0) *v *= factor;
    VirtualProtect(v, 4, old, &unused);
    return TRUE;
}

/* Restores what was written; refuses a site that no longer holds our bytes (someone else changed it). */
static BOOL DD_Uninstall(void)
{
    BOOL ok = TRUE;
    unsigned i;
    for (i = 0; i < DD_NCALLS; ++i) {
        void *site = (void *)(uintptr_t)(dd_calls[i].ea + dd_reloc);
        if (!dd_calls[i].done) continue;
        if (memcmp(site, dd_calls[i].now, dd_calls[i].size) || !WriteCode(site, dd_calls[i].orig, dd_calls[i].size)) ok = FALSE;
        else dd_calls[i].done = 0;
    }
    for (i = 0; i < DD_NOPS; ++i) {
        BYTE *p = (BYTE *)(uintptr_t)(dd_ops[i].ea + dd_reloc) + 2;
        uint32_t orig = dd_ops[i].addr + dd_reloc, mine = (uint32_t)(uintptr_t)dd_ops[i].mine, have;
        if (!dd_ops[i].done) continue;
        memcpy(&have, p, 4);
        if (have != mine || !WriteCode(p, &orig, 4)) ok = FALSE;
        else dd_ops[i].done = 0;
    }
    if (dd_spline_done && DD_Scale(DD_SPLINE_DEFAULT, 1.0f / nx_draw_distance)) dd_spline_done = 0;
    if (dd_bias_done && DD_Scale(DD_OBJ_BIAS_SQ, nx_detail_distance * nx_detail_distance)) dd_bias_done = 0;
    if (ok) nx_draw_distance = nx_detail_distance = 1.0f;
    return ok;   /* the call targets (dd_stub) stay allocated: the game may still be inside one */
}

static void DD_Install(HMODULE self, UINT reloc)
{
    int pr = DD_Percent(self, "DrawDistance", 400), pd = DD_Percent(self, "DetailDistance", 300);
    float r = (float)pr / 100.0f, d = (float)pd / 100.0f;
    int want[2], bad = 0;
    unsigned i;
    char text[224];
    dd_reloc = reloc;
    want[0] = pr > 100;
    want[1] = pd > 100;
    if (!want[0] && !want[1]) { Renderer_Log("draw distance: the game's own (DrawDistance=100, DetailDistance=100)"); return; }
    for (i = 0; i < DD_NOPS; ++i) {
        const BYTE *p = (const BYTE *)(uintptr_t)(dd_ops[i].ea + reloc);
        uint32_t have;
        if (!want[dd_ops[i].detail]) continue;
        memcpy(&have, p + 2, 4);
        if (p[0] != dd_ops[i].op[0] || p[1] != dd_ops[i].op[1] || have != dd_ops[i].addr + reloc) bad = 1;
    }
    if (want[0])
        for (i = 0; i < DD_NCALLS; ++i)
            if (memcmp((const void *)(uintptr_t)(dd_calls[i].ea + reloc), dd_calls[i].orig, dd_calls[i].size)) bad = 1;
    if (bad) {
        Renderer_Log("draw distance: the game code differs (another patch?): the game's own distances");
        return;
    }
    dd_forest_cut = 4096.0f * r;
    dd_spline_k = 0.02f * r;
    dd_cut = r;
    dd_cut2 = r * r;
    dd_fbias_mul = 16.0f / d;
    dd_fbias_add = 1.0f / d;
    dd_obias_mul = 4.0f / d;
    dd_obias_add = 1.0f / d;
    if (want[0]) {
        uint32_t a1 = (uint32_t)(uintptr_t)&dd_cut, a2 = (uint32_t)(uintptr_t)&dd_cut2;
        /* +0:  fld dword [esp+10h]; fmul dword [edi+38h]; fmul dword [dd_cut]; ret   (the scale is 4 further up: the call)
           +32: fld st; fmul st,st1; fmul dword [dd_cut2]; fcomp dword [esp+0Ch]; ret (fnstsw after the call reads it) */
        BYTE a[14] = {0xD9, 0x44, 0x24, 0x10, 0xD8, 0x4F, 0x38, 0xD8, 0x0D, 0, 0, 0, 0, 0xC3};
        BYTE b[15] = {0xD9, 0xC0, 0xD8, 0xC9, 0xD8, 0x0D, 0, 0, 0, 0, 0xD8, 0x5C, 0x24, 0x0C, 0xC3};
        if (!dd_stub) dd_stub = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!dd_stub) { Renderer_Log("draw distance: no memory for the LOD calls: the game's own distances"); return; }
        memcpy(a + 9, &a1, 4);
        memcpy(b + 6, &a2, 4);
        memcpy(dd_stub, a, sizeof a);
        memcpy(dd_stub + 32, b, sizeof b);
        FlushInstructionCache(GetCurrentProcess(), dd_stub, 64);
        for (i = 0; i < DD_NCALLS; ++i) {
            int32_t rel = (int32_t)((uintptr_t)(dd_stub + dd_calls[i].stub) - ((uintptr_t)(dd_calls[i].ea + reloc) + 5));
            dd_calls[i].now[0] = 0xE8;
            memcpy(dd_calls[i].now + 1, &rel, 4);
            memset(dd_calls[i].now + 5, 0x90, dd_calls[i].size - 5);
        }
    }
    for (i = 0; i < DD_NOPS && !bad; ++i) {
        uint32_t a = (uint32_t)(uintptr_t)dd_ops[i].mine;
        if (!want[dd_ops[i].detail]) continue;
        if (!WriteCode((BYTE *)(uintptr_t)(dd_ops[i].ea + reloc) + 2, &a, 4)) bad = 1;
        else dd_ops[i].done = 1;
    }
    for (i = 0; i < DD_NCALLS && want[0] && !bad; ++i) {
        if (!WriteCode((void *)(uintptr_t)(dd_calls[i].ea + reloc), dd_calls[i].now, dd_calls[i].size)) bad = 1;
        else dd_calls[i].done = 1;
    }
    if (bad) {
        DD_Uninstall();
        Renderer_Log("draw distance: a write failed: the game's own distances");
        return;
    }
    if (want[0]) { nx_draw_distance = r; dd_spline_done = DD_Scale(DD_SPLINE_DEFAULT, r); }
    if (want[1]) { nx_detail_distance = d; dd_bias_done = DD_Scale(DD_OBJ_BIAS_SQ, 1.0f / (d * d)); }
    snprintf(text, sizeof text, "draw distance %d%%, detail distance %d%%: bushes, trees and objects visible %.2fx as far, "
             "detailed models and terrain %.2fx as far (characters unchanged)", pr, pd, (double)r, (double)d);
    Renderer_Log(text);
}
#endif
