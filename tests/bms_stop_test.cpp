// #STOP, baked into tick space.
//
// The engine has no stop record -- its dispatcher knows notes, volume, BPM and markers and nothing
// else (decomp judge-timing.md, "the record dispatcher") -- so a stop is emulated by moving
// everything after it later by the stop's own length in ticks. Inserting N ticks takes exactly the
// time the stop asks for whatever the prevailing tempo is, because both sides of the conversion
// are tempo-driven, so notes, keysounds and the BGA all stay on the beat.
//
// Pinned against a real chart: BMSData\5KeyMix\COS\Bright Tunes - KOTDARI\04 Expert.bms has
// #BPM 100, #STOP01 3 and #06209:00000001 -- one stop of 6 ticks (37.5 ms) at measure 62, beat 3.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o bms_stop_test tests/bms_stop_test.cpp
//   ./bms_stop_test
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

// ---- must match bms_scanner.cpp ----------------------------------------------------------------
static const uint32_t kEzTicksPerMeasure = 384;

static uint32_t stopValueToTicks(double value) {
    if (!(value > 0.0)) return 0;
    const double ticks = value * (double)kEzTicksPerMeasure / 192.0;
    return ticks >= 4294967295.0 ? 0xffffffffu : (uint32_t)(ticks + 0.5);
}

static uint32_t stopShiftAt(const std::vector<std::pair<uint32_t, uint32_t>>& shift, uint32_t tick) {
    uint64_t moved = 0;
    for (const auto& s : shift) {
        if (s.first >= tick) break;
        moved += s.second;
    }
    const uint64_t out = (uint64_t)tick + moved;
    return out > 0xfffffffful ? 0xfffffffful : (uint32_t)out;
}

// The merge BuildStopShift does: sorted by tick, stops on the same tick added together.
static std::vector<std::pair<uint32_t, uint32_t>> build(std::vector<std::pair<uint32_t, uint32_t>> raw) {
    std::sort(raw.begin(), raw.end(),
              [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
                  return a.first < b.first;
              });
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& r : raw) {
        if (!r.second) continue;
        if (!out.empty() && out.back().first == r.first) out.back().second += r.second;
        else out.push_back(r);
    }
    return out;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-60s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

// ms a tick lasts at `bpm`: one beat is kEzTicksPerMeasure / 4 ticks.
static double tickMs(double bpm) { return 60000.0 / (bpm * (kEzTicksPerMeasure / 4)); }

int main() {
    // The unit: 192 is a whole note, which is one 4/4 measure.
    check(stopValueToTicks(192) == kEzTicksPerMeasure, "a stop of 192 is a whole measure");
    check(stopValueToTicks(48) == kEzTicksPerMeasure / 4, "a stop of 48 is one beat");
    check(stopValueToTicks(3) == 6, "the KOTDARI chart's stop of 3 is 6 ticks");
    check(stopValueToTicks(0) == 0 && stopValueToTicks(-5) == 0, "no stop, no ticks");

    // That real stop is 37.5 ms at the chart's 100 BPM.
    const double ms = 6 * tickMs(100.0);
    check(ms > 37.4 && ms < 37.6, "6 ticks at 100 BPM is 37.5 ms");

    // A stop of one beat costs exactly one beat of time, at any tempo.
    check(stopValueToTicks(48) * tickMs(100.0) > 599.9 &&
          stopValueToTicks(48) * tickMs(100.0) < 600.1, "one beat stopped at 100 BPM = 600 ms");
    check(stopValueToTicks(48) * tickMs(200.0) > 299.9 &&
          stopValueToTicks(48) * tickMs(200.0) < 300.1, "the same stop at 200 BPM = 300 ms");

    const auto shift = build({{1000, 96}, {500, 48}});

    check(stopShiftAt(shift, 0) == 0, "before every stop: unmoved");
    check(stopShiftAt(shift, 500) == 500, "a note ON the stop plays before it takes hold");
    check(stopShiftAt(shift, 501) == 549, "the next tick carries the first stop");
    check(stopShiftAt(shift, 1000) == 1048, "a note on the second stop carries only the first");
    check(stopShiftAt(shift, 2000) == 2144, "past both: both are carried");

    // Two stops on one tick add up, and the table stays sorted whatever order they arrived in.
    const auto merged = build({{1000, 96}, {500, 48}, {500, 24}});
    check(merged.size() == 2 && merged[0].first == 500 && merged[0].second == 72,
          "two stops on one tick merge");
    check(stopShiftAt(merged, 600) == 672, "the merged stop moves by the sum");

    check(stopShiftAt({}, 12345) == 12345, "a chart with no stops is untouched");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
