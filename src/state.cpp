// Savestate core. A savestate file holds the writable game sections, every arena page the allocator has touched
// (all-zero pages are only marked in a bitmap), and the game's graphics objects with their contents. Loading writes
// the memory back between two frames, at WinMain's PeekMessageA call, with the audio threads parked and the
// streaming thread idle.
//
// A savestate made earlier in this process reuses the graphics objects it pinned (fast). One made by another
// process (an earlier game session) re-creates them from the file and rewrites every pointer to them in the restored
// memory; values that belong to the running process (handles, threads, the window) are kept as they are.
#include "state.h"
#include "arena.h"
#include "common.h"
#include "d3d.h"
#include "game.h"
#include "hooks.h"

void HeapCheck(const char* where);
#include <mmsystem.h>
#include <dsound.h>
#include <emmintrin.h>
#include <cstdio>
#include <vector>

namespace ss {

bool forceRecreate = false;

namespace {

constexpr uint32_t MAGIC = 0x54534153;   // "SAST"
constexpr uint32_t VERSION = 2;
constexpr uint32_t PAGE = 4096;
constexpr size_t STAGING = 8u << 20;

struct Header {
    uint32_t magic, version;
    uint32_t gameVersion, exeStamp;    // GameVersion; TimeDateStamp of gta_sa.exe
    uint64_t session;                  // the process that wrote it
    uint32_t dataBegin, dataEnd;
    uint32_t arenaBase, arenaUsedEnd;
    uint32_t pages, storedPages;
    uint32_t holdrand;
    uint32_t gameTimeMs, frameCounter;
    uint32_t bbWidth, bbHeight, bbFormat;
    uint32_t ptd[4];                   // CRT per-thread data of the main, CdStream, audio stream and fade threads
    uint32_t rw[6];                    // RenderWare: window, IDirect3D9, adapter, device, depth-stencil, render surface
    uint32_t ds[4];                    // DirectSound device, 3D listener, streaming channel buffers
    uint64_t gpuOffset, gpuBytes;
    uint32_t complete;
    uint32_t reserved[7];
};

struct Slot {
    bool used = false;
    d3d::Capture* gpu = nullptr;       // pinned graphics objects (savestates made by this process)
    Header hdr = {};
    uint64_t bytes = 0;
    DWORD savedTick = 0;
};

Slot g_slots[MAX_SLOTS];
std::string g_dir;
uint8_t* g_staging = nullptr;
uint64_t g_session = 0;
uint32_t g_exeStamp = 0;
bool g_crossOk = false;                // other processes' savestates can be loaded
std::string g_crossWhy = "not checked yet";
bool g_validated = false;

// Free 32-bit address space and the largest free block, in MB.
void AddressSpace(unsigned* freeMB, unsigned* largestMB) {
    MEMORY_BASIC_INFORMATION mbi;
    size_t total = 0, largest = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a += mbi.RegionSize) {
        if (mbi.State == MEM_FREE) {
            total += mbi.RegionSize;
            if (mbi.RegionSize > largest) largest = mbi.RegionSize;
        }
    }
    *freeMB = (unsigned)(total >> 20);
    *largestMB = (unsigned)(largest >> 20);
}

double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

bool PageZero(const uint8_t* p) {
    const __m128i* q = (const __m128i*)p;
    __m128i acc = _mm_setzero_si128();
    for (int i = 0; i < (int)(PAGE / 16); i += 4) {
        acc = _mm_or_si128(acc, _mm_or_si128(_mm_or_si128(_mm_load_si128(q + i), _mm_load_si128(q + i + 1)),
                                             _mm_or_si128(_mm_load_si128(q + i + 2), _mm_load_si128(q + i + 3))));
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi8(acc, _mm_setzero_si128())) == 0xFFFF;
}

struct Writer : d3d::Sink {
    HANDLE f;
    size_t n = 0;
    uint64_t total = 0;
    bool ok = true;
    explicit Writer(HANDLE h) : f(h) {}
    void Flush() {
        DWORD w = 0;
        if (n && (!WriteFile(f, g_staging, (DWORD)n, &w, nullptr) || w != n)) ok = false;
        n = 0;
    }
    void Put(const void* p, size_t len) override {
        auto* s = (const uint8_t*)p;
        total += len;
        while (len && ok) {
            size_t k = STAGING - n < len ? STAGING - n : len;
            memcpy(g_staging + n, s, k);
            n += k; s += k; len -= k;
            if (n == STAGING) Flush();
        }
    }
};

struct Reader : d3d::Source {
    HANDLE f;
    size_t n = 0, pos = 0;
    bool ok = true;
    explicit Reader(HANDLE h) : f(h) {}
    bool Get(void* p, size_t len) override {
        auto* d = (uint8_t*)p;
        while (len && ok) {
            if (pos == n) {
                DWORD r = 0;
                if (!ReadFile(f, g_staging, (DWORD)STAGING, &r, nullptr) || !r) { ok = false; break; }
                n = r; pos = 0;
            }
            size_t k = n - pos < len ? n - pos : len;
            memcpy(d, g_staging + pos, k);
            pos += k; d += k; len -= k;
        }
        return ok;
    }
};

// ------------------------------------------------------------------ game state helpers
uint32_t& HoldRand() {
    auto ptd = ((uint8_t*(__cdecl*)())g_game->getptd)();
    return *(uint32_t*)(ptd + g_game->holdrandOffset);
}

bool CdStreamIdle() {
    uintptr_t streams = Mem<uintptr_t>(g_game->cdStreams);
    int count = Mem<int32_t>(g_game->cdStreamCount);
    for (int i = 0; i < count; i++) {
        uintptr_t s = streams + i * 0x30;
        if (Mem<uint32_t>(s + 0x04) || Mem<uint8_t>(s + 0x0E)) return false;   // nSectorsToRead, bInUse
    }
    uintptr_t q = g_game->cdStreamQueue;
    return Mem<int32_t>(q + 4) == Mem<int32_t>(q + 8);                       // head == tail
}

bool Quiesce(std::string& msg) {
    if (!ParkAudioThreads(2000)) { msg = "audio thread did not pause"; return false; }
    double until = NowMs() + 5000;
    while (!CdStreamIdle()) {
        if (NowMs() > until) {
            UnparkAudioThreads();
            msg = "streaming I/O did not finish";
            return false;
        }
        RealSleep(1);
    }
    return true;
}

// ------------------------------------------------------------------ audio around a load
// The hardware's channel pointer must match the stream thread's own copy; otherwise leave the radio alone.
uintptr_t StreamingChannel() {
    uintptr_t sc = Mem<uintptr_t>(g_game->audioHardware + audio::HW_StreamingChannel);
    uintptr_t viaThread = Mem<uintptr_t>(g_game->audioHardware + audio::HW_StreamThread + audio::ST_StreamingChannel);
    return sc && sc == viaThread ? sc : 0;
}

template <typename F> void ForEachStaticChannel(F f) {
    uintptr_t hw = g_game->audioHardware;
    uint16_t n = Mem<uint16_t>(hw + audio::HW_NumChannels);
    uintptr_t sc = StreamingChannel();
    for (uint16_t i = 0; i < n && i < 256; i++) {
        uintptr_t ch = Mem<uintptr_t>(hw + audio::HW_Channels + 4 * i);
        if (ch && ch != sc) f(ch);
    }
}

template <typename F> void ForEachFadeEntry(F f) {
    for (uint32_t i = 0; i < audio::FT_NumEntries; i++)
        f(g_game->smoothFadeThread + audio::FT_Entries + i * audio::FE_Size);
}

// DirectSound objects a savestate refers to: device, 3D listener, the streaming channel's two buffers.
void DirectSoundObjects(uint32_t out[4]) {
    uintptr_t hw = g_game->audioHardware, sc = StreamingChannel();
    out[0] = Mem<uint32_t>(hw + audio::HW_DirectSound);
    out[1] = Mem<uint32_t>(hw + audio::HW_Listener);
    out[2] = sc ? Mem<uint32_t>(sc + audio::CH_DSBuffer) : 0;
    out[3] = sc ? Mem<uint32_t>(sc + audio::SC_SilenceBuffer) : 0;
}

// Live state, right before it is overwritten: release what the live audio objects own outside game memory.
void AudioBeforeLoad() {
    ForEachStaticChannel([](uintptr_t ch) {
        if (auto* b = Mem<IDirectSoundBuffer*>(ch + audio::CH_DSBuffer)) { b->Stop(); b->Release(); }
        if (auto* b3 = Mem<IUnknown*>(ch + audio::CH_DS3DBuffer)) b3->Release();
        Mem<void*>(ch + audio::CH_DSBuffer) = nullptr;
        Mem<void*>(ch + audio::CH_DS3DBuffer) = nullptr;
    });
    ForEachFadeEntry([](uintptr_t e) {
        uint32_t st = Mem<uint32_t>(e + audio::FE_Status);
        if (auto* b = Mem<IDirectSoundBuffer*>(e)) {
            if (st == 1 || st == 2) { b->Stop(); b->Release(); }
        }
        Mem<void*>(e) = nullptr;
        Mem<uint32_t>(e + audio::FE_Status) = 0;
    });
    uintptr_t sc = StreamingChannel();
    if (!sc) return;
    for (uint32_t off : {audio::SC_Decoder, audio::SC_NextDecoder}) {
        if (void* dec = Mem<void*>(sc + off)) {
            // scalar deleting destructor (vtable slot 6): closes the track file, frees libvorbis state
            auto dtor = (*(void(__thiscall***)(void*, int))dec)[6];
            dtor(dec, 1);
            Mem<void*>(sc + off) = nullptr;
        }
    }
    Mem<int32_t>(sc + audio::SC_State) = audio::SC_StateStopped;
    if (auto* b = Mem<IDirectSoundBuffer*>(sc + audio::CH_DSBuffer)) b->Stop();
}

// A streamed track (radio, cutscene soundtrack, ambience) that was playing at save time is re-opened at its saved
// position after a load; once the stream thread has prepared it, playback is started from the main loop.
struct {
    bool pending = false;
    int8_t flags = 0;
    DWORD deadline = 0;
} g_restart;

bool g_restartStreams = true;

void RequestStreamRestart(uintptr_t sc, int32_t restoredState) {
    if (!g_restartStreams) return;
    uintptr_t st = g_game->audioHardware + audio::HW_StreamThread;
    if (Mem<uint8_t>(st + audio::ST_NeedsService)) return;            // a request was already pending at save
    // -1 started, -3 playing, -4 finishing: something was audible
    if (restoredState != -1 && restoredState != -3 && restoredState != -4) return;
    int32_t active = Mem<int32_t>(st + audio::ST_ActiveTrackId);
    if (active < 0) return;
    int32_t next = Mem<int32_t>(st + audio::ST_NextTrackId);
    if (next == active) next = -1;                                       // the channel already switched to it
    int32_t pos = Mem<int32_t>(st + audio::ST_PlayTimeMs);
    Mem<int32_t>(st + audio::ST_TrackId) = active;
    Mem<int32_t>(st + audio::ST_NextTrackId) = next;
    Mem<uint32_t>(st + audio::ST_StartOffsetMs) = pos > 0 ? (uint32_t)pos : 0;
    Mem<uint8_t>(st + audio::ST_Preparing) = 0;
    Mem<uint8_t>(st + audio::ST_NeedsService) = 1;
    g_restart.pending = true;
    g_restart.flags = Mem<int8_t>(st + audio::ST_TrackFlags) | (Mem<uint8_t>(sc + 0x61) ? 1 : 0);   // + m_bLoopTrack
    g_restart.deadline = GetTickCount() + 3000;
    Log("audio: re-opening stream track %d at %d ms (next %d)", active, pos, next);
}

// Sound effects: the restored sound manager has its sounds on the channels they held at save time, which are empty
// now, so it would take them as finished (loops would start again from the beginning, one-shots would be lost).
// Taken off the channels, they play on virtually like any sound that lost its channel: the manager keeps advancing
// their play time and starts each on a free channel at that point (CAEStaticChannel::Play seeks there; a loop with a
// lead-in before its loop start plays the lead-in again, as the game does for such a sound).
struct ResumedSound { uint32_t index; int16_t playTime; };
std::vector<ResumedSound> g_resumed;
uint32_t g_resumeCheckFrame = 0;

void ResumeSoundEffects() {
    uintptr_t sm = g_game->soundManager;
    g_resumed.clear();
    uint16_t n = Mem<uint16_t>(sm + audio::SM_NumChannels);
    auto* onChannel = Mem<int16_t*>(sm + audio::SM_OnChannel);
    for (uint16_t ch = 0; onChannel && ch < n && ch < 64; ch++) onChannel[ch] = -1;
    for (uint32_t i = 0; i < audio::SM_NumSounds; i++) {
        uintptr_t s = sm + audio::SM_Sounds + i * audio::SND_Size;
        if (!Mem<int16_t>(s + audio::SND_OnChannel)) continue;
        Mem<int16_t>(s + audio::SND_OnChannel) = 0;
        if (Mem<uint16_t>(s + audio::SND_InUse)) g_resumed.push_back({i, Mem<int16_t>(s + audio::SND_PlayTime)});
    }
    g_resumeCheckFrame = g_resumed.empty() ? 0 : Mem<uint32_t>(g_game->timerFrameCounter) + 3;
}

// A few frames after the load: how many of them have a channel again and went on from their saved play time.
void CheckResumedSounds() {
    if (!g_resumeCheckFrame || Mem<uint32_t>(g_game->timerFrameCounter) < g_resumeCheckFrame) return;
    g_resumeCheckFrame = 0;
    uintptr_t sm = g_game->soundManager;
    uint32_t back = 0, onward = 0;
    for (const ResumedSound& r : g_resumed) {
        uintptr_t s = sm + audio::SM_Sounds + r.index * audio::SND_Size;
        if (!Mem<uint16_t>(s + audio::SND_InUse) || !Mem<int16_t>(s + audio::SND_OnChannel)) continue;   // ended
        back++;
        if (Mem<int16_t>(s + audio::SND_PlayTime) >= r.playTime) onward++;   // not: loops with a lead-in replay it
    }
    Log("audio: %u sound effects playing at the save, %u of them on a channel again, %u going on from their position",
        (unsigned)g_resumed.size(), back, onward);
}

// Restored state: drop references to objects that only existed at save time (DirectSound buffers, libvorbis
// decoders and their track files), and let the sound manager start its sounds again where they were.
void AudioAfterLoad() {
    ForEachStaticChannel([](uintptr_t ch) {
        Mem<void*>(ch + audio::CH_DSBuffer) = nullptr;
        Mem<void*>(ch + audio::CH_DS3DBuffer) = nullptr;
        Mem<uint32_t>(ch + audio::CH_BufferStatus) = 0;
    });
    ResumeSoundEffects();
    ForEachFadeEntry([](uintptr_t e) {
        Mem<void*>(e) = nullptr;
        Mem<uint32_t>(e + audio::FE_Status) = 0;
    });
    uintptr_t sc = StreamingChannel();
    if (!sc) return;
    int32_t restoredState = Mem<int32_t>(sc + audio::SC_State);
    // The decoders stay allocated in the restored heap (small, bounded); their track file handles are not ours.
    Mem<void*>(sc + audio::SC_Decoder) = nullptr;
    Mem<void*>(sc + audio::SC_NextDecoder) = nullptr;
    Mem<int32_t>(sc + audio::SC_State) = audio::SC_StateStopped;
    g_restart.pending = false;
    RequestStreamRestart(sc, restoredState);
}

// ------------------------------------------------------------------ values that belong to the process
struct Keep { uintptr_t addr; uint32_t size; std::vector<uint8_t> data; };

void AddKeep(std::vector<Keep>& k, uintptr_t addr, uint32_t size) {
    if (addr && size && addr >= g_game->dataBegin && addr + size <= g_game->dataEnd) k.push_back({addr, size, {}});
    else if (addr && size && arena::Contains((void*)addr)) k.push_back({addr, size, {}});
}

// Delay-load import slots and module handles (they point into other DLLs and at our own hooks).
void AddDelayImports(std::vector<Keep>& k) {
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dir.VirtualAddress); d->DllNameRVA; d++) {
        if (!d->Attributes.RvaBased) continue;
        AddKeep(k, (uintptr_t)base + d->ModuleHandleRVA, 4);
        auto* names = (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA);
        uint32_t n = 0;
        while (names[n].u1.AddressOfData) n++;
        AddKeep(k, (uintptr_t)base + d->ImportAddressTableRVA, n * 4);
    }
}

std::vector<Keep> KeepList(bool otherProcess) {
    std::vector<Keep> k;
    for (auto& rg : g_game->timerOsAnchors) AddKeep(k, rg.addr, rg.size);
    uintptr_t st = g_game->audioHardware + audio::HW_StreamThread;
    AddKeep(k, st + audio::ST_CritSec, 24);
    AddKeep(k, st, 8);                                   // stream thread handle + id
    AddKeep(k, g_game->smoothFadeThread, 8);             // fade thread handle + id
    // psGlobal: window, DirectInput interface and devices (re-created when a savefile is loaded from the menu)
    uintptr_t ps = Mem<uintptr_t>(g_game->rsGlobal + 0x14);
    AddKeep(k, ps, 0x28);
    AddKeep(k, g_game->cdStreamGlobals.addr, g_game->cdStreamGlobals.size);
    AddKeep(k, g_game->tlsIndex, 4);
    if (!otherProcess) return k;
    AddKeep(k, g_game->crtHeap, 4);
    AddKeep(k, g_game->ioinfo - 4, 4 + 64 * 4);        // _nhandle, __pioinfo[]
    AddKeep(k, g_game->crtInitCSCache, 4);
    AddKeep(k, g_game->wmaLibrary, 8);
    AddKeep(k, g_game->rwWindowHandle, 24);              // window, IDirect3D9, adapter, device, DS and RT surfaces
    AddKeep(k, g_game->rwAdapterInfo, 0x350);
    AddKeep(k, g_game->rwPresent, sizeof(D3DPRESENT_PARAMETERS));
    AddDelayImports(k);
    return k;
}

void Capture(std::vector<Keep>& k) {
    for (auto& e : k) e.data.assign((uint8_t*)e.addr, (uint8_t*)e.addr + e.size);
}

void Restore(const std::vector<Keep>& k) {
    for (auto& e : k) memcpy((void*)e.addr, e.data.data(), e.size);
}

// The __pioinfo blocks live in the arena; __pioinfo[] itself is kept live, so after a load from another process the
// live table points at blocks the restored heap no longer has. Re-home the live entries into fresh blocks.
struct IoInfo { std::vector<std::vector<uint8_t>> blocks; };
constexpr uint32_t IOINFO_ENTRIES = 32, IOINFO_SIZE = 0x24;

IoInfo CaptureIoInfo() {
    IoInfo io;
    for (int i = 0; i < 64; i++) {
        auto* b = Mem<uint8_t*>(g_game->ioinfo + 4 * i);
        if (!b) break;
        io.blocks.emplace_back(b, b + IOINFO_ENTRIES * IOINFO_SIZE);
    }
    return io;
}

void RestoreIoInfo(const IoInfo& io) {
    for (size_t i = 0; i < io.blocks.size(); i++) {
        auto* b = (uint8_t*)arena::Alloc(IOINFO_ENTRIES * IOINFO_SIZE, false);
        if (!b) break;
        memcpy(b, io.blocks[i].data(), IOINFO_ENTRIES * IOINFO_SIZE);
        Mem<uint8_t*>(g_game->ioinfo + 4 * i) = b;
    }
}

// ------------------------------------------------------------------ CRT per-thread data
typedef LONG(NTAPI* NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
struct ThreadBasicInfo { LONG exitStatus; PVOID teb; HANDLE pid, tid; ULONG_PTR affinity; LONG prio, basePrio; };

// The TLS slot of another thread, through its TEB (TlsSlots at +0xE10 on x86).
void** TlsSlot(DWORD tid, DWORD index) {
    if (!tid || index >= 64) return nullptr;
    static auto query = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!query) return nullptr;
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return nullptr;
    ThreadBasicInfo tbi = {};
    LONG st = query(h, 0, &tbi, sizeof(tbi), nullptr);
    CloseHandle(h);
    if (st < 0 || !tbi.teb) return nullptr;
    return (void**)((uint8_t*)tbi.teb + 0xE10) + index;
}

void GameThreads(DWORD tids[4]) {
    tids[0] = GetCurrentThreadId();
    tids[1] = Mem<DWORD>(g_game->cdStreamGlobals.addr + 0x20);   // gStreamingThreadId
    tids[2] = Mem<DWORD>(g_game->audioHardware + audio::HW_StreamThread + audio::ST_ThreadId);
    tids[3] = Mem<DWORD>(g_game->smoothFadeThread + audio::FT_ThreadId);
}

struct LivePtd { void** slot = nullptr; std::vector<uint8_t> data; };

void ThreadPtds(uint32_t out[4], LivePtd live[4]) {
    DWORD tids[4];
    GameThreads(tids);
    DWORD index = Mem<DWORD>(g_game->tlsIndex);
    for (int i = 0; i < 4; i++) {
        void** slot = i == 0 ? nullptr : TlsSlot(tids[i], index);
        void* p = i == 0 ? TlsGetValue(index) : slot ? *slot : nullptr;
        if (out) out[i] = (uint32_t)(uintptr_t)p;
        if (live) {
            live[i].slot = slot;
            if (p && arena::Contains(p)) live[i].data.assign((uint8_t*)p, (uint8_t*)p + PTD_SIZE);
        }
    }
}

// The restored heap holds the per-thread data of the threads that existed at save time, but each running thread's
// TLS slot points at its own block from before the load. Give every running thread a block of its own in the
// restored heap (its own values, minus buffers that belonged to the old heap) and free the saved threads' blocks.
void FixPtds(const Header& h, LivePtd live[4]) {
    for (uint32_t p : h.ptd)
        if (p && arena::Contains((void*)(uintptr_t)p)) arena::Free((void*)(uintptr_t)p);
    DWORD index = Mem<DWORD>(g_game->tlsIndex);
    for (int i = 0; i < 4; i++) {
        if (live[i].data.empty()) continue;
        auto* b = (uint8_t*)arena::Alloc(PTD_SIZE, false);
        if (!b) continue;
        memcpy(b, live[i].data.data(), PTD_SIZE);
        memset(b + PTD_BUFFERS, 0, PTD_BUFFERS_END - PTD_BUFFERS);
        if (i == 0) TlsSetValue(index, b);
        else if (live[i].slot) *live[i].slot = b;
    }
}

// ------------------------------------------------------------------ page protection
// The exe declares its data sections writable, but other software can change a page's protection (seen on a player's
// PC with Vice City: a page of its data not writable, and the load crashed half-way). Such pages are made writable
// for the load and get their protection back afterwards.
struct Protection { uintptr_t addr; SIZE_T size; DWORD protect; };

std::vector<Protection> MakeWritable(uintptr_t begin, uintptr_t end) {
    std::vector<Protection> changed;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = begin; a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi));
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        DWORD p = mbi.Protect & 0xFF;
        if (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) continue;
        uintptr_t from = a, to = (uintptr_t)mbi.BaseAddress + mbi.RegionSize < end ? (uintptr_t)mbi.BaseAddress + mbi.RegionSize : end;
        DWORD want = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, old;
        if (VirtualProtect((void*)from, to - from, want, &old)) changed.push_back({from, to - from, mbi.Protect});
        else Log("load: cannot make %08X..%08X writable (protection %X, error %lu)", (unsigned)from, (unsigned)to,
                 (unsigned)mbi.Protect, GetLastError());
    }
    for (auto& c : changed)
        Log("load: %08X..%08X had protection %X, written through", (unsigned)c.addr, (unsigned)(c.addr + c.size),
            (unsigned)c.protect);
    return changed;
}

void RestoreProtection(const std::vector<Protection>& changed) {
    DWORD old;
    for (auto& c : changed) {
        VirtualProtect((void*)c.addr, c.size, c.protect, &old);
        if (c.protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            FlushInstructionCache(GetCurrentProcess(), (void*)c.addr, c.size);
    }
}

// ------------------------------------------------------------------ pointer rewriting
// A savestate of another process names its graphics and DirectSound objects by their old addresses, and the restored
// memory gets the re-created objects in their place: every word that equals an old address, except in script space.
// Only the value marks a pointer, and game data can equal an old address by chance; in script code such a change made
// a script run garbage (GTA III / Vice City, a player's PC: a crash 10 s after a load), and script space never holds
// these pointers. Elsewhere every match is changed: rules for where RenderWare keeps them missed real ones in a
// savestate of the user's (Vice City: arrays of index buffers and textures in RenderWare pools), whose stale pointers
// then crashed the texture code; a chance match outside scripts only changes a pixel or vertex value.
struct RemapScan {
    const d3d::Remap* m;
    uint8_t filter[8192];   // bit per 64 KB granule that holds a key
    uint32_t replaced, scriptMatches;
};

inline bool Match(const RemapScan& r, uint32_t v, d3d::Remap::const_iterator& it) {
    if (!(r.filter[v >> 19] & (1u << ((v >> 16) & 7)))) return false;
    it = r.m->find(v);
    return it != r.m->end();
}

inline void RemapRange(RemapScan& r, uint32_t* p, uint32_t* end) {
    d3d::Remap::const_iterator it;
    for (; p < end; p++)
        if (Match(r, *p, it)) { *p = it->second; r.replaced++; }
}

void RemapBlock(void* ctx, uint8_t* p, uint32_t bytes) {
    RemapRange(*(RemapScan*)ctx, (uint32_t*)p, (uint32_t*)(p + (bytes & ~3u)));
}

uint32_t RemapMemory(const d3d::Remap& m) {
    auto* r = new RemapScan{&m, {}, 0, 0};
    for (auto& [k, v] : m) r->filter[k >> 19] |= (uint8_t)(1u << ((k >> 16) & 7));
    uintptr_t ss = g_game->scriptSpace, ssEnd = ss + 200000 + 69000;
    RemapRange(*r, (uint32_t*)g_game->dataBegin, (uint32_t*)ss);
    RemapRange(*r, (uint32_t*)ssEnd, (uint32_t*)g_game->dataEnd);
    arena::ForEachUsed(&RemapBlock, r);
    d3d::Remap::const_iterator it;
    for (auto* p = (const uint32_t*)ss; p < (const uint32_t*)ssEnd; p++) r->scriptMatches += Match(*r, *p, it);
    if (r->scriptMatches)
        Log("load: %u words of script space equal to an old graphics object's address left as they are", r->scriptMatches);
    uint32_t n = r->replaced;
    delete r;
    return n;
}

// ------------------------------------------------------------------ slots
std::string SlotPath(int slot) {
    char p[MAX_PATH];
    sprintf_s(p, "%s\\slot%02d.sast", g_dir.c_str(), slot + 1);
    return p;
}

void ReleaseSlot(Slot& s) {
    if (s.gpu) { d3d::Release(s.gpu); s.gpu = nullptr; }
    s.used = false;
}

uint32_t ExeStamp() {
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    return ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->FileHeader.TimeDateStamp;
}

bool BackBuffer(uint32_t* w, uint32_t* h, uint32_t* fmt) {
    IDirect3DSurface9* bb = nullptr;
    auto* dev = d3d::Device();
    if (!dev || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    bb->Release();
    *w = d.Width; *h = d.Height; *fmt = (uint32_t)d.Format;
    return true;
}

// Checks that only need the running game (window, device): done once, at the first save or load.
void ValidateOnce() {
    if (g_validated) return;
    g_validated = true;
    uintptr_t ps = Mem<uintptr_t>(g_game->rsGlobal + 0x14);
    HWND wnd = ps ? Mem<HWND>(ps) : nullptr;
    std::string why;
    DWORD index = Mem<DWORD>(g_game->tlsIndex);
    void** own = TlsSlot(GetCurrentThreadId(), index);
    if (!ValidateProcessGlobals(wnd, d3d::Device(), why)) g_crossOk = false;
    else if (!own || *own != TlsGetValue(index)) { why = "cannot reach other threads' TLS"; g_crossOk = false; }
    else g_crossOk = true;
    g_crossWhy = g_crossOk ? "" : why;
    Log("savestates of other sessions: %s%s", g_crossOk ? "supported" : "DISABLED: ", g_crossWhy.c_str());
}

bool ReadHeader(const std::string& path, Header& h) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD r = 0;
    bool ok = ReadFile(f, &h, sizeof(h), &r, nullptr) && r == sizeof(h);
    CloseHandle(f);
    return ok && h.magic == MAGIC;
}

const char* Incompatible(const Header& h) {
    if (h.version != VERSION) return "it was made by another version of the mod";
    if (!h.complete) return "the file is incomplete";
    if (h.gameVersion != (uint32_t)g_game->version || h.exeStamp != g_exeStamp) return "it was made with another gta_sa.exe";
    if (h.dataBegin != g_game->dataBegin || h.dataEnd != g_game->dataEnd) return "it was made with another gta_sa.exe";
    return nullptr;
}

}  // namespace

bool Init(const char* folder, bool restartStreams) {
    g_restartStreams = restartStreams;
    g_dir = folder;
    CreateDirectoryA(g_dir.c_str(), nullptr);
    g_staging = (uint8_t*)VirtualAlloc(nullptr, STAGING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_session = ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)qpc.QuadPart ^ ((uint64_t)GetTickCount() << 16);
    g_exeStamp = ExeStamp();
    int found = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        Header h;
        if (!ReadHeader(SlotPath(i), h)) continue;
        if (const char* why = Incompatible(h)) { Log("slot %d on disk is not usable: %s", i + 1, why); continue; }
        g_slots[i].used = true;
        g_slots[i].hdr = h;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(SlotPath(i).c_str(), GetFileExInfoStandard, &fa))
            g_slots[i].bytes = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        found++;
    }
    Log("savestates in %s (%d from earlier sessions)", g_dir.c_str(), found);
    return g_staging != nullptr;
}

bool CanOperate(std::string& why) {
    if (g_arenaOverflow) { why = "game heap overflowed the savestate arena (raise ArenaMB)"; return false; }
    if (Mem<int32_t>(g_game->gGameState) != 9) { why = "not in game"; return false; }
    if (Mem<uint8_t>(g_game->menuManager + g_game->menuActiveOffset)) { why = "menu is open"; return false; }
    if (!Mem<void*>(g_game->players)) { why = "no player"; return false; }
    if (!d3d::DeviceUsable()) { why = "graphics device not ready"; return false; }
    return true;
}

bool Used(int slot) { return slot >= 0 && slot < MAX_SLOTS && g_slots[slot].used; }

std::string Describe(int slot) {
    char b[128];
    const Slot& s = g_slots[slot];
    if (!s.used) sprintf_s(b, "Slot %d: empty", slot + 1);
    else if (s.hdr.session != g_session) sprintf_s(b, "Slot %d: from an earlier session (%u MB)", slot + 1, (unsigned)(s.bytes >> 20));
    else sprintf_s(b, "Slot %d: saved %u s ago (%u MB)", slot + 1, (unsigned)((GetTickCount() - s.savedTick) / 1000),
                   (unsigned)(s.bytes >> 20));
    return b;
}

// A device reset invalidates the pinned objects; the slots stay usable through their files.
void DropAll() {
    for (auto& s : g_slots)
        if (s.gpu) { d3d::Release(s.gpu); s.gpu = nullptr; }
}

void Tick() {
    CheckResumedSounds();
    if (!g_restart.pending) return;
    uintptr_t st = g_game->audioHardware + audio::HW_StreamThread;
    uintptr_t sc = StreamingChannel();
    if (!sc || (int)(GetTickCount() - g_restart.deadline) > 0) {
        g_restart.pending = false;
        Log("audio: stream track was not prepared in time");
        return;
    }
    // The thread clears the request flag first and prepares the track inside its critical section.
    if (Mem<uint8_t>(st + audio::ST_NeedsService)) return;
    auto* cs = (CRITICAL_SECTION*)(st + audio::ST_CritSec);
    if (!TryEnterCriticalSection(cs)) return;
    bool ready = Mem<void*>(sc + audio::SC_Decoder) != nullptr;
    LeaveCriticalSection(cs);
    g_restart.pending = false;
    if (!ready) { Log("audio: stream track could not be re-opened"); return; }
    auto play = (*(void(__thiscall***)(void*, int16_t, int8_t, float))sc)[audio::CH_VtPlay / 4];
    play((void*)sc, 0, g_restart.flags, 1.0f);
    Log("audio: stream playback restarted");
}

bool Save(int slot, std::string& msg) {
    if (slot < 0 || slot >= MAX_SLOTS) return false;
    if (!CanOperate(msg)) return false;
    ValidateOnce();
    double t0 = NowMs();
    Slot& s = g_slots[slot];
    std::string path = SlotPath(slot), tmp = path + ".tmp";
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { msg = "cannot create " + tmp; return false; }
    if (!Quiesce(msg)) { CloseHandle(f); DeleteFileA(tmp.c_str()); return false; }
    double t1 = NowMs();
    arena::Lock();

    Header h = {};
    h.magic = MAGIC;
    h.version = VERSION;
    h.gameVersion = (uint32_t)g_game->version;
    h.exeStamp = g_exeStamp;
    h.session = g_session;
    h.dataBegin = (uint32_t)g_game->dataBegin;
    h.dataEnd = (uint32_t)g_game->dataEnd;
    h.arenaBase = (uint32_t)(uintptr_t)arena::Base();
    h.arenaUsedEnd = (uint32_t)arena::UsedEnd();
    h.pages = (h.arenaUsedEnd + PAGE - 1) / PAGE;
    h.holdrand = HoldRand();
    h.gameTimeMs = Mem<uint32_t>(g_game->timerTimeMs);
    h.frameCounter = Mem<uint32_t>(g_game->timerFrameCounter);
    BackBuffer(&h.bbWidth, &h.bbHeight, &h.bbFormat);
    ThreadPtds(h.ptd, nullptr);
    memcpy(h.rw, (void*)g_game->rwWindowHandle, sizeof(h.rw));
    DirectSoundObjects(h.ds);

    std::vector<uint8_t> bits((h.pages + 7) / 8, 0);
    const uint8_t* base = arena::Base();
    for (uint32_t p = 0; p < h.pages; p++) {
        if (!PageZero(base + (size_t)p * PAGE)) { bits[p >> 3] |= (uint8_t)(1u << (p & 7)); h.storedPages++; }
    }

    Writer w{f};
    w.Put(&h, sizeof(h));
    w.Put((const void*)g_game->dataBegin, g_game->dataEnd - g_game->dataBegin);
    w.Put(bits.data(), bits.size());
    for (uint32_t p = 0; p < h.pages && w.ok; p++)
        if (bits[p >> 3] & (1u << (p & 7))) w.Put(base + (size_t)p * PAGE, PAGE);
    double t2 = NowMs();

    HeapCheck("before save gpu");
    ReleaseSlot(s);
    s.gpu = d3d::Save();
    arena::Unlock();
    UnparkAudioThreads();
    double t3 = NowMs();

    // The pinned objects keep their contents, so the game may run on while they are written.
    h.gpuOffset = w.total;
    d3d::Write(s.gpu, w);
    h.gpuBytes = w.total - h.gpuOffset;
    w.Flush();
    h.complete = 1;
    DWORD wr = 0;
    bool ok = w.ok && SetFilePointer(f, 0, nullptr, FILE_BEGIN) == 0 && WriteFile(f, &h, sizeof(h), &wr, nullptr) &&
              wr == sizeof(h);
    CloseHandle(f);
    ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DeleteFileA(tmp.c_str());
        ReleaseSlot(s);
        s.used = ReadHeader(path, s.hdr) && !Incompatible(s.hdr);   // the previous file is still there
        msg = "writing the savestate failed (disk full?)";
        Log("save slot %d: write failed (error %u)", slot + 1, (unsigned)GetLastError());
        return false;
    }
    s.used = true;
    s.hdr = h;
    s.bytes = w.total;
    s.savedTick = GetTickCount();
    double t4 = NowMs();
    unsigned freeMB, largestMB;
    AddressSpace(&freeMB, &largestMB);
    char b[160];
    sprintf_s(b, "Savestate %d saved  (%u MB, %.0f ms)", slot + 1, (unsigned)(w.total >> 20), t4 - t0);
    msg = b;
    Log("save slot %d: %u MB (gpu %.1f MB), arena %u MB used (%u/%u pages stored), %u gpu objects | quiesce %.1f ms, "
        "memory %.1f ms, pin %.1f ms, gpu write %.1f ms, game time %u; address space free %u MB (largest %u MB)",
        slot + 1, (unsigned)(w.total >> 20), h.gpuBytes / 1048576.0, h.arenaUsedEnd >> 20, h.storedPages, h.pages,
        (unsigned)s.gpu->refs.size(), t1 - t0, t2 - t1, t3 - t2, t4 - t3, h.gameTimeMs, freeMB, largestMB);
    return true;
}

bool Load(int slot, std::string& msg) {
    if (slot < 0 || slot >= MAX_SLOTS) return false;
    Slot& s = g_slots[slot];
    char b[200];
    if (!s.used) { sprintf_s(b, "Savestate %d is empty", slot + 1); msg = b; return false; }
    if (!CanOperate(msg)) return false;
    ValidateOnce();
    HANDLE f = CreateFileA(SlotPath(slot).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { msg = "the savestate file is gone"; s.used = false; return false; }
    Reader r{f};
    Header h;
    if (!r.Get(&h, sizeof(h)) || h.magic != MAGIC) { CloseHandle(f); msg = "savestate file is damaged"; return false; }
    if (const char* why = Incompatible(h)) { CloseHandle(f); msg = std::string("cannot use it: ") + why; return false; }
    const bool fast = s.gpu && h.session == g_session && s.gpu->generation == d3d::Generation() && !forceRecreate;
    if (!fast) {
        uint32_t bw = 0, bh = 0, bf = 0;
        BackBuffer(&bw, &bh, &bf);
        const char* why = nullptr;
        if (!g_crossOk) why = g_crossWhy.c_str();
        else if (h.arenaBase != (uint32_t)(uintptr_t)arena::Base()) why = "the game heap is at another address (ArenaBase)";
        else if (h.arenaUsedEnd > arena::Reserved()) why = "it needs a larger ArenaMB";
        if (why) { CloseHandle(f); msg = std::string("cannot use it: ") + why; return false; }
        if (bw != h.bbWidth || bh != h.bbHeight || bf != h.bbFormat) {
            CloseHandle(f);
            sprintf_s(b, "cannot use it: it was made at %ux%u (switch the game to that resolution)", h.bbWidth, h.bbHeight);
            msg = b;
            return false;
        }
    }
    HeapCheck("before load");
    double t0 = NowMs();
    if (!Quiesce(msg)) { CloseHandle(f); return false; }
    arena::Lock();
    double t1 = NowMs();

    if (!arena::CommitAtLeast((size_t)h.pages * PAGE)) {
        arena::Unlock();
        UnparkAudioThreads();
        CloseHandle(f);
        msg = "not enough memory for the savestate's heap";
        return false;
    }
    // Everything below must not fail half-way.
    AudioBeforeLoad();
    HeapCheck("after audio cleanup");
    const size_t livePages = (arena::UsedEnd() + PAGE - 1) / PAGE;
    std::vector<Keep> keep = KeepList(!fast);
    Capture(keep);
    LivePtd ptds[4];
    ThreadPtds(nullptr, ptds);
    uint32_t liveDs[4], liveRw[6];
    DirectSoundObjects(liveDs);
    memcpy(liveRw, (void*)g_game->rwWindowHandle, sizeof(liveRw));
    IoInfo io = fast ? IoInfo{} : CaptureIoInfo();

    std::vector<Protection> reprotect = MakeWritable(g_game->dataBegin, g_game->dataEnd);
    r.Get((void*)g_game->dataBegin, g_game->dataEnd - g_game->dataBegin);
    std::vector<uint8_t> bits((h.pages + 7) / 8);
    r.Get(bits.data(), bits.size());
    uint8_t* base = arena::Base();
    const size_t maxPages = livePages > h.pages ? livePages : h.pages;
    for (size_t p = 0; p < maxPages && r.ok; p++) {
        uint8_t* dst = base + p * PAGE;
        if (p < h.pages && (bits[p >> 3] & (1u << (p & 7)))) r.Get(dst, PAGE);
        else if (!PageZero(dst)) memset(dst, 0, PAGE);
    }
    double t2 = NowMs();
    HeapCheck("after memory restore");

    std::string gpuErr;
    d3d::Remap remap;
    uint32_t rewritten = 0;
    if (fast) {
        Restore(keep);
        d3d::Load(s.gpu);
    } else {
        d3d::Recreate(r, remap, gpuErr);
        // RenderWare's copies of the IDirect3D9, the device and its implicit surfaces; the DirectSound objects
        for (int i : {1, 3, 4, 5})
            if (h.rw[i] && liveRw[i]) remap[h.rw[i]] = liveRw[i];
        for (int i = 0; i < 4; i++)
            if (h.ds[i] && liveDs[i]) remap[h.ds[i]] = liveDs[i];
        rewritten = RemapMemory(remap);
        for (int i = 0; i < 64; i++) {   // the saved process's CRT file table blocks: replaced by the live ones
            void* p = Mem<void*>(g_game->ioinfo + 4 * i);
            if (p && arena::Contains(p)) arena::Free(p);
        }
        Restore(keep);
        RestoreIoInfo(io);
        d3d::ApplyState(remap);
        d3d::Collect();   // the replaced game state's objects, unless another savestate of this process holds them
    }
    if (!fast)
        Log("load slot %d: per-thread data at save %08X %08X %08X %08X, main thread's before the load %p", slot + 1,
            h.ptd[0], h.ptd[1], h.ptd[2], h.ptd[3], TlsGetValue(Mem<DWORD>(g_game->tlsIndex)));
    FixPtds(h, ptds);
    HoldRand() = h.holdrand;
    RestoreProtection(reprotect);
    HeapCheck("after d3d");
    AudioAfterLoad();
    arena::Unlock();
    UnparkAudioThreads();
    CloseHandle(f);
    double t3 = NowMs();

    if (!r.ok) {
        Log("load slot %d: READ FAILED after restoring part of the state", slot + 1);
        msg = "reading the savestate failed: the game state may be corrupt";
        return false;
    }
    if (!gpuErr.empty()) Log("load slot %d: %s", slot + 1, gpuErr.c_str());
    sprintf_s(b, "Savestate %d loaded%s  (%.0f ms)", slot + 1, fast ? "" : " from disk", t3 - t0);
    msg = b;
    unsigned freeMB, largestMB;
    AddressSpace(&freeMB, &largestMB);
    Log("load slot %d (%s): quiesce %.1f ms, memory %.1f ms, gpu+fixups %.1f ms (%u pointers rewritten); game time "
        "%u -> %u; address space free %u MB (largest %u MB)", slot + 1, fast ? "pinned objects" : "re-created objects",
        t1 - t0, t2 - t1, t3 - t2, rewritten, h.gameTimeMs, Mem<uint32_t>(g_game->timerTimeMs), freeMB, largestMB);
    return true;
}

}  // namespace ss
