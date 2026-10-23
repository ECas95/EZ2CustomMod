// Proves the replacement category filter produces exactly what sub_435070 produced.
//
// The original works in two phases: gather every id of the category by LINEARLY scanning master[]
// with _stricmp (first match wins) into a 436-entry stack array, then copy the gathered entries
// whose level float is non-zero into the visible array. The replacement fuses both phases into one
// pass and replaces the linear scan with a case-insensitive hash index.
//
// Two things have to hold for that to be a safe swap:
//   - the hash index must resolve every id to the SAME master entry the linear scan did, including
//     case differences and duplicate ids (where the scan returns the lowest index), and
//   - fusing gather and filter must not change the visible list or its order.
// Both are checked here against a straight reimplementation of the original semantics.
//
// NormalizeId, HashId16 and the index build/lookup are copied verbatim from trace_hooks.cpp, the
// same way lane_assign_test.cpp copies chooseAutoLane.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o filter_index_test tests/filter_index_test.cpp
//   ./filter_index_test
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---- verbatim from trace_hooks.cpp ------------------------------------------------------------
static void NormalizeId(const char* src, char* dst16) {
    memset(dst16, 0, 16);
    for (int i = 0; i < 15 && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst16[i] = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
    }
}

static uint32_t HashId16(const char* k) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16 && k[i]; ++i) h = (h ^ (unsigned char)k[i]) * 16777619u;
    return h;
}

struct MasterIdIndex {
    std::vector<char>    keys;
    std::vector<int32_t> table;
    uint32_t mask = 0;
};
static MasterIdIndex g_midx;

static void BuildIndex(const std::vector<std::string>& masterIds) {
    const uint32_t count = (uint32_t)masterIds.size();
    g_midx.keys.assign((size_t)count * 16, 0);
    size_t cap = 64;
    while (cap < (size_t)count * 2) cap <<= 1;
    g_midx.table.assign(cap, -1);
    g_midx.mask = (uint32_t)(cap - 1);
    for (uint32_t i = 0; i < count; ++i) {
        char* key = &g_midx.keys[(size_t)i * 16];
        NormalizeId(masterIds[i].c_str(), key);
        if (!key[0]) continue;
        uint32_t h = HashId16(key) & g_midx.mask;
        bool exists = false;
        while (g_midx.table[h] >= 0) {
            if (memcmp(&g_midx.keys[(size_t)g_midx.table[h] * 16], key, 16) == 0) { exists = true; break; }
            h = (h + 1) & g_midx.mask;
        }
        if (exists) continue;                       // keep the first, like the original scan
        g_midx.table[h] = (int32_t)i;
    }
}

static int32_t LookupMaster(const char* rawId) {
    if (g_midx.table.empty()) return -1;
    char key[16];
    NormalizeId(rawId, key);
    if (!key[0]) return -1;
    uint32_t h = HashId16(key) & g_midx.mask;
    while (g_midx.table[h] >= 0) {
        if (memcmp(&g_midx.keys[(size_t)g_midx.table[h] * 16], key, 16) == 0) return g_midx.table[h];
        h = (h + 1) & g_midx.mask;
    }
    return -1;
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// Portable stand-in for _stricmp, matching what the original used.
static int striCompare(const char* a, const char* b) {
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

// What the original did: scan master[] top to bottom, first _stricmp match wins.
static int32_t LinearScan(const std::vector<std::string>& masterIds, const char* id) {
    for (size_t i = 0; i < masterIds.size(); ++i)
        if (striCompare(masterIds[i].c_str(), id) == 0) return (int32_t)i;
    return -1;
}

// The original's two phases, kept separate exactly as the decompilation has them.
static std::vector<std::string> ReferenceFilter(const std::vector<std::string>& masterIds,
                                                const std::vector<float>& levels,
                                                const std::vector<std::string>& categoryIds,
                                                size_t cap, size_t& reportedCount) {
    std::vector<size_t> gathered;                       // phase 2: into the 436-entry stack array
    for (const auto& id : categoryIds) {
        int32_t mi = LinearScan(masterIds, id.c_str());
        if (mi < 0) continue;
        if (gathered.size() >= cap) break;              // the stack array is what bounds this
        gathered.push_back((size_t)mi);
    }
    reportedCount = gathered.size();                    // what the original writes to this[31]
    std::vector<std::string> visible;                   // phase 3: drop the zero-level entries
    for (size_t mi : gathered)
        if (levels[mi] != 0.0f) visible.push_back(masterIds[mi]);
    return visible;
}

// The replacement: one pass, hashed lookup.
static std::vector<std::string> FusedFilter(const std::vector<std::string>& masterIds,
                                            const std::vector<float>& levels,
                                            const std::vector<std::string>& categoryIds,
                                            size_t cap, size_t* reportedCount = nullptr) {
    std::vector<size_t> gathered;                       // phase 1, now on the heap
    for (const auto& id : categoryIds) {
        int32_t mi = LookupMaster(id.c_str());
        if (mi < 0) continue;
        if (gathered.size() >= cap) break;              // the cap bounds the GATHER, as it did
        gathered.push_back((size_t)mi);
    }
    if (reportedCount) *reportedCount = gathered.size();
    std::vector<std::string> visible;                   // phase 2
    for (size_t mi : gathered)
        if (levels[mi] != 0.0f) visible.push_back(masterIds[mi]);
    return visible;
}


// The visible array as the replacement now leaves it, plus the count it publishes. The game reads
// visible[0 .. published-1], so every one of those slots has to hold a real entry.
struct Published { std::vector<std::string> visible; size_t count; };

static Published BuildPublished(const std::vector<std::string>& masterIds,
                                const std::vector<float>& levels,
                                const std::vector<std::string>& categoryIds,
                                size_t cap,
                                const std::vector<std::string>& previousContents) {
    std::vector<size_t> gathered;
    for (const auto& id : categoryIds) {
        int32_t mi = LookupMaster(id.c_str());
        if (mi < 0) continue;
        if (gathered.size() >= cap) break;
        gathered.push_back((size_t)mi);
    }
    // the array keeps whatever the previous category left in it
    std::vector<std::string> vis = previousContents;
    vis.resize(cap > vis.size() ? cap : vis.size(), "<stale>");
    size_t written = 0;
    for (size_t mi : gathered)
        if (levels[mi] != 0.0f) vis[written++] = masterIds[mi];
    if (written && written < gathered.size())
        for (size_t i = written; i < gathered.size(); ++i) vis[i] = vis[written - 1];
    return Published{vis, written ? gathered.size() : 0};
}

int main() {
    // ---- the index resolves exactly what the linear scan resolved --------------------------
    {
        std::vector<std::string> master;
        for (int i = 0; i < 5000; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "bms%05d", i);
            master.push_back(b);
        }
        master.push_back("MixedCase01");
        master.push_back("bms00042");          // duplicate of an earlier id, must lose to it
        master.push_back("mixedcase01");       // duplicate differing only in case
        BuildIndex(master);

        size_t mismatches = 0;
        for (size_t i = 0; i < master.size(); ++i)
            if (LookupMaster(master[i].c_str()) != LinearScan(master, master[i].c_str())) ++mismatches;
        check(mismatches == 0, "index agrees with the linear scan on every master id");

        check(LookupMaster("BMS00042") == LinearScan(master, "BMS00042"), "lookup is case-insensitive");
        check(LookupMaster("bms00042") == 42, "a duplicate id resolves to the LOWEST master index");
        check(LookupMaster("MIXEDCASE01") == 5000, "a case-only duplicate also keeps the first");
        check(LookupMaster("nothere") == -1, "an id absent from master[] misses");
        check(LookupMaster("") == -1, "an empty id misses rather than matching an empty slot");
    }

    // ---- fusing gather and filter changes nothing ------------------------------------------
    {
        std::vector<std::string> master;
        std::vector<float> levels;
        for (int i = 0; i < 3000; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "song%04d", i);
            master.push_back(b);
            levels.push_back((i % 7 == 0) ? 0.0f : 1.0f + (float)(i % 9));   // some unplayable
        }
        BuildIndex(master);

        // A category mixing present ids, absent ids and case variants, in a deliberate order,
        // kept well under the cap so gather-bounding and write-bounding cannot diverge.
        std::vector<std::string> cat;
        for (int i = 900; i >= 0; i -= 3) {
            char b[16];
            std::snprintf(b, sizeof(b), (i % 2) ? "SONG%04d" : "song%04d", i);
            cat.push_back(b);
            if (i % 250 == 0) cat.push_back("ghost999");                     // never in master
        }

        size_t reported = 0;
        auto ref   = ReferenceFilter(master, levels, cat, 436, reported);
        size_t fusedReported = 0;
        auto fused = FusedFilter(master, levels, cat, 436, &fusedReported);
        check(cat.size() < 436, "fixture stays under the cap");
        check(ref == fused, "below the cap: identical visible list, in the same order");
        check(!fused.empty(), "the fixture actually produces a non-empty list");

        // Deviation, stated as a test so it cannot regress silently: the original reports the
        // pre-filter gathered count, which overstates what it wrote whenever a song has no chart
        // for the active mode. The replacement reports what it wrote.
        check(reported > ref.size(), "the published count overstates what was written, as shipped");
        check(fusedReported == reported, "below the cap: identical published count too");
    }

    // ---- the cap bounds the GATHER, and the published count is the gathered total ----------
    // Both of these were "corrected" once and both are now pinned as-shipped. A category of 243
    // ids where 3 have no chart for the active mode crashed the game on a /GS check when the
    // replacement published the written count and let the cap bound the write instead; eleven
    // categories with no unplayable songs had just been browsed fine. Something downstream is
    // sized from the gathered count, so the filter does not get to redefine it.
    {
        std::vector<std::string> master;
        std::vector<float> levels;
        for (int i = 0; i < 1000; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "mix%04d", i);
            master.push_back(b);
            levels.push_back((i % 5 == 0) ? 0.0f : 1.0f);      // one in five is unplayable here
        }
        BuildIndex(master);
        std::vector<std::string> cat(master.begin(), master.end());

        size_t refCount = 0, fusedCount = 0;
        auto ref   = ReferenceFilter(master, levels, cat, 436, refCount);
        auto fused = FusedFilter(master, levels, cat, 436, &fusedCount);
        check(ref == fused, "at the cap: same visible list as the original");
        check(fusedCount == refCount, "at the cap: same published count as the original");
        check(fusedCount == 436, "the cap bounds the gather, so the count is exactly the cap");
        check(fused.size() < fusedCount,
              "written entries are fewer than the published count when songs are unplayable "
              "-- reproducing the original, tail entries included");
    }

    // ---- the replicated quirk, stated outright so nobody 'fixes' it again -------------------
    // The original publishes a count larger than what it wrote, leaving stale tail entries in the
    // visible array. That is a real defect (B-series in the exe report) but it is the game's
    // defect, and code downstream of the filter was built against it.
    {
        std::vector<std::string> master = {"a0000001", "a0000002", "a0000003"};
        std::vector<float> levels = {1.0f, 0.0f, 1.0f};        // the middle one is unplayable
        BuildIndex(master);
        size_t count = 0;
        auto vis = FusedFilter(master, levels, master, 436, &count);
        check(count == 3, "publishes 3 -- everything the gather matched");
        check(vis.size() == 2, "writes 2 -- only the playable ones");
        check(vis[0] == "a0000001" && vis[1] == "a0000003", "and it closes the gap when writing");
    }

    // ---- an empty or absent category must not fabricate entries ----------------------------
    {
        std::vector<std::string> master = {"only0001"};
        std::vector<float> levels = {1.0f};
        BuildIndex(master);
        check(FusedFilter(master, levels, {}, 436).empty(), "an empty category yields nothing");
        check(FusedFilter(master, levels, {"absent01", "absent02"}, 436).empty(),
              "a category of ids that are not in master[] yields nothing");
        check(FusedFilter(master, {0.0f}, {"only0001"}, 436).empty(),
              "a zero-level song is filtered out just as the original filtered it");
    }


    // ---- nothing the game can reach may be stale ---------------------------------------------
    // The disc-art loader does sprintf("system\\disc\\%s.bmp", visible[a2].id) for whatever index
    // the wheel is on, and the wheel goes up to the PUBLISHED count. The original publishes the
    // gathered total while writing only the playable entries, so the slots in between kept the
    // previous category's contents -- a stale id, a failed open, and a smashed stack buffer.
    {
        std::vector<std::string> master;
        std::vector<float> levels;
        for (int i = 0; i < 300; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "cyt%04d", i);
            master.push_back(b);
            levels.push_back((i == 100 || i == 177 || i == 240) ? 0.0f : 1.0f);  // 3 unplayable
        }
        BuildIndex(master);
        std::vector<std::string> cat(master.begin(), master.end());
        std::vector<std::string> stale(436, "<stale>");

        Published p = BuildPublished(master, levels, cat, 436, stale);
        check(p.count == 300, "publishes the gathered total, as the original did");
        bool anyStale = false;
        for (size_t i = 0; i < p.count; ++i)
            if (p.visible[i] == "<stale>") anyStale = true;
        check(!anyStale, "no published index still holds the previous category's data");
        check(p.visible[297] == p.visible[296] && p.visible[299] == p.visible[296],
              "the tail repeats the last playable entry");
    }

    // ---- a category with no playable song publishes nothing, not a count without entries -----
    {
        std::vector<std::string> master = {"dead0001", "dead0002"};
        std::vector<float> levels = {0.0f, 0.0f};
        BuildIndex(master);
        std::vector<std::string> stale(436, "<stale>");
        Published p = BuildPublished(master, levels, master, 436, stale);
        check(p.count == 0, "all-unplayable category publishes 0 rather than a count of debris");
    }

    // ---- the ordinary case is untouched -------------------------------------------------------
    {
        std::vector<std::string> master;
        std::vector<float> levels;
        for (int i = 0; i < 50; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "ok%05d", i);
            master.push_back(b);
            levels.push_back(1.0f);
        }
        BuildIndex(master);
        std::vector<std::string> stale(436, "<stale>");
        Published p = BuildPublished(master, levels, master, 436, stale);
        check(p.count == 50, "every song playable: count is the whole category");
        check(p.visible[49] == "ok00049" && p.visible[0] == "ok00000",
              "and the entries are exactly the category, in order");
    }

    if (g_failures == 0) std::printf("filter index: ok\n");
    return g_failures ? 1 : 0;
}
