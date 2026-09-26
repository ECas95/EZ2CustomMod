#pragma once
// Making a BMS chart playable on five keys, for the 5-key source mode. Two jobs, one pass:
//
// FOLDING. Packs ship 7-key (and pop'n / double) charts next to their 5-key ones, and some "5K"
// charts use the five CENTRE keys of the 7-key layout (Krazy Rain: channels 12-15 + 18). The 5-key
// mode maps channels 11-15 to its keys and nothing else, so everything on 18/19 or the 2P side was
// either a pedal note nobody authored or unplayable.
//   - Five lanes or fewer, just not 11-15: kept in their left-to-right order on keys 1-5.
//   - More than five: each lane goes to its proportional place on the five keys.
//   - Scratch / free zone (16/17, and the 2P 26/27) too: the 5-key mode has no turntable (the
//     shipped 5keyMix charts hold 0 scratch notes in 168,643), so a scratch note was a note on a
//     lane nobody could see or hit. It takes the free key nearest key 1, beside which the scratch
//     sits; the key notes are not spread to make room.
//
// OVERLAPS. Converted charts carry notes stacked on one key: two on the same tick, or one landing
// while a long note on that key is still held. Neither can be played. Every note keeps its own key
// when it is free; one that collides takes the nearest free key.
//
// Either way a chord wider than five keys cannot fit: the leftover notes still sound, as
// background. A chart that is on the five keys with nothing stacked comes out exactly as written,
// and the BMS file itself is never touched.
//
// Long notes are both forms: LNTYPE 1 pairs on 5x/6x, and #LNOBJ ends on the note channel. A long
// note's end always follows its head. The event type only needs measure, index, div, channel and
// wavId, so the scanner's own events are rewritten in place and the test can use its own.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "ln_pairing.h"

struct LaneFoldResult {
    bool folded = false;       // the chart used keys outside 1-5 and was spread onto them
    int lanesBefore = 0;       // distinct key lanes it used
    int moved = 0;             // notes that changed key (folding, or an overlap moved aside)
    int overlaps = 0;          // notes that collided on a key a 5-key chart already had
    int toBackground = 0;      // notes with no free key (a chord wider than five)
    bool changed() const { return folded || moved || toBackground; }
};

namespace lanefold {
// Keys in their physical left-to-right order: 1P keys 1-7, then the 2P side (pop'n 9-key is
// 11-15 + 22-25, and so lands in this order too). The scratch lanes come last, outside that order:
// they are placed by kScratchFirst, not by position.
static const int kKeyOrder[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19,
                                0x21, 0x22, 0x23, 0x24, 0x25, 0x28, 0x29, 0x16, 0x17};
static const int kLanes = 16, kScratchFirst = 14;
inline int keyIndex(int base) {
    for (int i = 0; i < kLanes; ++i) if (kKeyOrder[i] == base) return i;
    return -1;
}
inline int baseOf(int ch) { return (ch >= 0x51 && ch <= 0x69) ? ch - 0x40 : ch; }
inline bool isLnChannel(int ch) { return ch >= 0x51 && ch <= 0x69; }
// Position inside the chart as an integer: exact for every division that divides 57657600
// (every power of two to 256, times 3, 5, 7, 9, 11, 13 -- all real BMS grids).
inline long long posOf(int measure, int index, int div) {
    const long long unit = 57657600LL;
    return (long long)measure * unit + (div > 0 ? (long long)index * unit / div : 0);
}
}  // namespace lanefold

template <class Event>
LaneFoldResult FoldChartToFiveKeys(std::vector<Event>& events, int lnObjId) {
    using namespace lanefold;
    LaneFoldResult r;
    for (Event& e : events) {                                  // 2P scratch onto the 1P one
        const int b = baseOf(e.channel);
        if (b == 0x26 || b == 0x27) e.channel -= 0x10;
    }
    bool used[kLanes] = {};
    bool outside = false, keysOutside = false;   // any lane off keys 1-5 / a KEY lane off them
    for (const Event& e : events) {
        const int k = keyIndex(baseOf(e.channel));
        if (k < 0) continue;
        used[k] = true;
        if (k >= 5) outside = true;
        if (k >= 5 && k < kScratchFirst) keysOutside = true;
    }
    for (bool u : used) r.lanesBefore += u ? 1 : 0;
    r.folded = outside;

    std::vector<int> lanes;                      // key indexes in use, left to right, scratch aside
    for (int k = 0; k < kScratchFirst; ++k) if (used[k]) lanes.push_back(k);
    const int n = (int)lanes.size();
    // Where a note of this lane wants to be, 0..4. On a 5-key chart that is simply its own key.
    auto target = [&](int keyIdx) -> double {
        if (keyIdx >= kScratchFirst) return 0.0;
        if (!keysOutside) return (double)keyIdx;
        const int i = (int)(std::find(lanes.begin(), lanes.end(), keyIdx) - lanes.begin());
        return n <= 5 ? (double)i : (double)i * 4.0 / (double)(n - 1);
    };

    // Notes in time order: a head, and its end if it is a long note.
    struct Note { long long pos, end; size_t head; long long tail; int key; };
    std::vector<Note> notes;
    std::vector<long long> open(kLanes, -1);         // LNTYPE 1: index into notes of an open head
    std::vector<long long> last(kLanes, -1);         // #LNOBJ: last note on the lane
    std::vector<size_t> order;
    for (size_t i = 0; i < events.size(); ++i)
        if (keyIndex(baseOf(events[i].channel)) >= 0) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return posOf(events[a].measure, events[a].index, events[a].div) <
               posOf(events[b].measure, events[b].index, events[b].div);
    });
    // Which LNTYPE 1 events are heads and tails: ln_pairing.h, per key, so a lane that lost one
    // event does not come out inverted -- and so this and the chart converter read it the same way.
    std::vector<int> lnRole(events.size(), kLnAlone);
    for (int k = 0; k < kLanes; ++k) {
        std::vector<long long> lnPos, tapPos;
        std::vector<size_t> lnIdx;
        for (size_t i : order) {
            const Event& e = events[i];
            if (keyIndex(baseOf(e.channel)) != k) continue;
            const long long p = posOf(e.measure, e.index, e.div);
            if (isLnChannel(e.channel)) { lnPos.push_back(p); lnIdx.push_back(i); }
            else if (!(lnObjId > 0 && e.wavId == lnObjId)) tapPos.push_back(p);
        }
        if (lnIdx.empty()) continue;
        const std::vector<int> roles = PairLongNotes(lnPos, tapPos);
        for (size_t j = 0; j < lnIdx.size(); ++j) lnRole[lnIdx[j]] = roles[j];
    }
    for (size_t i : order) {
        const Event& e = events[i];
        const int key = keyIndex(baseOf(e.channel));
        const long long pos = posOf(e.measure, e.index, e.div);
        if (isLnChannel(e.channel)) {
            if (lnRole[i] == kLnTail && open[key] >= 0) {   // the end of the open long note
                Note& h = notes[(size_t)open[key]];
                h.end = pos; h.tail = (long long)i;
                open[key] = -1;
                continue;
            }
            // A head opens a hold; a lone event is an ordinary note (it keeps its 5x channel,
            // which the converter reads with the same pairing).
            open[key] = lnRole[i] == kLnHead ? (long long)notes.size() : -1;
        } else if (lnObjId > 0 && e.wavId == lnObjId && last[key] >= 0 &&
                   notes[(size_t)last[key]].tail < 0) {
            Note& h = notes[(size_t)last[key]];
            h.end = pos; h.tail = (long long)i;
            continue;
        }
        last[key] = (long long)notes.size();
        notes.push_back({pos, pos, i, -1, key});
    }

    // Assign. busy[k] is the position through which key k is held: a note on the same tick, or
    // anywhere up to and including the end of a long note, collides.
    long long busy[5] = {-1, -1, -1, -1, -1};
    std::vector<bool> drop(events.size(), false);
    for (const Note& nt : notes) {
        const double t = target(nt.key);
        int order5[5] = {0, 1, 2, 3, 4};
        std::stable_sort(order5, order5 + 5, [&](int a, int b) {
            return std::fabs(a - t) < std::fabs(b - t);
        });
        int lane = -1;
        for (int c : order5) if (busy[c] < nt.pos) { lane = c; break; }
        // On a 5-key chart the first choice is the note's own key; not getting it is an overlap.
        if (!outside && (lane < 0 || lane != nt.key)) ++r.overlaps;
        Event& head = events[nt.head];
        const bool ln = nt.tail >= 0;
        if (lane < 0) {                           // wider than five keys: keep the sound
            head.channel = 0x01;
            if (ln) drop[(size_t)nt.tail] = true;
            ++r.toBackground;
            continue;
        }
        busy[lane] = nt.end;
        head.channel = (isLnChannel(head.channel) ? 0x51 : 0x11) + lane;
        if (ln) {
            Event& tail = events[(size_t)nt.tail];
            tail.channel = (isLnChannel(tail.channel) ? 0x51 : 0x11) + lane;
        }
        if (nt.key != lane) ++r.moved;
    }
    if (r.toBackground) {
        std::vector<Event> kept;
        kept.reserve(events.size());
        for (size_t i = 0; i < events.size(); ++i) if (!drop[i]) kept.push_back(events[i]);
        events.swap(kept);
    }
    return r;
}
