#pragma once
// Pairing BMS LNTYPE 1 long-note events into heads and tails.
//
// LNTYPE 1 marks a long note with two events on its lane's 5x channel, head then tail, and a
// player simply alternates. One missing or extra event -- a converter that dropped a zero-length
// hold, or wrote two onto one slot -- and every long note after it on that lane is inverted: each
// tail becomes the head of a hold that spans the gap to the next note, and the notes in that gap
// land inside it. The library audit found this in hundreds of charts (O2J, RHY, SIF, BLP, DJM...).
//
// A head and its tail are always consecutive events on their lane -- nothing else of the 5x
// channel can sit inside a hold -- so the best reading is a short dynamic programme over the
// lane's events: each is either paired with the next one, or left on its own (played as a tap).
// A pair that encloses one of the lane's ordinary notes costs 1000, a lone event 1500.
// A well-formed lane comes out exactly as plain alternation would read it, and a broken one loses
// one event instead of everything after it.
#include <algorithm>
#include <cstddef>
#include <vector>

enum LnRole { kLnHead = 0, kLnTail = 1, kLnAlone = 2 };

// lnTicks: the lane's 5x events in time order. tapTicks: its ordinary notes, sorted.
// Returns one role per long-note event.
template <class T>
std::vector<int> PairLongNotes(const std::vector<T>& lnTicks, const std::vector<T>& tapTicks) {
    const size_t n = lnTicks.size();
    // Letting an event go costs more than one enclosed note: a single note inside a genuine hold is
    // an overlap (lane_fold.h moves it aside), not a reason to break the hold. An inverted lane
    // encloses notes in hold after hold, and one lone event is far cheaper than all of them.
    const long long kInside = 1000, kAlone = 1500;
    auto inside = [&](T a, T b) -> long long {      // ordinary notes strictly within (a, b)
        auto lo = std::upper_bound(tapTicks.begin(), tapTicks.end(), a);
        auto hi = std::lower_bound(tapTicks.begin(), tapTicks.end(), b);
        return hi > lo ? (long long)(hi - lo) : 0;
    };
    std::vector<long long> cost(n + 1, 0);
    std::vector<int> pick(n + 1, 0);                  // 1 = alone, 2 = pair, taken at i
    // Backwards, so ties resolve toward pairing from the start of the lane (plain alternation).
    for (size_t k = n; k-- > 0;) {
        long long best = cost[k + 1] + kAlone;
        int how = 1;
        if (k + 1 < n) {
            const long long pair = cost[k + 2] + kInside * inside(lnTicks[k], lnTicks[k + 1]);
            if (pair <= best) { best = pair; how = 2; }
        }
        cost[k] = best;
        pick[k] = how;
    }
    std::vector<int> role(n, kLnAlone);
    for (size_t k = 0; k < n;) {
        if (pick[k] == 2) { role[k] = kLnHead; role[k + 1] = kLnTail; k += 2; }
        else { role[k] = kLnAlone; k += 1; }
    }
    return role;
}
