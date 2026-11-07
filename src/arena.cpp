// TLSF (two-level segregated fit) allocator over one reserved region, in the style of Matt Conte's tlsf.c,
// with 8-byte alignment on 32-bit (8-byte block header: size + requested size) and in-place pool growth.
// The control block lives at the start of the region, so restoring the region restores the allocator.
#include "arena.h"
#include "common.h"
#include <cstring>

namespace arena {
namespace {

constexpr uint32_t SL_LOG2 = 5;
constexpr uint32_t SL_COUNT = 1u << SL_LOG2;          // 32 second-level lists
constexpr uint32_t FL_SHIFT = SL_LOG2 + 3;            // 8: sizes below 256 are linear, 8-byte steps
constexpr uint32_t FL_MAX = 30;                       // blocks below 1 GB
constexpr uint32_t FL_COUNT = FL_MAX - FL_SHIFT + 1;  // 23
constexpr uint32_t SMALL_BLOCK = 1u << FL_SHIFT;

constexpr uint32_t FREE_BIT = 1, PREV_FREE_BIT = 2;

struct Block {
    Block*   prevPhys;   // valid only while the previous block is free (lives in its payload tail)
    uint32_t pad;
    uint32_t size;       // payload bytes | FREE_BIT | PREV_FREE_BIT
    uint32_t reqSize;    // bytes the caller asked for
    Block*   nextFree;   // free blocks only
    Block*   prevFree;
};
constexpr uint32_t OVERHEAD = 8;                     // size + reqSize
constexpr uint32_t START_OFFSET = 16;                // payload = block + 16
constexpr uint32_t BLOCK_MIN = sizeof(Block) - 8;    // 16
constexpr uint32_t BLOCK_MAX = (1u << FL_MAX) - 1;

struct Control {
    Block    nullBlock;
    uint32_t flBitmap;
    uint32_t slBitmap[FL_COUNT];
    Block*   blocks[FL_COUNT][SL_COUNT];
    Block*   sentinel;    // zero-size used block that ends the pool
    uint32_t liveBytes;
    uint32_t liveBlocks;
};

constexpr size_t COMMIT_STEP = 16u << 20;
constexpr uint32_t GROW_MIN = 4u << 20;

uint8_t* g_base = nullptr;
size_t g_reserved = 0, g_committed = 0;
CRITICAL_SECTION g_cs;

inline Control* C() { return reinterpret_cast<Control*>(g_base); }

inline int Fls(uint32_t v) { unsigned long i; return _BitScanReverse(&i, v) ? (int)i : -1; }
inline int Ffs(uint32_t v) { unsigned long i; return _BitScanForward(&i, v) ? (int)i : -1; }

inline uint32_t BSize(const Block* b) { return b->size & ~3u; }
inline void SetSize(Block* b, uint32_t s) { b->size = s | (b->size & 3u); }
inline bool IsFree(const Block* b) { return (b->size & FREE_BIT) != 0; }
inline void SetFree(Block* b) { b->size |= FREE_BIT; }
inline void SetUsed(Block* b) { b->size &= ~FREE_BIT; }
inline bool IsPrevFree(const Block* b) { return (b->size & PREV_FREE_BIT) != 0; }
inline void SetPrevFree(Block* b) { b->size |= PREV_FREE_BIT; }
inline void SetPrevUsed(Block* b) { b->size &= ~PREV_FREE_BIT; }

inline Block* FromPtr(const void* p) { return (Block*)((uint8_t*)p - START_OFFSET); }
inline void* ToPtr(const Block* b) { return (uint8_t*)b + START_OFFSET; }
inline Block* NextBlock(const Block* b) { return (Block*)((uint8_t*)ToPtr(b) + BSize(b) - OVERHEAD); }
inline Block* LinkNext(Block* b) { Block* n = NextBlock(b); n->prevPhys = b; return n; }
inline void MarkFree(Block* b) { Block* n = LinkNext(b); SetPrevFree(n); SetFree(b); }
inline void MarkUsed(Block* b) { Block* n = NextBlock(b); SetPrevUsed(n); SetUsed(b); }

inline void MappingInsert(uint32_t size, int* fl, int* sl) {
    if (size < SMALL_BLOCK) {
        *fl = 0;
        *sl = (int)(size / (SMALL_BLOCK / SL_COUNT));
    } else {
        int f = Fls(size);
        *sl = (int)((size >> (f - SL_LOG2)) ^ (1u << SL_LOG2));
        *fl = f - (int)(FL_SHIFT - 1);
    }
}

inline void MappingSearch(uint32_t size, int* fl, int* sl) {
    if (size >= SMALL_BLOCK) size += (1u << (Fls(size) - SL_LOG2)) - 1;
    MappingInsert(size, fl, sl);
}

Block* SearchSuitable(int* fl, int* sl) {
    Control* c = C();
    uint32_t slMap = c->slBitmap[*fl] & (~0u << *sl);
    if (!slMap) {
        uint32_t flMap = (*fl + 1 < 32) ? (c->flBitmap & (~0u << (*fl + 1))) : 0;
        if (!flMap) return nullptr;
        *fl = Ffs(flMap);
        slMap = c->slBitmap[*fl];
    }
    *sl = Ffs(slMap);
    return c->blocks[*fl][*sl];
}

void RemoveFree(Block* b, int fl, int sl) {
    Control* c = C();
    Block* prev = b->prevFree;
    Block* next = b->nextFree;
    next->prevFree = prev;
    prev->nextFree = next;
    if (c->blocks[fl][sl] == b) {
        c->blocks[fl][sl] = next;
        if (next == &c->nullBlock) {
            c->slBitmap[fl] &= ~(1u << sl);
            if (!c->slBitmap[fl]) c->flBitmap &= ~(1u << fl);
        }
    }
}

void InsertFree(Block* b, int fl, int sl) {
    Control* c = C();
    Block* cur = c->blocks[fl][sl];
    b->nextFree = cur;
    b->prevFree = &c->nullBlock;
    cur->prevFree = b;
    c->blocks[fl][sl] = b;
    c->flBitmap |= 1u << fl;
    c->slBitmap[fl] |= 1u << sl;
}

void BlockRemove(Block* b) { int fl, sl; MappingInsert(BSize(b), &fl, &sl); RemoveFree(b, fl, sl); }
void BlockInsert(Block* b) { int fl, sl; MappingInsert(BSize(b), &fl, &sl); InsertFree(b, fl, sl); }

inline bool CanSplit(const Block* b, uint32_t size) { return BSize(b) >= sizeof(Block) + size; }

Block* Split(Block* b, uint32_t size) {
    Block* rem = (Block*)((uint8_t*)ToPtr(b) + size - OVERHEAD);
    uint32_t remSize = BSize(b) - (size + OVERHEAD);
    rem->size = remSize;
    rem->reqSize = 0;
    SetSize(b, size);
    MarkFree(rem);
    return rem;
}

Block* Absorb(Block* prev, Block* b) {
    prev->size += BSize(b) + OVERHEAD;
    LinkNext(prev);
    return prev;
}

Block* MergePrev(Block* b) {
    if (IsPrevFree(b)) {
        Block* prev = b->prevPhys;
        BlockRemove(prev);
        b = Absorb(prev, b);
    }
    return b;
}

Block* MergeNext(Block* b) {
    Block* next = NextBlock(b);
    if (IsFree(next)) {
        BlockRemove(next);
        b = Absorb(b, next);
    }
    return b;
}

void TrimFree(Block* b, uint32_t size) {
    if (CanSplit(b, size)) {
        Block* rem = Split(b, size);
        LinkNext(b);
        SetPrevFree(rem);
        BlockInsert(rem);
    }
}

void TrimUsed(Block* b, uint32_t size) {
    if (CanSplit(b, size)) {
        Block* rem = Split(b, size);
        SetPrevUsed(rem);
        rem = MergeNext(rem);
        BlockInsert(rem);
    }
}

Block* LocateFree(uint32_t size) {
    int fl = 0, sl = 0;
    MappingSearch(size, &fl, &sl);
    if (fl >= (int)FL_COUNT) return nullptr;
    Block* b = SearchSuitable(&fl, &sl);
    if (b && b != &C()->nullBlock) {
        RemoveFree(b, fl, sl);
        return b;
    }
    return nullptr;
}

inline uint32_t AdjustRequest(size_t size) {
    if (size > BLOCK_MAX / 2) return 0;
    uint32_t a = ((uint32_t)size + 7u) & ~7u;
    return a < BLOCK_MIN ? BLOCK_MIN : a;
}

bool CommitTo(uint8_t* end) {
    size_t need = (size_t)(end - g_base);
    if (need <= g_committed) return true;
    if (need > g_reserved) return false;
    size_t target = (need + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
    if (target > g_reserved) target = g_reserved;
    if (!VirtualAlloc(g_base + g_committed, target - g_committed, MEM_COMMIT, PAGE_READWRITE)) return false;
    g_committed = target;
    return true;
}

// Turn the sentinel into a free block of at least `need` payload bytes and put a new sentinel after it.
bool Grow(uint32_t need) {
    Control* c = C();
    uint32_t ext = need + (need >> 4) + 64;
    if (ext < GROW_MIN) ext = GROW_MIN;
    ext = (ext + 0xFFFFu) & ~0xFFFFu;
    Block* s = c->sentinel;
    uint8_t* newEnd = (uint8_t*)s + ext + sizeof(Block);
    if (!CommitTo(newEnd)) {
        Log("arena: out of reserved space (need %u bytes, reserved %u MB)", need, (unsigned)(g_reserved >> 20));
        return false;
    }
    s->size = (ext - OVERHEAD) | (s->size & PREV_FREE_BIT);
    s->reqSize = 0;
    Block* n = (Block*)((uint8_t*)s + ext);
    n->size = 0;
    n->reqSize = 0;
    MarkFree(s);
    c->sentinel = n;
    s = MergePrev(s);
    BlockInsert(s);
    return true;
}

}  // namespace

bool Init(size_t reserveBytes, uintptr_t preferredBase) {
    InitializeCriticalSectionAndSpinCount(&g_cs, 4000);
    g_reserved = (reserveBytes + 0xFFFF) & ~(size_t)0xFFFF;
    if (g_reserved > BLOCK_MAX) g_reserved = (size_t)BLOCK_MAX & ~(size_t)0xFFFF;
    // A savestate from another process can only be loaded at the address it was made at.
    if (preferredBase) g_base = (uint8_t*)VirtualAlloc((void*)preferredBase, g_reserved, MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) g_base = (uint8_t*)VirtualAlloc(nullptr, g_reserved, MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) return false;
    g_committed = 0;
    if (!CommitTo(g_base + COMMIT_STEP)) return false;

    Control* c = C();
    memset(c, 0, sizeof(Control));
    c->nullBlock.nextFree = c->nullBlock.prevFree = &c->nullBlock;
    for (uint32_t i = 0; i < FL_COUNT; i++)
        for (uint32_t j = 0; j < SL_COUNT; j++) c->blocks[i][j] = &c->nullBlock;

    // First pool: from after the control block to the end of the first commit step.
    uint8_t* mem = g_base + ((sizeof(Control) + 8 + 63) & ~(size_t)63);
    uint32_t poolBytes = (uint32_t)((g_base + g_committed - mem) - sizeof(Block)) & ~7u;   // room for the sentinel
    Block* b = (Block*)(mem - OVERHEAD);
    b->size = poolBytes;
    b->reqSize = 0;
    SetFree(b);
    SetPrevUsed(b);
    BlockInsert(b);
    Block* n = LinkNext(b);
    n->size = 0;
    n->reqSize = 0;
    SetUsed(n);
    SetPrevFree(n);
    c->sentinel = n;
    return true;
}

void Lock() { EnterCriticalSection(&g_cs); }
void Unlock() { LeaveCriticalSection(&g_cs); }

bool Contains(const void* p) { return (const uint8_t*)p >= g_base && (const uint8_t*)p < g_base + g_reserved; }

void* Alloc(size_t size, bool zero) {
    uint32_t adj = AdjustRequest(size);
    if (!adj) return nullptr;
    Lock();
    Block* b = LocateFree(adj);
    if (!b && Grow(adj)) b = LocateFree(adj);
    void* p = nullptr;
    if (b) {
        TrimFree(b, adj);
        MarkUsed(b);
        b->reqSize = (uint32_t)size;
        C()->liveBytes += BSize(b);
        C()->liveBlocks++;
        p = ToPtr(b);
    }
    Unlock();
    if (p && zero) memset(p, 0, size);
    return p;
}

bool Free(void* p) {
    if (!Contains(p)) return false;
    Lock();
    Block* b = FromPtr(p);
    if (IsFree(b)) {
        Unlock();
        Log("arena: double free of %p ignored", p);
        return true;
    }
    C()->liveBytes -= BSize(b);
    C()->liveBlocks--;
    MarkFree(b);
    b = MergePrev(b);
    b = MergeNext(b);
    BlockInsert(b);
    Unlock();
    return true;
}

void* Realloc(void* p, size_t size, bool inPlaceOnly, bool zero) {
    if (!p) return Alloc(size, zero);
    uint32_t adj = AdjustRequest(size);
    if (!adj) return nullptr;
    Lock();
    Block* b = FromPtr(p);
    Block* next = NextBlock(b);
    uint32_t cur = BSize(b);
    uint32_t oldReq = b->reqSize;
    uint32_t combined = cur + BSize(next) + OVERHEAD;
    if (adj > cur && (!IsFree(next) || adj > combined)) {
        Unlock();
        if (inPlaceOnly) return nullptr;
        void* q = Alloc(size, false);
        if (!q) return nullptr;
        memcpy(q, p, oldReq < size ? oldReq : size);
        if (zero && size > oldReq) memset((uint8_t*)q + oldReq, 0, size - oldReq);
        Free(p);
        return q;
    }
    C()->liveBytes -= cur;
    if (adj > cur) {
        MergeNext(b);
        MarkUsed(b);
    }
    TrimUsed(b, adj);
    b->reqSize = (uint32_t)size;
    C()->liveBytes += BSize(b);
    Unlock();
    if (zero && size > oldReq) memset((uint8_t*)p + oldReq, 0, size - oldReq);
    return p;
}

size_t Size(const void* p) { return FromPtr(p)->reqSize; }

bool CommitAtLeast(size_t bytes) { return CommitTo(g_base + bytes); }

void ForEachUsed(void (*fn)(void* ctx, uint8_t* p, uint32_t bytes), void* ctx) {
    Block* b = (Block*)(g_base + ((sizeof(Control) + 8 + 63) & ~(size_t)63) - OVERHEAD);
    for (Block* end = C()->sentinel; b != end; b = NextBlock(b))
        if (!IsFree(b)) fn(ctx, (uint8_t*)ToPtr(b), BSize(b));
}

uint8_t* Base() { return g_base; }
size_t UsedEnd() { return (size_t)((uint8_t*)C()->sentinel + sizeof(Block) - g_base); }
size_t Committed() { return g_committed; }
size_t Reserved() { return g_reserved; }
size_t LiveBytes() { return C()->liveBytes; }
uint32_t LiveBlocks() { return C()->liveBlocks; }

}  // namespace arena
