// Guards the songname window: which entries get a texture, and how many objects that costs.
//
// The whole category used to be loaded at once, which is exactly one GDI object per song, never
// released, because the game's texture pool is a bump allocator. Measured:
//
//     category=98 requested=400 loaded=400 gdi=2232(+400)
//     category=97 requested=287 loaded=287 gdi=2519(+287)
//
// A process gets 10,000, so twenty-five categories exhausted it and every song name went blank.
// Eviction is unavailable -- the loader caches textures in a binary tree keyed by filename, so
// removing or renaming a node breaks the search for every other entry. Loading only what the wheel
// can draw is what is left, and a null texture slot is safe: it draws a blank name.
//
// The window arithmetic is small, and an off-by-one either shows a blank name next to the cursor or
// quietly loads the whole category again, so it gets pinned.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o songname_window_test tests/songname_window_test.cpp
//   ./songname_window_test
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

// ---- must match trace_hooks.cpp ---------------------------------------------------------------
static const uint32_t kSongnameWindow  = 16;   // rows that get a NAME
static const uint32_t kDiscWindow      = 7;    // rows around the cursor that get a DISC
static const uint32_t kGdiObjectLimit  = 10000;
static const uint32_t kGdiSafeHeadroom = 1200;
static const uint32_t kVisibleCap      = 872;   // g_visibleCap; was a stale 436 here and in the mod

// Every row index the window touched, so the tests can prove none of them left the array.
static std::vector<int64_t> g_touched;

static uint32_t g_gdiInUse = 0;
static bool GdiHeadroomAvailable(uint32_t want) {
    if (g_gdiInUse == 0) return true;
    return g_gdiInUse + want + kGdiSafeHeadroom <= kGdiObjectLimit;
}

// The loop from LoadSongnameWindow, with the object reads replaced by a slot vector.
// entry+0x004 is the song name, entry+0x000 the small disc the change animation draws.
struct Slots { std::vector<bool> hasName; std::vector<bool> hasDisc; std::vector<bool> isCustom; };

static void LoadSongnameWindow(Slots& s, uint32_t selected, uint32_t visible,
                               unsigned long& loaded, unsigned long& blocked) {
    loaded = blocked = 0;
    g_touched.clear();
    if (!visible || visible > kVisibleCap) return;
    // `selected` comes off the renderer's stack and is not always a valid row -- see the LV-category
    // crash. Out of range it must do nothing, not index with the garbage.
    if (selected >= visible) return;
    const int32_t half     = (int32_t)(kSongnameWindow / 2);
    const int32_t discHalf = (int32_t)(kDiscWindow / 2);
    for (int32_t k = -half; k <= half; ++k) {
        int32_t raw = ((int32_t)selected + k) % (int32_t)visible;
        if (raw < 0) raw += (int32_t)visible;
        const uint32_t i = (uint32_t)raw;
        g_touched.push_back((int64_t)raw);
        const bool wantDisc = (k >= -discHalf && k <= discHalf);
        if (s.hasName[i] && (!wantDisc || s.hasDisc[i])) continue;
        if (!s.isCustom[i]) continue;
        if (!GdiHeadroomAvailable(2)) { ++blocked; break; }
        if (!s.hasName[i]) { s.hasName[i] = true; ++g_gdiInUse; ++loaded; }
        if (wantDisc && !s.hasDisc[i]) { s.hasDisc[i] = true; ++g_gdiInUse; ++loaded; }
    }
}
// -----------------------------------------------------------------------------------------------

// names across the window, discs only in the band
static const unsigned kWindowCost = (kSongnameWindow + 1) + kDiscWindow;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static Slots makeCategory(uint32_t n, bool custom = true) {
    Slots s;
    s.hasName.assign(n, false);
    s.hasDisc.assign(n, false);
    s.isCustom.assign(n, custom);
    return s;
}

// Every index the window produced has to be a real row. This is the check that would have caught
// the LV-category crash: the old arithmetic returned a NEGATIVE remainder, the caller cast it to
// uint32_t, and the entry pointer landed 584 bytes under the object.
static void checkInRange(uint32_t visible, const char* what) {
    for (int64_t i : g_touched) {
        if (i >= 0 && i < (int64_t)visible) continue;
        std::printf("FAIL: %s -- index %lld outside [0,%lu)\n", what, (long long)i,
                    (unsigned long)visible);
        ++g_failures;
        return;
    }
}

int main() {
    // ---- the LV-category crash -----------------------------------------------------------------
    // Navigating a NATIVE category handed the loader a `selected` from a frame that did not
    // describe the current list. Nothing it produces may leave the array, and a selection that is
    // not a row at all must load nothing rather than index with it.
    {
        const uint32_t visible = 22;              // LV categories in the crash log: 22..41 songs
        for (uint32_t selected : {visible, visible + 1, 436u, 65535u, 0x80000000u, 0xFFFFFFFFu}) {
            g_gdiInUse = 500;
            Slots s = makeCategory(visible);
            unsigned long loaded = 0, blocked = 0;
            LoadSongnameWindow(s, selected, visible, loaded, blocked);
            checkInRange(visible, "out-of-range selected");
            char msg[96];
            std::snprintf(msg, sizeof(msg), "selected=%lu >= visible loads nothing",
                          (unsigned long)selected);
            check(loaded == 0, msg);
        }
    }
    // A valid selection near row 0 still wraps backwards over the end of the list, and stays inside.
    {
        for (uint32_t visible : {1u, 2u, 5u, 22u, 400u, 872u}) {
            for (uint32_t selected : {0u, visible / 2, visible - 1}) {
                g_gdiInUse = 500;
                Slots s = makeCategory(visible);
                unsigned long loaded = 0, blocked = 0;
                LoadSongnameWindow(s, selected, visible, loaded, blocked);
                checkInRange(visible, "valid selected");
            }
        }
    }
    // The cap is the real one: a category between the old literal and the current ceiling must
    // still load. At 436 this silently stopped loading any name at all.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(600);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 300, 600, loaded, blocked);
        check(loaded > 0, "a 600-song category still loads (the 436 literal was stale vs the 872 cap)");
    }

    // A big category costs the window, not the category.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 0, 400, loaded, blocked);
        check(loaded == kWindowCost,
              "a full pass loads a name per row and a disc only near the cursor");
        check(loaded < 400, "a 400-song category no longer costs 400 objects");
        check(blocked == 0, "and nothing was blocked");
    }

    // The wheel is circular, so from the top of the list the rows above the cursor are the LAST
    // entries. Clamping there is what left them blank until the cursor reached them.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 0, 400, loaded, blocked);
        check(s.hasName[399], "the last entry is covered when the cursor sits on the first");
        check(s.hasName[400 - kSongnameWindow / 2], "and so is the rest of the tail in view");
        check(!s.hasName[200], "while the far side of the list is left alone");
    }

    // ...and symmetrically from the bottom.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 399, 400, loaded, blocked);
        check(s.hasName[0], "the first entry is covered when the cursor sits on the last");
        check(s.hasName[kSongnameWindow / 2 - 1], "along with the head of the list");
    }

    // Mid-list the window is centred and complete.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 200, 400, loaded, blocked);
        check(loaded == kWindowCost, "mid-list costs the same as at the top");
        for (int32_t k = -(int32_t)(kSongnameWindow / 2); k <= (int32_t)(kSongnameWindow / 2); ++k)
            if (!s.hasName[200 + k]) { check(false, "every entry in the window has a texture"); break; }
        check(!s.hasName[200 - kSongnameWindow / 2 - 1], "and nothing outside it does");
        check(!s.hasName[200 + kSongnameWindow / 2 + 1], "on either side");
    }

    // Scrolling only pays for what is newly exposed.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 200, 400, loaded, blocked);
        LoadSongnameWindow(s, 201, 400, loaded, blocked);
        check(loaded == 2, "moving one entry loads that entry's two slots and nothing else");
        LoadSongnameWindow(s, 201, 400, loaded, blocked);
        check(loaded == 0, "and standing still loads nothing at all");
    }

    // A category smaller than the window is covered entirely, without loading anything twice.
    // 12 rows against a 17-row window: the walk wraps right around the list, so this is the case
    // where double-loading would show up.
    {
        const uint32_t n = 12;
        g_gdiInUse = 500;
        Slots s = makeCategory(n);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, n - 1, n, loaded, blocked);
        for (uint32_t i = 0; i < n; ++i)
            if (!s.hasName[i]) { check(false, "every entry of a short category gets a name"); break; }
        // Every row gets a name; the disc band is smaller than the list, so only kDiscWindow of
        // them pay for a disc. Loading anything twice would push this number up.
        check(loaded == n + kDiscWindow,
              "a short category loads each name once and only the band pays for a disc");
        check(g_gdiInUse == 500 + n + kDiscWindow,
              "the wrap covers every row exactly once");
    }

    // Native songs are the game's own business.
    {
        g_gdiInUse = 500;
        Slots s = makeCategory(100, false);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 50, 100, loaded, blocked);
        check(loaded == 0, "native entries are never loaded by us");
    }

    // The budget: browsing many categories has to stay far under the ceiling.
    {
        g_gdiInUse = 500;
        unsigned long total = 0;
        for (int category = 0; category < 52; ++category) {
            Slots s = makeCategory(400);
            unsigned long loaded = 0, blocked = 0;
            LoadSongnameWindow(s, 0, 400, loaded, blocked);   // opened, not scrolled through
            total += loaded;
        }
        std::printf("  52 categories browsed: %lu objects, gdi now %lu\n", total, (unsigned long)g_gdiInUse);
        check(g_gdiInUse < kGdiObjectLimit - kGdiSafeHeadroom,
              "browsing every category stays inside the budget");
        check(total < 52 * 400 / 4, "and costs a small fraction of loading them whole");
        check(total == 52 * kWindowCost,
              "per category: a name for every row in the window, a disc only inside the band");

        // The reason the windows are sized the way they are. Browsing 110 categories measured
        // 251KB of surface per texture and nothing is ever freed, so the only lever is how many
        // textures a category costs. The old 32+32 window projected to 1979MB across 126
        // categories, which alone exhausts XP's 2GB on top of an ~840MB baseline.
        const unsigned perCat = kWindowCost;
        const double surfaceMB = 126.0 * perCat * 251.0 / 1024.0;
        std::printf("  126 categories at %u textures each = %.0f MB of surface\n",
                    perCat, surfaceMB);
        check(surfaceMB < 1000.0, "walking every category stays under 1GB of texture surface");
    }

    // The guard still holds when someone does scroll through everything.
    {
        g_gdiInUse = 9000;
        Slots s = makeCategory(400);
        unsigned long loaded = 0, blocked = 0;
        LoadSongnameWindow(s, 200, 400, loaded, blocked);
        check(loaded == 0 && blocked == 1, "past the safe line nothing loads and it stops at once");
        check(g_gdiInUse == 9000, "no object is consumed past the line");
    }

    if (g_failures == 0) std::printf("songname window: ok\n");
    return g_failures ? 1 : 0;
}
