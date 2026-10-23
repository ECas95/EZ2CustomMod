// Dropping the per-chart difficulty tag from a song's title.
//
// The wheel shows one name per song, but a BMS #TITLE carries the chart's difficulty with it, so
// the list read "Daphnis [Basic]", "Dark Diver [Basic]", ... A vocabulary cannot keep up with the
// packs in this library:
//
//   CHU.1..CHU.5   [Basic] [Advanced] [Expert] [Master] [Ultima]
//   PSK.1..PSK.3   [EASY 5K] [NORMAL 5K] [HARD 5K] [EXPERT 5K] [MASTER 5K] [APPEND 5K]
//   WJMAX.1/.2     [ANGEL 5K] [WAKGOOD 5K] [MINSU 5K] [MESSI 5K] [LIE 5K]   (streamer names)
//   SEVENS         [10036_1] [5097_5]                                        (chart ids)
//   MUS / MUS_R    [4T HARD] [6T EASY]
//   RHYS           no tag at all
//
// What they share is the shape, not the words: every chart of a song has the same title with a
// different trailing bracket. So the song's own charts decide what the tag is, and the word list
// is only the fallback for a song with nothing to compare against.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o title_tag_test tests/title_tag_test.cpp
//   ./title_tag_test
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

// ---- must match bms_scanner.cpp -----------------------------------------------------------------
static std::string stripDifficultyTag(const std::string& title) {
    std::string t = title;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (t.empty() || t.back() != ']') return t;
    size_t open = t.rfind('[');
    if (open == std::string::npos) return t;
    std::string inner = t.substr(open + 1, t.size() - open - 2);
    std::string up;
    for (char c : inner) if (c != ' ' && c != '\t') up += (char)toupper((unsigned char)c);
    while (up.size() > 2 && up.back() == 'K') {
        size_t digits = up.size() - 1;
        while (digits > 0 && isdigit((unsigned char)up[digits - 1])) --digits;
        if (digits == up.size() - 1 || digits == 0) break;
        up.erase(digits);
    }
    static const char* diffs[] = {"EASY","NORMAL","NM","HARD","HD","HYPER","ANOTHER","INSANE",
        "BEGINNER","LIGHT","EX","EXTRA","MX","MAXIMUM","SHD","CHAOS","BLACK","WHITE","SP","DP",
        "BASIC","ADVANCED","EXPERT","MASTER","ULTIMA","APPEND","PRESENT","FUTURE"};
    bool known = false;
    for (const char* d : diffs) if (up == d) { known = true; break; }
    if (!known && !inner.empty()) {
        bool anyLetter = false, anyDigit = false;
        for (char c : inner) {
            if (isalpha((unsigned char)c)) anyLetter = true;
            else if (isdigit((unsigned char)c)) anyDigit = true;
        }
        known = !anyLetter && anyDigit;
    }
    if (known) {
        std::string base = t.substr(0, open);
        while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
        return base.empty() ? t : base;
    }
    return t;
}

static std::string stripChartTagBySiblings(const std::vector<std::string>& titles) {
    std::string prefix, firstTag;
    bool havePrefix = false, tagsDiffer = false, seen = false;
    for (const auto& title : titles) {
        std::string t = title;
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (t.empty()) continue;
        if (t.back() != ']') return std::string();
        const size_t open = t.rfind('[');
        if (open == std::string::npos || open == 0) return std::string();
        std::string base = t.substr(0, open);
        while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
        if (base.empty()) return std::string();
        const std::string tag = t.substr(open + 1, t.size() - open - 2);
        if (!havePrefix) { prefix = base; firstTag = tag; havePrefix = true; }
        else {
            if (base != prefix) return std::string();
            if (tag != firstTag) tagsDiffer = true;
        }
        seen = true;
    }
    if (!seen || !havePrefix || !tagsDiffer) return std::string();
    return prefix;
}

// What the scanner does: the siblings decide, the word list is the fallback.
static std::string songTitle(const std::vector<std::string>& charts) {
    const std::string shared = stripChartTagBySiblings(charts);
    if (!shared.empty()) return shared;
    return charts.empty() ? std::string() : stripDifficultyTag(charts.front());
}

static int failures = 0;
static void check(const std::vector<std::string>& charts, const std::string& want, const char* what) {
    const std::string got = songTitle(charts);
    const bool ok = got == want;
    std::printf("%-46s %-26s %s\n", what, ("\"" + got + "\"").c_str(), ok ? "ok" : "FAILED");
    if (!ok) { std::printf("   wanted \"%s\"\n", want.c_str()); ++failures; }
}

int main() {
    check({"Aleph-0 [Basic]", "Aleph-0 [Advanced]", "Aleph-0 [Expert]",
           "Aleph-0 [Master]", "Aleph-0 [Ultima]"}, "Aleph-0", "CHU: Basic..Ultima");
    check({"ANiMA [EASY 5K]", "ANiMA [NORMAL 5K]", "ANiMA [HARD 5K]",
           "ANiMA [EXPERT 5K]", "ANiMA [MASTER 5K]", "ANiMA [APPEND 5K]"}, "ANiMA",
          "PSK: EASY 5K..APPEND 5K");
    check({"Your Star [WAKGOOD 5K]", "Your Star [ANGEL 5K]", "Your Star [MESSI 5K]"},
          "Your Star", "WJMAX: streamer names");
    check({"403 Forbidden [5097_1]", "403 Forbidden [5097_2]", "403 Forbidden [5097_5]"},
          "403 Forbidden", "SEVENS: numeric chart ids");
    check({"8th [4T EASY]", "8th [4T HARD]", "8th [6T EASY]", "8th [6T HARD]"},
          "8th", "MUS: lane count and difficulty");
    check({"Aegis", "Aegis", "Aegis"}, "Aegis", "RHYS: no tag to remove");

    // A real bracketed title survives: every chart carries the same one.
    check({"Song [Extended Mix]", "Song [Extended Mix]", "Song [Extended Mix]"},
          "Song [Extended Mix]", "a genuine bracketed title is kept");

    // One chart and nothing to compare against: the word list decides.
    check({"Daphnis [Basic]"}, "Daphnis", "single chart, a known difficulty word");
    check({"ANNIHILATER [10036_1]"}, "ANNIHILATER", "single chart, a numeric id");
    check({"Nightmare [Extended Mix]"}, "Nightmare [Extended Mix]",
          "single chart, a real bracketed title is kept");
    check({"Plain Title"}, "Plain Title", "single chart, no bracket at all");

    // Shapes the rule must refuse rather than guess at.
    check({"Song A [Basic]", "Song B [Master]"}, "Song A",
          "different titles: only the word list applies");
    check({"[Basic]"}, "[Basic]", "a title that is nothing but a tag is kept");
    check({}, "", "no charts at all");

    // An untitled chart among tagged ones does not disqualify the song.
    check({"Track [Basic]", "", "Track [Master]"}, "Track", "an untitled chart is ignored");

    // Trailing whitespace before the tag.
    check({"Spaced  [Basic]", "Spaced [Master]"}, "Spaced", "whitespace before the tag");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
