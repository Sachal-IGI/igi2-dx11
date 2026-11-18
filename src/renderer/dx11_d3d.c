/* The Neonix renderer, Direct3D 11 mode: IDirect3D8 (adapter, modes, formats, caps) and loading Direct3D 11.
   The caps are those Direct3D 8 reports on a current card (dx11_caps.inc), so the game picks the same paths as before,
   except: pixel shaders up to ps.1.3 (1.4 is not translated). */
#include "dx11_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dx11_caps.inc"

D3DCAPS8 nx_caps;

/* The adapter's driver name; the Neonix Launcher looks for this text in the DLL to know it has the renderer
   (Renderer.cs), so it must stay a stored string (a literal in snprintf becomes immediate stores). */
const char nx_driver_name[] = "neonix_d3d11";

typedef HRESULT (WINAPI *PFN_Create11)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                       ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static PFN_Create11 nx_create11;

HRESULT Nx_D3D11Create(ID3D11Device **d, ID3D11DeviceContext **c, D3D_FEATURE_LEVEL *level)
{
    static const D3D_FEATURE_LEVEL levels[4] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0};
    HRESULT hr;
    if (!nx_create11) return E_FAIL;
    hr = nx_create11(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION, d, level, c);
    if (hr == E_INVALIDARG)   /* no 11.1 runtime (Windows 7 without the platform update) */
        hr = nx_create11(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3, D3D11_SDK_VERSION, d, level, c);
    return hr;
}

typedef struct NxD3D {
    NxHead h;
    UINT modes;
    D3DDISPLAYMODE mode[256];
    D3DADAPTER_IDENTIFIER8 id;
} NxD3D;

static NxD3D *D(IDirect3D8 *This) { return (NxD3D *)This; }

static HRESULT STDMETHODCALLTYPE A_QueryInterface(IDirect3D8 *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(riid, &nxt_IID_IDirect3D8, sizeof(GUID)) || !memcmp(riid, &nxt_IID_IUnknown, sizeof(GUID))) {
        This->lpVtbl->AddRef(This);
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE A_AddRef(IDirect3D8 *This) { return (ULONG)InterlockedIncrement(&D(This)->h.refs); }
static ULONG STDMETHODCALLTYPE A_Release(IDirect3D8 *This)
{
    LONG r = InterlockedDecrement(&D(This)->h.refs);
    if (r == 0) free(D(This));
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE A_RegisterSoftwareDevice(IDirect3D8 *This, void *f) { (void)This; (void)f; return D3DERR_INVALIDCALL; }
static UINT STDMETHODCALLTYPE A_GetAdapterCount(IDirect3D8 *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE A_GetAdapterIdentifier(IDirect3D8 *This, UINT a, DWORD flags, D3DADAPTER_IDENTIFIER8 *id)
{
    (void)flags;
    if (a || !id) return D3DERR_INVALIDCALL;
    *id = D(This)->id;
    return D3D_OK;
}
static UINT STDMETHODCALLTYPE A_GetAdapterModeCount(IDirect3D8 *This, UINT a) { return a ? 0 : D(This)->modes; }
static HRESULT STDMETHODCALLTYPE A_EnumAdapterModes(IDirect3D8 *This, UINT a, UINT i, D3DDISPLAYMODE *m)
{
    if (a || !m || i >= D(This)->modes) return D3DERR_INVALIDCALL;
    *m = D(This)->mode[i];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE A_GetAdapterDisplayMode(IDirect3D8 *This, UINT a, D3DDISPLAYMODE *m)
{
    DEVMODEA dm;
    (void)This;
    if (a || !m) return D3DERR_INVALIDCALL;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    m->Width = dm.dmPelsWidth;
    m->Height = dm.dmPelsHeight;
    m->RefreshRate = dm.dmDisplayFrequency;
    m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static BOOL DisplayFormat(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 || f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_A1R5G5B5; }
static HRESULT STDMETHODCALLTYPE A_CheckDeviceType(IDirect3D8 *This, UINT a, D3DDEVTYPE t, D3DFORMAT display, D3DFORMAT back, WINBOOL windowed)
{
    (void)This; (void)windowed;
    if (a || t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return DisplayFormat(display) && DisplayFormat(back) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE A_CheckDeviceFormat(IDirect3D8 *This, UINT a, D3DDEVTYPE t, D3DFORMAT adapter, DWORD usage, D3DRESOURCETYPE rt,
                                                     D3DFORMAT f)
{
    const NxFormat *fmt = Nx_Format(f);
    BOOL depth = f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16 || f == D3DFMT_D32 || f == D3DFMT_D16_LOCKABLE ||
                 f == D3DFMT_D24X4S4 || f == D3DFMT_D15S1;
    (void)This; (void)adapter;
    if (a || t != D3DDEVTYPE_HAL || !fmt) return D3DERR_NOTAVAILABLE;
    if (rt == D3DRTYPE_VOLUMETEXTURE || rt == D3DRTYPE_VOLUME) return D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_DEPTHSTENCIL) return depth && (rt == D3DRTYPE_SURFACE) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (depth) return D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_RENDERTARGET) return DisplayFormat(f) ? D3D_OK : D3DERR_NOTAVAILABLE;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE A_CheckDeviceMultiSampleType(IDirect3D8 *This, UINT a, D3DDEVTYPE t, D3DFORMAT f, WINBOOL windowed,
                                                              D3DMULTISAMPLE_TYPE ms)
{
    (void)This; (void)f; (void)windowed;
    if (a || t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (ms == D3DMULTISAMPLE_NONE) return D3D_OK;
    return (UINT)ms <= 16 && (nx_ms_supported & (1u << ms)) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE A_CheckDepthStencilMatch(IDirect3D8 *This, UINT a, D3DDEVTYPE t, D3DFORMAT adapter, D3DFORMAT rt, D3DFORMAT ds)
{
    (void)This; (void)adapter; (void)rt; (void)ds;
    return a || t != D3DDEVTYPE_HAL ? D3DERR_NOTAVAILABLE : D3D_OK;
}
static HRESULT STDMETHODCALLTYPE A_GetDeviceCaps(IDirect3D8 *This, UINT a, D3DDEVTYPE t, D3DCAPS8 *caps)
{
    (void)This;
    if (a || !caps) return D3DERR_INVALIDCALL;
    if (t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    *caps = nx_caps;
    return D3D_OK;
}
static HMONITOR STDMETHODCALLTYPE A_GetAdapterMonitor(IDirect3D8 *This, UINT a)
{
    POINT p = {0, 0};
    (void)This;
    return a ? NULL : MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}
static HRESULT STDMETHODCALLTYPE A_CreateDevice(IDirect3D8 *This, UINT a, D3DDEVTYPE t, HWND window, DWORD behavior,
                                                D3DPRESENT_PARAMETERS *pp, struct IDirect3DDevice8 **out)
{
    D3DPRESENT_PARAMETERS keep;
    HRESULT hr;
    IDirect3D8 *real;
    if (a || t != D3DDEVTYPE_HAL || !pp) return D3DERR_NOTAVAILABLE;
    keep = *pp;
    hr = Nx_CreateDevice(This, window, behavior, pp, out);
    if (SUCCEEDED(hr)) return hr;
    /* Direct3D 11 did not start: the game gets the original Direct3D 8 device instead of an error */
    *pp = keep;
    real = nx_real_create8 ? nx_real_create8(D3D_SDK_VERSION) : NULL;
    if (!real) return hr;
    hr = real->lpVtbl->CreateDevice(real, a, t, window, behavior, pp, out);
    real->lpVtbl->Release(real);
    Nx_Log(SUCCEEDED(hr) ? "Direct3D 11 device failed: the original Direct3D 8 device is used" :
                           "Direct3D 11 device failed, Direct3D 8 too (%08lx)", (unsigned long)hr);
    return hr;
}

static IDirect3D8Vtbl kD3DVtbl = {
    .QueryInterface = A_QueryInterface, .AddRef = A_AddRef, .Release = A_Release, .RegisterSoftwareDevice = A_RegisterSoftwareDevice,
    .GetAdapterCount = A_GetAdapterCount, .GetAdapterIdentifier = A_GetAdapterIdentifier, .GetAdapterModeCount = A_GetAdapterModeCount,
    .EnumAdapterModes = A_EnumAdapterModes, .GetAdapterDisplayMode = A_GetAdapterDisplayMode, .CheckDeviceType = A_CheckDeviceType,
    .CheckDeviceFormat = A_CheckDeviceFormat, .CheckDeviceMultiSampleType = A_CheckDeviceMultiSampleType,
    .CheckDepthStencilMatch = A_CheckDepthStencilMatch, .GetDeviceCaps = A_GetDeviceCaps, .GetAdapterMonitor = A_GetAdapterMonitor,
    .CreateDevice = A_CreateDevice,
};

static BOOL nx_loaded;
static BOOL Load(void)
{
    static const char *const compilers[3] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};
    HMODULE d3d11, dxgi, comp = NULL;
    ID3D11Device *d = NULL;
    ID3D11DeviceContext *c = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;
    int i;
    if (nx_loaded) return nx_create11 && nx_D3DCompile;
    nx_loaded = TRUE;
    memcpy(&nx_caps, kCaps, sizeof kCaps < sizeof nx_caps ? sizeof kCaps : sizeof nx_caps);
    nx_caps.PixelShaderVersion = D3DPS_VERSION(1, 3);   /* ps.1.0-1.3 are translated (dx11_ps.c); 1.4 is not */
    nx_caps.MaxVertexShaderConst = 256;
    nx_caps.VertexShaderVersion = D3DVS_VERSION(1, 1);
    d3d11 = LoadLibraryA("d3d11.dll");
    dxgi = LoadLibraryA("dxgi.dll");
    for (i = 0; i < 3 && !comp; ++i) comp = LoadLibraryA(compilers[i]);
    if (!d3d11 || !dxgi || !comp) {
        Nx_Log("Direct3D 11 not here (d3d11 %p, dxgi %p, shader compiler %p)", (void *)d3d11, (void *)dxgi, (void *)comp);
        return FALSE;
    }
    nx_create11 = (PFN_Create11)(void *)GetProcAddress(d3d11, "D3D11CreateDevice");
    nx_D3DCompile = (HRESULT (WINAPI *)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT,
                                         UINT, ID3DBlob **, ID3DBlob **))(void *)GetProcAddress(comp, "D3DCompile");
    nx_D3DCreateBlob = (HRESULT (WINAPI *)(SIZE_T, ID3DBlob **))(void *)GetProcAddress(comp, "D3DCreateBlob");
    if (!nx_create11 || !nx_D3DCompile) return FALSE;
    hr = Nx_D3D11Create(&d, &c, &level);
    if (FAILED(hr)) {
        Nx_Log("no Direct3D 11 graphics card (%08lx)", (unsigned long)hr);
        nx_create11 = NULL;
        return FALSE;
    }
    if (level < D3D_FEATURE_LEVEL_10_0) {
        Nx_Log("graphics card below Direct3D 10 (level %x)", (unsigned)level);
        ID3D11DeviceContext_Release(c);
        ID3D11Device_Release(d);
        nx_create11 = NULL;
        return FALSE;
    }
    {
        /* the multisample counts both the back buffer and depth formats take */
        UINT n, q1, q2;
        for (n = 2; n <= 16; ++n)
            if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(d, DXGI_FORMAT_B8G8R8A8_UNORM, n, &q1)) && q1 &&
                SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(d, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2)) && q2)
                nx_ms_supported |= 1u << n;
    }
    ID3D11DeviceContext_Release(c);
    ID3D11Device_Release(d);
    return TRUE;
}

static void FillAdapter(NxD3D *a)
{
    DEVMODEA dm;
    DWORD i;
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    memset(&a->id, 0, sizeof a->id);
    snprintf(a->id.Driver, sizeof a->id.Driver, "%s", nx_driver_name);
    snprintf(a->id.Description, sizeof a->id.Description, "Direct3D 11");
    a->id.WHQLLevel = 1;
    if (dxgi) {
        typedef HRESULT (WINAPI *Factory1)(REFIID, void **);
        static const GUID iid_f1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
        Factory1 create = (Factory1)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
        IDXGIFactory1 *f = NULL;
        IDXGIAdapter1 *ad = NULL;
        DXGI_ADAPTER_DESC1 d;
        if (create && SUCCEEDED(create(&iid_f1, (void **)&f))) {
            if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, 0, &ad)) && SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d))) {
                WideCharToMultiByte(CP_ACP, 0, d.Description, -1, a->id.Description, sizeof a->id.Description, NULL, NULL);
                a->id.VendorId = d.VendorId;
                a->id.DeviceId = d.DeviceId;
                a->id.SubSysId = d.SubSysId;
                a->id.Revision = d.Revision;
                memcpy(&a->id.DeviceIdentifier, &d.AdapterLuid, sizeof d.AdapterLuid);
            }
            if (ad) IDXGIAdapter1_Release(ad);
            IDXGIFactory1_Release(f);
        }
    }
    /* the screen's 32-bit modes, as Direct3D 8 lists them */
    a->modes = 0;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm) && a->modes < 256; ++i) {
        UINT k;
        if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        for (k = 0; k < a->modes; ++k)
            if (a->mode[k].Width == dm.dmPelsWidth && a->mode[k].Height == dm.dmPelsHeight && a->mode[k].RefreshRate == dm.dmDisplayFrequency) break;
        if (k < a->modes) continue;
        a->mode[a->modes].Width = dm.dmPelsWidth;
        a->mode[a->modes].Height = dm.dmPelsHeight;
        a->mode[a->modes].RefreshRate = dm.dmDisplayFrequency;
        a->mode[a->modes].Format = D3DFMT_X8R8G8B8;
        ++a->modes;
    }
}

void *Nx_CreateD3D(void)
{
    NxD3D *a;
    if (!Load()) return NULL;
    a = (NxD3D *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->h.vtbl = &kD3DVtbl;
    a->h.refs = 1;
    a->h.kind = NXK_D3D;
    FillAdapter(a);
    return a;
}
