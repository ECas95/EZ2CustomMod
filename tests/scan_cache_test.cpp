// Guards the scan cache's serialisation.
//
// Every launch used to re-open and re-parse every .bms to recover the same title, artist, BPM and
// levels as last time -- 13 to 26 seconds at this library size. The cache stores that result and
// reuses it for folders that have not changed.
//
// Which makes this a serialiser, and a serialiser that is subtly wrong does not crash: it hands
// back a song with someone else's title, or a level of 0 that quietly removes it from the wheel.
// So every field is round-tripped, including the ones most likely to break a format:
//
//   - titles with tabs, quotes and newlines (any delimiter-based format would corrupt these)
//   - empty strings, which must stay empty rather than becoming absent
//   - non-ASCII bytes, which the library is full of
//   - a song with no charts at all
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o scan_cache_test tests/scan_cache_test.cpp
//   ./scan_cache_test
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- the format, copied from bms_scanner.cpp ---------------------------------------------------
struct Chart { std::string file, title, artist, stageFile, banner, bpm, playLevel, difficulty; int slot; };
struct Song {
    std::string id, mode, modeFolder, category, songDir, cacheDir, title, artist;
    std::vector<std::string> charts, audioFiles, imageFiles, videoFiles;
    std::vector<Chart> chartMeta;
    int levelNM = 0, levelHD = 0, levelSHD = 0, levelEX = 0;
    float bpmNM = 0, bpmHD = 0, bpmSHD = 0, bpmEX = 0;
};

static void putStr(std::vector<unsigned char>& b, const std::string& s) {
    uint32_t n = (uint32_t)s.size();
    b.insert(b.end(), (unsigned char*)&n, (unsigned char*)&n + 4);
    b.insert(b.end(), s.begin(), s.end());
}
static void putU32(std::vector<unsigned char>& b, uint32_t v) {
    b.insert(b.end(), (unsigned char*)&v, (unsigned char*)&v + 4);
}
static void putF32(std::vector<unsigned char>& b, float v) {
    b.insert(b.end(), (unsigned char*)&v, (unsigned char*)&v + 4);
}
static void putVec(std::vector<unsigned char>& b, const std::vector<std::string>& v) {
    putU32(b, (uint32_t)v.size());
    for (const auto& s : v) putStr(b, s);
}

struct Reader {
    const unsigned char* p; const unsigned char* end; bool ok = true;
    bool need(size_t n) { if ((size_t)(end - p) < n) { ok = false; return false; } return true; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
    float f32() { if (!need(4)) return 0.f; float v; memcpy(&v, p, 4); p += 4; return v; }
    std::string str() {
        uint32_t n = u32();
        if (!ok || n > (1u << 20) || !need(n)) { ok = false; return std::string(); }
        std::string s((const char*)p, n); p += n; return s;
    }
    std::vector<std::string> vec() {
        std::vector<std::string> v; uint32_t n = u32();
        if (!ok || n > (1u << 20)) { ok = false; return v; }
        for (uint32_t i = 0; i < n && ok; ++i) v.push_back(str());
        return v;
    }
};

static std::vector<unsigned char> save(const Song& s) {
    std::vector<unsigned char> b;
    putStr(b, s.id); putStr(b, s.mode); putStr(b, s.modeFolder); putStr(b, s.category);
    putStr(b, s.songDir); putStr(b, s.cacheDir); putStr(b, s.title); putStr(b, s.artist);
    putVec(b, s.charts); putVec(b, s.audioFiles); putVec(b, s.imageFiles); putVec(b, s.videoFiles);
    putU32(b, (uint32_t)s.chartMeta.size());
    for (const auto& c : s.chartMeta) {
        putStr(b, c.file); putStr(b, c.title); putStr(b, c.artist); putStr(b, c.stageFile);
        putStr(b, c.banner); putStr(b, c.bpm); putStr(b, c.playLevel); putStr(b, c.difficulty);
        putU32(b, (uint32_t)c.slot);
    }
    putU32(b, (uint32_t)s.levelNM); putU32(b, (uint32_t)s.levelHD);
    putU32(b, (uint32_t)s.levelSHD); putU32(b, (uint32_t)s.levelEX);
    putF32(b, s.bpmNM); putF32(b, s.bpmHD); putF32(b, s.bpmSHD); putF32(b, s.bpmEX);
    return b;
}

static bool load(const std::vector<unsigned char>& b, Song& s) {
    Reader r{ b.data(), b.data() + b.size() };
    s.id = r.str(); s.mode = r.str(); s.modeFolder = r.str(); s.category = r.str();
    s.songDir = r.str(); s.cacheDir = r.str(); s.title = r.str(); s.artist = r.str();
    s.charts = r.vec(); s.audioFiles = r.vec(); s.imageFiles = r.vec(); s.videoFiles = r.vec();
    uint32_t n = r.u32();
    if (n > (1u << 20)) return false;
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        Chart c;
        c.file = r.str(); c.title = r.str(); c.artist = r.str(); c.stageFile = r.str();
        c.banner = r.str(); c.bpm = r.str(); c.playLevel = r.str(); c.difficulty = r.str();
        c.slot = (int)r.u32();
        s.chartMeta.push_back(c);
    }
    s.levelNM = (int)r.u32(); s.levelHD = (int)r.u32();
    s.levelSHD = (int)r.u32(); s.levelEX = (int)r.u32();
    s.bpmNM = r.f32(); s.bpmHD = r.f32(); s.bpmSHD = r.f32(); s.bpmEX = r.f32();
    return r.ok;
}
// -------------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

int main() {
    // A song built out of everything that breaks a delimiter-based format.
    Song a;
    a.id = "bms016343";
    a.mode = "5keyMix"; a.modeFolder = "5KeyMix"; a.category = "PIU.2";
    a.songDir  = "C:\\EZ2AC\\BMSData\\5KeyMix\\PIU.2\\(3 ) A01 - Final Audition 3 U . F";
    a.cacheDir = "C:\\EZ2AC\\CustomCache\\5keyMix\\bms016343";
    a.title  = "A\ttitle\twith\ttabs and \"quotes\" and a \n newline";
    a.artist = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";      // non-ASCII, as much of the library is
    a.charts = { "01 Easy.bms", "02 Medium.bms", "03 Hard.bms", "04 Expert.bms" };
    a.audioFiles = { "song.mp3" };
    a.imageFiles = { "banner.png", "" };                     // an empty entry must survive
    a.videoFiles = {};
    a.chartMeta = {
        { "01 Easy.bms", "t", "a", "stage.png", "ban.png", "65.25",  "13", "BEGINNER", 0 },
        { "04 Expert.bms", "",  "", "",          "",        "130.5", "17", "",         3 },
    };
    a.levelNM = 13; a.levelHD = 14; a.levelSHD = 15; a.levelEX = 17;
    a.bpmNM = 65.25f; a.bpmHD = 130.5f; a.bpmSHD = 32.625f; a.bpmEX = 65.25f;

    Song b;
    check(load(save(a), b), "a full record round-trips without running out of buffer");

    check(b.id == a.id && b.mode == a.mode && b.modeFolder == a.modeFolder, "identity survives");
    check(b.category == a.category, "category survives");
    check(b.songDir == a.songDir && b.cacheDir == a.cacheDir, "paths survive");
    check(b.title == a.title, "a title with tabs, quotes and a newline survives intact");
    check(b.artist == a.artist, "non-ASCII bytes survive");
    check(b.charts == a.charts, "the chart list survives");
    check(b.imageFiles == a.imageFiles, "an empty string stays an empty string, not a missing one");
    check(b.videoFiles.empty(), "an empty list stays empty");
    check(b.chartMeta.size() == 2, "both chart records survive");
    check(b.chartMeta[0].playLevel == "13" && b.chartMeta[1].slot == 3, "chart fields survive");
    check(b.chartMeta[1].title.empty() && b.chartMeta[1].stageFile.empty(),
          "empty chart fields stay empty");
    check(b.levelNM == 13 && b.levelEX == 17, "levels survive -- a zero here hides the song");
    check(b.bpmNM == 65.25f && b.bpmSHD == 32.625f, "per-difficulty BPM survives exactly");

    // A song with nothing in it: the degenerate case a length-prefixed format must still handle.
    {
        Song empty, out;
        check(load(save(empty), out), "an empty record round-trips");
        check(out.id.empty() && out.charts.empty() && out.chartMeta.empty(),
              "and comes back empty rather than with garbage");
    }

    // Truncation must be detected, not half-read. A cache cut short by a crash mid-write would
    // otherwise return a song carrying the right title and a level of 0.
    {
        std::vector<unsigned char> good = save(a);
        for (size_t cut : { size_t(4), good.size() / 3, good.size() / 2, good.size() - 1 }) {
            std::vector<unsigned char> torn(good.begin(), good.begin() + cut);
            Song out;
            check(!load(torn, out), "a truncated record is rejected, never half-accepted");
        }
    }

    // A corrupt length must not be trusted into a huge allocation.
    {
        std::vector<unsigned char> bad = save(a);
        uint32_t huge = 0xFFFFFFF0u;
        memcpy(bad.data(), &huge, 4);              // the id's length prefix
        Song out;
        check(!load(bad, out), "an absurd length prefix is rejected");
    }

    if (g_failures == 0) std::printf("scan cache: ok\n");
    return g_failures ? 1 : 0;
}
