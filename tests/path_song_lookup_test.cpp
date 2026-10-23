// Guards CustomSongsMentionedIn -- deciding which custom song a file path belongs to.
//
// This runs on every CreateFileA the game makes. It used to ask each of the catalogue's songs
// whether its id appeared in the path, which with the auto-generated modes expanded is 179,424
// substring searches per file open; song select opens thousands as it comes up, and that is where
// the startup minutes went. It now scans the path for "bms" + six digits and looks those up.
//
// Replacing a scan with an index is only safe if it finds the SAME song, so that is what these
// pin down: the ids a path really contains, in catalogue order, and nothing it does not.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o path_song_lookup_test tests/path_song_lookup_test.cpp
//   ./path_song_lookup_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

struct CustomBmsSong { std::string id; };

static std::string LowerAscii(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return o;
}

// ---- copied verbatim from mod_main.cpp ----------------------------------------------------------

static std::unordered_map<std::string, size_t> g_songByIdIndex;
static size_t g_songByIdStamp = (size_t)-1;

static void EnsureSongIdIndex(const std::vector<CustomBmsSong>& songs) {
    if (g_songByIdStamp == songs.size()) return;
    g_songByIdIndex.clear();
    g_songByIdIndex.reserve(songs.size() * 2);
    for (size_t i = 0; i < songs.size(); ++i) {
        std::string k = songs[i].id;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        g_songByIdIndex.emplace(k, i);
    }
    g_songByIdStamp = songs.size();
}

static std::vector<size_t> CustomSongsMentionedIn(const std::string& path,
                                                  const std::vector<CustomBmsSong>& songs) {
    std::vector<size_t> out;
    EnsureSongIdIndex(songs);
    std::string lower = LowerAscii(path);
    for (size_t i = 0; i + 9 <= lower.size(); ++i) {
        if (lower.compare(i, 3, "bms") != 0) continue;
        bool digits = true;
        for (size_t d = 3; d < 9; ++d)
            if (!isdigit((unsigned char)lower[i + d])) { digits = false; break; }
        if (!digits) continue;
        std::unordered_map<std::string, size_t>::const_iterator it =
            g_songByIdIndex.find(lower.substr(i, 9));
        if (it != g_songByIdIndex.end()) out.push_back(it->second);
        i += 8;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// What the old code did, kept as the reference the new code must agree with.
static std::vector<size_t> ScanReference(const std::string& path,
                                         const std::vector<CustomBmsSong>& songs) {
    std::vector<size_t> out;
    for (size_t i = 0; i < songs.size(); ++i)
        if (path.find(songs[i].id) != std::string::npos) out.push_back(i);
    return out;
}

int main() {
    printf("path -> song lookup\n");

    std::vector<CustomBmsSong> songs;
    char buf[32];
    for (int i = 0; i < 5000; ++i) {
        snprintf(buf, sizeof(buf), "bms%06d", i);
        songs.push_back(CustomBmsSong{buf});
    }

    check(CustomSongsMentionedIn("System\\disc\\bms000042.abm", songs) == std::vector<size_t>{42},
          "a path names exactly the song whose id it contains");
    check(CustomSongsMentionedIn("System\\disc\\BMS000042.ABM", songs) == std::vector<size_t>{42},
          "and the match is case-insensitive, as the filesystem is");
    check(CustomSongsMentionedIn("System\\disc\\bms004999.abm", songs) == std::vector<size_t>{4999},
          "the last song in the catalogue is reachable");

    check(CustomSongsMentionedIn("System\\Common\\video_blank_512.abm", songs).empty(),
          "a path with no id names nothing");
    check(CustomSongsMentionedIn("bms00004.abm", songs).empty(),
          "five digits is not an id");
    check(CustomSongsMentionedIn("bmsabcdef.abm", songs).empty(),
          "neither are six non-digits");
    check(CustomSongsMentionedIn("bms999999.abm", songs).empty(),
          "an id outside the catalogue names nothing");
    check(CustomSongsMentionedIn("bms", songs).empty(), "a truncated id does not run off the end");
    check(CustomSongsMentionedIn("", songs).empty(), "nor does an empty path");

    // Order matters: the caller takes the first match, and it has to be the one the old scan took.
    const std::string two = "CustomCache\\bms000300\\bms000100.abm";
    std::vector<size_t> got = CustomSongsMentionedIn(two, songs);
    check(got.size() == 2 && got[0] == 100 && got[1] == 300,
          "two ids in one path come back in catalogue order, not path order");

    // Seven digits: the scan takes the first six and the trailing digit is left in the path.
    check(CustomSongsMentionedIn("bms0001234.abm", songs) == std::vector<size_t>{123},
          "a longer digit run still resolves its leading id");

    // And the whole thing must agree with the scan it replaced, across every song.
    bool agree = true;
    for (int i = 0; i < 5000 && agree; ++i) {
        snprintf(buf, sizeof(buf), "System\\songname\\bms%06d.abm", i);
        if (CustomSongsMentionedIn(buf, songs) != ScanReference(buf, songs)) agree = false;
    }
    check(agree, "5000 paths give the same answer as scanning the whole catalogue");

    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
