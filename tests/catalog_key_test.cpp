// Guards the autochart catalog lookup.
//
// catalogHasEquivalentSong used to be a full linear scan of g_songs, run once per (source song x
// target mode) pair. At 19271 songs and 8 targets that is 154168 x 19271 = ~3 BILLION _stricmp
// calls: startup sat at "100%" for minutes and looked frozen. Being quadratic, a 45% bigger
// library cost 110% more time, which is why a catalog that used to load stopped loading.
//
// It is now a hash lookup on a key built once. The only thing that matters is that it answers
// EXACTLY what the scan answered -- a false positive silently drops a song from every virtual mode,
// a false negative duplicates it. So this brute-forces both against each other.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o catalog_key_test tests/catalog_key_test.cpp
//   ./catalog_key_test
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
static int _stricmp(const char* a, const char* b) {
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#endif

struct Song { std::string mode, category, title, artist; };

// ---- the OLD implementation, kept as the oracle -------------------------------------------------
static bool scanHasEquivalent(const std::vector<Song>& catalog, const std::string& mode,
                              const Song& source) {
    for (const auto& existing : catalog) {
        if (_stricmp(existing.mode.c_str(), mode.c_str()) != 0) continue;
        if (_stricmp(existing.category.c_str(), source.category.c_str()) != 0) continue;
        if (_stricmp(existing.title.c_str(), source.title.c_str()) == 0 &&
            _stricmp(existing.artist.c_str(), source.artist.c_str()) == 0) return true;
    }
    return false;
}

// ---- the NEW implementation, copied from bms_scanner.cpp ----------------------------------------
static std::string catalogKey(const std::string& mode, const std::string& category,
                              const std::string& title, const std::string& artist) {
    std::string k;
    k.reserve(mode.size() + category.size() + title.size() + artist.size() + 4);
    for (const std::string* part : {&mode, &category, &title, &artist}) {
        for (char c : *part) k += (char)tolower((unsigned char)c);
        k += '\0';
    }
    return k;
}

static std::unordered_set<std::string> buildKeys(const std::vector<Song>& catalog) {
    std::unordered_set<std::string> keys;
    keys.reserve(catalog.size() * 2);
    for (const auto& s : catalog) keys.insert(catalogKey(s.mode, s.category, s.title, s.artist));
    return keys;
}
// -------------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

int main() {
    const char* modes[]  = {"5keyMix", "streetMix", "7keyMix", "clubMix", "spaceMix"};
    const char* cats[]   = {"ARC.1", "SIF.1", "RHY.2", "arc.1", "BD.1"};       // note the case clash
    const char* titles[] = {"Lion Heart", "Litania", "little apple", "LION HEART", "Zensoku Dreamer"};
    const char* arts[]   = {"TSAR", "SZAN", "tsar", "Se-U-Ra", ""};

    // A catalog covering every combination of the awkward cases above.
    std::vector<Song> catalog;
    unsigned seen = 0;
    for (const char* m : modes)
        for (const char* c : cats)
            for (const char* t : titles)
                for (const char* a : arts)
                    // Keep two of every three, so the catalog has holes and misses really happen.
                    // (The counter has to be independent of catalog.size(), or the first insert
                    // makes the condition permanently false and the catalog stays at one entry.)
                    if ((seen++ % 3) != 0) catalog.push_back(Song{m, c, t, a});
    std::printf("  test catalogue: %lu entries\n", (unsigned long)catalog.size());

    auto keys = buildKeys(catalog);

    // Every query the generator could make, both ways. This is the whole point of the test.
    unsigned long hits = 0, misses = 0, compared = 0;
    for (const char* c : cats)
        for (const char* t : titles)
            for (const char* a : arts) {
                    Song q{"(unused)", c, t, a};
                    for (const char* target : modes) {
                        const bool oracle = scanHasEquivalent(catalog, target, q);
                        const bool fast = keys.count(catalogKey(target, q.category, q.title, q.artist)) != 0;
                        ++compared;
                        if (oracle) ++hits; else ++misses;
                        if (oracle != fast) {
                            std::printf("FAIL: diverge for mode='%s' cat='%s' title='%s' artist='%s'"
                                        " -- scan=%d hash=%d\n", target, c, t, a, (int)oracle, (int)fast);
                            ++g_failures;
                            return 1;   // one divergence is enough; do not flood
                        }
                    }
                }
    std::printf("  %lu identical lookups (%lu hits, %lu misses)\n", compared, hits, misses);
    check(hits > 0, "the test covers lookups that DO match");
    check(misses > 0, "and lookups that do NOT match");

    // Case-insensitivity has to survive the keying, since the scan used _stricmp everywhere.
    {
        std::vector<Song> c2{{"5keyMix", "ARC.1", "Lion Heart", "TSAR"}};
        auto k2 = buildKeys(c2);
        Song q{"", "arc.1", "LION HEART", "tsar"};
        check(k2.count(catalogKey("5KEYMIX", q.category, q.title, q.artist)) != 0,
              "matches regardless of case, like _stricmp");
    }

    // The NUL separator: without it "ab"+"c" and "a"+"bc" collide and a song vanishes from a mode.
    {
        std::vector<Song> c3{{"m", "ab", "c", "x"}};
        auto k3 = buildKeys(c3);
        check(k3.count(catalogKey("m", "a", "bc", "x")) == 0,
              "fields do not run into each other (NUL separator)");
    }

    // An empty artist is real in this library and must not be confused with a missing field.
    {
        std::vector<Song> c4{{"m", "cat", "title", ""}};
        auto k4 = buildKeys(c4);
        check(k4.count(catalogKey("m", "cat", "title", "")) != 0, "an empty artist matches itself");
        check(k4.count(catalogKey("m", "cat", "title", "x")) == 0, "and not a non-empty one");
    }

    if (g_failures == 0) std::printf("catalog key: ok\n");
    return g_failures ? 1 : 0;
}
