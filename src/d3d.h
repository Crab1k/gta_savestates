// D3D9 side of a savestate: every resource created by game code is tracked and pinned (one extra reference),
// so memory restored from a savestate never points at a destroyed object. Loading reconciles reference counts
// and re-applies the device state captured with the savestate.
#pragma once
#include "common.h"
#include <d3d9.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace d3d {

struct DevState;

// A lock the game holds across frames (container: texture, or the surface itself when level == ~0u).
struct HeldLock { uint32_t container, surface, level; RECT rect; uint32_t whole, flags, bits; INT pitch; };

struct Capture {
    std::vector<std::pair<IUnknown*, uint32_t>> refs;   // object, references held by the game at save time
    IDirect3DStateBlock9* state = nullptr;
    DevState* dev = nullptr;                              // the same state as values (for the savestate file)
    std::vector<HeldLock> locks;
    std::vector<IUnknown*> permanent;                     // pinned for good while this memory is current
    uint32_t generation = 0;                              // device resets invalidate captures
};

struct Sink { virtual void Put(const void* p, size_t n) = 0; };
struct Source { virtual bool Get(void* p, size_t n) = 0; };
using Remap = std::unordered_map<uint32_t, uint32_t>;   // pointer value at save time -> object now

// What a tracked object is (how to re-create it).
enum Kind : uint8_t {
    K_Texture, K_VolumeTexture, K_CubeTexture, K_VertexBuffer, K_IndexBuffer, K_RenderTarget, K_DepthStencil,
    K_OffscreenPlain, K_SwapChain, K_StateBlock, K_VertexDecl, K_VertexShader, K_PixelShader, K_Query, K_Count
};
const char* KindName(int k);

bool InstallHooks();                 // Direct3DCreate9 import hook
IDirect3DDevice9* Device();
bool DeviceUsable();                 // device exists and is not lost
uint32_t Generation();

void Track(IUnknown* obj, Kind kind, bool defaultPool);   // a resource created on the game's behalf by our own code
void Collect();                                // release pinned objects nobody uses any more
size_t TrackedCount();
std::vector<std::pair<IUnknown*, Kind>> TrackedObjects();

Capture* Save();
void Load(const Capture* c);
void Release(Capture* c);

// Persistent form of a capture: every object the game holds with what re-creating it takes (creation parameters,
// contents of managed textures and buffers, shader code), then the device state.
void Write(const Capture* c, Sink& out);
// Loading a written capture in another process (or after a device reset): re-create its objects, make them the
// game's (with the reference counts it had), and drop the game's references to every object it held before.
// Fills `remap` with old pointer -> new object. Then the caller rewrites game memory and calls ApplyState.
bool Recreate(Source& in, Remap& remap, std::string& err);
void ApplyState(const Remap& remap);
IDirect3D9* Direct3D();

// Called right before the device is reset: all captures become unusable.
extern void (*onDeviceReset)();

// Overlay text drawn on top of the frame for `ms` milliseconds.
void ShowText(const char* text, DWORD ms);

// Save the next presented frame (with the overlay) as a PNG.
void Screenshot(const char* path);

// Lab diagnostic: log any Release that drops a tracked object to zero references.
extern bool releaseCheck;

// Lab option: force a window instead of fullscreen.
extern bool forceWindowed;
extern int windowX, windowY;

}
