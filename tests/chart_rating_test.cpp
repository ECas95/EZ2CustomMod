// Rating a chart from its own notes instead of trusting #PLAYLEVEL.
//
// Measured over 1379 charts of this library: the declared level tracks the note data up to 17 and
// then stops meaning anything. 46% of songs call their hardest chart 18, and inside that one level
// the busiest two seconds run from 13 to 83.5 notes a second -- a six-fold spread under one number.
// Peak density is the measurement that keeps rising through it; jacks are the one independent
// signal on top (0% of notes at level 2, 15.5% at 18).
//
// Re-checked over 400 songs after this curve: the level of a song's hardest chart put 45.8% of the
// library on 18 before and 14.0% on its worst bucket after, over 14 levels instead of 11.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o chart_rating_test tests/chart_rating_test.cpp
//   ./chart_rating_test
#include <cstdint>
#include <cstdio>

// ---- must match bms_scanner.cpp ----------------------------------------------------------------
struct ChartStats {
    uint32_t notes = 0;
    float    seconds = 0.0f;
    float    nps = 0.0f;
    float    peakNps = 0.0f;
    float    chordPct = 0.0f;
    float    jackPct = 0.0f;
    int      rated = 0;
    bool measured() const { return notes > 0 && seconds > 0.0f; }
};

static const float kPeakForLevel[] = {
    0.0f,  1.0f,  2.00f, 2.80f, 3.60f, 4.50f, 6.40f, 7.50f, 9.10f, 10.10f,
    11.00f, 11.90f, 12.80f, 13.90f, 14.80f, 15.70f, 16.60f, 19.50f,
    24.00f, 30.00f, 38.00f
};

static const float kBurstWeight = 0.8f;

static float chartScore(const ChartStats& st) {
    return kBurstWeight * st.peakNps + (1.0f - kBurstWeight) * st.nps * 2.6f;
}

static int rateChartFromStats(const ChartStats& st) {
    if (!st.measured()) return 0;
    const float score = chartScore(st);
    const int top = (int)(sizeof(kPeakForLevel) / sizeof(kPeakForLevel[0])) - 1;
    float level;
    if (score <= kPeakForLevel[1]) {
        level = 1.0f;
    } else if (score >= kPeakForLevel[top]) {
        level = (float)top;
    } else {
        level = 1.0f;
        for (int i = 1; i < top; ++i) {
            if (score >= kPeakForLevel[i] && score < kPeakForLevel[i + 1]) {
                const float span = kPeakForLevel[i + 1] - kPeakForLevel[i];
                level = (float)i + (span > 0.01f ? (score - kPeakForLevel[i]) / span : 0.0f);
                break;
            }
        }
    }
    if (st.jackPct >= 12.0f) level += 1.0f;
    int out = (int)(level + 0.5f);
    if (out < 1) out = 1;
    if (out > 20) out = 20;
    return out;
}

// A chart whose blended score comes out at `score`, with peak and sustained density in the
// proportion the library shows (nps*2.6 ~= peak), so these cases read as real charts.
static ChartStats made(float score, float jack = 0.0f) {
    ChartStats st;
    st.notes = 500; st.seconds = 100.0f;
    st.peakNps = score;
    st.nps = score / 2.6f;
    st.jackPct = jack;
    return st;
}

// Two charts of one song: the same climax, different sustained density. This is the case peak
// alone could not separate -- and the reason the column read 15/15/15/15.
static ChartStats sharedBurst(float peak, float nps) {
    ChartStats st;
    st.notes = 500; st.seconds = 100.0f;
    st.peakNps = peak; st.nps = nps;
    return st;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    // Nothing measured, nothing said: the caller falls back to #PLAYLEVEL.
    ChartStats empty;
    check(rateChartFromStats(empty) == 0, "an unmeasured chart is not rated");
    ChartStats noTime = made(10.0f); noTime.seconds = 0.0f;
    check(rateChartFromStats(noTime) == 0, "a chart with no span is not rated");

    // The curve reproduces the level each density was measured at.
    check(rateChartFromStats(made(2.00f)) == 2, "peak 2.0 -> level 2");
    check(rateChartFromStats(made(9.10f)) == 8, "peak 9.1 -> level 8");
    check(rateChartFromStats(made(14.80f)) == 14, "peak 14.8 -> level 14, the step that had collapsed");
    check(rateChartFromStats(made(19.50f)) == 17, "peak 19.5 -> level 17");

    // Monotone, and every level between 1 and 20 is reachable.
    int last = 0;
    bool monotone = true;
    for (float peak = 0.5f; peak < 45.0f; peak += 0.05f) {
        const int lv = rateChartFromStats(made(peak));
        if (lv < last) monotone = false;
        last = lv;
    }
    check(monotone, "more notes never means a lower level");
    bool reachable = true;
    for (int want = 1; want <= 20; ++want) {
        bool found = false;
        for (float peak = 0.5f; peak < 45.0f && !found; peak += 0.05f)
            if (rateChartFromStats(made(peak)) == want) found = true;
        if (!found) { reachable = false; std::printf("   level %d is unreachable\n", want); }
    }
    check(reachable, "every level 1..20 can be reached");

    // The wall the packs build at 18 gets spread out.
    check(rateChartFromStats(made(13.0f)) < rateChartFromStats(made(19.0f)) &&
          rateChartFromStats(made(19.0f)) < rateChartFromStats(made(28.0f)),
          "the declared-18 range (13 / 19 / 28 peak) separates");
    check(rateChartFromStats(made(83.5f)) == 20, "the hardest chart measured tops the scale");

    // Jacks are worth a level once they reach the top decile of what was measured.
    check(rateChartFromStats(made(12.0f, 15.0f)) == rateChartFromStats(made(12.0f)) + 1,
          "a heavily jacked chart rates one higher");
    check(rateChartFromStats(made(12.0f, 11.9f)) == rateChartFromStats(made(12.0f)),
          "just under the jack threshold changes nothing");
    check(rateChartFromStats(made(38.0f, 20.0f)) == 20, "the jack bonus cannot push past 20");

    // The within-song case: one song's Easy and Expert share a burst but not the rest of the chart.
    check(rateChartFromStats(sharedBurst(16.0f, 2.0f)) < rateChartFromStats(sharedBurst(16.0f, 6.2f)),
          "the same burst with thinner filler rates lower");
    check(chartScore(sharedBurst(16.0f, 6.2f)) > chartScore(sharedBurst(16.0f, 2.0f)),
          "sustained density is part of the score");
    check(chartScore(sharedBurst(20.0f, 1.0f)) > chartScore(sharedBurst(10.0f, 5.0f)),
          "the burst still dominates the blend");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
