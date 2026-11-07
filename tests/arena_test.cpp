// Offline stress test for the arena allocator: random alloc/free/realloc against a reference model.
#include "../src/arena.h"
#include "../src/common.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

struct Live { uint8_t* p; size_t size; uint8_t tag; };

static bool Check(const Live& l) {
    for (size_t i = 0; i < l.size; i++)
        if (l.p[i] != (uint8_t)(l.tag + i)) return false;
    return true;
}
static void Fill(Live& l) { for (size_t i = 0; i < l.size; i++) l.p[i] = (uint8_t)(l.tag + i); }

int main() {
    if (!arena::Init(512u << 20)) { puts("init failed"); return 1; }
    std::mt19937 rng(1234);
    std::vector<Live> live;
    size_t ops = 0, fails = 0;
    for (int round = 0; round < 400000; round++) {
        int op = rng() % 10;
        if (op < 4 || live.empty()) {
            size_t sz = (rng() % 100 < 95) ? rng() % 512 : rng() % (2u << 20);
            if (rng() % 5000 == 0) sz = 20u << 20;   // occasional huge block
            bool zero = rng() % 4 == 0;
            Live l{(uint8_t*)arena::Alloc(sz, zero), sz, (uint8_t)rng()};
            if (!l.p) { printf("alloc %zu failed at round %d\n", sz, round); return 1; }
            if ((uintptr_t)l.p & 7) { printf("misaligned %p\n", l.p); return 1; }
            if (zero) for (size_t i = 0; i < sz; i++) if (l.p[i]) { printf("not zeroed\n"); return 1; }
            if (arena::Size(l.p) != sz) { printf("size mismatch\n"); return 1; }
            Fill(l);
            live.push_back(l);
        } else if (op < 8) {
            size_t i = rng() % live.size();
            if (!Check(live[i])) { printf("corruption at round %d\n", round); fails++; }
            arena::Free(live[i].p);
            live[i] = live.back();
            live.pop_back();
        } else {
            size_t i = rng() % live.size();
            Live& l = live[i];
            if (!Check(l)) { printf("corruption before realloc at round %d\n", round); fails++; }
            size_t nsz = rng() % 4096;
            bool inPlace = rng() % 5 == 0;
            uint8_t* q = (uint8_t*)arena::Realloc(l.p, nsz, inPlace, false);
            if (!q) { if (!inPlace) { printf("realloc failed\n"); return 1; } continue; }
            size_t keep = l.size < nsz ? l.size : nsz;
            for (size_t k = 0; k < keep; k++)
                if (q[k] != (uint8_t)(l.tag + k)) { printf("realloc lost data at round %d\n", round); return 1; }
            l.p = q;
            l.size = nsz;
            Fill(l);
        }
        ops++;
        if (round % 100000 == 0)
            printf("round %d: %zu live, %u MB live bytes, used end %u MB\n", round, live.size(),
                   (unsigned)(arena::LiveBytes() >> 20), (unsigned)(arena::UsedEnd() >> 20));
    }
    for (auto& l : live) { if (!Check(l)) fails++; arena::Free(l.p); }
    printf("done: %zu ops, %zu failures, live blocks after free-all %u\n", ops, fails, arena::LiveBlocks());
    // after freeing everything a big block must fit again (coalescing works)
    void* big = arena::Alloc(arena::UsedEnd() - 1024 * 1024, false);
    printf("coalesce check: %s\n", big ? "ok" : "FAILED");
    return fails || !big ? 1 : 0;
}
