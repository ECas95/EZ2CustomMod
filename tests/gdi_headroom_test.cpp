// Guards the GDI headroom check that keeps the songname preload away from the process ceiling.
//
// Every texture the game loads costs a GDI object, and its pool never frees, so browsing categories
// only ever adds. The measured wall:
//
//     [mem] render failed: committed=3296MB largestFreeBlock=15MB regions=43375 gdiObjects=9999
//     [mem] render failed: committed=3296MB largestFreeBlock=15MB regions=43379 gdiObjects=10000
//
// At the limit EVERY GDI allocation fails at once -- the game's textures, our rendering, and
// GdiplusStartup, which is what made it look like a memory problem and cost a round of chasing.
//
// The arithmetic is small but it is the difference between a blank song name and a dead session, so
// it gets pinned. GdiHeadroomAvailable is copied verbatim from trace_hooks.cpp with the live query
// replaced by an injectable value.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o gdi_headroom_test tests/gdi_headroom_test.cpp
//   ./gdi_headroom_test
#include <cstdint>
#include <cstdio>

// ---- must match trace_hooks.cpp ---------------------------------------------------------------
static const uint32_t kGdiObjectLimit  = 10000;
static const uint32_t kGdiSafeHeadroom = 1200;

static uint32_t g_fakeInUse = 0;
static uint32_t GdiObjectsInUse() { return g_fakeInUse; }

static bool GdiHeadroomAvailable(uint32_t want) {
    const uint32_t used = GdiObjectsInUse();
    if (used == 0) return true;                    // query unavailable; do not block on it
    return used + want + kGdiSafeHeadroom <= kGdiObjectLimit;
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// What the preload loop does: keep loading while there is room, and report what it skipped.
struct Outcome { unsigned long loaded, skipped; };
static Outcome preload(unsigned long songs, uint32_t startingInUse, bool poolGrows) {
    g_fakeInUse = startingInUse;
    Outcome o{0, 0};
    for (unsigned long i = 0; i < songs; ++i) {
        if (!GdiHeadroomAvailable(1)) { ++o.skipped; continue; }
        ++o.loaded;
        if (poolGrows) ++g_fakeInUse;              // the pool never frees, so each load costs one
    }
    return o;
}

int main() {
    // A fresh process loads a whole category without noticing the guard.
    {
        Outcome o = preload(400, 500, true);
        check(o.loaded == 400 && o.skipped == 0, "a fresh process loads a 400-song category in full");
        check(g_fakeInUse == 900, "and each load costs exactly one object");
    }

    // Deep into a session the guard stops short, and never reaches the ceiling.
    {
        Outcome o = preload(400, 8500, true);
        check(o.skipped > 0, "close to the ceiling, some songs are skipped");
        check(o.loaded + o.skipped == 400, "every song is accounted for, loaded or skipped");
        check(g_fakeInUse <= kGdiObjectLimit - kGdiSafeHeadroom,
              "the guard never lets the count reach the limit");
        check(g_fakeInUse < 10000, "and the hard ceiling is never touched");
    }

    // Already past the safe line: nothing loads, nothing crashes.
    {
        Outcome o = preload(400, 9500, true);
        check(o.loaded == 0 && o.skipped == 400, "past the safe line the whole category is skipped");
        check(g_fakeInUse == 9500, "and no further objects are consumed");
    }

    // Exactly on the boundary, which is where an off-by-one would hide.
    {
        g_fakeInUse = kGdiObjectLimit - kGdiSafeHeadroom - 1;
        check(GdiHeadroomAvailable(1), "one below the line still admits a load");
        g_fakeInUse = kGdiObjectLimit - kGdiSafeHeadroom;
        check(!GdiHeadroomAvailable(1), "on the line, loading stops");
        g_fakeInUse = kGdiObjectLimit;
        check(!GdiHeadroomAvailable(1), "at the ceiling, loading stops");
    }

    // If the query is unavailable the guard must not silently disable the feature.
    {
        g_fakeInUse = 0;
        check(GdiHeadroomAvailable(1), "a zero reading is treated as 'unknown', not 'full'");
        Outcome o = preload(400, 0, false);
        check(o.loaded == 400 && o.skipped == 0, "and a category still loads normally");
    }

    // The headroom has to be big enough to matter: the wheel, the video path and the CRT all draw.
    check(kGdiSafeHeadroom >= 1000, "the reserve is large enough for the rest of the process");
    check(kGdiSafeHeadroom < kGdiObjectLimit / 4, "but not so large it wastes most of the budget");

    if (g_failures == 0) std::printf("gdi headroom: ok\n");
    return g_failures ? 1 : 0;
}
