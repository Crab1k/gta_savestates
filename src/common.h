// Shared helpers for savestate.asi (GTA San Andreas full savestates).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstddef>

void Log(const char* fmt, ...);

// Make [addr, addr+size) writable (code or read-only data).
inline void Unprotect(uintptr_t addr, size_t size) {
    DWORD old;
    VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &old);
}

// Write a 5-byte relative JMP (0xE9) or CALL (0xE8) at `at` to `to`.
inline void WriteRel(uintptr_t at, uint8_t op, const void* to) {
    Unprotect(at, 5);
    *(uint8_t*)at = op;
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

inline uintptr_t ReadCallTarget(uintptr_t at) { return at + 5 + *(int32_t*)(at + 1); }

template <typename T> inline T& Mem(uintptr_t a) { return *reinterpret_cast<T*>(a); }

// Patch the import `func` of `dll` in module `mod`'s IAT. Returns the previous pointer (nullptr if not found).
void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook);
// Same for a delay-loaded import (the game delay-loads d3d9.dll). Returns the old slot value (a delay-load
// thunk); the hook must resolve the real function with LoadLibrary/GetProcAddress when first called.
void* PatchDelayIat(HMODULE mod, const char* dll, const char* func, void* hook);

// True when `p` lies inside the main executable's image.
bool InGameImage(const void* p);
