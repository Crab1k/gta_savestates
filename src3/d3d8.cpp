#include "d3d8.h"
#include <smmintrin.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace d3d {

void (*onDeviceReset)() = nullptr;
bool releaseCheck = false;
bool forceWindowed = false;
int windowX = 40, windowY = 40;

namespace {

HWND g_hwnd = nullptr;

// Windows cuts a window down to about the screen size. RenderWare then cannot make a camera as large as the back
// buffer and the game quits at start (or loses its camera on a resolution change), so the window may be any size.
WNDPROC g_sizeWndProcNext = nullptr;
SIZE g_windowSize = {};

LRESULT CALLBACK SizeWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    LRESULT r = CallWindowProcA(g_sizeWndProcNext, w, m, wp, lp);
    if (m == WM_GETMINMAXINFO) {
        auto* mmi = (MINMAXINFO*)lp;
        if (mmi->ptMaxTrackSize.x < g_windowSize.cx) mmi->ptMaxTrackSize.x = g_windowSize.cx;
        if (mmi->ptMaxTrackSize.y < g_windowSize.cy) mmi->ptMaxTrackSize.y = g_windowSize.cy;
    }
    return r;
}

void MakeWindowed(D3DPRESENT_PARAMETERS8* pp, HWND hwnd) {
    if (!pp) return;
    if (!hwnd) hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : g_hwnd;
    g_hwnd = hwnd;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = 0;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;   // the desktop's format
    if (pp->SwapEffect == D3DSWAPEFFECT_FLIP) pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    if (hwnd && pp->BackBufferWidth && pp->BackBufferHeight) {
        LONG w = (LONG)pp->BackBufferWidth, h = (LONG)pp->BackBufferHeight;
        LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        RECT r = {0, 0, w, h};
        AdjustWindowRect(&r, style, FALSE);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoA(MonitorFromPoint(POINT{windowX, windowY}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& work = mi.rcWork;
        const RECT& mon = mi.rcMonitor;
        LONG x = windowX, y = windowY;
        if (r.right - r.left <= work.right - work.left && r.bottom - r.top <= work.bottom - work.top) {
            // with a title bar, kept inside the screen's work area
            x = (std::max)(work.left, (std::min)(x, work.right - (r.right - r.left)));
            y = (std::max)(work.top, (std::min)(y, work.bottom - (r.bottom - r.top)));
        } else {
            // too large for a title bar: without one, at the top of the screen and centred
            style = WS_POPUP | WS_VISIBLE;
            r = {0, 0, w, h};
            x = mon.left + (std::max)(0L, (mon.right - mon.left - w) / 2);
            y = mon.top;
            Log("window: %ldx%ld does not fit on the screen with a title bar, shown without one", w, h);
        }
        g_windowSize = {r.right - r.left, r.bottom - r.top};
        if (!g_sizeWndProcNext)
            g_sizeWndProcNext = (WNDPROC)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)(uintptr_t)&SizeWndProc);
        SetWindowLongA(hwnd, GWL_STYLE, style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, g_windowSize.cx, g_windowSize.cy, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        RECT c = {};
        GetClientRect(hwnd, &c);
        if (c.right != w || c.bottom != h) Log("window: client area %ldx%ld, not %ldx%ld", c.right, c.bottom, w, h);
    }
}

// IDirect3D8 / IDirect3DDevice8 vtable slots (d3d8.h order).
enum : int {
    D3D_CreateDevice = 15,
    DEV_CreateAdditionalSwapChain = 13, DEV_Reset = 14, DEV_Present = 15,
    DEV_CreateTexture = 20, DEV_CreateVolumeTexture = 21, DEV_CreateCubeTexture = 22, DEV_CreateVertexBuffer = 23,
    DEV_CreateIndexBuffer = 24, DEV_CreateRenderTarget = 25, DEV_CreateDepthStencilSurface = 26,
    DEV_CreateImageSurface = 27, DEV_CreateStateBlock = 57, DEV_CreateVertexShader = 75, DEV_CreatePixelShader = 87,
};

struct Obj {
    Kind     kind;
    bool     defaultPool;
    uint32_t snapRefs;   // captures that reference this object
};

std::unordered_map<IUnknown*, Obj> g_objs;
CRITICAL_SECTION g_cs;
IDirect3DDevice8* g_dev = nullptr;
IDirect3D8* g_d3d = nullptr;
uint32_t g_generation = 1;
uint32_t g_shaders = 0;
void* g_orig[128];

template <typename F> F Orig(int slot) { return (F)g_orig[slot]; }

// A slot that already points at another tool's code (DxWnd, which hooked it after us and calls us) is left alone:
// hooking it again would make our original point back at that tool, an endless loop.
bool InD3d8(void* fn);
void LogDeviceSlots(const char* when, IDirect3DDevice8* d);

void HookSlot(void** vt, int slot, void* fn) {
    if (vt[slot] == fn || (g_orig[slot] && !InD3d8(vt[slot]))) return;
    g_orig[slot] = vt[slot];
    Unprotect((uintptr_t)&vt[slot], 4);
    vt[slot] = fn;
}

void HookRelease(IUnknown* obj);
void HookTextureVt(IUnknown* t);
void HookSurfaceVt(IUnknown* sf);

void TrackObj(IUnknown* obj, Kind kind, bool defaultPool) {
    EnterCriticalSection(&g_cs);
    auto it = g_objs.find(obj);
    if (it == g_objs.end()) {
        obj->AddRef();   // the pin: the object outlives the game's last Release until Collect() sees it unused
        g_objs.emplace(obj, Obj{kind, defaultPool, 0});
        HookRelease(obj);
        if (kind == K_Texture) HookTextureVt(obj);
        else if (kind == K_RenderTarget || kind == K_DepthStencil || kind == K_ImageSurface) HookSurfaceVt(obj);
    }
    LeaveCriticalSection(&g_cs);
}

// Objects our own code creates (the message overlay) are not the game's. Everything else is: the call may come
// through another tool's hook (DxWnd) rather than straight from the game.
bool FromUs(void* ret) {
    static HMODULE self = nullptr;
    if (!self) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)&FromUs, &self);
    HMODULE m = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)ret, &m) && m == self;
}

void TrackIfGame(void* ret, IUnknown* obj, Kind kind, bool defaultPool) {
    if (obj && !FromUs(ret)) TrackObj(obj, kind, defaultPool);
}

// Lab diagnostic: catch a game Release that consumes our pin (an over-release by somebody).
int g_ourRelease = 0;
std::unordered_map<void**, ULONG(STDMETHODCALLTYPE*)(IUnknown*)> g_origRelease;

ULONG STDMETHODCALLTYPE H_Release(IUnknown* self) {
    void** vt = *(void***)self;
    ULONG r = g_origRelease[vt](self);
    if (r == 0 && !g_ourRelease) {
        auto it = g_objs.find(self);
        if (it != g_objs.end()) {
            Log("OVER-RELEASE: tracked object %p (vtable %p) reached 0 refs, released from %p", self, vt, _ReturnAddress());
            g_objs.erase(it);
        }
    }
    return r;
}

void HookRelease(IUnknown* obj) {
    if (!releaseCheck) return;
    void** vt = *(void***)obj;
    if (g_origRelease.count(vt)) return;
    g_origRelease[vt] = (ULONG(STDMETHODCALLTYPE*)(IUnknown*))vt[2];
    Unprotect((uintptr_t)&vt[2], 4);
    vt[2] = (void*)&H_Release;
}

ULONG RefCount(IUnknown* o) {   // current external reference count
    ULONG n = o->AddRef();
    o->Release();
    return n - 1;
}

// ------------------------------------------------------------------ outstanding locks
// The game may keep a raster locked across frames, with the locked surface and the pixel pointer stored in its
// memory. A savestate records such locks, so that loading it in another process can lock the re-created texture and
// point the game at the new pixels.
struct LockRec { RECT rect; bool whole; DWORD flags; void* bits; INT pitch; IUnknown* container; };
std::unordered_map<IUnknown*, LockRec> g_surfLocks;                  // locked surface -> lock
std::map<std::pair<IUnknown*, UINT>, LockRec> g_texLocks;           // (texture, level) -> lock

enum { V_SurfLock, V_SurfUnlock, V_TexLock, V_TexUnlock, V_TexSurfaceLevel, V_Count };
std::unordered_map<void**, void*> g_vtOrig[V_Count];               // hooked vtable -> original method

void HookVt(void** vt, int which, int slot, void* fn) {
    auto& m = g_vtOrig[which];
    if (m.count(vt) || vt[slot] == fn) return;
    m[vt] = vt[slot];
    Unprotect((uintptr_t)&vt[slot], 4);
    vt[slot] = fn;
}

template <typename F> F VtOrig(int which, void* obj) { return (F)g_vtOrig[which][*(void***)obj]; }

HRESULT STDMETHODCALLTYPE H_SurfLock(IDirect3DSurface8* sf, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    HRESULT hr = VtOrig<decltype(&H_SurfLock)>(V_SurfLock, sf)(sf, lr, r, fl);
    if (SUCCEEDED(hr) && lr) {
        EnterCriticalSection(&g_cs);
        IUnknown* container = nullptr;
        if (g_objs.count((IUnknown*)sf)) {
            container = (IUnknown*)sf;
        } else if (SUCCEEDED(sf->GetContainer(IID_IDirect3DTexture8_, (void**)&container)) && container) {
            container->Release();
            if (!g_objs.count(container)) container = nullptr;
        }
        if (container) g_surfLocks[(IUnknown*)sf] = LockRec{r ? *r : RECT{}, !r, fl, lr->pBits, lr->Pitch, container};
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_SurfUnlock(IDirect3DSurface8* sf) {
    EnterCriticalSection(&g_cs);
    g_surfLocks.erase((IUnknown*)sf);
    LeaveCriticalSection(&g_cs);
    return VtOrig<decltype(&H_SurfUnlock)>(V_SurfUnlock, sf)(sf);
}

HRESULT STDMETHODCALLTYPE H_TexLock(IDirect3DTexture8* t, UINT l, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    HRESULT hr = VtOrig<decltype(&H_TexLock)>(V_TexLock, t)(t, l, lr, r, fl);
    if (SUCCEEDED(hr) && lr) {
        EnterCriticalSection(&g_cs);
        g_texLocks[{(IUnknown*)t, l}] = LockRec{r ? *r : RECT{}, !r, fl, lr->pBits, lr->Pitch, (IUnknown*)t};
        // d3d locks a level's surface through its vtable but unlocks it directly: that record would never go
        for (auto it = g_surfLocks.begin(); it != g_surfLocks.end();)
            it = it->second.container == (IUnknown*)t && it->second.bits == lr->pBits ? g_surfLocks.erase(it) : std::next(it);
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_TexUnlock(IDirect3DTexture8* t, UINT l) {
    EnterCriticalSection(&g_cs);
    g_texLocks.erase({(IUnknown*)t, l});
    LeaveCriticalSection(&g_cs);
    return VtOrig<decltype(&H_TexUnlock)>(V_TexUnlock, t)(t, l);
}

void ForgetLocks(IUnknown* container) {
    for (auto it = g_surfLocks.begin(); it != g_surfLocks.end();)
        it = it->second.container == container ? g_surfLocks.erase(it) : std::next(it);
    for (auto it = g_texLocks.begin(); it != g_texLocks.end();)
        it = it->first.first == container ? g_texLocks.erase(it) : std::next(it);
}

void HookSurfaceVt(IUnknown* sf) {
    void** vt = *(void***)sf;
    HookVt(vt, V_SurfLock, 9, (void*)&H_SurfLock);
    HookVt(vt, V_SurfUnlock, 10, (void*)&H_SurfUnlock);
}

HRESULT STDMETHODCALLTYPE H_TexSurfaceLevel(IDirect3DTexture8* t, UINT l, IDirect3DSurface8** out) {
    HRESULT hr = VtOrig<decltype(&H_TexSurfaceLevel)>(V_TexSurfaceLevel, t)(t, l, out);
    if (SUCCEEDED(hr) && out && *out) HookSurfaceVt((IUnknown*)*out);
    return hr;
}

void HookTextureVt(IUnknown* t) {
    void** vt = *(void***)t;
    HookVt(vt, V_TexSurfaceLevel, 15, (void*)&H_TexSurfaceLevel);
    HookVt(vt, V_TexLock, 16, (void*)&H_TexLock);
    HookVt(vt, V_TexUnlock, 17, (void*)&H_TexUnlock);
}

// ------------------------------------------------------------------ creation hooks
#define RET _ReturnAddress()
HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice8* d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f,
                                          D3DPOOL p, IDirect3DTexture8** o) {
    HRESULT hr = Orig<decltype(&H_CreateTexture)>(DEV_CreateTexture)(d, w, h, lv, use, f, p, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_Texture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVolumeTexture(IDirect3DDevice8* d, UINT w, UINT h, UINT dp, UINT lv, DWORD use,
                                                D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture8** o) {
    HRESULT hr = Orig<decltype(&H_CreateVolumeTexture)>(DEV_CreateVolumeTexture)(d, w, h, dp, lv, use, f, p, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_VolumeTexture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateCubeTexture(IDirect3DDevice8* d, UINT e, UINT lv, DWORD use, D3DFORMAT f,
                                              D3DPOOL p, IDirect3DCubeTexture8** o) {
    HRESULT hr = Orig<decltype(&H_CreateCubeTexture)>(DEV_CreateCubeTexture)(d, e, lv, use, f, p, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_CubeTexture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexBuffer(IDirect3DDevice8* d, UINT len, DWORD use, DWORD fvf, D3DPOOL p,
                                               IDirect3DVertexBuffer8** o) {
    HRESULT hr = Orig<decltype(&H_CreateVertexBuffer)>(DEV_CreateVertexBuffer)(d, len, use, fvf, p, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_VertexBuffer, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateIndexBuffer(IDirect3DDevice8* d, UINT len, DWORD use, D3DFORMAT f, D3DPOOL p,
                                              IDirect3DIndexBuffer8** o) {
    HRESULT hr = Orig<decltype(&H_CreateIndexBuffer)>(DEV_CreateIndexBuffer)(d, len, use, f, p, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_IndexBuffer, p == D3DPOOL_DEFAULT);
    return hr;
}
std::unordered_map<IUnknown*, BOOL> g_lockable;   // render targets: created lockable
HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice8* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                               BOOL lock, IDirect3DSurface8** o) {
    HRESULT hr = Orig<decltype(&H_CreateRenderTarget)>(DEV_CreateRenderTarget)(d, w, h, f, ms, lock, o);
    if (SUCCEEDED(hr) && o && !FromUs(RET)) { g_lockable[(IUnknown*)*o] = lock; TrackObj((IUnknown*)*o, K_RenderTarget, true); }
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateDepthStencilSurface(IDirect3DDevice8* d, UINT w, UINT h, D3DFORMAT f,
                                                      D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8** o) {
    HRESULT hr = Orig<decltype(&H_CreateDepthStencilSurface)>(DEV_CreateDepthStencilSurface)(d, w, h, f, ms, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_DepthStencil, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateImageSurface(IDirect3DDevice8* d, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface8** o) {
    HRESULT hr = Orig<decltype(&H_CreateImageSurface)>(DEV_CreateImageSurface)(d, w, h, f, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, (IUnknown*)*o, K_ImageSurface, false);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice8* d, const DWORD* decl, const DWORD* fn, DWORD* h, DWORD u) {
    HRESULT hr = Orig<decltype(&H_CreateVertexShader)>(DEV_CreateVertexShader)(d, decl, fn, h, u);
    if (SUCCEEDED(hr) && !FromUs(RET)) { g_shaders++; Log("game created vertex shader %08X", h ? *h : 0); }
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice8* d, const DWORD* fn, DWORD* h) {
    HRESULT hr = Orig<decltype(&H_CreatePixelShader)>(DEV_CreatePixelShader)(d, fn, h);
    if (SUCCEEDED(hr) && !FromUs(RET)) { g_shaders++; Log("game created pixel shader %08X", h ? *h : 0); }
    return hr;
}
#undef RET

// ------------------------------------------------------------------ overlay
std::string g_text, g_texText;
DWORD g_textUntil = 0;
IDirect3DTexture8* g_textTex = nullptr;
DWORD g_overlayState = 0;
int g_texW = 0, g_texH = 0, g_textW = 0, g_textH = 0;

void ReleaseOverlay() {
    if (g_overlayState) { g_dev->DeleteStateBlock(g_overlayState); g_overlayState = 0; }
    if (g_textTex) { g_textTex->Release(); g_textTex = nullptr; }
}

bool BuildTextTexture() {
    if (g_textTex) { g_textTex->Release(); g_textTex = nullptr; }
    g_texText = g_text;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT r = {0, 0, 0, 0};
    DrawTextA(dc, g_text.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX);
    g_textW = r.right + 24;
    g_textH = r.bottom + 12;
    g_texW = 1; while (g_texW < g_textW) g_texW <<= 1;
    g_texH = 1; while (g_texH < g_textH) g_texH <<= 1;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_texW;
    bi.bmiHeader.biHeight = -g_texH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, g_texW * g_texH * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = {12, 6, g_textW, g_textH};
    DrawTextA(dc, g_text.c_str(), -1, &tr, DT_NOPREFIX);
    GdiFlush();
    bool ok = false;
    HRESULT hr = Orig<decltype(&H_CreateTexture)>(DEV_CreateTexture)(g_dev, g_texW, g_texH, 1, 0, D3DFMT_A8R8G8B8,
                                                                     D3DPOOL_MANAGED, &g_textTex);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(g_textTex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < g_texH; y++) {
            const uint32_t* src = (const uint32_t*)bits + y * g_texW;
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < g_texW; x++) {
                uint32_t v = src[x] & 0xFF;   // white text: any channel is the coverage
                bool inBox = x < g_textW && y < g_textH;
                uint32_t a = inBox ? 150 + (v * 105) / 255 : 0;
                dst[x] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        }
        g_textTex->UnlockRect(0);
        ok = true;
    }
    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

void DrawOverlay(IDirect3DDevice8* d) {
    if (g_text.empty() || (int)(GetTickCount() - g_textUntil) > 0) return;
    if (!g_textTex || g_texText != g_text) {
        if (!BuildTextTexture()) return;
    }
    if (!g_overlayState && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_overlayState))) return;
    if (FAILED(d->BeginScene())) return;
    d->CaptureStateBlock(g_overlayState);
    d->SetPixelShader(0);
    d->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, (IDirect3DBaseTexture8*)g_textTex);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetTextureStageState(0, 16 /* MAGFILTER */, D3DTEXF_POINT);
    d->SetTextureStageState(0, 17 /* MINFILTER */, D3DTEXF_POINT);
    d->SetTextureStageState(0, 18 /* MIPFILTER */, D3DTEXF_NONE);
    struct V { float x, y, z, w, u, v; };
    const float x0 = 16.0f - 0.5f, y0 = 16.0f - 0.5f;
    const float w = (float)g_texW, h = (float)g_texH;
    V v[4] = {{x0, y0, 0, 1, 0, 0}, {x0 + w, y0, 0, 1, 1, 0}, {x0, y0 + h, 0, 1, 0, 1}, {x0 + w, y0 + h, 0, 1, 1, 1}};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    d->ApplyStateBlock(g_overlayState);
    d->EndScene();
}

// ------------------------------------------------------------------ screenshots (lab)
std::string g_shotPath;

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    c = ~c;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// Minimal PNG writer: RGB8, zlib "stored" blocks (no compression).
bool WritePng(const char* path, const uint8_t* rgb, int w, int h) {
    std::string raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.append((const char*)rgb + (size_t)y * w * 3, (size_t)w * 3); }
    std::string z = "\x78\x01";
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0;) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((char)(n & 0xFF)); z.push_back((char)(n >> 8));
        z.push_back((char)(~n & 0xFF)); z.push_back((char)((~n >> 8) & 0xFF));
        z.append(raw, off, n);
        off += n;
        if (!n) break;
    }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back((char)(adler >> (i * 8)));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    auto be32 = [&](uint32_t v) { uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; fwrite(t, 1, 4, f); };
    auto chunk = [&](const char* type, const std::string& data) {
        be32((uint32_t)data.size());
        std::string td = std::string(type, 4) + data;
        fwrite(td.data(), 1, td.size(), f);
        be32(Crc32((const uint8_t*)td.data(), td.size()));
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::string ihdr(13, 0);
    for (int i = 0; i < 4; i++) { ihdr[i] = (char)(w >> (24 - 8 * i)); ihdr[4 + i] = (char)(h >> (24 - 8 * i)); }
    ihdr[8] = 8; ihdr[9] = 2;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", "");
    fclose(f);
    return true;
}

// D3D8 can only read the front buffer: a desktop-sized copy, cropped to the window's client area.
// The finished frame from the back buffer (just before Present): only the game's own image, never other windows.
void CaptureBackBuffer(IDirect3DDevice8* d) {
    std::string path;
    path.swap(g_shotPath);
    IDirect3DSurface8* bb = nullptr;
    if (FAILED(d->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC8 desc;
    bb->GetDesc(&desc);
    IDirect3DSurface8* sys = nullptr;
    D3DLOCKED_RECT lr;
    bool b32 = desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8, b16 = desc.Format == D3DFMT_R5G6B5;
    if ((b32 || b16) &&
        SUCCEEDED(Orig<decltype(&H_CreateImageSurface)>(DEV_CreateImageSurface)(d, desc.Width, desc.Height, desc.Format, &sys)) &&
        SUCCEEDED(d->CopyRects(bb, nullptr, 0, sys, nullptr)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        int w = (int)desc.Width, h = (int)desc.Height;
        std::string rgb((size_t)w * h * 3, 0);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
            for (int x = 0; x < w; x++) {
                size_t o = ((size_t)y * w + x) * 3;
                if (b32) { rgb[o] = (char)row[x * 4 + 2]; rgb[o + 1] = (char)row[x * 4 + 1]; rgb[o + 2] = (char)row[x * 4]; }
                else {
                    uint16_t v = ((const uint16_t*)row)[x];
                    rgb[o] = (char)((v >> 11) << 3); rgb[o + 1] = (char)(((v >> 5) & 63) << 2); rgb[o + 2] = (char)((v & 31) << 3);
                }
            }
        }
        sys->UnlockRect();
        WritePng(path.c_str(), (const uint8_t*)rgb.data(), w, h);
    }
    if (sys) sys->Release();
    bb->Release();
}

// ------------------------------------------------------------------ device-level hooks
uint32_t g_presents = 0;
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice8* d, const RECT* s, const RECT* dst, HWND w, const RGNDATA* r) {
    if (++g_presents % 300 == 1) Log("present #%u", g_presents);
    DrawOverlay(d);
    if (!g_shotPath.empty()) CaptureBackBuffer(d);
    return Orig<decltype(&H_Present)>(DEV_Present)(d, s, dst, w, r);
}

HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice8* d, D3DPRESENT_PARAMETERS8* pp) {
    Log("device reset: pinned graphics objects of savestates are released");
    if (onDeviceReset) onDeviceReset();
    ReleaseOverlay();
    Collect();
    if (forceWindowed) MakeWindowed(pp, g_hwnd);
    HRESULT hr = Orig<decltype(&H_Reset)>(DEV_Reset)(d, pp);
    g_generation++;
    Log("device reset -> 0x%08X (%u objects tracked)", (unsigned)hr, (unsigned)g_objs.size());
    return hr;
}

void HookDevice(IDirect3DDevice8* dev) {
    void** vt = *(void***)dev;
    HookSlot(vt, DEV_Reset, (void*)&H_Reset);
    HookSlot(vt, DEV_Present, (void*)&H_Present);
    HookSlot(vt, DEV_CreateTexture, (void*)&H_CreateTexture);
    HookSlot(vt, DEV_CreateVolumeTexture, (void*)&H_CreateVolumeTexture);
    HookSlot(vt, DEV_CreateCubeTexture, (void*)&H_CreateCubeTexture);
    HookSlot(vt, DEV_CreateVertexBuffer, (void*)&H_CreateVertexBuffer);
    HookSlot(vt, DEV_CreateIndexBuffer, (void*)&H_CreateIndexBuffer);
    HookSlot(vt, DEV_CreateRenderTarget, (void*)&H_CreateRenderTarget);
    HookSlot(vt, DEV_CreateDepthStencilSurface, (void*)&H_CreateDepthStencilSurface);
    HookSlot(vt, DEV_CreateImageSurface, (void*)&H_CreateImageSurface);
    HookSlot(vt, DEV_CreateVertexShader, (void*)&H_CreateVertexShader);
    HookSlot(vt, DEV_CreatePixelShader, (void*)&H_CreatePixelShader);
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS8*,
                                                   IDirect3DDevice8**);
// IDirect3D8::CreateDevice is hooked in front of whatever is in the vtable slot each time an IDirect3D8 is made:
// other hooks come and go there (Windows' compatibility layer re-hooks it between the game's two Direct3DCreate8
// calls and goes straight to d3d8.dll; DxWnd hooks it after us and calls us). A call that comes back into our hook
// while it is already running (through DxWnd) goes straight to d3d8.dll's own function.
std::unordered_map<void**, CreateDevice_t> g_origCreateDevice;   // per vtable: what was in the slot before us
CreateDevice_t g_realCreateDevice = nullptr;                      // d3d8.dll's own
int g_inCreateDevice = 0;
IDirect3D8* (WINAPI* g_origDirect3DCreate8)(UINT) = nullptr;

HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D8* d3d, UINT ad, D3DDEVTYPE t, HWND wnd, DWORD fl,
                                         D3DPRESENT_PARAMETERS8* pp, IDirect3DDevice8** out) {
    auto it = g_origCreateDevice.find(*(void***)d3d);
    CreateDevice_t next = it != g_origCreateDevice.end() ? it->second : g_realCreateDevice;
    if (g_inCreateDevice || !next) {
        if (!g_realCreateDevice) return E_FAIL;
        return g_realCreateDevice(d3d, ad, t, wnd, fl, pp, out);
    }
    g_hwnd = wnd;
    if (forceWindowed) MakeWindowed(pp, wnd);
    g_inCreateDevice++;
    HRESULT hr = next(d3d, ad, t, wnd, fl, pp, out);
    g_inCreateDevice--;
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        LogDeviceSlots("before our hooks", g_dev);
        HookDevice(g_dev);
        LogDeviceSlots("after our hooks", g_dev);
        Log("CreateDevice ok: device %p vtable %p (%ux%u format %d windowed=%d flags %X)", g_dev, *(void**)g_dev,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? (int)pp->BackBufferFormat : 0,
            pp ? pp->Windowed : -1, fl);
    } else {
        Log("CreateDevice failed 0x%08X", (unsigned)hr);
    }
    return hr;
}

const char* ModName(void* fn) {
    static char buf[4][MAX_PATH];
    static int k = 0;
    char* b = buf[k++ & 3];
    HMODULE m = nullptr;
    strcpy_s(b, MAX_PATH, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fn, &m))
        GetModuleFileNameA(m, b, MAX_PATH);
    const char* s = strrchr(b, '\\');
    return s ? s + 1 : b;
}

void LogDeviceSlots(const char* when, IDirect3DDevice8* d) {
    if (!d) return;
    void** vt = *(void***)d;
    Log("device %p vtable %p %s: Reset %s, Present %s, CreateTexture %s, GetDepthStencilSurface %s", d, vt, when,
        ModName(vt[14]), ModName(vt[15]), ModName(vt[20]), ModName(vt[32]));
}

bool InD3d8(void* fn) {
    HMODULE m = nullptr, d3d8 = GetModuleHandleA("d3d8.dll");
    return d3d8 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      (LPCSTR)fn, &m) && m == d3d8;
}

IDirect3D8* WINAPI H_Direct3DCreate8(UINT sdk) {
    if (!g_origDirect3DCreate8) {
        HMODULE m = LoadLibraryA("d3d8.dll");
        g_origDirect3DCreate8 = m ? (decltype(g_origDirect3DCreate8))GetProcAddress(m, "Direct3DCreate8") : nullptr;
        if (!g_origDirect3DCreate8) return nullptr;
    }
    IDirect3D8* d3d = g_origDirect3DCreate8(sdk);
    static bool modesLogged = false;
    if (d3d && !modesLogged) {   // the display modes the game will choose from (its saved mode is an index into them)
        modesLogged = true;
        struct Mode { UINT w, h, refresh; DWORD format; } m;
        UINT n = d3d->GetAdapterModeCount(0);
        std::string line;
        for (UINT i = 0; i < n && i < 200; i++)
            if (SUCCEEDED(d3d->EnumAdapterModes(0, i, &m))) {
                char b[48];
                sprintf_s(b, " %u:%ux%u/%u/%u", i, m.w, m.h, (unsigned)m.format, m.refresh);
                line += b;
            }
        Log("display modes (%u):%s", n, line.c_str());
    }
    if (d3d) {
        g_d3d = d3d;
        void** vt = *(void***)d3d;
        void* cur = vt[D3D_CreateDevice];
        if (!g_realCreateDevice && InD3d8(cur)) g_realCreateDevice = (CreateDevice_t)cur;
        if (cur != (void*)&H_CreateDevice) {
            g_origCreateDevice[vt] = (CreateDevice_t)cur;
            Unprotect((uintptr_t)&vt[D3D_CreateDevice], 4);
            vt[D3D_CreateDevice] = (void*)&H_CreateDevice;
        }
        char mod[MAX_PATH] = "?";
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)cur, &m))
            GetModuleFileNameA(m, mod, MAX_PATH);
        Log("Direct3DCreate8(%u) = %p; CreateDevice was %s", sdk, d3d,
            cur == (void*)&H_CreateDevice ? "ours" : strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod);
    }
    return d3d;
}

// ------------------------------------------------------------------ device state as values
const uint8_t kRenderStates[] = {
    7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 40, 47, 48, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148,
    151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 166, 167, 168, 170, 171, 172, 173};
const uint8_t kStageStates[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                                26, 27, 28};
const D3DTRANSFORMSTATETYPE kTransforms[] = {D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_TEXTURE1,
    D3DTS_TEXTURE2, D3DTS_TEXTURE3, D3DTS_TEXTURE4, D3DTS_TEXTURE5, D3DTS_TEXTURE6, D3DTS_TEXTURE7, D3DTS_WORLD};
constexpr int NRS = sizeof(kRenderStates), NTSS = sizeof(kStageStates), NXF = 11;

}  // namespace

// Everything Get* can read back, with object pointers stored as values (they are mapped when applied).
struct DevState {
    DWORD rs[NRS]; uint8_t rsOk[NRS];
    DWORD tss[8][NTSS];
    uint32_t tex[8];
    struct { uint32_t vb; UINT stride; } streams[16];
    uint32_t ib, rt, ds;
    UINT baseVertex;
    DWORD vs, ps;
    D3DMATRIX xf[NXF];
    D3DVIEWPORT8 vp;
    D3DMATERIAL8 mat;
    D3DLIGHT8 light[16]; uint8_t lightOk[16]; BOOL lightOn[16];
    float clip[6][4];
    UINT palette;
};

namespace {

template <typename T> uint32_t Val(T* o) {
    if (o) o->Release();   // Get* added a reference; the value is all that is kept
    return (uint32_t)(uintptr_t)o;
}

DevState* RecordState(IDirect3DDevice8* d) {
    auto* s = new DevState();
    for (int i = 0; i < NRS; i++) s->rsOk[i] = SUCCEEDED(d->GetRenderState((D3DRENDERSTATETYPE)kRenderStates[i], &s->rs[i]));
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < NTSS; i++) d->GetTextureStageState(t, kStageStates[i], &s->tss[t][i]);
        IDirect3DBaseTexture8* tx = nullptr;
        d->GetTexture(t, &tx);
        s->tex[t] = Val(tx);
    }
    for (UINT i = 0; i < 16; i++) {
        IDirect3DVertexBuffer8* vb = nullptr;
        d->GetStreamSource(i, &vb, &s->streams[i].stride);
        s->streams[i].vb = Val(vb);
    }
    IDirect3DIndexBuffer8* ib = nullptr; d->GetIndices(&ib, &s->baseVertex); s->ib = Val(ib);
    d->GetVertexShader(&s->vs);
    d->GetPixelShader(&s->ps);
    IDirect3DSurface8* rt = nullptr; d->GetRenderTarget(&rt); s->rt = Val(rt);
    IDirect3DSurface8* ds = nullptr; d->GetDepthStencilSurface(&ds); s->ds = Val(ds);
    for (int i = 0; i < NXF; i++) d->GetTransform(kTransforms[i], &s->xf[i]);
    d->GetViewport(&s->vp);
    d->GetMaterial(&s->mat);
    for (DWORD i = 0; i < 16; i++) {
        s->lightOk[i] = SUCCEEDED(d->GetLight(i, &s->light[i]));
        if (s->lightOk[i]) d->GetLightEnable(i, &s->lightOn[i]);
    }
    for (DWORD i = 0; i < 6; i++) d->GetClipPlane(i, s->clip[i]);
    d->GetCurrentTexturePalette(&s->palette);
    return s;
}

int g_unmapped = 0;
template <typename T> T* Map(const Remap& m, uint32_t v) {
    if (!v) return nullptr;
    auto it = m.find(v);
    if (it == m.end()) { g_unmapped++; return nullptr; }
    return (T*)(uintptr_t)it->second;
}

void ApplyStateValues(IDirect3DDevice8* d, const DevState* s, const Remap& m) {
    g_unmapped = 0;
    for (int i = 0; i < NRS; i++)
        if (s->rsOk[i]) d->SetRenderState((D3DRENDERSTATETYPE)kRenderStates[i], s->rs[i]);
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < NTSS; i++) d->SetTextureStageState(t, kStageStates[i], s->tss[t][i]);
        d->SetTexture(t, Map<IDirect3DBaseTexture8>(m, s->tex[t]));
    }
    for (UINT i = 0; i < 16; i++) d->SetStreamSource(i, Map<IDirect3DVertexBuffer8>(m, s->streams[i].vb), s->streams[i].stride);
    d->SetIndices(Map<IDirect3DIndexBuffer8>(m, s->ib), s->baseVertex);
    d->SetVertexShader(s->vs);   // fixed-function: the FVF code
    d->SetPixelShader(s->ps);
    if (auto* rt = Map<IDirect3DSurface8>(m, s->rt)) d->SetRenderTarget(rt, Map<IDirect3DSurface8>(m, s->ds));
    for (int i = 0; i < NXF; i++) d->SetTransform(kTransforms[i], &s->xf[i]);
    d->SetViewport(&s->vp);
    d->SetMaterial(&s->mat);
    for (DWORD i = 0; i < 16; i++)
        if (s->lightOk[i]) { d->SetLight(i, &s->light[i]); d->LightEnable(i, s->lightOn[i]); }
    for (DWORD i = 0; i < 6; i++) d->SetClipPlane(i, s->clip[i]);
    d->SetCurrentTexturePalette(s->palette);
}

// ------------------------------------------------------------------ object contents
UINT BytesPerPixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_V16U16:
    case D3DFMT_Q8W8V8U8: case D3DFMT_X8L8V8U8: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_A8P8: return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: return 1;
    default: return 0;
    }
}

// Row size and row count of one locked level (block-compressed formats: rows of 4x4 blocks).
bool Layout(D3DFORMAT f, UINT w, UINT h, UINT* rowBytes, UINT* rows) {
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (!bw) bw = 1;
    if (!bh) bh = 1;
    switch (f) {
    case D3DFMT_DXT1: *rowBytes = bw * 8; *rows = bh; return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *rowBytes = bw * 16; *rows = bh; return true;
    default: {
        UINT bpp = BytesPerPixel(f);
        *rowBytes = w * bpp;
        *rows = h;
        return bpp != 0;
    }
    }
}

// Entry of the persistent catalog.
struct Entry {
    uint32_t oldPtr;
    uint8_t  kind, pad[3];
    uint32_t gameRefs;
    uint32_t p[8];       // creation parameters (see DescribeObj)
    uint32_t content;    // bytes of content that follow
};

std::map<D3DFORMAT, int> g_noContent;   // textures written without contents, by format (unknown layout)

// Creation parameters and content size. Content is kept for everything the device cannot rebuild by itself:
// managed / system-memory textures and image surfaces, all vertex and index buffers (dynamic ones too).
// Render targets are redrawn every frame.
bool DescribeObj(IUnknown* o, const Obj& info, Entry& e) {
    memset(e.p, 0, sizeof(e.p));
    e.content = 0;
    switch (info.kind) {
    case K_Texture: {
        auto* t = (IDirect3DTexture8*)o;
        D3DSURFACE_DESC8 sd;
        if (FAILED(t->GetLevelDesc(0, &sd))) return false;
        DWORD levels = t->GetLevelCount();
        uint32_t p[] = {sd.Width, sd.Height, levels, sd.Usage, (uint32_t)sd.Format, (uint32_t)sd.Pool};
        memcpy(e.p, p, sizeof(p));
        if (sd.Pool != D3DPOOL_DEFAULT)
            for (DWORD l = 0; l < levels; l++) {
                UINT rb, rows;
                t->GetLevelDesc(l, &sd);
                if (!Layout(sd.Format, sd.Width, sd.Height, &rb, &rows)) { e.content = 0; g_noContent[sd.Format]++; break; }
                e.content += rb * rows;
            }
        return true;
    }
    case K_CubeTexture: {
        auto* t = (IDirect3DCubeTexture8*)o;
        D3DSURFACE_DESC8 sd;
        if (FAILED(t->GetLevelDesc(0, &sd))) return false;
        DWORD levels = t->GetLevelCount();
        uint32_t p[] = {sd.Width, levels, sd.Usage, (uint32_t)sd.Format, (uint32_t)sd.Pool};
        memcpy(e.p, p, sizeof(p));
        if (sd.Pool != D3DPOOL_DEFAULT)
            for (DWORD l = 0; l < levels; l++) {
                UINT rb, rows;
                t->GetLevelDesc(l, &sd);
                if (!Layout(sd.Format, sd.Width, sd.Height, &rb, &rows)) { e.content = 0; g_noContent[sd.Format]++; break; }
                e.content += 6 * rb * rows;
            }
        return true;
    }
    case K_VolumeTexture: {
        auto* t = (IDirect3DVolumeTexture8*)o;
        D3DVOLUME_DESC8 vd;
        if (FAILED(t->GetLevelDesc(0, &vd))) return false;
        uint32_t p[] = {vd.Width, vd.Height, vd.Depth, t->GetLevelCount(), vd.Usage, (uint32_t)vd.Format, (uint32_t)vd.Pool};
        memcpy(e.p, p, sizeof(p));
        return true;   // contents are not kept (the games create none)
    }
    case K_VertexBuffer: {
        D3DVERTEXBUFFER_DESC8 d;
        if (FAILED(((IDirect3DVertexBuffer8*)o)->GetDesc(&d))) return false;
        uint32_t p[] = {d.Size, d.Usage, d.FVF, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        e.content = d.Size;
        return true;
    }
    case K_IndexBuffer: {
        D3DINDEXBUFFER_DESC8 d;
        if (FAILED(((IDirect3DIndexBuffer8*)o)->GetDesc(&d))) return false;
        uint32_t p[] = {d.Size, d.Usage, (uint32_t)d.Format, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        e.content = d.Size;
        return true;
    }
    case K_RenderTarget: case K_DepthStencil: case K_ImageSurface: {
        D3DSURFACE_DESC8 d;
        if (FAILED(((IDirect3DSurface8*)o)->GetDesc(&d))) return false;
        auto lk = g_lockable.find(o);
        uint32_t p[] = {d.Width, d.Height, (uint32_t)d.Format, (uint32_t)d.MultiSampleType,
                        lk != g_lockable.end() ? (uint32_t)lk->second : 0, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        if (info.kind == K_ImageSurface) {
            UINT rb, rows;
            if (Layout(d.Format, d.Width, d.Height, &rb, &rows)) e.content = rb * rows;
        }
        return true;
    }
    default:
        return false;
    }
}

std::vector<uint8_t> g_zero;
const HeldLock* HeldLevel(IUnknown* tex, UINT level);

void PutRows(Sink& out, const void* bits, INT pitch, UINT rowBytes, UINT rows) {
    if ((UINT)pitch == rowBytes) { out.Put(bits, (size_t)rowBytes * rows); return; }
    for (UINT y = 0; y < rows; y++) out.Put((const uint8_t*)bits + (size_t)y * pitch, rowBytes);
}

void PutZero(Sink& out, size_t n) {
    if (g_zero.size() < 65536) g_zero.assign(65536, 0);
    while (n) { size_t k = n < g_zero.size() ? n : g_zero.size(); out.Put(g_zero.data(), k); n -= k; }
}

// Dynamic buffers usually live in write-combined memory, where ordinary reads are very slow; SSE4.1 streaming
// loads read it a cache line at a time.
std::vector<uint8_t> g_wcScratch;

void CopyFromWC(void* dst, const void* src, size_t n) {
    static const bool sse41 = IsProcessorFeaturePresent(37 /* PF_SSE4_1_INSTRUCTIONS_AVAILABLE */) != 0;
    auto* s = (const uint8_t*)src;
    auto* d = (uint8_t*)dst;
    if (sse41) {
        size_t head = (16 - ((uintptr_t)s & 15)) & 15;
        if (head > n) head = n;
        memcpy(d, s, head);
        s += head; d += head; n -= head;
        _mm_mfence();
        for (; n >= 64; n -= 64, s += 64, d += 64) {
            __m128i a = _mm_stream_load_si128((__m128i*)s), b = _mm_stream_load_si128((__m128i*)(s + 16));
            __m128i c = _mm_stream_load_si128((__m128i*)(s + 32)), x = _mm_stream_load_si128((__m128i*)(s + 48));
            _mm_storeu_si128((__m128i*)d, a);
            _mm_storeu_si128((__m128i*)(d + 16), b);
            _mm_storeu_si128((__m128i*)(d + 32), c);
            _mm_storeu_si128((__m128i*)(d + 48), x);
        }
        for (; n >= 16; n -= 16, s += 16, d += 16) _mm_storeu_si128((__m128i*)d, _mm_stream_load_si128((__m128i*)s));
    }
    memcpy(d, s, n);
}

void WriteContent(Sink& out, IUnknown* o, const Obj& info, const Entry& e) {
    if (!e.content) return;
    size_t written = 0;
    switch (info.kind) {
    case K_Texture: {
        auto* t = (IDirect3DTexture8*)o;
        for (DWORD l = 0; l < e.p[2]; l++) {
            D3DSURFACE_DESC8 sd;
            UINT rb, rows;
            t->GetLevelDesc(l, &sd);
            Layout(sd.Format, sd.Width, sd.Height, &rb, &rows);
            D3DLOCKED_RECT lr;
            if (const HeldLock* held = HeldLevel(o, l)) {
                PutRows(out, (const void*)(uintptr_t)held->bits, held->pitch, rb, rows);
            } else if (SUCCEEDED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY))) {
                PutRows(out, lr.pBits, lr.Pitch, rb, rows);
                t->UnlockRect(l);
            } else {
                PutZero(out, (size_t)rb * rows);
            }
            written += (size_t)rb * rows;
        }
        break;
    }
    case K_CubeTexture: {
        auto* t = (IDirect3DCubeTexture8*)o;
        for (int f = 0; f < 6; f++)
            for (DWORD l = 0; l < e.p[1]; l++) {
                D3DSURFACE_DESC8 sd;
                UINT rb, rows;
                t->GetLevelDesc(l, &sd);
                Layout(sd.Format, sd.Width, sd.Height, &rb, &rows);
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, D3DLOCK_READONLY))) {
                    PutRows(out, lr.pBits, lr.Pitch, rb, rows);
                    t->UnlockRect((D3DCUBEMAP_FACES)f, l);
                } else {
                    PutZero(out, (size_t)rb * rows);
                }
                written += (size_t)rb * rows;
            }
        break;
    }
    case K_VertexBuffer: case K_IndexBuffer: {
        BYTE* p = nullptr;
        // between two frames a dynamic buffer holds its final data: read it without waiting for the GPU
        DWORD fl = D3DLOCK_READONLY | (e.p[1] & D3DUSAGE_DYNAMIC ? D3DLOCK_NOOVERWRITE : 0);
        HRESULT hr = info.kind == K_VertexBuffer ? ((IDirect3DVertexBuffer8*)o)->Lock(0, 0, &p, fl)
                                                 : ((IDirect3DIndexBuffer8*)o)->Lock(0, 0, &p, fl);
        if (SUCCEEDED(hr)) {
            if (e.p[3] == D3DPOOL_DEFAULT) {
                if (g_wcScratch.size() < e.content) g_wcScratch.resize(e.content);
                CopyFromWC(g_wcScratch.data(), p, e.content);
                out.Put(g_wcScratch.data(), e.content);
            } else {
                out.Put(p, e.content);
            }
            if (info.kind == K_VertexBuffer) ((IDirect3DVertexBuffer8*)o)->Unlock();
            else ((IDirect3DIndexBuffer8*)o)->Unlock();
        } else {
            PutZero(out, e.content);
        }
        written = e.content;
        break;
    }
    case K_ImageSurface: {
        auto* sf = (IDirect3DSurface8*)o;
        UINT rb, rows;
        Layout((D3DFORMAT)e.p[2], e.p[0], e.p[1], &rb, &rows);
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            PutRows(out, lr.pBits, lr.Pitch, rb, rows);
            sf->UnlockRect();
            written = (size_t)rb * rows;
        }
        break;
    }
    default: break;
    }
    if (written < e.content) PutZero(out, e.content - written);
}

constexpr uint32_t GPU_MAGIC = 0x38555047;   // "GPU8"
std::vector<uint8_t> g_skip;
DevState* g_pendingState = nullptr;

// Locks the game holds right now on tracked objects: texture levels (through the level's surface or the texture) and
// standalone surfaces.
std::vector<HeldLock> RecordLocks() {
    std::vector<HeldLock> out;
    EnterCriticalSection(&g_cs);
    for (auto& [sf, rec] : g_surfLocks) {
        if (!g_objs.count(rec.container)) continue;
        HeldLock h = {};
        h.surface = (uint32_t)(uintptr_t)sf;
        h.container = (uint32_t)(uintptr_t)rec.container;
        h.rect = rec.rect; h.whole = rec.whole; h.flags = rec.flags;
        h.bits = (uint32_t)(uintptr_t)rec.bits; h.pitch = rec.pitch;
        h.level = ~0u;
        if (rec.container != sf) {
            auto* tex = (IDirect3DTexture8*)rec.container;
            for (DWORD l = 0; l < tex->GetLevelCount(); l++) {
                IDirect3DSurface8* x = nullptr;
                if (SUCCEEDED(tex->GetSurfaceLevel(l, &x)) && x) {
                    if ((IUnknown*)x == sf) h.level = l;
                    x->Release();
                }
            }
            if (h.level == ~0u) continue;
        }
        out.push_back(h);
    }
    for (auto& [key, rec] : g_texLocks) {
        if (!g_objs.count(key.first)) continue;
        HeldLock h = {};
        h.container = (uint32_t)(uintptr_t)key.first;
        h.level = key.second;
        h.rect = rec.rect; h.whole = rec.whole; h.flags = rec.flags;
        h.bits = (uint32_t)(uintptr_t)rec.bits; h.pitch = rec.pitch;
        out.push_back(h);
    }
    LeaveCriticalSection(&g_cs);
    return out;
}

// A whole level the game holds locked cannot be locked again for reading: its pixels come from the game's lock.
const HeldLock* g_writeLocks = nullptr;
size_t g_writeLockCount = 0;
const HeldLock* HeldLevel(IUnknown* tex, UINT level) {
    for (size_t i = 0; i < g_writeLockCount; i++)
        if (g_writeLocks[i].container == (uint32_t)(uintptr_t)tex && g_writeLocks[i].level == level && g_writeLocks[i].whole)
            return &g_writeLocks[i];
    return nullptr;
}

bool Skip(Source& in, size_t n) {
    if (g_skip.size() < 65536) g_skip.resize(65536);
    while (n) {
        size_t k = n < g_skip.size() ? n : g_skip.size();
        if (!in.Get(g_skip.data(), k)) return false;
        n -= k;
    }
    return true;
}

bool GetRows(Source& in, void* bits, INT pitch, UINT rowBytes, UINT rows) {
    if ((UINT)pitch == rowBytes) return in.Get(bits, (size_t)rowBytes * rows);
    for (UINT y = 0; y < rows; y++)
        if (!in.Get((uint8_t*)bits + (size_t)y * pitch, rowBytes)) return false;
    return true;
}

// Create one object from its entry and read its content; nullptr if it could not be created (content skipped).
IUnknown* CreateFromEntry(Source& in, const Entry& e, bool* readOk) {
    IDirect3DDevice8* d = g_dev;
    IUnknown* obj = nullptr;
    size_t consumed = 0;
    *readOk = true;
    switch (e.kind) {
    case K_Texture: {
        IDirect3DTexture8* t = nullptr;
        if (FAILED(Orig<decltype(&H_CreateTexture)>(DEV_CreateTexture)(d, e.p[0], e.p[1], e.p[2], e.p[3],
                (D3DFORMAT)e.p[4], (D3DPOOL)e.p[5], &t))) break;
        obj = (IUnknown*)t;
        if (!e.content) break;
        for (DWORD l = 0; l < e.p[2] && *readOk; l++) {
            D3DSURFACE_DESC8 sd;
            UINT rb, rows;
            t->GetLevelDesc(l, &sd);
            Layout(sd.Format, sd.Width, sd.Height, &rb, &rows);
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
                *readOk = GetRows(in, lr.pBits, lr.Pitch, rb, rows);
                t->UnlockRect(l);
            } else {
                *readOk = Skip(in, (size_t)rb * rows);
            }
            consumed += (size_t)rb * rows;
        }
        break;
    }
    case K_CubeTexture: {
        IDirect3DCubeTexture8* t = nullptr;
        if (FAILED(Orig<decltype(&H_CreateCubeTexture)>(DEV_CreateCubeTexture)(d, e.p[0], e.p[1], e.p[2],
                (D3DFORMAT)e.p[3], (D3DPOOL)e.p[4], &t))) break;
        obj = (IUnknown*)t;
        if (!e.content) break;
        for (int f = 0; f < 6; f++)
            for (DWORD l = 0; l < e.p[1] && *readOk; l++) {
                D3DSURFACE_DESC8 sd;
                UINT rb, rows;
                t->GetLevelDesc(l, &sd);
                Layout(sd.Format, sd.Width, sd.Height, &rb, &rows);
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0))) {
                    *readOk = GetRows(in, lr.pBits, lr.Pitch, rb, rows);
                    t->UnlockRect((D3DCUBEMAP_FACES)f, l);
                } else {
                    *readOk = Skip(in, (size_t)rb * rows);
                }
                consumed += (size_t)rb * rows;
            }
        break;
    }
    case K_VolumeTexture: {
        IDirect3DVolumeTexture8* t = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateVolumeTexture)>(DEV_CreateVolumeTexture)(d, e.p[0], e.p[1], e.p[2], e.p[3],
                e.p[4], (D3DFORMAT)e.p[5], (D3DPOOL)e.p[6], &t))) obj = (IUnknown*)t;
        break;
    }
    case K_VertexBuffer: {
        IDirect3DVertexBuffer8* b = nullptr;
        if (FAILED(Orig<decltype(&H_CreateVertexBuffer)>(DEV_CreateVertexBuffer)(d, e.p[0], e.p[1], e.p[2],
                (D3DPOOL)e.p[3], &b))) break;
        obj = (IUnknown*)b;
        BYTE* p = nullptr;
        if (e.content && SUCCEEDED(b->Lock(0, 0, &p, 0))) {
            *readOk = in.Get(p, e.content);
            b->Unlock();
            consumed = e.content;
        }
        break;
    }
    case K_IndexBuffer: {
        IDirect3DIndexBuffer8* b = nullptr;
        if (FAILED(Orig<decltype(&H_CreateIndexBuffer)>(DEV_CreateIndexBuffer)(d, e.p[0], e.p[1], (D3DFORMAT)e.p[2],
                (D3DPOOL)e.p[3], &b))) break;
        obj = (IUnknown*)b;
        BYTE* p = nullptr;
        if (e.content && SUCCEEDED(b->Lock(0, 0, &p, 0))) {
            *readOk = in.Get(p, e.content);
            b->Unlock();
            consumed = e.content;
        }
        break;
    }
    case K_RenderTarget: {
        IDirect3DSurface8* sf = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateRenderTarget)>(DEV_CreateRenderTarget)(d, e.p[0], e.p[1], (D3DFORMAT)e.p[2],
                (D3DMULTISAMPLE_TYPE)e.p[3], e.p[4], &sf))) {
            obj = (IUnknown*)sf;
            g_lockable[obj] = e.p[4];
        }
        break;
    }
    case K_DepthStencil: {
        IDirect3DSurface8* sf = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateDepthStencilSurface)>(DEV_CreateDepthStencilSurface)(d, e.p[0], e.p[1],
                (D3DFORMAT)e.p[2], (D3DMULTISAMPLE_TYPE)e.p[3], &sf))) obj = (IUnknown*)sf;
        break;
    }
    case K_ImageSurface: {
        IDirect3DSurface8* sf = nullptr;
        if (FAILED(Orig<decltype(&H_CreateImageSurface)>(DEV_CreateImageSurface)(d, e.p[0], e.p[1], (D3DFORMAT)e.p[2], &sf)))
            break;
        obj = (IUnknown*)sf;
        D3DLOCKED_RECT lr;
        UINT rb, rows;
        if (e.content && Layout((D3DFORMAT)e.p[2], e.p[0], e.p[1], &rb, &rows) && SUCCEEDED(sf->LockRect(&lr, nullptr, 0))) {
            *readOk = GetRows(in, lr.pBits, lr.Pitch, rb, rows);
            sf->UnlockRect();
            consumed = (size_t)rb * rows;
        }
        break;
    }
    default: break;
    }
    if (*readOk && consumed < e.content) *readOk = Skip(in, e.content - consumed);
    return obj;
}

}  // namespace

bool InstallHooks() {
    static bool init = false;
    if (!init) { InitializeCriticalSection(&g_cs); g_objs.reserve(16384); init = true; }
    HMODULE exe = GetModuleHandleA(nullptr);
    void* old = PatchIat(exe, "d3d8.dll", "Direct3DCreate8", (void*)&H_Direct3DCreate8);
    if (old && old != (void*)&H_Direct3DCreate8) g_origDirect3DCreate8 = (decltype(g_origDirect3DCreate8))old;
    Log("d3d hook: Direct3DCreate8 %s", old ? "import patched" : "NOT FOUND");
    return old != nullptr;
}

IDirect3DDevice8* Device() { return g_dev; }
void LogSlots(const char* when) { LogDeviceSlots(when, g_dev); }
IDirect3D8* Direct3D() { return g_d3d; }

bool DeviceUsable() { return g_dev && g_dev->TestCooperativeLevel() == S_OK; }

uint32_t Generation() { return g_generation; }
uint32_t ShaderCount() { return g_shaders; }

void Track(IUnknown* obj, Kind kind, bool defaultPool) { TrackObj(obj, kind, defaultPool); }

const char* KindName(int k) {
    static const char* names[K_Count] = {"texture", "volume texture", "cube texture", "vertex buffer", "index buffer",
                                         "render target", "depth stencil", "image surface"};
    return k >= 0 && k < K_Count ? names[k] : "?";
}

std::vector<std::pair<IUnknown*, Kind>> TrackedObjects() {
    EnterCriticalSection(&g_cs);
    std::vector<std::pair<IUnknown*, Kind>> v;
    v.reserve(g_objs.size());
    for (auto& [obj, o] : g_objs) v.emplace_back(obj, o.kind);
    LeaveCriticalSection(&g_cs);
    return v;
}

void Collect() {
    EnterCriticalSection(&g_cs);
    for (auto it = g_objs.begin(); it != g_objs.end();) {
        if (it->second.snapRefs == 0 && RefCount(it->first) <= 1) {
            if (!g_surfLocks.empty() || !g_texLocks.empty()) ForgetLocks(it->first);
            g_lockable.erase(it->first);
            g_ourRelease++;
            it->first->Release();
            g_ourRelease--;
            it = g_objs.erase(it);
        } else {
            ++it;
        }
    }
    LeaveCriticalSection(&g_cs);
}

size_t TrackedCount() { return g_objs.size(); }

// A savestate pins what its memory can use: objects the game holds references to, and objects bound to the device
// (re-bound by the state block on load).
Capture* Save() {
    Collect();
    auto* c = new Capture;
    c->generation = g_generation;
    c->dev = RecordState(g_dev);
    std::unordered_set<IUnknown*> bound;
    for (uint32_t v : c->dev->tex) bound.insert((IUnknown*)(uintptr_t)v);
    for (auto& s : c->dev->streams) bound.insert((IUnknown*)(uintptr_t)s.vb);
    for (uint32_t v : {c->dev->ib, c->dev->rt, c->dev->ds}) bound.insert((IUnknown*)(uintptr_t)v);
    EnterCriticalSection(&g_cs);
    c->refs.reserve(g_objs.size());
    for (auto& [obj, o] : g_objs) {
        uint32_t refs = RefCount(obj) - 1;
        if (!refs && !bound.count(obj)) continue;
        c->refs.emplace_back(obj, refs);
        o.snapRefs++;
    }
    LeaveCriticalSection(&g_cs);
    if (FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &c->state))) c->state = 0;
    c->locks = RecordLocks();
    return c;
}

void Load(const Capture* c) {
    std::unordered_map<IUnknown*, uint32_t> want(c->refs.begin(), c->refs.end());
    EnterCriticalSection(&g_cs);
    uint32_t adjusted = 0;
    for (auto& [obj, o] : g_objs) {
        auto it = want.find(obj);
        uint32_t target = it == want.end() ? 0 : it->second;
        ULONG cur = obj->AddRef() - 2;   // minus our probe and our pin
        if (cur != target) adjusted++;
        for (; cur < target; cur++) obj->AddRef();
        g_ourRelease++;
        for (; cur > target; cur--) obj->Release();
        g_ourRelease--;
        obj->Release();
    }
    LeaveCriticalSection(&g_cs);
    if (c->state) g_dev->ApplyStateBlock(c->state);
    Log("d3d load: %u tracked, %u in savestate, %u refcounts adjusted", (unsigned)g_objs.size(),
        (unsigned)c->refs.size(), adjusted);
}

void Release(Capture* c) {
    if (!c) return;
    EnterCriticalSection(&g_cs);
    for (auto& [obj, n] : c->refs) {
        auto it = g_objs.find(obj);
        if (it != g_objs.end() && it->second.snapRefs) it->second.snapRefs--;
    }
    LeaveCriticalSection(&g_cs);
    if (c->state && g_dev && c->generation == g_generation) g_dev->DeleteStateBlock(c->state);
    delete c->dev;
    delete c;
}

void Write(const Capture* c, Sink& out) {
    uint32_t skipped = 0;
    EnterCriticalSection(&g_cs);
    std::vector<std::pair<IUnknown*, Entry>> entries;
    entries.reserve(c->refs.size());
    for (auto& [obj, refs] : c->refs) {
        auto it = g_objs.find(obj);
        if (it == g_objs.end()) continue;
        Entry e = {};
        e.oldPtr = (uint32_t)(uintptr_t)obj;
        e.kind = (uint8_t)it->second.kind;
        e.gameRefs = refs;
        if (!DescribeObj(obj, it->second, e)) { skipped++; continue; }
        entries.emplace_back(obj, e);
    }
    uint32_t n = (uint32_t)entries.size();
    g_writeLocks = c->locks.data();
    g_writeLockCount = c->locks.size();
    out.Put(&GPU_MAGIC, 4);
    out.Put(&n, 4);
    uint64_t bytes = 0, pos = 8;
    c->dynamic.clear();
    for (auto& [obj, e] : entries) {
        out.Put(&e, sizeof(e));
        pos += sizeof(e);
        // every vertex buffer (RenderWare sub-allocates its static ones too and reuses freed parts), dynamic index buffers
        bool changes = e.kind == K_VertexBuffer || (e.kind == K_IndexBuffer && (e.p[1] & D3DUSAGE_DYNAMIC));
        if (changes && e.content) c->dynamic.push_back({obj, pos, e.content});
        WriteContent(out, obj, g_objs[obj], e);   // exactly e.content bytes
        pos += e.content;
        bytes += e.content;
    }
    LeaveCriticalSection(&g_cs);
    g_writeLockCount = 0;
    uint32_t sz = c->dev ? (uint32_t)sizeof(DevState) : 0;
    out.Put(&sz, 4);
    if (sz) out.Put(c->dev, sz);
    std::vector<HeldLock> locks;   // only locks on written objects
    for (auto& h : c->locks)
        for (auto& [obj, e] : entries)
            if (e.oldPtr == h.container) { locks.push_back(h); break; }
    uint32_t nl = (uint32_t)locks.size();
    out.Put(&nl, 4);
    if (nl) out.Put(locks.data(), nl * sizeof(HeldLock));
    if (skipped) Log("d3d write: %u objects of unsupported kinds left out", skipped);
    for (auto& [f, k] : g_noContent) Log("d3d write: %d textures of format %d written without contents", k, (int)f);
    g_noContent.clear();
    int byKind[K_Count] = {}, refSum = 0;
    for (auto& [obj, e] : entries) { byKind[e.kind]++; refSum += e.gameRefs; }
    char kinds[256] = "";
    for (int k = 0; k < K_Count; k++)
        if (byKind[k]) { char t[40]; sprintf_s(t, " %s %d,", KindName(k), byKind[k]); strcat_s(kinds, t); }
    Log("d3d write: %u objects (%d game references, %u tracked, %u held locks):%s %.1f MB of contents", n, refSum,
        (unsigned)g_objs.size(), nl, kinds, bytes / 1048576.0);
}

void RestoreDynamic(const Capture* c, HANDLE file, uint64_t gpuOffset) {
    uint64_t bytes = 0;
    uint32_t done = 0;
    for (auto& d : c->dynamic) {
        if (!g_objs.count(d.obj)) continue;
        auto it = g_objs.find(d.obj);
        void* p = nullptr;
        DWORD fl = 0;   // DISCARD only for dynamic buffers (the whole buffer is rewritten either way)
        if (it->second.kind == K_VertexBuffer) {
            D3DVERTEXBUFFER_DESC8 vd;
            if (SUCCEEDED(((IDirect3DVertexBuffer8*)d.obj)->GetDesc(&vd)) && (vd.Usage & D3DUSAGE_DYNAMIC)) fl = D3DLOCK_DISCARD;
        } else {
            fl = D3DLOCK_DISCARD;
        }
        HRESULT hr = it->second.kind == K_VertexBuffer ? ((IDirect3DVertexBuffer8*)d.obj)->Lock(0, 0, (BYTE**)&p, fl)
                                                       : ((IDirect3DIndexBuffer8*)d.obj)->Lock(0, 0, (BYTE**)&p, fl);
        if (FAILED(hr) || !p) continue;
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)(gpuOffset + d.offset);
        DWORD got = 0;
        bool ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, p, d.size, &got, nullptr) && got == d.size;
        if (it->second.kind == K_VertexBuffer) ((IDirect3DVertexBuffer8*)d.obj)->Unlock();
        else ((IDirect3DIndexBuffer8*)d.obj)->Unlock();
        if (ok) { done++; bytes += d.size; }
    }
    Log("d3d load: %u vertex/dynamic buffers rewritten (%.1f MB)", done, bytes / 1048576.0);
}

bool Recreate(Source& in, Remap& remap, std::string& err) {
    uint32_t magic = 0, n = 0;
    if (!in.Get(&magic, 4) || magic != GPU_MAGIC || !in.Get(&n, 4)) { err = "graphics section damaged"; return false; }
    // The game's references to its current objects lived in the memory that was just replaced. (Locks stay: a lock
    // belongs to the object, which other savestates of this process may still refer to; it goes when the object goes.)
    EnterCriticalSection(&g_cs);
    uint32_t dropped = 0;
    g_ourRelease++;
    for (auto& [obj, o] : g_objs) {
        ULONG cur = obj->AddRef() - 2;   // minus our probe and our pin
        for (; cur > 0; cur--, dropped++) obj->Release();
        obj->Release();
    }
    g_ourRelease--;
    uint32_t created = 0, failed = 0;
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        Entry e;
        if (!in.Get(&e, sizeof(e))) { ok = false; break; }
        bool readOk = true;
        IUnknown* obj = CreateFromEntry(in, e, &readOk);
        ok = readOk;
        if (!obj) { failed++; continue; }
        TrackObj(obj, (Kind)e.kind, e.kind == K_RenderTarget || e.kind == K_DepthStencil ||
                                    ((e.kind == K_Texture && e.p[5] == D3DPOOL_DEFAULT)));
        // created with one reference, which becomes one of the game's
        if (e.gameRefs == 0) { g_ourRelease++; obj->Release(); g_ourRelease--; }
        for (uint32_t k = 1; k < e.gameRefs; k++) obj->AddRef();
        remap[e.oldPtr] = (uint32_t)(uintptr_t)obj;
        created++;
    }
    LeaveCriticalSection(&g_cs);
    uint32_t sz = 0;
    delete g_pendingState;
    g_pendingState = nullptr;
    if (ok && in.Get(&sz, 4) && sz == sizeof(DevState)) {
        g_pendingState = new DevState;
        ok = in.Get(g_pendingState, sz);
    }
    uint32_t nl = 0, relocked = 0;
    std::vector<HeldLock> locks;
    if (ok && in.Get(&nl, 4) && nl < 100000) {
        locks.resize(nl);
        ok = !nl || in.Get(locks.data(), nl * sizeof(HeldLock));
    }
    for (auto& h : locks) {
        auto it = remap.find(h.container);
        if (!ok || it == remap.end()) continue;
        auto* obj = (IUnknown*)(uintptr_t)it->second;
        D3DLOCKED_RECT lr = {};
        const RECT* r = h.whole ? nullptr : &h.rect;
        if (h.level == ~0u) {
            if (FAILED(((IDirect3DSurface8*)obj)->LockRect(&lr, r, h.flags))) continue;
        } else if (h.surface) {
            // the game holds the level's surface (one reference on the texture, already counted in its references)
            IDirect3DSurface8* sf = nullptr;
            if (FAILED(((IDirect3DTexture8*)obj)->GetSurfaceLevel(h.level, &sf)) || !sf) continue;
            ((IDirect3DTexture8*)obj)->Release();
            if (FAILED(sf->LockRect(&lr, r, h.flags))) continue;
            remap[h.surface] = (uint32_t)(uintptr_t)sf;
        } else if (FAILED(((IDirect3DTexture8*)obj)->LockRect(h.level, &lr, r, h.flags))) {
            continue;
        }
        remap[h.bits] = (uint32_t)(uintptr_t)lr.pBits;
        relocked++;
    }
    if (nl) Log("d3d recreate: %u of %u held locks re-established", relocked, nl);
    Log("d3d recreate: %u objects created, %u failed, %u references of the previous game state dropped%s", created,
        failed, dropped, ok ? "" : ", FILE TRUNCATED");
    if (!ok) err = "graphics section truncated";
    else if (failed) err = std::to_string(failed) + " graphics objects could not be re-created";
    return ok;
}

void ApplyState(const Remap& remap) {
    if (!g_pendingState) return;
    ApplyStateValues(g_dev, g_pendingState, remap);
    Log("d3d state applied (%d object references without a match)", g_unmapped);
    delete g_pendingState;
    g_pendingState = nullptr;
}

void Screenshot(const char* path) { g_shotPath = path; }

void ShowText(const char* text, DWORD ms) {
    g_text = text;
    g_textUntil = GetTickCount() + ms;
}

}  // namespace d3d
