// Does binding a texture, or capturing it in a state block, change its public reference count?
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static ULONG Refs(IUnknown* o) { o->AddRef(); return o->Release(); }

int main() {
    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = w;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice failed %08X\n", (unsigned)hr); return 1; }
    IDirect3DTexture9* tex = nullptr;
    IDirect3DVertexBuffer9* vb = nullptr;
    dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    dev->CreateVertexBuffer(256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr);
    printf("created:            tex %lu vb %lu\n", Refs(tex), Refs(vb));
    dev->SetTexture(0, tex);
    dev->SetStreamSource(0, vb, 0, 16);
    printf("bound:              tex %lu vb %lu\n", Refs(tex), Refs(vb));
    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    printf("state block (ALL):  tex %lu vb %lu\n", Refs(tex), Refs(vb));
    dev->SetTexture(0, nullptr);
    dev->SetStreamSource(0, nullptr, 0, 0);
    printf("unbound, sb alive:  tex %lu vb %lu\n", Refs(tex), Refs(vb));
    sb->Capture();
    printf("sb recaptured:      tex %lu vb %lu\n", Refs(tex), Refs(vb));
    sb->Apply();
    printf("sb applied:         tex %lu vb %lu\n", Refs(tex), Refs(vb));
    sb->Release();
    printf("sb released:        tex %lu vb %lu\n", Refs(tex), Refs(vb));
    ULONG r = tex->Release();
    printf("tex released by app -> %lu (still bound: %s)\n", r, "applied sb rebound nothing after recapture");
    return 0;
}
