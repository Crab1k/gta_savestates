// The game heap replacement: one reserved region, TLSF allocator, control block inside the region.
// Everything the game allocates lives in [Base(), Base() + PoolEnd()), so a savestate can copy it verbatim.
#pragma once
#include <cstddef>
#include <cstdint>

namespace arena {

bool   Init(size_t reserveBytes, uintptr_t preferredBase);   // falls back to any address
void*  Alloc(size_t size, bool zero);
bool   Free(void* p);                       // false: not an arena pointer
void*  Realloc(void* p, size_t size, bool inPlaceOnly, bool zero);
size_t Size(const void* p);                 // requested size of an arena block
bool   Contains(const void* p);

void Lock();
void Unlock();

uint8_t* Base();
size_t   UsedEnd();        // bytes from Base() that the allocator has ever touched (pool end + sentinel)
size_t   Committed();      // bytes committed (live, never shrinks)
bool     CommitAtLeast(size_t bytes);   // before restoring a savestate whose heap had grown further
size_t   Reserved();
size_t   LiveBytes();      // bytes in allocated blocks
uint32_t LiveBlocks();

// Call fn(ctx, payload, payloadBytes) for every allocated block, in address order (arena locked by the caller).
void ForEachUsed(void (*fn)(void* ctx, uint8_t* p, uint32_t bytes), void* ctx);

}
