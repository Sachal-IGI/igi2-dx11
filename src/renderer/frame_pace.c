/* Frame pacing without a busy core (Direct3D 11 mode).
   igi2.exe's main loop (WinMain 0x40476F) calls App_Run (0x401480) again and again and only sleeps while its window is in
   the background (Sleep(2) at 0x404798). IGI2Patch's frame limiter just returns from the flow function until the next
   logic tick or frame is due, so with a frame cap the loop spins a whole CPU core between frames. Right after App_Run
   returns (0x404783, "cmp [isServerDedicated], ebx", replaced by a call to NxPace_Stub, which does that compare last),
   after a pass that drew nothing, this waits on a high-resolution timer until shortly before the next frame or 30 Hz
   logic tick is due; the loop runs the last fraction of a millisecond as before, so IGI2Patch's own timing still decides
   when the tick and the frame happen. App_Run itself is left alone: something hooks it and finds its caller by the
   return address (calling it from here crashed).
   When they are due: IGI2Patch renders when "now - last render decision >= interval" and ticks on a fixed schedule; the
   pass in which a frame was presented or the tick counter (flow + 0x40) moved started at that decision, so its start
   time plus the interval is the next due time. The interval comes from IGI2Patch.ini [NewFPSLimiterPatch] MaxRenderFPS
   (a number; -1 the screen's refresh rate; 0 unlimited: nothing to wait for, not installed). [Renderer] FramePacing = 0
   in igi2_unified.ini turns it off. */
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>

#define AFTER_APP_RUN 0x00404783u       /* 39 1D 60 88 6D 00  cmp [isServerDedicated], ebx */
#define FLOW_POINTER 0x083685D8u        /* flow: +0x40 logic tick, +0x44 ticks a second */
#define SERVER_DEDICATED 0x006D8860u

volatile LONG nx_present_count;

static BOOL g_on;
static LONGLONG g_margin_us = 600;   /* how long before the due time the loop runs again (high-resolution timer) */
static LONG g_seen_presents;
static int g_seen_tick = -1;
static LONGLONG g_pass_start;
static LONGLONG g_freq, g_frame_ticks, g_render_base, g_tick_base;
static HANDLE g_timer;
static BOOL g_hires;
static unsigned g_waits;
static double g_waited_ms;

static LONGLONG Now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static int Tick(int *frequency)
{
    BYTE *flow = *(BYTE **)(uintptr_t)FLOW_POINTER;
    if (!flow) { *frequency = 0; return -1; }
    *frequency = *(int *)(flow + 0x44);
    return *(int *)(flow + 0x40);
}

static void Wait(LONGLONG ticks)
{
    double ms = (double)ticks * 1000.0 / (double)g_freq;
    if (ms > 50) ms = 50;
    if (g_hires) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ms * 10000.0);   /* relative, 100 ns units */
        if (SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(g_timer, 100);
    } else if (ms >= 1.0) {
        Sleep((DWORD)ms);
    }
    ++g_waits;
    g_waited_ms += ms;
}

/* after each App_Run: what it did (a frame, a tick), and the wait when it did neither */
void __cdecl NxPace_AfterRun(void)
{
    LONGLONG start = g_pass_start, margin = g_freq * (g_hires ? g_margin_us : 1500) / 1000000;   /* 0.6 / 1.5 ms */
    int frequency, tick = Tick(&frequency);
    BOOL presented = nx_present_count != g_seen_presents, ticked = tick >= 0 && tick != g_seen_tick;
    g_seen_presents = nx_present_count;
    g_seen_tick = tick;
    if (!*(int *)(uintptr_t)SERVER_DEDICATED && start) {
        if (presented) g_render_base = start;
        if (ticked) g_tick_base = start;
        if (!presented && !ticked && g_render_base && g_tick_base && frequency > 0 && frequency <= 1000) {
            LONGLONG now = Now(), render_due = g_render_base + g_frame_ticks, tick_due = g_tick_base + g_freq / frequency, next;
            /* a due time far behind means that clock is not running (no frames: loading, a paused screen): ignore it */
            if (now - render_due > g_freq / 10) render_due = 0;
            if (now - tick_due > g_freq / 10) tick_due = 0;
            next = render_due && tick_due ? (render_due < tick_due ? render_due : tick_due) : render_due ? render_due : tick_due;
            if (next && next - margin - now > g_freq / 3333) Wait(next - margin - now);   /* more than 0.3 ms to wait */
        }
    }
    g_pass_start = Now();
}

__asm__(".intel_syntax noprefix\n"
        ".globl _NxPace_Stub\n"
        "_NxPace_Stub:\n"
        "    pushad\n"
        "    call _NxPace_AfterRun\n"
        "    popad\n"
        "    cmp dword ptr ds:0x6D8860, ebx\n"   /* the instruction this call replaced; the loop tests its flags */
        "    ret\n"
        ".att_syntax prefix\n");
extern void NxPace_Stub(void);

void NxPace_Frame(void)
{
    static DWORD last_log;
    DWORD t = GetTickCount();
    if (!g_on) return;
    if (!last_log) last_log = t;
    if (t - last_log >= 30000) {
        char text[160];
        snprintf(text, sizeof text, "frame pacing: %u waits, %.1f ms waited a second (instead of spinning)", g_waits,
                 g_waited_ms * 1000.0 / (double)(t - last_log));
        Renderer_Log(text);
        g_waits = 0; g_waited_ms = 0;
        last_log = t;
    }
}

static int CapFromIni(void)
{
    char path[MAX_PATH + 32], text[32];
    snprintf(path, sizeof path, "%s\\IGI2Patch.ini", Renderer_GameFolder());
    GetPrivateProfileStringA("NewFPSLimiterPatch", "MaxRenderFPS", "-1", text, sizeof text, path);
    return atoi(text);
}

void NxPace_Install(void)
{
    BYTE *site = (BYTE *)(uintptr_t)AFTER_APP_RUN;
    static const BYTE expect[6] = {0x39, 0x1D, 0x60, 0x88, 0x6D, 0x00};
    char ini[MAX_PATH + 32], text[160];
    int cap = CapFromIni(), on;
    int32_t rel;
    BYTE call[6];
    LARGE_INTEGER f;
    snprintf(ini, sizeof ini, "%s\\plugins\\igi2_unified.ini", Renderer_GameFolder());
    on = (int)GetPrivateProfileIntA("Renderer", "FramePacing", 1, ini);
    g_margin_us = (LONGLONG)GetPrivateProfileIntA("Renderer", "FramePacingMarginUs", 600, ini);   /* dev; under 0.6 ms the timer
                                                                                            sometimes wakes too late and frames slip */
    if (g_margin_us < 100 || g_margin_us > 3000) g_margin_us = 600;
    if (!on) { Renderer_Log("frame pacing: off (FramePacing=0)"); return; }
    if (cap == 0) { Renderer_Log("frame pacing: frame limit unlimited, nothing to wait for"); return; }
    if (cap < 0) {
        DEVMODEA dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        cap = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24 ? (int)dm.dmDisplayFrequency : 60;
    }
    if (cap < 10 || cap > 1000) return;
    if (memcmp(site, expect, sizeof expect)) {
        Renderer_Log("frame pacing: the main loop is not the 1.3 code, not installed");
        return;
    }
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_frame_ticks = g_freq / cap;
    {
        typedef HANDLE (WINAPI *CreateTimerEx)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        CreateTimerEx create = (CreateTimerEx)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
        g_timer = create ? create(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003 /* TIMER_ALL_ACCESS */) : NULL;
        g_hires = g_timer != NULL;
        if (!g_hires) timeBeginPeriod(1);
    }
    rel = (int32_t)((uintptr_t)NxPace_Stub - ((uintptr_t)site + 5));
    call[0] = 0xE8;
    memcpy(call + 1, &rel, 4);
    call[5] = 0x90;
    if (!Renderer_WriteCode(site, call, sizeof call)) { Renderer_Log("frame pacing: cannot patch the main loop"); return; }
    g_on = TRUE;
    snprintf(text, sizeof text, "frame pacing: on, %d FPS cap, waits on a %s timer between frames instead of spinning a core",
             cap, g_hires ? "high-resolution" : "1 ms");
    Renderer_Log(text);
}
