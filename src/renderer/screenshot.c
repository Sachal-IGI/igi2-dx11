/* Screenshot key (Sachal's DX11). [Screenshots] in plugins\igi2_unified.ini: Key = a Windows virtual-key code (default
   123 = F12, 0 = off), Folder = where the pictures go (default "screenshots" in the game folder). When the key goes down
   with the game in front, the frame is read back once (Nx_ReadSurface, which also resolves anti-aliasing) and copied as
   24-bit BGR; a worker thread writes it as IGI2_<date>_<time>.png through the Windows Imaging Component, so the game
   only waits for the read-back. COM is loaded at run time (the DLL links no COM library) and the WIC GUIDs are written
   out here. One picture at a time: a press while one is still being written is ignored. */
#include "dx11_internal.h"
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID kClsidFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};   /* CLSID_WICImagingFactory */
static const GUID kIidFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};     /* IID_IWICImagingFactory */
static const GUID kPng = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};             /* GUID_ContainerFormatPng */
static const GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};          /* GUID_WICPixelFormat24bppBGR */

typedef struct ShotJob { UINT w, h; BYTE *bgr; WCHAR path[MAX_PATH + 64]; char shown[MAX_PATH + 64]; } ShotJob;
static volatile LONG g_busy;
static int g_key = -1;
static char g_folder[MAX_PATH];

static void Settings(void)
{
    char ini[MAX_PATH + 32], folder[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\plugins\\igi2_unified.ini", Renderer_GameFolder());
    g_key = (int)GetPrivateProfileIntA("Screenshots", "Key", 123, ini);
    if (g_key < 0 || g_key > 254) g_key = 0;
    GetPrivateProfileStringA("Screenshots", "Folder", "screenshots", folder, sizeof folder, ini);
    if (folder[0] && (folder[1] == ':' || folder[0] == '\\')) snprintf(g_folder, sizeof g_folder, "%s", folder);
    else snprintf(g_folder, sizeof g_folder, "%s\\%s", Renderer_GameFolder(), folder[0] ? folder : "screenshots");
}

static DWORD WINAPI Encode(void *arg)
{
    ShotJob *j = (ShotJob *)arg;
    HMODULE ole = LoadLibraryA("ole32.dll");
    HRESULT (WINAPI *init)(void *, DWORD) = ole ? (HRESULT (WINAPI *)(void *, DWORD))(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
    HRESULT (WINAPI *create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
        ole ? (HRESULT (WINAPI *)(REFCLSID, IUnknown *, DWORD, REFIID, void **))(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
    void (WINAPI *uninit)(void) = ole ? (void (WINAPI *)(void))(void *)GetProcAddress(ole, "CoUninitialize") : NULL;
    IWICImagingFactory *f = NULL;
    IWICStream *s = NULL;
    IWICBitmapEncoder *e = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *bag = NULL;
    WICPixelFormatGUID format = kBgr24;
    HRESULT hr = E_FAIL;
    BOOL com = FALSE;
    char line[MAX_PATH + 120];
    if (init && create && uninit) {
        com = SUCCEEDED(init(NULL, COINIT_MULTITHREADED));
        hr = create(&kClsidFactory, NULL, CLSCTX_INPROC_SERVER, &kIidFactory, (void **)&f);
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &s);
        if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(s, j->path, GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &kPng, NULL, &e);
        if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(e, &fr, &bag);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, bag);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, j->w, j->h);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &format);
        if (SUCCEEDED(hr) && memcmp(&format, &kBgr24, sizeof format)) hr = E_FAIL;   /* PNG takes 24-bit BGR as it is */
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, j->h, j->w * 3, j->w * 3 * j->h, j->bgr);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
        if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(e);
        if (bag) IPropertyBag2_Release(bag);
        if (fr) IWICBitmapFrameEncode_Release(fr);
        if (e) IWICBitmapEncoder_Release(e);
        if (s) IWICStream_Release(s);
        if (f) IWICImagingFactory_Release(f);
        if (com) uninit();
    }
    if (SUCCEEDED(hr)) snprintf(line, sizeof line, "screenshot saved: %s", j->shown);
    else { snprintf(line, sizeof line, "screenshot could not be saved (%08lx): %s", (unsigned long)hr, j->shown); DeleteFileW(j->path); }
    Renderer_Log(line);
    if (ole) FreeLibrary(ole);
    free(j->bgr);
    free(j);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void Take(NxDevice *dev)
{
    NxSurface *b = dev->backbuffer;
    ShotJob *j;
    SYSTEMTIME t;
    UINT x, y;
    HANDLE thread;
    if (!b || !b->fmt || (b->fmt->bpp != 4 && b->fmt->bpp != 2) || FAILED(Nx_ReadSurface(b)) || !b->mem) {
        Renderer_Log("screenshot: the frame could not be read");
        return;
    }
    j = (ShotJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->w = b->width; j->h = b->height;
    j->bgr = (BYTE *)malloc((size_t)j->w * 3 * j->h);
    if (!j->bgr) { free(j); return; }
    for (y = 0; y < j->h; ++y) {
        const BYTE *src = b->mem + (size_t)y * b->pitch;
        BYTE *dst = j->bgr + (size_t)y * j->w * 3;
        for (x = 0; x < j->w; ++x) {
            if (b->fmt->bpp == 4) { dst[3 * x] = src[4 * x]; dst[3 * x + 1] = src[4 * x + 1]; dst[3 * x + 2] = src[4 * x + 2]; }
            else {
                WORD v = ((const WORD *)src)[x];
                dst[3 * x] = (BYTE)((v & 31) << 3); dst[3 * x + 1] = (BYTE)(((v >> 5) & 63) << 2); dst[3 * x + 2] = (BYTE)((v >> 11) << 3);
            }
        }
    }
    CreateDirectoryA(g_folder, NULL);
    GetLocalTime(&t);
    snprintf(j->shown, sizeof j->shown, "%s\\IGI2_%04u-%02u-%02u_%02u-%02u-%02u.png", g_folder, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    for (x = 2; GetFileAttributesA(j->shown) != INVALID_FILE_ATTRIBUTES && x < 100; ++x)
        snprintf(j->shown, sizeof j->shown, "%s\\IGI2_%04u-%02u-%02u_%02u-%02u-%02u_%u.png", g_folder, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, x);
    MultiByteToWideChar(CP_ACP, 0, j->shown, -1, j->path, MAX_PATH + 64);
    thread = CreateThread(NULL, 0, Encode, j, 0, NULL);
    if (!thread) { free(j->bgr); free(j); InterlockedExchange(&g_busy, 0); return; }
    CloseHandle(thread);
}

/* once a frame, on the game thread, before the frame is presented */
void Nx_ScreenshotKey(NxDevice *dev)
{
    static BOOL was_down;
    static DWORD test_checked;
    BOOL down;
    if (g_key < 0) Settings();
    if (GetTickCount() - test_checked > 1000) {
        /* dev: nx_screenshot.test in the game folder counts as one press (hidden tests cannot press keys) */
        char path[MAX_PATH + 32];
        test_checked = GetTickCount();
        snprintf(path, sizeof path, "%s\\nx_screenshot.test", Renderer_GameFolder());
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && DeleteFileA(path) && !InterlockedExchange(&g_busy, 1)) Take(dev);
    }
    if (!g_key) return;
    down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down && !was_down && GetForegroundWindow() == dev->window && !InterlockedExchange(&g_busy, 1)) Take(dev);
    was_down = down;
}
