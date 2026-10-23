// The density profile the song-select graph draws.
//
// The chart's span is split into kDensityBuckets equal slices and each is scaled against the
// busiest one, so every chart fills the graph and what reads is the SHAPE -- where the bursts are,
// how long the quiet stretch runs. The absolute figures are on the panel beside it.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o density_graph_test tests/density_graph_test.cpp
//   ./density_graph_test
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

static const int kDensityBuckets = 64;

// ---- must match measureChart() in bms_scanner.cpp -----------------------------------------------
static void buildDensity(const std::vector<double>& times, unsigned char out[kDensityBuckets]) {
    for (int i = 0; i < kDensityBuckets; ++i) out[i] = 0;
    if (times.size() < 2) return;
    const double span = times.back() - times.front();
    if (!(span > 0.5)) return;
    uint32_t hits[kDensityBuckets] = {};
    const double t0 = times.front();
    for (double t : times) {
        int b = (int)((t - t0) / span * kDensityBuckets);
        if (b < 0) b = 0;
        if (b >= kDensityBuckets) b = kDensityBuckets - 1;
        ++hits[b];
    }
    uint32_t busiest = 0;
    for (uint32_t h : hits) if (h > busiest) busiest = h;
    if (!busiest) return;
    for (int b = 0; b < kDensityBuckets; ++b)
        out[b] = (unsigned char)((hits[b] * 255u + busiest / 2) / busiest);
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    unsigned char d[kDensityBuckets];

    // An even chart fills every bar to the top.
    {
        std::vector<double> t;
        for (int i = 0; i < 640; ++i) t.push_back(i * 0.25);     // 160 s, 10 per bucket
        buildDensity(t, d);
        bool flat = true;
        for (int i = 0; i < kDensityBuckets; ++i) if (d[i] != 255) flat = false;
        check(flat, "a chart of even density fills every bar");
    }

    // A burst in the middle: the burst tops out, the rest sits well below it.
    {
        std::vector<double> t;
        for (int i = 0; i < 100; ++i) t.push_back(i * 1.0);       // 100 s at 1/s
        for (int i = 0; i < 200; ++i) t.push_back(50.0 + i * 0.01);
        std::sort(t.begin(), t.end());
        buildDensity(t, d);
        check(d[kDensityBuckets / 2] == 255, "the busiest slice is the full height");
        check(d[2] > 0 && d[2] < 60, "a quiet slice is short but still drawn");
    }

    // Silence at the end reads as empty bars, not as a shorter graph.
    {
        std::vector<double> t;
        for (int i = 0; i < 100; ++i) t.push_back(i * 0.1);       // all inside the first 10 s
        t.push_back(40.0);                                        // one lonely note at the end
        buildDensity(t, d);
        check(d[0] == 255, "the dense opening tops out");
        int empty = 0;
        for (int i = kDensityBuckets / 2; i < kDensityBuckets - 1; ++i) if (d[i] == 0) ++empty;
        check(empty > 20, "the silent stretch stays empty");
        check(d[kDensityBuckets - 1] > 0, "the last note still shows");
    }

    // Degenerate input leaves the profile blank rather than dividing by a zero span.
    {
        std::vector<double> t{5.0, 5.0, 5.0};
        buildDensity(t, d);
        bool blank = true;
        for (int i = 0; i < kDensityBuckets; ++i) if (d[i]) blank = false;
        check(blank, "notes all at one instant leave the graph blank");
        buildDensity({}, d);
        check(d[0] == 0, "an empty chart leaves the graph blank");
    }

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
