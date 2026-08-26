// Guards the generated-chart playability rules in bms_scanner.cpp (chooseAutoLane + placement).
//
// Three ways a generated chart came out unplayable:
//   overlap      - two playable notes on the SAME lane at the SAME tick. Nothing can hit both.
//                  moveBestTo used to skip the used-this-tick check, so pedal and scratch could
//                  resolve to one track and stack.
//   jack         - the same lane hit again a moment later. The old code remembered ONE note back
//                  and its 48-point penalty was swamped by the unbounded laneUse term.
//   chained long - a long note starting on a lane just after the previous long ENDED there. The
//                  engine mishandles it: the second cannot be caught and the combo drops.
//
// chooseAutoLane is extracted verbatim from bms_scanner.cpp; the placement loop around it is
// reduced to the parts that enforce these invariants.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o lane_assign_test tests/lane_assign_test.cpp
//   ./lane_assign_test
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

static uint32_t autoHash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = a * 0x9E3779B9u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

static const uint32_t kJackGapTicks = 24;
static const uint32_t kLongChainGapTicks = 24;

// ---- verbatim from bms_scanner.cpp ----------------------------------------------------------
static int chooseAutoLane(const std::vector<int>& candidates, uint32_t tick, int sourceTrack,
                          const std::set<int>& usedAtTick, const std::array<uint32_t,64>& blockedUntil,
                          const std::array<uint64_t,64>& laneUse,
                          const std::array<uint32_t,64>& laneLastTick,
                          const std::array<bool,64>& laneEver,
                          const std::array<bool,64>& laneLastWasLong,
                          int lastLane, uint32_t seed, bool noteIsLong, int strictness) {
    int best = -1;
    uint64_t bestCost = ~uint64_t(0);
    for (size_t i = 0; i < candidates.size(); ++i) {
        int lane = candidates[i];
        if (lane < 0 || lane >= 64 || usedAtTick.find(lane) != usedAtTick.end()) continue;
        if (blockedUntil[lane] > tick) continue;
        uint32_t since = (laneEver[lane] && tick >= laneLastTick[lane]) ? tick - laneLastTick[lane] : 0xffffffffu;
        if (strictness >= 1 && noteIsLong && laneLastWasLong[lane] &&
            tick >= blockedUntil[lane] && tick - blockedUntil[lane] < kLongChainGapTicks) continue;
        if (strictness >= 2 && laneEver[lane] && since < kJackGapTicks) continue;
        uint64_t cost = laneUse[lane] * 32u;
        if (lane == lastLane) cost += 48u;
        if (laneEver[lane] && since < kJackGapTicks * 4u) cost += (kJackGapTicks * 4u - since);
        int sourcePos = sourceTrack >= 3 && sourceTrack <= 9 ? sourceTrack - 3 : 0;
        int targetPos = static_cast<int>(i);
        cost += static_cast<uint64_t>(std::abs(sourcePos - targetPos) * 3);
        cost += autoHash(tick, static_cast<uint32_t>(lane), seed) & 7u;
        if (cost < bestCost) { bestCost = cost; best = lane; }
    }
    return best;
}
// ---------------------------------------------------------------------------------------------

struct Note { uint32_t tick; int sourceTrack; uint32_t len; };
struct Placed { uint32_t tick; int lane; uint32_t len; };

// The placement loop reduced to its invariant-bearing parts, including the strictness step-down.
// minStrictness lets a test disable the rules to prove a checker really detects their violation.
static std::vector<Placed> run(const std::vector<int>& lanes, const std::vector<Note>& notes,
                               size_t& dropped, int minStrictness = 0) {
    std::array<uint32_t,64> blockedUntil{};
    std::array<uint64_t,64> laneUse{};
    std::array<uint32_t,64> laneLastTick{};
    std::array<bool,64> laneEver{};
    std::array<bool,64> laneLastWasLong{};
    std::vector<Placed> out;
    int lastLane = -1;
    dropped = 0;
    uint32_t curTick = notes.empty() ? 0 : notes.front().tick;
    std::set<int> used;
    for (const auto& n : notes) {
        if (n.tick != curTick) { used.clear(); curTick = n.tick; }
        const bool isLong = n.len > 6;
        int lane = -1;
        for (int s = 2; s >= minStrictness && lane < 0; --s)
            lane = chooseAutoLane(lanes, n.tick, n.sourceTrack, used, blockedUntil, laneUse,
                                  laneLastTick, laneEver, laneLastWasLong, lastLane, 1337u, isLong, s);
        if (lane < 0) { ++dropped; continue; }
        out.push_back({n.tick, lane, n.len});
        used.insert(lane); ++laneUse[lane];
        laneLastTick[lane] = n.tick; laneEver[lane] = true;
        laneLastWasLong[lane] = isLong;
        if (isLong) blockedUntil[lane] = n.tick + n.len;
        lastLane = lane;
    }
    return out;
}

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static bool noOverlap(const std::vector<Placed>& p) {
    std::set<std::pair<uint32_t,int> > seen;
    for (const auto& x : p) if (!seen.insert(std::make_pair(x.tick, x.lane)).second) return false;
    return true;
}

static size_t countJacks(const std::vector<Placed>& p) {
    std::array<uint32_t,64> last{};
    std::array<bool,64> ever{};
    size_t n = 0;
    for (const auto& x : p) {
        if (ever[x.lane] && x.tick > last[x.lane] && x.tick - last[x.lane] < kJackGapTicks) ++n;
        last[x.lane] = x.tick; ever[x.lane] = true;
    }
    return n;
}

// A long starting on a lane less than kLongChainGapTicks after the previous long ENDED there.
static size_t countChainedLongs(const std::vector<Placed>& p) {
    std::array<uint32_t,64> lastLongEnd{};
    std::array<bool,64> hadLong{};
    size_t n = 0;
    for (const auto& x : p) {
        bool isLong = x.len > 6;
        if (isLong && hadLong[x.lane] && x.tick >= lastLongEnd[x.lane] &&
            x.tick - lastLongEnd[x.lane] < kLongChainGapTicks) ++n;
        if (isLong) { lastLongEnd[x.lane] = x.tick + x.len; hadLong[x.lane] = true; }
    }
    return n;
}

// Nothing may be placed on a lane while a long note is still held there.
static bool noNoteInsideLong(const std::vector<Placed>& p) {
    std::array<uint32_t,64> heldUntil{};
    for (const auto& x : p) {
        if (heldUntil[x.lane] > x.tick) return false;
        if (x.len > 6) heldUntil[x.lane] = x.tick + x.len;
    }
    return true;
}

int main() {
    const std::vector<int> sevenKey = {3,4,5,6,7,8,9};
    size_t dropped = 0;

    // 16th-note stream: seven lanes and a 24-tick gap leave room, so nothing should jack or drop.
    {
        std::vector<Note> notes;
        for (uint32_t t = 0; t < 12 * 200; t += 12) notes.push_back({t, 3 + (int)((t / 12) % 5), 1});
        auto placed = run(sevenKey, notes, dropped);
        check(placed.size() == notes.size(), "16th stream: every note placed");
        check(dropped == 0, "16th stream: nothing dropped");
        check(noOverlap(placed), "16th stream: no two notes share a lane at one tick");
        check(countJacks(placed) == 0, "16th stream: no jacks inside the gap");
    }

    // Chords: five simultaneous notes per tick on seven lanes.
    {
        std::vector<Note> notes;
        for (uint32_t t = 0; t < 48 * 60; t += 48)
            for (int k = 0; k < 5; ++k) notes.push_back({t, 3 + k, 1});
        auto placed = run(sevenKey, notes, dropped);
        check(placed.size() == notes.size(), "chords: every note placed");
        check(noOverlap(placed), "chords: no overlap within a chord");
    }

    // Back-to-back longs: each ends 8 ticks before the next starts, well inside the 24-tick rest,
    // so every one of them has to move to a different lane.
    {
        std::vector<Note> notes;
        for (uint32_t t = 0; t < 48 * 120; t += 48) notes.push_back({t, 3 + (int)((t / 48) % 5), 40});
        auto placed = run(sevenKey, notes, dropped);
        check(placed.size() == notes.size(), "long chain: every long placed");
        check(dropped == 0, "long chain: nothing dropped");
        check(noNoteInsideLong(placed), "long chain: nothing placed inside a held long");
        check(countChainedLongs(placed) == 0, "long chain: no long follows a long on the same lane");
    }

    // Control for the checker itself. The zero above only means something if countChainedLongs
    // can actually see a violation, so hand it one: a long ending at 40 followed by a long starting
    // at 48 on the same lane -- 8 ticks of rest, well inside the 24 required.
    //
    // Deliberately NOT done by re-running the assigner with the rule off: the graded recency term
    // already spreads notes on its own, so an unguarded run does not reliably chain and a control
    // built that way passes for the wrong reason. The hard rule is what makes zero a guarantee
    // rather than a tendency.
    {
        std::vector<Placed> chained = { {0, 3, 40}, {48, 3, 40} };
        check(countChainedLongs(chained) == 1, "control: the checker detects a real chain");
        std::vector<Placed> spaced = { {0, 3, 40}, {80, 3, 40} };   // 40 ticks of rest, legal
        check(countChainedLongs(spaced) == 0, "control: the checker accepts an adequate rest");
        std::vector<Placed> otherLane = { {0, 3, 40}, {48, 4, 40} };
        check(countChainedLongs(otherLane) == 0, "control: a different lane is never a chain");
    }

    // Longs denser than the rest allows on two lanes: the rule yields rather than drop notes.
    {
        const std::vector<int> twoLanes = {3,4};
        std::vector<Note> notes;
        for (uint32_t t = 0; t < 32 * 100; t += 32) notes.push_back({t, 3, 24});
        auto placed = run(twoLanes, notes, dropped);
        check(placed.size() == notes.size(), "dense longs: every note still placed");
        check(dropped == 0, "dense longs: relaxation prevents drops");
        check(noOverlap(placed), "dense longs: still no overlap");
        check(noNoteInsideLong(placed), "dense longs: still nothing inside a held long");
    }

    if (g_failures == 0) std::printf("lane assign: ok\n");
    return g_failures ? 1 : 0;
}
