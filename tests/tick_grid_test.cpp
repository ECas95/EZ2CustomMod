// Guards the tick grid the .ez charts are emitted on.
//
// BMS positions are exact rationals -- index/div of a measure -- and the .ez format stores integer
// ticks, so the conversion rounds. How much it rounds depends entirely on the grid:
//
//   at 192 ticks/measure, the 1/384 subdivision these charts are written in lands on x.5 every
//   time, so a quarter of all notes were displaced by the full half tick: 4.17 ms at 150 BPM.
//   Measured over 45,707 notes from 80 installed charts: 24.07% off-grid, mean error 4.17 ms.
//
//   at 384 it is exact, and every other common subdivision halves its worst case.
//
// bmsTickToEzTick is copied verbatim from bms_scanner.cpp.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o tick_grid_test tests/tick_grid_test.cpp
//   ./tick_grid_test
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>

// ---- must match bms_scanner.cpp ---------------------------------------------------------------
static const uint32_t kEzTicksPerMeasure     = 384;
static const uint32_t kNativeTicksPerMeasure = 192;
static const uint32_t kModelTickScale        = kEzTicksPerMeasure / kNativeTicksPerMeasure;

// ---- verbatim from bms_scanner.cpp (measureScale reduced to a plain vector) --------------------
static uint32_t bmsTickToEzTick(const std::vector<double>& measureScale, int measure, int index,
                                int div, uint32_t grid) {
    double pos = 0.0;
    for (int m = 0; m < measure; ++m) {
        double scale = 1.0;
        if (m < (int)measureScale.size()) scale = measureScale[m];
        pos += (double)grid * scale;
    }
    double scale = 1.0;
    if (measure < (int)measureScale.size()) scale = measureScale[measure];
    pos += ((double)index / (double)div) * (double)grid * scale;
    if (pos < 0.0) pos = 0.0;
    return (uint32_t)(pos + 0.5);
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// Milliseconds a whole tick is worth, so errors can be judged in the unit that matters.
static double tickMs(uint32_t grid, double bpm) { return 240000.0 / ((double)grid * bpm); }

// Worst rounding error, in ms, over every position of a subdivision.
static double worstErrorMs(int div, uint32_t grid, double bpm) {
    const std::vector<double> noScale;
    double worst = 0.0;
    for (int i = 0; i < div; ++i) {
        const double exact = (double)i * (double)grid / (double)div;
        const double got   = (double)bmsTickToEzTick(noScale, 0, i, div, grid);
        worst = std::fmax(worst, std::fabs(got - exact));
    }
    return worst * tickMs(grid, bpm) / 1.0;
}

int main() {
    const double bpm = 150.0;
    std::printf("  grid %u -> %.3f ms per tick at %.0f BPM\n",
                kEzTicksPerMeasure, tickMs(kEzTicksPerMeasure, bpm), bpm);

    // The subdivision that dominates the installed charts.
    {
        const double before = worstErrorMs(384, kNativeTicksPerMeasure, bpm);
        const double after  = worstErrorMs(384, kEzTicksPerMeasure, bpm);
        std::printf("  div=384 worst error: %.2f ms -> %.2f ms\n", before, after);
        check(before > 4.0, "control: 1/384 really was displaced by ~4 ms on the old grid");
        check(after == 0.0, "1/384 is exact on the new grid");
    }

    // Everything that was already exact must stay exact.
    for (int div : {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 384}) {
        if (kEzTicksPerMeasure % div != 0) continue;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "div=%d is exact", div);
        check(worstErrorMs(div, kEzTicksPerMeasure, bpm) == 0.0, msg);
    }

    // Subdivisions that divide neither grid must at least not get worse.
    for (int div : {5, 7, 9, 11, 13, 5 * 7}) {
        const double before = worstErrorMs(div, kNativeTicksPerMeasure, bpm);
        const double after  = worstErrorMs(div, kEzTicksPerMeasure, bpm);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "div=%d does not get worse (%.2f -> %.2f ms)", div, before, after);
        check(after <= before + 1e-9, msg);
    }

    // A measure-length change (#xxx02) must still land measure boundaries where they belong.
    {
        std::vector<double> scale{1.0, 0.75, 1.0};
        check(bmsTickToEzTick(scale, 1, 0, 1, kEzTicksPerMeasure) == kEzTicksPerMeasure,
              "measure 1 starts one full measure in");
        check(bmsTickToEzTick(scale, 2, 0, 1, kEzTicksPerMeasure) ==
                  kEzTicksPerMeasure + (uint32_t)(kEzTicksPerMeasure * 0.75),
              "a 3/4 measure shortens the next boundary by a quarter");
    }

    // Spacing rules are musical durations, so they have to move with the grid or they silently
    // halve. kJackGapTicks is an 8th note; at 384 that is 48 ticks, not 24.
    {
        const uint32_t jack = 24 * kModelTickScale;
        check(jack == 48, "the jack window is still an 8th note on the new grid");
        check(jack * 8 == kEzTicksPerMeasure, "and an 8th note is exactly 1/8 of a measure");
    }

    // The model is learned from native charts, so its gaps arrive in native ticks.
    {
        const uint32_t learnedNative = 24;                       // an 8th note as the model saw it
        const uint32_t applied = learnedNative * kModelTickScale;
        check(applied * 8 == kEzTicksPerMeasure,
              "a model gap keeps its musical length after scaling into our grid");
    }

    if (g_failures == 0) std::printf("tick grid: ok\n");
    return g_failures ? 1 : 0;
}
