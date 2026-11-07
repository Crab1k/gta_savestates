// Savestate core for GTA III / Vice City (same file format and design as San Andreas: game sections + used arena
// pages + Direct3D catalog). Saving and loading happen at WinMain's PeekMessageA call, with the CdStream thread idle
// and Miles' service thread locked out (it calls the game's file callbacks while it streams audio).
#include "state3.h"
#include "../src/arena.h"
#include "d3d8.h"
#include "game3.h"
#include "hooks3.h"
#include <emmintrin.h>
#include <cstdio>
#include <vector>

namespace ss {

bool forceRecreate = false;

namespace {

constexpr uint32_t MAGIC = 0x33535353;   // "SSS3"
constexpr uint32_t VERSION = 5;
constexpr uint32_t PAGE = 4096;
constexpr size_t STAGING = 8u << 20;

struct Header {
    uint32_t magic, version;
    uint32_t game, exeStamp;           // Game3; TimeDateStamp of the exe
    uint64_t session;                  // the process that wrote it
    uint32_t dataBegin, dataEnd;
    uint32_t arenaBase, arenaUsedEnd;
    uint32_t pages, storedPages;
    uint32_t gameTimeMs, frameCounter;
    uint32_t bbWidth, bbHeight, bbFormat;
    uint32_t threadData[2];            // runtime per-thread data of the main and CdStream threads
    uint32_t device;                   // IDirect3DDevice8* at save time
    uint32_t extraBytes;               // Miles buffers that follow the pages
    uint64_t gpuOffset, gpuBytes;
    uint32_t complete;
    uint32_t rw[4];                    // RenderWare's device, render target, depth surface, IDirect3D8 at save time
    uint32_t streamBytes;              // StreamRec[] after the Miles buffers
    uint32_t fileCount;                // FileRec[] after the StreamRecs
    uint32_t channelCount;             // ChannelRec[] after the FileRecs
};

// A Miles stream at save time. Stream handles belong to the process: a load re-opens the file at the position.
struct StreamRec { StreamFile file; int32_t posMs, totalMs, status, used; };
// Version 4 (the first GTA III release) is the same file without the stream's game file.
constexpr uint32_t VERSION_NO_STREAM_FILE = 4;
struct StreamRec4 { char name[MAX_PATH]; int32_t volume, pan, posMs, totalMs, status, used; };
constexpr int32_t SMP_PLAYING = 4;

// A sound-effect channel at save time: the audio manager's record of it is in the saved memory; this is what only
// Miles knows. A load starts the same sample again from this point.
struct ChannelRec { uint32_t playing, offset; };

struct Miles {
    void (WINAPI* streamPos)(uintptr_t, int32_t*, int32_t*);
    int32_t (WINAPI* streamStatus)(uintptr_t);
    void (WINAPI* setStreamPos)(uintptr_t, int32_t);
    void (WINAPI* pauseStream)(uintptr_t, int32_t);
    void (WINAPI* loopCount)(uintptr_t, int32_t);
    void (WINAPI* serviceStream)(uintptr_t, int32_t);
    void (WINAPI* end3d)(uintptr_t);
    void (WINAPI* end2d)(uintptr_t);
} g_ail = {};
bool g_ailOk = false;

struct Slot {
    bool used = false;
    d3d::Capture* gpu = nullptr;
    Header hdr = {};
    uint64_t bytes = 0;
    DWORD savedTick = 0;
};

Slot g_slots[MAX_SLOTS];
std::string g_dir;
uint8_t* g_staging = nullptr;
uint64_t g_session = 0;
uint32_t g_exeStamp = 0;
bool g_crossOk = false;   // savestates of other processes (checked at the first save or load)
bool g_validated = false;


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

// ------------------------------------------------------------------ quiet moment
bool CdStreamIdle() {
    uintptr_t streams = Mem<uintptr_t>(g3->cdStreams);
    int count = Mem<int32_t>(g3->cdStreamCount);
    for (int i = 0; i < count; i++) {
        uintptr_t s = streams + i * 0x30;
        if (Mem<uint32_t>(s + 0x04) || Mem<uint8_t>(s + 0x0E)) return false;   // nSectorsToRead, bInUse
    }
    uintptr_t q = g3->cdStreamQueue;
    return Mem<int32_t>(q + 4) == Mem<int32_t>(q + 8);                       // head == tail
}

void (WINAPI* g_ailLock)() = nullptr;
void (WINAPI* g_ailUnlock)() = nullptr;

bool Quiesce(std::string& msg) {
    double until = NowMs() + 5000;
    while (!CdStreamIdle()) {
        if (NowMs() > until) { msg = "streaming I/O did not finish"; return false; }
        RealSleep(1);
    }
    if (g_ailLock) g_ailLock();
    return true;
}

void Unquiesce() {
    if (g_ailUnlock) g_ailUnlock();
}

// ------------------------------------------------------------------ values that belong to the process
struct Keep { uintptr_t addr; uint32_t size; std::vector<uint8_t> data; };

void AddKeep(std::vector<Keep>& k, uintptr_t addr, uint32_t size) {
    if (!addr || !size) return;
    if ((addr >= g3->dataBegin && addr + size <= g3->dataEnd) || arena::Contains((void*)addr)) k.push_back({addr, size, {}});
}

void AddImportSlots(void* ctx, uintptr_t slot, uint32_t bytes) { AddKeep(*(std::vector<Keep>*)ctx, slot, bytes); }

std::vector<Keep> KeepList(bool otherProcess) {
    std::vector<Keep> k;
    for (auto& rg : g3->timerOs) AddKeep(k, rg.addr, rg.size);
    AddKeep(k, Mem<uintptr_t>(g3->rsPs), g3->psSize);   // window, DirectInput
    AddKeep(k, g3->cdStreamGlobals.addr, g3->cdStreamGlobals.size);
    AddKeep(k, g3->tlsIndex, 4);
    AddKeep(k, g3->milesDriver, 4);
    AddKeep(k, g3->milesStream, 4 * g3->streamCount);   // re-opened from the StreamRecs
    AddKeep(k, g3->milesSamples.addr, g3->milesSamples.size);
    AddKeep(k, g3->milesSample2d, 4);
    AddKeep(k, g3->milesProvider, 4);
    for (auto& rg : g3->milesBuffers) AddKeep(k, rg.addr, 4);
    ImportSlots(&AddImportSlots, &k);   // our hooks and this process's DLL addresses
    if (!otherProcess) return k;
    AddKeep(k, g3->rwGlobals, 20);      // window, device, its surfaces, IDirect3D8 (old ones remapped to these)
    AddKeep(k, g3->rwPresent.addr, g3->rwPresent.size);
    return k;
}

void Capture(std::vector<Keep>& k) {
    for (auto& e : k) e.data.assign((uint8_t*)e.addr, (uint8_t*)e.addr + e.size);
}

void Restore(const std::vector<Keep>& k) {
    for (auto& e : k) memcpy((void*)e.addr, e.data.data(), e.size);
}

// The exe declares its data sections writable, but other software can change a page's protection (seen on a player's
// PC: Vice City's _rwcseg at 0x6F3000 not writable, and the load crashed half-way). Such pages are made writable for
// the load and get their protection back afterwards.
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

// Miles buffers whose contents change during play (ped speech): saved with the savestate.
uint32_t ExtraSize(const Range3& r) {
    if (!r.addr) return 0;
    return r.size >= 0x400000 ? Mem<uint32_t>(r.size) : r.size;
}

// The game keeps the address of each loaded ped comment in the ped speech buffer (7 slots). The buffer belongs to the
// process: a savestate of another one names addresses in that process's buffer, whose saved contents now fill this
// one's. A comment that was playing would otherwise be started from freed memory (GTA III lab: Mssfast.m3d crashed
// reading the old buffer right after a load from disk).
void RebasePedSlots(uint32_t savedBuffer) {
    const Range3& b = g3->milesBuffers[1];
    uint32_t live = b.addr ? Mem<uint32_t>(b.addr) : 0, size = ExtraSize(b), moved = 0;
    if (!g3->pedSlotAddrs || !live || !savedBuffer || savedBuffer == live) return;
    for (int i = 0; i < 7; i++) {
        uint32_t& a = Mem<uint32_t>(g3->pedSlotAddrs + 4 * i);
        if (a >= savedBuffer && a < savedBuffer + size) { a = a - savedBuffer + live; moved++; }
    }
    Log("audio: %u ped comment slots moved from the saved speech buffer %08X to this process's %08X", moved,
        savedBuffer, live);
}

// ------------------------------------------------------------------ runtime per-thread data
typedef LONG(NTAPI* NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
struct ThreadBasicInfo { LONG exitStatus; PVOID teb; HANDLE pid, tid; ULONG_PTR affinity; LONG prio, basePrio; };

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

struct LiveTd { void** slot = nullptr; bool main = false; std::vector<uint8_t> data; };

void ThreadData(uint32_t out[2], LiveTd live[2]) {
    DWORD index = Mem<DWORD>(g3->tlsIndex);
    DWORD tids[2] = {GetCurrentThreadId(), Mem<DWORD>(g3->cdStreamThreadId)};
    for (int i = 0; i < 2; i++) {
        void** slot = i == 0 ? nullptr : TlsSlot(tids[i], index);
        void* p = i == 0 ? TlsGetValue(index) : slot ? *slot : nullptr;
        if (out) out[i] = (uint32_t)(uintptr_t)p;
        if (live) {
            live[i].slot = slot;
            live[i].main = i == 0;
            if (p && arena::Contains(p)) live[i].data.assign((uint8_t*)p, (uint8_t*)p + g3->threadDataSize);
        }
    }
}

// The restored heap holds the per-thread blocks of the threads that existed at save time (linked in the runtime's
// list), but each running thread's TLS slot points at its own block from before the load. Free the saved blocks and
// give every running thread a new block (allocated by the runtime, linked into its list) with its own values.
void FixThreadData(LiveTd live[2]) {
    auto rtMalloc = (void*(__cdecl*)(uint32_t))g3->mallocFn;
    auto rtFree = (void(__cdecl*)(void*))g3->freeFn;
    uintptr_t head = Mem<uintptr_t>(g3->threadDataList);
    int freed = 0;
    while (head && arena::Contains((void*)head) && freed < 64) {
        uintptr_t next = Mem<uintptr_t>(head);
        rtFree((void*)head);
        head = next;
        freed++;
    }
    Mem<uintptr_t>(g3->threadDataList) = 0;
    DWORD index = Mem<DWORD>(g3->tlsIndex);
    for (int i = 0; i < 2; i++) {
        if (live[i].data.empty()) continue;
        auto* b = (uint8_t*)rtMalloc(g3->threadDataSize);
        if (!b) continue;
        memcpy(b, live[i].data.data(), g3->threadDataSize);
        Mem<uintptr_t>((uintptr_t)b) = Mem<uintptr_t>(g3->threadDataList);   // list link
        Mem<uintptr_t>(g3->threadDataList) = (uintptr_t)b;
        if (live[i].main) TlsSetValue(index, b);
        else if (live[i].slot) *live[i].slot = b;
    }
}

// ------------------------------------------------------------------ runtime file handles
// The runtime's file table holds a record per open file with the OS handle, and the runtime reads on from the OS
// file position. The restored records hold the handles of the moment (or the process) that saved: give each the
// running process's handle of the same file, or open the file again, at the position it had at save time.
struct FileRec { int32_t index; int32_t disk; int64_t pos; char path[MAX_PATH]; };

HANDLE OsHandle(int32_t index) {
    if (!g3->mslHandles || index < 0 || index > 255) return nullptr;
    uintptr_t rec = Mem<uintptr_t>(g3->mslHandles + 4 * index);
    return rec ? Mem<HANDLE>(rec) : nullptr;
}

bool PathOf(HANDLE h, char* path) {
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return false;
    DWORD n = GetFinalPathNameByHandleA(h, path, MAX_PATH, 0);
    return n && n < MAX_PATH;
}

std::vector<FileRec> SaveFiles() {
    std::vector<FileRec> v;
    for (int i = 0; i < 256; i++) {
        HANDLE h = OsHandle(i);
        if (!h) continue;
        FileRec f = {};
        f.index = i;
        f.disk = PathOf(h, f.path);
        LARGE_INTEGER zero = {}, cur = {};
        if (f.disk && SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) f.pos = cur.QuadPart;
        v.push_back(f);
    }
    return v;
}

void CaptureFileHandles(HANDLE live[256]) {
    for (int i = 0; i < 256; i++) live[i] = OsHandle(i);
}

void RestoreFiles(const std::vector<FileRec>& saved, const HANDLE live[256]) {
    bool used[256] = {};
    int rehomed = 0, reopened = 0, failed = 0, closed = 0;
    for (const FileRec& f : saved) {
        uintptr_t rec = Mem<uintptr_t>(g3->mslHandles + 4 * f.index);
        if (!rec || !(arena::Contains((void*)rec) || (rec >= g3->dataBegin && rec < g3->dataEnd))) continue;
        used[f.index] = true;
        HANDLE h = live[f.index];
        if (!f.disk) {   // console / device: whatever this process has
            if (h) Mem<HANDLE>(rec) = h;
            continue;
        }
        char path[MAX_PATH];
        if (h && !(PathOf(h, path) && !_stricmp(path, f.path))) h = nullptr;   // closed since, or another file now
        if (h) rehomed++;
        else {
            if (live[f.index] && live[f.index] != INVALID_HANDLE_VALUE) { CloseHandle(live[f.index]); closed++; }
            h = CreateFileA(f.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { failed++; Log("file %s: cannot open it again", f.path); }
            else reopened++;
        }
        Mem<HANDLE>(rec) = h;
        LARGE_INTEGER pos;
        pos.QuadPart = f.pos;
        if (h != INVALID_HANDLE_VALUE) SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    }
    // files this process has open that the restored runtime does not know: nobody would close them
    for (int i = 0; i < 256; i++) {
        char path[MAX_PATH];
        if (!used[i] && PathOf(live[i], path)) { CloseHandle(live[i]); closed++; }
    }
    Log("files: %d kept, %d opened again, %d closed, %d failed", rehomed, reopened, closed, failed);
}

// ------------------------------------------------------------------ pointer rewriting
// A savestate of another process names its graphics objects by their old addresses, and the restored memory gets the
// re-created objects in their place: every word that equals an old address, except in script space. Only the value
// marks a pointer, and game data can equal an old address by chance; in script code such a change made a script run
// garbage (a player's PC crashed 10 s after a load), and script space never holds these pointers. Elsewhere every
// match is changed: rules for where RenderWare keeps them (raster extensions, mesh instance data) missed real ones
// in a savestate of the user's (arrays of index buffers and textures in RenderWare pools), whose stale pointers then
// crashed the texture code; a chance match outside scripts only changes a pixel or vertex value.
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
    uintptr_t ss = g3->scriptSpace, ssEnd = ss + g3->scriptScratch + 0x100;
    RemapRange(*r, (uint32_t*)g3->dataBegin, (uint32_t*)ss);
    RemapRange(*r, (uint32_t*)ssEnd, (uint32_t*)g3->dataEnd);
    arena::ForEachUsed(&RemapBlock, r);
    d3d::Remap::const_iterator it;
    for (auto* p = (const uint32_t*)ss; p < (const uint32_t*)ssEnd; p++) r->scriptMatches += Match(*r, *p, it);
    if (r->scriptMatches)
        Log("load: %u words of script space equal to an old graphics object's address left as they are", r->scriptMatches);
    uint32_t n = r->replaced;
    delete r;
    return n;
}

// ------------------------------------------------------------------ audio
int32_t (WINAPI* g_status3d)(uintptr_t) = nullptr;
int32_t (WINAPI* g_status2d)(uintptr_t) = nullptr;
uint32_t (WINAPI* g_offset3d)(uintptr_t) = nullptr;
void (WINAPI* g_setOffset3d)(uintptr_t, uint32_t) = nullptr;
uint32_t (WINAPI* g_position2d)(uintptr_t) = nullptr;
void (WINAPI* g_setPosition2d)(uintptr_t, uint32_t) = nullptr;

// Channel i of the audio manager: a 3D sample, or the 2D one after them.
uintptr_t ChannelSample(uint32_t i, bool* is2d) {
    uint32_t n3d = g3->milesSamples.size / 4;
    *is2d = i >= n3d;
    if (i < n3d) return Mem<uintptr_t>(g3->milesSamples.addr + 4 * i);
    return i == n3d && g3->milesSample2d ? Mem<uintptr_t>(g3->milesSample2d) : 0;
}

std::vector<ChannelRec> SaveChannels() {
    std::vector<ChannelRec> v;
    if (!g3->audioActive || !g_status3d || !g_offset3d) return v;
    uint32_t n = Mem<uint8_t>(g3->audioActiveCount);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        ChannelRec c = {};
        bool is2d;
        uintptr_t s = ChannelSample(i, &is2d);
        if (s && !is2d && g_status3d(s) == SMP_PLAYING) { c.playing = 1; c.offset = g_offset3d(s); }
        if (s && is2d && g_status2d && g_status2d(s) == SMP_PLAYING) { c.playing = 1; c.offset = g_position2d(s); }
        v.push_back(c);
    }
    return v;
}

// Starts channel i again from its restored record, as ProcessActiveQueues starts a sound, then seeks.
bool RestartChannel(uint32_t i, uintptr_t rec, uint32_t offset) {
    auto& f = g3->sfx;
    if (!f.init) return false;
    void* sm = (void*)f.sampleManager;
    auto init = (uint8_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t))f.init;
    auto int1 = [&](uintptr_t fn, uint32_t a) { ((void(__fastcall*)(void*, void*, uint32_t, uint32_t))fn)(sm, nullptr, i, a); };
    if (!init(sm, nullptr, i, Mem<uint32_t>(rec + 8), Mem<uint8_t>(rec + 0xC))) return false;
    int1(f.frequency, Mem<uint32_t>(rec + 0x14));
    uint32_t vol = Mem<uint8_t>(rec + 0x2C);
    if (Mem<uint8_t>(f.audioManager + 4)) vol = (vol < 0x3F ? vol : 0x3F) * 2;
    if (f.masterVolume) {   // Vice City scales by the effects volume as ProcessActiveQueues does
        uint32_t master = Mem<uint8_t>(f.audioManager + f.masterVolume);
        bool flagged = Mem<uint8_t>(f.audioManager + f.masterFlags) || Mem<uint8_t>(f.audioManager + f.masterFlags + 1);
        if ((flagged && !Mem<uint8_t>(rec + 0xD)) || master < 0x7F) vol = master * vol / 0x7F;
    }
    int1(f.emittingVolume, vol);
    ((void(__fastcall*)(void*, void*, uint32_t, int32_t, int32_t))f.loopPoints)(sm, nullptr, i, Mem<int32_t>(rec + 0x24),
                                                                                Mem<int32_t>(rec + 0x28));
    int1(f.loopCount, Mem<uint32_t>(rec + 0x20));
    int1(f.reverb, Mem<uint8_t>(rec + 0x48));
    float pos[3], intensity = Mem<float>(rec + 0x34);
    if (!Mem<uint8_t>(rec + 0xD)) {
        ((void(__fastcall*)(void*, void*, void*, float*))f.translate)((void*)f.audioManager, nullptr, (void*)(rec + 0x3C), pos);
    } else {   // 2D: pan
        uint32_t pan = Mem<uint8_t>(rec + 0x4B);
        float step = Mem<float>(f.panStep);
        pos[0] = pan == 0x3F ? Mem<float>(f.pos2d) : pan < 0x3F ? -(float)(0x3F - pan) * step : (float)(pan - 0x3F) * step;
        pos[1] = pos[2] = Mem<float>(f.pos2d);
        intensity = 100000.0f;
    }
    ((void(__fastcall*)(void*, void*, uint32_t, float, float, float))f.position)(sm, nullptr, i, pos[0], pos[1], pos[2]);
    ((void(__fastcall*)(void*, void*, uint32_t, float, float))f.distances)(sm, nullptr, i, intensity,
                                                                          Mem<float>(f.minDistFactor) * intensity);
    bool is2d;
    uintptr_t s = ChannelSample(i, &is2d);
    if (s && !is2d && g_setOffset3d) g_setOffset3d(s, offset);
    if (s && is2d && g_setPosition2d) g_setPosition2d(s, offset);
    ((void(__fastcall*)(void*, void*, uint32_t))f.start)(sm, nullptr, i);
    if (s && !is2d && g_setOffset3d) g_setOffset3d(s, offset);   // in case starting rewound it
    if (s && is2d && g_setPosition2d) g_setPosition2d(s, offset);
    return true;
}

void SaveStreams(StreamRec* recs) {
    for (uint32_t i = 0; i < g3->streamCount; i++) {
        StreamRec& r = recs[i];
        memset(&r, 0, sizeof(r));
        uintptr_t s = Mem<uintptr_t>(g3->milesStream + 4 * i);
        if (!s || !g_ailOk || !StreamFileOf(s, r.file)) continue;
        g_ail.streamPos(s, &r.totalMs, &r.posMs);
        r.status = g_ail.streamStatus(s);
        r.used = 1;
    }
}

// The running game's streams are closed before its memory is replaced: their files may be the game's own (Vice City:
// MSL FILEs in the game heap), which the restored memory would no longer have.
void CloseLiveStreams() {
    if (!g_ailOk) return;
    for (uint32_t i = 0; i < g3->streamCount; i++) {
        uintptr_t& slot = Mem<uintptr_t>(g3->milesStream + 4 * i);
        if (!slot) continue;
        g_ail.pauseStream(slot, 1);
        CloseStream(slot);
        slot = 0;
    }
}

// After a load the Miles channels still play what the running process played: stop them (the restored game state
// starts its sounds again) and re-open the streams the savestate had, at their positions.
void RestoreAudio(const StreamRec* recs, uint32_t n, const std::vector<ChannelRec>& channels) {
    if (!g_ailOk) return;
    for (uint32_t i = 0; i < g3->milesSamples.size / 4; i++)
        if (uintptr_t s = Mem<uintptr_t>(g3->milesSamples.addr + 4 * i)) g_ail.end3d(s);
    if (uintptr_t s = g3->milesSample2d ? Mem<uintptr_t>(g3->milesSample2d) : 0) g_ail.end2d(s);
    // The restored audio manager has its sounds as playing in those channels and only keeps updating a playing
    // sound: start every channel that was playing at save time again, at the same point of its sample. A channel
    // that cannot be (or was not playing) is marked empty, so the manager starts whatever it wants there.
    if (g3->audioActive) {
        uint32_t count = Mem<uint8_t>(g3->audioActiveCount), resumed = 0, cleared = 0, atPlace = 0;
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uintptr_t e = g3->audioActive + i * g3->audioActiveStride;
            bool wanted = Mem<uint32_t>(e + 8) != g3->noSample && i < channels.size() && channels[i].playing;
            if (wanted && RestartChannel(i, e, channels[i].offset)) {
                resumed++;
                bool is2d;
                uintptr_t s = ChannelSample(i, &is2d);
                uint32_t now = !s ? 0 : is2d ? (g_position2d ? g_position2d(s) : 0) : (g_offset3d ? g_offset3d(s) : 0);
                if (now + 8192 >= channels[i].offset && now <= channels[i].offset + 8192) atPlace++;
                continue;
            }
            if (Mem<uint32_t>(e + 8) != g3->noSample) cleared++;
            Mem<int32_t>(e) = -5;
            Mem<uint32_t>(e + 8) = g3->noSample;
        }
        Log("audio: %u sound effects resumed (%u at their saved position), %u channels cleared", resumed, atPlace, cleared);
    }
    uintptr_t dig = Mem<uintptr_t>(g3->milesDriver);
    for (uint32_t i = 0; i < g3->streamCount; i++) {
        uintptr_t& slot = Mem<uintptr_t>(g3->milesStream + 4 * i);
        if (slot) {
            g_ail.pauseStream(slot, 1);
            CloseStream(slot);
            slot = 0;
        }
        if (i >= n || !recs[i].used) continue;
        const StreamRec& r = recs[i];
        CloseGameFile(r.file.file);   // the saved stream's file, in the restored memory (opened again by RestoreFiles)
        uintptr_t s = OpenStream(dig, r.file);
        if (!s) { Log("audio: cannot re-open stream %s", r.file.name); continue; }
        g_ail.loopCount(s, 1);
        g_ail.setStreamPos(s, r.posMs);
        Log("audio: stream %u %s at %d of %d ms (status %d)", i, r.file.name, r.posMs, r.totalMs, r.status);
        // Only a playing stream is started. The rest are buffered the way the game preloads a line: a mission's
        // dialogue loaded and waiting to be played reports SMP_DONE at 0 like a finished one, and the game starts it
        // later with AIL_start_stream (lab: the whole line plays after a load either way; started from its end it
        // only showed as playing for a moment).
        if (r.status == SMP_PLAYING) g_ail.pauseStream(s, 0);
        else g_ail.serviceStream(s, 1);
        slot = s;
    }
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
    IDirect3DSurface8* bb = nullptr;
    auto* dev = d3d::Device();
    if (!dev || FAILED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC8 d;
    bb->GetDesc(&d);
    ((IUnknown*)bb)->Release();
    *w = d.Width; *h = d.Height; *fmt = (uint32_t)d.Format;
    return true;
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
    if (h.version != VERSION && h.version != VERSION_NO_STREAM_FILE) return "it was made by another version of the tool";
    if (!h.complete) return "the file is incomplete";
    if (h.game != (uint32_t)g3->id || h.exeStamp != g_exeStamp) return "it was made with another game exe";
    if (h.dataBegin != g3->dataBegin || h.dataEnd != g3->dataEnd) return "it was made with another game exe";
    return nullptr;
}

}  // namespace

bool Init(const char* folder) {
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

void LateInit() {
    HMODULE mss = GetModuleHandleA("mss32.dll");
    if (mss) {
        g_ailLock = (void(WINAPI*)())GetProcAddress(mss, "_AIL_lock@0");
        g_ailUnlock = (void(WINAPI*)())GetProcAddress(mss, "_AIL_unlock@0");
    }
    Log("Miles: AIL_lock %p, AIL_unlock %p", g_ailLock, g_ailUnlock);
    if (!mss) return;
    auto fn = [&](const char* name) { return (void*)GetProcAddress(mss, name); };
    void* f[] = {fn("_AIL_stream_ms_position@12"), fn("_AIL_stream_status@4"), fn("_AIL_set_stream_ms_position@8"),
                 fn("_AIL_pause_stream@8"), fn("_AIL_set_stream_loop_count@8"), fn("_AIL_service_stream@8"),
                 fn("_AIL_end_3D_sample@4"), fn("_AIL_end_sample@4")};
    g_status3d = (int32_t(WINAPI*)(uintptr_t))fn("_AIL_3D_sample_status@4");
    g_status2d = (int32_t(WINAPI*)(uintptr_t))fn("_AIL_sample_status@4");
    g_offset3d = (uint32_t(WINAPI*)(uintptr_t))fn("_AIL_3D_sample_offset@4");
    g_setOffset3d = (void(WINAPI*)(uintptr_t, uint32_t))fn("_AIL_set_3D_sample_offset@8");
    g_position2d = (uint32_t(WINAPI*)(uintptr_t))fn("_AIL_sample_position@4");
    g_setPosition2d = (void(WINAPI*)(uintptr_t, uint32_t))fn("_AIL_set_sample_position@8");
    g_ailOk = true;
    for (void* p : f) g_ailOk = g_ailOk && p;
    if (g_ailOk) memcpy(&g_ail, f, sizeof(f));
    else Log("Miles: stream functions missing, audio is not re-synchronised after loads");
}

// Checks that need the running game (its window, device, open files): done once, at the first save or load.
void ValidateOnce() {
    if (g_validated) return;
    g_validated = true;
    std::string why;
    DWORD index = Mem<DWORD>(g3->tlsIndex);
    void** own = TlsSlot(GetCurrentThreadId(), index);
    if (!own || *own != TlsGetValue(index)) why = "cannot reach other threads' TLS";
    else if (!g3->rwGlobals || Mem<void*>(g3->rwGlobals + 4) != (void*)d3d::Device()) why = "RenderWare's device is not where expected";
    else if (!Mem<HWND>(g3->rwGlobals) || Mem<HWND>(g3->rwGlobals) != Mem<HWND>(g3->rwPresent.addr + 0x18)) why = "RenderWare's window is not where expected";
    // the runtime's handle table: the sound effects file (open all the time) must be found through it
    HANDLE sfx = OsHandle(g3->sfxFile ? Mem<int32_t>(Mem<uintptr_t>(g3->sfxFile)) : -1);
    char path[MAX_PATH] = "";
    DWORD n = sfx ? GetFinalPathNameByHandleA(sfx, path, MAX_PATH, 0) : 0;
    if (why.empty() && !(n >= 7 && n < MAX_PATH && !_stricmp(path + n - 7, "SFX.RAW"))) why = "the runtime's file table is not where expected";
    g_crossOk = why.empty();
    Log("savestates of other sessions: %s%s", g_crossOk ? "supported" : "DISABLED: ", why.c_str());
}

bool CanOperate(std::string& why) {
    if (!g_heapHooked) { why = "the game's heap could not be hooked (see savestate.log)"; return false; }
    if (g_arenaOverflow) { why = "game heap overflowed the savestate arena (raise ArenaMB)"; return false; }
    if (Mem<int32_t>(g3->gGameState) != g3->gsPlaying) { why = "not in game"; return false; }
    if (Mem<uint8_t>(g3->menuActive)) { why = "menu is open"; return false; }
    if (!Mem<void*>(g3->players)) { why = "no player"; return false; }
    if (!d3d::DeviceUsable()) { why = "graphics device not ready"; return false; }
    ValidateOnce();
    return true;
}

int PlayingChannels() {
    int n = 0;
    if (!g_status3d) return -1;
    for (uint32_t i = 0; i < g3->milesSamples.size / 4; i++)
        if (uintptr_t s = Mem<uintptr_t>(g3->milesSamples.addr + 4 * i)) n += g_status3d(s) == SMP_PLAYING;
    return n;
}

std::string StreamStates() {
    std::string s;
    for (uint32_t i = 0; i < g3->streamCount; i++) {
        uintptr_t h = Mem<uintptr_t>(g3->milesStream + 4 * i);
        StreamFile f = {};
        int32_t total = 0, pos = 0;
        char b[MAX_PATH + 64];
        if (!h || !g_ailOk) { sprintf_s(b, "%s%u: -", i ? ", " : "", i); s += b; continue; }
        StreamFileOf(h, f);
        g_ail.streamPos(h, &total, &pos);
        sprintf_s(b, "%s%u: %s %d/%d ms status %d", i ? ", " : "", i, f.name, pos, total, g_ail.streamStatus(h));
        s += b;
    }
    return s;
}

// Channels the audio manager has as playing a loop while Miles has them silent (lost engines, waves...).
int SilentLoops() {
    if (!g_status3d || !g3->audioActive) return -1;
    int n = 0;
    uint32_t count = Mem<uint8_t>(g3->audioActiveCount);
    for (uint32_t i = 0; i < count && i < g3->milesSamples.size / 4; i++) {
        uintptr_t e = g3->audioActive + i * g3->audioActiveStride;
        uintptr_t s = Mem<uintptr_t>(g3->milesSamples.addr + 4 * i);
        if (Mem<uint32_t>(e + 8) != g3->noSample && Mem<int32_t>(e + 0x20) == 0 && s && g_status3d(s) != SMP_PLAYING) n++;
    }
    return n;
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

void Tick() {}

bool Save(int slot, std::string& msg) {
    if (slot < 0 || slot >= MAX_SLOTS) return false;
    if (!CanOperate(msg)) return false;
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
    h.game = (uint32_t)g3->id;
    h.exeStamp = g_exeStamp;
    h.session = g_session;
    h.dataBegin = (uint32_t)g3->dataBegin;
    h.dataEnd = (uint32_t)g3->dataEnd;
    h.arenaBase = (uint32_t)(uintptr_t)arena::Base();
    h.arenaUsedEnd = (uint32_t)arena::UsedEnd();
    h.pages = (h.arenaUsedEnd + PAGE - 1) / PAGE;
    h.gameTimeMs = Mem<uint32_t>(g3->timerMs);
    h.frameCounter = Mem<uint32_t>(g3->timerFrame);
    BackBuffer(&h.bbWidth, &h.bbHeight, &h.bbFormat);
    ThreadData(h.threadData, nullptr);
    h.device = (uint32_t)(uintptr_t)d3d::Device();
    for (int i = 0; i < 4; i++) h.rw[i] = Mem<uint32_t>(g3->rwGlobals + 4 + 4 * i);
    for (auto& r : g3->milesBuffers) h.extraBytes += ExtraSize(r);
    std::vector<StreamRec> streams(g3->streamCount);
    SaveStreams(streams.data());
    h.streamBytes = (uint32_t)(streams.size() * sizeof(StreamRec));
    std::vector<FileRec> files = SaveFiles();
    h.fileCount = (uint32_t)files.size();
    std::vector<ChannelRec> channels = SaveChannels();
    h.channelCount = (uint32_t)channels.size();

    std::vector<uint8_t> bits((h.pages + 7) / 8, 0);
    const uint8_t* base = arena::Base();
    for (uint32_t p = 0; p < h.pages; p++)
        if (!PageZero(base + (size_t)p * PAGE)) { bits[p >> 3] |= (uint8_t)(1u << (p & 7)); h.storedPages++; }

    Writer w{f};
    w.Put(&h, sizeof(h));
    w.Put((const void*)g3->dataBegin, g3->dataEnd - g3->dataBegin);
    w.Put(bits.data(), bits.size());
    for (uint32_t p = 0; p < h.pages && w.ok; p++)
        if (bits[p >> 3] & (1u << (p & 7))) w.Put(base + (size_t)p * PAGE, PAGE);
    for (auto& r : g3->milesBuffers)
        if (uint32_t n = ExtraSize(r)) w.Put(Mem<void*>(r.addr), n);
    w.Put(streams.data(), h.streamBytes);
    w.Put(files.data(), files.size() * sizeof(FileRec));
    w.Put(channels.data(), channels.size() * sizeof(ChannelRec));
    double t2 = NowMs();

    ReleaseSlot(s);
    s.gpu = d3d::Save();
    arena::Unlock();
    Unquiesce();
    double t3 = NowMs();

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
        s.used = ReadHeader(path, s.hdr) && !Incompatible(s.hdr);
        msg = "writing the savestate failed (disk full?)";
        Log("save slot %d: write failed (error %u)", slot + 1, (unsigned)GetLastError());
        return false;
    }
    s.used = true;
    s.hdr = h;
    s.bytes = w.total;
    s.savedTick = GetTickCount();
    double t4 = NowMs();
    char b[160];
    sprintf_s(b, "Savestate %d saved  (%u MB, %.0f ms)", slot + 1, (unsigned)(w.total >> 20), t4 - t0);
    msg = b;
    Log("save slot %d: %u MB (gpu %.1f MB), arena %u MB used (%u/%u pages stored), %u gpu objects | quiesce %.1f ms, "
        "memory %.1f ms, pin %.1f ms, gpu write %.1f ms, game time %u", slot + 1, (unsigned)(w.total >> 20),
        h.gpuBytes / 1048576.0, h.arenaUsedEnd >> 20, h.storedPages, h.pages, (unsigned)s.gpu->refs.size(), t1 - t0,
        t2 - t1, t3 - t2, t4 - t3, h.gameTimeMs);
    return true;
}

bool Load(int slot, std::string& msg) {
    if (slot < 0 || slot >= MAX_SLOTS) return false;
    Slot& s = g_slots[slot];
    char b[200];
    if (!s.used) { sprintf_s(b, "Savestate %d is empty", slot + 1); msg = b; return false; }
    if (!CanOperate(msg)) return false;
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
        if (!g_crossOk) why = "loading savestates of an earlier session is not supported yet for this game";
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
    double t0 = NowMs();
    if (!Quiesce(msg)) { CloseHandle(f); return false; }
    arena::Lock();
    double t1 = NowMs();
    if (!arena::CommitAtLeast((size_t)h.pages * PAGE)) {
        arena::Unlock();
        Unquiesce();
        CloseHandle(f);
        msg = "not enough memory for the savestate's heap";
        return false;
    }

    // Everything below must not fail half-way.
    const size_t livePages = (arena::UsedEnd() + PAGE - 1) / PAGE;
    CloseLiveStreams();   // before the slots are captured: they come back empty
    std::vector<Keep> keep = KeepList(!fast);
    Capture(keep);
    LiveTd td[2];
    ThreadData(nullptr, td);
    uint32_t liveRw[4];
    memcpy(liveRw, (void*)(g3->rwGlobals + 4), sizeof(liveRw));
    HANDLE liveFiles[256];
    CaptureFileHandles(liveFiles);

    std::vector<Protection> reprotect = MakeWritable(g3->dataBegin, g3->dataEnd);
    r.Get((void*)g3->dataBegin, g3->dataEnd - g3->dataBegin);
    const uint32_t savedPedBuffer = g3->milesBuffers[1].addr ? Mem<uint32_t>(g3->milesBuffers[1].addr) : 0;
    std::vector<uint8_t> bits((h.pages + 7) / 8);
    r.Get(bits.data(), bits.size());
    uint8_t* base = arena::Base();
    const size_t maxPages = livePages > h.pages ? livePages : h.pages;
    for (size_t p = 0; p < maxPages && r.ok; p++) {
        uint8_t* dst = base + p * PAGE;
        if (p < h.pages && (bits[p >> 3] & (1u << (p & 7)))) r.Get(dst, PAGE);
        else if (!PageZero(dst)) memset(dst, 0, PAGE);
    }
    // Miles buffers (their pointers are kept live, so they are the running process's buffers)
    Restore(keep);
    RebasePedSlots(savedPedBuffer);
    for (auto& rg : g3->milesBuffers) {
        uint32_t n = ExtraSize(rg);
        if (!n) continue;
        if (Mem<void*>(rg.addr)) r.Get(Mem<void*>(rg.addr), n);
        else { std::vector<uint8_t> skip(n); r.Get(skip.data(), n); }
    }
    std::vector<StreamRec> streams;
    if (h.version == VERSION_NO_STREAM_FILE) {
        std::vector<StreamRec4> old(h.streamBytes / sizeof(StreamRec4));
        r.Get(old.data(), old.size() * sizeof(StreamRec4));
        for (const StreamRec4& o : old) {
            StreamRec s = {};
            memcpy(s.file.name, o.name, sizeof(o.name));
            s.file.volume = o.volume;
            s.file.pan = o.pan;
            s.posMs = o.posMs;
            s.totalMs = o.totalMs;
            s.status = o.status;
            s.used = o.used;
            streams.push_back(s);
        }
    } else {
        streams.resize(h.streamBytes / sizeof(StreamRec));
        r.Get(streams.data(), streams.size() * sizeof(StreamRec));
    }
    std::vector<FileRec> files(h.fileCount <= 256 ? h.fileCount : 0);
    r.Get(files.data(), files.size() * sizeof(FileRec));
    std::vector<ChannelRec> channels(h.channelCount <= 64 ? h.channelCount : 0);
    r.Get(channels.data(), channels.size() * sizeof(ChannelRec));
    double t2 = NowMs();

    std::string gpuErr;
    d3d::Remap remap;
    uint32_t rewritten = 0;
    if (fast) {
        d3d::Load(s.gpu);
        d3d::RestoreDynamic(s.gpu, f, h.gpuOffset);
    } else {
        d3d::Recreate(r, remap, gpuErr);
        for (int i = 0; i < 4; i++)   // RenderWare's copies of the device, its implicit surfaces and IDirect3D8
            if (h.rw[i] && liveRw[i]) remap[h.rw[i]] = liveRw[i];
        rewritten = RemapMemory(remap);
        Restore(keep);   // a live value can equal an old object's address
        d3d::ApplyState(remap);
        d3d::Collect();   // the replaced game state's objects, unless another savestate of this process holds them
    }
    RestoreFiles(files, liveFiles);
    FixThreadData(td);
    RestoreProtection(reprotect);
    arena::Unlock();
    Unquiesce();
    RestoreAudio(streams.data(), (uint32_t)streams.size(), channels);
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
    Log("load slot %d (%s): quiesce %.1f ms, memory %.1f ms, gpu+fixups %.1f ms (%u pointers rewritten); game time "
        "%u -> %u", slot + 1, fast ? "pinned objects" : "re-created objects", t1 - t0, t2 - t1, t3 - t2, rewritten,
        h.gameTimeMs, Mem<uint32_t>(g3->timerMs));
    return true;
}

}  // namespace ss
