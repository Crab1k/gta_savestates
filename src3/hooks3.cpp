// GTA III / Vice City hooks. Their runtime (Metrowerks MSL) gets heap memory from GlobalAlloc in big chunks and
// manages it itself, so redirecting GlobalAlloc/GlobalFree puts the whole game heap into the arena. PeekMessageA is
// the main-loop safe point, as in San Andreas.
//
// Vice City JP is wrapped by a protector: the Windows loader fills only the protector's import table; the game's own
// table is filled by the protector right before the game's start-up code runs. Hooks in that table are installed
// late: a one-shot hot-patch on GlobalAlloc catches the first call from game code, which comes after the table was
// filled and before the runtime has allocated anything. The protector also rewrote many of the game's import calls
// into calls of its own dispatchers (every Miles call): those functions are hooked at their entry instead.
#include "hooks3.h"
#include "../src/arena.h"
#include "game3.h"
#include <cstring>
#include <map>
#include <vector>

// ------------------------------------------------------------------ import tables
static uintptr_t g_textLo, g_textHi;

static bool Resolved(uintptr_t v) {
    MEMORY_BASIC_INFORMATION mbi;
    return v >= 0x10000 && VirtualQuery((void*)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE;
}

// Every import descriptor table of the exe: the one in the header, plus any other ".idata" section (the game's own
// table when a protector replaced the header's).
static std::vector<IMAGE_IMPORT_DESCRIPTOR*> ImportTables(uint8_t* base) {
    std::vector<IMAGE_IMPORT_DESCRIPTOR*> t;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress) t.push_back((IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress));
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".idata", 6) && sec[i].VirtualAddress != dir.VirtualAddress)
            t.push_back((IMAGE_IMPORT_DESCRIPTOR*)(base + sec[i].VirtualAddress));
    return t;
}

void ImportSlots(void (*fn)(void* ctx, uintptr_t slot, uint32_t count), void* ctx) {
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    for (auto* imp : ImportTables(base))
        for (; imp->Name; imp++) {
            auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            uint32_t n = 0;
            while (iat[n].u1.Function) n++;
            fn(ctx, (uintptr_t)iat, n * 4);
        }
}

// Patch `func` of `dll` in every import table (resolved slots only: an unresolved one would be overwritten later).
// Returns the previous pointer of the first slot patched.
void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = (uint8_t*)mod;
    void* first = nullptr;
    for (auto* imp : ImportTables(base)) {
        for (; imp->Name; imp++) {
            if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
            auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            auto* names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
            if (!names) continue;
            for (int i = 0; iat[i].u1.Function; i++) {
                if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                if (strcmp((const char*)byName->Name, func) != 0) continue;
                void* old = (void*)iat[i].u1.Function;
                if (old == hook) { if (!first) first = old; continue; }
                if (!Resolved((uintptr_t)old)) continue;
                Unprotect((uintptr_t)&iat[i].u1.Function, 4);
                iat[i].u1.Function = (uintptr_t)hook;
                if (!first) first = old;
            }
        }
    }
    return first;
}

void* PatchDelayIat(HMODULE, const char*, const char*, void*) { return nullptr; }

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

static bool InGameText(const void* p) { return (uintptr_t)p >= g_textLo && (uintptr_t)p < g_textHi; }

// ------------------------------------------------------------------ heap and main loop
// The game's runtime takes its heap from one GlobalAlloc call and gives it back through one GlobalFree call, and
// WinMain's loop calls PeekMessageA once per frame. Those call instructions are redirected (in memory) rather than
// the import slots, so a tool that hooks the import table too (DxWnd) or the protector's call dispatchers change
// nothing: the arena gets exactly the runtime's heap, and the main loop is found without looking at return addresses.
static HGLOBAL (WINAPI* RealGlobalAlloc)(UINT, SIZE_T) = &GlobalAlloc;
static HGLOBAL (WINAPI* RealGlobalFree)(HGLOBAL) = &GlobalFree;
bool g_arenaOverflow = false, g_heapHooked = false;
static uint32_t g_arenaAllocs = 0;
static uintptr_t g_peekSlot = 0;

static HGLOBAL ArenaGlobalAlloc(UINT flags, SIZE_T size) {
    if (!(flags & GMEM_MOVEABLE)) {
        if (void* p = arena::Alloc(size, (flags & GMEM_ZEROINIT) != 0)) { g_arenaAllocs++; return (HGLOBAL)p; }
        if (!g_arenaOverflow) Log("arena full: %u-byte allocation fell back to GlobalAlloc; savestates disabled", (unsigned)size);
        g_arenaOverflow = true;
    }
    return RealGlobalAlloc(flags, size);
}

static HGLOBAL WINAPI Site_GlobalAlloc(UINT flags, SIZE_T size) { return ArenaGlobalAlloc(flags, size); }

static HGLOBAL WINAPI Site_GlobalFree(HGLOBAL h) {
    if (h && arena::Free((void*)h)) return nullptr;
    return RealGlobalFree(h);
}

uint32_t ArenaAllocCount() { return g_arenaAllocs; }

// The original goes through the game's import slot, whatever another tool put there.
static BOOL WINAPI Site_PeekMessageA(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT remove) {
    OnMainLoop();
    return (*(BOOL(WINAPI**)(LPMSG, HWND, UINT, UINT, UINT))g_peekSlot)(msg, wnd, lo, hi, remove);
}

static BOOL (WINAPI* RealPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT) = &PeekMessageA;

static BOOL WINAPI Imp_PeekMessageA(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT remove) {
    if ((uintptr_t)_ReturnAddress() == g3->callPeekMessage + 6) OnMainLoop();
    return RealPeekMessageA(msg, wnd, lo, hi, remove);
}

// "call [slot]" (FF 15, 6 bytes) -> "call fn; nop". Returns the slot, 0 if the code is not the expected one.
// Permanent hot-patch of an exported function ("mov edi, edi" + 5 bytes of padding). Returns the function past its
// first instruction: the way to call the original from then on.
static void* HotPatchPermanent(void* fn, void* hook) {
    auto* p = (uint8_t*)fn;
    if (!p || p[0] != 0x8B || p[1] != 0xFF) return nullptr;
    for (int i = 1; i <= 5; i++) if (p[-i] != 0xCC && p[-i] != 0x90) return nullptr;
    DWORD old;
    VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    p[-5] = 0xE9;
    *(int32_t*)(p - 4) = (int32_t)((uintptr_t)hook - (uintptr_t)p);
    p[0] = 0xEB; p[1] = 0xF9;                                            // jmp $-5
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return p + 2;
}

static uintptr_t RedirectCall(uintptr_t at, void* fn, const char* what) {
    auto* p = (uint8_t*)at;
    if (!at || p[0] != 0xFF || p[1] != 0x15) { Log("%s: unexpected code at %08X, not hooked", what, (unsigned)at); return 0; }
    uintptr_t slot = *(uint32_t*)(p + 2);
    DWORD old;
    VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE8;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)fn - (at + 5));
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return slot;
}

void (WINAPI* RealSleep)(DWORD) = &Sleep;

// ------------------------------------------------------------------ Miles streams
// The game opens a stream per track (radio, cutscene). Remember what each handle plays, so that a savestate can
// re-open the track that was playing: the handles themselves belong to the process.
typedef uintptr_t (WINAPI* OpenStream_t)(uintptr_t, const char*, int32_t);
typedef void (WINAPI* StreamHandle_t)(uintptr_t);
typedef void (WINAPI* StreamInt_t)(uintptr_t, int32_t);
static OpenStream_t RealOpenStream;
static StreamHandle_t RealCloseStream;
static StreamInt_t RealStreamVolume, RealStreamPan;
static CRITICAL_SECTION g_streamCs;
static std::map<uintptr_t, StreamFile> g_streams;

// Miles file callbacks (AIL_set_file_callbacks keeps them in four globals). When the game set its own, the open
// callback is wrapped so that each stream's file handle in the game is known: Vice City streams through MSL FILEs,
// which live in the game heap and must be closed with it.
typedef uint32_t (WINAPI* FileOpen_t)(const char*, uint32_t*);
typedef void (WINAPI* FileClose_t)(uint32_t);
static uintptr_t g_cbVar[4];   // open, close, seek, read
static FileOpen_t g_gameOpen;
static FileClose_t g_gameClose;
static bool g_openingStream;
static uint32_t g_streamFile;

static uint32_t WINAPI Wrap_FileOpen(const char* name, uint32_t* handle) {
    uint32_t r = g_gameOpen(name, handle);
    if (g_openingStream && r && handle) g_streamFile = *handle;
    return r;
}

static void FindFileCallbacks(HMODULE mss) {
    auto* p = mss ? (uint8_t*)GetProcAddress(mss, "_AIL_set_file_callbacks@16") : nullptr;
    int n = 0;
    for (int i = 0; p && i < 0x60 && n < 4; i++)
        if (p[i] == 0xA3) { g_cbVar[n++] = *(uint32_t*)(p + i + 1); i += 4; }   // mov [var], eax
    if (n != 4) memset(g_cbVar, 0, sizeof(g_cbVar));
}

// The game registers its callbacks while starting its audio: take them over on the first stream.
static void WrapFileCallbacks() {
    if (!g_cbVar[0] || g_gameOpen) return;
    HMODULE mss = GetModuleHandleA("mss32.dll");
    auto own = [&](uintptr_t v) {
        HMODULE m = nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)v, &m) && m == mss;
    };
    uintptr_t open = Mem<uintptr_t>(g_cbVar[0]), close = Mem<uintptr_t>(g_cbVar[1]);
    if (!open || own(open) || open == (uintptr_t)&Wrap_FileOpen) return;   // Miles' own file functions
    g_gameOpen = (FileOpen_t)open;
    g_gameClose = (FileClose_t)close;
    Mem<uintptr_t>(g_cbVar[0]) = (uintptr_t)&Wrap_FileOpen;
    Log("Miles: the game's file callbacks are used for streams (open %p, close %p)", (void*)open, (void*)close);
}

void CloseGameFile(uint32_t file) {
    if (file && g_gameClose) g_gameClose(file);
}

static uintptr_t WINAPI Hook_OpenStream(uintptr_t dig, const char* name, int32_t mem) {
    WrapFileCallbacks();
    // Vice City's open callback renames the radio's "X.ADF" to "X.mp3" in the name it is given (so that Miles decodes
    // it as MP3): keep the name the game asked for, which is the file that exists.
    char asked[MAX_PATH] = {};
    if (name) strncpy_s(asked, name, _TRUNCATE);
    g_openingStream = true;
    g_streamFile = 0;
    uintptr_t h = RealOpenStream(dig, name, mem);
    g_openingStream = false;
    if (h && name) {
        EnterCriticalSection(&g_streamCs);
        StreamFile& f = g_streams[h];
        strcpy_s(f.name, asked);
        f.volume = 127;
        f.pan = 64;
        f.file = g_gameOpen ? g_streamFile : 0;
        LeaveCriticalSection(&g_streamCs);
    }
    return h;
}

static void WINAPI Hook_CloseStream(uintptr_t h) {
    EnterCriticalSection(&g_streamCs);
    g_streams.erase(h);
    LeaveCriticalSection(&g_streamCs);
    RealCloseStream(h);
}

static void WINAPI Hook_StreamVolume(uintptr_t h, int32_t v) {
    EnterCriticalSection(&g_streamCs);
    auto it = g_streams.find(h);
    if (it != g_streams.end()) it->second.volume = v;
    LeaveCriticalSection(&g_streamCs);
    RealStreamVolume(h, v);
}

static void WINAPI Hook_StreamPan(uintptr_t h, int32_t v) {
    EnterCriticalSection(&g_streamCs);
    auto it = g_streams.find(h);
    if (it != g_streams.end()) it->second.pan = v;
    LeaveCriticalSection(&g_streamCs);
    RealStreamPan(h, v);
}

bool StreamFileOf(uintptr_t stream, StreamFile& out) {
    EnterCriticalSection(&g_streamCs);
    auto it = g_streams.find(stream);
    bool ok = it != g_streams.end();
    if (ok) out = it->second;
    LeaveCriticalSection(&g_streamCs);
    return ok;
}

uintptr_t OpenStream(uintptr_t driver, const StreamFile& f) {
    if (!RealOpenStream || !driver) return 0;
    char name[MAX_PATH];   // the game's callback may write to it
    strcpy_s(name, f.name);
    uintptr_t h = Hook_OpenStream(driver, name, 0);
    // Savestates made before the fix above hold the renamed radio file ("AUDIO\VCPR.mp3" for AUDIO\VCPR.ADF).
    size_t n = strlen(f.name);
    if (!h && n > 4 && !strcmp(f.name + n - 4, ".mp3")) {
        strcpy_s(name, f.name);
        strcpy_s(name + n - 4, sizeof(name) - (n - 4), ".ADF");
        h = Hook_OpenStream(driver, name, 0);
    }
    if (h) {
        Hook_StreamVolume(h, f.volume);
        Hook_StreamPan(h, f.pan);
    }
    return h;
}

void CloseStream(uintptr_t stream) {
    if (stream && RealCloseStream) Hook_CloseStream(stream);
}

// Entry hook for the Miles functions, which all start with "inc word [counter]" (7 bytes, no relative operand): the
// trampoline runs that instruction and jumps back after it.
static void* EntryHook(void* fn, void* hook) {
    auto* p = (uint8_t*)fn;
    if (!p || p[0] != 0x66 || p[1] != 0xFF || p[2] != 0x05) return nullptr;
    auto* t = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, p, 7);
    t[7] = 0xE9;
    *(int32_t*)(t + 8) = (int32_t)((uintptr_t)p + 7 - ((uintptr_t)t + 12));
    DWORD old;
    VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - ((uintptr_t)p + 5));
    p[5] = 0x90; p[6] = 0x90;
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    return t;
}

static void HookMilesStreams(HMODULE exe) {
    InitializeCriticalSection(&g_streamCs);
    struct { const char* name; void* hook; void** real; } h[] = {
        {"_AIL_open_stream@12", (void*)&Hook_OpenStream, (void**)&RealOpenStream},
        {"_AIL_close_stream@4", (void*)&Hook_CloseStream, (void**)&RealCloseStream},
        {"_AIL_set_stream_volume@8", (void*)&Hook_StreamVolume, (void**)&RealStreamVolume},
        {"_AIL_set_stream_pan@8", (void*)&Hook_StreamPan, (void**)&RealStreamPan},
    };
    // at the functions' entry when their code is the expected one (every caller, however it calls), else in the imports
    HMODULE mss = GetModuleHandleA("mss32.dll");
    FindFileCallbacks(mss);
    bool entry = mss != nullptr;
    for (auto& e : h) {
        auto* p = mss ? (uint8_t*)GetProcAddress(mss, e.name) : nullptr;
        entry = entry && p && p[0] == 0x66 && p[1] == 0xFF && p[2] == 0x05;
    }
    int n = 0;
    for (auto& e : h) {
        void* old = entry ? EntryHook((void*)GetProcAddress(mss, e.name), e.hook) : PatchIat(exe, "mss32.dll", e.name, e.hook);
        if (old && old != e.hook) { *e.real = old; n++; }
    }
    Log("Miles stream hooks: %d of 4 (%s)", n, entry ? "function entries" : "imports");
    if (n != 4) RealOpenStream = nullptr;   // all or nothing: a partly tracked stream could not be re-opened
}

// ------------------------------------------------------------------ installation
static void (*g_lateInit)() = nullptr;
static uint8_t* g_hotFn = nullptr;
static uint8_t g_hotSaved[7];

static void PatchGameImports() {
    HMODULE exe = GetModuleHandleA(nullptr);
    bool alloc = RedirectCall(g3->callGlobalAlloc, (void*)&Site_GlobalAlloc, "GlobalAlloc") != 0;
    bool freed = false;
    if (g3->callGlobalFree) freed = RedirectCall(g3->callGlobalFree, (void*)&Site_GlobalFree, "GlobalFree") != 0;
    else if (void* orig = HotPatchPermanent((void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GlobalFree"),
                                            (void*)&Site_GlobalFree)) {   // every caller: arena blocks go to the arena
        RealGlobalFree = (decltype(RealGlobalFree))orig;
        freed = true;
    }
    if (g3->peekViaImport) {
        void* old = PatchIat(exe, "user32.dll", "PeekMessageA", (void*)&Imp_PeekMessageA);
        if (old && old != (void*)&Imp_PeekMessageA) RealPeekMessageA = (decltype(RealPeekMessageA))old;
        g_peekSlot = old ? 1 : 0;
    } else {
        g_peekSlot = RedirectCall(g3->callPeekMessage, (void*)&Site_PeekMessageA, "PeekMessageA");
    }
    g_heapHooked = alloc && freed;
    Log("game calls: GlobalAlloc %s, GlobalFree %s, main loop %s", alloc ? "redirected" : "-", freed ? "redirected" : "-",
        g_peekSlot ? "redirected" : "-");
    HookMilesStreams(exe);
    if (g_lateInit) g_lateInit();
}

// One-shot hot-patch: the first GlobalAlloc call from game code installs the import hooks, then the patch goes.
static void RemoveHotPatch() {
    if (!g_hotFn) return;
    DWORD old;
    VirtualProtect(g_hotFn - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_hotFn - 5, g_hotSaved, 7);
    VirtualProtect(g_hotFn - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_hotFn - 5, 7);
    g_hotFn = nullptr;
}

static HGLOBAL WINAPI Hot_GlobalAlloc(UINT flags, SIZE_T size) {
    auto orig = (HGLOBAL(WINAPI*)(UINT, SIZE_T))(g_hotFn + 2);   // past "mov edi, edi"
    if (!InGameText(_ReturnAddress())) return orig(flags, size);
    auto exported = (decltype(RealGlobalAlloc))g_hotFn;
    RemoveHotPatch();
    RealGlobalAlloc = exported;
    Log("first GlobalAlloc from game code: the protector is done, hooking the game");
    PatchGameImports();
    return ArenaGlobalAlloc(flags, size);
}

static bool HotPatch(void* fn, void* hook) {
    auto* p = (uint8_t*)fn;
    if (p[0] != 0x8B || p[1] != 0xFF) return false;                    // mov edi, edi
    for (int i = 1; i <= 5; i++) if (p[-i] != 0xCC && p[-i] != 0x90) return false;
    memcpy(g_hotSaved, p - 5, 7);
    DWORD old;
    VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    p[-5] = 0xE9;
    *(int32_t*)(p - 4) = (int32_t)((uintptr_t)hook - (uintptr_t)p);
    p[0] = 0xEB; p[1] = 0xF9;                                            // jmp $-5
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    g_hotFn = p;
    return true;
}

bool InstallHooks3(void (*lateInit)()) {
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    g_textLo = (uintptr_t)base + sec[0].VirtualAddress;
    g_textHi = g_textLo + sec[0].Misc.VirtualSize;
    g_lateInit = lateInit;
    if (!g3->lateImports) {
        PatchGameImports();
        return true;
    }
    // GlobalAlloc as the game will call it: kernel32's export (a thunk into kernelbase)
    void* fn = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GlobalAlloc");
    if (!HotPatch(fn, (void*)&Hot_GlobalAlloc)) { Log("cannot hot-patch GlobalAlloc: savestates disabled"); return false; }
    Log("waiting for the protector to fill the game's imports (GlobalAlloc hot-patched at %p)", fn);
    return true;
}
