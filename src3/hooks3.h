#pragma once
#include "../src/common.h"

// Install the heap (GlobalAlloc/GlobalFree) and main-loop (PeekMessageA) import hooks. `lateInit` runs once the
// game's import table is usable (at once, or after the protector filled it): install the other hooks there.
bool InstallHooks3(void (*lateInit)());

// Call fn(ctx, slotsAddress, bytes) for every import address table of the exe (they hold our hooks and the
// process's own DLL addresses, so a load must keep them).
void ImportSlots(void (*fn)(void* ctx, uintptr_t slot, uint32_t count), void* ctx);

uint32_t ArenaAllocCount();

// The file a Miles stream handle was opened with, and the volume / pan the game last gave it.
// file: the game's own file handle for it when the game gave Miles file callbacks (Vice City: an MSL FILE*).
struct StreamFile { char name[MAX_PATH]; int32_t volume, pan; uint32_t file; };
bool StreamFileOf(uintptr_t stream, StreamFile& out);
uintptr_t OpenStream(uintptr_t driver, const StreamFile& f);   // through the game's import (so it is tracked)
void CloseStream(uintptr_t stream);
void CloseGameFile(uint32_t file);   // through the game's Miles close callback (if it set callbacks)

extern void (WINAPI* RealSleep)(DWORD);
extern bool g_arenaOverflow, g_heapHooked;
void OnMainLoop();                // defined in main3.cpp, called once per WinMain loop iteration
