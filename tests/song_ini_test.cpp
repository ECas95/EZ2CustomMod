// Guards the per-chart .ini served at chart load in direct mode (EnsureSongIni / BuildSongIni).
//
// The game asks for "<mode>1p-<id>[-hd|-shd|-ex].ini" and, without it, falls back to Good 36 /
// Miss 72 and Level 99 -- customs judged tighter than natives. What is pinned here: the requested
// name resolves to the right difficulty slot (case-insensitively), and the text matches the native
// files' shape and per-mode values, with nothing from the song's title able to open a section.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o song_ini_test tests/song_ini_test.cpp
//   ./song_ini_test
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

struct CustomBmsChart { std::string file; std::string playLevel; int slot = -1; };
struct CustomBmsSong { std::string id, mode, title; std::vector<CustomBmsChart> chartMeta; };

static std::string NormalizeModeName(const std::string& raw) { return raw; }
static const uint32_t kModelTickScale = 2;   // 384 / 192, as in bms_scanner.cpp
static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}
static const char* suffixForSlot(int slot) {
    static const char* suffixes[4] = {"", "-hd", "-shd", "-ex"};
    return slot >= 0 && slot < 4 ? suffixes[slot] : "";
}

// ---- copied verbatim from bms_scanner.cpp -------------------------------------------------------

static std::string BuildSongIni(const CustomBmsSong& song, const CustomBmsChart& chart) {
    const std::string m = NormalizeModeName(song.mode);
    const int k = (int)kModelTickScale;
    int kool = 6, cool = 24, good = 50, miss = 70;
    if (m == "ez2Catch")       { kool = 5;  cool = 18; good = 40; miss = 56; }
    else if (m == "turnTable") { kool = 10; cool = 21; good = 45; miss = 60; }
    char judge[96];
    const int widen = 3 * (k - 1);
    snprintf(judge, sizeof(judge), "Kool = %d\r\nCool = %d\r\nGood = %d\r\nMiss = %d\r\n",
             kool * k + widen, cool * k + widen, good * k + widen, miss * k + widen);
    const char* gauge = m == "ez2Catch"  ? "Cool = 0.2\r\nGood = 0.0\r\nMiss = 0.0\r\nFail = -4.0\r\n"
                      : m == "turnTable" ? "Cool = 0.2\r\nGood = 0.1\r\nMiss = -1.5\r\nFail = -4.0\r\n"
                      : m == "clubMix"   ? "Cool = 0.4\r\nGood = 0.3\r\nMiss = -1.5\r\nFail = -4.0\r\n"
                      :                    "Cool = 0.2\r\nGood = 0.1\r\nMiss = -1.8\r\nFail = -4.8\r\n";
    std::string ini;
    ini += "[General]\r\n";
    ini += "Level = " + (chart.playLevel.empty() ? std::string("1") : chart.playLevel) + "\r\n";
    ini += "MeasureScale = 1.6\r\n\r\n";
    ini += std::string("[JudgmentDelta]\r\n") + judge + "\r\n";
    ini += std::string("[GaugeUpDownRate]\r\n") + gauge;
    return ini;
}

// The name -> chart resolution from EnsureSongIni, up to the point it touches disk.
static const CustomBmsChart* ChartForRequest(const CustomBmsSong& song, const std::string& requestedName) {
    std::string lower = lowerCopy(requestedName);
    size_t idPos = lower.find(lowerCopy(song.id));
    if (idPos == std::string::npos || lower.size() < 4 ||
        lower.compare(lower.size() - 4, 4, ".ini") != 0) return nullptr;
    std::string suffix = lower.substr(idPos + song.id.size(), lower.size() - 4 - idPos - song.id.size());
    int slot = 0;
    for (int s = 1; s < 4; ++s) if (suffix == suffixForSlot(s)) slot = s;
    const CustomBmsChart* chart = nullptr;
    for (size_t i = 0; i < song.chartMeta.size(); ++i)
        if (song.chartMeta[i].slot == slot && (!chart || song.chartMeta[i].file < chart->file)) chart = &song.chartMeta[i];
    if (!chart) for (size_t i = 0; i < song.chartMeta.size() && !chart; ++i) if (song.chartMeta[i].slot == 0) chart = &song.chartMeta[i];
    return chart;
}

// ---- the shape the game parses (decrypted Sound\10Anti\5keymix1p-10anti.ini), with the judge
// windows doubled: they are chart ticks, and our charts carry twice the native ticks per beat.
static const char* kNative5Key =
    "[General]\r\nLevel = 8\r\nMeasureScale = 1.6\r\n\r\n"
    "[JudgmentDelta]\r\nKool = 15\r\nCool = 51\r\nGood = 103\r\nMiss = 143\r\n\r\n"
    "[GaugeUpDownRate]\r\nCool = 0.2\r\nGood = 0.1\r\nMiss = -1.8\r\nFail = -4.8\r\n";

int main() {
    printf("song ini\n");

    CustomBmsSong song;
    song.id = "bms000042";
    song.mode = "5keyMix";
    song.title = "[Full] Song [SHD]";
    song.chartMeta = { {"b.bms", "8", 0}, {"a.bms", "8", 0}, {"hd.bms", "11", 1}, {"shd.bms", "14", 2} };

    const CustomBmsChart* c = ChartForRequest(song, "5KeyMix1p-bms000042.ini");
    check(c && c->slot == 0 && c->file == "a.bms", "the bare name is the NM chart, first by file name");
    c = ChartForRequest(song, "5KeyMix1p-bms000042-hd.ini");
    check(c && c->slot == 1, "-hd is slot 1");
    c = ChartForRequest(song, "5KEYMIX1P-BMS000042-SHD.INI");
    check(c && c->slot == 2, "-shd is slot 2, whatever the case (the game's mode name is mixed)");
    c = ChartForRequest(song, "5KeyMix1p-bms000042-ex.ini");
    check(c && c->slot == 0, "a difficulty the song lacks falls back to NM rather than nothing");
    check(!ChartForRequest(song, "5KeyMix1p-bms000042.int"), "the decrypt temp is not an ini");
    check(!ChartForRequest(song, "5KeyMix1p-bms000043.ini"), "another song's name is refused");

    std::string ini = BuildSongIni(song, song.chartMeta[0]);
    check(ini == kNative5Key, "5keyMix text is the native shape with the windows scaled to our grid");
    check(ini.find(song.title) == std::string::npos, "the title never enters the file");
    check(std::count(ini.begin(), ini.end(), '[') == 3, "exactly the three sections the game reads");

    song.mode = "ez2Catch";
    check(BuildSongIni(song, song.chartMeta[0]).find("Kool = 13\r\nCool = 39\r\nGood = 83\r\nMiss = 115") != std::string::npos,
          "catch carries its own judgement set, scaled the same way");
    song.mode = "turnTable";
    check(BuildSongIni(song, song.chartMeta[0]).find("Kool = 23\r\nCool = 45\r\nGood = 93\r\nMiss = 123") != std::string::npos,
          "scratch too");
    song.mode = "clubMix";
    check(BuildSongIni(song, song.chartMeta[0]).find("Cool = 0.4\r\nGood = 0.3") != std::string::npos,
          "club keeps the default judgement and its own gauge");
    CustomBmsChart noLevel{"x.bms", "", 0};
    check(BuildSongIni(song, noLevel).find("Level = 1\r\n") != std::string::npos, "a missing level is 1, not 99");

    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
