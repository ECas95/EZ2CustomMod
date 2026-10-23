// The BGA start time, in the units the events are actually in.
//
// directTickToMilliseconds divided by a hardcoded 48 ticks per beat -- the NATIVE grid's
// 192-per-measure figure -- while every tick it is given comes from bmsTickToEzTick on the
// kEzTicksPerMeasure = 384 grid. Every custom song's video therefore started exactly twice as
// late as its #BGA event, which is the whole of "the BGA does not follow the BMS offset".
//
// Pinned against two readings from the log of 'Accident - MAX', both with startTick=365:
//   03 Hard.bms,   183 BPM    -> was 2493 ms, should be 1247
//   04 Expert.bms, 22.875 BPM -> was 19945 ms, should be 9973
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o bga_offset_test tests/bga_offset_test.cpp
//   ./bga_offset_test
#include <cstdint>
#include <cstdio>
#include <vector>

// ---- must match bms_scanner.cpp ----------------------------------------------------------------
static const uint32_t kEzTicksPerMeasure = 384;
static const double kDirectTicksPerBeat = (double)kEzTicksPerMeasure / 4.0;

struct Tempo { uint32_t tick; double bpm; };   // stands in for the type-3 events of track 0

static uint32_t directTickToMilliseconds(uint32_t targetTick, double baseBpm,
                                         const std::vector<Tempo>& timing) {
    if (!targetTick) return 0;
    double bpm = baseBpm > 0.0 ? baseBpm : 120.0;
    double ms = 0.0;
    uint32_t lastTick = 0;
    for (const auto& event : timing) {
        if (event.tick > targetTick) break;
        if (event.tick > lastTick) {
            ms += (double)(event.tick - lastTick) * 60000.0 / (bpm * kDirectTicksPerBeat);
            lastTick = event.tick;
        }
        if (event.bpm > 0.0) bpm = event.bpm;
    }
    if (targetTick > lastTick)
        ms += (double)(targetTick - lastTick) * 60000.0 / (bpm * kDirectTicksPerBeat);
    return (uint32_t)(ms + 0.5);
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    const std::vector<Tempo> none;

    // One whole measure at 120 BPM is two seconds, whatever the grid.
    check(directTickToMilliseconds(kEzTicksPerMeasure, 120.0, none) == 2000,
          "one measure at 120 BPM = 2000 ms");
    // A beat is a beat: 96 ticks on this grid.
    check(directTickToMilliseconds(96, 120.0, none) == 500, "one beat at 120 BPM = 500 ms");

    check(directTickToMilliseconds(365, 183.0, none) == 1247, "Accident/Hard: tick 365 at 183 BPM");
    check(directTickToMilliseconds(365, 22.875, none) == 9973, "Accident/Expert: tick 365 at 22.875 BPM");

    // A tempo change before the target is honoured: half a measure at 120, half at 240.
    std::vector<Tempo> change{{192, 240.0}};
    check(directTickToMilliseconds(384, 120.0, change) == 1500, "120 then 240 BPM mid-measure");
    // A change AFTER the target must not count.
    std::vector<Tempo> later{{300, 240.0}};
    check(directTickToMilliseconds(192, 120.0, later) == 1000, "a later tempo change is ignored");

    check(directTickToMilliseconds(0, 183.0, none) == 0, "tick 0 needs no delay");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
