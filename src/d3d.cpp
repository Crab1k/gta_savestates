#include "d3d.h"
#include <smmintrin.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdio>

namespace d3d {

void (*onDeviceReset)() = nullptr;
bool releaseCheck = false;
bool forceWindowed = false;
int windowX = 40, windowY = 40;

namespace {

HWND g_hwnd = nullptr;

void MakeWindowed(D3DPRESENT_PARAMETERS* pp, HWND hwnd) {
    if (!pp) return;
    if (!hwnd) hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : g_hwnd;
    g_hwnd = hwnd;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN || pp->BackBufferFormat == D3DFMT_A8R8G8B8)
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if (hwnd && pp->BackBufferWidth && pp->BackBufferHeight) {
        LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        SetWindowLongA(hwnd, GWL_STYLE, style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
        RECT r = {0, 0, (LONG)pp->BackBufferWidth, (LONG)pp->BackBufferHeight};
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, windowX, windowY, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

// IDirect3D9 / IDirect3DDevice9 vtable slots (d3d9.h order; 23/26/27/65 match the game's own call offsets).
enum : int {
    D3D_CreateDevice = 16,
    DEV_CreateAdditionalSwapChain = 13, DEV_Reset = 16, DEV_Present = 17,
    DEV_CreateTexture = 23, DEV_CreateVolumeTexture = 24, DEV_CreateCubeTexture = 25,
    DEV_CreateVertexBuffer = 26, DEV_CreateIndexBuffer = 27, DEV_CreateRenderTarget = 28,
    DEV_CreateDepthStencilSurface = 29, DEV_CreateOffscreenPlainSurface = 36,
    DEV_CreateStateBlock = 59, DEV_EndStateBlock = 61, DEV_CreateVertexDeclaration = 86,
    DEV_CreateVertexShader = 91, DEV_CreatePixelShader = 106, DEV_CreateQuery = 118,
};

struct Obj {
    Kind     kind;
    bool     defaultPool;
    bool     permanent;  // never collected (see PinForever)
    uint32_t snapRefs;   // captures that reference this object
    uint32_t flags;      // render target: lockable; depth stencil: discard
};

std::unordered_map<IUnknown*, Obj> g_objs;
CRITICAL_SECTION g_cs;
IDirect3DDevice9* g_dev = nullptr;
uint32_t g_generation = 1;
void** g_devVt = nullptr;
void* g_orig[128];

template <typename F> F Orig(int slot) { return (F)g_orig[slot]; }

void HookSlot(void** vt, int slot, void* fn) {
    if (vt[slot] == fn) return;
    g_orig[slot] = vt[slot];
    Unprotect((uintptr_t)&vt[slot], 4);
    vt[slot] = fn;
}

void HookRelease(IUnknown* obj);
void HookTextureVt(IUnknown* t);
void HookSurfaceVt(IUnknown* sf);

void TrackObj(IUnknown* obj, Kind kind, bool defaultPool, bool permanent, uint32_t flags = 0) {
    EnterCriticalSection(&g_cs);
    auto it = g_objs.find(obj);
    if (it == g_objs.end()) {
        obj->AddRef();   // the pin: the object outlives the game's last Release until Collect() sees it unused
        g_objs.emplace(obj, Obj{kind, defaultPool, permanent, 0, flags});
        HookRelease(obj);
        if (kind == K_Texture) HookTextureVt(obj);
        else if (kind == K_RenderTarget || kind == K_DepthStencil || kind == K_OffscreenPlain) HookSurfaceVt(obj);
    }
    LeaveCriticalSection(&g_cs);
}

void TrackIfGame(void* ret, IUnknown* obj, Kind kind, bool defaultPool, uint32_t flags = 0) {
    if (obj && InGameImage(ret)) TrackObj(obj, kind, defaultPool, false, flags);
}

// RenderWare caches vertex declarations by raw pointer: the cache itself holds no reference (each user gets one,
// AddRef on a cache hit), so a declaration must never be destroyed even when no user is left. Shaders are created
// once at startup. Their references are still reconciled on load like any other object's.
void PinForever(void* ret, IUnknown* obj, Kind kind) {
    if (obj && InGameImage(ret)) TrackObj(obj, kind, false, true);
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
            Log("OVER-RELEASE: %s object %p (vtable %p) reached 0 refs, released from %p",
                it->second.permanent ? "permanent" : "tracked", self, vt, _ReturnAddress());
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
// RenderWare keeps some rasters locked across frames (the number plate charset stays locked for the whole game), with
// the locked surface and the pixel pointer stored in game memory. A savestate records such locks, so that loading it
// in another process can lock the re-created texture and point the game at the new pixels.
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

HRESULT STDMETHODCALLTYPE H_SurfLock(IDirect3DSurface9* sf, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    HRESULT hr = VtOrig<decltype(&H_SurfLock)>(V_SurfLock, sf)(sf, lr, r, fl);
    if (SUCCEEDED(hr) && lr) {
        EnterCriticalSection(&g_cs);
        IUnknown* container = nullptr;
        if (g_objs.count(sf)) {
            container = sf;
        } else if (SUCCEEDED(sf->GetContainer(__uuidof(IDirect3DTexture9), (void**)&container)) && container) {
            container->Release();
            if (!g_objs.count(container)) container = nullptr;
        }
        if (container) g_surfLocks[sf] = LockRec{r ? *r : RECT{}, !r, fl, lr->pBits, lr->Pitch, container};
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_SurfUnlock(IDirect3DSurface9* sf) {
    EnterCriticalSection(&g_cs);
    g_surfLocks.erase(sf);
    LeaveCriticalSection(&g_cs);
    return VtOrig<decltype(&H_SurfUnlock)>(V_SurfUnlock, sf)(sf);
}

HRESULT STDMETHODCALLTYPE H_TexLock(IDirect3DTexture9* t, UINT l, D3DLOCKED_RECT* lr, const RECT* r, DWORD fl) {
    HRESULT hr = VtOrig<decltype(&H_TexLock)>(V_TexLock, t)(t, l, lr, r, fl);
    if (SUCCEEDED(hr) && lr) {
        EnterCriticalSection(&g_cs);
        g_texLocks[{t, l}] = LockRec{r ? *r : RECT{}, !r, fl, lr->pBits, lr->Pitch, t};
        // d3d9 locks the level's surface through its vtable but unlocks it directly: that record would never go
        for (auto it = g_surfLocks.begin(); it != g_surfLocks.end();)
            it = it->second.container == t && it->second.bits == lr->pBits ? g_surfLocks.erase(it) : std::next(it);
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_TexUnlock(IDirect3DTexture9* t, UINT l) {
    EnterCriticalSection(&g_cs);
    g_texLocks.erase({t, l});
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
    HookVt(vt, V_SurfLock, 13, (void*)&H_SurfLock);
    HookVt(vt, V_SurfUnlock, 14, (void*)&H_SurfUnlock);
}

HRESULT STDMETHODCALLTYPE H_TexSurfaceLevel(IDirect3DTexture9* t, UINT l, IDirect3DSurface9** out) {
    HRESULT hr = VtOrig<decltype(&H_TexSurfaceLevel)>(V_TexSurfaceLevel, t)(t, l, out);
    if (SUCCEEDED(hr) && out && *out) HookSurfaceVt(*out);
    return hr;
}

void HookTextureVt(IUnknown* t) {
    void** vt = *(void***)t;
    HookVt(vt, V_TexSurfaceLevel, 18, (void*)&H_TexSurfaceLevel);
    HookVt(vt, V_TexLock, 19, (void*)&H_TexLock);
    HookVt(vt, V_TexUnlock, 20, (void*)&H_TexUnlock);
}

// ------------------------------------------------------------------ creation hooks
#define RET _ReturnAddress()
HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f,
                                          D3DPOOL p, IDirect3DTexture9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateTexture)>(DEV_CreateTexture)(d, w, h, lv, use, f, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_Texture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT lv, DWORD use,
                                                D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateVolumeTexture)>(DEV_CreateVolumeTexture)(d, w, h, dp, lv, use, f, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_VolumeTexture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateCubeTexture(IDirect3DDevice9* d, UINT e, UINT lv, DWORD use, D3DFORMAT f,
                                              D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateCubeTexture)>(DEV_CreateCubeTexture)(d, e, lv, use, f, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_CubeTexture, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD use, DWORD fvf, D3DPOOL p,
                                               IDirect3DVertexBuffer9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateVertexBuffer)>(DEV_CreateVertexBuffer)(d, len, use, fvf, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_VertexBuffer, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD use, D3DFORMAT f, D3DPOOL p,
                                              IDirect3DIndexBuffer9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateIndexBuffer)>(DEV_CreateIndexBuffer)(d, len, use, f, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_IndexBuffer, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f,
                                               D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lock, IDirect3DSurface9** o,
                                               HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateRenderTarget)>(DEV_CreateRenderTarget)(d, w, h, f, ms, q, lock, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_RenderTarget, true, lock ? 1 : 0);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateDepthStencilSurface(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f,
                                                      D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL disc,
                                                      IDirect3DSurface9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateDepthStencilSurface)>(DEV_CreateDepthStencilSurface)(d, w, h, f, ms, q, disc, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_DepthStencil, true, disc ? 1 : 0);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateOffscreenPlainSurface(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p,
                                                        IDirect3DSurface9** o, HANDLE* sh) {
    HRESULT hr = Orig<decltype(&H_CreateOffscreenPlainSurface)>(DEV_CreateOffscreenPlainSurface)(d, w, h, f, p, o, sh);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_OffscreenPlain, p == D3DPOOL_DEFAULT);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateAdditionalSwapChain(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp,
                                                      IDirect3DSwapChain9** o) {
    HRESULT hr = Orig<decltype(&H_CreateAdditionalSwapChain)>(DEV_CreateAdditionalSwapChain)(d, pp, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_SwapChain, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateStateBlock(IDirect3DDevice9* d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** o) {
    HRESULT hr = Orig<decltype(&H_CreateStateBlock)>(DEV_CreateStateBlock)(d, t, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_StateBlock, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_EndStateBlock(IDirect3DDevice9* d, IDirect3DStateBlock9** o) {
    HRESULT hr = Orig<decltype(&H_EndStateBlock)>(DEV_EndStateBlock)(d, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_StateBlock, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexDeclaration(IDirect3DDevice9* d, const D3DVERTEXELEMENT9* e,
                                                    IDirect3DVertexDeclaration9** o) {
    HRESULT hr = Orig<decltype(&H_CreateVertexDeclaration)>(DEV_CreateVertexDeclaration)(d, e, o);
    if (SUCCEEDED(hr) && o) PinForever(RET, *o, K_VertexDecl);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** o) {
    HRESULT hr = Orig<decltype(&H_CreateVertexShader)>(DEV_CreateVertexShader)(d, fn, o);
    if (SUCCEEDED(hr) && o) PinForever(RET, *o, K_VertexShader);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** o) {
    HRESULT hr = Orig<decltype(&H_CreatePixelShader)>(DEV_CreatePixelShader)(d, fn, o);
    if (SUCCEEDED(hr) && o) PinForever(RET, *o, K_PixelShader);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateQuery(IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** o) {
    HRESULT hr = Orig<decltype(&H_CreateQuery)>(DEV_CreateQuery)(d, t, o);
    if (SUCCEEDED(hr) && o) TrackIfGame(RET, *o, K_Query, false);
    return hr;
}
#undef RET

// ------------------------------------------------------------------ overlay
std::string g_text, g_texText;
DWORD g_textUntil = 0;
IDirect3DTexture9* g_textTex = nullptr;
IDirect3DStateBlock9* g_overlayState = nullptr;
int g_texW = 0, g_texH = 0, g_textW = 0, g_textH = 0;

void ReleaseOverlay() {
    if (g_overlayState) { g_overlayState->Release(); g_overlayState = nullptr; }
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
                                                                     D3DPOOL_MANAGED, &g_textTex, nullptr);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(g_textTex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < g_texH; y++) {
            const uint32_t* src = (const uint32_t*)bits + y * g_texW;
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < g_texW; x++) {
                uint32_t v = src[x] & 0xFF;   // white text: any channel is the coverage
                bool inBox = x < g_textW && y < g_textH;
                uint32_t a = inBox ? 150 + (v * 105) / 255 : 0;
                uint32_t c = v;
                dst[x] = (a << 24) | (c << 16) | (c << 8) | c;
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

void DrawOverlay(IDirect3DDevice9* d) {
    if (g_text.empty() || (int)(GetTickCount() - g_textUntil) > 0) return;
    if (!g_textTex || g_texText != g_text) {
        if (!BuildTextTexture()) return;
    }
    if (!g_overlayState && FAILED(Orig<decltype(&H_CreateStateBlock)>(DEV_CreateStateBlock)(d, D3DSBT_ALL, &g_overlayState)))
        return;
    if (FAILED(d->BeginScene())) return;
    g_overlayState->Capture();
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, g_textTex);
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
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    struct V { float x, y, z, w, u, v; };
    const float x0 = 16.0f - 0.5f, y0 = 16.0f - 0.5f;
    const float w = (float)g_texW, h = (float)g_texH;
    V v[4] = {{x0, y0, 0, 1, 0, 0}, {x0 + w, y0, 0, 1, 1, 0}, {x0, y0 + h, 0, 1, 0, 1}, {x0 + w, y0 + h, 0, 1, 1, 1}};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    g_overlayState->Apply();
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

void CaptureBackBuffer(IDirect3DDevice9* d) {
    std::string path;
    path.swap(g_shotPath);
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    if (SUCCEEDED(Orig<decltype(&H_CreateOffscreenPlainSurface)>(DEV_CreateOffscreenPlainSurface)(
            d, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::string rgb((size_t)desc.Width * desc.Height * 3, 0);
            for (UINT y = 0; y < desc.Height; y++) {
                const uint8_t* s = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < desc.Width; x++) {
                    size_t o = ((size_t)y * desc.Width + x) * 3;
                    rgb[o] = (char)s[x * 4 + 2]; rgb[o + 1] = (char)s[x * 4 + 1]; rgb[o + 2] = (char)s[x * 4];
                }
            }
            sys->UnlockRect();
            WritePng(path.c_str(), (const uint8_t*)rgb.data(), desc.Width, desc.Height);
        }
    }
    if (sys) sys->Release();
    bb->Release();
}

// ------------------------------------------------------------------ device-level hooks
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* s, const RECT* dst, HWND w, const RGNDATA* r) {
    DrawOverlay(d);
    if (!g_shotPath.empty()) CaptureBackBuffer(d);
    return Orig<decltype(&H_Present)>(DEV_Present)(d, s, dst, w, r);
}

HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    Log("device reset: savestates referencing GPU objects are dropped");
    if (onDeviceReset) onDeviceReset();
    ReleaseOverlay();
    Collect();
    if (forceWindowed) MakeWindowed(pp, g_hwnd);
    HRESULT hr = Orig<decltype(&H_Reset)>(DEV_Reset)(d, pp);
    g_generation++;
    Log("device reset -> 0x%08X (%u objects tracked)", (unsigned)hr, (unsigned)g_objs.size());
    return hr;
}

void HookDevice(IDirect3DDevice9* dev) {
    void** vt = *(void***)dev;
    g_devVt = vt;
    HookSlot(vt, DEV_CreateAdditionalSwapChain, (void*)&H_CreateAdditionalSwapChain);
    HookSlot(vt, DEV_Reset, (void*)&H_Reset);
    HookSlot(vt, DEV_Present, (void*)&H_Present);
    HookSlot(vt, DEV_CreateTexture, (void*)&H_CreateTexture);
    HookSlot(vt, DEV_CreateVolumeTexture, (void*)&H_CreateVolumeTexture);
    HookSlot(vt, DEV_CreateCubeTexture, (void*)&H_CreateCubeTexture);
    HookSlot(vt, DEV_CreateVertexBuffer, (void*)&H_CreateVertexBuffer);
    HookSlot(vt, DEV_CreateIndexBuffer, (void*)&H_CreateIndexBuffer);
    HookSlot(vt, DEV_CreateRenderTarget, (void*)&H_CreateRenderTarget);
    HookSlot(vt, DEV_CreateDepthStencilSurface, (void*)&H_CreateDepthStencilSurface);
    HookSlot(vt, DEV_CreateOffscreenPlainSurface, (void*)&H_CreateOffscreenPlainSurface);
    HookSlot(vt, DEV_CreateStateBlock, (void*)&H_CreateStateBlock);
    HookSlot(vt, DEV_EndStateBlock, (void*)&H_EndStateBlock);
    HookSlot(vt, DEV_CreateVertexDeclaration, (void*)&H_CreateVertexDeclaration);
    HookSlot(vt, DEV_CreateVertexShader, (void*)&H_CreateVertexShader);
    HookSlot(vt, DEV_CreatePixelShader, (void*)&H_CreatePixelShader);
    HookSlot(vt, DEV_CreateQuery, (void*)&H_CreateQuery);
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
CreateDevice_t g_origCreateDevice = nullptr;
IDirect3D9* (WINAPI* g_origDirect3DCreate9)(UINT) = nullptr;
IDirect3D9* g_d3d = nullptr;

HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND wnd, DWORD fl,
                                         D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (forceWindowed) MakeWindowed(pp, wnd);
    HRESULT hr = g_origCreateDevice(d3d, ad, t, wnd, fl, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        HookDevice(g_dev);
        Log("CreateDevice ok: device %p vtable %p (%ux%u windowed=%d)", g_dev, *(void**)g_dev,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    } else {
        Log("CreateDevice failed 0x%08X", (unsigned)hr);
    }
    return hr;
}

IDirect3D9* WINAPI H_Direct3DCreate9(UINT sdk) {
    if (!g_origDirect3DCreate9) {   // delay-loaded import: load the same d3d9.dll the game would
        HMODULE m = LoadLibraryA("d3d9.dll");
        g_origDirect3DCreate9 = m ? (decltype(g_origDirect3DCreate9))GetProcAddress(m, "Direct3DCreate9") : nullptr;
        if (!g_origDirect3DCreate9) return nullptr;
    }
    IDirect3D9* d3d = g_origDirect3DCreate9(sdk);
    if (d3d) {
        g_d3d = d3d;
        void** vt = *(void***)d3d;
        if (vt[D3D_CreateDevice] != (void*)&H_CreateDevice) {
            g_origCreateDevice = (CreateDevice_t)vt[D3D_CreateDevice];
            Unprotect((uintptr_t)&vt[D3D_CreateDevice], 4);
            vt[D3D_CreateDevice] = (void*)&H_CreateDevice;
        }
    }
    Log("Direct3DCreate9(%u) = %p", sdk, d3d);
    return d3d;
}

// ------------------------------------------------------------------ device state as values
const uint8_t kRenderStates[] = {
    7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 166, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178, 179, 180,
    181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 198, 199, 200, 201, 202, 203, 204, 205,
    206, 207, 208, 209};
const uint8_t kStageStates[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 24, 26, 27, 28, 32};
const DWORD kSamplers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 257, 258, 259, 260};
const D3DTRANSFORMSTATETYPE kTransforms[] = {D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_TEXTURE1,
    D3DTS_TEXTURE2, D3DTS_TEXTURE3, D3DTS_TEXTURE4, D3DTS_TEXTURE5, D3DTS_TEXTURE6, D3DTS_TEXTURE7, D3DTS_WORLD};
constexpr int NRS = sizeof(kRenderStates), NTSS = sizeof(kStageStates), NSMP = 20, NXF = 11;

}  // namespace

// Everything Get* can read back, with object pointers stored as values (they are mapped when applied).
struct DevState {
    DWORD rs[NRS]; uint8_t rsOk[NRS];
    DWORD tss[8][NTSS];
    DWORD ss[NSMP][14];
    uint32_t tex[NSMP];
    struct { uint32_t vb; UINT off, stride, freq; } streams[16];
    uint32_t ib, decl, vs, ps, rt[4], ds;
    DWORD fvf;
    D3DMATRIX xf[NXF];
    D3DVIEWPORT9 vp;
    D3DMATERIAL9 mat;
    D3DLIGHT9 light[16]; uint8_t lightOk[16]; BOOL lightOn[16];
    float clip[6][4];
    UINT nvsF, npsF;
    float vsF[256][4]; int vsI[16][4]; BOOL vsB[16];
    float psF[224][4]; int psI[16][4]; BOOL psB[16];
    RECT scissor;
    float npatch;
    UINT palette;
};

namespace {

template <typename T> uint32_t Val(T* o) {
    if (o) o->Release();   // Get* added a reference; the value is all that is kept
    return (uint32_t)(uintptr_t)o;
}

DevState* RecordState(IDirect3DDevice9* d) {
    auto* s = new DevState();
    for (int i = 0; i < NRS; i++) s->rsOk[i] = SUCCEEDED(d->GetRenderState((D3DRENDERSTATETYPE)kRenderStates[i], &s->rs[i]));
    for (int t = 0; t < 8; t++)
        for (int i = 0; i < NTSS; i++) d->GetTextureStageState(t, (D3DTEXTURESTAGESTATETYPE)kStageStates[i], &s->tss[t][i]);
    for (int k = 0; k < NSMP; k++) {
        for (int i = 0; i < 13; i++) d->GetSamplerState(kSamplers[k], (D3DSAMPLERSTATETYPE)(i + 1), &s->ss[k][i]);
        IDirect3DBaseTexture9* t = nullptr;
        d->GetTexture(kSamplers[k], &t);
        s->tex[k] = Val(t);
    }
    for (UINT i = 0; i < 16; i++) {
        IDirect3DVertexBuffer9* vb = nullptr;
        d->GetStreamSource(i, &vb, &s->streams[i].off, &s->streams[i].stride);
        s->streams[i].vb = Val(vb);
        d->GetStreamSourceFreq(i, &s->streams[i].freq);
    }
    IDirect3DIndexBuffer9* ib = nullptr; d->GetIndices(&ib); s->ib = Val(ib);
    IDirect3DVertexDeclaration9* decl = nullptr; d->GetVertexDeclaration(&decl); s->decl = Val(decl);
    IDirect3DVertexShader9* vs = nullptr; d->GetVertexShader(&vs); s->vs = Val(vs);
    IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); s->ps = Val(ps);
    d->GetFVF(&s->fvf);
    for (DWORD i = 0; i < 4; i++) {
        IDirect3DSurface9* rt = nullptr;
        d->GetRenderTarget(i, &rt);
        s->rt[i] = Val(rt);
    }
    IDirect3DSurface9* ds = nullptr; d->GetDepthStencilSurface(&ds); s->ds = Val(ds);
    for (int i = 0; i < NXF; i++) d->GetTransform(kTransforms[i], &s->xf[i]);
    d->GetViewport(&s->vp);
    d->GetMaterial(&s->mat);
    for (DWORD i = 0; i < 16; i++) {
        s->lightOk[i] = SUCCEEDED(d->GetLight(i, &s->light[i]));
        if (s->lightOk[i]) d->GetLightEnable(i, &s->lightOn[i]);
    }
    for (DWORD i = 0; i < 6; i++) d->GetClipPlane(i, s->clip[i]);
    D3DCAPS9 caps = {};
    d->GetDeviceCaps(&caps);
    s->nvsF = caps.MaxVertexShaderConst < 256 ? caps.MaxVertexShaderConst : 256;
    s->npsF = caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? 224 : 32;
    d->GetVertexShaderConstantF(0, &s->vsF[0][0], s->nvsF);
    d->GetVertexShaderConstantI(0, &s->vsI[0][0], 16);
    d->GetVertexShaderConstantB(0, s->vsB, 16);
    d->GetPixelShaderConstantF(0, &s->psF[0][0], s->npsF);
    d->GetPixelShaderConstantI(0, &s->psI[0][0], 16);
    d->GetPixelShaderConstantB(0, s->psB, 16);
    d->GetScissorRect(&s->scissor);
    s->npatch = d->GetNPatchMode();
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

void ApplyStateValues(IDirect3DDevice9* d, const DevState* s, const Remap& m) {
    g_unmapped = 0;
    for (int i = 0; i < NRS; i++)
        if (s->rsOk[i]) d->SetRenderState((D3DRENDERSTATETYPE)kRenderStates[i], s->rs[i]);
    for (int t = 0; t < 8; t++)
        for (int i = 0; i < NTSS; i++) d->SetTextureStageState(t, (D3DTEXTURESTAGESTATETYPE)kStageStates[i], s->tss[t][i]);
    for (int k = 0; k < NSMP; k++) {
        for (int i = 0; i < 13; i++) d->SetSamplerState(kSamplers[k], (D3DSAMPLERSTATETYPE)(i + 1), s->ss[k][i]);
        d->SetTexture(kSamplers[k], Map<IDirect3DBaseTexture9>(m, s->tex[k]));
    }
    for (UINT i = 0; i < 16; i++) {
        d->SetStreamSource(i, Map<IDirect3DVertexBuffer9>(m, s->streams[i].vb), s->streams[i].off, s->streams[i].stride);
        d->SetStreamSourceFreq(i, s->streams[i].freq ? s->streams[i].freq : 1);
    }
    d->SetIndices(Map<IDirect3DIndexBuffer9>(m, s->ib));
    d->SetVertexShader(Map<IDirect3DVertexShader9>(m, s->vs));
    d->SetPixelShader(Map<IDirect3DPixelShader9>(m, s->ps));
    if (s->decl) d->SetVertexDeclaration(Map<IDirect3DVertexDeclaration9>(m, s->decl));
    else d->SetFVF(s->fvf);
    for (DWORD i = 0; i < 4; i++) {
        auto* rt = Map<IDirect3DSurface9>(m, s->rt[i]);
        if (rt || i) d->SetRenderTarget(i, rt);
    }
    if (auto* ds = Map<IDirect3DSurface9>(m, s->ds)) d->SetDepthStencilSurface(ds);
    for (int i = 0; i < NXF; i++) d->SetTransform(kTransforms[i], &s->xf[i]);
    d->SetViewport(&s->vp);
    d->SetMaterial(&s->mat);
    for (DWORD i = 0; i < 16; i++)
        if (s->lightOk[i]) { d->SetLight(i, &s->light[i]); d->LightEnable(i, s->lightOn[i]); }
    for (DWORD i = 0; i < 6; i++) d->SetClipPlane(i, s->clip[i]);
    d->SetVertexShaderConstantF(0, &s->vsF[0][0], s->nvsF);
    d->SetVertexShaderConstantI(0, &s->vsI[0][0], 16);
    d->SetVertexShaderConstantB(0, s->vsB, 16);
    d->SetPixelShaderConstantF(0, &s->psF[0][0], s->npsF);
    d->SetPixelShaderConstantI(0, &s->psI[0][0], 16);
    d->SetPixelShaderConstantB(0, s->psB, 16);
    d->SetScissorRect(&s->scissor);
    d->SetNPatchMode(s->npatch);
    d->SetCurrentTexturePalette(s->palette);
}

// ------------------------------------------------------------------ object contents
UINT BytesPerPixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_V16U16: case D3DFMT_Q8W8V8U8: case D3DFMT_X8L8V8U8:
    case D3DFMT_R32F: case D3DFMT_G16R16F: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_A8P8: return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
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
    uint8_t  kind, permanent, defaultPool, pad;
    uint32_t gameRefs;
    uint32_t p[8];       // creation parameters (see DescribeObj)
    uint32_t content;    // bytes of content that follow
};

std::map<D3DFORMAT, int> g_noContent;   // textures written without contents, by format (unknown layout)

// Creation parameters and content size. Content is kept for everything the device cannot rebuild by itself:
// managed / system-memory textures, all vertex and index buffers (dynamic ones too: the game fills some only when
// their data changes), declarations and shader code. Render targets are redrawn every frame.
bool DescribeObj(IUnknown* o, const Obj& info, Entry& e) {
    memset(e.p, 0, sizeof(e.p));
    e.content = 0;
    switch (info.kind) {
    case K_Texture: {
        auto* t = (IDirect3DTexture9*)o;
        D3DSURFACE_DESC sd;
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
        auto* t = (IDirect3DCubeTexture9*)o;
        D3DSURFACE_DESC sd;
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
        auto* t = (IDirect3DVolumeTexture9*)o;
        D3DVOLUME_DESC vd;
        if (FAILED(t->GetLevelDesc(0, &vd))) return false;
        uint32_t p[] = {vd.Width, vd.Height, vd.Depth, t->GetLevelCount(), vd.Usage, (uint32_t)vd.Format, (uint32_t)vd.Pool};
        memcpy(e.p, p, sizeof(p));
        return true;   // contents are not kept (the game creates none)
    }
    case K_VertexBuffer: {
        D3DVERTEXBUFFER_DESC d;
        if (FAILED(((IDirect3DVertexBuffer9*)o)->GetDesc(&d))) return false;
        uint32_t p[] = {d.Size, d.Usage, d.FVF, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        e.content = d.Size;
        return true;
    }
    case K_IndexBuffer: {
        D3DINDEXBUFFER_DESC d;
        if (FAILED(((IDirect3DIndexBuffer9*)o)->GetDesc(&d))) return false;
        uint32_t p[] = {d.Size, d.Usage, (uint32_t)d.Format, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        e.content = d.Size;
        return true;
    }
    case K_RenderTarget: case K_DepthStencil: case K_OffscreenPlain: {
        D3DSURFACE_DESC d;
        if (FAILED(((IDirect3DSurface9*)o)->GetDesc(&d))) return false;
        uint32_t p[] = {d.Width, d.Height, (uint32_t)d.Format, (uint32_t)d.MultiSampleType, d.MultiSampleQuality,
                        info.flags, (uint32_t)d.Pool};
        memcpy(e.p, p, sizeof(p));
        return true;
    }
    case K_VertexDecl: {
        UINT n = 0;
        if (FAILED(((IDirect3DVertexDeclaration9*)o)->GetDeclaration(nullptr, &n))) return false;
        e.content = n * sizeof(D3DVERTEXELEMENT9);
        return true;
    }
    case K_VertexShader: case K_PixelShader: {
        UINT n = 0;
        HRESULT hr = info.kind == K_VertexShader ? ((IDirect3DVertexShader9*)o)->GetFunction(nullptr, &n)
                                                 : ((IDirect3DPixelShader9*)o)->GetFunction(nullptr, &n);
        if (FAILED(hr)) return false;
        e.content = n;
        return true;
    }
    case K_Query:
        e.p[0] = (uint32_t)((IDirect3DQuery9*)o)->GetType();
        return true;
    case K_StateBlock:
        return true;   // re-created empty (the game makes none of its own)
    default:
        return false;  // swap chains: not supported
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
int64_t g_dynLock = 0, g_dynCopy = 0;

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
        auto* t = (IDirect3DTexture9*)o;
        for (DWORD l = 0; l < e.p[2]; l++) {
            D3DSURFACE_DESC sd;
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
        auto* t = (IDirect3DCubeTexture9*)o;
        for (int f = 0; f < 6; f++)
            for (DWORD l = 0; l < e.p[1]; l++) {
                D3DSURFACE_DESC sd;
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
        void* p = nullptr;
        // between two frames a dynamic buffer holds its final data: read it without waiting for the GPU
        DWORD fl = D3DLOCK_READONLY | (e.p[1] & D3DUSAGE_DYNAMIC ? D3DLOCK_NOOVERWRITE : 0);
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        HRESULT hr = info.kind == K_VertexBuffer ? ((IDirect3DVertexBuffer9*)o)->Lock(0, 0, &p, fl)
                                                 : ((IDirect3DIndexBuffer9*)o)->Lock(0, 0, &p, fl);
        QueryPerformanceCounter(&t1);
        if (SUCCEEDED(hr) && (e.p[1] & D3DUSAGE_DYNAMIC)) {
            if (g_wcScratch.size() < e.content) g_wcScratch.resize(e.content);
            CopyFromWC(g_wcScratch.data(), p, e.content);
            out.Put(g_wcScratch.data(), e.content);
            QueryPerformanceCounter(&t2);
            g_dynLock += t1.QuadPart - t0.QuadPart;
            g_dynCopy += t2.QuadPart - t1.QuadPart;
            if (info.kind == K_VertexBuffer) ((IDirect3DVertexBuffer9*)o)->Unlock();
            else ((IDirect3DIndexBuffer9*)o)->Unlock();
        } else if (SUCCEEDED(hr)) {
            out.Put(p, e.content);
            if (info.kind == K_VertexBuffer) ((IDirect3DVertexBuffer9*)o)->Unlock();
            else ((IDirect3DIndexBuffer9*)o)->Unlock();
        } else {
            PutZero(out, e.content);
        }
        written = e.content;
        break;
    }
    case K_VertexDecl: {
        std::vector<D3DVERTEXELEMENT9> el(e.content / sizeof(D3DVERTEXELEMENT9));
        UINT n = (UINT)el.size();
        ((IDirect3DVertexDeclaration9*)o)->GetDeclaration(el.data(), &n);
        out.Put(el.data(), e.content);
        written = e.content;
        break;
    }
    case K_VertexShader: case K_PixelShader: {
        std::vector<uint8_t> code(e.content);
        UINT n = e.content;
        if (info.kind == K_VertexShader) ((IDirect3DVertexShader9*)o)->GetFunction(code.data(), &n);
        else ((IDirect3DPixelShader9*)o)->GetFunction(code.data(), &n);
        out.Put(code.data(), e.content);
        written = e.content;
        break;
    }
    default: break;
    }
    if (written < e.content) PutZero(out, e.content - written);
}

constexpr uint32_t GPU_MAGIC = 0x31555047;   // "GPU1"

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
            auto* tex = (IDirect3DTexture9*)rec.container;
            for (DWORD l = 0; l < tex->GetLevelCount(); l++) {
                IDirect3DSurface9* x = nullptr;
                if (SUCCEEDED(tex->GetSurfaceLevel(l, &x)) && x) {
                    if (x == sf) h.level = l;
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
std::vector<uint8_t> g_skip;
DevState* g_pendingState = nullptr;

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
    IDirect3DDevice9* d = g_dev;
    IUnknown* obj = nullptr;
    size_t consumed = 0;
    *readOk = true;
    switch (e.kind) {
    case K_Texture: {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(Orig<decltype(&H_CreateTexture)>(DEV_CreateTexture)(d, e.p[0], e.p[1], e.p[2], e.p[3],
                (D3DFORMAT)e.p[4], (D3DPOOL)e.p[5], &t, nullptr))) break;
        obj = t;
        if (!e.content) break;
        for (DWORD l = 0; l < e.p[2] && *readOk; l++) {
            D3DSURFACE_DESC sd;
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
        IDirect3DCubeTexture9* t = nullptr;
        if (FAILED(Orig<decltype(&H_CreateCubeTexture)>(DEV_CreateCubeTexture)(d, e.p[0], e.p[1], e.p[2],
                (D3DFORMAT)e.p[3], (D3DPOOL)e.p[4], &t, nullptr))) break;
        obj = t;
        if (!e.content) break;
        for (int f = 0; f < 6; f++)
            for (DWORD l = 0; l < e.p[1] && *readOk; l++) {
                D3DSURFACE_DESC sd;
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
        IDirect3DVolumeTexture9* t = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateVolumeTexture)>(DEV_CreateVolumeTexture)(d, e.p[0], e.p[1], e.p[2], e.p[3],
                e.p[4], (D3DFORMAT)e.p[5], (D3DPOOL)e.p[6], &t, nullptr))) obj = t;
        break;
    }
    case K_VertexBuffer: {
        IDirect3DVertexBuffer9* b = nullptr;
        if (FAILED(Orig<decltype(&H_CreateVertexBuffer)>(DEV_CreateVertexBuffer)(d, e.p[0], e.p[1], e.p[2],
                (D3DPOOL)e.p[3], &b, nullptr))) break;
        obj = b;
        void* p = nullptr;
        if (e.content && SUCCEEDED(b->Lock(0, 0, &p, 0))) {
            *readOk = in.Get(p, e.content);
            b->Unlock();
            consumed = e.content;
        }
        break;
    }
    case K_IndexBuffer: {
        IDirect3DIndexBuffer9* b = nullptr;
        if (FAILED(Orig<decltype(&H_CreateIndexBuffer)>(DEV_CreateIndexBuffer)(d, e.p[0], e.p[1], (D3DFORMAT)e.p[2],
                (D3DPOOL)e.p[3], &b, nullptr))) break;
        obj = b;
        void* p = nullptr;
        if (e.content && SUCCEEDED(b->Lock(0, 0, &p, 0))) {
            *readOk = in.Get(p, e.content);
            b->Unlock();
            consumed = e.content;
        }
        break;
    }
    case K_RenderTarget: {
        IDirect3DSurface9* sf = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateRenderTarget)>(DEV_CreateRenderTarget)(d, e.p[0], e.p[1], (D3DFORMAT)e.p[2],
                (D3DMULTISAMPLE_TYPE)e.p[3], e.p[4], e.p[5], &sf, nullptr))) obj = sf;
        break;
    }
    case K_DepthStencil: {
        IDirect3DSurface9* sf = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateDepthStencilSurface)>(DEV_CreateDepthStencilSurface)(d, e.p[0], e.p[1],
                (D3DFORMAT)e.p[2], (D3DMULTISAMPLE_TYPE)e.p[3], e.p[4], e.p[5], &sf, nullptr))) obj = sf;
        break;
    }
    case K_OffscreenPlain: {
        IDirect3DSurface9* sf = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateOffscreenPlainSurface)>(DEV_CreateOffscreenPlainSurface)(d, e.p[0], e.p[1],
                (D3DFORMAT)e.p[2], (D3DPOOL)e.p[6], &sf, nullptr))) obj = sf;
        break;
    }
    case K_VertexDecl: {
        std::vector<D3DVERTEXELEMENT9> el(e.content / sizeof(D3DVERTEXELEMENT9) + 1);
        *readOk = in.Get(el.data(), e.content);
        consumed = e.content;
        IDirect3DVertexDeclaration9* v = nullptr;
        if (*readOk && SUCCEEDED(Orig<decltype(&H_CreateVertexDeclaration)>(DEV_CreateVertexDeclaration)(d, el.data(), &v)))
            obj = v;
        break;
    }
    case K_VertexShader: case K_PixelShader: {
        std::vector<DWORD> code(e.content / 4 + 1);
        *readOk = in.Get(code.data(), e.content);
        consumed = e.content;
        if (!*readOk) break;
        if (e.kind == K_VertexShader) {
            IDirect3DVertexShader9* v = nullptr;
            if (SUCCEEDED(Orig<decltype(&H_CreateVertexShader)>(DEV_CreateVertexShader)(d, code.data(), &v))) obj = v;
        } else {
            IDirect3DPixelShader9* v = nullptr;
            if (SUCCEEDED(Orig<decltype(&H_CreatePixelShader)>(DEV_CreatePixelShader)(d, code.data(), &v))) obj = v;
        }
        break;
    }
    case K_Query: {
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateQuery)>(DEV_CreateQuery)(d, (D3DQUERYTYPE)e.p[0], &q))) obj = q;
        break;
    }
    case K_StateBlock: {
        IDirect3DStateBlock9* sb = nullptr;
        if (SUCCEEDED(Orig<decltype(&H_CreateStateBlock)>(DEV_CreateStateBlock)(d, D3DSBT_ALL, &sb))) obj = sb;
        break;
    }
    default: break;
    }
    if (*readOk && consumed < e.content) *readOk = Skip(in, e.content - consumed);
    return obj;
}

}  // namespace

bool InstallHooks() {
    InitializeCriticalSection(&g_cs);
    g_objs.reserve(16384);
    HMODULE exe = GetModuleHandleA(nullptr);
    g_origDirect3DCreate9 = (decltype(g_origDirect3DCreate9))PatchIat(exe, "d3d9.dll", "Direct3DCreate9",
                                                                       (void*)&H_Direct3DCreate9);
    bool ok = g_origDirect3DCreate9 != nullptr;
    if (!ok) ok = PatchDelayIat(exe, "d3d9.dll", "Direct3DCreate9", (void*)&H_Direct3DCreate9) != nullptr;
    Log("d3d hook: Direct3DCreate9 %s", ok ? (g_origDirect3DCreate9 ? "import patched" : "delay import patched") : "NOT FOUND");
    return ok;
}

IDirect3DDevice9* Device() { return g_dev; }

bool DeviceUsable() { return g_dev && g_dev->TestCooperativeLevel() == D3D_OK; }

uint32_t Generation() { return g_generation; }

void Track(IUnknown* obj, Kind kind, bool defaultPool) { TrackObj(obj, kind, defaultPool, false); }

const char* KindName(int k) {
    static const char* names[K_Count] = {"texture", "volume texture", "cube texture", "vertex buffer", "index buffer",
                                         "render target", "depth stencil", "offscreen surface", "swap chain",
                                         "state block", "vertex decl", "vertex shader", "pixel shader", "query"};
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
    void HeapCheck(const char*);
    HeapCheck("before collect");
    EnterCriticalSection(&g_cs);
    for (auto it = g_objs.begin(); it != g_objs.end();) {
        if (!it->second.permanent && it->second.snapRefs == 0 && RefCount(it->first) <= 1) {
            if (!g_surfLocks.empty() || !g_texLocks.empty()) ForgetLocks(it->first);
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

// A savestate pins what its memory can use: objects the game holds references to, objects bound to the device
// (re-bound by the state block on load), and the "permanent" ones (vertex declarations in RenderWare's cache, which
// holds no references).
Capture* Save() {
    Collect();
    auto* c = new Capture;
    c->generation = g_generation;
    c->dev = RecordState(g_dev);
    std::unordered_set<IUnknown*> bound;
    for (uint32_t v : c->dev->tex) bound.insert((IUnknown*)(uintptr_t)v);
    for (auto& s : c->dev->streams) bound.insert((IUnknown*)(uintptr_t)s.vb);
    for (uint32_t v : {c->dev->ib, c->dev->decl, c->dev->vs, c->dev->ps, c->dev->ds}) bound.insert((IUnknown*)(uintptr_t)v);
    for (uint32_t v : c->dev->rt) bound.insert((IUnknown*)(uintptr_t)v);
    EnterCriticalSection(&g_cs);
    c->refs.reserve(g_objs.size());
    for (auto& [obj, o] : g_objs) {
        uint32_t refs = RefCount(obj) - 1;
        if (!refs && !o.permanent && !bound.count(obj)) continue;
        c->refs.emplace_back(obj, refs);
        if (o.permanent) c->permanent.push_back(obj);
        o.snapRefs++;
    }
    LeaveCriticalSection(&g_cs);
    if (FAILED(Orig<decltype(&H_CreateStateBlock)>(DEV_CreateStateBlock)(g_dev, D3DSBT_ALL, &c->state))) c->state = nullptr;
    c->locks = RecordLocks();
    return c;
}

void Load(const Capture* c) {
    std::unordered_map<IUnknown*, uint32_t> want(c->refs.begin(), c->refs.end());
    std::unordered_set<IUnknown*> perm(c->permanent.begin(), c->permanent.end());
    EnterCriticalSection(&g_cs);
    uint32_t adjusted = 0;
    for (auto& [obj, o] : g_objs) {
        // The restored memory can only reach declarations it could reach at save time.
        if (o.kind == K_VertexDecl || o.kind == K_VertexShader || o.kind == K_PixelShader) o.permanent = perm.count(obj) != 0;
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
    if (c->state) c->state->Apply();
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
    if (c->state) c->state->Release();
    delete c->dev;
    delete c;
}

IDirect3D9* Direct3D() { return g_d3d; }

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
        e.permanent = it->second.permanent;
        e.defaultPool = it->second.defaultPool;
        e.gameRefs = refs;
        if (!DescribeObj(obj, it->second, e)) { skipped++; continue; }
        entries.emplace_back(obj, e);
    }
    uint32_t n = (uint32_t)entries.size();
    g_writeLocks = c->locks.data();
    g_writeLockCount = c->locks.size();
    out.Put(&GPU_MAGIC, 4);
    out.Put(&n, 4);
    uint64_t bytes = 0;
    for (auto& [obj, e] : entries) {
        out.Put(&e, sizeof(e));
        WriteContent(out, obj, g_objs[obj], e);
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
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    Log("d3d write: dynamic buffers: lock %.1f ms, read %.1f ms", g_dynLock * 1000.0 / qf.QuadPart,
        g_dynCopy * 1000.0 / qf.QuadPart);
    g_dynLock = g_dynCopy = 0;
    int byKind[K_Count] = {}, perm = 0, refSum = 0;
    for (auto& [obj, e] : entries) { byKind[e.kind]++; perm += e.permanent; refSum += e.gameRefs; }
    char kinds[256] = "";
    for (int k = 0; k < K_Count; k++)
        if (byKind[k]) { char t[40]; sprintf_s(t, " %s %d,", KindName(k), byKind[k]); strcat_s(kinds, t); }
    Log("d3d write: %u objects (%d permanent, %d game references, %u tracked):%s %.1f MB of contents", n, perm, refSum,
        (unsigned)g_objs.size(), kinds, bytes / 1048576.0);
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
        o.permanent = false;             // the new memory refers to the re-created declarations only
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
        uint32_t flags = e.kind == K_RenderTarget || e.kind == K_DepthStencil ? e.p[5] : 0;
        TrackObj(obj, (Kind)e.kind, e.defaultPool != 0, e.permanent != 0, flags);
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
            if (FAILED(((IDirect3DSurface9*)obj)->LockRect(&lr, r, h.flags))) continue;
        } else if (h.surface) {
            // the game holds the level's surface (one reference on the texture, already counted in its references)
            IDirect3DSurface9* sf = nullptr;
            if (FAILED(((IDirect3DTexture9*)obj)->GetSurfaceLevel(h.level, &sf)) || !sf) continue;
            ((IDirect3DTexture9*)obj)->Release();
            if (FAILED(sf->LockRect(&lr, r, h.flags))) continue;
            remap[h.surface] = (uint32_t)(uintptr_t)sf;
        } else if (FAILED(((IDirect3DTexture9*)obj)->LockRect(h.level, &lr, r, h.flags))) {
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
