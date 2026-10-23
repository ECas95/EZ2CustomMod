// Folding charts onto five keys for the 5-key mode (src/lane_fold.h).
#include "../src/lane_fold.h"
#include <cassert>
#include <cstdio>

struct Ev { int measure, channel, index, div, wavId; };

static int chanAt(const std::vector<Ev>& v, int measure, int index, int div, int wav) {
    for (const Ev& e : v)
        if (e.measure == measure && e.index * div == index * e.div && e.wavId == wav) return e.channel;
    return -1;
}

int main() {
    // 1. A native 5-key chart is not touched at all.
    {
        std::vector<Ev> v = {{0, 0x11, 0, 4, 1}, {0, 0x15, 1, 4, 2}, {0, 0x13, 2, 4, 3}, {0, 0x52, 0, 4, 4},
                             {0, 0x52, 2, 4, 4}, {0, 0x12, 3, 4, 5}, {0, 0x01, 0, 1, 9}};
        const std::vector<Ev> before = v;
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(!r.folded && !r.changed() && r.overlaps == 0);
        for (size_t i = 0; i < v.size(); ++i) assert(v[i].channel == before[i].channel);
    }
    // 1b. Overlaps on a 5-key chart: a stacked tap and a tap inside a held long note move aside;
    // everything that did not collide stays put.
    {
        std::vector<Ev> v = {{0, 0x13, 0, 4, 1}, {0, 0x13, 0, 4, 2},           // two on key 3, same tick
                             {1, 0x51, 0, 4, 3}, {1, 0x51, 2, 4, 3},           // key 1 held beats 0..2
                             {1, 0x11, 1, 4, 4},                               // lands inside the hold
                             {1, 0x11, 3, 4, 5},                               // after the hold: fine
                             {1, 0x15, 1, 4, 6}};                              // unrelated: stays
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(!r.folded && r.overlaps == 2 && r.moved == 2 && r.toBackground == 0);
        assert(chanAt(v, 0, 0, 4, 1) == 0x13);
        const int moved = chanAt(v, 0, 0, 4, 2);
        assert(moved == 0x12 || moved == 0x14);                                // nearest free key
        assert(chanAt(v, 1, 1, 4, 4) == 0x12);                                 // key 1 held -> key 2
        assert(chanAt(v, 1, 3, 4, 5) == 0x11 && chanAt(v, 1, 1, 4, 6) == 0x15);
        for (const Ev& e : v) if (e.wavId == 3) assert(e.channel == 0x51);
    }
    // 1c. Six notes stacked on one tick of a 5-key chart: five keys, one background.
    {
        std::vector<Ev> v;
        for (int i = 0; i < 6; ++i) v.push_back({2, 0x13, 0, 1, 40 + i});
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.toBackground == 1 && r.overlaps == 6 - 1);
        int onKeys = 0;
        for (const Ev& e : v) if (e.channel >= 0x11 && e.channel <= 0x15) ++onKeys;
        assert(onKeys == 5);
    }
    // 2. Krazy Rain "5K": the centre keys 12-15 + 18 are kept in order on keys 1-5.
    {
        std::vector<Ev> v = {{0, 0x12, 0, 4, 1}, {0, 0x13, 1, 4, 2}, {0, 0x14, 2, 4, 3}, {0, 0x15, 3, 4, 4},
                             {1, 0x18, 0, 4, 5}, {1, 0x58, 1, 4, 6}, {1, 0x58, 3, 4, 6}};
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.folded && r.lanesBefore == 5);
        assert(chanAt(v, 0, 0, 4, 1) == 0x11 && chanAt(v, 0, 3, 4, 4) == 0x14);
        assert(chanAt(v, 1, 0, 4, 5) == 0x15);
        for (const Ev& e : v) if (e.wavId == 6) assert(e.channel == 0x55);   // LN head and tail together
        assert(r.toBackground == 0);
    }
    // 3. 7-key: every lane lands on a key; a chord of seven keeps five on keys and two as background.
    {
        std::vector<Ev> v;
        const int keys7[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19};
        for (int i = 0; i < 7; ++i) v.push_back({0, keys7[i], i, 8, 10 + i});   // a staircase: no clash
        for (int i = 0; i < 7; ++i) v.push_back({1, keys7[i], 0, 1, 20 + i});   // one 7-note chord
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.folded && r.lanesBefore == 7);
        // Staircase: 1..7 spread proportionally, left to right.
        assert(chanAt(v, 0, 0, 8, 10) == 0x11 && chanAt(v, 0, 6, 8, 16) == 0x15);
        int prev = 0x11;
        for (int i = 0; i < 7; ++i) { const int c = chanAt(v, 0, i, 8, 10 + i); assert(c >= prev && c <= 0x15); prev = c; }
        // Chord: five distinct keys, two to the background channel, none lost.
        int onKeys = 0, bg = 0; bool seen[5] = {};
        for (int i = 0; i < 7; ++i) {
            const int c = chanAt(v, 1, 0, 1, 20 + i);
            if (c == 0x01) ++bg; else { assert(c >= 0x11 && c <= 0x15 && !seen[c - 0x11]); seen[c - 0x11] = true; ++onKeys; }
        }
        assert(onKeys == 5 && bg == 2 && r.toBackground == 2);
    }
    // 4. A long note holds its key: a note that would land on it during the hold moves over.
    {
        // Key 7 (0x19) long note from beat 0 to beat 3 maps to key 5; key 5 (0x15) tap at beat 1
        // also wants key 4 or 5 -- it must not land on a held key.
        std::vector<Ev> v = {{0, 0x59, 0, 4, 1}, {0, 0x59, 3, 4, 1}, {0, 0x11, 0, 4, 2}, {0, 0x12, 1, 4, 3},
                             {0, 0x13, 1, 4, 4}, {0, 0x14, 1, 4, 5}, {0, 0x15, 1, 4, 6}, {0, 0x18, 2, 4, 7}};
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.folded);
        int lnKey = -1;
        for (const Ev& e : v) if (e.wavId == 1) { assert(e.channel >= 0x51 && e.channel <= 0x55); lnKey = e.channel - 0x40; }
        for (const Ev& e : v)
            if (e.wavId != 1 && e.measure == 0 && e.index * 4 > 0 && e.index * 4 < 3 * e.div) assert(e.channel != lnKey);
    }
    // 5. #LNOBJ: the end marker follows its note to the new key.
    {
        std::vector<Ev> v = {{0, 0x19, 0, 2, 5}, {0, 0x19, 1, 2, 99}, {0, 0x11, 0, 2, 6}, {0, 0x18, 1, 4, 7}};
        LaneFoldResult r = FoldChartToFiveKeys(v, 99);
        assert(r.folded);
        assert(chanAt(v, 0, 0, 2, 5) == chanAt(v, 0, 1, 2, 99));
    }
    // 6. pop'n 9 keys (11-15 + 22-25) fold in left-to-right order.
    {
        std::vector<Ev> v;
        const int keys9[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x22, 0x23, 0x24, 0x25};
        for (int i = 0; i < 9; ++i) v.push_back({i, keys9[i], 0, 1, 30 + i});
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.folded && r.lanesBefore == 9 && r.toBackground == 0);
        assert(chanAt(v, 0, 0, 1, 30) == 0x11 && chanAt(v, 8, 0, 1, 38) == 0x15 && chanAt(v, 4, 0, 1, 34) == 0x13);
    }
    // 7. The 5-key mode has no turntable (the shipped 5keyMix charts: 0 scratch notes in 168,643),
    // so scratch leaves the five keys too. The keys stay exactly where they are; a scratch note
    // takes the free key nearest key 1, where the scratch sits. 2P scratch is handled the same.
    {
        std::vector<Ev> v = {{0, 0x11, 0, 1, 1}, {0, 0x26, 0, 1, 2},          // key 1 busy -> key 2
                             {1, 0x16, 0, 1, 3},                              // alone -> key 1
                             {2, 0x13, 0, 1, 4}, {2, 0x16, 0, 1, 5},          // key 3 untouched
                             {3, 0x56, 0, 2, 6}, {3, 0x56, 1, 2, 6},          // scratch hold -> key 1 hold
                             {3, 0x11, 1, 4, 7}};                             // lands in it -> key 2
        LaneFoldResult r = FoldChartToFiveKeys(v, 0);
        assert(r.folded && r.toBackground == 0);
        assert(chanAt(v, 0, 0, 1, 1) == 0x11 && chanAt(v, 0, 0, 1, 2) == 0x12);
        assert(chanAt(v, 1, 0, 1, 3) == 0x11);
        assert(chanAt(v, 2, 0, 1, 4) == 0x13 && chanAt(v, 2, 0, 1, 5) == 0x11);
        for (const Ev& e : v) if (e.wavId == 6) assert(e.channel == 0x51);
        assert(chanAt(v, 3, 1, 4, 7) == 0x12);
        for (const Ev& e : v) assert(e.channel != 0x16 && e.channel != 0x26 && e.channel != 0x56);
    }
    // 7b. A 5-key chart with scratch keeps every key note where it was (no proportional spread).
    {
        std::vector<Ev> v;
        for (int k = 0; k < 5; ++k) v.push_back({k, 0x11 + k, 0, 1, 40 + k});
        v.push_back({9, 0x16, 0, 1, 49});
        FoldChartToFiveKeys(v, 0);
        for (int k = 0; k < 5; ++k) assert(chanAt(v, k, 0, 1, 40 + k) == 0x11 + k);
        assert(chanAt(v, 9, 0, 1, 49) == 0x11);
    }
    std::puts("lane_fold_test OK");
    return 0;
}
