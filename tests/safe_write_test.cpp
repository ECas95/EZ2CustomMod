// Guards SafeWriteBytes and its cached range check -- the fast path for filling the song object.
//
// Writing the catalogue used to cost three syscalls per field: a VirtualQuery to validate, then
// VirtualProtect to PAGE_READWRITE and back again. writeEntry makes seventeen of those per song
// across twenty thousand songs, and re-protecting small slices inside one large region also splits
// the process's VAD tree, so each call is slower than the last. Measured here at 58 s for one pass.
//
// The fix caches the range once and memcpys. That is a correctness risk if it ever writes outside
// the range it validated, or stops honouring read-only targets, so both are pinned below.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o safe_write_test tests/safe_write_test.cpp
//   ./safe_write_test
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// ---- copied verbatim from trace_hooks.cpp -------------------------------------------------------

static uintptr_t g_readableBase = 0, g_readableEnd = 0;

static bool CanReadPtr(const void* ptr, size_t bytes = 4) {
    if (!ptr) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    if (g_readableEnd && a >= g_readableBase && a + bytes <= g_readableEnd) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xff;
    if (p == PAGE_NOACCESS || p == PAGE_EXECUTE) return false;
    g_readableBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    g_readableEnd  = g_readableBase + mbi.RegionSize;
    return a + bytes <= g_readableEnd;
}

static uint32_t g_writableBase = 0, g_writableEnd = 0;

static void DropRangeCaches() {
    g_writableBase = g_writableEnd = 0;
    g_readableBase = g_readableEnd = 0;
}

static bool RangeIsWritable(uint32_t addr, size_t bytes) {
    if (g_writableEnd && addr >= g_writableBase && addr + bytes <= g_writableEnd) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD p = mbi.Protect & 0xff;
    if (p != PAGE_READWRITE && p != PAGE_WRITECOPY &&
        p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) return false;
    g_writableBase = reinterpret_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress));
    g_writableEnd  = g_writableBase + static_cast<uint32_t>(mbi.RegionSize);
    return addr + bytes <= g_writableEnd;
}

static bool SafeWriteBytes(uint32_t addr, const void* src, size_t bytes) {
    if (RangeIsWritable(addr, bytes)) {
        std::memcpy(reinterpret_cast<void*>(addr), src, bytes);
        return true;
    }
    if (!CanReadPtr(reinterpret_cast<void*>(addr), bytes)) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), bytes, PAGE_READWRITE, &oldProtect)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), src, bytes);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), bytes, oldProtect, &tmp);
    return true;
}

int main() {
    printf("safe writes\n");
    const size_t kSize = 64 * 1024;

    // ---- the fast path: a writable region, written all over -------------------------------------
    // Twice the space is reserved and only the first half committed, so what lies past the region
    // is reliably not writable. Committing exactly kSize and testing the address after it tests the
    // OS allocator instead: it is free to place another writable region there, and when it did, the
    // write past the end legitimately succeeded and the check failed at random.
    unsigned char* rw = (unsigned char*)VirtualAlloc(NULL, kSize * 2, MEM_RESERVE, PAGE_NOACCESS);
    if (rw) rw = (unsigned char*)VirtualAlloc(rw, kSize, MEM_COMMIT, PAGE_READWRITE);
    check(rw != NULL, "a writable region with reserved space after it");
    if (!rw) return 1;
    DropRangeCaches();

    const uint32_t base = (uint32_t)(uintptr_t)rw;
    const uint32_t magic = 0xA5A5F00D;
    check(SafeWriteBytes(base, &magic, 4), "a write into writable memory succeeds");
    check(*(uint32_t*)rw == magic, "and the bytes actually land");

    // The cache is now warm; every later write in the region must still be correct.
    bool allOk = true, allLanded = true;
    for (size_t off = 0; off + 4 <= kSize; off += 4) {
        const uint32_t v = (uint32_t)off ^ 0x1234;
        if (!SafeWriteBytes(base + (uint32_t)off, &v, 4)) allOk = false;
        if (*(uint32_t*)(rw + off) != v) allLanded = false;
    }
    check(allOk, "every write across the region succeeds with a warm cache");
    check(allLanded, "and every one lands at the right address");

    // ---- the boundary the cache must not cross --------------------------------------------------
    check(!SafeWriteBytes(base + (uint32_t)kSize - 2, &magic, 4),
          "a write running past the region's end is refused");
    check(!SafeWriteBytes(base + (uint32_t)kSize, &magic, 4),
          "so is one starting past it");

    // ---- read-only targets still work, through the slow path -------------------------------------
    unsigned char* ro = (unsigned char*)VirtualAlloc(NULL, kSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READONLY);
    check(ro != NULL, "a read-only region to test against");
    if (ro) {
        DropRangeCaches();
        const uint32_t robase = (uint32_t)(uintptr_t)ro;
        check(!RangeIsWritable(robase, 4), "read-only memory is not taken as writable");
        check(SafeWriteBytes(robase, &magic, 4), "and still gets written, via VirtualProtect");
        check(*(uint32_t*)ro == magic, "with the bytes landing there too");
        DWORD prot = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(ro, &mbi, sizeof(mbi));
        prot = mbi.Protect & 0xff;
        check(prot == PAGE_READONLY, "and its protection restored afterwards");
        VirtualFree(ro, 0, MEM_RELEASE);
    }

    // ---- a cache must not survive an operation boundary ------------------------------------------
    // This is why DropRangeCaches runs at the top of every object pass: a range cached against one
    // song object must never be trusted for the next, because the memory it described may be gone.
    // The contract tested is the drop itself -- that no bounds remain to match against. Freeing a
    // region and asking whether the address still validates would test the OS allocator instead,
    // and it does hand the same address back, which made that check fail at random.
    DropRangeCaches();
    check(g_writableBase == 0 && g_writableEnd == 0 &&
          g_readableBase == 0 && g_readableEnd == 0,
          "the drop leaves no cached bounds behind");
    check(RangeIsWritable(base, 4), "a warm cache re-validates from scratch after a drop");
    check(g_writableEnd != 0, "and caches the range it just proved");

    VirtualFree(rw, 0, MEM_RELEASE);
    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
