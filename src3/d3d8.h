// Direct3D 8 side of a savestate (GTA III, Vice City): every resource created by game code is tracked and pinned
// (one extra reference), so memory restored from a savestate never points at a destroyed object. Loading
// reconciles reference counts and re-applies the device state captured with the savestate. For savestates of
// another process the objects are re-created from the file (see Write / Recreate).
#pragma once
#include "../src/common.h"
#include "d3d8min.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace d3d {

enum Kind : uint8_t {
    K_Texture, K_VolumeTexture, K_CubeTexture, K_VertexBuffer, K_IndexBuffer, K_RenderTarget, K_DepthStencil,
    K_ImageSurface, K_Count
};
const char* KindName(int k);

struct DevState;

// A lock the game holds across frames (container: texture, or the surface itself when level == ~0u).
struct HeldLock { uint32_t container, surface, level; RECT rect; uint32_t whole, flags, bits; INT pitch; };

struct Capture {
    std::vector<std::pair<IUnknown*, uint32_t>> refs;   // object, references held by the game at save time
    DWORD state = 0;                                      // state block token (D3DSBT_ALL)
    DevState* dev = nullptr;                              // the same state as values (for the savestate file)
    std::vector<HeldLock> locks;
    uint32_t generation = 0;                              // device resets invalidate captures
    // Vertex buffers keep changing after the save (RenderWare packs model vertices into a few shared ones, static
    // and dynamic, and reuses freed parts) while the restored memory remembers the old layout: where their contents
    // are in the savestate file.
    struct Dynamic { IUnknown* obj; uint64_t offset; uint32_t size; };
    mutable std::vector<Dynamic> dynamic;                 // offset from the start of the graphics section
};

struct Sink { virtual void Put(const void* p, size_t n) = 0; };
struct Source { virtual bool Get(void* p, size_t n) = 0; };
using Remap = std::unordered_map<uint32_t, uint32_t>;   // pointer value at save time -> object now

bool InstallHooks();                 // Direct3DCreate8 import hook (call again once a protector filled the imports)
IDirect3DDevice8* Device();
void LogSlots(const char* when);     // which module each key device method goes to (diagnostics)
IDirect3D8* Direct3D();
bool DeviceUsable();                 // device exists and is not lost
uint32_t Generation();

void Track(IUnknown* obj, Kind kind, bool defaultPool);   // a resource created on the game's behalf by our own code
void Collect();                                           // release pinned objects nobody uses any more
size_t TrackedCount();
std::vector<std::pair<IUnknown*, Kind>> TrackedObjects();
uint32_t ShaderCount();              // vertex/pixel shaders the game created (handles are not re-created)

Capture* Save();
void Load(const Capture* c);
void Release(Capture* c);

void Write(const Capture* c, Sink& out);
// After Load(): put the saved contents back into the dynamic buffers (read from the savestate file).
void RestoreDynamic(const Capture* c, HANDLE file, uint64_t gpuOffset);
bool Recreate(Source& in, Remap& remap, std::string& err);
void ApplyState(const Remap& remap);

// Called right before the device is reset: all captures become unusable.
extern void (*onDeviceReset)();

// Overlay text drawn on top of the frame for `ms` milliseconds.
void ShowText(const char* text, DWORD ms);

// Save the next presented frame (with the overlay) as a PNG.
void Screenshot(const char* path);

// Lab options: log a Release that drops a tracked object to zero; force a window instead of fullscreen.
extern bool releaseCheck;
extern bool forceWindowed;
extern int windowX, windowY;

}
