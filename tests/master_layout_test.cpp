// Guards where master[] lives inside the SongSelect object.
//
// The array is relocated so a grown library fits, and the original delta (0xEFDF4, landing at
// obj+0x14E0B0) was picked as "just past the object end (~0x103540)". That end was wrong: the
// object keeps live fields far above it, up to obj+0x210988, with a dense cluster around
// obj+0x20C000..0x20E900. Once master[] grew past entry ~880 those fields sat INSIDE song
// records, and the game rewrote them every frame:
//
//     this+2146528 -> master[880]     this+2150756 -> master[885]
//     this+2148028 -> master[882]     this+2152376 -> master[887]
//     this+2148052 -> master[882]     [esi+0x20E894] -> master[891]
//
// A zeroed level float makes the filter drop the song; a zeroed id makes the disc-art loader
// sprintf "system\disc\%s.bmp" over an unterminated field and smash a 260-byte stack buffer.
// With the stock 436 entries master ended below the cluster, so it only showed up on a big library.
//
// The offsets below are the ones the decompilation actually shows. Keep this in step with
// kMasterHome in trace_hooks.cpp.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o master_layout_test tests/master_layout_test.cpp
//   ./master_layout_test
#include <cstdint>
#include <cstdio>
#include <initializer_list>

// ---- must match trace_hooks.cpp ---------------------------------------------------------------
static const uint32_t kMasterOrigBase = 0x5E2BC;
static const uint32_t kMasterHome     = 0x400000;
static const uint32_t kMasterRel      = kMasterHome - kMasterOrigBase;
static const uint32_t kEntryBytes     = 0x374;          // 884

// Our own relocated arrays, which must also stay clear of object fields.
struct Region { const char* name; uint32_t base; uint32_t bytes; };
static const Region kOurArrays[] = {
    { "category texture (small)", 0x110000, 127 * 4 },
    { "category texture (big)",   0x110800, 127 * 4 },
    { "version array B",          0x111000, 147 * 4 },
    { "version array A",          0x112000, 147 * 4 },
    { "sortstyle array",          0x113000, 128 * 4 },
};

// Every object offset the decompilation shows above 1 MB, where a relocated array might land.
static const uint32_t kLiveFields[] = {
    0x109DE4, 0x20BD94, 0x20C0E0, 0x20C67C, 0x20C6D4, 0x20D164, 0x20D280,
    0x20D59C, 0x20D7B8, 0x20DA3C, 0x20DA40, 0x20DAA0, 0x20E890, 0x20E894,
    0x20E898, 0x20EB04, 0x20EB08, 0x20EB0C, 0x20EB10, 0x20EB14, 0x210988,
};
static const uint32_t kHighestField = 0x210988;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static bool overlaps(uint32_t aLo, uint32_t aLen, uint32_t b) {
    return b >= aLo && b < aLo + aLen;
}

int main() {
    std::printf("  master[] home  obj+0x%X   (delta 0x%X)\n", kMasterHome, kMasterRel);
    std::printf("  highest field  obj+0x%X   margin %.2f MB\n",
                kHighestField, (double)(kMasterHome - kHighestField) / 1048576.0);

    check(kMasterHome > kHighestField, "master[] starts above every known object field");
    check(kMasterHome - kHighestField >= 0x100000,
          "and keeps at least 1 MB of margin for fields the decompilation missed");
    check(kMasterRel == kMasterHome - kMasterOrigBase, "the delta matches the home it encodes");

    // The old delta must fail these, or the test proves nothing.
    {
        const uint32_t oldHome = kMasterOrigBase + 0xEFDF4;
        check(oldHome < kHighestField, "control: the previous home really did sit below live fields");
        const uint32_t badIdx = (0x20C67C - oldHome) / kEntryBytes;
        check(badIdx == 882, "control: obj+0x20C67C landed in master[882] under the old layout");
    }

    // No live field may fall inside master[] at any capacity we would ever configure.
    for (uint32_t cap : {436u, 6502u, 60000u}) {
        const uint32_t bytes = cap * kEntryBytes;
        for (uint32_t f : kLiveFields) {
            if (!overlaps(kMasterHome, bytes, f)) continue;
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "obj+0x%X falls inside master[] at capacity %u", f, cap);
            check(false, msg);
        }
    }
    check(true, "no live object field falls inside master[] at 436, 6502 or 60000 entries");

    // Our own relocated arrays must be clear too, and clear of master[].
    for (const Region& r : kOurArrays) {
        for (uint32_t f : kLiveFields) {
            if (!overlaps(r.base, r.bytes, f)) continue;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s overlaps live field obj+0x%X", r.name, f);
            check(false, msg);
        }
        check(r.base + r.bytes <= kMasterHome, "our array ends before master[] begins");
    }
    check(true, "every relocated array of ours sits in free space");

    // The arrays must not overlap each other either.
    for (size_t i = 0; i < sizeof(kOurArrays) / sizeof(kOurArrays[0]); ++i)
        for (size_t j = i + 1; j < sizeof(kOurArrays) / sizeof(kOurArrays[0]); ++j) {
            const Region& a = kOurArrays[i];
            const Region& b = kOurArrays[j];
            check(a.base + a.bytes <= b.base || b.base + b.bytes <= a.base,
                  "two relocated arrays of ours overlap");
        }

    if (g_failures == 0) std::printf("master layout: ok\n");
    return g_failures ? 1 : 0;
}
