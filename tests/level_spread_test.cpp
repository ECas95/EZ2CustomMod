// One level per chart, within a song.
//
// The measurement can land two charts of a song on the same number -- 13 and 13 when one is a hair
// harder -- and a difficulty column reading 11/13/13/13 tells the player nothing. The charts are
// already in difficulty order (by the blended score, then by note count: the lighter chart is the
// easier one), so the levels are walked in that order and each is pushed at least one above the
// chart below it. Running off the top slides the whole set down instead of piling onto 20.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o level_spread_test tests/level_spread_test.cpp
//   ./level_spread_test
#include <cstdio>
#include <string>
#include <vector>

// ---- must match AssignSongBinDifficultyFields in bms_scanner.cpp --------------------------------
static std::vector<int> spread(std::vector<int> lv) {
    const size_t count = lv.size();
    for (size_t i = 1; i < count; ++i) if (lv[i] <= lv[i - 1]) lv[i] = lv[i - 1] + 1;
    if (count && lv[count - 1] > 20) {
        const int over = lv[count - 1] - 20;
        for (size_t i = 0; i < count; ++i) lv[i] -= over;
    }
    for (size_t i = 0; i < count; ++i) if (lv[i] < 1) lv[i] = 1;
    for (size_t i = 1; i < count; ++i) if (lv[i] <= lv[i - 1]) lv[i] = lv[i - 1] + 1;
    for (size_t i = 0; i < count; ++i) if (lv[i] > 20) lv[i] = 20;
    return lv;
}

static int failures = 0;
static std::string show(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += "/"; s += std::to_string(v[i]); }
    return s.empty() ? "-" : s;
}
static void check(const std::vector<int>& in, const std::vector<int>& want, const char* what) {
    const std::vector<int> got = spread(in);
    const bool ok = got == want;
    std::printf("%-52s %-18s %s\n", what, show(got).c_str(), ok ? "ok" : "FAILED");
    if (!ok) { std::printf("   wanted %s from %s\n", show(want).c_str(), show(in).c_str()); ++failures; }
}
static void checkTrue(bool ok, const char* what) {
    std::printf("%-52s %-18s %s\n", what, "", ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    // What the screenshots showed.
    check({13, 13, 13, 13}, {13, 14, 15, 16}, "four charts all rated 13");
    check({11, 13, 13, 13}, {11, 13, 14, 15}, "NM apart, the other three tied");
    check({15, 15, 15, 15}, {15, 16, 17, 18}, "four charts all rated 15");

    // Already distinct: left exactly as measured.
    check({5, 10, 15, 18}, {5, 10, 15, 18}, "distinct levels are not touched");
    check({1, 2, 3, 4}, {1, 2, 3, 4}, "already minimal and distinct");

    // The top of the scale: slide down, never pile up on 20.
    check({20, 20, 20, 20}, {17, 18, 19, 20}, "four charts all at the ceiling");
    check({19, 20, 20, 20}, {17, 18, 19, 20}, "three at the ceiling");
    check({20, 20, 20, 20, 20}, {16, 17, 18, 19, 20}, "five charts at the ceiling");
    check({18, 19, 20, 20}, {17, 18, 19, 20}, "one over the top slides the set");

    // The bottom of the scale.
    check({1, 1, 1, 1}, {1, 2, 3, 4}, "four charts all at the floor");
    check({1, 1, 1, 1, 1}, {1, 2, 3, 4, 5}, "five charts all at the floor");

    // Degenerate sizes.
    check({}, {}, "a song with no charts");
    check({7}, {7}, "a single chart keeps its level");
    check({9, 9}, {9, 10}, "two tied charts");

    // The properties that matter, over every shape a song can take.
    bool distinct = true, ordered = true, inRange = true;
    for (int base = 1; base <= 20; ++base) {
        for (int n = 1; n <= 5; ++n) {
            std::vector<int> in((size_t)n, base);
            const std::vector<int> got = spread(in);
            for (size_t i = 0; i < got.size(); ++i) {
                if (got[i] < 1 || got[i] > 20) inRange = false;
                if (i && got[i] <= got[i - 1]) { distinct = false; ordered = false; }
            }
        }
    }
    checkTrue(distinct, "no two charts ever share a level");
    checkTrue(ordered, "levels always rise with difficulty");
    checkTrue(inRange, "every level stays within 1..20");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
