/* modules/renderer: the Neonix renderer's way into igi2.exe 1.3.
   Direct3D_Open (0x44A010) loads D3D8.DLL, stores GetProcAddress("Direct3DCreate8") at 0x757C04 and calls 0x449FA0, the first
   user of that pointer (SetMode 0x44A564 and the mode lists 0x44B3CD/0x44C808 use it later). The call at 0x44A071 goes through
   Renderer_BeforeFirstCreate, which puts Nx_Direct3DCreate8 in the pointer. A dedicated server never opens Direct3D.
   Modes: off (the original Direct3D 8, nothing patched) and trace (file nx_d3d8_trace.on in the game folder: every Direct3D 8
   call is counted to plugins\nx_d3d8_trace.txt and shaders are written to plugins\nx_trace; the picture is the original). */
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IGI2_DEFAULT_BASE 0x00400000u
#define OPEN_FIRST_CREATE_CALL 0x0044A071u /* E8 2A FF FF FF  call 0x449FA0 */
#define FIRST_CREATE 0x00449FA0u
#define CREATE8_POINTER 0x00757C04u

typedef struct PATCHMODULEINFO { DWORD dwDefBaseAddr; DWORD dwCurBaseAddr; UINT uiRelocValue; } PATCHMODULEINFO;
typedef UINT (__cdecl *PMGetCurVersionID_fn)(void);
typedef PATCHMODULEINFO *(__cdecl *PMGetCurPMIHandle_fn)(void);

enum { MODE_OFF, MODE_TRACE, MODE_DX11 };
static const BYTE kOriginalCall[5] = {0xE8, 0x2A, 0xFF, 0xFF, 0xFF};
static BYTE g_call[5];
static UINT g_reloc;
static int g_mode;
static BOOL g_trace_dx11;
static BOOL g_measure;       /* nx_measure.on: frame time stats in the log */
BOOL nx_trace_quiet;         /* nx_d3d8_measure.on: the trace layer passes calls on without counting them */
static BOOL g_patched;
static volatile LONG g_status;
static char g_folder[MAX_PATH];
static int (__cdecl *g_first_create)(void);
IDirect3D8 *(WINAPI *nx_real_create8)(UINT);

void Renderer_Log(const char *text) { Unified_Log("renderer", text); }
const char *Renderer_GameFolder(void) { return g_folder; }

/* dev test: nx_shot.req in the game folder holds "<frame> <bmp path>"; the first presented frame from that number on is
   written there (both the original Direct3D 8 through the trace, and Direct3D 11), then the request is removed */
BOOL Renderer_ShotRequest(unsigned frame, char *path, size_t cap)
{
    static unsigned next_check;
    static DWORD first_tick, last_check;
    char req[MAX_PATH + 32], text[MAX_PATH + 32];
    unsigned at = 0;
    FILE *f;
    size_t n;
    int used = 0;
    if (!first_tick) first_tick = GetTickCount();
    if (frame < next_check || GetTickCount() - last_check < 250) return FALSE;   /* a file check: 4 times a second */
    next_check = frame + 8;
    last_check = GetTickCount();
    snprintf(req, sizeof req, "%s\\nx_shot.req", g_folder);
    f = fopen(req, "rb");
    if (!f) return FALSE;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ')) --n;
    text[n] = 0;
    /* "<frame> <path>", or "t<ms> <path>": milliseconds after the first frame */
    if (text[0] == 't') {
        if (sscanf(text + 1, "%u %n", &at, &used) < 1 || !text[1 + used]) return FALSE;
        if (GetTickCount() - first_tick < at) return FALSE;
        ++used;
    } else {
        if (sscanf(text, "%u %n", &at, &used) < 1 || !text[used]) return FALSE;
        if (frame < at) return FALSE;
    }
    DeleteFileA(req);
    snprintf(path, cap, "%s", text + used);
    return TRUE;
}

volatile int nx_dev_ab, nx_dev_ab_on;
__declspec(dllimport) BOOL WINAPI QueryThreadCycleTime(HANDLE thread, PULONG64 cycles);   /* Vista+; hidden by the SDK version macro */

/* frame times (both renderers): every 5 s the average FPS and the 50/95/99th percentile frame time of that span */
void Renderer_FrameTick(void)
{
    static LARGE_INTEGER freq, last, span_start;
    static float ms[4096];
    static unsigned n, frames, hitches;
    static double hitch_ms;
    static ULONG64 cyc_start;
    LARGE_INTEGER now;
    float t;
    if (!g_measure) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (!last.QuadPart) { last = span_start = now; return; }
    t = (float)((double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart);
    if (n < 4096) ms[n++] = t;
    if (t > 50) { ++hitches; hitch_ms += t; }
    ++frames;
    last = now;
    if ((double)(now.QuadPart - span_start.QuadPart) >= 5.0 * (double)freq.QuadPart) {
        char text[320];
        unsigned i, j;
        double secs = (double)(now.QuadPart - span_start.QuadPart) / (double)freq.QuadPart;
        for (i = 1; i < n; ++i) { float v = ms[i]; for (j = i; j > 0 && ms[j - 1] > v; --j) ms[j] = ms[j - 1]; ms[j] = v; }
        {
            /* the game thread's own CPU cycles a frame: unlike frame times, barely moved by other programs' load */
            ULONG64 cyc = 0;
            char tail[48] = "";
            QueryThreadCycleTime(GetCurrentThread(), &cyc);
            if (nx_dev_ab_on) snprintf(tail, sizeof tail, " [ab %d]", nx_dev_ab);
            snprintf(text, sizeof text, "frames: %.1f FPS, frame ms p50 %.2f p95 %.2f p99 %.2f max %.2f (%u frames; %u over 50 ms, %.0f ms in them), game thread %.0f kcycles a frame%s",
                     frames / secs, n ? ms[n / 2] : 0.0, n ? ms[n * 95 / 100] : 0.0, n ? ms[n * 99 / 100] : 0.0, n ? ms[n - 1] : 0.0,
                     frames, hitches, hitch_ms, frames && cyc_start ? (double)(cyc - cyc_start) / 1000.0 / frames : 0.0, tail);
            cyc_start = cyc;
        }
        Renderer_Log(text);
        n = frames = hitches = 0;
        hitch_ms = 0;
        span_start = now;
        /* dev A/B: nx_ab.txt in the game folder holds a number that code under test reads (nx_dev_ab) for the next
           span, so old and new code run on the same view in one run */
        {
            char path[MAX_PATH + 16];
            FILE *f;
            int v = 0;
            snprintf(path, sizeof path, "%s\\nx_ab.txt", g_folder);
            if ((f = fopen(path, "r")) != NULL) {
                if (fscanf(f, "%d", &v) != 1) v = 0;
                fclose(f);
                nx_dev_ab_on = 1;
            }
            nx_dev_ab = v;
        }
    }
}

/* dev: the igi2.exe return addresses on this thread's stack (a value right after a call instruction), outermost last */
void Renderer_LogCallers(const char *tag)
{
    char text[600];
    size_t at;
    const DWORD *sp = (const DWORD *)__builtin_frame_address(0);
    unsigned i, n = 0;
    at = (size_t)snprintf(text, sizeof text, "%s:", tag);
    for (i = 0; i < 4096 && n < 24 && at < sizeof text - 12; ++i) {
        DWORD v;
        const BYTE *p;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(sp + i, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
        v = sp[i];
        if (v < 0x401000u + g_reloc || v >= 0x681000u + g_reloc) continue;
        p = (const BYTE *)(uintptr_t)v;
        if (p[-5] != 0xE8 && !(p[-6] == 0xFF && p[-5] == 0x15) && !(p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) && !(p[-3] == 0xFF && (p[-2] & 0x38) == 0x10))
            continue;
        at += (size_t)snprintf(text + at, sizeof text - at, " %06lx", (unsigned long)(v - g_reloc));
        ++n;
    }
    Renderer_Log(text);
}

/* 32-bit B8G8R8x8 or 16-bit R5G6B5 pixels, top row first, to a 24-bit BMP */
BOOL Renderer_WriteBMP(const char *path, unsigned w, unsigned h, const BYTE *pixels, unsigned pitch, unsigned bpp)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    unsigned y, row = w * 3, pad = (4 - row % 4) % 4;
    BYTE *line;
    FILE *f;
    if (!pixels || !w || !h || (bpp != 4 && bpp != 2)) return FALSE;
    line = (BYTE *)malloc(row + 4);
    if (!line) return FALSE;
    f = fopen(path, "wb");
    if (!f) { free(line); return FALSE; }
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (row + pad) * h;
    ih.biSize = sizeof ih; ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    for (y = h; y-- > 0;) {
        unsigned x;
        const BYTE *src = pixels + (size_t)y * pitch;
        for (x = 0; x < w; ++x) {
            if (bpp == 4) { line[3 * x] = src[4 * x]; line[3 * x + 1] = src[4 * x + 1]; line[3 * x + 2] = src[4 * x + 2]; }
            else {
                WORD v = ((const WORD *)src)[x];
                line[3 * x] = (BYTE)((v & 31) << 3); line[3 * x + 1] = (BYTE)(((v >> 5) & 63) << 2); line[3 * x + 2] = (BYTE)((v >> 11) << 3);
            }
        }
        memset(line + row, 0, pad);
        fwrite(line, 1, row + pad, f);
    }
    fclose(f);
    free(line);
    return TRUE;
}

static BOOL WriteCode(void *at, const void *bytes, SIZE_T size)
{
    DWORD old, unused;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(at, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return VirtualProtect(at, size, old, &unused);
}
BOOL Renderer_WriteCode(void *at, const void *bytes, SIZE_T size) { return WriteCode(at, bytes, size); }
#include "draw_distance.h"


static IDirect3D8 *WINAPI Nx_Direct3DCreate8(UINT sdk)
{
    IDirect3D8 *real;
    if (g_mode == MODE_DX11) {
        IDirect3D8 *ours = (IDirect3D8 *)Nx_CreateD3D();
        if (ours && g_trace_dx11) return (IDirect3D8 *)NxT_Wrap(NXT_D3D, ours);
        if (ours) return ours;
        g_mode = MODE_OFF;
        Renderer_Log("Direct3D 11 cannot start here: original Direct3D 8");
    }
    real = nx_real_create8 ? nx_real_create8(sdk) : NULL;
    if (!real || g_mode != MODE_TRACE) return real;
    NxT_DumpCaps(real);
    return (IDirect3D8 *)NxT_Wrap(NXT_D3D, real);
}

static int __cdecl Renderer_BeforeFirstCreate(void)
{
    void **slot = (void **)(uintptr_t)(CREATE8_POINTER + g_reloc);
    if (*slot && *slot != (void *)Nx_Direct3DCreate8) {
        nx_real_create8 = (IDirect3D8 *(WINAPI *)(UINT))*slot;
        *slot = (void *)Nx_Direct3DCreate8;
        Renderer_Log(g_mode == MODE_TRACE ? "trace: Direct3D 8 calls are counted to plugins\\nx_d3d8_trace.txt"
                                          : "Direct3DCreate8 taken over");
    }
    return g_first_create();
}

static HMODULE g_self;
void renderer_Initialize(HMODULE self) { g_self = self; }

/* plugins\igi2_unified.ini [Renderer]: Mode = dx11 (the default since 1.4) or dx8; VSync 1 (default) / 0 / -1 the game's own;
   Anisotropy = 1..16 (Direct3D 11) */
unsigned nx_anisotropy, nx_antialiasing, nx_alpha_to_coverage = 1;
int nx_vsync = -1;
static BOOL IniWantsDX11(void)
{
    char ini[MAX_PATH + 32], mode[16], *slash;
    if (!g_self || !GetModuleFileNameA(g_self, ini, MAX_PATH)) return FALSE;
    slash = strrchr(ini, '\\');
    if (!slash) return FALSE;
    snprintf(slash + 1, sizeof ini - (size_t)(slash + 1 - ini), "igi2_unified.ini");
    GetPrivateProfileStringA("Renderer", "Mode", "dx11", mode, sizeof mode, ini);
    nx_anisotropy = (unsigned)GetPrivateProfileIntA("Renderer", "Anisotropy", 0, ini);
    if (nx_anisotropy > 16) nx_anisotropy = 16;
    nx_antialiasing = (unsigned)GetPrivateProfileIntA("Renderer", "Antialiasing", 0, ini);
    if (nx_antialiasing > 16) nx_antialiasing = 16;
    nx_alpha_to_coverage = (unsigned)GetPrivateProfileIntA("Renderer", "AlphaToCoverage", 1, ini) != 0;
    nx_vsync = GetPrivateProfileIntA("Renderer", "VSync", 1, ini);
    if (nx_vsync > 4) nx_vsync = 4;
    if (nx_vsync < -1) nx_vsync = -1;
    return !_stricmp(mode, "dx11") || !_stricmp(mode, "d3d11") || !_stricmp(mode, "neonix");
}

void __stdcall renderer_Load(HMODULE patch)
{
    PMGetCurVersionID_fn version = patch ? (PMGetCurVersionID_fn)(uintptr_t)GetProcAddress(patch, "PMGetCurVersionID") : NULL;
    PMGetCurPMIHandle_fn info = patch ? (PMGetCurPMIHandle_fn)(uintptr_t)GetProcAddress(patch, "PMGetCurPMIHandle") : NULL;
    PATCHMODULEINFO *m;
    BYTE *site;
    char path[MAX_PATH + 32], *slash;
    int32_t rel;

    g_status = 2;
    GetModuleFileNameA(NULL, g_folder, sizeof g_folder);
    slash = strrchr(g_folder, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\nx_measure.on", g_folder);
    g_measure = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(path, sizeof path, "%s\\nx_d3d8_measure.on", g_folder);
    nx_trace_quiet = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(path, sizeof path, "%s\\nx_d3d8_trace.on", g_folder);
    g_mode = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || nx_trace_quiet ? MODE_TRACE : MODE_OFF;
    snprintf(path, sizeof path, "%s\\nx_dx11.on", g_folder);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || (g_mode == MODE_OFF && IniWantsDX11())) {
        g_measure = TRUE;   /* while the renderer is new: frame time lines in the log under Direct3D 11 */
        g_trace_dx11 = g_mode == MODE_TRACE;   /* both files: the trace counts the calls made to the Direct3D 11 device */
        g_mode = MODE_DX11;
    }
    {
        /* [Renderer] DrawDistance and DetailDistance are game-side: they apply under either renderer (draw_distance.h) */
        PATCHMODULEINFO *dm;
        if (version && info && version() == 0u && (dm = info()) && dm->dwDefBaseAddr == IGI2_DEFAULT_BASE && dm->dwCurBaseAddr)
            DD_Install(g_self, dm->uiRelocValue);
    }
    if (g_mode == MODE_OFF) { Renderer_Log("original Direct3D 8"); return; }
    if (!version || !info || version() != 0u || !(m = info()) || m->dwDefBaseAddr != IGI2_DEFAULT_BASE || !m->dwCurBaseAddr) {
        Renderer_Log("not igi2.exe 1.3: original Direct3D 8");
        return;
    }
    g_reloc = m->uiRelocValue;
    site = (BYTE *)(uintptr_t)(OPEN_FIRST_CREATE_CALL + g_reloc);
    if (memcmp(site, kOriginalCall, sizeof kOriginalCall)) {
        Renderer_Log("Direct3D_Open is not the 1.3 code (another patch?): original Direct3D 8");
        return;
    }
    g_first_create = (int (__cdecl *)(void))(uintptr_t)(FIRST_CREATE + g_reloc);
    rel = (int32_t)((uintptr_t)Renderer_BeforeFirstCreate - ((uintptr_t)site + 5));
    g_call[0] = 0xE8;
    memcpy(g_call + 1, &rel, 4);
    if (!WriteCode(site, g_call, sizeof g_call)) { Renderer_Log("cannot patch Direct3D_Open: original Direct3D 8"); return; }
    g_patched = TRUE;
    Renderer_Log(g_mode == MODE_TRACE ? "trace mode: Direct3D 8 calls will be counted" : "Direct3D 11 mode");
    if (!g_reloc) {
        NxTerrain_Install(g_mode == MODE_DX11);
        if (g_mode == MODE_DX11) NxPace_Install();
        if (g_mode == MODE_DX11) NxScene_Install();
    }
}

BOOL __cdecl renderer_Unload(void)
{
    BYTE *site = (BYTE *)(uintptr_t)(OPEN_FIRST_CREATE_CALL + g_reloc);
    void **slot = (void **)(uintptr_t)(CREATE8_POINTER + g_reloc);
    if (!DD_Uninstall()) return FALSE;
    if (!g_patched) { g_status = 0; return TRUE; }
    if (*slot == (void *)Nx_Direct3DCreate8) return FALSE; /* the game holds our objects */
    if (memcmp(site, g_call, sizeof g_call) || !WriteCode(site, kOriginalCall, sizeof kOriginalCall)) return FALSE;
    g_patched = FALSE;
    g_status = 0;
    return TRUE;
}

LONG __cdecl Renderer_GetStatus(void) { return g_status; }
