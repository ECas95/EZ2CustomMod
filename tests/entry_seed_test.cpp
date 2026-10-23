// Guards the per-entry texture slots a custom song inherits from the native template.
//
// The inject seeds every custom master[] record with a byte copy of master[0] -- a NATIVE song --
// so the record arrives holding that song's texture pointers. Two of those slots are per-entry and
// are drawn straight out of the record rather than looked up by name, so a leftover pointer is not
// inert: it renders.
//
//     +0x000  small disc. sub_431AE0 draws this whenever the disc is under 176px, which is the
//             whole change-song animation. Left seeded, every custom song animated in showing
//             master[0]'s disc -- the reported "BABY DANCE fallback".
//     +0x004  song name.
//
// Only +0x004 used to be cleared. Both must be, and nothing else in the record may be disturbed:
// the id, the level floats and the version field all have to survive the clear.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o entry_seed_test tests/entry_seed_test.cpp
//   ./entry_seed_test
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static const uint32_t kEntryBytes    = 0x374;
static const uint32_t kSmallDiscOff  = 0x000;
static const uint32_t kSongnameOff   = 0x004;
static const uint32_t kEntryIdOff    = 0x10C;
static const uint32_t kVersionOff    = 0x370;
static const uint32_t kLevelOff[4]   = { 0x214, 0x220, 0x22C, 0x238 };
// A difficulty block is 12 bytes: level, bpm, bpm. The readout (sub_434080) draws the NM pair.
static const uint32_t kBpmOff[4]     = { 0x218, 0x224, 0x230, 0x23C };
static const uint32_t kBpmLoOff      = kBpmOff[0];
static const uint32_t kBpmHiOff      = kBpmOff[0] + 4;

// The part of writeEntry that matters here, over a plain buffer.
static void seedEntry(unsigned char* entry, const unsigned char* nativeTemplate,
                      const char* id, int versionNN, const float levels[4], float bpm = 0.0f) {
    memcpy(entry, nativeTemplate, kEntryBytes);
    if (versionNN >= 0) {
        uint32_t vv = (uint32_t)versionNN;
        memcpy(entry + kVersionOff, &vv, 4);
    }
    char idbuf[0x20] = {};
    strncpy(idbuf, id, sizeof(idbuf) - 1);
    memcpy(entry + kEntryIdOff, idbuf, strlen(idbuf) + 1);
    uint32_t noTexture = 0;
    memcpy(entry + kSmallDiscOff, &noTexture, 4);
    memcpy(entry + kSongnameOff, &noTexture, 4);
    for (int i = 0; i < 4; ++i) memcpy(entry + kLevelOff[i], &levels[i], 4);
    if (bpm > 0.0f)
        for (int i = 0; i < 4; ++i) {
            memcpy(entry + kBpmOff[i], &bpm, 4);
            memcpy(entry + kBpmOff[i] + 4, &bpm, 4);
        }
}

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static uint32_t u32(const unsigned char* p, uint32_t off) {
    uint32_t v; memcpy(&v, p + off, 4); return v;
}

int main() {
    // A native template carrying live texture pointers, as master[0] does.
    std::vector<unsigned char> native(kEntryBytes, 0xAB);
    const uint32_t nativeDisc = 0x0BADD15C;
    memcpy(native.data() + kSmallDiscOff, &nativeDisc, 4);
    const uint32_t nativeName = 0x0BADFACE;
    memcpy(native.data() + kSongnameOff, &nativeName, 4);
    strcpy((char*)native.data() + kEntryIdOff, "nativesong");

    // The template also carries the native song's BPM, which is what made every custom song in
    // the wheel read 111: that is BABY DANCE's tempo, inherited by the whole library.
    const float nativeBpm = 111.0f;
    memcpy(native.data() + kBpmLoOff, &nativeBpm, 4);
    memcpy(native.data() + kBpmHiOff, &nativeBpm, 4);

    std::vector<unsigned char> entry(kEntryBytes, 0);
    const float levels[4] = { 5.0f, 9.0f, 13.0f, 17.0f };
    seedEntry(entry.data(), native.data(), "bms012345", 66, levels, 174.0f);

    check(u32(entry.data(), kSmallDiscOff) == 0,
          "the small disc slot is cleared, so no native disc animates in");
    check(u32(entry.data(), kSongnameOff) == 0, "the song name slot is cleared");

    // The clear must not damage anything else the record carries.
    // Anchor the BPM offsets to the level offsets, which are independently known. A BPM field
    // that is not level+4 inside its own 12-byte block is in the wrong place, however plausible
    // the number written there looks.
    for (int i = 0; i < 4; ++i) {
        char msg[80];
        std::snprintf(msg, sizeof(msg), "slot %d: bpm sits at level+4 inside its block", i);
        check(kBpmOff[i] == kLevelOff[i] + 4, msg);
    }
    check(kBpmOff[3] + 8 < kVersionOff, "the last block ends before the version field");

    {
        float lo, hi;
        memcpy(&lo, entry.data() + kBpmLoOff, 4);
        memcpy(&hi, entry.data() + kBpmHiOff, 4);
        check(hi == 174.0f, "the song's own BPM is written, not the template's");
        check(lo == 174.0f, "both ends of the BPM range are written");
        check(hi != nativeBpm, "control: 111 is what the whole library used to show");
    }

    check(strcmp((const char*)entry.data() + kEntryIdOff, "bms012345") == 0,
          "the custom id survives");
    check(u32(entry.data(), kVersionOff) == 66, "the version field survives");
    for (int i = 0; i < 4; ++i) {
        float f; memcpy(&f, entry.data() + kLevelOff[i], 4);
        char msg[64];
        std::snprintf(msg, sizeof(msg), "level %d survives", i);
        check(f == levels[i], msg);
    }
    check(entry[8] == 0xAB, "the rest of the template is still copied through");
    // 0x370..0x373 IS the version field -- the last four bytes of the record -- so the last
    // untouched byte is the one before it.
    check(entry[kVersionOff - 1] == 0xAB, "including the tail, right up to the version field");

    // A song with no parseable tempo writes nothing, rather than stamping 0 over the field.
    {
        std::vector<unsigned char> e2(kEntryBytes, 0);
        seedEntry(e2.data(), native.data(), "bms999999", 66, levels, 0.0f);
        float hi; memcpy(&hi, e2.data() + kBpmHiOff, 4);
        check(hi == nativeBpm, "no tempo parsed leaves the field alone instead of writing zero");
    }

    // The control: without the clear, the native pointer is exactly what gets drawn.
    {
        std::vector<unsigned char> old(kEntryBytes, 0);
        memcpy(old.data(), native.data(), kEntryBytes);
        uint32_t zero = 0;
        memcpy(old.data() + kSongnameOff, &zero, 4);      // the old code cleared only this one
        check(u32(old.data(), kSmallDiscOff) == nativeDisc,
              "control: clearing only the name really did leave the native disc behind");
    }

    if (g_failures == 0) std::printf("entry seed: ok\n");
    return g_failures ? 1 : 0;
}
