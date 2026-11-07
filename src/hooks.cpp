// Import-table hooks on gta_sa.exe: heap -> arena, PeekMessageA -> main-loop safe point, Sleep -> thread parking.
#include "hooks.h"
#include "arena.h"
#include "game.h"
#include <cstring>

// ------------------------------------------------------------------ IAT patching
void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    HMODULE target = GetModuleHandleA(dll);
    void* real = target ? (void*)GetProcAddress(target, func) : nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto* names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        for (int i = 0; iat[i].u1.Function; i++) {
            bool match = false;
            if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = strcmp((const char*)byName->Name, func) == 0;
            } else if (real) {
                match = (void*)iat[i].u1.Function == real;
            }
            if (!match) continue;
            void* old = (void*)iat[i].u1.Function;
            Unprotect((uintptr_t)&iat[i].u1.Function, 4);
            iat[i].u1.Function = (uintptr_t)hook;
            return old;
        }
    }
    return nullptr;
}

void* PatchDelayIat(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dir.VirtualAddress); d->DllNameRVA; d++) {
        if (!d->Attributes.RvaBased) continue;   // VC6-style VA descriptors are not used by this exe
        if (_stricmp((const char*)(base + d->DllNameRVA), dll) != 0) continue;
        auto* iat = (IMAGE_THUNK_DATA*)(base + d->ImportAddressTableRVA);
        auto* names = (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            // The hook resolves the real function itself (the delay-load helper never runs for this import).
            void* old = (void*)iat[i].u1.Function;
            Unprotect((uintptr_t)&iat[i].u1.Function, 4);
            iat[i].u1.Function = (uintptr_t)hook;
            return old;
        }
    }
    return nullptr;
}

bool InGameImage(const void* p) {
    static uintptr_t lo = 0, hi = 0;
    if (!lo) {
        auto* base = (uint8_t*)GetModuleHandleA(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        lo = (uintptr_t)base;
        hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    return (uintptr_t)p >= lo && (uintptr_t)p < hi;
}

// ------------------------------------------------------------------ heap
static LPVOID (WINAPI* RealHeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL   (WINAPI* RealHeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID (WINAPI* RealHeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T (WINAPI* RealHeapSize)(HANDLE, DWORD, LPCVOID);
static HANDLE (WINAPI* RealHeapCreate)(DWORD, SIZE_T, SIZE_T);
bool g_arenaOverflow = false;

static void NoteOverflow(SIZE_T size) {
    if (!g_arenaOverflow) Log("arena full: %u-byte allocation fell back to the process heap; savestates disabled", (unsigned)size);
    g_arenaOverflow = true;
}

static LPVOID WINAPI Hook_HeapAlloc(HANDLE h, DWORD flags, SIZE_T size) {
    if (void* p = arena::Alloc(size, (flags & HEAP_ZERO_MEMORY) != 0)) return p;
    NoteOverflow(size);
    return RealHeapAlloc(h, flags, size);
}

static BOOL WINAPI Hook_HeapFree(HANDLE h, DWORD flags, LPVOID p) {
    if (!p) return TRUE;
    if (arena::Free(p)) return TRUE;
    return RealHeapFree(h, flags, p);
}

static LPVOID WINAPI Hook_HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T size) {
    const bool inPlace = (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0;
    const bool zero = (flags & HEAP_ZERO_MEMORY) != 0;
    if (arena::Contains(p)) {
        if (void* q = arena::Realloc(p, size, inPlace, zero)) return q;
        if (inPlace) return nullptr;
        NoteOverflow(size);
        void* q = RealHeapAlloc(h, flags & HEAP_ZERO_MEMORY, size);
        if (q) {
            size_t old = arena::Size(p);
            memcpy(q, p, old < size ? old : size);
            arena::Free(p);
        }
        return q;
    }
    if (inPlace || !p) return p ? RealHeapReAlloc(h, flags, p, size) : Hook_HeapAlloc(h, flags, size);
    // A block from before the hook: move it into the arena.
    SIZE_T old = RealHeapSize(h, 0, p);
    void* q = arena::Alloc(size, false);
    if (!q) return RealHeapReAlloc(h, flags, p, size);
    memcpy(q, p, old < size ? old : size);
    if (zero && size > old) memset((uint8_t*)q + old, 0, size - old);
    RealHeapFree(h, 0, p);
    return q;
}

static SIZE_T WINAPI Hook_HeapSize(HANDLE h, DWORD flags, LPCVOID p) {
    if (arena::Contains(p)) return arena::Size(p);
    return RealHeapSize(h, flags, p);
}

static HANDLE WINAPI Hook_HeapCreate(DWORD opts, SIZE_T init, SIZE_T max) {
    HANDLE h = RealHeapCreate(opts, init, max);
    Log("game HeapCreate(0x%X, %u, %u) = %p (CRT heap created after the arena: every CRT block is captured)",
        opts, (unsigned)init, (unsigned)max, h);
    return h;
}

bool InstallHeapHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    RealHeapAlloc = (decltype(RealHeapAlloc))PatchIat(exe, "kernel32.dll", "HeapAlloc", (void*)&Hook_HeapAlloc);
    RealHeapFree = (decltype(RealHeapFree))PatchIat(exe, "kernel32.dll", "HeapFree", (void*)&Hook_HeapFree);
    RealHeapReAlloc = (decltype(RealHeapReAlloc))PatchIat(exe, "kernel32.dll", "HeapReAlloc", (void*)&Hook_HeapReAlloc);
    RealHeapSize = (decltype(RealHeapSize))PatchIat(exe, "kernel32.dll", "HeapSize", (void*)&Hook_HeapSize);
    RealHeapCreate = (decltype(RealHeapCreate))PatchIat(exe, "kernel32.dll", "HeapCreate", (void*)&Hook_HeapCreate);
    bool ok = RealHeapAlloc && RealHeapFree && RealHeapReAlloc && RealHeapSize;
    Log("heap hooks: alloc=%p free=%p realloc=%p size=%p create=%p", RealHeapAlloc, RealHeapFree, RealHeapReAlloc,
        RealHeapSize, RealHeapCreate);
    return ok;
}

// ------------------------------------------------------------------ main loop + thread parking
static BOOL (WINAPI* RealPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
void (WINAPI* RealSleep)(DWORD);

static volatile LONG g_parkRequest = 0;
static volatile LONG g_parkedMask = 0;
static HANDLE g_unparkEvent = nullptr;   // manual reset: set = threads may run
static HANDLE g_wakeEvent = nullptr;     // manual reset: set = a park is requested, cut sleeps short

static BOOL WINAPI Hook_PeekMessageA(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT remove) {
    if ((uintptr_t)_ReturnAddress() == g_game->peekMessageRet) OnMainLoop();
    return RealPeekMessageA(msg, wnd, lo, hi, remove);
}

static LONG AudioThreadBit(DWORD tid) {
    if (tid == Mem<DWORD>(g_game->audioHardware + audio::HW_StreamThread + audio::ST_ThreadId)) return 1;
    if (tid == Mem<DWORD>(g_game->smoothFadeThread + audio::FT_ThreadId)) return 2;
    return 0;
}

static void Park(LONG bit) {
    _InterlockedOr(&g_parkedMask, bit);
    WaitForSingleObject(g_unparkEvent, INFINITE);
    _InterlockedAnd(&g_parkedMask, ~bit);
}

// The audio threads only call Sleep between two units of work, so a Sleep call is a safe place to hold them.
static void WINAPI Hook_Sleep(DWORD ms) {
    LONG bit = AudioThreadBit(GetCurrentThreadId());
    if (!bit) { RealSleep(ms); return; }
    if (g_parkRequest) { Park(bit); RealSleep(ms); return; }
    // Sleep on the wake event instead, so a savestate request doesn't wait out a 100 ms sleep.
    if (WaitForSingleObject(g_wakeEvent, ms) == WAIT_OBJECT_0 && g_parkRequest) Park(bit);
}

static bool ThreadAlive(HANDLE h) {
    DWORD code = 0;
    return h && GetExitCodeThread(h, &code) && code == STILL_ACTIVE;
}

bool ParkAudioThreads(DWORD timeoutMs) {
    uintptr_t st = g_game->audioHardware + audio::HW_StreamThread;
    uintptr_t ft = g_game->smoothFadeThread;
    LONG want = 0;
    if (ThreadAlive(Mem<HANDLE>(st))) want |= 1;
    if (ThreadAlive(Mem<HANDLE>(ft))) want |= 2;
    ResetEvent(g_unparkEvent);
    _InterlockedExchange(&g_parkRequest, 1);
    SetEvent(g_wakeEvent);
    DWORD start = GetTickCount();
    while ((g_parkedMask & want) != want) {
        if (GetTickCount() - start > timeoutMs) {
            Log("park: timeout (want %d, parked %d)", (int)want, (int)g_parkedMask);
            UnparkAudioThreads();
            return false;
        }
        RealSleep(0);
    }
    return true;
}

void UnparkAudioThreads() {
    _InterlockedExchange(&g_parkRequest, 0);
    ResetEvent(g_wakeEvent);
    SetEvent(g_unparkEvent);
}

bool InstallLoopHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    g_unparkEvent = CreateEventA(nullptr, TRUE, TRUE, nullptr);
    g_wakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    RealPeekMessageA = (decltype(RealPeekMessageA))PatchIat(exe, "user32.dll", "PeekMessageA", (void*)&Hook_PeekMessageA);
    RealSleep = (decltype(RealSleep))PatchIat(exe, "kernel32.dll", "Sleep", (void*)&Hook_Sleep);
    if (!RealSleep) RealSleep = &Sleep;
    Log("loop hooks: PeekMessageA=%p Sleep=%p", RealPeekMessageA, RealSleep);
    return RealPeekMessageA != nullptr;
}
