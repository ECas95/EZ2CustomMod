// The BPM a chart actually runs at: the tempo with the most SECONDS behind it.
//
// One rule, two callers. The scan reads the tempo track (channels 02/03/08) and stores this value
// as the chart's BPM, so the wheel, the difficulty card and the constant-speed rule all agree; the
// direct loader recomputes it from the converted chart for the song it is about to play.
//
// The declared #BPM is not always it: BMSData\5KeyMix\PIU.1\Accident - MAX\04 Expert.bms opens with
// #BPM 22.875 and switches to 183 inside the first measure, so dividing by the declared value
// pinned the scroll at 999% ("[speed] custom bpm 183.0 -> 22.9: speed 250% -> 999%"). Surveyed over
// 3000 charts of the library: 3.8% have tempo changes, 0.5% declare something they do not run at,
// 0.2% are off by more than 1.5x -- the worst being Happy Traveling (declared 118, runs at 236).
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o dominant_bpm_test tests/dominant_bpm_test.cpp
//   ./dominant_bpm_test
#include <cstdio>
#include <utility>
#include <vector>

// ---- must match bms_scanner.cpp ----------------------------------------------------------------
static double dominantBpmWeighted(double declared, const std::vector<std::pair<double, double>>& changes,
                                  double endBeat) {
    if (!(declared > 0.0) || !(endBeat > 0.0)) return declared;
    std::vector<std::pair<double, double>> weight;
    auto add = [&weight](double atBpm, double beats) {
        if (!(atBpm > 0.0) || !(beats > 0.0)) return;
        const double secs = beats * 60.0 / atBpm;
        for (auto& w : weight) if (w.first == atBpm) { w.second += secs; return; }
        weight.push_back(std::make_pair(atBpm, secs));
    };
    double bpm = declared, last = 0.0;
    for (const auto& c : changes) {
        if (c.first > endBeat) break;
        if (c.first > last) { add(bpm, c.first - last); last = c.first; }
        if (c.second > 0.0) bpm = c.second;
    }
    if (endBeat > last) add(bpm, endBeat - last);
    double best = declared, bestSecs = -1.0;
    for (const auto& w : weight) if (w.second > bestSecs) { bestSecs = w.second; best = w.first; }
    return best;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    // A chart with one tempo keeps it.
    check(dominantBpmWeighted(155.0, {}, 1200.0) == 155.0, "no tempo changes: the declared BPM stands");

    // The Accident case: 22.875 for the first measure (4 beats), 183 for the rest.
    const std::vector<std::pair<double, double>> accident{{4.0, 183.0}};
    check(dominantBpmWeighted(22.875, accident, 434.0) == 183.0,
          "Accident/Expert: 22.875 for one measure, 183 after -> 183");

    // Happy Traveling: the declared value is half the real one and never actually runs.
    const std::vector<std::pair<double, double>> doubled{{0.0, 236.0}};
    check(dominantBpmWeighted(118.0, doubled, 400.0) == 236.0,
          "a change at beat 0 replaces the declared tempo outright");

    // The opening tempo wins when it really is the one that runs.
    const std::vector<std::pair<double, double>> brief{{416.0, 240.0}};
    check(dominantBpmWeighted(150.0, brief, 434.0) == 150.0, "a change near the end does not win");

    // Weighted by time, not by beats: 10 beats at 60 BPM (10 s) beat 30 at 240 BPM (7.5 s).
    const std::vector<std::pair<double, double>> slowTail{{30.0, 60.0}};
    check(dominantBpmWeighted(240.0, slowTail, 40.0) == 60.0,
          "10 beats at 60 BPM outlast 30 beats at 240 BPM");

    // A tempo that comes back is summed across its stretches.
    const std::vector<std::pair<double, double>> alternating{{10.0, 200.0}, {20.0, 100.0},
                                                             {30.0, 200.0}, {40.0, 100.0}};
    check(dominantBpmWeighted(100.0, alternating, 50.0) == 100.0, "a tempo's stretches are summed");

    // Changes past the end of the chart are not counted.
    const std::vector<std::pair<double, double>> beyond{{10.0, 90.0}, {500.0, 300.0}};
    check(dominantBpmWeighted(120.0, beyond, 100.0) == 90.0, "a change past the end is ignored");

    // Degenerate input comes straight back.
    check(dominantBpmWeighted(0.0, accident, 434.0) == 0.0, "no declared BPM: nothing to work out");
    check(dominantBpmWeighted(155.0, accident, 0.0) == 155.0, "an empty chart keeps the declared BPM");
    const std::vector<std::pair<double, double>> zeroed{{10.0, 0.0}};
    check(dominantBpmWeighted(120.0, zeroed, 100.0) == 120.0, "a zero tempo record is ignored");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
