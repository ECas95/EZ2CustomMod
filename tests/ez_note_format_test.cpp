// Guards the 13-byte note event against the format the game actually ships.
//
// Measured by decrypting Sound\10Anti\5keymix1p-10anti-hd.ez with the FNEX .ez key and counting all
// 2067 of its events:
//
//     type 1, len=6      1988    ordinary taps
//     type 1, len=36..120  73    long notes, duration in ticks
//     type 3                9    BPM markers
//     types 2 / 4 / 250     5    markers
//
// So no shipped note has a length of zero: 6 is the floor the format uses for an instantaneous
// one. Ours emitted 0, because len was only assigned while resolving a long note -- which gives the
// engine a single instant to match an input against, and is why custom charts were nearly
// unhittable while their timing was otherwise correct.
//
// The field is raw: the engine's hold test is `width > 6` on the record's own u16, never rescaled
// by the grid (Panel::m42b2f0, decomp gamepanels.cpp), so the floor is 6 on any grid. It was
// scaled to 12 for the 384 grid once, and that drew every custom tap as a hold.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o ez_note_format_test tests/ez_note_format_test.cpp
//   ./ez_note_format_test
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ---- must match bms_scanner.cpp ----------------------------------------------------------------
static const uint32_t kEzTicksPerMeasure    = 384;
static const uint32_t kNativeTicksPerMeasure = 192;
static const uint32_t kModelTickScale        = kEzTicksPerMeasure / kNativeTicksPerMeasure;
static const int      kMinNoteLenTicks       = 6;

struct Ev { uint32_t tick; int wav; int len; int type; float bpm; };

static void putU16(std::vector<unsigned char>& b, size_t at, uint16_t v) {
    b[at] = (unsigned char)(v & 0xff); b[at + 1] = (unsigned char)(v >> 8);
}
static void putU32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

// The writer's event loop, copied from GenerateEzFile.
static std::vector<unsigned char> writeEvents(const std::vector<Ev>& evs) {
    std::vector<unsigned char> out;
    for (const Ev& ev : evs) {
        size_t off = out.size();
        out.resize(off + 13, 0);
        putU32(out, off + 0, ev.tick);
        out[off + 4] = (unsigned char)ev.type;
        if (ev.type == 3) {
            float f = ev.bpm;
            unsigned char tmp[4];
            memcpy(tmp, &f, 4);
            memcpy(&out[off + 5], tmp, 4);
        } else {
            putU16(out, off + 5, (uint16_t)ev.wav);
            out[off + 7] = 0x7f;
            out[off + 8] = 0x40;
            out[off + 9] = 0x00;
            putU16(out, off + 10,
                   (uint16_t)(ev.len < kMinNoteLenTicks ? kMinNoteLenTicks : ev.len));
            out[off + 12] = 0x00;
        }
    }
    return out;
}
// -------------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static uint16_t lenOf(const std::vector<unsigned char>& b, size_t i) {
    return (uint16_t)(b[i * 13 + 10] | (b[i * 13 + 11] << 8));
}

int main() {
    std::printf("  floor: %d ticks (native 6 at %u/measure, ours at %u)\n",
                kMinNoteLenTicks, kNativeTicksPerMeasure, kEzTicksPerMeasure);

    // A tap arrives with len 0 -- nothing ever assigns it -- and must not be written that way.
    {
        std::vector<Ev> evs{{384, 3, 0, 1, 0.0f}};
        auto b = writeEvents(evs);
        check(b.size() == 13, "one event is 13 bytes");
        check(lenOf(b, 0) == (uint16_t)kMinNoteLenTicks,
              "a tap is written with the floor, never with zero");
        check(lenOf(b, 0) != 0, "control: zero is what made custom charts unhittable");
    }

    // Long notes keep their own duration.
    {
        std::vector<Ev> evs{{0, 1, 96, 1, 0.0f}, {192, 2, 240, 1, 0.0f}};
        auto b = writeEvents(evs);
        check(lenOf(b, 0) == 96,  "a long note keeps its length");
        check(lenOf(b, 1) == 240, "and so does a longer one");
    }

    // A long note shorter than the floor still gets the floor -- a 1-tick hold is a tap.
    {
        std::vector<Ev> evs{{0, 1, 3, 1, 0.0f}};
        auto b = writeEvents(evs);
        check(lenOf(b, 0) == (uint16_t)kMinNoteLenTicks, "a sub-floor hold is raised to the floor");
    }

    // The three constant bytes, verified against the shipped chart: every note there carries
    // 7F 40 00 at offsets 7, 8 and 9.
    {
        std::vector<Ev> evs{{384, 0x36, 0, 1, 0.0f}};
        auto b = writeEvents(evs);
        check(b[7] == 0x7f && b[8] == 0x40 && b[9] == 0x00,
              "the fixed bytes match what every shipped note carries");
        check(b[5] == 0x36 && b[6] == 0x00, "the wav id is a little-endian u16 at offset 5");
        check(b[12] == 0x00, "the trailing byte is zero");
    }

    // BPM markers are not notes and must not be given a length.
    {
        std::vector<Ev> evs{{0, 0, 0, 3, 145.0f}};
        auto b = writeEvents(evs);
        float f = 0.0f;
        memcpy(&f, &b[5], 4);
        check(f == 145.0f, "a BPM marker stores its tempo as a float at offset 5");
        check(b[4] == 3, "and keeps type 3");
    }

    // The floor must NOT follow the grid: the engine reads the field raw and calls anything above
    // 6 a hold, so 12 here is a chart of nothing but long notes.
    check(kMinNoteLenTicks == 6, "the floor is the engine's raw 6, not scaled to our grid");
    check(kModelTickScale == 2 && kMinNoteLenTicks <= 6, "and stays a tap under the engine's `width > 6` hold test");

    if (g_failures == 0) std::printf("ez note format: ok\n");
    return g_failures ? 1 : 0;
}
