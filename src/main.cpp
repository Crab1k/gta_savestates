// savestate.dll: full savestates for GTA San Andreas (loaded by GTASA-Savestates.exe, or by an ASI loader).
//   F5 save to the current slot, F9 load it, F6/F7 previous/next slot (savestate.ini to change keys).
#include "arena.h"
#include "common.h"
#include "d3d.h"
#include "game.h"
#include "hooks.h"
#include "lab.h"
#include "state.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static char g_gameDir[MAX_PATH], g_selfDir[MAX_PATH], g_iniPath[MAX_PATH];

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static int IniInt(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_iniPath); }

// ------------------------------------------------------------------ RenderWare: one vertex buffer per geometry
// The stock RwD3D9CreateVertexBuffer hands out ranges of shared buffers and reuses freed ranges, which would let a
// loaded savestate draw from a range that now holds another model. A buffer of its own never changes contents.
static BOOL __cdecl My_RwD3D9CreateVertexBuffer(UINT stride, UINT size, IDirect3DVertexBuffer9** vb, UINT* offset) {
    auto* dev = Mem<IDirect3DDevice9*>(g_game->rwD3DDevice);
    *offset = 0;
    if (FAILED(dev->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, vb, nullptr))) {
        *vb = nullptr;
        return FALSE;
    }
    d3d::Track(*vb, d3d::K_VertexBuffer, false);
    return TRUE;
}

static void __cdecl My_RwD3D9DestroyVertexBuffer(UINT, UINT, IDirect3DVertexBuffer9* vb, UINT) {
    if (vb) vb->Release();
}

// WinMain frees its argv array (an arena block it allocated at startup) on exit. After a savestate from another
// process was loaded, the block at that address belongs to something else: leave it to the process exit.
static void PatchExit() {
    uintptr_t at = g_game->winMainFreeArgs;
    if (Mem<uint32_t>(at - 5) != 0x20244C8B || Mem<uint8_t>(at) != 0xE8) { Log("WinMain exit: unexpected code"); return; }
    Unprotect(at, 5);
    memset((void*)at, 0x90, 5);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

static void PatchRenderWare() {
    WriteRel(g_game->rwCreateVertexBuffer, 0xE9, (void*)&My_RwD3D9CreateVertexBuffer);
    WriteRel(g_game->rwDestroyVertexBuffer, 0xE9, (void*)&My_RwD3D9DestroyVertexBuffer);
    // D3DResourceSystem::SetUseD3DResourceBuffering(bool) -> always off: freed textures/index buffers are released
    // instead of being recycled with new contents.
    uintptr_t f = g_game->setUseResBuffering;
    Unprotect(f, 8);
    uint8_t code[8] = {0xC6, 0x05, 0, 0, 0, 0, 0x00, 0xC3};   // mov byte ptr [flag], 0 ; ret
    *(uint32_t*)(code + 2) = (uint32_t)g_game->useResBuffering;
    memcpy((void*)f, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), (void*)f, sizeof(code));
    Mem<uint8_t>(g_game->useResBuffering) = 0;
}

// ------------------------------------------------------------------ hotkeys + per-frame work
static int g_keySave, g_keyLoad, g_keyPrev, g_keyNext, g_slot = 0;
static bool g_down[256];
static bool g_showMessages = true;

static bool Pressed(int vk) {
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = d && !g_down[vk & 0xFF];
    g_down[vk & 0xFF] = d;
    return edge;
}

// Lab diagnostic: validate the process heap (where D3D and DirectSound objects live).
static bool g_heapCheck = false;
void HeapCheck(const char* where) {
    static bool reported = false;
    if (!g_heapCheck || reported || HeapValidate(GetProcessHeap(), 0, nullptr)) return;
    reported = true;
    Log("HEAP CHECK: process heap corrupt (detected at %s, frame %u)", where,
        Mem<uint32_t>(g_game->timerFrameCounter));
}

static bool Focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void Say(const std::string& s) {
    Log("%s", s.c_str());
    if (g_showMessages) d3d::ShowText(s.c_str(), 2500);
}

void OnMainLoop() {
    static uint32_t lastFrame = 0;
    uint32_t frame = Mem<uint32_t>(g_game->timerFrameCounter);
    if (frame != lastFrame) {
        lastFrame = frame;
        if (frame % 300 == 0) d3d::Collect();
    }
    ss::Tick();
    lab::Tick();
    if (g_heapCheck && frame % 30 == 0) HeapCheck("frame");
    if (!Focused()) return;
    std::string msg;
    if (Pressed(g_keySave)) {
        if (!ss::Save(g_slot, msg)) msg = "Cannot save: " + msg;
        Say(msg);
    }
    if (Pressed(g_keyLoad)) {
        if (!ss::Load(g_slot, msg) && msg.rfind("Savestate", 0) != 0) msg = "Cannot load: " + msg;
        Say(msg);
    }
    if (Pressed(g_keyPrev)) { g_slot = (g_slot + ss::MAX_SLOTS - 1) % ss::MAX_SLOTS; Say(ss::Describe(g_slot)); }
    if (Pressed(g_keyNext)) { g_slot = (g_slot + 1) % ss::MAX_SLOTS; Say(ss::Describe(g_slot)); }
}

// ------------------------------------------------------------------ crash log
// Logs the first access violations (first chance) with the module it happened in and the code addresses found on the stack
// (gta_sa.exe and this ASI), which is usually enough to see the call path.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0;
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || _InterlockedIncrement(&count) > 3) return EXCEPTION_CONTINUE_SEARCH;
    auto* cx = ep->ContextRecord;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    Log("CRASH 0x%08X at %p (%s +0x%X), access %s %p | eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X",
        (unsigned)er->ExceptionCode, er->ExceptionAddress, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
        (unsigned)((uintptr_t)er->ExceptionAddress - (uintptr_t)mod),
        er->NumberParameters > 1 ? (er->ExceptionInformation[0] ? "write" : "read") : "?",
        er->NumberParameters > 1 ? (void*)er->ExceptionInformation[1] : nullptr, (unsigned)cx->Eax, (unsigned)cx->Ebx,
        (unsigned)cx->Ecx, (unsigned)cx->Edx, (unsigned)cx->Esi, (unsigned)cx->Edi, (unsigned)cx->Esp);
    char line[2048] = "CRASH stack:";
    int n = 0;
    for (uintptr_t* sp = (uintptr_t*)cx->Esp; n < 32 && sp < (uintptr_t*)cx->Esp + 2048; sp++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = *sp;
        if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            continue;
        HMODULE m = (HMODULE)mbi.AllocationBase;
        char mn[MAX_PATH] = "?";
        GetModuleFileNameA(m, mn, MAX_PATH);
        const char* base = strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn;
        char e[64];
        if (m == GetModuleHandleA(nullptr)) sprintf_s(e, " %X", (unsigned)v);
        else sprintf_s(e, " %.12s+%X", base, (unsigned)(v - (uintptr_t)m));
        strcat_s(line, e);
        n++;
    }
    Log("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ entry
static void Init(HMODULE self) {
    InitializeCriticalSection(&g_logCs);
    // Our files (ini, log, savestates) live next to this DLL, so the game folder is never written to.
    GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
    *strrchr(g_gameDir, '\\') = 0;
    GetModuleFileNameA(self, g_selfDir, MAX_PATH);
    *strrchr(g_selfDir, '\\') = 0;
    char logPath[MAX_PATH];
    sprintf_s(logPath, "%s\\savestate.log", g_selfDir);
    g_log = _fsopen(logPath, "w", _SH_DENYNO);
    sprintf_s(g_iniPath, "%s\\savestate.ini", g_selfDir);

    g_game = DetectGame();
    if (!g_game) return;
    Log("savestate.dll: %s", g_game->name);
    AddVectoredExceptionHandler(1, &CrashFilter);   // logs the first access violations, never handles them
    {
        HANDLE heaps[64];
        DWORD n = GetProcessHeaps(64, heaps);
        char b[600] = "";
        for (DWORD i = 0; i < n && i < 64; i++) { char e[16]; sprintf_s(e, " %p", heaps[i]); strcat_s(b, e); }
        Log("process heap %p; heaps:%s", GetProcessHeap(), b);
    }

    size_t arenaMB = (size_t)IniInt("Savestate", "ArenaMB", 384);
    char baseText[32] = {};
    GetPrivateProfileStringA("Savestate", "ArenaBase", "0x10100000", baseText, sizeof(baseText), g_iniPath);
    uintptr_t arenaBase = (uintptr_t)strtoul(baseText, nullptr, 0) & ~(uintptr_t)0xFFFF;
    if (!arena::Init(arenaMB << 20, arenaBase)) {
        Log("cannot reserve %u MB for the game heap: savestates disabled (lower ArenaMB)", (unsigned)arenaMB);
        return;
    }
    Log("arena: %u MB reserved at %p%s", (unsigned)arenaMB, arena::Base(),
        (uintptr_t)arena::Base() == arenaBase ? "" : " (NOT at ArenaBase: savestates of other sessions cannot be loaded)");
    if (!InstallHeapHooks()) { Log("heap hooks failed: savestates disabled"); return; }
    PatchRenderWare();
    PatchExit();
    d3d::InstallHooks();
    InstallLoopHooks();

    g_keySave = IniInt("Savestate", "SaveKey", VK_F5);
    g_keyLoad = IniInt("Savestate", "LoadKey", VK_F9);
    g_keyPrev = IniInt("Savestate", "PrevSlotKey", VK_F6);
    g_keyNext = IniInt("Savestate", "NextSlotKey", VK_F7);
    g_showMessages = IniInt("Savestate", "ShowMessages", 1) != 0;
    g_heapCheck = IniInt("Lab", "HeapCheck", 0) != 0;

    char folder[MAX_PATH] = {};
    GetPrivateProfileStringA("Savestate", "Folder", "", folder, MAX_PATH, g_iniPath);
    if (!folder[0]) sprintf_s(folder, "%s\\savestates", g_selfDir);
    ss::Init(folder, IniInt("Savestate", "RestartStreamedAudio", 1) != 0);
    d3d::onDeviceReset = &ss::DropAll;
    lab::Init(g_iniPath, g_gameDir);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Init(h);
    }
    return TRUE;
}
