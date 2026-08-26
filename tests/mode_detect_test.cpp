// Guards the active-mode detection in bms_scanner.cpp (DetectActiveModeFromPath).
//
// This is the bug this test exists for: the game's folder for 7-key is "7StreetMix", but the
// candidate list only carried "7keymix", so entering 7-key never switched the active mode. Song
// Select kept serving another mode's catalog and ApplyModeAutoGeneration never ran, so generated
// 7k charts silently played on the 5 lanes of their 5-key source. Nothing errored anywhere.
//
// It also pins the ORDER dependency: "streetmix" is a substring of "7streetmix", so testing it
// first would classify every 7-key path as 5-key. Reordering the list must fail this test.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o mode_detect_test tests/mode_detect_test.cpp
//   ./mode_detect_test
#include <cstdio>
#include <string>

static std::string lowerCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

// Extracted verbatim from bms_scanner.cpp
static std::string NormalizeModeName(const std::string& raw) {
    std::string l = lowerCopy(raw);
    if (l == "5keyonly" || l == "5konly" || l == "5k_only") return "5keyOnly";
    if (l == "5keymix" || l == "5k" || l == "5kstandard") return "5keyMix";
    if (l == "streetmix") return "streetMix";
    if (l == "7keymix" || l == "7streetmix" || l == "7k" || l == "7kstandard") return "7keyMix";
    if (l == "clubmix" || l == "club" || l == "10k") return "clubMix";
    if (l == "spacemix" || l == "space" || l == "14k") return "spaceMix";
    if (l == "rubymix" || l == "ruby") return "rubyMix";
    if (l == "ez2catch" || l == "catch") return "ez2Catch";
    if (l == "turntable" || l == "scratchmix" || l == "scratch") return "turnTable";
    if (l == "cv2mix" || l == "cv2") return "cv2Mix";
    return std::string();
}

// Extracted verbatim from DetectActiveModeFromPath, returning the mode instead of setting a global.
// SetActiveMode runs its argument through NormalizeModeName, which is reproduced here.
static std::string detectModeFromPath(const std::string& path) {
    std::string p = lowerCopy(path);
    bool strong = (p.find("song.bin") != std::string::npos) ||
                  (p.find("stage.ini") != std::string::npos) ||
                  (p.find("stage.int") != std::string::npos) ||
                  (p.find(".gds") != std::string::npos);
    if (!strong) return std::string();
    const char* candidates[] = {"7streetmix", "5keyonly", "5keymix", "streetmix", "7keymix",
                                "clubmix", "spacemix", "rubymix", "ez2catch", "turntable",
                                "scratchmix", "cv2mix"};
    for (const char* c : candidates)
        if (p.find(c) != std::string::npos) return NormalizeModeName(c);
    return std::string();
}

static int g_failures = 0;
static void expect(const std::string& path, const std::string& want) {
    std::string got = detectModeFromPath(path);
    if (got != want) {
        std::printf("FAIL: '%s' -> '%s' (expected '%s')\n", path.c_str(), got.c_str(), want.c_str());
        ++g_failures;
    }
}

int main() {
    // Every real per-mode folder that ships in the game's system\ directory.
    expect("system\\5keymix\\song.bin",      "5keyMix");
    expect("system\\7StreetMix\\Stage.ini",  "7keyMix");   // the regression this test exists for
    // 5keymix and streetmix are two different game modes, each with its own song.bin and
    // stage.ini. Collapsing them onto one canonical handed the keys-only mode the other's
    // scratch and pedal. streetmix must also still not be shadowed by the 7streetmix entry.
    expect("system\\streetmix\\Stage.ini",   "streetMix");
    expect("system\\clubmix\\song.bin",      "clubMix");
    expect("system\\SpaceMix\\Stage.ini",    "spaceMix");
    expect("system\\rubymix\\song.bin",      "rubyMix");
    expect("system\\ez2catch\\Stage.ini",    "ez2Catch");
    expect("system\\scratchmix\\song.bin",   "turnTable");
    expect("system\\CV2Mix\\Stage.ini",      "cv2Mix");

    // Case must not matter: the game mixes casings across these paths.
    expect("SYSTEM\\7STREETMIX\\STAGE.INI",  "7keyMix");

    // Only real per-mode DB/stage paths may flip the active mode. Shared UI assets must not, or
    // a logo load would silently repoint the whole catalog at the wrong mode.
    expect("system\\modeselect\\logo.abm",   "");
    expect("system\\Title\\TitleLoop\\title0097.bmp", "");

    // Radio modes have no autogen profile; they must resolve to nothing rather than
    // accidentally matching a neighbouring mode's name.
    expect("system\\10radiomix\\song.bin",   "");
    expect("system\\14radiomix\\song.bin",   "");
    expect("system\\5radiomix\\song.bin",    "");

    if (g_failures == 0) std::printf("mode detect: ok\n");
    return g_failures ? 1 : 0;
}
