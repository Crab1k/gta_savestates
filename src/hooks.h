#pragma once
#include "common.h"

bool InstallHeapHooks();          // routes the exe's HeapAlloc & co. into the arena
bool InstallLoopHooks();          // PeekMessageA (main-loop safe point), Sleep (thread parking)

// Park the audio stream and smooth-fade threads inside their Sleep calls (outside any work).
bool ParkAudioThreads(DWORD timeoutMs);
void UnparkAudioThreads();

extern void (WINAPI* RealSleep)(DWORD);
extern bool g_arenaOverflow;      // an allocation had to fall back to the real heap: savestates unsafe

void OnMainLoop();                // defined in main.cpp, called once per WinMain loop iteration
