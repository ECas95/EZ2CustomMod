// The song-select detail panels' layout arithmetic.
//
// Both of this panel's defects were arithmetic, not drawing: the four stat rows ran past the
// bottom rim so JACK fell off the plate entirely, and the big note count was drawn into a
// rectangle that overlapped the one "NOTES" had just been written into, which bit the corner off
// the label. Neither is visible from the code -- you have to add the numbers up -- so they are
// added up here.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o panel_layout_test tests/panel_layout_test.cpp
//   ./panel_layout_test
#include <cstdio>
#include <vector>

// ---- must match bms_scanner.h / mod_main.cpp ----------------------------------------------------
static const int kStatsPanelW = 88,  kStatsPanelH = 76;
static const int kGraphPanelW = 176, kGraphPanelH = 56;
static const int kDensityBuckets = 64;
static const int kPanelRadius = 9;

struct Box { int l, t, r, b; const char* what; };

static bool inside(const Box& x, int w, int h) {
    return x.l >= 0 && x.t >= 0 && x.r <= w && x.b <= h && x.l < x.r && x.t < x.b;
}
static bool overlap(const Box& a, const Box& b) {
    return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
}

// The stats plate, exactly as RenderDetailBgr lays it out.
static std::vector<Box> statsBoxes() {
    const int w = kStatsPanelW;
    const int pad = 8, right = w - pad, split = w / 2 + 2;
    std::vector<Box> out;
    out.push_back({pad, 8, split, 20, "NOTES label"});
    out.push_back({split, 5, right, 24, "note count"});
    int y = 26;
    const char* names[4] = {"NPS", "PEAK", "CHORD", "JACK"};
    for (int i = 0; i < 4; ++i) {
        out.push_back({pad, y, split, y + 11, names[i]});
        out.push_back({split, y, right, y + 11, "value"});
        y += 11;
    }
    return out;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

// Where the plate is put, and the two things it has to stay clear of. The category tab row runs
// along the top of the song-select screen and the disc's top edge is below it; the plate has to
// live in the band between them. Putting it higher for more room ran it through the tabs.
static const int kStatsX = 300, kStatsY = 80;
static const int kTabsBottom = 76;      // the category tabs end about here
static const int kDiscTop = 160;        // the disc starts about here

int main() {
    const std::vector<Box> boxes = statsBoxes();

    check(kStatsY >= kTabsBottom, "the plate starts below the category tabs");
    check(kStatsY + kStatsPanelH <= kDiscTop, "the plate ends above the disc");
    check(kStatsX + kStatsPanelW <= 388, "the plate does not reach into the disc sideways");

    bool allIn = true;
    for (const Box& x : boxes) {
        if (!inside(x, kStatsPanelW, kStatsPanelH)) {
            allIn = false;
            std::printf("   '%s' is outside the plate: %d,%d..%d,%d vs %dx%d\n",
                        x.what, x.l, x.t, x.r, x.b, kStatsPanelW, kStatsPanelH);
        }
    }
    check(allIn, "every stats rect is inside the plate");

    bool clear = true;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j)
            if (overlap(boxes[i], boxes[j])) {
                // A label and its own value share a row but not a column, so they must not overlap.
                clear = false;
                std::printf("   '%s' overlaps '%s'\n", boxes[i].what, boxes[j].what);
            }
    check(clear, "no two stats rects overlap");

    // Room left under the last row for the rounded rim, so text never touches the border.
    int lowest = 0;
    for (const Box& x : boxes) if (x.b > lowest) lowest = x.b;
    check(kStatsPanelH - lowest >= kPanelRadius / 3, "the last row clears the bottom rim");
    check(lowest > kStatsPanelH / 2, "the plate is not mostly empty");

    // Every label column is wide enough for its text at the condensed 9px face: the font is
    // created at 42% of its height, so a character is about 3.8px and "CHORD" needs ~19.
    const int labelWidth = kStatsPanelW / 2 + 2 - 8;
    check(labelWidth >= 20, "the label column fits the longest label");

    // The graph: 64 bars two pixels wide, centred, with the axis and the heading clear of them.
    const int left = (kGraphPanelW - kDensityBuckets * 2) / 2;
    const int base = kGraphPanelH - 8, top = 16;
    check(left >= 4, "the bars clear the left rim");
    check(left + kDensityBuckets * 2 <= kGraphPanelW - 4, "the bars clear the right rim");
    check(base + 1 <= kGraphPanelH, "the axis is inside the plate");
    check(base - top >= 20, "the bars have room to be worth reading");
    check(top >= 15, "the bars start below the heading");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
