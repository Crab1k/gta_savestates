// savestate3.dll: full savestates for GTA III and Vice City (loaded by the launcher before the game's code runs).
//   F5 save to the current slot, F9 load it, F6/F7 previous/next slot (savestate.ini to change keys).
#include "../src/arena.h"
#include "../src/common.h"
#include "d3d8.h"
#include "game3.h"
#include "hooks3.h"
#include "lab3.h"
#include "state3.h"
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

// ------------------------------------------------------------------ code patches (in memory only)
static bool PatchBytes(uintptr_t at, const void* expect, const void* bytes, size_t n, const char* what) {
    if (memcmp((void*)at, expect, n) != 0) { Log("%s: unexpected code at %08X, not patched", what, (unsigned)at); return false; }
    Unprotect(at, n);
    memcpy((void*)at, bytes, n);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
    return true;
}

static void PatchGame() {
    // RenderWare's dynamic vertex buffer pool hands a freed buffer of the same size to the next model, which rewrites
    // it: a savestate would then draw another model's vertices. Never reuse; freed buffers are evicted instead.
    uint16_t jz = g3->vbReuseJzBytes, nop2 = 0x9090;
    PatchBytes(g3->vbReuseJz, &jz, &nop2, 2, "vertex buffer reuse");
    // WinMain frees its argv array (an arena block from start-up) on exit; after a load from another process that
    // address belongs to something else. Leave it to the process exit.
    // RenderWare's texture cache hands a destroyed texture of the same size and format to the next texture, which
    // rewrites its pixels: a savestate that kept it would show the new image. Always create a new texture.
    if (g3->texCacheJne) {
        uint8_t jne[2] = {0x75, Mem<uint8_t>(g3->texCacheJne + 1)}, jmp[2] = {0xEB, jne[1]};
        PatchBytes(g3->texCacheJne, jne, jmp, 2, "texture cache");
    }
    if (g3->texListCount) {
        uint8_t cur[6], none[6] = {0x31, 0xF6, 0x90, 0x90, 0x90, 0x90};   // xor esi, esi
        memcpy(cur, (void*)g3->texListCount, 6);
        if (cur[0] == 0x8B && cur[1] == 0x35) PatchBytes(g3->texListCount, cur, none, 6, "released texture list");
        else Log("released texture list: unexpected code at %08X, not patched", (unsigned)g3->texListCount);
    }
    uint8_t call = 0xE8, nop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90}, cur[5];
    memcpy(cur, (void*)g3->winMainFreeArgs, 5);
    if (cur[0] == call) PatchBytes(g3->winMainFreeArgs, cur, nop5, 5, "WinMain exit");
}

static void __cdecl MissionPassedScale(float x, float y) {
    ((void(__cdecl*)(float, float))g3->fontSetScale)(x * 0.95f, y * 0.95f);
}

static void PatchMissionPassed() {
    uintptr_t at = g3->passedScaleCall;
    if (!at || Mem<uint8_t>(at) != 0xE8 || ReadCallTarget(at) != g3->fontSetScale) {
        Log("mission passed text: unexpected code, not changed");
        return;
    }
    WriteRel(at, 0xE8, (void*)&MissionPassedScale);
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

static bool Focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void Say(const std::string& s) {
    Log("%s", s.c_str());
    if (g_showMessages) d3d::ShowText(s.c_str(), 2500);
}

// Vice City skips a running cutscene when its window loses focus. Quitting during a cutscene (closing the window)
// deactivates the window as it is destroyed, after the players are gone, and the skip reads the missing player ped:
// the game is over by then, so the cutscene is dropped first.
static WNDPROC g_gameWndProc;

static LRESULT CALLBACK GuardWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && Mem<uint8_t>(g3->cutsceneRunning) && !Mem<uintptr_t>(g3->players))
        Mem<uint8_t>(g3->cutsceneRunning) = 0;
    return CallWindowProcA(g_gameWndProc, w, m, wp, lp);
}

static void GuardWindow() {
    uintptr_t ps = Mem<uintptr_t>(g3->rsPs);
    HWND w = ps ? Mem<HWND>(ps) : nullptr;
    if (!g3->cutsceneRunning || !w) return;
    g_gameWndProc = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)(uintptr_t)&GuardWndProc);
    if (!g_gameWndProc) Log("window procedure not guarded (error %lu)", GetLastError());
}

void OnMainLoop() {
    static int slotLogs = 0;
    if (slotLogs < 1) {
        slotLogs++;
        GuardWindow();
        d3d::LogSlots("in the main loop");
        if (GetModuleHandleA("dxwnd.dll"))   // seen crashing right after the game's first device reset
            Log("WARNING: DxWnd is attached to the game. Use [Display] Windowed=1 instead and keep DxWnd away from it.");
    }
    static uint32_t lastFrame = 0;
    uint32_t frame = Mem<uint32_t>(g3->timerFrame);
    if (frame != lastFrame) {
        lastFrame = frame;
        if (frame % 300 == 0) d3d::Collect();
    }
    ss::Tick();
    lab::Tick();
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
// Once the game's code and import table are final (at once for GTA III; Vice City JP: after its protector ran).
static void LateInit() {
    AddVectoredExceptionHandler(1, &CrashFilter);   // not before: Vice City's protector raises and handles its own
    PatchGame();
    PatchMissionPassed();
    lab::LatePatch();
    d3d::InstallHooks();
    ss::LateInit();
}

static void Init(HMODULE self) {
    InitializeCriticalSection(&g_logCs);
    GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
    *strrchr(g_gameDir, '\\') = 0;
    GetModuleFileNameA(self, g_selfDir, MAX_PATH);
    *strrchr(g_selfDir, '\\') = 0;
    char logPath[MAX_PATH];
    sprintf_s(logPath, "%s\\savestate.log", g_selfDir);
    g_log = _fsopen(logPath, "w", _SH_DENYNO);
    sprintf_s(g_iniPath, "%s\\savestate.ini", g_selfDir);

    g3 = DetectGame3();
    if (!g3) return;
    Log("savestate3.dll: %s", g3->name);

    size_t arenaMB = (size_t)IniInt("Savestate", "ArenaMB", 384);
    char baseText[32] = {};
    GetPrivateProfileStringA("Savestate", "ArenaBase", "0x30000000", baseText, sizeof(baseText), g_iniPath);   // mss32.dll sits at 0x21100000
    uintptr_t arenaBase = (uintptr_t)strtoul(baseText, nullptr, 0) & ~(uintptr_t)0xFFFF;
    // what already sits where the arena should go
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = arenaBase; a < arenaBase + (arenaMB << 20) && VirtualQuery((void*)a, &mbi, sizeof(mbi));
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State == MEM_FREE) continue;
        char mod[MAX_PATH] = "";
        if (mbi.Type == MEM_IMAGE) GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, MAX_PATH);
        Log("arena range is used at %p (allocation %p, %X bytes, state %X type %X) %s", mbi.BaseAddress,
            mbi.AllocationBase, (unsigned)mbi.RegionSize, (unsigned)mbi.State, (unsigned)mbi.Type, mod);
    }
    if (!arena::Init(arenaMB << 20, arenaBase)) {
        Log("cannot reserve %u MB for the game heap: savestates disabled (lower ArenaMB)", (unsigned)arenaMB);
        return;
    }
    Log("arena: %u MB reserved at %p%s", (unsigned)arenaMB, arena::Base(),
        (uintptr_t)arena::Base() == arenaBase ? "" : " (NOT at ArenaBase: savestates of other sessions cannot be loaded)");
    g_keySave = IniInt("Savestate", "SaveKey", VK_F5);
    g_keyLoad = IniInt("Savestate", "LoadKey", VK_F9);
    g_keyPrev = IniInt("Savestate", "PrevSlotKey", VK_F6);
    g_keyNext = IniInt("Savestate", "NextSlotKey", VK_F7);
    g_showMessages = IniInt("Savestate", "ShowMessages", 1) != 0;
    if (IniInt("Display", "Windowed", 0)) {   // the games have no windowed mode of their own
        d3d::forceWindowed = true;
        d3d::windowX = IniInt("Display", "WindowX", 40);
        d3d::windowY = IniInt("Display", "WindowY", 40);
    }

    char folder[MAX_PATH] = {};
    GetPrivateProfileStringA("Savestate", "Folder", "", folder, MAX_PATH, g_iniPath);
    if (!folder[0]) sprintf_s(folder, "%s\\savestates", g_selfDir);
    ss::Init(folder);
    d3d::onDeviceReset = &ss::DropAll;
    lab::Init(g_iniPath, g_gameDir, g_selfDir);
    if (!InstallHooks3(&LateInit)) return;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Init(h);
    }
    return TRUE;
}
