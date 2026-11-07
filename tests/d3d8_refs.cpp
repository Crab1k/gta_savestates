// Direct3D 8: does binding a texture / vertex buffer / index buffer, or a state block, change its public reference
// count? (d3d8.dll is loaded at run time: the SDK has no import library for it.)
#include <windows.h>
#include "../src3/d3d8min.h"
#include <cstdio>

static ULONG Refs(IUnknown* o) { o->AddRef(); return o->Release(); }
#define R(x) Refs((IUnknown*)(x))

int main() {
    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    auto create = (IDirect3D8*(WINAPI*)(UINT))GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8");
    IDirect3D8* d3d = create(D3D8_SDK_VERSION);
    D3DPRESENT_PARAMETERS8 pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = w;
    IDirect3DDevice8* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice failed %08X\n", (unsigned)hr); return 1; }
    IDirect3DTexture8* tex = nullptr;
    IDirect3DVertexBuffer8* vb = nullptr;
    IDirect3DIndexBuffer8* ib = nullptr;
    dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    dev->CreateVertexBuffer(256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb);
    dev->CreateIndexBuffer(256, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
    printf("created:            tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    dev->SetTexture(0, tex);
    dev->SetStreamSource(0, vb, 16);
    dev->SetIndices(ib, 0);
    printf("bound:              tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    DWORD sb = 0;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    printf("state block (ALL):  tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    dev->SetTexture(0, nullptr);
    dev->SetStreamSource(0, nullptr, 0);
    dev->SetIndices(nullptr, 0);
    printf("unbound, sb alive:  tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    dev->ApplyStateBlock(sb);
    printf("sb applied:         tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    dev->DeleteStateBlock(sb);
    printf("sb deleted:         tex %lu vb %lu ib %lu\n", R(tex), R(vb), R(ib));
    IDirect3DBaseTexture8* got = nullptr;
    dev->GetTexture(0, &got);
    printf("GetTexture:         tex %lu (returned %p)\n", R(tex), got);
    if (got) got->Release();
    DWORD fvf = 0;
    dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_TEX1);
    dev->GetVertexShader(&fvf);
    printf("vertex shader (FVF) handle: %08lX\n", fvf);
    return 0;
}
