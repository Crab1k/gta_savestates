// Minimal Direct3D 8 declarations (the Windows SDK no longer ships d3d8.h): the COM interfaces in vtable order,
// and the structures that differ from Direct3D 9. Shared enums and structures come from d3d9types.h.
#pragma once
#include <d3d9types.h>
#include <d3d9caps.h>
#include <unknwn.h>

struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DSurface8;
struct IDirect3DVolume8;
struct IDirect3DSwapChain8;
struct IDirect3DBaseTexture8;
struct IDirect3DTexture8;
struct IDirect3DCubeTexture8;
struct IDirect3DVolumeTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;

struct D3DPRESENT_PARAMETERS8 {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

struct D3DSURFACE_DESC8 {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width, Height;
};

struct D3DVOLUME_DESC8 {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    UINT Width, Height, Depth;
};

struct D3DVERTEXBUFFER_DESC8 { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; DWORD FVF; };
struct D3DINDEXBUFFER_DESC8 { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; };

typedef D3DVIEWPORT9 D3DVIEWPORT8;
typedef D3DMATERIAL9 D3DMATERIAL8;
typedef D3DLIGHT9 D3DLIGHT8;

#define D3D8_SDK_VERSION 220
#ifndef D3DCREATE_HARDWARE_VERTEXPROCESSING
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x20
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40
#define D3DCREATE_MIXED_VERTEXPROCESSING 0x80
#endif
#define D3D8_STDMETHOD(name) virtual HRESULT __stdcall name
#define D3D8_UNKNOWN \
    virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0; \
    virtual ULONG __stdcall AddRef() = 0; \
    virtual ULONG __stdcall Release() = 0;

struct IDirect3D8 {
    D3D8_UNKNOWN
    D3D8_STDMETHOD(RegisterSoftwareDevice)(void*) = 0;
    virtual UINT __stdcall GetAdapterCount() = 0;
    D3D8_STDMETHOD(GetAdapterIdentifier)(UINT, DWORD, void*) = 0;
    virtual UINT __stdcall GetAdapterModeCount(UINT) = 0;
    D3D8_STDMETHOD(EnumAdapterModes)(UINT, UINT, void*) = 0;
    D3D8_STDMETHOD(GetAdapterDisplayMode)(UINT, void*) = 0;
    D3D8_STDMETHOD(CheckDeviceType)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) = 0;
    D3D8_STDMETHOD(CheckDeviceFormat)(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) = 0;
    D3D8_STDMETHOD(CheckDeviceMultiSampleType)(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE) = 0;
    D3D8_STDMETHOD(CheckDepthStencilMatch)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) = 0;
    D3D8_STDMETHOD(GetDeviceCaps)(UINT, D3DDEVTYPE, void*) = 0;
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT) = 0;
    D3D8_STDMETHOD(CreateDevice)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS8*, IDirect3DDevice8**) = 0;
};

struct IDirect3DDevice8 {
    D3D8_UNKNOWN
    D3D8_STDMETHOD(TestCooperativeLevel)() = 0;                                       // 3
    virtual UINT __stdcall GetAvailableTextureMem() = 0;
    D3D8_STDMETHOD(ResourceManagerDiscardBytes)(DWORD) = 0;
    D3D8_STDMETHOD(GetDirect3D)(IDirect3D8**) = 0;
    D3D8_STDMETHOD(GetDeviceCaps)(void*) = 0;
    D3D8_STDMETHOD(GetDisplayMode)(void*) = 0;
    D3D8_STDMETHOD(GetCreationParameters)(void*) = 0;
    D3D8_STDMETHOD(SetCursorProperties)(UINT, UINT, IDirect3DSurface8*) = 0;         // 10
    virtual void __stdcall SetCursorPosition(int, int, DWORD) = 0;
    virtual BOOL __stdcall ShowCursor(BOOL) = 0;
    D3D8_STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS8*, IDirect3DSwapChain8**) = 0;
    D3D8_STDMETHOD(Reset)(D3DPRESENT_PARAMETERS8*) = 0;                              // 14
    D3D8_STDMETHOD(Present)(const RECT*, const RECT*, HWND, const RGNDATA*) = 0;
    D3D8_STDMETHOD(GetBackBuffer)(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(GetRasterStatus)(void*) = 0;
    virtual void __stdcall SetGammaRamp(DWORD, const void*) = 0;
    virtual void __stdcall GetGammaRamp(void*) = 0;
    D3D8_STDMETHOD(CreateTexture)(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture8**) = 0;   // 20
    D3D8_STDMETHOD(CreateVolumeTexture)(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture8**) = 0;
    D3D8_STDMETHOD(CreateCubeTexture)(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture8**) = 0;
    D3D8_STDMETHOD(CreateVertexBuffer)(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8**) = 0;
    D3D8_STDMETHOD(CreateIndexBuffer)(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer8**) = 0;
    D3D8_STDMETHOD(CreateRenderTarget)(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, BOOL, IDirect3DSurface8**) = 0;   // 25
    D3D8_STDMETHOD(CreateDepthStencilSurface)(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(CreateImageSurface)(UINT, UINT, D3DFORMAT, IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(CopyRects)(IDirect3DSurface8*, const RECT*, UINT, IDirect3DSurface8*, const POINT*) = 0;
    D3D8_STDMETHOD(UpdateTexture)(IDirect3DBaseTexture8*, IDirect3DBaseTexture8*) = 0;
    D3D8_STDMETHOD(GetFrontBuffer)(IDirect3DSurface8*) = 0;                          // 30
    D3D8_STDMETHOD(SetRenderTarget)(IDirect3DSurface8*, IDirect3DSurface8*) = 0;
    D3D8_STDMETHOD(GetRenderTarget)(IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(BeginScene)() = 0;
    D3D8_STDMETHOD(EndScene)() = 0;                                                  // 35
    D3D8_STDMETHOD(Clear)(DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = 0;
    D3D8_STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) = 0;
    D3D8_STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE, D3DMATRIX*) = 0;
    D3D8_STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) = 0;
    D3D8_STDMETHOD(SetViewport)(const D3DVIEWPORT8*) = 0;                            // 40
    D3D8_STDMETHOD(GetViewport)(D3DVIEWPORT8*) = 0;
    D3D8_STDMETHOD(SetMaterial)(const D3DMATERIAL8*) = 0;
    D3D8_STDMETHOD(GetMaterial)(D3DMATERIAL8*) = 0;
    D3D8_STDMETHOD(SetLight)(DWORD, const D3DLIGHT8*) = 0;
    D3D8_STDMETHOD(GetLight)(DWORD, D3DLIGHT8*) = 0;                                 // 45
    D3D8_STDMETHOD(LightEnable)(DWORD, BOOL) = 0;
    D3D8_STDMETHOD(GetLightEnable)(DWORD, BOOL*) = 0;
    D3D8_STDMETHOD(SetClipPlane)(DWORD, const float*) = 0;
    D3D8_STDMETHOD(GetClipPlane)(DWORD, float*) = 0;
    D3D8_STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE, DWORD) = 0;                   // 50
    D3D8_STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE, DWORD*) = 0;
    D3D8_STDMETHOD(BeginStateBlock)() = 0;
    D3D8_STDMETHOD(EndStateBlock)(DWORD*) = 0;
    D3D8_STDMETHOD(ApplyStateBlock)(DWORD) = 0;
    D3D8_STDMETHOD(CaptureStateBlock)(DWORD) = 0;                                    // 55
    D3D8_STDMETHOD(DeleteStateBlock)(DWORD) = 0;
    D3D8_STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, DWORD*) = 0;
    D3D8_STDMETHOD(SetClipStatus)(const void*) = 0;
    D3D8_STDMETHOD(GetClipStatus)(void*) = 0;
    D3D8_STDMETHOD(GetTexture)(DWORD, IDirect3DBaseTexture8**) = 0;                  // 60
    D3D8_STDMETHOD(SetTexture)(DWORD, IDirect3DBaseTexture8*) = 0;
    D3D8_STDMETHOD(GetTextureStageState)(DWORD, DWORD, DWORD*) = 0;
    D3D8_STDMETHOD(SetTextureStageState)(DWORD, DWORD, DWORD) = 0;
    D3D8_STDMETHOD(ValidateDevice)(DWORD*) = 0;
    D3D8_STDMETHOD(GetInfo)(DWORD, void*, DWORD) = 0;                                // 65
    D3D8_STDMETHOD(SetPaletteEntries)(UINT, const PALETTEENTRY*) = 0;
    D3D8_STDMETHOD(GetPaletteEntries)(UINT, PALETTEENTRY*) = 0;
    D3D8_STDMETHOD(SetCurrentTexturePalette)(UINT) = 0;
    D3D8_STDMETHOD(GetCurrentTexturePalette)(UINT*) = 0;
    D3D8_STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, UINT, UINT) = 0;                 // 70
    D3D8_STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) = 0;
    D3D8_STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) = 0;
    D3D8_STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = 0;
    D3D8_STDMETHOD(ProcessVertices)(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) = 0;
    D3D8_STDMETHOD(CreateVertexShader)(const DWORD*, const DWORD*, DWORD*, DWORD) = 0;   // 75
    D3D8_STDMETHOD(SetVertexShader)(DWORD) = 0;
    D3D8_STDMETHOD(GetVertexShader)(DWORD*) = 0;
    D3D8_STDMETHOD(DeleteVertexShader)(DWORD) = 0;
    D3D8_STDMETHOD(SetVertexShaderConstant)(DWORD, const void*, DWORD) = 0;
    D3D8_STDMETHOD(GetVertexShaderConstant)(DWORD, void*, DWORD) = 0;                // 80
    D3D8_STDMETHOD(GetVertexShaderDeclaration)(DWORD, void*, DWORD*) = 0;
    D3D8_STDMETHOD(GetVertexShaderFunction)(DWORD, void*, DWORD*) = 0;
    D3D8_STDMETHOD(SetStreamSource)(UINT, IDirect3DVertexBuffer8*, UINT) = 0;
    D3D8_STDMETHOD(GetStreamSource)(UINT, IDirect3DVertexBuffer8**, UINT*) = 0;
    D3D8_STDMETHOD(SetIndices)(IDirect3DIndexBuffer8*, UINT) = 0;                    // 85
    D3D8_STDMETHOD(GetIndices)(IDirect3DIndexBuffer8**, UINT*) = 0;
    D3D8_STDMETHOD(CreatePixelShader)(const DWORD*, DWORD*) = 0;
    D3D8_STDMETHOD(SetPixelShader)(DWORD) = 0;
    D3D8_STDMETHOD(GetPixelShader)(DWORD*) = 0;
    D3D8_STDMETHOD(DeletePixelShader)(DWORD) = 0;                                    // 90
    D3D8_STDMETHOD(SetPixelShaderConstant)(DWORD, const void*, DWORD) = 0;
    D3D8_STDMETHOD(GetPixelShaderConstant)(DWORD, void*, DWORD) = 0;
    D3D8_STDMETHOD(GetPixelShaderFunction)(DWORD, void*, DWORD*) = 0;
    D3D8_STDMETHOD(DrawRectPatch)(UINT, const float*, const void*) = 0;
    D3D8_STDMETHOD(DrawTriPatch)(UINT, const float*, const void*) = 0;               // 95
    D3D8_STDMETHOD(DeletePatch)(UINT) = 0;
};

#define D3D8_RESOURCE \
    D3D8_UNKNOWN \
    D3D8_STDMETHOD(GetDevice)(IDirect3DDevice8**) = 0; \
    D3D8_STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) = 0; \
    D3D8_STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) = 0; \
    D3D8_STDMETHOD(FreePrivateData)(REFGUID) = 0; \
    virtual DWORD __stdcall SetPriority(DWORD) = 0; \
    virtual DWORD __stdcall GetPriority() = 0; \
    virtual void __stdcall PreLoad() = 0; \
    virtual D3DRESOURCETYPE __stdcall GetType() = 0;

struct IDirect3DBaseTexture8 {
    D3D8_RESOURCE
    virtual DWORD __stdcall SetLOD(DWORD) = 0;
    virtual DWORD __stdcall GetLOD() = 0;
    virtual DWORD __stdcall GetLevelCount() = 0;
};

struct IDirect3DTexture8 : IDirect3DBaseTexture8 {
    D3D8_STDMETHOD(GetLevelDesc)(UINT, D3DSURFACE_DESC8*) = 0;                       // 14
    D3D8_STDMETHOD(GetSurfaceLevel)(UINT, IDirect3DSurface8**) = 0;                  // 15
    D3D8_STDMETHOD(LockRect)(UINT, D3DLOCKED_RECT*, const RECT*, DWORD) = 0;         // 16
    D3D8_STDMETHOD(UnlockRect)(UINT) = 0;                                            // 17
    D3D8_STDMETHOD(AddDirtyRect)(const RECT*) = 0;
};

struct IDirect3DCubeTexture8 : IDirect3DBaseTexture8 {
    D3D8_STDMETHOD(GetLevelDesc)(UINT, D3DSURFACE_DESC8*) = 0;
    D3D8_STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES, UINT, IDirect3DSurface8**) = 0;
    D3D8_STDMETHOD(LockRect)(D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD) = 0;
    D3D8_STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES, UINT) = 0;
    D3D8_STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, const RECT*) = 0;
};

struct IDirect3DVolumeTexture8 : IDirect3DBaseTexture8 {
    D3D8_STDMETHOD(GetLevelDesc)(UINT, D3DVOLUME_DESC8*) = 0;
    D3D8_STDMETHOD(GetVolumeLevel)(UINT, IDirect3DVolume8**) = 0;
    D3D8_STDMETHOD(LockBox)(UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD) = 0;
    D3D8_STDMETHOD(UnlockBox)(UINT) = 0;
    D3D8_STDMETHOD(AddDirtyBox)(const D3DBOX*) = 0;
};

struct IDirect3DSurface8 {
    D3D8_UNKNOWN
    D3D8_STDMETHOD(GetDevice)(IDirect3DDevice8**) = 0;
    D3D8_STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) = 0;
    D3D8_STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) = 0;
    D3D8_STDMETHOD(FreePrivateData)(REFGUID) = 0;
    D3D8_STDMETHOD(GetContainer)(REFIID, void**) = 0;                                // 7
    D3D8_STDMETHOD(GetDesc)(D3DSURFACE_DESC8*) = 0;                                  // 8
    D3D8_STDMETHOD(LockRect)(D3DLOCKED_RECT*, const RECT*, DWORD) = 0;               // 9
    D3D8_STDMETHOD(UnlockRect)() = 0;                                                // 10
};

struct IDirect3DVertexBuffer8 {
    D3D8_RESOURCE
    D3D8_STDMETHOD(Lock)(UINT, UINT, BYTE**, DWORD) = 0;                             // 11
    D3D8_STDMETHOD(Unlock)() = 0;
    D3D8_STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC8*) = 0;
};

struct IDirect3DIndexBuffer8 {
    D3D8_RESOURCE
    D3D8_STDMETHOD(Lock)(UINT, UINT, BYTE**, DWORD) = 0;
    D3D8_STDMETHOD(Unlock)() = 0;
    D3D8_STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC8*) = 0;
};

// IID of IDirect3DTexture8 (for GetContainer)
static const GUID IID_IDirect3DTexture8_ = {0xe4cdd575, 0x2866, 0x4f01, {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}};
