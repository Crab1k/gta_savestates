// Lab diagnostic: what the savestate image (game data sections + allocated arena blocks) points at outside itself.
// A savestate loaded in another process must re-create or re-bind every one of these targets.
#include "census3.h"
#include "../src/arena.h"
#include "../src/common.h"
#include "d3d8.h"
#include "game3.h"
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace census {
namespace {

enum : uint8_t { C_UNKNOWN, C_NONE, C_PRIVATE, C_MAPPED, C_IMAGE, C_CODE };

std::vector<uint8_t> g_cls;   // per 4 KB page
uintptr_t g_imgLo, g_imgHi;
struct Range { uintptr_t lo, hi; int heap; };
std::vector<Range> g_heaps;
std::unordered_map<uintptr_t, d3d::Kind> g_tracked;

uint8_t Classify(uintptr_t v) {
    if (v < 0x10000 || v >= 0x80000000u) return C_NONE;
    uint8_t& c = g_cls[v >> 12];
    if (c != C_UNKNOWN) return c;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t page = v & ~(uintptr_t)0xFFF;
    uint8_t r = C_NONE;
    size_t len = 0x1000;
    if (VirtualQuery((void*)page, &mbi, sizeof(mbi))) {
        len = mbi.RegionSize - (page - (uintptr_t)mbi.BaseAddress);
        bool exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            !(page >= g_imgLo && page < g_imgHi) && !arena::Contains((void*)page))
            r = mbi.Type == MEM_IMAGE ? (exec ? C_CODE : C_IMAGE) : mbi.Type == MEM_MAPPED ? C_MAPPED : C_PRIVATE;
    }
    for (size_t i = 0; i < len >> 12 && ((page >> 12) + i) < g_cls.size(); i++) g_cls[(page >> 12) + i] = r;
    return r;
}

std::string ModuleOf(uintptr_t v, bool withOffset = true) {
    MEMORY_BASIC_INFORMATION mbi;
    char name[MAX_PATH] = "?";
    if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) return "?";
    GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, MAX_PATH);
    const char* b = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    bool sys = strstr(name, "\\SysWOW64\\") || strstr(name, "\\System32\\") || strstr(name, "\\system32\\");
    char out[MAX_PATH + 24];
    if (withOffset) sprintf_s(out, "%s%s+%X", sys ? "sys:" : "", b, (unsigned)(v - (uintptr_t)mbi.AllocationBase));
    else sprintf_s(out, "%s%s", sys ? "sys:" : "", b);
    return out;
}

int HeapOf(uintptr_t v) {
    for (auto& r : g_heaps)
        if (v >= r.lo && v < r.hi) return r.heap;
    return -1;
}

bool IsCom(uintptr_t v) {
    if (v & 3) return false;
    uint8_t c = Classify(v);
    if (c != C_PRIVATE && c != C_MAPPED) return false;
    uintptr_t vt = *(uintptr_t*)v;
    uint8_t cv = Classify(vt);
    if ((cv != C_IMAGE && cv != C_CODE) || (vt & 3)) return false;
    for (int i = 0; i < 3; i++)
        if (Classify(((uintptr_t*)vt)[i]) != C_CODE) return false;
    return true;
}

std::unordered_map<uintptr_t, std::string> g_exports;

// Empty string: not interesting.
std::string Describe(uintptr_t v) {
    auto ex = g_exports.find(v);
    if (ex != g_exports.end()) return "export " + ex->second;
    auto it = g_tracked.find(v);
    if (it != g_tracked.end()) return std::string("d3d ") + d3d::KindName(it->second);
    if (v == (uintptr_t)d3d::Device()) return "d3d device";
    uint8_t c = Classify(v);
    if (c == C_NONE) return "";
    if (IsCom(v)) return "com " + ModuleOf(*(uintptr_t*)v);
    if (c == C_CODE) return "code " + ModuleOf(v);
    int h = HeapOf(v);
    if (h >= 0 && !(v & 7)) return "heap#" + std::to_string(h);
    return "";
}

// Exported functions of every loaded module: an exact match in game memory is a stored function pointer, which
// would be stale after a reboot (system DLLs move) unless it is kept live or re-resolved.
void AddExport(uintptr_t addr, const char* module, const char* name) {
    g_exports.emplace(addr, std::string(module) + "!" + name);
}

// Images mapped as data (resources) may have unreadable directories: SEH, so no C++ objects in here.
void ParseExports(uint8_t* base, const char* module) {
    __try {
        if (((IMAGE_DOS_HEADER*)base)->e_magic != IMAGE_DOS_SIGNATURE) return;
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return;
        auto* ex = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
        auto* funcs = (DWORD*)(base + ex->AddressOfFunctions);
        auto* names = (DWORD*)(base + ex->AddressOfNames);
        auto* ords = (WORD*)(base + ex->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            DWORD rva = funcs[ords[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;   // forwarder
            AddExport((uintptr_t)base + rva, module, (const char*)(base + names[i]));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CollectExports() {
    g_exports.clear();
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x80000000u && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a += mbi.RegionSize) {
        if (mbi.Type != MEM_IMAGE || mbi.AllocationBase != mbi.BaseAddress) continue;
        if ((HMODULE)mbi.AllocationBase == GetModuleHandleA(nullptr)) continue;
        char mod[MAX_PATH] = "?";
        if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, MAX_PATH)) continue;   // not a loaded module
        ParseExports((uint8_t*)mbi.AllocationBase, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod);
    }
}

struct Agg { int n = 0; std::vector<std::string> eg; };
std::map<std::string, Agg> g_agg;

void ScanBlock(void*, uint8_t* p, uint32_t bytes) {
    for (uint32_t off = 0; off + 4 <= bytes; off += 4) {
        uintptr_t v = *(uintptr_t*)(p + off);
        std::string d = Describe(v);
        if (d.empty()) continue;
        if (d.rfind("code ", 0) == 0) d = d.substr(0, d.find('+'));
        Agg& a = g_agg[d];
        a.n++;
        if (a.eg.size() < 3) {
            char e[48];
            sprintf_s(e, " %08X(+%X/%X)", (unsigned)(uintptr_t)(p + off), off, bytes);
            a.eg.push_back(e);
        }
    }
}

}  // namespace

void Run(const char* tag) {
    double t0 = (double)GetTickCount();
    g_cls.assign(1u << 20, C_UNKNOWN);
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    g_imgLo = (uintptr_t)base;
    g_imgHi = g_imgLo + nt->OptionalHeader.SizeOfImage;
    g_tracked.clear();
    for (auto& [o, k] : d3d::TrackedObjects()) g_tracked[(uintptr_t)o] = k;
    CollectExports();
    g_heaps.clear();
    HANDLE heaps[64];
    DWORD nh = GetProcessHeaps(64, heaps);
    for (DWORD i = 0; i < nh && i < 64; i++) {
        PROCESS_HEAP_ENTRY e = {};
        if (!HeapLock(heaps[i])) continue;
        while (HeapWalk(heaps[i], &e))
            if (e.wFlags & PROCESS_HEAP_REGION)
                g_heaps.push_back({(uintptr_t)e.Region.lpFirstBlock, (uintptr_t)e.Region.lpLastBlock, (int)i});
        HeapUnlock(heaps[i]);
    }
    Log("CENSUS %s: tracked d3d objects %u, device %p, arena %p..+%u MB, %u heaps (process heap #0 = %p)", tag,
        (unsigned)g_tracked.size(), d3d::Device(), arena::Base(), (unsigned)(arena::UsedEnd() >> 20), nh, heaps[0]);

    std::map<std::string, int> dataKinds;
    for (uintptr_t a = g3->dataBegin; a < g3->dataEnd; a += 4) {
        uintptr_t v = *(uintptr_t*)a;
        std::string d = Describe(v);
        if (d.empty()) continue;
        dataKinds[d.rfind("code ", 0) == 0 ? d.substr(0, d.find('+')) : d]++;
        Log("CENSUS data %08X = %08X  %s", (unsigned)a, (unsigned)v, d.c_str());
    }
    for (auto& [k, n] : dataKinds) Log("CENSUS data summary: %5d x %s", n, k.c_str());

    g_agg.clear();
    arena::Lock();
    arena::ForEachUsed(&ScanBlock, nullptr);
    arena::Unlock();
    for (auto& [k, a] : g_agg) {
        std::string eg;
        for (auto& e : a.eg) eg += e;
        Log("CENSUS arena: %7d x %-36s e.g.%s", a.n, k.c_str(), eg.c_str());
    }

    uintptr_t ps = Mem<uintptr_t>(g3->rsPs);
    HWND wnd = ps ? Mem<HWND>(ps) : nullptr;
    for (uintptr_t a = g3->dataBegin; a < g3->dataEnd; a += 4)
        if (wnd && Mem<HWND>(a) == wnd) Log("CENSUS hwnd %p at %08X", wnd, (unsigned)a);
    for (int i = 0; i < 0x40; i += 4) Log("CENSUS psGlobal+%02X = %08X", i, ps ? Mem<uint32_t>(ps + i) : 0);
    Log("CENSUS %s done in %.0f ms", tag, (double)GetTickCount() - t0);
}

}  // namespace census
