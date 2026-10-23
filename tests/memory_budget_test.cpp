// Automatic cache sizes by address space and physical memory (config.h MemoryBudgetFor).
#include "../src/config.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    const unsigned long long mb = 1ull << 20, gb = 1ull << 30;
    // XP 32-bit: 2 GB of address space (reported a little under 2048 MB).
    MemoryBudget b = MemoryBudgetFor(2047 * mb, 4 * gb);
    assert(b.keysoundMB == 128 && b.assetMB == 96 && b.previewFpsCap == 10 && !strcmp(b.tier, "2GB"));
    // /3GB, or a 64-bit host with little RAM, takes the middle tier.
    b = MemoryBudgetFor(3 * gb, 8 * gb);
    assert(b.keysoundMB == 256 && b.assetMB == 160 && b.previewFpsCap == 20);
    b = MemoryBudgetFor(4 * gb, 2 * gb);
    assert(!strcmp(b.tier, "3GB"));
    // Large-address-aware on 64-bit with plenty of RAM: the sizes the mod shipped with.
    b = MemoryBudgetFor(4 * gb, 16 * gb);
    assert(b.keysoundMB == 384 && b.assetMB == 256 && b.previewFpsCap == 30);
    // A small machine drops to the smallest tier whatever its address space.
    b = MemoryBudgetFor(4 * gb, 1 * gb);
    assert(!strcmp(b.tier, "2GB"));
    // Unknown physical memory (0) does not count against the machine.
    b = MemoryBudgetFor(4 * gb, 0);
    assert(!strcmp(b.tier, "4GB"));
    std::puts("memory_budget_test OK");
    return 0;
}
