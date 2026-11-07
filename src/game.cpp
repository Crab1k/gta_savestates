#include "game.h"
#include "common.h"
#include <cstdio>

// 1.0 US (HOODLUM; Compact shares the game addresses). Sources: gta-reversed, the analysed Ghidra program.
static const GameAddrs k10US = {
    GameVersion::V10US, "GTA SA 1.0 US",
    0x8A4000, 0xCB0000,                 // .data, _TEXT_HA, _rwdseg
    0x748A5D,                           // WinMain: call [PeekMessageA] at 0x748A57
    0xC8D4C0,
    0xBA6748, 0x5C,
    0xB7CB4C,
    0xB7CB84,
    0xB7CD98,
    0xC17040,
    {{0xB7CB28, 8}, {0xB7CB30, 16}, {0xB7CB44, 4}, {0, 0}},
    0x827B3D, 0x14,
    0x8E3FFC, 0x8E4090, 0x8E3FEC,
    0xC97C28,
    0x7F5500, 0x7F56A0,
    0x730AC0, 0x8D6084,
    0xB5F8B8, 0xB608D0, 0xB62CB0,
    0x8E3110, 0xC9C2F4, 0xC9C300, 0xC9AF94, 0xB6BAD4,
    {0x8E3FE0, 0x8B8},
    0xC97C1C, 0xC9BCE0, 0xC9C040,
    0x748EA6,
    0xA49960,
};

// 1.01 EU: found by masked byte-pattern matching against 1.0 (tools/xver.py) and verified by reference counts.
// Code moved by +0x0..+0x900 depending on the region; data by +0x2680 (0x8E..0xBA), +0x27C0 (0xC1..0xC9),
// +0x23C8 (0x8D6084).
static const GameAddrs k101EU = {
    GameVersion::V101EU, "GTA SA 1.01 EU",
    0x8A5000, 0xCB2000,
    0x74933D,                           // WinMain: call [PeekMessageA] at 0x749337
    0xC8FC80,
    0xBA8DC8, 0x5C,
    0xB7F1CC,
    0xB7F204,
    0xB7F418,
    0xC19800,
    {{0xB7F1A8, 8}, {0xB7F1B0, 16}, {0xB7F1C4, 4}, {0, 0}},
    0x82842D, 0x14,
    0x8E667C, 0x8E6710, 0x8E666C,
    0xC9A3E8,
    0x7F5E00, 0x7F5FA0,
    0x7312F0, 0x8D844C,
    0xB61F38, 0xB62F50, 0xB65330,
    0x8E5790, 0xC9EAB4, 0xC9EAC0, 0xC9D754, 0xB6E154,
    {0x8E6660, 0x8B8},
    0xC9A3DC, 0xC9E4A0, 0xC9E800,
    0x7497AB,
    0xA4BFE0,                           // found in a savestate by main.scm's models segment
};

const GameAddrs* g_game = nullptr;

const GameAddrs* DetectGame() {
    auto* dos = (IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)dos + dos->e_lfanew);
    uint32_t entry = nt->OptionalHeader.AddressOfEntryPoint;
    // Version markers (as plugin-sdk): a `mov edi, 0x94BF` in the CRT startup at a version-specific address.
    if (*(uint32_t*)0x82457C == 0x94BF) return &k10US;
    if (*(uint32_t*)0x82533C == 0x94BF) return &k101EU;
    if (*(uint32_t*)0x8252FC == 0x94BF) Log("GTA SA 1.01 US is not supported yet (1.0 US and 1.01 EU are)");
    else Log("unsupported gta_sa.exe (entry RVA 0x%X); supported: 1.0 US, 1.01 EU", entry);
    return nullptr;
}

bool ValidateProcessGlobals(HWND window, void* device, std::string& why) {
    const GameAddrs* g = g_game;
    char b[200];
    // __getptd: push dword ptr [__tlsindex] right after GetLastError
    if (Mem<uint16_t>(g->getptd + 8) != 0x35FF || Mem<uint32_t>(g->getptd + 10) != g->tlsIndex) {
        sprintf_s(b, "CRT TLS index not at %08X", (unsigned)g->tlsIndex); why = b; return false;
    }
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    bool heapOk = false;
    for (DWORD i = 0; i < n && i < 64; i++) heapOk |= heaps[i] == Mem<HANDLE>(g->crtHeap);
    if (!heapOk) { sprintf_s(b, "CRT heap handle not at %08X", (unsigned)g->crtHeap); why = b; return false; }
    if (Mem<HWND>(g->rwWindowHandle) != window || Mem<HWND>(g->rwPresent + 0x1C) != window ||
        Mem<void*>(g->rwWindowHandle + 12) != device || Mem<void*>(g->rwD3DDevice) != device) {
        why = "RenderWare window/device globals not where expected"; return false;
    }
    if (Mem<uintptr_t>(g->cdStreamGlobals.addr + 0x1C) != Mem<uintptr_t>(g->cdStreams)) {
        why = "CdStream globals not where expected"; return false;
    }
    if (Mem<uint32_t>(g->winMainFreeArgs - 5) != 0x20244C8B) {   // mov ecx, [esp+20h]; push ecx; call _free
        why = "WinMain exit code not where expected"; return false;
    }
    return true;
}
