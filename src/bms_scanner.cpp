#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "bms_scanner.h"
#include "config.h"
#include "logger.h"
#include "fnex_crypto.h"
#include "progress_popup.h"
#include "audio_decode.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <map>
#include <memory>
#include <array>
#include <cstdlib>
#include <cmath>

static std::vector<CustomBmsSong> g_songs;
static std::string g_activeMode;
static int g_scanTotal = 0;   // song folders to process this scan — drives the popup bar
static bool g_directBmsEnabled = false;
static std::string g_directBmsVideoPath;
static uint32_t g_directBmsVideoStartMs = 0;
// True when the clip came from the generic Video\_spv pool rather than the song's own BGA.
// Those are shorter than most songs, so they are the ones worth looping.
static bool g_directBmsVideoFallback = false;

void SetDirectBmsEnabled(bool enabled) { g_directBmsEnabled = enabled; }
bool DirectBmsEnabled() { return g_directBmsEnabled; }
void ResetDirectBmsVideo() { StopDirectBmsFfmpegVideo(); g_directBmsVideoPath.clear();
                             g_directBmsVideoStartMs = 0; g_directBmsVideoFallback = false; }
const std::string& DirectBmsVideoPath() { return g_directBmsVideoPath; }
uint32_t DirectBmsVideoStartMs() { return g_directBmsVideoStartMs; }
bool DirectBmsVideoIsFallback() { return g_directBmsVideoFallback; }

// Forward declarations for helpers used by the audio cache block.
static bool fileExistsLocal(const std::string& path);
static std::string fileNameOnlyLocal(std::string p);
static std::string stripQuotes(std::string s);

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
    return s;
}

static bool endsWith(const std::string& s, const char* ext) {
    std::string l = lowerCopy(s);
    std::string e = lowerCopy(ext);
    return l.size() >= e.size() && l.compare(l.size()-e.size(), e.size(), e) == 0;
}

static bool endsWithAny(const std::string& s, const char* const* exts, size_t n) {
    for (size_t i=0;i<n;i++) if (endsWith(s, exts[i])) return true;
    return false;
}

static bool endsWithBms(const std::string& s) {
    const char* exts[] = {".bms", ".bme", ".bml"};
    return endsWithAny(s, exts, 3);
}

static bool endsWithAudio(const std::string& s) {
    const char* exts[] = {".wav", ".ogg", ".mp3", ".flac", ".aiff", ".aif", ".m4a", ".aac",
                          ".wma", ".mp2", ".ac3", ".ape", ".wv", ".opus"};
    return endsWithAny(s, exts, sizeof(exts) / sizeof(exts[0]));
}

static bool endsWithImage(const std::string& s) {
    const char* exts[] = {".png", ".jpg", ".jpeg", ".gif", ".tga", ".bmp", ".abm"};
    return endsWithAny(s, exts, 7);
}

static bool endsWithVideo(const std::string& s) {
    const char* exts[] = {".mp4", ".m4v", ".mov", ".avi", ".wmv", ".asf", ".mpg", ".mpeg",
                          ".mpe", ".m1v", ".m2v", ".vob", ".ts", ".m2ts", ".mts", ".mkv",
                          ".webm", ".ogv", ".flv", ".3gp", ".3g2", ".rm", ".rmvb", ".divx",
                          ".ogm", ".spv"};
    return endsWithAny(s, exts, sizeof(exts) / sizeof(exts[0]));
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ' || s.back()=='\t')) s.pop_back();
    size_t i=0; while (i<s.size() && (s[i]==' ' || s[i]=='\t')) ++i;
    return s.substr(i);
}

std::string NormalizeModeName(const std::string& raw) {
    std::string l = lowerCopy(raw);
    if (l == "5keyonly" || l == "5konly" || l == "5k_only") return "5keyOnly";
    // "5keymix" and "streetmix" are SEPARATE game modes: each ships its own song.bin, stage.ini,
    // .gds and panel, and its own per-song charts (streetmix even carries difficulties 5keymix does
    // not). Collapsing both into one canonical made the mod serve a single profile to both, so the
    // keys-only mode inherited the other's scratch/pedal and its custom charts came out wrong.
    // Which of the two is the keys-only one is NOT decided here -- each canonical learns its own
    // native layout, and the model enables or disables scratch/pedal from that.
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

bool IsKnownModeFolder(const std::string& raw) { return !NormalizeModeName(raw).empty(); }

void SetActiveMode(const std::string& mode, const char* reason) {
    std::string n = NormalizeModeName(mode);
    if (n.empty()) n = mode;
    if (n.empty()) return;
    if (_stricmp(g_activeMode.c_str(), n.c_str()) != 0) {
        g_activeMode = n;
        LogLine("[mode] active='%s' reason='%s'", g_activeMode.c_str(), reason ? reason : "?");
    }
}

void DetectActiveModeFromPath(const char* path, const char* reason) {
    if (!path) return;
    std::string p = lowerCopy(path);
    // Avoid ModeSelect/logo assets causing false active-mode flips. Only trust real per-mode DB/stage/director paths.
    bool strong = (p.find("song.bin") != std::string::npos) ||
                  (p.find("stage.ini") != std::string::npos) ||
                  (p.find("stage.int") != std::string::npos) ||
                  (p.find(".gds") != std::string::npos);
    if (!strong) return;
    // These must be the folder names the GAME uses, not the mod's canonical mode names — the two
    // differ for 7-key, whose folder is "7StreetMix". The old list only had "7keymix", so entering
    // 7-key never switched the active mode: SongSelect kept serving the previous mode's catalog and
    // ApplyModeAutoGeneration never ran, leaving generated 7k charts on the source's 5 lanes.
    // SetActiveMode maps each of these through NormalizeModeName, so aliases are fine here.
    // ORDER MATTERS: "7streetmix" must be tested before "streetmix", which is a substring of it.
    const char* candidates[] = {"7streetmix", "5keyonly", "5keymix", "streetmix", "7keymix",
                                "clubmix", "spacemix", "rubymix", "ez2catch", "turntable",
                                "scratchmix", "cv2mix"};
    for (const char* c : candidates) {
        if (p.find(c) != std::string::npos) {
            SetActiveMode(c, reason);
            return;
        }
    }
}

const std::string& GetActiveMode() { return g_activeMode; }

// BMS #TITLE carries the chart difficulty as a trailing "[EASY]"/"[HARD]"/... tag (e.g.
// "Aboriginal [EASY]"). The songname image is per-song, so drop a trailing bracket whose
// content is a known difficulty word — leaves real bracketed titles ("[Extended Mix]") alone.
static std::string stripDifficultyTag(const std::string& title) {
    std::string t = title;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (t.empty() || t.back() != ']') return t;
    size_t open = t.rfind('[');
    if (open == std::string::npos) return t;
    std::string inner = t.substr(open + 1, t.size() - open - 2);
    std::string up;
    for (char c : inner) if (c != ' ' && c != '\t') up += (char)toupper((unsigned char)c);
    static const char* diffs[] = {"EASY","NORMAL","NM","HARD","HD","HYPER","ANOTHER","INSANE",
        "BEGINNER","LIGHT","EX","EXTRA","MX","MAXIMUM","SHD","CHAOS","BLACK","WHITE","SP","DP"};
    for (const char* d : diffs) if (up == d) {
        std::string base = t.substr(0, open);
        while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
        return base.empty() ? t : base;
    }
    return t;
}

static void parseBmsMeta(const std::string& path, CustomBmsChart& meta) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char line[1024];
    int n = 0;
    // wavRefs/bmpRefs are NOT collected: the direct loader re-parses the selected BMS at play time.
    // This pass only needs selector metadata, which sits above the measure data —
    // stop at the first data line (#nnnCC:...) instead of reading thousands of event lines per chart.
    while (fgets(line, sizeof(line), f) && n++ < 4000) {
        std::string s = trim(line);
        if (s.size() < 2 || s[0] != '#') continue;
        if (isdigit((unsigned char)s[1]) && s.size() > 6 && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && s.find(':') != std::string::npos)
            break; // measure data starts — headers are done
        std::string u = s;
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c){ return (char)toupper(c); });
        if (u.rfind("#TITLE", 0) == 0 && meta.title.empty()) meta.title = trim(s.substr(6));
        else if (u.rfind("#ARTIST", 0) == 0 && meta.artist.empty()) meta.artist = trim(s.substr(7));
        else if (u.rfind("#BPM", 0) == 0 && meta.bpm.empty() && s.size() > 4 && !isdigit((unsigned char)s[4])) meta.bpm = trim(s.substr(4));
        else if (u.rfind("#PLAYLEVEL", 0) == 0 && meta.playLevel.empty()) meta.playLevel = trim(s.substr(10));
        else if (u.rfind("#STAGEFILE", 0) == 0 && meta.stageFile.empty()) meta.stageFile = stripQuotes(s.substr(10));
        else if (u.rfind("#BANNER", 0) == 0 && meta.banner.empty()) meta.banner = stripQuotes(s.substr(7));
    }
    fclose(f);
}



static bool IsLikelyChartForMode(const std::string& file, const std::string& mode) {
    (void)file;
    // The mode folder is authoritative; filenames are not a file format.
    return !NormalizeModeName(mode).empty();
}

static std::string inferDifficulty(const std::string& file) {
    std::string l = lowerCopy(file);
    if (l.find("_nm") != std::string::npos || l.find("normal") != std::string::npos || l.find("easy") != std::string::npos) return "NM";
    if (l.find("_hd") != std::string::npos || l.find("hard") != std::string::npos) return "HD";
    if (l.find("_shd") != std::string::npos || l.find("_mx") != std::string::npos || l.find("maximum") != std::string::npos || l.find("chaos") != std::string::npos) return "SHD";
    if (l.find("_ex") != std::string::npos || l.find("extra") != std::string::npos) return "EX";
    return "";
}

static void ensureDir(const std::string& p) { CreateDirectoryA(p.c_str(), nullptr); }

static void writeTextFile(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}



struct ParsedBmsEvent {
    int measure = 0;
    int channel = 0;
    int index = 0;
    int div = 1;
    int wavId = 0;
};

struct ParsedBmsTempoEvent {
    int measure = 0;
    int index = 0;
    int div = 1;
    double bpm = 120.0;
};

struct ParsedBmsForEz {
    std::string title;
    std::string artist;
    double bpm = 120.0;
    int playLevel = 1;
    std::vector<ParsedBmsEvent> events;
    std::vector<ParsedBmsTempoEvent> tempoEvents;
    std::vector<double> measureScale;
    std::vector<std::pair<int, std::string>> wavMap;
    std::map<int, std::string> bmpMap;
    std::string directVideo;
    std::set<int> bgmWavIds; // sample ids referenced from BMS #xxx01 BGM channel
    std::map<int, double> bpmMap;
    int lnObjId = 0;
};

static int base36Value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    return 0;
}

static int parseBase36_2(const std::string& s, size_t pos) {
    if (pos + 1 >= s.size()) return 0;
    return base36Value(s[pos]) * 36 + base36Value(s[pos + 1]);
}

static int parseIntSafe(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    return atoi(s.c_str());
}

static double parseDoubleSafe(const std::string& s, double fallback) {
    if (s.empty()) return fallback;
    return atof(s.c_str());
}

static std::string stripQuotes(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string bmsDirectiveValue(const std::string& line, size_t prefixLength) {
    size_t pos = prefixLength;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '=' || line[pos] == ':'))
        ++pos;
    return stripQuotes(trim(line.substr(pos)));
}

static bool parseBmsForEz(const std::string& path, ParsedBmsForEz& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] != '#') continue;
        std::string u = s;
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c){ return (char)toupper(c); });
        if (u.rfind("#TITLE", 0) == 0 && out.title.empty()) out.title = stripQuotes(s.substr(6));
        else if (u.rfind("#ARTIST", 0) == 0 && out.artist.empty()) out.artist = stripQuotes(s.substr(7));
        else if (u.rfind("#BPM", 0) == 0 && s.size() > 4 && !isalnum((unsigned char)s[4])) out.bpm = parseDoubleSafe(trim(s.substr(4)), out.bpm);
        else if (u.rfind("#BPM", 0) == 0 && s.size() >= 7) {
            size_t sp = s.find_first_of(" \t", 4);
            if (sp != std::string::npos) {
                int id = parseBase36_2(s, 4);
                double bpm = parseDoubleSafe(trim(s.substr(sp + 1)), 0.0);
                if (id > 0 && bpm > 0.0) out.bpmMap[id] = bpm;
            }
        }
        else if (u.rfind("#LNOBJ", 0) == 0) out.lnObjId = parseBase36_2(trim(s.substr(6)), 0);
        else if (u.rfind("#PLAYLEVEL", 0) == 0) out.playLevel = parseIntSafe(trim(s.substr(10)), out.playLevel);
        else if (u.rfind("#WAV", 0) == 0 && s.size() >= 6) {
            size_t sp = s.find_first_of(" \t", 4);
            if (sp != std::string::npos) {
                int id = parseBase36_2(s, 4);
                std::string file = stripQuotes(trim(s.substr(sp + 1)));
                if (id > 0 && id <= 0x7ff && !file.empty()) out.wavMap.push_back({id, file});
            }
        } else if (u.rfind("#BMP", 0) == 0 && s.size() >= 6) {
            size_t sp = s.find_first_of(" \t=:", 6);
            if (sp != std::string::npos) {
                int id = parseBase36_2(s, 4);
                std::string file = stripQuotes(trim(s.substr(sp + 1)));
                if (id > 0 && id < 1296 && !file.empty()) out.bmpMap[id] = file;
            }
        } else if (u.rfind("#VIDEOFILE", 0) == 0) {
            std::string file = bmsDirectiveValue(s, 10);
            if (!file.empty()) out.directVideo = file;
        } else if (u.rfind("#MOVIE", 0) == 0) {
            std::string file = bmsDirectiveValue(s, 6);
            if (!file.empty()) out.directVideo = file;
        } else if (u.rfind("#VIDEO", 0) == 0 &&
                   (s.size() == 6 || isspace((unsigned char)s[6]) || s[6] == '=' || s[6] == ':')) {
            std::string file = bmsDirectiveValue(s, 6);
            if (!file.empty()) out.directVideo = file;
        } else if (s.size() > 7 && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3])) {
            int measure = (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
            // BMS channel is base-16 (11..1F=P1, 21..2F=P2, 51.. long-notes) — the mapping table
            // bmsChannelToEzTrackMode compares against 0x11/0x51/etc. Reading it as DECIMAL (the old
            // 5-key-only code) misaligned every lane once the map moved to hex → playableIds=0, notes
            // dropped, chart played audio-only. Parse hex to match. Channels 01/02/03/08 are the same
            // in both bases; only the note lanes differ.
            char chpair[3] = { s[4], s[5], 0 };
            int channel = (int)strtol(chpair, nullptr, 16);
            size_t colon = s.find(':');
            if (colon == std::string::npos) continue;
            std::string data = trim(s.substr(colon + 1));
            if (channel == 2) {
                double scale = parseDoubleSafe(data, 1.0);
                if ((int)out.measureScale.size() <= measure) out.measureScale.resize(measure + 1, 1.0);
                out.measureScale[measure] = scale;
                continue;
            }
            if (data.size() < 2 || (data.size() % 2) != 0) continue;
            int div = (int)data.size() / 2;
            for (int i = 0; i < div; ++i) {
                int wav = parseBase36_2(data, (size_t)i * 2);
                if (wav == 0) continue;
                if (channel == 3 || channel == 8) {
                    double bpm = 0.0;
                    if (channel == 3) {
                        char pair[3] = {data[(size_t)i * 2], data[(size_t)i * 2 + 1], 0};
                        bpm = (double)strtoul(pair, nullptr, 16);
                    } else {
                        auto it = out.bpmMap.find(wav);
                        if (it != out.bpmMap.end()) bpm = it->second;
                    }
                    if (bpm > 0.0) out.tempoEvents.push_back({measure, i, div, bpm});
                    continue;
                }
                ParsedBmsEvent ev;
                ev.measure = measure;
                ev.channel = channel;
                ev.index = i;
                ev.div = div;
                ev.wavId = wav;
                out.events.push_back(ev);
                if (channel == 1) out.bgmWavIds.insert(wav);
            }
        }
    }
    fclose(f);
    if (out.title.empty()) out.title = "BMSData";
    if (out.artist.empty()) out.artist = "BMSData";
    if (out.bpm <= 0.0) out.bpm = 120.0;
    return true;
}

// Generalized BMS-channel -> FNEX EZ-track. Verified against decrypted native .ez per mode
// (5key/7street/club/space/scratch/catch/cv2): every mapped track exists in the native layout.
// Long-note channels 5x/6x map like their play channel (0x40 lower). BGM (#01) -> BG lane 22.
// The ONLY per-mode fork is channel 16: it's the SCRATCH (T10) in 5-key/scratch/club/space/cat/cv2,
// but the 6th KEY (T8) in 7-key/space's 7-lane side. Everything else is uniform, so `sevenKey`
// (does this mode have a 6th/7th key column?) is the only parameter. ch = raw BMS channel int.
static int bmsChannelToEzTrackMode(int ch, bool sevenKey) {
    int base = ch;
    if (base >= 0x51 && base <= 0x59) base -= 0x40; // P1 long-note -> P1 play lane
    if (base >= 0x61 && base <= 0x69) base -= 0x40; // P2 long-note -> P2 play lane
    // P1 keys. 11..15 -> T3..T7 always. 16/17: 6th/7th key (T8/T9) on 7-key modes, else scratch/pedal.
    if (base >= 0x11 && base <= 0x15) return 3 + (base - 0x11);
    if (base == 0x16) return sevenKey ? 8 : 10;   // 6th key vs scratch
    if (base == 0x17) return sevenKey ? 9 : 10;   // 7th key vs scratch (non-7k rarely uses 17)
    if (base == 0x18 || base == 0x19) return 11;  // P1 pedal
    // P2 keys 21..25 -> T12..T16; 26/27 -> T17/T18 (7-key P2). Pedal 28/29 -> T19.
    if (base >= 0x21 && base <= 0x27) return 12 + (base - 0x21);
    if (base == 0x28 || base == 0x29) return 19;
    if (ch == 1) return 22; // BMS BGM channel -> first FNEX BG lane.
    return -1;
}
// 5-key wrapper kept for the existing call sites; 5-key has no 6th/7th key so sevenKey=false.
static bool g_curModeSevenKey = false;
static std::string g_curGenerateMode = "5keyOnly";
static bool g_curAutoGenerate = false;
// Set for the SOURCE mode's own charts: extract scratch/pedal, but never move the authored notes.
static bool g_curAutoEnrich = false;
static int bmsChannelToEzTrack5Key(int ch) { return bmsChannelToEzTrackMode(ch, g_curModeSevenKey); }

static int bmsEventToEzTrack5Key(const ParsedBmsEvent& e) {
    int tr = bmsChannelToEzTrack5Key(e.channel);
    if (e.channel == 1 && tr >= 0) {
        // Spread BGM objects over BG 1..10, matching native EZ files better than
        // putting all #01 events into a single giant background lane.
        tr = 22 + (e.wavId % 10);
    }
    return tr;
}

// ch is the base-16 BMS channel now (see parseBmsForEz). Long-note lanes are 0x51..0x59 (P1) and
// 0x61..0x69 (P2), matching bmsChannelToEzTrackMode's −0x40 fold.
static bool isLongNoteChannel(int ch) { return (ch >= 0x51 && ch <= 0x59) || (ch >= 0x61 && ch <= 0x69); }

// Ticks per 4/4 measure in the .ez charts we emit.
//
// The shipped charts use 192, and for a long time so did we -- but BMS files are not authored on
// that grid. Measured over 45,707 notes from 80 of the installed charts, the dominant subdivision
// is 1/384 of a measure: 192/384 = 0.5, so every odd position lands exactly between two ticks and
// rounds. 24% of all notes were displaced, by a mean of 4.17 ms and essentially always the full
// half-tick. Native charts sit exactly on their grid and have none of that, which is why custom
// charts felt harder to time than native ones at the same level.
//
// 384 makes the dominant case exact and halves the worst case for everything else. The engine
// honours the field rather than assuming it -- sub_411BA0 reads the resolution from the song
// package at +0x88 and derives ticks-per-beat as resolution/4 -- and 192 is only the value its
// own loader writes as a default.
static const uint32_t kEzTicksPerMeasure = 384;
// What the shipped charts use, and therefore the unit of every tick statistic the native model
// learned. Gaps taken from the model have to be scaled before they are applied to charts of ours.
static const uint32_t kNativeTicksPerMeasure = 192;
static const uint32_t kModelTickScale = kEzTicksPerMeasure / kNativeTicksPerMeasure;

// Every shipped tap note carries a length, and it is never zero. Decrypting 5keymix1p-10anti-hd.ez
// and counting: 1988 of its 2067 notes are type 1 with len=6, and the rest are long notes with
// 36/48/72/96/120 -- so 6 is not a duration anyone authored, it is the floor the format uses for
// an instantaneous note.
//
// The field is NOT in grid units and the engine never rescales it: Panel::m42b2f0 (decomp
// gamepanels.cpp) routes a hit to the hold drawer when `st.width > 6` and to the tap drawer
// otherwise, straight off the record's u16. So the floor is the native 6 whatever our grid is;
// scaling it to 12 "for the 384 grid" turned every custom tap into a hold.
static const int kMinNoteLenTicks = 6;

static uint32_t bmsTickToEzTick(const ParsedBmsForEz& bms, int measure, int index, int div) {
    double pos = 0.0;
    for (int m = 0; m < measure; ++m) {
        double scale = 1.0;
        if (m < (int)bms.measureScale.size()) scale = bms.measureScale[m];
        pos += (double)kEzTicksPerMeasure * scale;
    }
    double scale = 1.0;
    if (measure < (int)bms.measureScale.size()) scale = bms.measureScale[measure];
    pos += ((double)index / (double)div) * (double)kEzTicksPerMeasure * scale;
    if (pos < 0.0) pos = 0.0;
    return (uint32_t)(pos + 0.5);
}

static void putU16(std::vector<unsigned char>& b, size_t off, uint16_t v) {
    if (b.size() < off + 2) b.resize(off + 2);
    b[off] = (unsigned char)(v & 0xff);
    b[off+1] = (unsigned char)((v >> 8) & 0xff);
}

static void putU32(std::vector<unsigned char>& b, size_t off, uint32_t v) {
    if (b.size() < off + 4) b.resize(off + 4);
    b[off] = (unsigned char)(v & 0xff);
    b[off+1] = (unsigned char)((v >> 8) & 0xff);
    b[off+2] = (unsigned char)((v >> 16) & 0xff);
    b[off+3] = (unsigned char)((v >> 24) & 0xff);
}

static void putFloat(std::vector<unsigned char>& b, size_t off, float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    putU32(b, off, u);
}

static void putFixedString(std::vector<unsigned char>& b, size_t off, size_t len, const std::string& s) {
    if (b.size() < off + len) b.resize(off + len);
    size_t n = std::min(len - 1, s.size());
    memcpy(&b[off], s.data(), n);
}

struct EzRawEventOut {
    uint32_t tick;
    int track;
    int wav;
    int len;
    int ch;
    int type;
    float bpm;
};

static bool writeBinaryFile(const std::string& path, const std::vector<unsigned char>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

// Preview = a random `seconds`-long window of the BGM SSF, not the start. SSF layout:
// 18-byte header [u16 ch][u32 rate][u32 avg][u16 align][u16 bits][u32 dataSize] + raw PCM.
// We keep the header (adjust dataSize) and copy a frame-aligned slice from a random offset.
static bool MakeRandomPreviewSsf(const std::string& src, const std::string& dst, uint32_t seconds) {
    FILE* f = fopen(src.c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[18];
    if (fread(hdr, 1, 18, f) != 18) { fclose(f); return false; }
    uint32_t rate = 0, avg = 0, dataSize = 0; uint16_t align = 0;
    memcpy(&rate, hdr + 2, 4); memcpy(&avg, hdr + 6, 4);
    memcpy(&align, hdr + 10, 2); memcpy(&dataSize, hdr + 14, 4);
    if (align == 0 || avg == 0) { fclose(f); return false; }
    uint32_t want = avg * seconds;                 // bytes for `seconds` of audio
    if (dataSize <= want) { fclose(f); return false; } // shorter than window → caller copies whole
    uint32_t maxStart = dataSize - want;
    uint32_t start = (uint32_t)(((uint64_t)rand() * 1103515245u + GetTickCount()) % maxStart);
    start -= start % align;                          // frame-align
    std::vector<unsigned char> out(18 + want);
    memcpy(out.data(), hdr, 18);
    uint32_t newSize = want;
    memcpy(out.data() + 14, &newSize, 4);            // patch dataSize to the window
    if (fseek(f, 18 + start, SEEK_SET) != 0 || fread(out.data() + 18, 1, want, f) != want) { fclose(f); return false; }
    fclose(f);
    return writeBinaryFile(dst, out);
}

static bool validateGeneratedEz(const std::vector<unsigned char>& b) {
    if (b.size() < 0x96 || memcmp(b.data(), "EZFF", 4) != 0) return false;
    uint16_t tracks = 0;
    uint32_t totalTicks = 0;
    float seconds = 0.0f;
    memcpy(&tracks, b.data() + 0x8c, 2);
    memcpy(&totalTicks, b.data() + 0x8e, 4);
    memcpy(&seconds, b.data() + 0x92, 4);
    if (tracks != 64 || totalTicks == 0 || seconds <= 0.0f) return false;
    size_t off = 0x96;
    bool bpmFound = false;
    for (uint16_t t = 0; t < tracks; ++t) {
        if (off + 0x4e > b.size() || memcmp(b.data() + off, "EZTR", 4) != 0) return false;
        uint32_t bytes = 0;
        memcpy(&bytes, b.data() + off + 0x4a, 4);
        if ((bytes % 13) != 0 || off + 0x4e + bytes > b.size()) return false;
        for (size_t e = off + 0x4e; e < off + 0x4e + bytes; e += 13) {
            if (b[e + 4] == 3) bpmFound = true;
            if (b[e + 4] == 1 && (b[e + 8] != 0x40 || b[e + 9] != 0 || (b[e + 10] == 0 && b[e + 11] == 0))) return false;
        }
        off += 0x4e + bytes;
    }
    return bpmFound && off == b.size();
}


// Runtime-key encrypt: reads key from the loaded game binary at keyRva (stride-4 table).
// This directly mirrors the game's decrypt functions (FUN_00410a40, FUN_00410c90, FUN_004135f0)
// which all use key[k*4] from their respective key tables. RVAs:
//   EZ  key: 0x0a74b0  (FUN_00410a40, line: *(char *)(iVar5 + 0x4a74b0))
//   EZI key: 0x0a7cb0  (FUN_00410c90, line: *(char *)(iVar5 + 0x4a7cb0))
//   INI key: 0x0a8568  (FUN_004135f0, line: (&DAT_004a8568)[uVar10 * 4])
static std::vector<unsigned char> encryptFnexRuntimeKey(const unsigned char* data, size_t n, size_t keyRva) {
    const unsigned char* key = reinterpret_cast<const unsigned char*>(GetModuleHandleA(NULL)) + keyRva;
    std::vector<unsigned char> tmp(n, 0);
    size_t off = 0;
    int keyIndex = 0;
    bool wideBlock = false;
    while (off < n) {
        size_t len;
        if (n < off + 8) {
            len = n - off;
        } else {
            len = wideBlock ? 16 : 8;
            if (n < off + len) len = n - off;
            wideBlock = !wideBlock;
        }
        for (size_t u = 0; u < len; ++u) {
            int k = keyIndex + static_cast<int>(len) - static_cast<int>(u) - 1;
            if (k > 0x1ff) k -= 0x200;
            tmp[off + len - 1 - u] = static_cast<unsigned char>((data[off + u] + key[k * 4]) & 0xff);
        }
        keyIndex += static_cast<int>(len);
        if (keyIndex > 0x1ff) keyIndex -= 0x200;
        if (len > 8) off = (off - 8) + len;
        off += 8;
    }
    std::reverse(tmp.begin(), tmp.end());
    return tmp;
}


static std::vector<unsigned char> decryptFnexRuntimeKey(const unsigned char* encrypted, size_t n, size_t keyRva) {
    std::vector<unsigned char> tmp(n, 0), plain(n, 0);
    if (!encrypted || !n) return plain;
    std::reverse_copy(encrypted, encrypted + n, tmp.begin());
    const unsigned char* key = reinterpret_cast<const unsigned char*>(GetModuleHandleA(NULL)) + keyRva;
    size_t off = 0;
    int keyIndex = 0;
    bool wideBlock = false;
    while (off < n) {
        size_t len;
        if (n < off + 8) len = n - off;
        else {
            len = wideBlock ? 16 : 8;
            if (n < off + len) len = n - off;
            wideBlock = !wideBlock;
        }
        for (size_t u = 0; u < len; ++u) {
            int k = keyIndex + static_cast<int>(len) - static_cast<int>(u) - 1;
            if (k > 0x1ff) k -= 0x200;
            plain[off + u] = static_cast<unsigned char>(tmp[off + len - 1 - u] - key[k * 4]);
        }
        keyIndex += static_cast<int>(len);
        if (keyIndex > 0x1ff) keyIndex -= 0x200;
        if (len > 8) off = (off - 8) + len;
        off += 8;
    }
    return plain;
}

static bool writeEncryptedFileRuntime(const std::string& path, const unsigned char* data, size_t size, size_t keyRva) {
    std::vector<unsigned char> enc = encryptFnexRuntimeKey(data, size, keyRva);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    if (!enc.empty()) fwrite(enc.data(), 1, enc.size(), f);
    fclose(f);
    return true;
}

static bool writeEncryptedTextFileRuntime(const std::string& path, const std::string& text, size_t keyRva) {
    return writeEncryptedFileRuntime(path, reinterpret_cast<const unsigned char*>(text.data()), text.size(), keyRva);
}

static bool writeEncryptedEziFile(const std::string& path, const std::string& plain) {
    return writeEncryptedTextFileRuntime(path, plain, 0x0a7cb0);
}


// ---- Native-chart statistical model + procedural mode generator ---------------------
// The model is intentionally lightweight and deterministic: native FNEX charts teach the
// timing distribution and density of scratch/pedal/second-side notes, while the source BMS
// supplies the actual keysounds. No external ML runtime or model file is required.
struct NativeModeModel {
    std::string mode;
    uint32_t charts = 0;
    uint64_t notes = 0;
    uint64_t scratchNotes = 0;
    uint64_t pedalNotes = 0;
    uint64_t secondSideNotes = 0;
    uint64_t longNotes = 0;
    uint64_t scratchLongNotes = 0;
    uint64_t pedalLongNotes = 0;
    uint64_t scratchChordNotes = 0;
    uint64_t pedalChordNotes = 0;
    uint64_t scratchGapSum = 0;
    uint64_t scratchGapCount = 0;
    uint64_t pedalGapSum = 0;
    uint64_t pedalGapCount = 0;
    std::array<uint64_t, 16> scratchPhase{};
    std::array<uint64_t, 16> pedalPhase{};
    // Per-track usage lets the generator discover the actual FNEX lane layout for each mode
    // instead of assuming that scratch/pedal/P2 always occupy the same numeric tracks.
    std::array<uint64_t, 64> primaryTrackNotes{};
    std::array<uint64_t, 64> secondTrackNotes{};
    std::array<uint64_t, 64> scratchTrackNotes{};
    std::array<uint64_t, 64> pedalTrackNotes{};
};

static std::map<std::string, NativeModeModel> g_nativeModeModels;
static bool g_nativeModeModelsReady = false;

static uint16_t modelReadU16(const unsigned char* p) {
    uint16_t v = 0; std::memcpy(&v, p, sizeof(v)); return v;
}
static uint32_t modelReadU32(const unsigned char* p) {
    uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); return v;
}

static bool readWholeFileForModel(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    if (size <= 0 || size > 64 * 1024 * 1024) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(size));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

static std::string inferNativeModeFromPath(const std::string& raw) {
    std::string p = lowerCopy(raw);
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.find("5keyonly") != std::string::npos || p.find("5konly") != std::string::npos) return "5keyOnly";
    if (p.find("7streetmix") != std::string::npos || p.find("7keymix") != std::string::npos) return "7keyMix";
    if (p.find("clubmix") != std::string::npos) return "clubMix";
    if (p.find("spacemix") != std::string::npos) return "spaceMix";
    if (p.find("rubymix") != std::string::npos) return "rubyMix";
    if (p.find("catch1p") != std::string::npos || p.find("ez2catch") != std::string::npos) return "ez2Catch";
    if (p.find("scratchmix") != std::string::npos || p.find("turntable") != std::string::npos) return "turnTable";
    if (p.find("cv2mix") != std::string::npos) return "cv2Mix";
    if (p.find("5keymix") != std::string::npos) return "5keyMix";
    if (p.find("streetmix") != std::string::npos) return "streetMix"; // 7streetmix already returned above
    return std::string();
}

// Native FNEX charts store lane = the EZTR BLOCK INDEX (0..63); the u16 at +0x04 is always 0 and the
// name string is empty (verified by decrypting real SOUND\*.ez). So role is classified by block number,
// using the same lane map our own EZ writer emits: 10/20=scratch, 11/19=pedal, 12-18=P2 keys, 3-9=keys.
static int nativeTrackRole(int track) {
    if (track == 10 || track == 20) return 1;
    if (track == 11 || track == 19 || track == 21) return 2;
    if (track >= 12 && track <= 18) return 3;
    if (track >= 3 && track <= 9) return 0;
    return -1;
}

static bool accumulateNativeEzModel(const std::string& mode, const std::vector<unsigned char>& ez) {
    if (ez.size() < 0x96 || std::memcmp(ez.data(), "EZFF", 4) != 0 || modelReadU16(ez.data() + 0x8c) != 64)
        return false;

    struct NativeNote { uint32_t tick; uint16_t len; int role; int track; };
    std::vector<NativeNote> notes;
    std::vector<uint32_t> scratchTicks, pedalTicks;
    size_t off = 0x96;
    for (int block = 0; block < 64; ++block) {
        if (off + 0x4e > ez.size() || std::memcmp(ez.data() + off, "EZTR", 4) != 0) return false;
        int track = block; // native lane number IS the block index; +0x04 field is unused (always 0)
        uint32_t bytes = modelReadU32(ez.data() + off + 0x4a);
        int role = nativeTrackRole(track);
        off += 0x4e;
        if (bytes % 13 != 0 || off + bytes > ez.size()) return false;
        for (uint32_t i = 0; i < bytes / 13; ++i, off += 13) {
            if (ez[off + 4] != 1 || role < 0) continue;
            NativeNote n{modelReadU32(ez.data() + off), modelReadU16(ez.data() + off + 10), role, track};
            notes.push_back(n);
            if (role == 1) scratchTicks.push_back(n.tick);
            if (role == 2) pedalTicks.push_back(n.tick);
        }
    }
    if (off != ez.size() || notes.empty()) return false;

    std::map<uint32_t, uint32_t> chordSize;
    for (const auto& n : notes) ++chordSize[n.tick];
    NativeModeModel& model = g_nativeModeModels[mode];
    model.mode = mode;
    ++model.charts;
    for (const auto& n : notes) {
        ++model.notes;
        if (n.len > 6) ++model.longNotes;
        int phase = static_cast<int>((n.tick % 48u) * 16u / 48u);
        if (phase < 0) phase = 0;
        if (phase > 15) phase = 15;
        if (n.role == 1) {
            ++model.scratchNotes;
            if (n.track >= 0 && n.track < 64) ++model.scratchTrackNotes[n.track];
            if (n.len > 6) ++model.scratchLongNotes;
            if (chordSize[n.tick] > 1) ++model.scratchChordNotes;
            ++model.scratchPhase[phase];
        } else if (n.role == 2) {
            ++model.pedalNotes;
            if (n.track >= 0 && n.track < 64) ++model.pedalTrackNotes[n.track];
            if (n.len > 6) ++model.pedalLongNotes;
            if (chordSize[n.tick] > 1) ++model.pedalChordNotes;
            ++model.pedalPhase[phase];
        } else if (n.role == 3) {
            ++model.secondSideNotes;
            if (n.track >= 0 && n.track < 64) ++model.secondTrackNotes[n.track];
        } else if (n.role == 0 && n.track >= 0 && n.track < 64) {
            ++model.primaryTrackNotes[n.track];
        }
    }
    std::sort(scratchTicks.begin(), scratchTicks.end());
    scratchTicks.erase(std::unique(scratchTicks.begin(), scratchTicks.end()), scratchTicks.end());
    for (size_t i = 1; i < scratchTicks.size(); ++i) {
        model.scratchGapSum += scratchTicks[i] - scratchTicks[i - 1];
        ++model.scratchGapCount;
    }
    std::sort(pedalTicks.begin(), pedalTicks.end());
    pedalTicks.erase(std::unique(pedalTicks.begin(), pedalTicks.end()), pedalTicks.end());
    for (size_t i = 1; i < pedalTicks.size(); ++i) {
        model.pedalGapSum += pedalTicks[i] - pedalTicks[i - 1];
        ++model.pedalGapCount;
    }
    return true;
}

static void scanNativeModelDirectory(const std::string& dir, int depth,
                                     std::map<std::string, int>& counts,
                                     std::set<std::string>& seenStems) {
    if (depth > 6) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(JoinPath(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!std::strcmp(fd.cFileName, ".") || !std::strcmp(fd.cFileName, "..")) continue;
        std::string path = JoinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scanNativeModelDirectory(path, depth + 1, counts, seenStems);
            continue;
        }
        std::string low = lowerCopy(path);
        bool isEs = endsWith(low, ".es"), isEz = endsWith(low, ".ez");
        if (!isEs && !isEz) continue;
        std::string mode = inferNativeModeFromPath(path);
        if (mode.empty() || counts[mode] >= Config().nativeModelMaxChartsPerMode) continue;
        std::string stem = low.substr(0, low.size() - (isEs ? 3 : 3));
        if (seenStems.find(stem) != seenStems.end()) continue;
        if (isEz) {
            std::string es = path.substr(0, path.size() - 3) + ".es";
            if (GetFileAttributesA(es.c_str()) != INVALID_FILE_ATTRIBUTES) continue; // prefer plaintext sibling
        }
        std::vector<unsigned char> bytes;
        if (!readWholeFileForModel(path, bytes)) continue;
        if (std::memcmp(bytes.data(), "EZFF", 4) != 0 && isEz)
            bytes = decryptFnexRuntimeKey(bytes.data(), bytes.size(), 0x0a74b0);
        if (!accumulateNativeEzModel(mode, bytes)) continue;
        seenStems.insert(stem);
        ++counts[mode];
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static double modelRatio(uint64_t value, uint64_t total, double fallback, double lo, double hi) {
    if (!total) return fallback;
    double r = static_cast<double>(value) / static_cast<double>(total);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return r;
}

static std::vector<int> modelTracksByUsage(const std::array<uint64_t,64>& usage, size_t limit = 64) {
    std::vector<std::pair<uint64_t,int>> ranked;
    for (int track = 0; track < 64; ++track)
        if (usage[track]) ranked.push_back({usage[track], track});
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<int> out;
    for (const auto& item : ranked) {
        if (out.size() >= limit) break;
        out.push_back(item.second);
    }
    return out;
}

// Tracks the native charts really use, by share of that side's notes, in physical left-to-right
// order. Unlike modelTracksByUsage this does NOT take a count from the caller: the count is exactly
// what the hardcoded profiles kept getting wrong (rubyMix hardcoded 4 lanes where the native charts
// use 5; ez2Catch/cv2Mix had no second side at all while the native charts use one).
static std::vector<int> modelTracksAboveShare(const std::array<uint64_t,64>& usage, double minShare) {
    uint64_t total = 0;
    for (int track = 0; track < 64; ++track) total += usage[track];
    std::vector<int> out;
    if (!total) return out;
    for (int track = 0; track < 64; ++track)
        if (static_cast<double>(usage[track]) / static_cast<double>(total) > minShare) out.push_back(track);
    return out; // already ascending == physical order
}

static std::string tracksToText(const std::vector<int>& tracks) {
    std::string out;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(tracks[i]);
    }
    return out.empty() ? std::string("-") : out;
}

static int modelDominantTrack(const std::array<uint64_t,64>& usage, int fallback, int excluded = -1) {
    int best = fallback;
    uint64_t bestCount = 0;
    for (int track = 0; track < 64; ++track) {
        if (track == excluded) continue;
        if (usage[track] > bestCount) { bestCount = usage[track]; best = track; }
    }
    return best;
}

static void writeNativeModelReport(const std::string& root) {
    std::string dir = JoinPath(GetExeDirectory(), "logs");
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = JoinPath(dir, "EZ2NativeChartModel.txt");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "Native chart model root=%s\r\n", root.c_str());
    for (const auto& kv : g_nativeModeModels) {
        const NativeModeModel& m = kv.second;
        double scratch = m.notes ? 100.0 * m.scratchNotes / m.notes : 0.0;
        double pedal = m.notes ? 100.0 * m.pedalNotes / m.notes : 0.0;
        double p2 = m.notes ? 100.0 * m.secondSideNotes / m.notes : 0.0;
        double sg = m.scratchGapCount ? static_cast<double>(m.scratchGapSum) / m.scratchGapCount : 0.0;
        double pg = m.pedalGapCount ? static_cast<double>(m.pedalGapSum) / m.pedalGapCount : 0.0;
        std::vector<int> scratchTracks = modelTracksByUsage(m.scratchTrackNotes, 2);
        std::vector<int> pedalTracks = modelTracksByUsage(m.pedalTrackNotes, 2);
        std::fprintf(f, "%s charts=%lu notes=%llu scratch=%.3f%% pedal=%.3f%% secondSide=%.3f%% scratchGap=%.2f pedalGap=%.2f tracks[scratch=%d,%d pedal=%d,%d]\r\n",
            m.mode.c_str(), static_cast<unsigned long>(m.charts), static_cast<unsigned long long>(m.notes),
            scratch, pedal, p2, sg, pg,
            scratchTracks.empty() ? -1 : scratchTracks[0], scratchTracks.size() < 2 ? -1 : scratchTracks[1],
            pedalTracks.empty() ? -1 : pedalTracks[0], pedalTracks.size() < 2 ? -1 : pedalTracks[1]);
    }
    std::fclose(f);
}

// The SOUND walk visits the WHOLE native tree (~165k files on a full install) even though it only
// ever keeps nativeModelMaxChartsPerMode charts per mode, and it runs inside the startup gate before
// a single BMS song is scanned. With a warm OS file cache that is ~2s; cold — a cabinet's storage on
// every boot — it is minutes with no progress feedback, which reads as a hang. Cache the learned
// model next to the other mod data (cacheRoot, so it stays drive-relative) and skip the walk when it
// is present. Delete the file to force a relearn after changing SOUND.
static const char* kNativeModelCacheName = "native_model.cache";
static std::string nativeModelCachePath() {
    return JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), kNativeModelCacheName);
}

// Single source of truth for field order so the reader and writer can never drift apart.
template <typename Fn>
static void visitNativeModelFields(NativeModeModel& m, Fn fn) {
    fn(m.notes); fn(m.scratchNotes); fn(m.pedalNotes); fn(m.secondSideNotes);
    fn(m.longNotes); fn(m.scratchLongNotes); fn(m.pedalLongNotes);
    fn(m.scratchChordNotes); fn(m.pedalChordNotes);
    fn(m.scratchGapSum); fn(m.scratchGapCount); fn(m.pedalGapSum); fn(m.pedalGapCount);
    for (auto& v : m.scratchPhase) fn(v);
    for (auto& v : m.pedalPhase) fn(v);
    for (auto& v : m.primaryTrackNotes) fn(v);
    for (auto& v : m.secondTrackNotes) fn(v);
    for (auto& v : m.scratchTrackNotes) fn(v);
    for (auto& v : m.pedalTrackNotes) fn(v);
}

static void writeNativeModelCache() {
    std::string dir = JoinPath(GetExeDirectory(), Config().cacheRoot);
    ensureDir(dir);
    FILE* f = std::fopen(JoinPath(dir, kNativeModelCacheName).c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "EZ2NativeModelCache 2 %lu\n", static_cast<unsigned long>(g_nativeModeModels.size()));
    for (auto& kv : g_nativeModeModels) {
        NativeModeModel& m = kv.second;
        std::fprintf(f, "%s %lu", m.mode.c_str(), static_cast<unsigned long>(m.charts));
        visitNativeModelFields(m, [&](uint64_t& v) {
            std::fprintf(f, " %llu", static_cast<unsigned long long>(v));
        });
        std::fputc('\n', f);
    }
    std::fclose(f);
}

// Parsed with strtoull rather than fscanf: MSVCRT's scanf does not reliably accept %llu, and a
// silent parse failure here would poison the model instead of just missing the cache.
static bool readNativeModelCache() {
    FILE* f = std::fopen(nativeModelCachePath().c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    std::fclose(f);

    std::vector<std::string> tok;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) tok.push_back(cur);

    size_t i = 0;
    auto next = [&]() -> const std::string* { return i < tok.size() ? &tok[i++] : nullptr; };
    const std::string* tag = next();
    const std::string* version = next();
    const std::string* count = next();
    // v2: 5keymix/streetmix became separate modes, so a v1 cache holds a merged 5keyMix model and
    // must be relearned rather than reused.
    if (!tag || !version || !count || *tag != "EZ2NativeModelCache" || *version != "2") return false;
    unsigned long modes = std::strtoul(count->c_str(), nullptr, 10);
    if (modes == 0 || modes > 64) return false;

    std::map<std::string, NativeModeModel> loaded;
    for (unsigned long mi = 0; mi < modes; ++mi) {
        const std::string* name = next();
        const std::string* charts = next();
        if (!name || !charts) return false;
        NativeModeModel m;
        m.mode = *name;
        m.charts = static_cast<uint32_t>(std::strtoul(charts->c_str(), nullptr, 10));
        bool ok = true;
        visitNativeModelFields(m, [&](uint64_t& v) {
            const std::string* s = next();
            if (!s) { ok = false; return; }
            v = std::strtoull(s->c_str(), nullptr, 10);
        });
        if (!ok) return false;
        loaded[m.mode] = m;
    }
    if (loaded.empty()) return false;
    g_nativeModeModels.swap(loaded);
    return true;
}

static void LearnNativeModeModels() {
    if (g_nativeModeModelsReady) return;
    g_nativeModeModelsReady = true;
    if (!Config().learnNativeCharts) {
        LogLine("[autochart-model] native learning disabled");
        return;
    }
    if (readNativeModelCache()) {
        LogLine("[autochart-model] loaded cached model modes=%lu path='%s' (SOUND walk skipped; delete that file to relearn)",
            static_cast<unsigned long>(g_nativeModeModels.size()), nativeModelCachePath().c_str());
        return;
    }
    std::string root = JoinPath(GetExeDirectory(), Config().nativeChartRoot);
    DWORD attr = GetFileAttributesA(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        LogLine("[autochart-model] native root missing '%s'; fallback profiles active", root.c_str());
        return;
    }
    DWORD started = GetTickCount();
    std::map<std::string, int> counts;
    std::set<std::string> seen;
    // No cache yet: this walks the entire SOUND tree and is the single slowest step of startup on
    // cold storage. Say so before blocking, so a long first boot is legible instead of looking hung.
    LogLine("[autochart-model] no cache; walking '%s' (first run only, minutes on cold/slow storage)", root.c_str());
    scanNativeModelDirectory(root, 0, counts, seen);
    writeNativeModelReport(root);
    writeNativeModelCache();
    for (const auto& kv : g_nativeModeModels) {
        const NativeModeModel& m = kv.second;
        LogLine("[autochart-model] mode='%s' charts=%lu notes=%llu scratch=%llu pedal=%llu p2=%llu",
            m.mode.c_str(), static_cast<unsigned long>(m.charts), static_cast<unsigned long long>(m.notes),
            static_cast<unsigned long long>(m.scratchNotes), static_cast<unsigned long long>(m.pedalNotes),
            static_cast<unsigned long long>(m.secondSideNotes));
    }
    LogLine("[autochart-model] learned modes=%lu charts=%lu scanMs=%lu root='%s'",
        static_cast<unsigned long>(g_nativeModeModels.size()), static_cast<unsigned long>(seen.size()),
        static_cast<unsigned long>(GetTickCount() - started), root.c_str());
}

struct AutoModeProfile {
    std::vector<int> primaryKeys;
    std::vector<int> secondaryKeys;
    int scratchTrack = -1;
    int scratchTrack2 = -1;
    int pedalTrack1 = -1;
    int pedalTrack2 = -1;
    double scratchShare = 0.0;
    double pedalShare = 0.0;
    bool turntableOnly = false;
    bool catchApproximation = false;
};

static AutoModeProfile profileForMode(const std::string& rawMode) {
    std::string mode = NormalizeModeName(rawMode);
    AutoModeProfile p;
    if (mode == "5keyOnly") p.primaryKeys = {3,4,5,6,7};
    else if (mode == "5keyMix") {
        p.primaryKeys = {3,4,5,6,7}; p.scratchTrack = 10; p.pedalTrack1 = 11;
        p.scratchShare = 0.085; p.pedalShare = 0.030;
    } else if (mode == "streetMix") {
        // Same shape as the sibling 5-key mode. Whether this one actually has scratch/pedal is
        // settled by its own learned model below, not by this fallback.
        p.primaryKeys = {3,4,5,6,7}; p.scratchTrack = 10; p.pedalTrack1 = 11;
        p.scratchShare = 0.085; p.pedalShare = 0.030;
    } else if (mode == "7keyMix") {
        p.primaryKeys = {3,4,5,6,7,8,9}; p.scratchTrack = 10; p.pedalTrack1 = 11;
        p.scratchShare = 0.070; p.pedalShare = 0.025;
    } else if (mode == "clubMix") {
        p.primaryKeys = {3,4,5,6,7}; p.secondaryKeys = {12,13,14,15,16};
        p.scratchTrack = 10; p.scratchTrack2 = 20; p.pedalTrack1 = 11; p.pedalTrack2 = 19;
        p.scratchShare = 0.050; p.pedalShare = 0.035;
    } else if (mode == "spaceMix") {
        p.primaryKeys = {3,4,5,6,7,8,9}; p.secondaryKeys = {12,13,14,15,16,17,18};
        p.scratchTrack = 10; p.scratchTrack2 = 20; p.pedalTrack1 = 11; p.pedalTrack2 = 19;
        p.scratchShare = 0.040; p.pedalShare = 0.025;
    } else if (mode == "rubyMix") {
        p.primaryKeys = {3,4,5,6};
    } else if (mode == "ez2Catch") {
        p.primaryKeys = {3,4,5,6,7}; p.catchApproximation = true;
    } else if (mode == "turnTable") {
        p.scratchTrack = 10; p.pedalTrack1 = 11; p.scratchShare = 1.0;
        p.pedalShare = 0.025; p.turntableOnly = true;
    } else if (mode == "cv2Mix") {
        p.primaryKeys = {3,4,5,6,7,8,9}; p.scratchTrack = 10; p.pedalTrack1 = 11;
        p.scratchShare = 0.060; p.pedalShare = 0.025;
    } else p.primaryKeys = {3,4,5,6,7};
    return p;
}

static uint32_t autoHash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = a * 0x9E3779B9u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

static double autoPhaseScore(const std::array<uint64_t,16>* phases, uint32_t tick, bool pedal) {
    int bucket = static_cast<int>((tick % 48u) * 16u / 48u);
    if (bucket < 0) bucket = 0;
    if (bucket > 15) bucket = 15;
    if (phases) {
        uint64_t maximum = 0;
        for (uint64_t v : *phases) if (v > maximum) maximum = v;
        if (maximum) return 0.25 + 1.75 * static_cast<double>((*phases)[bucket]) / maximum;
    }
    // Fallback favors beats and eighth notes, with pedals more strongly tied to beat starts.
    uint32_t phase = tick % 48u;
    if (phase == 0) return pedal ? 2.2 : 1.8;
    if (phase == 24) return pedal ? 1.0 : 1.5;
    if (phase == 12 || phase == 36) return pedal ? 0.45 : 1.1;
    return 0.35;
}

static double autoSampleScore(const ParsedBmsForEz& bms, int wav, bool pedal) {
    std::string name;
    for (const auto& pair : bms.wavMap) if (pair.first == wav) { name = lowerCopy(pair.second); break; }
    if (name.empty()) return 0.0;
    double score = 0.0;
    const char* bassWords[] = {"kick","bass","bd","sub","low","pedal"};
    const char* scratchWords[] = {"scratch","scr","turn","vinyl","fx","hit","perc","noise"};
    for (const char* w : bassWords) if (name.find(w) != std::string::npos) score += pedal ? 2.4 : 1.3;
    for (const char* w : scratchWords) if (name.find(w) != std::string::npos) score += pedal ? 0.3 : 2.0;
    return score;
}

struct AutoNotePack { EzRawEventOut event; int sourceTrack; };
struct AutoTickGroup { uint32_t tick; std::vector<AutoNotePack> notes; };

static std::set<uint32_t> selectAutoSpecialTicks(const std::vector<AutoTickGroup>& groups,
                                                  size_t desired, uint32_t minGap,
                                                  const std::array<uint64_t,16>* phases,
                                                  const ParsedBmsForEz& bms, bool pedal,
                                                  uint32_t seed, const std::set<uint32_t>& forbidden) {
    struct Candidate { uint32_t tick; double score; };
    std::vector<Candidate> candidates;
    candidates.reserve(groups.size());
    for (const auto& group : groups) {
        if (forbidden.find(group.tick) != forbidden.end() || group.notes.empty()) continue;
        double bestSample = 0.0;
        bool hasLong = false;
        for (const auto& n : group.notes) {
            bestSample = std::max(bestSample, autoSampleScore(bms, n.event.wav, pedal));
            if (n.event.len > 6) hasLong = true;
        }
        double chord = group.notes.size() > 1 ? 0.9 : 0.0;
        double score = autoPhaseScore(phases, group.tick, pedal) + bestSample + chord;
        if (hasLong) score += pedal ? 0.6 : -0.15;
        score += static_cast<double>(autoHash(group.tick, seed, static_cast<uint32_t>(group.notes[0].event.wav)) & 0xffffu) / 65535.0;
        candidates.push_back({group.tick, score});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.tick < b.tick;
    });
    std::set<uint32_t> selected;
    for (const auto& c : candidates) {
        if (selected.size() >= desired) break;
        bool tooClose = false; // ponytail: 'near' is a reserved MinGW keyword (near/far ptr qualifier)
        auto it = selected.lower_bound(c.tick);
        if (it != selected.end() && *it - c.tick < minGap) tooClose = true;
        if (it != selected.begin()) { auto prev = it; --prev; if (c.tick - *prev < minGap) tooClose = true; }
        if (!tooClose) selected.insert(c.tick);
    }
    return selected;
}

// An 8th note at whatever grid we emit: hitting the same lane again inside that window
// is a jack, and a generated chart full of them is unplayable. "lastLane" only ever remembered ONE
// note back and its 48-point penalty was swamped by the unbounded laneUse term, so nothing actually
// stopped a lane repeating on consecutive ticks.
static const uint32_t kJackGapTicks = 24 * kModelTickScale;
// Minimum rest between the end of a long note and the start of the next long on the SAME lane.
static const uint32_t kLongChainGapTicks = 24 * kModelTickScale;

static int chooseAutoLane(const std::vector<int>& candidates, uint32_t tick, int sourceTrack,
                          const std::set<int>& usedAtTick, const std::array<uint32_t,64>& blockedUntil,
                          const std::array<uint64_t,64>& laneUse,
                          const std::array<uint32_t,64>& laneLastTick,
                          const std::array<bool,64>& laneEver,
                          const std::array<bool,64>& laneLastWasLong,
                          int lastLane, uint32_t seed, bool noteIsLong, int strictness) {
    int best = -1;
    uint64_t bestCost = ~uint64_t(0);
    for (size_t i = 0; i < candidates.size(); ++i) {
        int lane = candidates[i];
        if (lane < 0 || lane >= 64 || usedAtTick.find(lane) != usedAtTick.end()) continue;
        if (blockedUntil[lane] > tick) continue;
        uint32_t since = (laneEver[lane] && tick >= laneLastTick[lane]) ? tick - laneLastTick[lane] : 0xffffffffu;
        // Two long notes back to back on one lane are not playable in this engine: there is no room
        // to release and press again, the second cannot be caught and the combo drops. Measured from
        // the END of the previous long (blockedUntil holds it), not from its start -- keying this off
        // "the last note here was long" alone would poison a lane permanently and a chart made mostly
        // of long notes would exhaust every lane. Outranks the jack gap: a jack is merely hard, this
        // one the engine gets wrong, so it survives one more relaxation step.
        if (strictness >= 1 && noteIsLong && laneLastWasLong[lane] &&
            tick >= blockedUntil[lane] && tick - blockedUntil[lane] < kLongChainGapTicks) continue;
        // Hard gap, relaxed first. The caller steps strictness down rather than let a note fall to
        // background audio merely because the chart is dense.
        if (strictness >= 2 && laneEver[lane] && since < kJackGapTicks) continue;
        uint64_t cost = laneUse[lane] * 32u;
        if (lane == lastLane) cost += 48u;
        // Graded recency on top of the hard gap: among legal lanes, prefer the one resting longest.
        if (laneEver[lane] && since < kJackGapTicks * 4u) cost += (kJackGapTicks * 4u - since);
        int sourcePos = sourceTrack >= 3 && sourceTrack <= 9 ? sourceTrack - 3 : 0;
        int targetPos = static_cast<int>(i);
        cost += static_cast<uint64_t>(std::abs(sourcePos - targetPos) * 3);
        cost += autoHash(tick, static_cast<uint32_t>(lane), seed) & 7u;
        if (cost < bestCost) { bestCost = cost; best = lane; }
    }
    return best;
}

static void ApplyModeAutoGeneration(std::vector<EzRawEventOut> (&byTrack)[64],
                                    const ParsedBmsForEz& bms, const std::string& rawMode,
                                    const std::string& identity) {
    if (!Config().autoGenerateModes || (!g_curAutoGenerate && !g_curAutoEnrich)) return;
    std::string mode = NormalizeModeName(rawMode);
    if (mode.empty() || mode == "5keyOnly") return;
    AutoModeProfile profile = profileForMode(mode);

    std::map<uint32_t, std::vector<AutoNotePack>> grouped;
    size_t playableCount = 0;
    for (int track = 3; track <= 21; ++track) {
        std::vector<EzRawEventOut> keep;
        keep.reserve(byTrack[track].size());
        for (const auto& event : byTrack[track]) {
            if (event.type == 1) {
                grouped[event.tick].push_back({event, track});
                ++playableCount;
            } else keep.push_back(event);
        }
        byTrack[track].swap(keep);
    }
    if (!playableCount) return;

    std::vector<AutoTickGroup> groups;
    groups.reserve(grouped.size());
    for (auto& kv : grouped) groups.push_back({kv.first, std::move(kv.second)});

    const NativeModeModel* model = nullptr;
    auto mit = g_nativeModeModels.find(mode);
    if (mit != g_nativeModeModels.end() && mit->second.charts >= 4 && mit->second.notes) model = &mit->second;
    if (model) {
        // Native track names/usage are authoritative. Keep profile fallbacks only when the model
        // did not observe the corresponding controller in enough source charts.
        // Same class of bug as the lane set below: the hardcoded profile decided WHETHER a mode
        // has scratch/pedal at all and the model could only correct WHICH track. rubyMix shipped
        // with neither set, while its native charts put 7.2% of notes on scratch (track 10) and
        // 5.2% on pedal (track 11) -- so generated ruby charts came out as a bare copy of the
        // 5-key source. Let the model turn a control ON when the native charts clearly use it.
        if (profile.scratchTrack < 0 && model->notes && model->scratchNotes * 100u >= model->notes) {
            int learned = modelDominantTrack(model->scratchTrackNotes, -1);
            if (learned >= 0) {
                profile.scratchTrack = learned;
                LogLine("[autochart-lanes] mode='%s' scratch enabled from model: track=%d (%.1f%% of native notes)",
                        mode.c_str(), learned,
                        100.0 * (double)model->scratchNotes / (double)model->notes);
            }
        }
        if (profile.pedalTrack1 < 0 && model->notes && model->pedalNotes * 100u >= model->notes) {
            int learned = modelDominantTrack(model->pedalTrackNotes, -1);
            if (learned >= 0) {
                profile.pedalTrack1 = learned;
                LogLine("[autochart-lanes] mode='%s' pedal enabled from model: track=%d (%.1f%% of native notes)",
                        mode.c_str(), learned,
                        100.0 * (double)model->pedalNotes / (double)model->notes);
            }
        }
        // Symmetric to the enable above, and the half that was missing: a control the hardcoded
        // profile assumes has to go away when the mode's OWN native charts do not use it. This is
        // what keeps the keys-only 5-key mode free of the scratch/pedal its sibling mode has,
        // whichever of the two it turns out to be.
        if (profile.scratchTrack >= 0 && model->notes && model->scratchNotes * 200u < model->notes) {
            LogLine("[autochart-lanes] mode='%s' scratch DISABLED from model: %llu of %llu native notes (<0.5%%)",
                    mode.c_str(), (unsigned long long)model->scratchNotes, (unsigned long long)model->notes);
            profile.scratchTrack = -1; profile.scratchTrack2 = -1; profile.scratchShare = 0.0;
        }
        if (profile.pedalTrack1 >= 0 && model->notes && model->pedalNotes * 200u < model->notes) {
            LogLine("[autochart-lanes] mode='%s' pedal DISABLED from model: %llu of %llu native notes (<0.5%%)",
                    mode.c_str(), (unsigned long long)model->pedalNotes, (unsigned long long)model->notes);
            profile.pedalTrack1 = -1; profile.pedalTrack2 = -1; profile.pedalShare = 0.0;
        }
        if (profile.scratchTrack >= 0) {
            profile.scratchTrack = modelDominantTrack(model->scratchTrackNotes, profile.scratchTrack);
            if (profile.scratchTrack2 >= 0)
                profile.scratchTrack2 = modelDominantTrack(model->scratchTrackNotes, profile.scratchTrack2, profile.scratchTrack);
        }
        if (profile.pedalTrack1 >= 0) {
            profile.pedalTrack1 = modelDominantTrack(model->pedalTrackNotes, profile.pedalTrack1);
            if (profile.pedalTrack2 >= 0)
                profile.pedalTrack2 = modelDominantTrack(model->pedalTrackNotes, profile.pedalTrack2, profile.pedalTrack1);
        }
        // The lane SET comes from the model, not just the lane numbers. The old code asked for
        // exactly profile.primaryKeys.size() tracks, so the model could fix WHICH tracks a mode uses
        // but never HOW MANY — the hardcoded count stayed authoritative and wrong: rubyMix played on
        // 4 lanes where the native charts use 5, and ez2Catch/cv2Mix never got a second side at all,
        // so every note piled into half the playfield. clubMix/spaceMix only looked right because
        // their hardcoded profiles happened to match the native layout exactly.
        std::vector<int> learnedPrimary = modelTracksAboveShare(model->primaryTrackNotes, 0.02);
        std::vector<int> learnedSecond  = modelTracksAboveShare(model->secondTrackNotes, 0.02);
        if (!learnedPrimary.empty() && learnedPrimary.size() <= 16 && learnedPrimary != profile.primaryKeys) {
            LogLine("[autochart-lanes] mode='%s' primary hardcoded=[%s] -> learned=[%s]", mode.c_str(),
                    tracksToText(profile.primaryKeys).c_str(), tracksToText(learnedPrimary).c_str());
            profile.primaryKeys = learnedPrimary;
        }
        if (!learnedSecond.empty() && learnedSecond.size() <= 16 && learnedSecond != profile.secondaryKeys) {
            LogLine("[autochart-lanes] mode='%s' second  hardcoded=[%s] -> learned=[%s]", mode.c_str(),
                    tracksToText(profile.secondaryKeys).c_str(), tracksToText(learnedSecond).c_str());
            profile.secondaryKeys = learnedSecond;
        }
        // turnTable is hardcoded as a one-scratch-lane mode, but the learned model reports its native
        // charts using key lanes and essentially no scratch. Trust that only when it is unambiguous
        // (a real model, and under 1% scratch), and say so in the log — if inferNativeModeFromPath
        // mislabelled those charts this is the line that will show it.
        if (profile.turntableOnly && model->charts >= 32 && model->notes &&
            model->scratchNotes * 100u < model->notes && !profile.primaryKeys.empty()) {
            LogLine("[autochart-lanes] mode='%s' turntableOnly OFF: native model has %llu scratch of %llu notes on %lu key lanes",
                    mode.c_str(), static_cast<unsigned long long>(model->scratchNotes),
                    static_cast<unsigned long long>(model->notes),
                    static_cast<unsigned long>(profile.primaryKeys.size() + profile.secondaryKeys.size()));
            profile.turntableOnly = false;
        }
    }
    double intensity = static_cast<double>(Config().autoGenerateIntensity) / 100.0;
    double scratchShare = profile.scratchShare;
    double pedalShare = profile.pedalShare;
    if (model) {
        if (profile.scratchTrack >= 0 && !profile.turntableOnly)
            scratchShare = modelRatio(model->scratchNotes, model->notes, scratchShare, 0.010, 0.22);
        if (profile.pedalTrack1 >= 0)
            pedalShare = modelRatio(model->pedalNotes, model->notes, pedalShare, 0.005, 0.14);
    }
    scratchShare *= intensity;
    pedalShare *= intensity;
    if (profile.turntableOnly) scratchShare = 1.0;
    if (scratchShare > 0.35 && !profile.turntableOnly) scratchShare = 0.35;
    if (pedalShare > 0.20) pedalShare = 0.20;
    double secondSideShare = profile.secondaryKeys.empty() ? 0.0 : 0.50;
    if (model && !profile.secondaryKeys.empty()) {
        uint64_t ordinary = model->notes > model->scratchNotes + model->pedalNotes ?
            model->notes - model->scratchNotes - model->pedalNotes : model->notes;
        secondSideShare = modelRatio(model->secondSideNotes, ordinary, secondSideShare, 0.15, 0.80);
    }

    // These come out of the model in NATIVE ticks -- the shipped charts it was learned from --
    // so they are clamped in that unit and then scaled into the grid we actually emit. Leaving them
    // unscaled would halve every spacing rule in musical terms the moment the grid changed.
    uint32_t scratchGap = model && model->scratchGapCount ?
        static_cast<uint32_t>(model->scratchGapSum / model->scratchGapCount) : 24u;
    uint32_t pedalGap = model && model->pedalGapCount ?
        static_cast<uint32_t>(model->pedalGapSum / model->pedalGapCount) : 48u;
    if (scratchGap < 6) scratchGap = 6;
    if (scratchGap > kNativeTicksPerMeasure) scratchGap = kNativeTicksPerMeasure;
    if (pedalGap < 12) pedalGap = 12;
    if (pedalGap > kNativeTicksPerMeasure * 2) pedalGap = kNativeTicksPerMeasure * 2;
    scratchGap *= kModelTickScale;
    pedalGap   *= kModelTickScale;

    uint32_t seed = static_cast<uint32_t>(Config().autoGenerateSeed);
    for (char c : identity) seed = seed * 33u + static_cast<unsigned char>(c);
    size_t scratchDesired = profile.scratchTrack >= 0 ? static_cast<size_t>(playableCount * scratchShare + 0.5) : 0;
    size_t pedalDesired = profile.pedalTrack1 >= 0 ? static_cast<size_t>(playableCount * pedalShare + 0.5) : 0;
    if (profile.turntableOnly) scratchDesired = groups.size();
    const std::array<uint64_t,16>* scratchPhases = model ? &model->scratchPhase : nullptr;
    const std::array<uint64_t,16>* pedalPhases = model ? &model->pedalPhase : nullptr;
    std::set<uint32_t> none;
    std::set<uint32_t> scratchTicks = selectAutoSpecialTicks(groups, scratchDesired, scratchGap,
        scratchPhases, bms, false, seed ^ 0x5A17u, none);
    std::set<uint32_t> pedalTicks = selectAutoSpecialTicks(groups, pedalDesired, pedalGap,
        pedalPhases, bms, true, seed ^ 0xBEEFu, scratchTicks);

    std::array<uint32_t,64> blockedUntil{};
    std::array<uint64_t,64> laneUse{};
    std::array<uint32_t,64> laneLastTick{};
    std::array<bool,64> laneEver{};
    std::array<bool,64> laneLastWasLong{};
    int lastLane = -1;
    size_t scratchMade = 0, pedalMade = 0, p2Made = 0, overflowAudio = 0;
    for (auto& group : groups) {
        std::set<int> used;
        std::vector<AutoNotePack> remaining = group.notes;
        auto moveBestTo = [&](int target, bool pedal) -> bool {
            if (target < 0 || target >= 64 || blockedUntil[target] > group.tick || remaining.empty()) return false;
            // Pedal is placed before scratch and both resolve their track through the model, so they
            // can land on the SAME track. Without this the second call stacked another playable note
            // on the same lane at the same tick -- an overlap that can never be hit.
            if (used.find(target) != used.end()) return false;
            if (laneEver[target] && group.tick >= laneLastTick[target] &&
                group.tick - laneLastTick[target] < kJackGapTicks) return false;
            size_t best = 0;
            double bestScore = -1e9;
            for (size_t i = 0; i < remaining.size(); ++i) {
                double score = autoSampleScore(bms, remaining[i].event.wav, pedal);
                if (remaining[i].event.len > 6) score += pedal ? 0.8 : -0.1;
                score += static_cast<double>(autoHash(group.tick, remaining[i].event.wav, seed + static_cast<uint32_t>(i)) & 0xffu) / 1024.0;
                if (score > bestScore) { bestScore = score; best = i; }
            }
            EzRawEventOut event = remaining[best].event;
            // Same rule for the scratch/pedal lanes: refuse rather than chain two longs, and let the
            // normal lane assignment below place this note somewhere it can actually be played.
            if (event.len > 6 && laneLastWasLong[target]) return false;
            remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(best));
            event.track = target;
            byTrack[target].push_back(event);
            used.insert(target); ++laneUse[target];
            laneLastTick[target] = group.tick; laneEver[target] = true;
            laneLastWasLong[target] = event.len > 6;
            if (event.len > 6) blockedUntil[target] = std::max(blockedUntil[target], event.tick + static_cast<uint32_t>(event.len));
            lastLane = target;
            return true;
        };

        if (pedalTicks.find(group.tick) != pedalTicks.end()) {
            int pedalTarget = profile.pedalTrack1;
            if (profile.pedalTrack2 >= 0 && ((group.tick / 192u) & 1u)) pedalTarget = profile.pedalTrack2;
            if (moveBestTo(pedalTarget, true)) ++pedalMade;
        }
        if (scratchTicks.find(group.tick) != scratchTicks.end() && !remaining.empty()) {
            int scratchTarget = profile.scratchTrack;
            if (profile.scratchTrack2 >= 0 && ((group.tick / 192u) & 1u)) scratchTarget = profile.scratchTrack2;
            if (moveBestTo(scratchTarget, false)) ++scratchMade;
        }

        if (profile.turntableOnly) {
            // TurnTable has one gameplay lane. Simultaneous excess keysounds remain audible on BG lanes.
            if (!remaining.empty() && used.find(profile.scratchTrack) == used.end() &&
                blockedUntil[profile.scratchTrack] <= group.tick) {
                EzRawEventOut event = remaining.front().event;
                remaining.erase(remaining.begin());
                event.track = profile.scratchTrack;
                byTrack[profile.scratchTrack].push_back(event);
                ++laneUse[profile.scratchTrack]; ++scratchMade;
                if (event.len > 6) blockedUntil[profile.scratchTrack] = event.tick + static_cast<uint32_t>(event.len);
            }
            for (auto& n : remaining) {
                int bg = 22 + (n.event.wav % 10);
                n.event.track = bg; byTrack[bg].push_back(n.event); ++overflowAudio;
            }
            continue;
        }

        if (g_curAutoEnrich) {
            // The author's own chart. The scratch/pedal extraction above is the ONLY change; every
            // remaining note stays on the lane it was written for, so 5-key standard gains the
            // controls the mode really has without its authored pattern being reshuffled.
            for (auto& note : remaining) {
                int track = note.sourceTrack;
                if (track < 0 || track >= 64) continue;
                note.event.track = track;
                byTrack[track].push_back(note.event);
                ++laneUse[track];
            }
            continue;
        }

        bool preferSecond = false;
        if (!profile.secondaryKeys.empty()) {
            uint32_t roll = autoHash(group.tick / 24u, seed ^ 0x2F2Fu, static_cast<uint32_t>(group.notes.size())) & 0xffffu;
            preferSecond = static_cast<double>(roll) / 65535.0 < secondSideShare;
            // Phrase alternation remains a weak tie-breaker so both hands do not look like pure noise.
            if (((group.tick / 192u) & 1u) != 0u && secondSideShare >= 0.35) preferSecond = true;
        }
        std::vector<int> laneCandidates;
        if (preferSecond) {
            laneCandidates.insert(laneCandidates.end(), profile.secondaryKeys.begin(), profile.secondaryKeys.end());
            laneCandidates.insert(laneCandidates.end(), profile.primaryKeys.begin(), profile.primaryKeys.end());
        } else {
            laneCandidates.insert(laneCandidates.end(), profile.primaryKeys.begin(), profile.primaryKeys.end());
            laneCandidates.insert(laneCandidates.end(), profile.secondaryKeys.begin(), profile.secondaryKeys.end());
        }
        for (auto& note : remaining) {
            const bool noteIsLong = note.event.len > 6;
            // Strictness steps down instead of dropping the note: 2 = jack gap + no chained longs,
            // 1 = no chained longs, 0 = any legal lane. Overlap and active long notes are refused
            // at every level.
            int lane = -1;
            for (int strictness = 2; strictness >= 0 && lane < 0; --strictness)
                lane = chooseAutoLane(laneCandidates, group.tick, note.sourceTrack, used,
                                      blockedUntil, laneUse, laneLastTick, laneEver, laneLastWasLong,
                                      lastLane, seed, noteIsLong, strictness);
            if (lane < 0) {
                int bg = 22 + (note.event.wav % 10);
                note.event.track = bg; byTrack[bg].push_back(note.event); ++overflowAudio;
                continue;
            }
            note.event.track = lane;
            byTrack[lane].push_back(note.event);
            used.insert(lane); ++laneUse[lane];
            laneLastTick[lane] = group.tick; laneEver[lane] = true;
            laneLastWasLong[lane] = noteIsLong;
            if (lane >= 12 && lane <= 18) ++p2Made;
            if (note.event.len > 6)
                blockedUntil[lane] = std::max(blockedUntil[lane], note.event.tick + static_cast<uint32_t>(note.event.len));
            lastLane = lane;
        }
    }
    // Safety net and measurement for the two symptoms this pass targets. Two playable notes on one
    // lane at the same tick can never be hit, so the extra is moved to background audio (the
    // keysound still plays) rather than dropped. Remaining jacks are only counted, not altered --
    // they can be legitimate at low density, and the count is what tells us if the gap is working.
    size_t collisions = 0, jacks = 0;
    for (int track = 3; track <= 21; ++track) {
        std::stable_sort(byTrack[track].begin(), byTrack[track].end(), [](const EzRawEventOut& a, const EzRawEventOut& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.wav < b.wav;
        });
        std::vector<EzRawEventOut> keep;
        keep.reserve(byTrack[track].size());
        bool havePrev = false;
        uint32_t prevTick = 0;
        for (const auto& event : byTrack[track]) {
            if (event.type != 1) { keep.push_back(event); continue; }
            if (havePrev && event.tick == prevTick) {
                EzRawEventOut moved = event;
                moved.track = 22 + (event.wav % 10);
                byTrack[moved.track].push_back(moved);
                ++collisions;
                continue;
            }
            if (havePrev && event.tick - prevTick < kJackGapTicks) ++jacks;
            keep.push_back(event);
            havePrev = true;
            prevTick = event.tick;
        }
        byTrack[track].swap(keep);
    }
    for (int track = 0; track < 64; ++track)
        std::stable_sort(byTrack[track].begin(), byTrack[track].end(), [](const EzRawEventOut& a, const EzRawEventOut& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.wav < b.wav;
        });
    // Per-lane histogram: "notes=240" alone cannot tell an even spread from everything piled onto
    // three lanes. This is the line that shows which lanes a generated chart actually reaches.
    std::string laneHist;
    {
        std::vector<int> shown = profile.primaryKeys;
        shown.insert(shown.end(), profile.secondaryKeys.begin(), profile.secondaryKeys.end());
        for (int extra : {profile.scratchTrack, profile.scratchTrack2, profile.pedalTrack1, profile.pedalTrack2})
            if (extra >= 0 && std::find(shown.begin(), shown.end(), extra) == shown.end()) shown.push_back(extra);
        for (size_t i = 0; i < shown.size(); ++i) {
            if (i) laneHist += ",";
            laneHist += std::to_string(shown[i]) + ":" +
                        std::to_string(static_cast<unsigned long long>(laneUse[shown[i]]));
        }
        if (laneHist.empty()) laneHist = "-";
    }
    LogLine("[autochart] id='%s' mode='%s' modelCharts=%lu notes=%lu scratch=%lu pedal=%lu p2=%lu overflowAudio=%lu collisions=%lu jacks=%lu shares=%.3f/%.3f/%.3f tracks=%d,%d,%d,%d lanes=[%s]%s",
        identity.c_str(), mode.c_str(), static_cast<unsigned long>(model ? model->charts : 0),
        static_cast<unsigned long>(playableCount), static_cast<unsigned long>(scratchMade),
        static_cast<unsigned long>(pedalMade), static_cast<unsigned long>(p2Made),
        static_cast<unsigned long>(overflowAudio),
        static_cast<unsigned long>(collisions), static_cast<unsigned long>(jacks),
        scratchShare, pedalShare, secondSideShare,
        profile.scratchTrack, profile.scratchTrack2, profile.pedalTrack1, profile.pedalTrack2,
        laneHist.c_str(),
        g_curAutoEnrich ? " enrichSourceOnly=1" : profile.catchApproximation ? " catchApproximation=1" : "");
}


static bool copyFileSimple(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out); return true;
}


static std::string stripExtensionLocal(std::string p) {
    size_t slash = p.find_last_of("\\/");
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return p.substr(0, dot);
    return p;
}

static std::string replaceExtensionLocal(const std::string& p, const char* ext) {
    return stripExtensionLocal(p) + ext;
}

static bool wavToNativeSsfLocal(const std::string& wavPath, const std::string& ezwDst) {
    FILE* wav = fopen(wavPath.c_str(), "rb");
    std::vector<unsigned char> bytes;
    if (wav) {
        fseek(wav, 0, SEEK_END);
        long size = ftell(wav);
        rewind(wav);
        if (size > 0) {
            bytes.resize((size_t)size);
            fread(bytes.data(), 1, bytes.size(), wav);
        }
        fclose(wav);
    }

    // Native FNEX SSF is not RIFF: 18-byte header followed by raw PCM.
    // [u16 channels][u32 rate][u32 bytes/sec][u16 align][u16 bits][u32 data size]
    uint16_t format = 0, channels = 0, align = 0, bits = 0;
    uint32_t rate = 0, avg = 0, dataSize = 0;
    size_t dataOffset = 0;
    if (bytes.size() >= 12 && !memcmp(bytes.data(), "RIFF", 4) && !memcmp(bytes.data() + 8, "WAVE", 4)) {
        for (size_t pos = 12; pos + 8 <= bytes.size();) {
            uint32_t chunkSize = 0;
            memcpy(&chunkSize, bytes.data() + pos + 4, 4);
            size_t payload = pos + 8;
            if (payload + chunkSize > bytes.size()) break;
            if (!memcmp(bytes.data() + pos, "fmt ", 4) && chunkSize >= 16) {
                memcpy(&format, bytes.data() + payload + 0, 2);
                memcpy(&channels, bytes.data() + payload + 2, 2);
                memcpy(&rate, bytes.data() + payload + 4, 4);
                memcpy(&avg, bytes.data() + payload + 8, 4);
                memcpy(&align, bytes.data() + payload + 12, 2);
                memcpy(&bits, bytes.data() + payload + 14, 2);
                if (format == 0xfffe && chunkSize >= 40) {
                    uint32_t subFormat = 0;
                    memcpy(&subFormat, bytes.data() + payload + 24, 4);
                    if (subFormat == 1) format = 1;
                }
            } else if (!memcmp(bytes.data() + pos, "data", 4)) {
                dataOffset = payload;
                dataSize = chunkSize;
            }
            pos = payload + chunkSize + (chunkSize & 1u);
        }
    }
    if (format != 1 || channels == 0 || dataOffset == 0 || dataSize == 0) {
        LogLine("[cache] wav->native ssf parse failed wav='%s'", wavPath.c_str());
        return false;
    }
    std::vector<unsigned char> ssf(18 + dataSize);
    memcpy(ssf.data() + 0, &channels, 2);
    memcpy(ssf.data() + 2, &rate, 4);
    memcpy(ssf.data() + 6, &avg, 4);
    memcpy(ssf.data() + 10, &align, 2);
    memcpy(ssf.data() + 12, &bits, 2);
    memcpy(ssf.data() + 14, &dataSize, 4);
    memcpy(ssf.data() + 18, bytes.data() + dataOffset, dataSize);
    bool ok2 = writeBinaryFile(ezwDst, ssf);
    if (!ok2 || !fileExistsLocal(ezwDst)) {
        LogLine("[cache] wav->native ssf write failed wav='%s' ezw='%s'", wavPath.c_str(), ezwDst.c_str());
        return false;
    }
    LogLine("[cache] wav->native ssf channels=%u rate=%lu bits=%u data=%lu file='%s'",
            (unsigned)channels, (unsigned long)rate, (unsigned)bits, (unsigned long)dataSize,
            fileNameOnlyLocal(ezwDst).c_str());
    return true;
}

// Media Foundation resolved at runtime: XP has no mfplat/mfreadwrite, and a static import would
// stop the whole DLL from loading there. Resolved once; absent → ffmpeg.exe fallback below.
typedef HRESULT (WINAPI* PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI* PFN_MFShutdown)(void);
typedef HRESULT (WINAPI* PFN_MFCreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI* PFN_MFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
static PFN_MFStartup pMFStartup = nullptr;
static PFN_MFShutdown pMFShutdown = nullptr;
static PFN_MFCreateMediaType pMFCreateMediaType = nullptr;
static PFN_MFCreateSourceReaderFromURL pMFCreateSourceReaderFromURL = nullptr;
static bool MediaFoundationAvailable() {
    static int s_state = -1;
    if (s_state >= 0) return s_state == 1;
    HMODULE plat = LoadLibraryA("mfplat.dll");
    HMODULE rw = LoadLibraryA("mfreadwrite.dll");
    if (plat && rw) {
        pMFStartup = (PFN_MFStartup)GetProcAddress(plat, "MFStartup");
        pMFShutdown = (PFN_MFShutdown)GetProcAddress(plat, "MFShutdown");
        pMFCreateMediaType = (PFN_MFCreateMediaType)GetProcAddress(plat, "MFCreateMediaType");
        pMFCreateSourceReaderFromURL = (PFN_MFCreateSourceReaderFromURL)GetProcAddress(rw, "MFCreateSourceReaderFromURL");
    }
    s_state = (pMFStartup && pMFShutdown && pMFCreateMediaType && pMFCreateSourceReaderFromURL) ? 1 : 0;
    LogLine("[cache] media foundation %s", s_state ? "available" : "ABSENT (XP?) - using ffmpeg fallback for audio");
    return s_state == 1;
}

// XP fallback: decode any audio via ffmpeg.exe (already shipped for the video pipeline) to raw
// mono 22.05kHz s16le, then wrap in the SSF header. Slower (one process per keysound) but the
// result is cached, so only the first play of a song pays it.
// ponytail: needs an XP-capable ffmpeg build (<=3.x) in the game dir on XP; modern builds are Vista+.
static std::string FfmpegExePath(bool& modernOut); // fwd decl (defined near ConvertVideoToSpv)

// inputArgs land before -i. That matters for -ss: after the input it is an output seek, which
// decodes and discards everything up to the mark -- on a 70MB BGA that is seconds of work for a
// six-second preview.
static bool startFfmpegPipe(const std::string& src, const std::string& outputArgs,
                            PROCESS_INFORMATION& process, HANDLE& readPipe, bool realtime = false,
                            const std::string& inputArgs = std::string()) {
    bool modern = false;
    std::string ff = FfmpegExePath(modern);
    if (ff.empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0) ||
        !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        readPipe = nullptr;
        return false;
    }
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdError = nul;
    startup.hStdOutput = writePipe;
    std::string command = "\"" + ff + "\" -y -v 0" + (realtime ? " -re" : "") +
                          (inputArgs.empty() ? std::string() : " " + inputArgs) +
                          " -i \"" + src + "\" " + outputArgs;
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    std::string cwd = GetExeDirectory();
    BOOL ok = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, cwd.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(readPipe); readPipe = nullptr; }
    return ok != FALSE;
}

static bool ffmpegDecodePcmLocal(const std::string& src, uint32_t maxSeconds,
                                 std::vector<unsigned char>& pcm,
                                 uint32_t& channels, uint32_t& rate, uint32_t& bits) {
    char limit[32] = {};
    if (maxSeconds) snprintf(limit, sizeof(limit), " -t %lu", (unsigned long)maxSeconds);
    PROCESS_INFORMATION process{};
    HANDLE pipe = nullptr;
    if (!startFfmpegPipe(src, std::string("-vn -ac 2 -ar 44100") + limit + " -f s16le -", process, pipe))
        return false;
    CloseHandle(process.hThread);
    pcm.clear();
    unsigned char chunk[64 * 1024];
    DWORD got = 0;
    bool tooLarge = false;
    while (ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) && got) {
        if (pcm.size() + got > 512u * 1024u * 1024u) { tooLarge = true; break; }
        pcm.insert(pcm.end(), chunk, chunk + got);
    }
    CloseHandle(pipe);
    if (tooLarge) TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    bool ok = !tooLarge && code == 0 && !pcm.empty() && (pcm.size() % 4) == 0;
    channels = ok ? 2 : 0; rate = ok ? 44100 : 0; bits = ok ? 16 : 0;
    LogLine("[audio] ffmpeg PCM src='%s' bytes=%lu rc=%lu ok=%d",
            src.c_str(), (unsigned long)pcm.size(), (unsigned long)code, ok ? 1 : 0);
    return ok;
}



// The disc is a circle and a frame is a square, so the corners have to go. Native disc art is a
// circle of radius 125 on a 256x256 canvas with everything outside it black -- measured, and
// identical across every shipped disc.
//
// Applied once per decode rather than per blit: once a song instead of ten times a second.
static void MaskPreviewFramesToCircle(std::vector<unsigned char>& frames, uint32_t frameCount,
                                      int size) {
    if (!frameCount || size <= 0) return;
    const size_t frameBytes = (size_t)size * size * 3u;
    const int r = (size * 125) / 256;
    const int cx = size / 2, cy = size / 2, r2 = r * r;
    for (uint32_t f = 0; f < frameCount; ++f) {
        unsigned char* px = frames.data() + (size_t)f * frameBytes;
        for (int y = 0; y < size; ++y) {
            const int dy = y - cy, dy2 = dy * dy;
            unsigned char* row = px + (size_t)y * size * 3;
            for (int x = 0; x < size; ++x) {
                const int dx = x - cx;
                if (dx * dx + dy2 > r2) { row[x*3] = row[x*3+1] = row[x*3+2] = 0; }
            }
        }
    }
}

// Decode a short chunk of a BGA video into raw BGR24 frames, all of them, into one buffer.
//
// The song-select preview loops six seconds of video inside a 256x256 disc. Decoding that once into
// RAM and looping the buffer is what keeps it affordable: the alternative is holding an ffmpeg
// process and a decoder open for as long as the cursor sits on the song, and BGA sources here have
// a median size of 73MB. 60 frames of 256x256x3 is 11.8MB, allocated once and reused for every
// song, so the cost does not grow with the library.
//
// Seeks past the intro by default -- BGA videos usually open on a title card, which makes a poor
// preview -- and falls back to the start when the file turns out to be shorter than the seek.
bool DecodeBgaPreview(const std::string& videoPath, int size, int fps, int seconds,
                      std::vector<unsigned char>& frames, uint32_t& frameCount) {
    frames.clear();
    frameCount = 0;
    if (videoPath.empty() || size <= 0 || fps <= 0 || seconds <= 0) return false;

    const size_t frameBytes = (size_t)size * (size_t)size * 3u;
    const size_t wanted = frameBytes * (size_t)(fps * seconds);

    char out[256];
    snprintf(out, sizeof(out),
             // -s, not -vf: the XP-compatible ffmpeg we ship is SVN-r11870 from 2008 and predates
             // libavfilter, so it rejects -vf outright and produces nothing. -s works in both eras.
             "-t %d -s %dx%d -r %d -an -f rawvideo -pix_fmt bgr24 -",
             seconds, size, size, fps);

    for (int attempt = 0; attempt < 2; ++attempt) {
        // First pass skips the intro; second starts at zero for anything too short for that.
        const std::string inputArgs = attempt == 0 ? "-ss 10" : "";
        PROCESS_INFORMATION process{};
        HANDLE pipe = nullptr;
        if (!startFfmpegPipe(videoPath, out, process, pipe, false, inputArgs)) return false;
        CloseHandle(process.hThread);

        frames.clear();
        frames.reserve(wanted);
        unsigned char chunk[64 * 1024];
        DWORD got = 0;
        while (frames.size() < wanted && ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) && got)
            frames.insert(frames.end(), chunk, chunk + got);
        CloseHandle(pipe);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hProcess);

        frames.resize((frames.size() / frameBytes) * frameBytes);   // drop a torn tail
        frameCount = (uint32_t)(frames.size() / frameBytes);

        MaskPreviewFramesToCircle(frames, frameCount, size);
        if (frameCount >= 2) {
            LogLine("[preview] decoded '%s' %ux%d frames=%lu (%.1f MB)%s",
                    videoPath.c_str(), (unsigned)size, size,
                    (unsigned long)frameCount, frames.size() / 1048576.0,
                    attempt ? " [from start]" : "");
            return true;
        }
    }
    frames.clear();
    frameCount = 0;
    LogLine("[preview] no frames from '%s'", fileNameOnlyLocal(videoPath).c_str());
    return false;
}


// ---- .scr background previews -------------------------------------------------------------------
//
// 319 native songs animate their background with .scr instead of a video, and .scr is not a video
// format at all: the Ez2Visual reference guide describes a keyframed layered scene with path
// interpolation, deformation and per-layer blend functions (R:G:B-A:SrcAlphaFunc:DestAlphaFunc).
// Reproducing that means reimplementing an animation runtime.
//
// None of which is needed, because the frames it composes are already individual images on disk:
// bg\<id>\ holds them as .abm, and 179 of the 281 folders that have any name them as a numbered
// run (name0001.abm, name0002.abm, ...). Reading a run off disk gives a real animated preview and
// skips the .scr and .str formats entirely.
//
// The frames go into the same buffer the video path fills, so the surface, the circular mask, the
// loop and the frame clock are all shared: this adds a source, not a second player.

// Undo the .abm transform in place. Forward (AbmFromBmpBytes) swaps the magic BM -> AW, restates
// bpp/width/height at 2/6/8 and XORs four header dwords; the XOR is its own inverse, and the
// restated fields are ignored because the real ones come back with it.
static bool AbmDecodeHeader(std::vector<unsigned char>& b, int& w, int& h, int& bpp,
                            size_t& dataOff, bool& bottomUp) {
    if (b.size() < 54 || b[0] != 'A' || b[1] != 'W') return false;
    static const uint32_t xors[] = {0x109a, 0xcfa1, 0x51ae, 0xb18f};
    static const size_t offsets[] = {10, 18, 22, 28};
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        memcpy(&v, b.data() + offsets[i], 4);
        v ^= xors[i];
        memcpy(b.data() + offsets[i], &v, 4);
    }
    uint32_t off = 0; int32_t bw = 0, bh = 0; uint16_t bb = 0;
    memcpy(&off, b.data() + 10, 4);
    memcpy(&bw,  b.data() + 18, 4);
    memcpy(&bh,  b.data() + 22, 4);
    memcpy(&bb,  b.data() + 28, 2);
    if (bw <= 0 || bh == 0 || (bb != 16 && bb != 24 && bb != 32)) return false;
    if (off >= b.size()) return false;
    w = bw;
    h = bh < 0 ? -bh : bh;
    bpp = bb;
    dataOff = off;
    bottomUp = bh > 0;                       // the usual BMP orientation
    return true;
}

// One .abm scaled into size*size BGR24, nearest neighbour -- bilinear costs more than it buys at
// this size, on art that is already being reduced far past the point where it shows.
static bool AbmFrameToBgr(const std::string& path, int size, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> b;
    bool read = false;
    if (sz > 54) {
        b.resize((size_t)sz);
        read = fread(b.data(), 1, b.size(), f) == b.size();
    }
    fclose(f);
    if (!read) return false;

    int w = 0, h = 0, bpp = 0;
    size_t dataOff = 0;
    bool bottomUp = true;
    if (!AbmDecodeHeader(b, w, h, bpp, dataOff, bottomUp)) return false;

    const int bytesPP = bpp / 8;
    const size_t stride = (((size_t)w * bytesPP + 3) & ~(size_t)3);
    if (dataOff + stride * (size_t)h > b.size()) return false;

    out.assign((size_t)size * size * 3, 0);
    for (int y = 0; y < size; ++y) {
        const int sy = (int)((int64_t)y * h / size);
        const unsigned char* row =
            b.data() + dataOff + (size_t)(bottomUp ? (h - 1 - sy) : sy) * stride;
        unsigned char* dst = out.data() + (size_t)y * size * 3;
        for (int x = 0; x < size; ++x) {
            const int sx = (int)((int64_t)x * w / size);
            const unsigned char* p = row + (size_t)sx * bytesPP;
            if (bpp == 16) {
                // 1:5:5:5, not 5:6:5. Read as 5:6:5 the green channel steals a bit from red and
                // everything shifts: the frames came out as green noise. Verified by rendering the
                // same frame both ways -- 1555 gives a coherent image, 565 gives static.
                const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                dst[x*3+0] = (unsigned char)(( v        & 0x1f) << 3);
                dst[x*3+1] = (unsigned char)(((v >> 5)  & 0x1f) << 3);
                dst[x*3+2] = (unsigned char)(((v >> 10) & 0x1f) << 3);
            } else {
                dst[x*3+0] = p[0];
                dst[x*3+1] = p[1];
                dst[x*3+2] = p[2];
            }
        }
    }
    return true;
}

// The best numbered run of frames in bg\<id>\, masks excluded.
//
// A real .scr composites several layers, each placed and scaled by keyframes -- and those transforms
// live in the .scr and .str, which nobody has documented past their headers (freem's formats.md
// marks the layer and keyframe structures "todo", and says outright that how a .scr times its .str
// playback is unknown). Stacking the runs without them would produce a collage, not the animation:
// a 128x128 sprite meant to sit in one corner would be blown up over the whole disc.
//
// So one run is chosen, and the choice is by FRAME AREA rather than by length. Picking the longest
// run looked reasonable and was wrong in the case that matters: bg\5shottime's longest is
// 7th_title_move at 100 frames of 160x120 -- a title card -- while its actual scene is ryu_an at
// 256x256. Area picks the layer that fills the frame, which is the one worth previewing.
//
// Area alone overshoots, though: bg\5shottime has a 512x256 run of ten frames, which wins on size
// and barely animates. So the rule is area among the runs that are long enough to be an animation
// -- a quarter of the longest run in the folder -- which lands on ryu_an there, 41 frames at
// 256x256, the actual scene.
//
// Ties go to the longer run, which settles folders like bg\Commonbg9 where the groups (3s_v1_,
// 3s_v2_, 3s_v3_, 3s_v4_) are four VARIANTS of one background at identical size, not layers.
// Width*height of an .abm without decoding its pixels.
static uint64_t AbmFrameArea(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::vector<unsigned char> head(64, 0);
    const size_t got = fread(head.data(), 1, head.size(), f);
    fclose(f);
    if (got < 32) return 0;
    int w = 0, h = 0, bpp = 0;
    size_t off = 0;
    bool bottomUp = true;
    if (!AbmDecodeHeader(head, w, h, bpp, off, bottomUp)) return 0;
    return (uint64_t)w * (uint64_t)h;
}

static std::vector<std::string> FindScrFrameRun(const std::string& dir) {
    std::vector<std::string> best;
    uint64_t bestArea = 0;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(JoinPath(dir, "*.abm").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    std::map<std::string, std::vector<std::string> > runs;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        if (lower.find("mask") != std::string::npos) continue;   // alpha companion, not a frame
        size_t dot = name.find_last_of('.');
        std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        size_t end = stem.size();
        while (end > 0 && isdigit((unsigned char)stem[end - 1])) --end;
        if (end == stem.size()) continue;                        // unnumbered: a one-off
        runs[stem.substr(0, end)].push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    size_t longest = 0;
    for (std::map<std::string, std::vector<std::string> >::iterator it = runs.begin();
         it != runs.end(); ++it)
        if (it->second.size() > longest) longest = it->second.size();
    const size_t minRun = longest / 4 < 2 ? 2 : longest / 4;

    for (std::map<std::string, std::vector<std::string> >::iterator it = runs.begin();
         it != runs.end(); ++it) {
        if (it->second.size() < minRun) continue;            // too short to read as an animation
        std::sort(it->second.begin(), it->second.end());
        const uint64_t area = AbmFrameArea(JoinPath(dir, it->second.front()));
        if (area > bestArea || (area == bestArea && it->second.size() > best.size())) {
            bestArea = area;
            best = it->second;
        }
    }
    std::sort(best.begin(), best.end());
    for (size_t i = 0; i < best.size(); ++i) best[i] = JoinPath(dir, best[i]);
    return best;
}

// ---- .scr / .str scene compositing --------------------------------------------------------------
//
// A .scr is a timeline and a .str is a scene, and reversing both is what makes a real preview
// possible. Neither is documented past its header -- freem's formats.md marks the .str layer and
// keyframe structures "todo" and says outright that how a .scr times its .str playback is unknown --
// so the layouts below were derived from the shipped files and checked against all of them.
//
//   .scr  "SCR0", then a table of 136-byte entries: start time in MILLISECONDS (u32), a plane
//         index (u32), and a 128-byte .str filename. The table does not always start at the same
//         offset (0x14, 0x1C and 0x24 all occur), so it is located by trying each and keeping the
//         one whose names resolve to .str files that exist -- 297 of 305 files parse that way, and
//         3611 of 3613 references resolve.
//
//         The plane is what makes this worth doing: entries on different planes play AT THE SAME
//         TIME, each plane advancing through its own scenes. Sorting by time proves it -- 297 of
//         297 files are ordered within every plane, against 152 ordered globally.
//
//   .str  "STRM", fps at 8, layer count at 0x10, 16 reserved bytes, then per layer: a texture count,
//         that many 128-byte names, a keyframe count, and that many 124-byte keyframes. Walking it
//         lands exactly on the last byte of 10469 of 10543 files, which is what says 124 is right.
//
//         A keyframe holds frame number, type, position, the four quad corners, an animation-frame
//         index, an angle, RGBA and a source/destination blend pair. Type 0 keyframes are the real
//         keys and are interpolated between; type 1 entries are all-zero placeholders (their count
//         matches the all-zero blend pair exactly, across 231764 of them).
//
// Of 895214 keyframes, 98.2% have axis-aligned quads, so corner deformation is dropped and the quad
// is taken as its bounding rectangle; rotation is kept because a quarter of the real keys use it.

// A .scr entry starts a scene on a plane. Its time field is in SCENE FRAMES at 60 Hz, the same
// unit the .str keyframes use -- not milliseconds. Two things say so. A .str header carries both an
// fps (always 60) and a maxkey, and bg\10prm's overlay declares fps 60 with maxkey 30 and keys at
// 0 and 30: half a second for a cymbal flash, where milliseconds would make it 30ms and invisible.
// And across 3251 consecutive entry pairs, the gap a .scr leaves for a scene divided by that
// scene's own last keyframe has a median of exactly 1.00 -- each scene is scheduled for precisely
// as long as it runs, which only holds if both fields share a unit.
//
// Reading the .scr as milliseconds while dividing keyframes by 60 was the original mistake, and it
// advanced scene-local time 16.7x too slowly: every scene showed only its first six percent before
// the timeline cut to the next, which is what made previews look like they were racing.
static const int kSceneHz = 60;

struct ScrEntry { uint32_t frame; uint32_t plane; std::string str; };

struct StrKey {
    int32_t frame;                   // when this key applies, in frames from the scene's start
    float   x, y;                    // where the quad's centre sits, in the scene's 640x480 space
    float   qx0, qx1, qy0, qy1;      // quad bounds relative to that centre
    float   ani, angle;
    float   delay;                   // textures advanced per scene frame, when the layer has many
    float   col[4];                  // RGBA 0..255, alpha included
    int32_t sa, da;                  // D3D7 blend factors
};
struct StrLayer { std::vector<std::string> tex; std::vector<StrKey> keys; };
struct StrScene { int fps; std::vector<StrLayer> layers; };

static const int kSceneW = 640, kSceneH = 480;   // the coordinate space keyframes are written in

static bool ScrFileExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static size_t FieldLen(const char* p, size_t cap) {
    size_t n = 0;
    while (n < cap && p[n]) ++n;
    return n;
}

static bool ReadWholeFile(const std::string& path, size_t minSize, size_t maxSize,
                          std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (sz > (long)minSize && sz < (long)maxSize) {
        out.resize((size_t)sz);
        ok = fread(out.data(), 1, out.size(), f) == out.size();
    }
    fclose(f);
    return ok;
}

static bool ParseStrScene(const std::string& path, StrScene& out) {
    std::vector<unsigned char> d;
    if (!ReadWholeFile(path, 0x24, 64u * 1024 * 1024, d)) return false;
    if (memcmp(d.data(), "STRM", 4) != 0) return false;

    uint32_t fps = 0, layers = 0;
    memcpy(&fps, d.data() + 8, 4);
    memcpy(&layers, d.data() + 0x10, 4);
    if (layers > 256) return false;
    out.fps = fps ? (int)fps : 60;      // authoring rate only -- keyframes are timed in ms
    out.layers.clear();

    size_t off = 0x24;
    for (uint32_t L = 0; L < layers; ++L) {
        if (off + 4 > d.size()) return false;
        uint32_t tex = 0;
        memcpy(&tex, d.data() + off, 4);
        off += 4;
        if (tex > 256 || off + (size_t)tex * 128 + 4 > d.size()) return false;
        StrLayer layer;
        for (uint32_t t = 0; t < tex; ++t) {
            const char* p = (const char*)d.data() + off;
            layer.tex.push_back(std::string(p, FieldLen(p, 128)));
            off += 128;
        }
        uint32_t nk = 0;
        memcpy(&nk, d.data() + off, 4);
        off += 4;
        if (nk > 100000 || off + (size_t)nk * 124 > d.size()) return false;
        for (uint32_t k = 0; k < nk; ++k) {
            const unsigned char* b = d.data() + off;
            off += 124;
            int32_t type = 0;
            memcpy(&type, b + 4, 4);
            if (type != 0) continue;                 // placeholder, carries nothing
            StrKey key;
            memcpy(&key.frame, b + 0, 4);
            memcpy(&key.x, b + 8, 4);
            memcpy(&key.y, b + 12, 4);
            float xy[8];
            memcpy(xy, b + 48, 32);
            key.qx0 = key.qx1 = xy[0];
            key.qy0 = key.qy1 = xy[4];
            for (int i = 1; i < 4; ++i) {
                if (xy[i] < key.qx0) key.qx0 = xy[i];
                if (xy[i] > key.qx1) key.qx1 = xy[i];
                if (xy[4 + i] < key.qy0) key.qy0 = xy[4 + i];
                if (xy[4 + i] > key.qy1) key.qy1 = xy[4 + i];
            }
            memcpy(&key.ani, b + 80, 4);
            memcpy(&key.delay, b + 88, 4);
            memcpy(&key.angle, b + 92, 4);
            memcpy(key.col, b + 96, 16);
            memcpy(&key.sa, b + 112, 4);
            memcpy(&key.da, b + 116, 4);
            layer.keys.push_back(key);
        }
        out.layers.push_back(layer);
    }
    return !out.layers.empty();
}

// The entry table, found by trying each offset the shipped files use and keeping the one whose
// names actually resolve. Guessing the header layout was not worth it: three variants exist, and
// file existence is ground truth that settles all of them.
static bool ParseScrTimeline(const std::string& path, const std::string& dir,
                             std::vector<ScrEntry>& out) {
    std::vector<unsigned char> d;
    if (!ReadWholeFile(path, 0x14, 16u * 1024 * 1024, d)) return false;
    if (memcmp(d.data(), "SCR0", 4) != 0) return false;

    static const size_t kStarts[] = {0x14, 0x18, 0x1C, 0x20, 0x24};
    int bestHits = -1;
    for (int s = 0; s < 5; ++s) {
        std::vector<ScrEntry> got;
        int hits = 0;
        for (size_t off = kStarts[s]; off + 8 <= d.size(); off += 136) {
            ScrEntry e;
            memcpy(&e.frame, d.data() + off, 4);
            memcpy(&e.plane, d.data() + off + 4, 4);
            const char* p = (const char*)d.data() + off + 8;
            const size_t avail = d.size() - (off + 8) < 128 ? d.size() - (off + 8) : 128;
            e.str.assign(p, FieldLen(p, avail));
            bool clean = true;                       // the rest of the field must be padding
            for (size_t i = e.str.size(); i < avail; ++i)
                if (p[i]) { clean = false; break; }
            if (!clean) break;
            if (!e.str.empty()) {
                std::string low = e.str;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char)tolower(c); });
                if (low.size() < 5 || low.compare(low.size() - 4, 4, ".str") != 0) break;
                if (ScrFileExists(JoinPath(dir, e.str))) ++hits;
            }
            got.push_back(e);
        }
        if (!got.empty() && hits > bestHits) { bestHits = hits; out.swap(got); }
    }
    return bestHits > 0;
}

// ---- rasterising one layer ----------------------------------------------------------------------

struct ScrTex { int w, h; std::vector<unsigned char> bgra; };

// Decoded textures, kept for one song's decode. A scene reuses the same art across every frame it
// draws, so decoding once per name instead of once per frame is the difference between a preview
// that appears and one that keeps the cabinet waiting.
struct ScrTexCache {
    std::map<std::string, ScrTex> map;
    size_t bytes;
    size_t cap;
    ScrTexCache() : bytes(0), cap(24u * 1024 * 1024) {}
};

// Textures are stored no larger than the canvas they will be drawn into, and the cache as a whole
// gets a budget. Neither is optional on a 32-bit cabinet: the art is authored for a 640x480 screen,
// so bg\therhythm's scenes reach for 163 MB of it at full size, in a process that is already
// holding the song object. Capped at preview size the same set is a fifteenth of that, and the
// budget bounds the pathological case that is left.
static const ScrTex* ScrLoadTex(ScrTexCache& cache, const std::string& dir, const std::string& name,
                                int maxDim) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    std::map<std::string, ScrTex>::iterator it = cache.map.find(key);
    if (it != cache.map.end()) return it->second.w ? &it->second : NULL;
    if (cache.bytes >= cache.cap) return NULL;   // budget spent; the rest of this scene goes unpainted

    ScrTex tex;
    tex.w = tex.h = 0;
    // The scenes name their art .bmp; on disk it is the .abm the game actually ships.
    const size_t dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::vector<unsigned char> b;
    int w = 0, h = 0, bpp = 0;
    size_t dataOff = 0;
    bool bottomUp = true;
    if (ReadWholeFile(JoinPath(dir, stem + ".abm"), 54, 64u * 1024 * 1024, b) &&
        AbmDecodeHeader(b, w, h, bpp, dataOff, bottomUp)) {
        const int bytesPP = bpp / 8;
        const size_t stride = (((size_t)w * bytesPP + 3) & ~(size_t)3);
        if (dataOff + stride * (size_t)h <= b.size() && w > 0 && h > 0) {
            // Shrunk on the way in, keeping the aspect, so nothing bigger than the canvas is kept.
            int tw = w, th = h;
            if (maxDim > 0 && (w > maxDim || h > maxDim)) {
                if (w >= h) { tw = maxDim; th = (int)((int64_t)h * maxDim / w); }
                else        { th = maxDim; tw = (int)((int64_t)w * maxDim / h); }
                if (tw < 1) tw = 1;
                if (th < 1) th = 1;
            }
            tex.w = tw;
            tex.h = th;
            tex.bgra.assign((size_t)tw * th * 4, 0);
            for (int y = 0; y < th; ++y) {
                const int sy = (int)((int64_t)y * h / th);
                const unsigned char* row =
                    b.data() + dataOff + (size_t)(bottomUp ? (h - 1 - sy) : sy) * stride;
                unsigned char* dst = tex.bgra.data() + (size_t)y * tw * 4;
                for (int x = 0; x < tw; ++x) {
                    const unsigned char* p = row + (size_t)((int64_t)x * w / tw) * bytesPP;
                    if (bpp == 16) {
                        const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                        dst[x*4+0] = (unsigned char)(( v        & 0x1f) << 3);
                        dst[x*4+1] = (unsigned char)(((v >> 5)  & 0x1f) << 3);
                        dst[x*4+2] = (unsigned char)(((v >> 10) & 0x1f) << 3);
                        dst[x*4+3] = 255;
                    } else {
                        dst[x*4+0] = p[0];
                        dst[x*4+1] = p[1];
                        dst[x*4+2] = p[2];
                        dst[x*4+3] = bytesPP == 4 ? p[3] : 255;
                    }
                }
            }
        }
    }
    cache.bytes += tex.bgra.size();
    std::map<std::string, ScrTex>::iterator ins =
        cache.map.insert(std::make_pair(key, ScrTex())).first;
    ins->second.w = tex.w;
    ins->second.h = tex.h;
    ins->second.bgra.swap(tex.bgra);
    return ins->second.w ? &ins->second : NULL;
}

// D3D7 blend factor -> multiplier in 0..256 fixed point. src/dst are one channel, sa is source
// alpha. The four that matter cover better than 96% of the keyframes: ONE/ZERO (opaque), ONE/ONE
// (additive), SRCALPHA/INVSRCALPHA (the usual blend) and ZERO/SRCCOLOR (multiply).
static inline int ScrBlendFactor(int code, int src, int sa, int dst) {
    switch (code) {
        case 1:  return 0;                    // ZERO
        case 2:  return 256;                  // ONE
        case 3:  return src + 1;              // SRCCOLOR
        case 4:  return 256 - src;            // INVSRCCOLOR
        case 5:  return sa + 1;               // SRCALPHA
        case 6:  return 256 - sa;             // INVSRCALPHA
        case 9:  return dst + 1;              // DESTCOLOR
        case 10: return 256 - dst;            // INVDESTCOLOR
        default: return sa + 1;               // DESTALPHA variants: no destination alpha is kept
    }
}

// Interpolate a layer's state at `at`, in frames since the scene started.
static bool ScrEvalLayer(const StrLayer& layer, float at, StrKey& out) {
    if (layer.keys.empty()) return false;
    // A layer exists only between its first and last key. Holding the last key instead looks
    // harmless and is not: bgRED spells one animation out as eight layers with consecutive
    // three-frame windows, and holding them all left eight additive copies stacked on the disc,
    // which saturates to a flat white circle. A lone key is a static layer and does persist.
    if (layer.keys.size() > 1) {
        if (at < (float)layer.keys.front().frame - 0.5f) return false;
        if (at > (float)layer.keys.back().frame + 0.5f) return false;
    }
    size_t i = 0;
    while (i + 1 < layer.keys.size() && (float)layer.keys[i + 1].frame <= at) ++i;
    const StrKey& a = layer.keys[i];
    const StrKey& b = layer.keys[i + 1 < layer.keys.size() ? i + 1 : i];
    const float span = (float)(b.frame - a.frame);
    float t = 0.0f;
    if (span > 0.0f && at > (float)a.frame) {
        t = (at - (float)a.frame) / span;
        if (t > 1.0f) t = 1.0f;
    }
    out = a;
    out.x     += (b.x - a.x) * t;
    out.y     += (b.y - a.y) * t;
    out.qx0   += (b.qx0 - a.qx0) * t;
    out.qx1   += (b.qx1 - a.qx1) * t;
    out.qy0   += (b.qy0 - a.qy0) * t;
    out.qy1   += (b.qy1 - a.qy1) * t;
    out.ani   += (b.ani - a.ani) * t;
    out.angle += (b.angle - a.angle) * t;
    // How far into the layer we are, which is what drives its texture cycle.
    out.frame  = (int32_t)at;
    for (int c = 0; c < 4; ++c) out.col[c] += (b.col[c] - a.col[c]) * t;
    return true;
}

// Draw one layer into the preview-sized BGR canvas. Scene coordinates are 640x480 and the canvas is
// size*size, so everything is scaled on the way in -- rasterising at preview size rather than
// compositing at 640x480 and shrinking is six times less work for the same result at 256px.
static void ScrDrawLayer(unsigned char* canvas, int size, ScrTexCache& cache,
                         const std::string& dir, const StrLayer& layer, const StrKey& st) {
    if (layer.tex.empty() || st.col[3] < 0.5f) return;
    // Which of the layer's textures to draw.
    //
    // Not from the keyframe's animation index, which is the obvious reading and is wrong: layers
    // with fifty textures routinely hold that index at zero across every key. The cycle is driven
    // by `delay` instead -- textures advanced per scene frame -- and the two agree exactly where it
    // can be checked, bgloki carrying fifty textures at 0.5 over a hundred-frame layer. Reading
    // only the index left those layers frozen on their first frame.
    int idx = 0;
    if (layer.tex.size() > 1) {
        float pos = st.ani;
        if (st.delay > 0.0f && !layer.keys.empty())
            pos += ((float)st.frame - (float)layer.keys.front().frame) * st.delay;
        const int n = (int)layer.tex.size();
        idx = (int)pos % n;                              // every anitype seen here cycles
        if (idx < 0) idx += n;

        // Only every few textures of a long cycle are ever decoded. A preview samples ten times a
        // second while these cycles run at up to sixty textures a second, so most are skipped
        // anyway -- but landing on a different one each time still meant decoding the whole set,
        // and that pushed the slowest song from 0.8 s to 4.5 s. Snapping to a stride bounds the
        // work at kCycleTexCap files per layer.
        // ponytail: coarser animation on cycles longer than the cap; raise it if XP has the room.
        const int kCycleTexCap = 24;
        if (n > kCycleTexCap) {
            const int stride = (n + kCycleTexCap - 1) / kCycleTexCap;
            idx = (idx / stride) * stride;
        }
    }
    const ScrTex* tex = ScrLoadTex(cache, dir, layer.tex[idx], size);
    if (!tex) return;

    const float sx = (float)size / (float)kSceneW, sy = (float)size / (float)kSceneH;
    const float w = (st.qx1 - st.qx0) * sx, h = (st.qy1 - st.qy0) * sy;
    if (w < 0.5f || h < 0.5f) return;
    const float cx = (st.x + (st.qx0 + st.qx1) * 0.5f) * sx;
    const float cy = (st.y + (st.qy0 + st.qy1) * 0.5f) * sy;

    const float ang = st.angle * 3.14159265f / 180.0f;
    const float ca = cosf(ang), sa = sinf(ang);
    const float rad = sqrtf(w * w + h * h) * 0.5f + 2.0f;
    int px0 = (int)(cx - rad), px1 = (int)(cx + rad) + 1;
    int py0 = (int)(cy - rad), py1 = (int)(cy + rad) + 1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > size) px1 = size;
    if (py1 > size) py1 = size;
    if (px0 >= px1 || py0 >= py1) return;

    const int modB = (int)st.col[2], modG = (int)st.col[1], modR = (int)st.col[0];
    const int alpha = (int)st.col[3];
    const bool plain = modB >= 255 && modG >= 255 && modR >= 255 && alpha >= 255;
    const bool opaque = st.sa == 2 && st.da == 1;        // ONE, ZERO: a straight copy
    const float iw = 1.0f / w, ih = 1.0f / h;

    // Colour modulation and alpha through lookup tables rather than arithmetic.
    //
    // The obvious `sp[c] * mod / 255` is four integer divisions per pixel, and integer division is
    // tens of cycles: profiling a heavy scene put the inner loop at 85 ns a pixel and bgparadise
    // at 4.2 seconds for its 49 million, with texture loading only 32 ms of that. Tables make the
    // whole thing a load.
    unsigned char lutB[256], lutG[256], lutR[256], lutA[256];
    for (int v = 0; v < 256; ++v) {
        lutB[v] = (unsigned char)(v * modB / 255);
        lutG[v] = (unsigned char)(v * modG / 255);
        lutR[v] = (unsigned char)(v * modR / 255);
        lutA[v] = (unsigned char)(v * alpha / 255);
    }
    // The two blends that dominate the shipped scenes get their own branch-free inner loops:
    // ONE/ONE is additive and needs no factors at all, and SRCALPHA/INVSRCALPHA is the ordinary
    // one. Together with ONE/ZERO they cover better than four keyframes in five.
    const bool additive = st.sa == 2 && st.da == 2;
    const bool srcAlpha = st.sa == 5 && st.da == 6;

    // Black is the transparent colour on an opaque sprite.
    //
    // 11,626 of the 28,288 sprite-sized layers draw with ONE/ZERO, a straight copy -- and copying
    // is what put a black rectangle around every one of them, which is what the fish in that
    // preview looked like. The art has no alpha to lean on (16bpp 1:5:5:5 with the one bit unset,
    // 24bpp with none at all), so the era's usual trick is the only thing left: the key colour is
    // black, and the hardware simply did not write those pixels.
    //
    // Only for sprites. A full-screen opaque layer is allowed its black, because some of them
    // exist to black something out -- the letterbox bars in 12Basic are exactly that.
    // ponytail: exact black only; a sprite matted on near-black keeps a faint edge.
    const bool keyBlack = opaque && (st.qx1 - st.qx0) < (float)kSceneW * 0.9f
                                 && (st.qy1 - st.qy0) < (float)kSceneH * 0.9f;

    // Three quarters of the real keyframes have no rotation, and a full-screen quad at 256px is
    // 65k pixels a frame -- so the unrotated case gets its own path, with the horizontal texture
    // lookup hoisted into a row of precomputed columns instead of two multiplies and a divide per
    // pixel. Compositing a busy scene went from 2.2 s to well under a second on that alone.
    std::vector<int> cols;
    const bool axis = st.angle > -0.01f && st.angle < 0.01f;
    if (axis) {
        cols.resize((size_t)(px1 - px0), -1);
        for (int x = px0; x < px1; ++x) {
            const float u = ((float)x + 0.5f - cx) * iw + 0.5f;
            if (u < 0.0f || u >= 1.0f) continue;
            int tu = (int)(u * tex->w);
            if (tu < 0) tu = 0;
            if (tu >= tex->w) tu = tex->w - 1;
            cols[(size_t)(x - px0)] = tu;
        }
    }

    for (int y = py0; y < py1; ++y) {
        const float dy = (float)y + 0.5f - cy;
        unsigned char* drow = canvas + (size_t)y * size * 3;
        int rowStart = -1;
        if (axis) {
            const float v = dy * ih + 0.5f;
            if (v < 0.0f || v >= 1.0f) continue;
            int tv = (int)(v * tex->h);
            if (tv < 0) tv = 0;
            if (tv >= tex->h) tv = tex->h - 1;
            rowStart = tv * tex->w;
        }
        for (int x = px0; x < px1; ++x) {
            int tu, tv;
            const unsigned char* sp;
            if (axis) {
                tu = cols[(size_t)(x - px0)];
                if (tu < 0) continue;
                sp = tex->bgra.data() + ((size_t)rowStart + tu) * 4;
            } else {
                const float dx = (float)x + 0.5f - cx;
                const float lx =  dx * ca + dy * sa;     // into the quad's own frame
                const float ly = -dx * sa + dy * ca;
                const float u = lx * iw + 0.5f, v = ly * ih + 0.5f;
                if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
                tu = (int)(u * tex->w);
                tv = (int)(v * tex->h);
                if (tu < 0) tu = 0;
                if (tv < 0) tv = 0;
                if (tu >= tex->w) tu = tex->w - 1;
                if (tv >= tex->h) tv = tex->h - 1;
                sp = tex->bgra.data() + ((size_t)tv * tex->w + tu) * 4;
            }

            if (keyBlack && !sp[0] && !sp[1] && !sp[2]) continue;   // the key colour
            unsigned char* dp = drow + (size_t)x * 3;
            if (opaque && plain) {                       // the commonest case, and the cheapest
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                continue;
            }
            const int sB = lutB[sp[0]], sG = lutG[sp[1]], sR = lutR[sp[2]];
            if (opaque) {
                dp[0] = (unsigned char)sB;
                dp[1] = (unsigned char)sG;
                dp[2] = (unsigned char)sR;
                continue;
            }
            if (additive) {
                const int b = dp[0] + sB, g = dp[1] + sG, r = dp[2] + sR;
                dp[0] = (unsigned char)(b > 255 ? 255 : b);
                dp[1] = (unsigned char)(g > 255 ? 255 : g);
                dp[2] = (unsigned char)(r > 255 ? 255 : r);
                continue;
            }
            const int sA = lutA[sp[3]];
            if (srcAlpha) {
                const int a1 = sA + 1, a0 = 256 - sA;
                dp[0] = (unsigned char)((sB * a1 + dp[0] * a0) >> 8);
                dp[1] = (unsigned char)((sG * a1 + dp[1] * a0) >> 8);
                dp[2] = (unsigned char)((sR * a1 + dp[2] * a0) >> 8);
                continue;
            }
            const int src[3] = {sB, sG, sR};
            for (int c = 0; c < 3; ++c) {
                const int d = dp[c];
                const int o = (src[c] * ScrBlendFactor(st.sa, src[c], sA, d) +
                               d      * ScrBlendFactor(st.da, src[c], sA, d)) >> 8;
                dp[c] = (unsigned char)(o < 0 ? 0 : (o > 255 ? 255 : o));
            }
        }
    }
}

// The composited .scr preview: every plane's active scene, layered, sampled in real time.
//
// Returns false when the song has no usable .scr, so the caller can fall back to reading a raw
// frame run off disk -- which is still the right answer for scenes like bg\12Basic, whose whole
// .scr is a scanline overlay and two letterbox bars over content that lives elsewhere.
static bool DecodeScrComposited(const std::string& dir, const std::string& songId, int size,
                                int fps, int seconds, std::vector<unsigned char>& frames,
                                uint32_t& frameCount, double& coverage) {
    coverage = 0.0;
    std::string scrPath;
    WIN32_FIND_DATAA fd;
    memset(&fd, 0, sizeof(fd));
    HANDLE h = FindFirstFileA(JoinPath(dir, "*.scr").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                scrPath = JoinPath(dir, fd.cFileName);
                break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (scrPath.empty()) return false;

    std::vector<ScrEntry> timeline;
    if (!ParseScrTimeline(scrPath, dir, timeline) || timeline.empty()) return false;

    // Scenes are parsed once and reused across every frame that references them, as are textures.
    std::map<std::string, StrScene> scenes;
    ScrTexCache texCache;

    std::vector<uint32_t> planes;
    uint32_t startFrame = 0xFFFFFFFFu;
    for (size_t i = 0; i < timeline.size(); ++i) {
        if (std::find(planes.begin(), planes.end(), timeline[i].plane) == planes.end())
            planes.push_back(timeline[i].plane);
        if (!timeline[i].str.empty() && timeline[i].frame < startFrame)
            startFrame = timeline[i].frame;
    }
    if (startFrame == 0xFFFFFFFFu) return false;
    std::sort(planes.begin(), planes.end());

    // Load every scene the timeline names, and with them the timeline's real length.
    //
    // That length is when the LAST scene on each plane finishes -- not the furthest any scene
    // would have reached. Taking the maximum over all entries leaves a dead tail whenever the
    // timeline ends on a short scene: bg\8Nihilism closes on a four-frame noise flash but also
    // contains a four-second intro, and measuring to the intro's end left three silent seconds
    // that the loop then dutifully replayed as nothing.
    uint32_t endFrame = startFrame;
    for (size_t p = 0; p < planes.size(); ++p) {
        const ScrEntry* last = NULL;
        for (size_t i = 0; i < timeline.size(); ++i) {
            if (timeline[i].plane != planes[p]) continue;
            if (!last || timeline[i].frame >= last->frame) last = &timeline[i];
        }
        if (!last) continue;
        uint32_t done = last->frame;
        if (!last->str.empty()) {
            std::map<std::string, StrScene>::iterator it = scenes.find(last->str);
            if (it == scenes.end()) {
                StrScene sc;
                sc.fps = 60;
                ParseStrScene(JoinPath(dir, last->str), sc);
                it = scenes.insert(std::make_pair(last->str, sc)).first;
            }
            int32_t tail = 0;
            for (size_t L = 0; L < it->second.layers.size(); ++L) {
                const StrLayer& layer = it->second.layers[L];
                if (!layer.keys.empty() && layer.keys.back().frame > tail)
                    tail = layer.keys.back().frame;
            }
            done += (uint32_t)tail;
        }
        if (done > endFrame) endFrame = done;
    }

    // Most .scr timelines run the length of the song, but plenty are far shorter than the preview,
    // and those have to loop or the preview freezes on the last scene and then empties as its
    // layers pass their final keyframe.
    uint32_t spanFrames = endFrame > startFrame ? endFrame - startFrame : 0;
    if (spanFrames < kSceneHz / 4) spanFrames = kSceneHz / 4;

    const size_t frameBytes = (size_t)size * size * 3u;
    const uint32_t want = (uint32_t)(fps * seconds);
    frames.assign(frameBytes * want, 0);
    frameCount = 0;

    std::vector<double> cover;
    cover.reserve(want);
    for (uint32_t f = 0; f < want; ++f) {
        // Preview frames are wall-clock; the timeline is scene frames at 60 Hz.
        const uint32_t at =
            startFrame + ((f * (uint32_t)kSceneHz / (uint32_t)fps) % spanFrames);
        unsigned char* canvas = frames.data() + (size_t)f * frameBytes;
        for (size_t p = 0; p < planes.size(); ++p) {
            // The scene a plane is showing at `ms` is the last one it was told to start.
            const ScrEntry* active = NULL;
            for (size_t i = 0; i < timeline.size(); ++i) {
                if (timeline[i].plane != planes[p] || timeline[i].frame > at) continue;
                if (!active || timeline[i].frame >= active->frame) active = &timeline[i];
            }
            if (!active || active->str.empty()) continue;   // an empty name clears the plane

            std::map<std::string, StrScene>::iterator it = scenes.find(active->str);
            if (it == scenes.end()) {
                StrScene sc;
                sc.fps = 60;
                ParseStrScene(JoinPath(dir, active->str), sc);
                it = scenes.insert(std::make_pair(active->str, sc)).first;
            }
            const StrScene& scene = it->second;
            if (scene.layers.empty()) continue;

            // A scene repeats inside the slot the timeline gives it. Most slots are exactly one
            // playthrough -- that median ratio of 1.00 again -- but a fair number are far longer
            // than the scene: bg\7loki hands a 99-frame loop a 1200-frame slot, and holding its
            // final keyframe for the other 92% left the disc empty most of the time.
            int32_t tail = 0;
            for (size_t L = 0; L < scene.layers.size(); ++L)
                if (!scene.layers[L].keys.empty() && scene.layers[L].keys.back().frame > tail)
                    tail = scene.layers[L].keys.back().frame;
            float local = (float)(at - active->frame);
            if (tail > 0 && local > (float)tail) local = fmodf(local, (float)(tail + 1));

            for (size_t L = 0; L < scene.layers.size(); ++L) {
                StrKey st;
                if (ScrEvalLayer(scene.layers[L], local, st))
                    ScrDrawLayer(canvas, size, texCache, dir, scene.layers[L], st);
            }
        }
        size_t on = 0, seen = 0;
        for (size_t i = 0; i < frameBytes; i += 33, ++seen) if (canvas[i] > 16) ++on;
        cover.push_back(seen ? (double)on / (double)seen : 0.0);
        ++frameCount;
    }
    if (frameCount < 2) { frames.clear(); frameCount = 0; return false; }

    // How much of the disc this scene actually fills, typically. The median rather than the mean,
    // because the case worth catching is a scene that is empty most of the time with a few bright
    // frames in it -- bg\5blackbird's .scr is nothing but white_flash.str, an effects track whose
    // photographs live in .str files it never sequences, and its mean brightness looks healthy on
    // the strength of two flashes. The caller compares this against the raw frame run and keeps
    // whichever actually shows something, which needs no threshold to be tuned per song.
    std::sort(cover.begin(), cover.end());
    coverage = cover[cover.size() / 2];

    MaskPreviewFramesToCircle(frames, frameCount, size);
    LogLine("[preview] scr '%s' composited %lu frames, %lu scenes on %lu plane(s), %lu textures,"
            " coverage %.2f, %.1f MB of texture", songId.c_str(), (unsigned long)frameCount,
            (unsigned long)scenes.size(), (unsigned long)planes.size(),
            (unsigned long)texCache.map.size(), coverage, texCache.bytes / 1048576.0);
    return true;
}

// The raw numbered frame run, read straight off disk. Older and blunter than compositing the
// scene, and still the better answer where a .scr turns out to be an effects track rather than the
// background itself.
static bool DecodeScrFrameRun(const std::string& dir, const std::string& songId, int size, int fps,
                              int seconds, std::vector<unsigned char>& frames, uint32_t& frameCount,
                              double& coverage) {
    frames.clear();
    frameCount = 0;
    coverage = 0.0;
    const std::vector<std::string> run = FindScrFrameRun(dir);
    if (run.size() < 2) return false;

    const size_t frameBytes = (size_t)size * size * 3u;
    const uint32_t want = (uint32_t)(fps * seconds);
    // A run longer than the preview is sampled evenly rather than truncated, so a 400-frame
    // animation previews as the whole thing sped up instead of only its opening seconds.
    const size_t step = run.size() > (size_t)want ? run.size() / want : 1;

    std::vector<unsigned char> one;
    std::vector<double> cover;
    for (size_t i = 0; i < run.size() && frameCount < want; i += step) {
        if (!AbmFrameToBgr(run[i], size, one) || one.size() != frameBytes) continue;
        frames.insert(frames.end(), one.begin(), one.end());
        size_t on = 0, seen = 0;
        for (size_t p = 0; p < frameBytes; p += 33, ++seen) if (one[p] > 16) ++on;
        cover.push_back(seen ? (double)on / (double)seen : 0.0);
        ++frameCount;
    }
    if (frameCount < 2) { frames.clear(); frameCount = 0; return false; }

    // Measured before the mask, as the composited path's is, so the two are comparable.
    std::sort(cover.begin(), cover.end());
    coverage = cover[cover.size() / 2];
    MaskPreviewFramesToCircle(frames, frameCount, size);
    LogLine("[preview] scr '%s' frame run %lu of %lu on disk, coverage %.2f",
            songId.c_str(), (unsigned long)frameCount, (unsigned long)run.size(), coverage);
    return true;
}

// A song's .scr background, decoded into the same frame buffer the video preview uses.
//
// Both sources are tried and the one that fills more of the disc wins. Compositing the scene is
// right nearly every time -- it is the actual animation, layered, in time -- but a handful of .scr
// files sequence only an effects overlay, and for those the raw run holds the content. Comparing
// what each produced settles it per song without a threshold to guess at, and the loser costs only
// the decode.
bool DecodeScrPreview(const std::string& songId, int size, int fps, int seconds,
                      std::vector<unsigned char>& frames, uint32_t& frameCount) {
    frames.clear();
    frameCount = 0;
    if (songId.empty() || size <= 0 || fps <= 0 || seconds <= 0) return false;

    const std::string dir = JoinPath(JoinPath(GetExeDirectory(), "bg"), songId);
    double sceneCover = 0.0;
    uint32_t sceneCount = 0;
    std::vector<unsigned char> scene;
    const bool haveScene =
        DecodeScrComposited(dir, songId, size, fps, seconds, scene, sceneCount, sceneCover);

    // The composited scene wins whenever it has anything at all in it. Coverage is the test for
    // "anything at all", not a contest -- a static full-screen frame from the run scores a perfect
    // 1.00 and would otherwise beat bg\5shottime, whose real animated scene sits at 0.84.
    if (haveScene && sceneCover >= 0.05) {
        frames.swap(scene);
        frameCount = sceneCount;
        return true;
    }

    double runCover = 0.0;
    uint32_t runCount = 0;
    std::vector<unsigned char> run;
    const bool haveRun = DecodeScrFrameRun(dir, songId, size, fps, seconds, run, runCount, runCover);

    if (haveScene && (!haveRun || sceneCover >= runCover)) {
        frames.swap(scene);
        frameCount = sceneCount;
        return true;
    }
    if (haveRun) {
        LogLine("[preview] scr '%s' using frame run (%.2f) over composited scene (%.2f)",
                songId.c_str(), runCover, haveScene ? sceneCover : 0.0);
        frames.swap(run);
        frameCount = runCount;
        return true;
    }
    return false;
}

// The BGA video a song would play, or empty when it has none.
//
// Two places to look. Custom songs carry their own file in the song folder. Native ones use the
// game's layout, which the loader spells out:
//
//     sprintf(this + 184, "%s\\Video\\%s\\%s.spv", Buffer, String1, String1);
//
// and .spv is not a format of its own -- it is ASF/WMV under another extension (magic 30 26 B2 75),
// which is why the same decode path handles both. 153 native songs have one.
//
// A preview only ever shows a song's OWN video. Video\_spv holds 21 shared clips that the gameplay
// path hands to songs with no BGA of their own (selectRandomBgaPoolVideo), and those are wallpaper,
// not this song -- showing one as a preview tells the player something false about the song.
//
// Tracing the three routes here says a pooled clip cannot reach this function: videoFiles only ever
// collects files found inside the song's folder, and the .spv cache step is gated on that list being
// non-empty. It was reported happening anyway, so rather than trust the trace, the rule is enforced
// where it matters and the resolved path is logged -- if one still arrives, the log now names it.
static bool IsSharedBgaPoolClip(const std::string& path) {
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return low.find("\\_spv\\") != std::string::npos ||
           low.find("/_spv/")    != std::string::npos;
}

std::string BgaVideoPathForSong(const std::string& id) {
    for (const auto& s : g_songs) {
        if (_stricmp(s.id.c_str(), id.c_str()) != 0) continue;
        for (const auto& v : s.videoFiles) {
            std::string p = JoinPath(s.songDir, v);
            if (IsSharedBgaPoolClip(p)) {
                LogLine("[preview] id='%s' refusing shared pool clip '%s'", id.c_str(), p.c_str());
                continue;
            }
            return p;
        }
        return std::string();
    }
    // Not one of ours, so it is a native song: Video\<id>\<id>.spv off the game directory.
    std::string spv = JoinPath(JoinPath(JoinPath(GetExeDirectory(), "Video"), id), id + ".spv");
    if (IsSharedBgaPoolClip(spv)) return std::string();
    FILE* probe = fopen(spv.c_str(), "rb");
    if (!probe) return std::string();
    fclose(probe);
    return spv;
}

static HANDLE g_ffmpegVideoProcess = nullptr;
static HANDLE g_ffmpegVideoPipe = nullptr;
static HANDLE g_ffmpegVideoThread = nullptr;
static volatile LONG g_ffmpegVideoStop = 0;
static CRITICAL_SECTION g_ffmpegVideoLock;
static bool g_ffmpegVideoLockReady = false;
static unsigned char* g_ffmpegVideoLatest = nullptr;
static bool g_ffmpegVideoFrameReady = false;
static const uint32_t kFfmpegVideoFrameBytes = 640u * 480u * 3u;

static DWORD WINAPI ffmpegVideoReader(void*) {
    unsigned char* frame = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, kFfmpegVideoFrameBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!frame) return 1;
    while (!g_ffmpegVideoStop) {
        uint32_t total = 0;
        while (total < kFfmpegVideoFrameBytes && !g_ffmpegVideoStop) {
            DWORD got = 0;
            if (!ReadFile(g_ffmpegVideoPipe, frame + total, kFfmpegVideoFrameBytes - total, &got, nullptr) || !got)
                break;
            total += got;
        }
        if (total != kFfmpegVideoFrameBytes) break;
        EnterCriticalSection(&g_ffmpegVideoLock);
        memcpy(g_ffmpegVideoLatest, frame, kFfmpegVideoFrameBytes);
        g_ffmpegVideoFrameReady = true;
        LeaveCriticalSection(&g_ffmpegVideoLock);
    }
    VirtualFree(frame, 0, MEM_RELEASE);
    return 0;
}

void StopDirectBmsFfmpegVideo() {
    InterlockedExchange(&g_ffmpegVideoStop, 1);
    if (g_ffmpegVideoProcess) {
        if (WaitForSingleObject(g_ffmpegVideoProcess, 50) == WAIT_TIMEOUT) {
            TerminateProcess(g_ffmpegVideoProcess, 0);
            WaitForSingleObject(g_ffmpegVideoProcess, 1000);
        }
    }
    if (g_ffmpegVideoThread) {
        WaitForSingleObject(g_ffmpegVideoThread, 1000);
        CloseHandle(g_ffmpegVideoThread);
        g_ffmpegVideoThread = nullptr;
    }
    if (g_ffmpegVideoPipe) { CloseHandle(g_ffmpegVideoPipe); g_ffmpegVideoPipe = nullptr; }
    if (g_ffmpegVideoProcess) {
        CloseHandle(g_ffmpegVideoProcess);
        g_ffmpegVideoProcess = nullptr;
    }
    if (g_ffmpegVideoLockReady) {
        EnterCriticalSection(&g_ffmpegVideoLock);
        g_ffmpegVideoFrameReady = false;
        LeaveCriticalSection(&g_ffmpegVideoLock);
    }
}

bool StartDirectBmsFfmpegVideo(const std::string& path) {
    StopDirectBmsFfmpegVideo();
    if (!g_ffmpegVideoLockReady) {
        InitializeCriticalSection(&g_ffmpegVideoLock);
        g_ffmpegVideoLockReady = true;
    }
    if (!g_ffmpegVideoLatest)
        g_ffmpegVideoLatest = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kFfmpegVideoFrameBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_ffmpegVideoLatest) return false;
    PROCESS_INFORMATION process{};
    HANDLE pipe = nullptr;
    if (!startFfmpegPipe(path, "-an -r 30 -s 640x480 -pix_fmt bgr24 -f rawvideo -",
                         process, pipe, true))
        return false;
    CloseHandle(process.hThread);
    g_ffmpegVideoProcess = process.hProcess;
    g_ffmpegVideoPipe = pipe;
    InterlockedExchange(&g_ffmpegVideoStop, 0);
    g_ffmpegVideoThread = CreateThread(nullptr, 0, ffmpegVideoReader, nullptr, 0, nullptr);
    if (!g_ffmpegVideoThread) { StopDirectBmsFfmpegVideo(); return false; }
    LogLine("[bga-direct] ffmpeg stream started source='%s' format=BGR24/640x480/30fps", path.c_str());
    return true;
}

bool ReadDirectBmsFfmpegFrame(void* buffer, uint32_t bytes) {
    if (!g_ffmpegVideoProcess || !buffer || bytes != kFfmpegVideoFrameBytes ||
        !g_ffmpegVideoLockReady || !TryEnterCriticalSection(&g_ffmpegVideoLock)) return false;
    bool ready = g_ffmpegVideoFrameReady;
    if (ready) memcpy(buffer, g_ffmpegVideoLatest, bytes);
    LeaveCriticalSection(&g_ffmpegVideoLock);
    return ready;
}

static bool mediaFoundationDecodePcmLocal(const std::string& src, uint32_t maxSeconds,
                                          std::vector<unsigned char>& pcm,
                                          uint32_t& channels, uint32_t& rate, uint32_t& bits) {
    pcm.clear(); channels = rate = bits = 0;
    // .ogg and .mp3 decode in process (stb_vorbis / minimp3): no Media Foundation start-up per
    // file, and no ffmpeg.exe per file on XP where there is no Media Foundation at all.
    if (DecodeAudioInProcess(src, maxSeconds, pcm, channels, rate, bits)) return true;
    if (!MediaFoundationAvailable()) return ffmpegDecodePcmLocal(src, maxSeconds, pcm, channels, rate, bits);
    int wideLen = MultiByteToWideChar(CP_ACP, 0, src.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) return false;
    std::vector<wchar_t> wide((size_t)wideLen);
    MultiByteToWideChar(CP_ACP, 0, src.c_str(), -1, wide.data(), wideLen);

    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(co);
    HRESULT hr = pMFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool mfStarted = SUCCEEDED(hr);
    IMFSourceReader* reader = nullptr;
    IMFMediaType* requested = nullptr;
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(hr)) hr = pMFCreateSourceReaderFromURL(wide.data(), nullptr, &reader);
    // Native EZ2 .ssf are STEREO 44.1kHz/16-bit and the engine mixes at a FIXED 44.1k stereo — it
    // ignores the .ssf header rate. Our old mono/22.05k output therefore played at ~2x speed (half the
    // samples per second), and worse, left the mixer clocked at 22.05k so NATIVE previews then played
    // SLOW after a game. Match the native format exactly. Costs ~4x cache vs 22k-mono, but it's the
    // only correct rate. The SourceReader auto-inserts the resampler/upmixer to hit these params.
    const uint32_t kTargetChannels = 2, kTargetRate = 44100, kTargetBits = 16;
    if (SUCCEEDED(hr)) hr = pMFCreateMediaType(&requested);
    if (SUCCEEDED(hr)) hr = requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kTargetChannels);
    if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kTargetRate);
    if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kTargetBits);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, requested);
    if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);

    channels = SUCCEEDED(hr) ? MFGetAttributeUINT32(actual, MF_MT_AUDIO_NUM_CHANNELS, 0) : 0;
    rate = SUCCEEDED(hr) ? MFGetAttributeUINT32(actual, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) : 0;
    bits = SUCCEEDED(hr) ? MFGetAttributeUINT32(actual, MF_MT_AUDIO_BITS_PER_SAMPLE, 16) : 0;
    // Preview only needs a few seconds — stop decoding early so a long BGM doesn't freeze the UI for
    // minutes on hover. 0 = decode the whole file (keysounds path).
    size_t maxBytes = (maxSeconds && rate && channels && bits) ? (size_t)maxSeconds * rate * channels * (bits / 8) : 0;
    while (SUCCEEDED(hr)) {
        if (maxBytes && pcm.size() >= maxBytes) break;
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) break;
        if (sample) {
            IMFMediaBuffer* buffer = nullptr;
            hr = sample->ConvertToContiguousBuffer(&buffer);
            if (SUCCEEDED(hr)) {
                BYTE* data = nullptr;
                DWORD current = 0;
                hr = buffer->Lock(&data, nullptr, &current);
                if (SUCCEEDED(hr)) {
                    pcm.insert(pcm.end(), data, data + current);
                    buffer->Unlock();
                }
                buffer->Release();
            }
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    bool ok = channels > 0 && channels <= 0xffff && rate > 0 && bits > 0 && bits <= 0xffff &&
              !pcm.empty() && pcm.size() <= 0xffffffffu;
    if (actual) actual->Release();
    if (requested) requested->Release();
    if (reader) reader->Release();
    if (mfStarted) pMFShutdown();
    if (uninit) CoUninitialize();
    LogLine("[audio] media-foundation PCM src='%s' ch=%lu rate=%lu bits=%lu bytes=%lu ok=%d",
            src.c_str(), (unsigned long)channels, (unsigned long)rate, (unsigned long)bits,
            (unsigned long)pcm.size(), ok ? 1 : 0);
    return ok || ffmpegDecodePcmLocal(src, maxSeconds, pcm, channels, rate, bits);
}

static bool mediaFoundationToSsfLocal(const std::string& src, const std::string& dst, uint32_t maxSeconds = 0) {
    std::vector<unsigned char> pcm;
    uint32_t channels = 0, rate = 0, bits = 0;
    if (!mediaFoundationDecodePcmLocal(src, maxSeconds, pcm, channels, rate, bits)) return false;
    if (bits == 16) ConvertPcm16(pcm, channels, rate, 2, 44100);   // the in-process decoders keep the file's own format
    uint16_t ch16 = (uint16_t)channels, bits16 = (uint16_t)bits;
    uint16_t align = (uint16_t)(channels * bits / 8);
    uint32_t avg = rate * align, dataSize = (uint32_t)pcm.size();
    std::vector<unsigned char> ssf(18 + pcm.size());
    memcpy(ssf.data(), &ch16, 2); memcpy(ssf.data() + 2, &rate, 4); memcpy(ssf.data() + 6, &avg, 4);
    memcpy(ssf.data() + 10, &align, 2); memcpy(ssf.data() + 12, &bits16, 2); memcpy(ssf.data() + 14, &dataSize, 4);
    memcpy(ssf.data() + 18, pcm.data(), pcm.size());
    return writeBinaryFile(dst, ssf);
}

static bool createSsfFromResolvedAudio(const std::string& src, const std::string& cacheDir, const std::string& resolvedName, std::string& sampleNameOut, bool decode = true) {
    std::string name = fileNameOnlyLocal(resolvedName);
    std::string low = lowerCopy(name);
    std::string stem = stripExtensionLocal(name);
    std::string ssfNameOut = stem + ".ssf";
    sampleNameOut = ssfNameOut;
    std::string ssfDst = JoinPath(cacheDir, ssfNameOut);
    // Naming-only mode: resolve the sample ref WITHOUT the heavy PCM decode, so the .ezi can be
    // written up-front while the keysound bytes are deferred to first play (EnsureSongKeysounds).
    if (!decode) { sampleNameOut = endsWith(low, ".ssf") ? name : ssfNameOut; return true; }
    // ponytail: only .ssf is ever requested (EZI references it, redirect serves it). The old
    // .ezw cache copy was byte-identical dead weight = 2x the audio cache. Write .ssf only.

    if (endsWith(low, ".ssf")) {
        sampleNameOut = name;
        bool ok = copyFileSimple(src, JoinPath(cacheDir, name));
        LogLine("[cache] audio ssf copy src='%s' sampleRef='%s' ok=%d", src.c_str(), sampleNameOut.c_str(), ok ? 1 : 0);
        return ok;
    }
    if (endsWith(low, ".ezw")) {
        bool ok = copyFileSimple(src, ssfDst);
        sampleNameOut = ssfNameOut;
        LogLine("[cache] audio ezw->ssf src='%s' sampleRef='%s' ok=%d", src.c_str(), sampleNameOut.c_str(), ok ? 1 : 0);
        return ok;
    }

    bool converted = endsWith(low, ".wav") ? wavToNativeSsfLocal(src, ssfDst) : false;
    if (!converted) converted = mediaFoundationToSsfLocal(src, ssfDst);
    if (converted) {
        sampleNameOut = ssfNameOut;
        LogLine("[cache] audio wav->native ssf src='%s' ssf='%s' sampleRef='%s' ok=%d",
                src.c_str(), ssfNameOut.c_str(), sampleNameOut.c_str(), converted ? 1 : 0);
        return true;
    }

    LogLine("[cache] audio conversion failed without fallback src='%s'", src.c_str());
    return false;
}

// A cached .ez written under a different tick grid still PLAYS correctly -- the resolution travels
// in its header -- but it keeps the old quantisation error, so it has to be rebuilt to gain
// anything. Reading two bytes is far cheaper than reparsing, and it means the grid can change
// without anyone having to remember to wipe CustomCache by hand.
static bool ezCacheMatchesGrid(const std::string& ezPath) {
    FILE* f = fopen(ezPath.c_str(), "rb");
    if (!f) return false;
    unsigned char head[0x88] = {};
    size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (got < sizeof(head) || memcmp(head, "EZFF", 4) != 0) return false;
    uint16_t res = (uint16_t)(head[0x86] | (head[0x87] << 8));
    return res == (uint16_t)kEzTicksPerMeasure;
}

static bool generateEzFromBms(const std::string& bmsPath, const std::string& ezPath,
                              const std::string& baseName, std::string* report,
                              std::vector<unsigned char>* memoryOut = nullptr) {
    ParsedBmsForEz bms;
    if (!parseBmsForEz(bmsPath, bms)) return false;
    std::vector<EzRawEventOut> byTrack[64];
    uint32_t totalTicks = kEzTicksPerMeasure;
    for (const auto& e : bms.events) {
        int tr = bmsEventToEzTrack5Key(e);
        if (tr < 0 || tr >= 64) continue;
        uint32_t tick = bmsTickToEzTick(bms, e.measure, e.index, e.div);
        EzRawEventOut out{tick, tr, e.wavId, 6, e.channel, 1, 0.0f};
        byTrack[tr].push_back(out);
        if (tick + kEzTicksPerMeasure > totalTicks) totalTicks = tick + kEzTicksPerMeasure;
    }

    // Native EZ stores BPM changes as type-3 events in track 0.
    byTrack[0].push_back({0, 0, 0, 0, 0, 3, (float)bms.bpm});
    std::vector<std::pair<uint32_t, double>> tempos;
    tempos.push_back({0, bms.bpm});
    for (const auto& te : bms.tempoEvents) {
        uint32_t tick = bmsTickToEzTick(bms, te.measure, te.index, te.div);
        byTrack[0].push_back({tick, 0, 0, 0, 0, 3, (float)te.bpm});
        tempos.push_back({tick, te.bpm});
        if (tick + kEzTicksPerMeasure > totalTicks) totalTicks = tick + kEzTicksPerMeasure;
    }

    int longNotes = 0;
    for (int t = 0; t < 64; ++t) {
        auto& events = byTrack[t];
        std::stable_sort(events.begin(), events.end(), [](const EzRawEventOut& a, const EzRawEventOut& b){ return a.tick < b.tick; });

        // BMS LNTYPE 1 uses alternating endpoints. EZ uses one event whose length is
        // the distance to the release; emitting both endpoints creates phantom MISSes.
        int open = -1;
        for (size_t i = 0; i < events.size(); ++i) {
            if (!isLongNoteChannel(events[i].ch)) continue;
            if (open < 0) {
                open = (int)i;
            } else {
                uint32_t delta = events[i].tick > events[(size_t)open].tick ? events[i].tick - events[(size_t)open].tick : 7;
                events[(size_t)open].len = (int)std::min<uint32_t>(delta, 0xffff);
                if (events[(size_t)open].len < kMinNoteLenTicks)
                    events[(size_t)open].len = kMinNoteLenTicks;
                events[i].type = -1;
                open = -1;
                ++longNotes;
            }
        }

        std::vector<EzRawEventOut> normalized;
        normalized.reserve(events.size());
        for (const auto& ev : events) {
            if (ev.type < 0) continue;
            if (bms.lnObjId > 0 && ev.type == 1 && !isLongNoteChannel(ev.ch) && ev.ch != 1 && ev.wav == bms.lnObjId) {
                for (auto it = normalized.rbegin(); it != normalized.rend(); ++it) {
                    if (it->type == 1 && it->ch == ev.ch && it->len == 6) {
                        uint32_t delta = ev.tick > it->tick ? ev.tick - it->tick : 7;
                        it->len = (int)std::min<uint32_t>(delta, 0xffff);
                        if (it->len < 7) it->len = 7;
                        ++longNotes;
                        break;
                    }
                }
                continue;
            }
            normalized.push_back(ev);
        }
        events.swap(normalized);
    }

    // Virtual target-mode charts are redistributed only after long notes have been normalized into
    // single events. This guarantees both LN endpoints stay on the same generated control lane.
    ApplyModeAutoGeneration(byTrack, bms, g_curGenerateMode, baseName);

    // End of chart = the last tick REACHED by any note, counting a long note's tail (tick+len), not just
    // its head. The earlier totalTicks only saw note heads (tick+192), so a chart ending on a held long
    // declared itself over mid-hold → forced result before the release. Recompute from resolved events,
    // THEN pad ~3s so no note type (tap or long) snaps straight to the result screen.
    for (int t = 0; t < 64; ++t)
        for (const auto& ev : byTrack[t]) {
            if (ev.type == 3) continue; // BPM marker, not a note
            uint32_t endTick = ev.tick + (uint32_t)(ev.len > (int)kEzTicksPerMeasure ? ev.len : (int)kEzTicksPerMeasure);
            if (endTick > totalTicks) totalTicks = endTick;
        }
    {
        uint32_t pad = (uint32_t)(3.0 * 0.8 * (bms.bpm > 0.0 ? bms.bpm : 120.0) * kModelTickScale);
        if (pad < kEzTicksPerMeasure) pad = kEzTicksPerMeasure;
        totalTicks += pad;

        // Declaring the longer chart in the header was not enough: the stage still ended the moment
        // the last note was consumed, which on a chart finishing in a long note meant the result
        // screen arrived while it was still being held. So the padding is given something to hold
        // on to -- one BPM marker at the padded end.
        //
        // Type 3 is the BPM event, not a note: the engine advances through it and never judges it,
        // so this buys the reaction room without adding anything playable. It goes on the track
        // that already carries the tempo markers, so nothing else has to know about it.
        int tempoTrack = -1;
        for (int t = 0; t < 64 && tempoTrack < 0; ++t)
            for (const auto& ev : byTrack[t])
                if (ev.type == 3) { tempoTrack = t; break; }
        if (tempoTrack < 0) tempoTrack = 0;
        double tailBpm = bms.bpm > 0.0 ? bms.bpm : 120.0;
        if (!tempos.empty()) tailBpm = tempos.back().second;
        EzRawEventOut tail{};
        tail.tick = totalTicks > 1 ? totalTicks - 1 : 0;
        tail.type = 3;
        tail.track = tempoTrack;
        tail.bpm   = (float)tailBpm;
        byTrack[tempoTrack].push_back(tail);
    }

    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    double durationSeconds = 0.0;
    double currentBpm = bms.bpm;
    uint32_t cursor = 0;
    for (const auto& tempo : tempos) {
        uint32_t tick = std::min(tempo.first, totalTicks);
        if (tick > cursor && currentBpm > 0.0) durationSeconds += (tick - cursor) * 240.0 / ((double)kEzTicksPerMeasure * currentBpm);
        cursor = tick;
        if (tempo.second > 0.0) currentBpm = tempo.second;
    }
    if (totalTicks > cursor && currentBpm > 0.0) durationSeconds += (totalTicks - cursor) * 240.0 / ((double)kEzTicksPerMeasure * currentBpm);

    std::vector<unsigned char> out;
    out.resize(0x96, 0);
    out[0] = 'E'; out[1] = 'Z'; out[2] = 'F'; out[3] = 'F';
    out[4] = 0; out[5] = 8; // v23: native FNEX charts are version 8; basename must match the .ez/.es filename.
    putFixedString(out, 0x06, 0x40, baseName);
    putFixedString(out, 0x46, 0x40, bms.artist);
    putU16(out, 0x86, (uint16_t)kEzTicksPerMeasure);
    putFloat(out, 0x88, (float)bms.bpm);
    putU16(out, 0x8c, 64);
    putU32(out, 0x8e, totalTicks);
    putFloat(out, 0x92, (float)durationSeconds);

    for (int t = 0; t < 64; ++t) {
        std::vector<unsigned char> lh(0x4e, 0);
        lh[0] = 'E'; lh[1] = 'Z'; lh[2] = 'T'; lh[3] = 'R';
        putU16(lh, 0x04, (uint16_t)t);
        char lname[64];
        if (t == 0) lname[0] = 0;
        else if (t == 1) lstrcpynA(lname, "Dummy", sizeof(lname));
        else if (t == 2) lstrcpynA(lname, "Dummy2", sizeof(lname));
        else if (t >= 3 && t <= 9) wsprintfA(lname, "(1P)Line %d", t - 2);
        else if (t == 10) lstrcpynA(lname, "(1P)Scratch", sizeof(lname));
        else if (t == 11) lstrcpynA(lname, "(1P)Pedal", sizeof(lname));
        else if (t >= 12 && t <= 18) wsprintfA(lname, "(2P)Line %d", t - 11); // club/space P2 lanes
        else if (t == 19) lstrcpynA(lname, "(2P)Pedal", sizeof(lname));
        else if (t == 20) lstrcpynA(lname, "(2P)Scratch", sizeof(lname));
        else if (t >= 22 && t <= 31) wsprintfA(lname, "BG %d", t - 21);
        else lstrcpynA(lname, "Dummy", sizeof(lname));
        putFixedString(lh, 0x06, 0x40, lname);
        putU32(lh, 0x46, totalTicks);
        putU32(lh, 0x4a, (uint32_t)(byTrack[t].size() * 13));
        out.insert(out.end(), lh.begin(), lh.end());
        for (const auto& ev : byTrack[t]) {
            size_t off = out.size();
            out.resize(off + 13, 0);
            putU32(out, off + 0, ev.tick);
            out[off + 4] = (unsigned char)ev.type;
            if (ev.type == 3) {
                putFloat(out, off + 5, ev.bpm);
            } else {
                putU16(out, off + 5, (uint16_t)ev.wav);
                out[off + 7] = 0x7f;
                out[off + 8] = 0x40;
                out[off + 9] = 0x00;
                // A tap never had its length set, so it arrived here as 0. Everything the game
                // ships carries at least kMinNoteLenTicks, long or not.
                putU16(out, off + 10,
                       (uint16_t)(ev.len < kMinNoteLenTicks ? kMinNoteLenTicks : ev.len));
                out[off + 12] = 0x00;
            }
        }
    }
    if (report) {
        size_t n = 0; for (int t=0;t<64;++t) n += byTrack[t].size();
        *report = "events=" + std::to_string((unsigned long long)n) + " bpm=" + std::to_string(bms.bpm) +
                  " ticks=" + std::to_string((unsigned long long)totalTicks) + " seconds=" + std::to_string(durationSeconds) +
                  " longNotes=" + std::to_string(longNotes);
    }
    if (!validateGeneratedEz(out)) {
        LogLine("[cache] ez selfcheck failed chart='%s'", fileNameOnlyLocal(bmsPath).c_str());
        return false;
    }
    if (memoryOut) {
        *memoryOut = std::move(out);
        return true;
    }
    LogLine("[cache] ez selfcheck ok chart='%s' ticks=%lu seconds=%.3f longNotes=%d",
            fileNameOnlyLocal(bmsPath).c_str(), (unsigned long)totalTicks, durationSeconds, longNotes);
    // Write plaintext .es as a bypass fallback: if the game's .ez decryptor runs but the
    // encrypted .ez key is somehow off, HookGameFsOpen diverts the decryptor's .es write
    // to discard and FUN_00411460 opens our pre-written plaintext .es directly.
    std::string esPath = replaceExtensionLocal(ezPath, ".es");
    writeBinaryFile(esPath, out);
    writeBinaryFile(ezPath + ".plain.ez", out);
    // Use runtime key (reads actual game binary at 0x0a74b0) instead of hardcoded table.
    bool ok = writeEncryptedFileRuntime(ezPath, out.data(), out.size(), 0x0a74b0);
    LogLine("[cache] ez runtime-key encrypted file='%s' size=%d keyRva=0x0a74b0 esPreWrite=1 ok=%d",
            fileNameOnlyLocal(ezPath).c_str(), (int)out.size(), ok ? 1 : 0);
    return ok;
}

static std::string normalizeRelPath(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

static bool fileExistsLocal(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static uint64_t fileMTimeLocal(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}
// A cached artifact is stale if the source file was modified after it (e.g. the user swapped the BMS).
static bool cacheIsFresh(const std::string& cached, const std::string& source) {
    if (!fileExistsLocal(cached)) return false;
    uint64_t c = fileMTimeLocal(cached), s = fileMTimeLocal(source);
    return c != 0 && c >= s; // cached newer-or-equal than source → still valid
}

static bool hasFileExtensionLocal(const std::string& p) {
    std::string fn = p;
    size_t slash = fn.find_last_of("\\/");
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    size_t dot = fn.find_last_of('.');
    return dot != std::string::npos && dot + 1 < fn.size();
}

static std::string fileNameOnlyLocal(std::string p) {
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    return p;
}

static std::string lowerPathLocal(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
    return s;
}

static std::string stemOnlyLocal(std::string p) {
    p = fileNameOnlyLocal(p);
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos) p = p.substr(0, dot);
    return p;
}

static void addAudioCandidate(std::vector<std::string>& candidates, const std::string& c) {
    if (c.empty()) return;
    std::string n = normalizeRelPath(c);
    for (const auto& x : candidates) {
        if (lowerPathLocal(x) == lowerPathLocal(n)) return;
    }
    candidates.push_back(n);
}

static bool recursiveFindAudioByName(const std::string& dir, const std::string& wantedName, bool stemMatch, int depth, std::string& found) {
    if (depth > 5 || wantedName.empty()) return false;
    std::string pattern = JoinPath(dir, "*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string wantedLower = lowerPathLocal(wantedName);
    std::string wantedStem = lowerPathLocal(stemOnlyLocal(wantedName));
    do {
        const char* n = fd.cFileName;
        if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
        std::string full = JoinPath(dir, n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursiveFindAudioByName(full, wantedName, stemMatch, depth + 1, found)) {
                FindClose(h);
                return true;
            }
        } else {
            std::string nameLower = lowerPathLocal(fileNameOnlyLocal(n));
            std::string stemLower = lowerPathLocal(stemOnlyLocal(n));
            if (nameLower == wantedLower || (stemMatch && stemLower == wantedStem)) {
                found = full;
                FindClose(h);
                return true;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return false;
}

static bool resolveBmsAudioAsset(const std::string& songDir, const std::string& relIn, std::string& srcOut, std::string& cacheNameOut) {
    std::string rel = normalizeRelPath(stripQuotes(trim(relIn)));
    std::vector<std::string> candidates;
    addAudioCandidate(candidates, rel);
    addAudioCandidate(candidates, JoinPath("sound", rel));
    addAudioCandidate(candidates, JoinPath("Sound", rel));
    addAudioCandidate(candidates, JoinPath("SOUND", rel));

    const char* exts[] = {".wav", ".ogg", ".mp3", ".flac", ".aiff", ".aif", ".m4a", ".aac",
                          ".wma", ".mp2", ".ac3", ".ape", ".wv", ".opus", ".ssf", ".ezw"};
    if (!hasFileExtensionLocal(rel)) {
        for (const char* e : exts) {
            addAudioCandidate(candidates, rel + e);
            addAudioCandidate(candidates, JoinPath("sound", rel + e));
            addAudioCandidate(candidates, JoinPath("Sound", rel + e));
            addAudioCandidate(candidates, JoinPath("SOUND", rel + e));
        }
    }

    std::string baseOnly = fileNameOnlyLocal(rel);
    if (baseOnly != rel) {
        addAudioCandidate(candidates, baseOnly);
        addAudioCandidate(candidates, JoinPath("sound", baseOnly));
        if (!hasFileExtensionLocal(baseOnly)) {
            for (const char* e : exts) {
                addAudioCandidate(candidates, baseOnly + e);
                addAudioCandidate(candidates, JoinPath("sound", baseOnly + e));
            }
        }
    }

    for (const std::string& c : candidates) {
        std::string src = JoinPath(songDir, c);
        if (fileExistsLocal(src)) {
            srcOut = src;
            cacheNameOut = fileNameOnlyLocal(c);
            return true;
        }
    }

    std::string found;
    for (const std::string& c : candidates) {
        if (recursiveFindAudioByName(songDir, fileNameOnlyLocal(c), false, 0, found)) {
            srcOut = found;
            cacheNameOut = fileNameOnlyLocal(found);
            return true;
        }
    }
    if (!hasFileExtensionLocal(rel)) {
        if (recursiveFindAudioByName(songDir, baseOnly, true, 0, found)) {
            srcOut = found;
            cacheNameOut = fileNameOnlyLocal(found);
            return true;
        }
    }

    srcOut = JoinPath(songDir, rel);
    cacheNameOut = fileNameOnlyLocal(rel);
    return false;
}

static bool isNativeBmsVisual(const std::string& path) {
    return endsWith(path, ".bmp") || endsWith(path, ".abm");
}

static std::string directVisualCandidate(const CustomBmsSong& song, const std::string& rel,
                                         const std::string& wantedExt) {
    if (rel.empty()) return std::string();
    std::string path = rel;
    if (!(path.size() > 1 && path[1] == ':') && path.front() != '\\' && path.front() != '/')
        path = JoinPath(song.songDir, path);
    if (!isNativeBmsVisual(path) || (!wantedExt.empty() && !endsWith(path, wantedExt.c_str())))
        return std::string();
    // KGraphics::loadBmpTexture (decomp kgraphics2.cpp @0x407c60) reads the WHOLE file into a
    // static 8 MB buffer at 0x530c48 with no size check, and the texture profile sits right after
    // it. A BMS stagefile past that (1920x1080 32-bit is 8.3 MB) would overwrite .data, so it is
    // not served; the generated art stands in.
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &ad)) return std::string();
    if (ad.nFileSizeHigh || ad.nFileSizeLow >= 0x800000u) {
        LogLine("[direct] visual '%s' is %lu bytes, past the game's 8MB bmp buffer; skipped",
                path.c_str(), (unsigned long)ad.nFileSizeLow);
        return std::string();
    }
    return path;
}

std::string ResolveDirectBmsVisual(const CustomBmsSong& song, const std::string& request) {
    std::string low = lowerPathLocal(request);
    std::string role;
    if (low.find("resultdisc") != std::string::npos) role = "resultdisc";
    else if (low.find("discsmall") != std::string::npos) role = "discsmall";
    else if (low.find("eyecatch") != std::string::npos) role = "eyecatch";
    else if (low.find("songname") != std::string::npos) role = "songname";
    else if (low.find("disc") != std::string::npos) role = "disc";
    else return std::string();

    std::string wantedExt = endsWith(low, ".abm") ? ".abm" : endsWith(low, ".bmp") ? ".bmp" : "";
    int wantedSlot = low.find("-shd") != std::string::npos ? 2
                   : low.find("-hd")  != std::string::npos ? 1
                   : low.find("-ex")  != std::string::npos ? 3 : 0;
    const CustomBmsChart* chart = nullptr;
    for (const auto& c : song.chartMeta) if (c.slot == wantedSlot) { chart = &c; break; }
    if (!chart && !song.chartMeta.empty()) chart = &song.chartMeta.front();

    // Explicit role files win. Difficulty-tagged files win over the shared role image.
    std::string shared;
    const char* suffix = wantedSlot == 1 ? "-hd" : wantedSlot == 2 ? "-shd" : wantedSlot == 3 ? "-ex" : "-ez";
    for (const auto& file : song.imageFiles) {
        std::string name = lowerCopy(file);
        if (name.find(role) == std::string::npos) continue;
        std::string path = directVisualCandidate(song, file, wantedExt);
        if (path.empty()) continue;
        if (name.find(suffix) != std::string::npos) return path;
        if (shared.empty()) shared = path;
    }
    if (!shared.empty()) return shared;

    // BMS-native selector art. v0.2.9 intentionally limits this to BMP/ABM because its
    // PNG/JPG renderer hook corrupted shared gameplay surfaces.
    if (chart) {
        std::string path = directVisualCandidate(song, chart->banner, wantedExt);
        if (!path.empty()) return path;
        path = directVisualCandidate(song, chart->stageFile, wantedExt);
        if (!path.empty()) return path;
    }
    for (const auto& file : song.imageFiles) {
        std::string path = directVisualCandidate(song, file, wantedExt);
        if (!path.empty()) return path;
    }
    return std::string();
}

static bool generateEziFromBms(const CustomBmsSong& song, const std::string& bmsPath, const std::string& eziPath, const std::string& ezzPath, bool materializeAudio) {
    ParsedBmsForEz bms;
    if (!parseBmsForEz(bmsPath, bms)) return false;

    std::set<int> playableIds;
    std::set<int> bgmIds;
    for (const auto& e : bms.events) {
        if (e.wavId <= 0 || e.wavId > 0x7ff) continue;
        if (e.channel == 1) bgmIds.insert(e.wavId);
        if (bmsChannelToEzTrack5Key(e.channel) >= 0 && e.channel != 1) playableIds.insert(e.wavId);
    }

    // EZI contains both playable keysounds and channel-01 background samples. The EZ
    // track decides when they play; the EZI second field is 1 for background-only data.
    std::string text;
    int expectedWritten = 0, missing = 0, backgroundWritten = 0;
    for (const auto& kv : bms.wavMap) {
        int id = kv.first;
        if (id <= 0 || id > 0x7ff) continue;
        if (playableIds.count(id) == 0 && bgmIds.count(id) == 0) continue;
        bool bgmOnly = (bgmIds.count(id) != 0 && playableIds.count(id) == 0);
        std::string src, cacheName;
        bool found = resolveBmsAudioAsset(song.songDir, kv.second, src, cacheName);
        if (!found) { missing++; continue; }
        std::string sampleRef;
        bool audioOk = createSsfFromResolvedAudio(src, song.cacheDir, cacheName, sampleRef, materializeAudio);
        if (!audioOk) { missing++; continue; }
        text += std::to_string(id) + (bgmOnly ? " 1 " : " 0 ") + fileNameOnlyLocal(sampleRef) + "\r\n";
        if (bgmOnly) ++backgroundWritten;
        expectedWritten++;
    }

    writeTextFile(eziPath + ".expected_keysounds_plain.txt", text);
    bool ok = writeEncryptedEziFile(eziPath, text);
    DeleteFileA(ezzPath.c_str());
    writeTextFile(eziPath + ".plain.txt", text);

    LogLine("[cache] audio ezi encrypted chart='%s' wavRefs=%d playableIds=%d samples=%d background=%d missing=%d ok=%d nativeSsf=1",
            fileNameOnlyLocal(bmsPath).c_str(), (int)bms.wavMap.size(), (int)playableIds.size(),
            expectedWritten, backgroundWritten, missing, ok ? 1 : 0);
    return ok;
}

// Slots 0..3 are the game's NM/HD/SHD/EX; slot 4 is the mod's EZ column, and "-ez" is a suffix
// only ever seen by our own hooks (the game builds names from what we put in its text slot).
static const int kChartSlots = 5;
static const char* suffixForSlot(int slot) {
    static const char* suffixes[kChartSlots] = {"", "-hd", "-shd", "-ex", "-ez"};
    return slot >= 0 && slot < kChartSlots ? suffixes[slot] : "";
}

static int clampPlayLevelForSongBin(const std::string& raw) {
    int v = parseIntSafe(trim(raw), 0);
    if (v < 1) v = 1;
    if (v > 18) v = 18;
    return v;
}

static float parseBpmForSongBin(const std::string& raw) {
    double v = parseDoubleSafe(trim(raw), 0.0);
    if (v <= 0.0) return 0.0f;
    return (float)v;
}

static void AssignSongBinDifficultyFields(CustomBmsSong& song) {
    song.levelNM = song.levelHD = song.levelSHD = song.levelEX = song.levelEZ = 0;
    song.bpmNM = song.bpmHD = song.bpmSHD = song.bpmEX = song.bpmEZ = 0.0f;
    std::vector<size_t> ordered;
    // .mut() here and below is safe because this runs from scanFlatSongFolder, long before
    // ExpandAutoGeneratedModes shares this buffer with the song's virtual copies. Writing to a
    // shared buffer after that point would rewrite the original and every sibling mode at once.
    for (size_t i = 0; i < song.chartMeta.size(); ++i) {
        auto& chart = song.chartMeta.mut()[i];
        chart.slot = -1;
        chart.playLevel = std::to_string(clampPlayLevelForSongBin(chart.playLevel));
        ordered.push_back(i);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        int la = clampPlayLevelForSongBin(song.chartMeta[a].playLevel);
        int lb = clampPlayLevelForSongBin(song.chartMeta[b].playLevel);
        if (la != lb) return la < lb;
        int byName = _stricmp(song.chartMeta[a].file.c_str(), song.chartMeta[b].file.c_str());
        return byName != 0 ? byName < 0 : song.chartMeta[a].file < song.chartMeta[b].file;
    });

    // Up to four charts fill NM..EX in level order. A fifth goes to the EZ column (slot 4) as
    // the easiest, with the other four in NM..EX; past five, EZ takes the easiest, EX the hardest
    // and NM/HD/SHD are spaced over what is left. Slot order always follows authored #PLAYLEVEL.
    const size_t n = ordered.size();
    if (n <= 4) {
        for (size_t slot = 0; slot < n; ++slot) song.chartMeta.mut()[ordered[slot]].slot = (int)slot;
    } else {
        song.chartMeta.mut()[ordered[0]].slot = 4;                 // EZ
        const size_t rest = n - 1;                                 // ordered[1..n-1] -> NM..EX
        for (size_t slot = 0; slot < 4; ++slot) {
            size_t pos = rest <= 4 ? slot : (slot * (rest - 1) + 1) / 3;
            song.chartMeta.mut()[ordered[1 + pos]].slot = (int)slot;
        }
    }

    int* levels[kChartSlots] = {&song.levelNM, &song.levelHD, &song.levelSHD, &song.levelEX, &song.levelEZ};
    float* bpms[kChartSlots] = {&song.bpmNM, &song.bpmHD, &song.bpmSHD, &song.bpmEX, &song.bpmEZ};
    for (const auto& chart : song.chartMeta) {
        if (chart.slot < 0 || chart.slot >= kChartSlots) continue;
        int level = clampPlayLevelForSongBin(chart.playLevel);
        *levels[chart.slot] = level;
        float bpm = parseBpmForSongBin(chart.bpm);
        if (bpm > 0.0f) *bpms[chart.slot] = bpm;
    }
}

static void FinalizeSongBinDifficultyFields(CustomBmsSong& song) {
    // Use the first known chart BPM for all empty slots, matching native song.bin's repeated BPM fields.
    float firstBpm = 0.0f;
    if (song.bpmNM > 0.0f) firstBpm = song.bpmNM;
    else if (song.bpmHD > 0.0f) firstBpm = song.bpmHD;
    else if (song.bpmSHD > 0.0f) firstBpm = song.bpmSHD;
    else if (song.bpmEX > 0.0f) firstBpm = song.bpmEX;
    if (firstBpm <= 0.0f) firstBpm = 120.0f;
    if (song.bpmNM <= 0.0f) song.bpmNM = firstBpm;
    if (song.bpmHD <= 0.0f) song.bpmHD = firstBpm;
    if (song.bpmSHD <= 0.0f) song.bpmSHD = firstBpm;
    if (song.bpmEX <= 0.0f) song.bpmEX = firstBpm;
    if (song.bpmEZ <= 0.0f) song.bpmEZ = firstBpm;
}

static std::string modePrefixForCache(const std::string& mode) {
    std::string m = NormalizeModeName(mode);
    if (m == "5keyMix") return "5keyMix1p-";
    if (m == "streetMix") return "streetmix1p-";
    if (m == "5keyOnly") return "5keyOnly1p-";
    if (m == "7keyMix") return "7streetmix1p-";
    if (m == "clubMix") return "clubmix1p-";
    if (m == "spaceMix") return "spacemix1p-";
    if (m == "rubyMix") return "rubymix1p-";
    if (m == "ez2Catch") return "catch1p-";
    if (m == "turnTable") return "scratchmix1p-";
    if (m == "cv2Mix") return "cv2mix1p-";
    return "5keyMix1p-";
}

// 7-key modes have a 6th/7th key column (channels 16/17 = keys, not scratch): 7-key & space (14=7+7).
static bool modeIsSevenKey(const std::string& mode) {
    std::string m = NormalizeModeName(mode);
    return m == "7keyMix" || m == "spaceMix";
}

// The per-chart .gds the game reads through loadGds (decomp maingame.cpp @0x423790). Without it
// the game falls back to Kool 6/Cool 24/Good 36/Miss 72 and Level 99; natives ship Good 50/Miss 70.
// Format and values are those of the shipped natives (decrypted Sound\10Anti\*.ini); the per-mode
// judgement/gauge sets are the ones every native carries for that mode. No [SongOriginalName]:
// a title with '[' in it would read as a section header and skip the sections after it.
static std::string BuildSongIni(const CustomBmsSong& song, const CustomBmsChart& chart) {
    const std::string m = NormalizeModeName(song.mode);
    // The windows are in CHART TICKS: the loader sets m_nTPB = resolution x 4 (decomp
    // ez2djload.cpp), so a native chart's tick is 1/192 beat and ours, at kEzTicksPerMeasure,
    // is 1/384. The native numbers therefore have to be scaled by the grid ratio, or every
    // window on a custom chart is half as many milliseconds as on a native one. The engine then
    // widens every window by 3 ticks (decomp judge-timing.md, the caller of Judge::setWindows)
    // whatever the grid, so that part is pre-scaled too: (v + 3) * k - 3 lands on exactly the
    // native width in milliseconds.
    const int k = (int)kModelTickScale;
    const int widen = 3 * (k - 1);
    int kool = 6, cool = 24, good = 50, miss = 70;
    if (m == "ez2Catch")       { kool = 5;  cool = 18; good = 40; miss = 56; }
    else if (m == "turnTable") { kool = 10; cool = 21; good = 45; miss = 60; }
    char judge[96];
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

// Direct mode writes no playable cache, so the .ini the game asks for at chart load
// ("<mode>1p-<id>[-hd|-shd|-ex].ini", bare name after pushDirectory) is produced on demand,
// keyed by the difficulty suffix so Level matches the chart. Returns the path to serve, or "".
std::string EnsureSongIni(const CustomBmsSong& song, const std::string& requestedName) {
    if (song.cacheDir.empty()) return std::string();
    std::string lower = lowerCopy(requestedName);
    size_t idPos = lower.find(lowerCopy(song.id));
    if (idPos == std::string::npos || lower.size() < 4 ||
        lower.compare(lower.size() - 4, 4, ".ini") != 0) return std::string();
    std::string suffix = lower.substr(idPos + song.id.size(), lower.size() - 4 - idPos - song.id.size());
    int slot = 0;
    for (int s = 1; s < kChartSlots; ++s) if (suffix == suffixForSlot(s)) slot = s;
    const CustomBmsChart* chart = nullptr;
    for (size_t i = 0; i < song.chartMeta.size(); ++i)
        if (song.chartMeta[i].slot == slot && (!chart || song.chartMeta[i].file < chart->file)) chart = &song.chartMeta[i];
    if (!chart) for (size_t i = 0; i < song.chartMeta.size() && !chart; ++i) if (song.chartMeta[i].slot == 0) chart = &song.chartMeta[i];
    if (!chart) return std::string();
    std::string dst = JoinPath(song.cacheDir, requestedName);
    const std::string ini = BuildSongIni(song, *chart);
    // Fresh AND byte-for-byte what the current BuildSongIni would write: the cipher is
    // deterministic, so an ini from an older version (other window values, even of the same
    // length) is replaced, not served. A few hundred bytes compared per chart load.
    if (cacheIsFresh(dst, JoinPath(song.songDir, chart->file))) {
        std::vector<unsigned char> have;
        if (readWholeFileForModel(dst, have) &&
            have == encryptFnexRuntimeKey(reinterpret_cast<const unsigned char*>(ini.data()), ini.size(), 0x0a8568))
            return dst;
    }
    ensureDir(JoinPath(GetExeDirectory(), Config().cacheRoot));
    ensureDir(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), song.mode));
    ensureDir(song.cacheDir);
    bool ok = writeEncryptedTextFileRuntime(dst, ini, 0x0a8568);
    LogLine("[direct] ini id='%s' slot=%d level=%s file='%s' ok=%d", song.id.c_str(), slot,
            chart->playLevel.c_str(), requestedName.c_str(), ok ? 1 : 0);
    return ok ? dst : std::string();
}

static void GeneratePlayableCacheV10(const CustomBmsSong& song, bool materializeAudio) {
    // All EZ2 modes share the same .ez/.ezi format and channel→track mapping (see
    // bmsChannelToEzTrackMode); the mode only sets the cache prefix and the 7-key flag. Catch is
    // still charts-as-lanes here (playable, but the "catch" gameplay uses X-position, not lanes —
    // ponytail: good enough to appear/play; faithful catch positions would need a separate encoder).
    g_curModeSevenKey = modeIsSevenKey(song.mode);
    g_curGenerateMode = song.mode;
    g_curAutoGenerate = song.autoGenerated;
    g_curAutoEnrich = !song.autoGenerated && Config().autoGenerateEnrichSource;
    std::vector<CustomBmsChart> charts = song.chartMeta;
    charts.erase(std::remove_if(charts.begin(), charts.end(), [](const CustomBmsChart& c){ return c.slot < 0 || c.slot >= kChartSlots; }), charts.end());
    std::sort(charts.begin(), charts.end(), [](const CustomBmsChart& a, const CustomBmsChart& b){
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.file < b.file;
    });
    if (charts.empty()) {
        LogLine("[cache] no compatible 5-key BMS charts for id='%s'", song.id.c_str());
        return;
    }
    std::string prefix = modePrefixForCache(song.mode);
    int emitted = 0;
    for (size_t i=0; i<charts.size(); ++i) {
        std::string suffix = suffixForSlot(charts[i].slot);
        std::string base = prefix + song.id + suffix;
        std::string src = JoinPath(song.songDir, charts[i].file);
        std::string ezPath = JoinPath(song.cacheDir, base + ".ez");
        std::string eziPath = JoinPath(song.cacheDir, base + ".ezi");
        std::string ezzPath = JoinPath(song.cacheDir, base + ".ezz");
        std::string report;
        // Warm boot: .ez + .ezi already cached AND newer than the source BMS → skip (the bulk of a warm
        // scan was re-parsing these). The mtime check invalidates the cache when the user swaps a BMS
        // (same folder/id, new content) — otherwise stale .ez kept old levels/notes. Was a plain
        // exists-check; that froze edits until the CustomCache folder was manually deleted.
        if (!materializeAudio && cacheIsFresh(ezPath, src) && cacheIsFresh(eziPath, src) &&
            ezCacheMatchesGrid(ezPath)) { emitted++; continue; }
        // materializeAudio pass only needs to (re)write the keysound .ssf via the EZI loop;
        // the .ez chart is already on disk from the up-front pass, so skip re-parsing it.
        bool okEz = materializeAudio ? true : generateEzFromBms(src, ezPath, base, &report);
        bool okEzi = generateEziFromBms(song, src, eziPath, ezzPath, materializeAudio);
        // v13 writes plaintext EZZ/EZI payload with NO header/comment lines.
        // FNEX LoadEZI's comment handling does not skip the rest of the line,
        // so comments can be misread as a sample named "by".
        LogLine("[cache] generated id='%s' chart='%s' base='%s' ez=%d ezi=%d %s", song.id.c_str(), charts[i].file.c_str(), base.c_str(), okEz?1:0, okEzi?1:0, report.c_str());
        emitted++;
    }
    if (!materializeAudio) {
        std::string iniAPath = JoinPath(song.cacheDir, prefix + song.id + ".ini");
        std::string iniSrc = JoinPath(song.songDir, charts[0].file); // ini's Level comes from chart[0]
        if (cacheIsFresh(iniAPath, iniSrc)) {
            LogLine("[cache] playable cache cached id='%s' files=%d", song.id.c_str(), emitted);
            return; // warm boot: encrypted inis already written and still fresh
        }
        std::string ini = BuildSongIni(song, charts[0]);
        std::string iniA = JoinPath(song.cacheDir, prefix + song.id + ".ini");
        std::string iniB = JoinPath(song.cacheDir, song.id + ".ini");
        // Use runtime key (reads actual game binary at 0x0a8568) instead of hardcoded table.
        bool okIniA = writeEncryptedTextFileRuntime(iniA, ini, 0x0a8568);
        bool okIniB = writeEncryptedTextFileRuntime(iniB, ini, 0x0a8568);
        writeTextFile(iniA + ".plain.txt", ini);
        writeTextFile(iniB + ".plain.txt", ini);
        LogLine("[cache] ini runtime-key encrypted id='%s' files='%s','%s' keyRva=0x0a8568 ok=%d/%d",
                song.id.c_str(), fileNameOnlyLocal(iniA).c_str(), fileNameOnlyLocal(iniB).c_str(), okIniA ? 1 : 0, okIniB ? 1 : 0);
    }
    LogLine("[cache] playable cache %s id='%s' files=%d cache='%s'",
            materializeAudio ? "audio-materialized" : "emitted", song.id.c_str(), emitted, song.cacheDir.c_str());
}


// ---- Direct BMS gameplay bridge -----------------------------------------------------
// The existing converter already produces the exact 13-byte FNEX event records. Keep that
// validated conversion in memory and commit its packed 13-byte records through the same native
// functions used by BMSBridge v0.2.9. No chart/audio
// artifacts touch disk.
#pragma pack(push, 1)
struct DirectRawEvent {
    uint32_t tick;
    uint8_t type;
    uint8_t payload[8];
};
static_assert(sizeof(DirectRawEvent) == 13, "FNEX direct event ABI changed");
#pragma pack(pop)

#pragma pack(push, 1)
struct DirectWaveFormat {
    uint16_t formatTag, channels;
    uint32_t sampleRate, averageBytes;
    uint16_t blockAlign, bits, extraSize;
};
#pragma pack(pop)

static uint16_t directU16(const unsigned char* p) {
    uint16_t v; memcpy(&v, p, sizeof(v)); return v;
}
static uint32_t directU32(const unsigned char* p) {
    uint32_t v; memcpy(&v, p, sizeof(v)); return v;
}
static float directFloat(const unsigned char* p) {
    float v; memcpy(&v, p, sizeof(v)); return v;
}

static bool decodeDirectEz(const std::vector<unsigned char>& ez,
                           std::vector<DirectRawEvent> (&tracks)[64],
                           uint32_t (&trackMax)[64], float& bpm, float& seconds, uint32_t& resolution) {
    if (ez.size() < 0x96 || memcmp(ez.data(), "EZFF", 4) != 0 || directU16(ez.data() + 0x8c) != 64)
        return false;
    // Ticks per measure travels with the chart rather than being assumed, so charts cached under an
    // older grid keep playing correctly instead of running at the wrong speed.
    resolution = directU16(ez.data() + 0x86);
    if (resolution == 0) resolution = kNativeTicksPerMeasure;
    bpm = directFloat(ez.data() + 0x88);
    seconds = directFloat(ez.data() + 0x92);
    size_t off = 0x96;
    for (int block = 0; block < 64; ++block) {
        if (off + 0x4e > ez.size() || memcmp(ez.data() + off, "EZTR", 4) != 0) return false;
        int track = directU16(ez.data() + off + 4);
        uint32_t bytes = directU32(ez.data() + off + 0x4a);
        trackMax[track >= 0 && track < 64 ? track : 0] = directU32(ez.data() + off + 0x46);
        off += 0x4e;
        if (track < 0 || track >= 64 || bytes % 13 != 0 || off + bytes > ez.size()) return false;
        uint32_t count = bytes / 13;
        // ponytail: the native per-track processed buffer is 0x14000 bytes = 2048 events.
        if (count > 2048) return false;
        tracks[track].reserve(count);
        for (uint32_t i = 0; i < count; ++i, off += 13) {
            DirectRawEvent event{};
            event.tick = directU32(ez.data() + off);
            event.type = ez[off + 4];
            memcpy(event.payload, ez.data() + off + 5, sizeof(event.payload));
            tracks[track].push_back(event);
        }
    }
    return off == ez.size();
}

static bool parseDirectPcmWav(const std::string& path, std::vector<unsigned char>& file,
                              DirectWaveFormat& format, const unsigned char*& pcm, uint32_t& pcmBytes) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 44 || size > 128 * 1024 * 1024) { fclose(f); return false; }
    file.resize((size_t)size);
    bool read = fread(file.data(), 1, file.size(), f) == file.size();
    fclose(f);
    if (!read || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0)
        return false;

    bool haveFormat = false;
    pcm = nullptr;
    pcmBytes = 0;
    for (size_t pos = 12; pos + 8 <= file.size();) {
        uint32_t id = directU32(file.data() + pos);
        uint32_t len = directU32(file.data() + pos + 4);
        pos += 8;
        if (pos + len > file.size()) return false;
        if (id == 0x20746d66 && len >= 16) { // "fmt "
            memset(&format, 0, sizeof(format));
            format.formatTag   = directU16(file.data() + pos);
            format.channels    = directU16(file.data() + pos + 2);
            format.sampleRate  = directU32(file.data() + pos + 4);
            format.averageBytes= directU32(file.data() + pos + 8);
            format.blockAlign  = directU16(file.data() + pos + 12);
            format.bits        = directU16(file.data() + pos + 14);
            format.extraSize   = len >= 18 ? directU16(file.data() + pos + 16) : 0;
            haveFormat = true;
        } else if (id == 0x61746164) { // "data"
            pcm = file.data() + pos;
            pcmBytes = len;
        }
        pos += (len + 1u) & ~1u;
    }
    return haveFormat && pcm && pcmBytes && format.channels && format.sampleRate && format.blockAlign &&
           (format.formatTag == 1 || format.formatTag == 3);
}

static void* createDirectSample(void* audio, const std::string& path, DirectWaveFormat format,
                                const unsigned char* pcm, uint32_t pcmBytes) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    using CreateSample = void* (__thiscall*)(void*, int, void*, int);
    using RegisterSample = int (__thiscall*)(void*, void*);
    auto create = reinterpret_cast<CreateSample>(base + 0x0e910);
    auto reg = reinterpret_cast<RegisterSample>(base + 0x0dea0);
    void* sample = create(audio, (int)pcmBytes, &format, 0);
    if (!sample) return nullptr;
    void* buffer = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(sample) + 184);
    if (!buffer) return nullptr;
    void** vtable = *reinterpret_cast<void***>(buffer);
    using Lock = HRESULT (__stdcall*)(void*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD);
    using Unlock = HRESULT (__stdcall*)(void*, void*, DWORD, void*, DWORD);
    auto lock = reinterpret_cast<Lock>(vtable[11]);
    auto unlock = reinterpret_cast<Unlock>(vtable[19]);
    void *one = nullptr, *two = nullptr;
    DWORD oneBytes = 0, twoBytes = 0;
    if (FAILED(lock(buffer, 0, pcmBytes, &one, &oneBytes, &two, &twoBytes, 0))) return nullptr;
    uint32_t copied = 0;
    if (one && oneBytes) {
        uint32_t n = std::min<uint32_t>(oneBytes, pcmBytes);
        memcpy(one, pcm, n); copied = n;
    }
    if (two && twoBytes && copied < pcmBytes) {
        uint32_t n = std::min<uint32_t>(twoBytes, pcmBytes - copied);
        memcpy(two, pcm + copied, n); copied += n;
    }
    unlock(buffer, one, oneBytes, two, twoBytes);
    if (copied != pcmBytes || !reg(reinterpret_cast<unsigned char*>(audio) + 84, sample)) return nullptr;
    lstrcpynA(reinterpret_cast<char*>(sample) + 38, path.c_str(), 128);
    return sample;
}

static void* loadDirectPcm(void* audio, const std::string& path) {
    std::vector<unsigned char> file;
    DirectWaveFormat format{};
    const unsigned char* pcm = nullptr;
    uint32_t pcmBytes = 0;
    if (!parseDirectPcmWav(path, file, format, pcm, pcmBytes)) return nullptr;
    return createDirectSample(audio, path, format, pcm, pcmBytes);
}

static const CustomBmsChart* selectDirectChart(const CustomBmsSong& song, const char* request) {
    std::string low = lowerCopy(request ? request : "");
    int slot = low.find("-shd") != std::string::npos ? 2
             : low.find("-hd")  != std::string::npos ? 1
             : low.find("-ex")  != std::string::npos ? 3
             : low.find("-ez")  != std::string::npos ? 4 : 0;
    for (const auto& chart : song.chartMeta) if (chart.slot == slot) return &chart;
    for (const auto& chart : song.chartMeta) if (chart.slot >= 0 && chart.slot < kChartSlots) return &chart;
    return nullptr;
}

static std::string resolveDirectVideoAsset(const CustomBmsSong& song, const std::string& relIn) {
    std::string rel = normalizeRelPath(stripQuotes(trim(relIn)));
    if (rel.empty()) return std::string();
    bool absolute = (rel.size() > 1 && rel[1] == ':') || rel.front() == '\\' || rel.front() == '/';
    std::string path = absolute ? rel : JoinPath(song.songDir, rel);
    if (endsWithVideo(path) && fileExistsLocal(path)) return path;

    std::string found;
    if (recursiveFindAudioByName(song.songDir, fileNameOnlyLocal(rel), false, 0, found) &&
        endsWithVideo(found)) return found;
    if (!hasFileExtensionLocal(rel) &&
        recursiveFindAudioByName(song.songDir, fileNameOnlyLocal(rel), true, 0, found) &&
        endsWithVideo(found)) return found;
    return std::string();
}

static bool findFirstDirectVideo(const std::string& dir, int depth, std::string& found) {
    if (depth > 3) return false;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(JoinPath(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string path = JoinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = findFirstDirectVideo(path, depth + 1, found);
        else if (endsWithVideo(path)) { found = path; ok = true; }
    } while (!ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static bool selectRandomBgaPoolVideo(std::string& selected) {
    static std::vector<std::string> pool;
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
        std::string dir = JoinPath(JoinPath(GetExeDirectory(), "Video"), "_spv");
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(JoinPath(dir, "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && endsWithVideo(fd.cFileName))
                    pool.push_back(JoinPath(dir, fd.cFileName));
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        LogLine("[bga-direct] random pool indexed files=%lu dir='%s'",
                (unsigned long)pool.size(), dir.c_str());
    }
    if (pool.empty()) return false;
    selected = pool[(size_t)rand() % pool.size()];
    LogLine("[bga-direct] random fallback source='%s'", selected.c_str());
    return true;
}

static uint32_t directTickToMilliseconds(uint32_t targetTick, float baseBpm,
                                         const std::vector<DirectRawEvent>& timing) {
    if (!targetTick) return 0;
    double bpm = baseBpm > 0.0f ? baseBpm : 120.0;
    double ms = 0.0;
    uint32_t lastTick = 0;
    for (const auto& event : timing) {
        if (event.tick > targetTick) break;
        if (event.tick > lastTick) {
            ms += (double)(event.tick - lastTick) * 60000.0 / (bpm * 48.0);
            lastTick = event.tick;
        }
        if (event.type == 3) {
            float next = directFloat(event.payload);
            if (next > 0.0f) bpm = next;
        }
    }
    if (targetTick > lastTick)
        ms += (double)(targetTick - lastTick) * 60000.0 / (bpm * 48.0);
    return ms >= 4294967295.0 ? 0xffffffffu : (uint32_t)(ms + 0.5);
}

static void selectDirectBmsVideo(const CustomBmsSong& song, const ParsedBmsForEz& chart,
                                 const std::map<int, std::string>& bmpDefinitions,
                                 const std::string& explicitVideo, float bpm,
                                 const std::vector<DirectRawEvent>& timing) {
    std::string selected;
    uint32_t selectedTick = 0xffffffffu;
    int selectedLayer = 3;
    for (const auto& event : chart.events) {
        int layer = event.channel == 0x04 ? 0 : event.channel == 0x07 ? 1 :
                    event.channel == 0x06 ? 2 : -1;
        if (layer < 0) continue;
        auto def = bmpDefinitions.find(event.wavId);
        if (def == bmpDefinitions.end()) continue;
        std::string path = resolveDirectVideoAsset(song, def->second);
        if (path.empty()) continue;
        uint32_t tick = bmsTickToEzTick(chart, event.measure, event.index, event.div);
        if (tick < selectedTick || (tick == selectedTick && layer < selectedLayer)) {
            selected = path;
            selectedTick = tick;
            selectedLayer = layer;
        }
    }
    if (selected.empty()) selected = resolveDirectVideoAsset(song, explicitVideo);
    if (selected.empty()) {
        for (const auto& rel : song.videoFiles) {
            selected = resolveDirectVideoAsset(song, rel);
            if (!selected.empty()) break;
        }
    }
    if (selected.empty()) findFirstDirectVideo(song.songDir, 0, selected);
    g_directBmsVideoFallback = selected.empty() && selectRandomBgaPoolVideo(selected);
    if (selected.empty()) {
        LogLine("[bga-direct] no playable video id='%s'", song.id.c_str());
        return;
    }
    if (selectedTick == 0xffffffffu) selectedTick = 0;
    g_directBmsVideoPath = selected;
    g_directBmsVideoStartMs = directTickToMilliseconds(selectedTick, bpm, timing);
    LogLine("[bga-direct] selected id='%s' source='%s' startTick=%lu startMs=%lu layer=%d backend=DirectShow",
            song.id.c_str(), selected.c_str(), (unsigned long)selectedTick,
            (unsigned long)g_directBmsVideoStartMs, selectedLayer == 3 ? 0 : selectedLayer);
}

// ---- Keysound prefetch ----------------------------------------------------------------------
// Decoded PCM for the song the cursor is resting on, so that confirming it costs a memcpy per
// keysound instead of a decode. A background thread fills it (KeysoundPrefetchRequest, from song
// select once the cursor has settled), LoadCustomBmsDirect drains it, and a request for a
// different song throws it away. Bounded: past the budget the rest is decoded at load time.
struct DecodedPcm {
    std::vector<unsigned char> pcm;
    uint32_t channels = 0, rate = 0, bits = 0;
};
static CRITICAL_SECTION g_pcmCacheLock;
static bool g_pcmCacheLockReady = false;
static std::string g_pcmCacheSongId;
static std::map<std::string, DecodedPcm> g_pcmCache;    // by source path, lowercased
static size_t g_pcmCacheBytes = 0;
static const size_t kPcmCacheBudget = 384u << 20;
static HANDLE g_prefetchThread = nullptr;
static volatile LONG g_prefetchGeneration = 0;          // bumped to stop the running thread
static volatile LONG g_prefetchCancel = 0;              // polled inside the decoders, per chunk
static volatile LONG g_prefetchBusy = 0;                // one thread in flight, ever
static std::string g_prefetchPending;                   // the song asked for while one was busy

static void pcmCacheInit() {
    if (g_pcmCacheLockReady) return;
    InitializeCriticalSection(&g_pcmCacheLock);
    g_pcmCacheLockReady = true;
}

static bool isNativeSamplePath(const std::string& p) {
    return endsWith(p, ".wav") || endsWith(p, ".ssf") || endsWith(p, ".ezw");
}

struct PrefetchJob {
    std::string id, songDir;
    std::vector<std::string> charts;
    LONG generation;
};

static DWORD WINAPI keysoundPrefetchProc(void* arg) {
    std::unique_ptr<PrefetchJob> job(static_cast<PrefetchJob*>(arg));
    const DWORD t0 = GetTickCount();
    // Every compressed source any chart of the song names; the selected difficulty is not known
    // yet and the charts of one song share nearly all of their keysounds.
    std::vector<std::string> sources;
    std::set<std::string> seen;
    for (const auto& chart : job->charts) {
        ParsedBmsForEz parsed;
        if (!parseBmsForEz(JoinPath(job->songDir, chart), parsed)) continue;
        for (const auto& wav : parsed.wavMap) {
            std::string source, unused;
            if (!resolveBmsAudioAsset(job->songDir, wav.second, source, unused)) continue;
            if (isNativeSamplePath(source)) continue;
            if (seen.insert(lowerPathLocal(source)).second) sources.push_back(source);
        }
    }
    size_t files = 0, bytes = 0;
    bool full = false, stopped = false;
    for (const auto& src : sources) {
        if (InterlockedCompareExchange(&g_prefetchGeneration, 0, 0) != job->generation) { stopped = true; break; }
        DecodedPcm d;
        // In-process decodes poll the cancel flag per chunk; anything else (Media Foundation,
        // ffmpeg) runs to the end of the file, which is the old behaviour and rare.
        if (!DecodeAudioInProcess(src, 0, d.pcm, d.channels, d.rate, d.bits, &g_prefetchCancel)) {
            if (InterlockedCompareExchange(&g_prefetchCancel, 0, 0)) { stopped = true; break; }
            if (!mediaFoundationDecodePcmLocal(src, 0, d.pcm, d.channels, d.rate, d.bits)) continue;
        }
        EnterCriticalSection(&g_pcmCacheLock);
        if (g_pcmCacheSongId == job->id) {
            if (g_pcmCacheBytes + d.pcm.size() <= kPcmCacheBudget) {
                g_pcmCacheBytes += d.pcm.size();
                bytes += d.pcm.size();
                ++files;
                g_pcmCache[lowerPathLocal(src)] = std::move(d);
            } else full = true;
        } else stopped = true;
        LeaveCriticalSection(&g_pcmCacheLock);
        if (full || stopped) break;
    }
    if (files || !sources.empty())
        LogLine("[prefetch] id='%s' %lu/%lu compressed keysounds, %lu KB in %lu ms%s%s",
                job->id.c_str(), (unsigned long)files, (unsigned long)sources.size(),
                (unsigned long)(bytes >> 10), (unsigned long)(GetTickCount() - t0),
                full ? " (budget full)" : "", stopped ? " (cursor moved)" : "");
    InterlockedExchange(&g_prefetchBusy, 0);
    return 0;
}

static void startPrefetchThread(const CustomBmsSong& song) {
    const LONG generation = InterlockedIncrement(&g_prefetchGeneration);
    InterlockedExchange(&g_prefetchCancel, 0);
    if (g_prefetchThread) { CloseHandle(g_prefetchThread); g_prefetchThread = nullptr; }
    EnterCriticalSection(&g_pcmCacheLock);
    g_pcmCache.clear();
    g_pcmCacheBytes = 0;
    g_pcmCacheSongId = song.id;
    LeaveCriticalSection(&g_pcmCacheLock);
    std::unique_ptr<PrefetchJob> job(new PrefetchJob{song.id, song.songDir, song.charts, generation});
    InterlockedExchange(&g_prefetchBusy, 1);
    // Below normal: the game thread must always win the CPU over a decode that only saves time later.
    g_prefetchThread = CreateThread(nullptr, 0, keysoundPrefetchProc, job.get(), CREATE_SUSPENDED, nullptr);
    if (!g_prefetchThread) { InterlockedExchange(&g_prefetchBusy, 0); return; }
    SetThreadPriority(g_prefetchThread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(g_prefetchThread);
    job.release();
}

// Song select calls this every frame once the cursor has rested on a custom song. One decode
// thread exists at a time: a request for a different song cancels the running one (the decoders
// poll the flag per chunk, so it exits within milliseconds) and the new song starts once it has.
// Several threads racing the game for the CPU was what made the wheel crawl.
void KeysoundPrefetchRequest(const char* id) {
    if (!id || !*id || !DirectBmsEnabled()) return;
    pcmCacheInit();
    static std::string s_requested;
    if (s_requested != id) {
        s_requested = id;
        g_prefetchPending = id;
        if (InterlockedCompareExchange(&g_prefetchBusy, 0, 0)) {
            InterlockedIncrement(&g_prefetchGeneration);
            InterlockedExchange(&g_prefetchCancel, 1);
        }
    }
    if (g_prefetchPending.empty() || InterlockedCompareExchange(&g_prefetchBusy, 0, 0)) return;
    const CustomBmsSong* song = FindCustomSongById(g_prefetchPending.c_str());
    g_prefetchPending.clear();
    if (song) startPrefetchThread(*song);
}

// What a load takes out of the cache: everything decoded for this song, or nothing.
static std::map<std::string, DecodedPcm> takePrefetchedPcm(const std::string& id) {
    std::map<std::string, DecodedPcm> out;
    if (!g_pcmCacheLockReady) return out;
    g_prefetchPending.clear();
    EnterCriticalSection(&g_pcmCacheLock);
    const bool ours = g_pcmCacheSongId == id;
    LeaveCriticalSection(&g_pcmCacheLock);
    // Our own song mid-decode is worth waiting for (at normal priority now); any other is cancelled.
    if (g_prefetchThread) {
        if (ours) SetThreadPriority(g_prefetchThread, THREAD_PRIORITY_NORMAL);
        else { InterlockedIncrement(&g_prefetchGeneration); InterlockedExchange(&g_prefetchCancel, 1); }
        WaitForSingleObject(g_prefetchThread, ours ? 20000 : 2000);
        CloseHandle(g_prefetchThread);
        g_prefetchThread = nullptr;
    }
    EnterCriticalSection(&g_pcmCacheLock);
    if (g_pcmCacheSongId == id) out.swap(g_pcmCache);
    g_pcmCache.clear();
    g_pcmCacheBytes = 0;
    g_pcmCacheSongId.clear();
    LeaveCriticalSection(&g_pcmCacheLock);
    return out;
}

// A keysound the game's own loader could not take: decoded by us, then handed to it as PCM.
struct DeferredSample {
    int id = 0;
    std::string source;
    DecodedPcm out;
    bool ok = false;
};
struct DeferredQueue {
    std::vector<DeferredSample>* items;
    volatile LONG next;
};

static DWORD WINAPI deferredDecodeWorker(void* arg) {
    DeferredQueue* q = static_cast<DeferredQueue*>(arg);
    for (;;) {
        const LONG i = InterlockedIncrement(&q->next) - 1;
        if (i < 0 || (size_t)i >= q->items->size()) break;
        DeferredSample& d = (*q->items)[(size_t)i];
        if (d.ok) continue;
        d.ok = mediaFoundationDecodePcmLocal(d.source, 0, d.out.pcm, d.out.channels, d.out.rate, d.out.bits);
    }
    return 0;
}

// Decode the not-yet-decoded entries on up to four threads (one per core, at most). The game
// thread joins them; nothing here touches the game.
static unsigned decodeDeferredParallel(std::vector<DeferredSample>& items) {
    size_t todo = 0;
    for (const auto& d : items) if (!d.ok) ++todo;
    if (!todo) return 0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned threads = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    if (threads > 4) threads = 4;
    if (threads > todo) threads = (unsigned)todo;
    DeferredQueue q{&items, 0};
    if (threads <= 1) { deferredDecodeWorker(&q); return 1; }
    HANDLE h[4] = {};
    unsigned started = 0;
    for (unsigned t = 0; t < threads; ++t) {
        h[started] = CreateThread(nullptr, 0, deferredDecodeWorker, &q, 0, nullptr);
        if (h[started]) ++started;
    }
    if (!started) { deferredDecodeWorker(&q); return 1; }
    WaitForMultipleObjects(started, h, TRUE, INFINITE);
    for (unsigned t = 0; t < started; ++t) CloseHandle(h[t]);
    return started;
}

static void* createDirectSampleFromPcm(void* audio, const std::string& source, const DecodedPcm& d) {
    if (d.pcm.empty() || !d.channels || !d.rate || !d.bits) return nullptr;
    DirectWaveFormat format{};
    format.formatTag = 1;
    format.channels = (uint16_t)d.channels;
    format.sampleRate = d.rate;
    format.bits = (uint16_t)d.bits;
    format.blockAlign = (uint16_t)(d.channels * d.bits / 8);
    format.averageBytes = d.rate * format.blockAlign;
    return createDirectSample(audio, source, format, d.pcm.data(), (uint32_t)d.pcm.size());
}

bool LoadCustomBmsDirect(void* songPackage, const char* request) {
    if (!songPackage || !request) return false;
    const CustomBmsSong* song = nullptr;
    std::string lowRequest = lowerCopy(request);
    for (const auto& candidate : g_songs)
        if (lowRequest.find(lowerCopy(candidate.id)) != std::string::npos) { song = &candidate; break; }
    if (!song) return false;
    const DWORD loadStart = GetTickCount();
    ResetDirectBmsVideo();
    const CustomBmsChart* chart = selectDirectChart(*song, request);
    if (!chart) { LogLine("[direct] no playable chart id='%s'", song->id.c_str()); return false; }

    std::string chartPath = JoinPath(song->songDir, chart->file);
    g_curModeSevenKey = modeIsSevenKey(song->mode);
    g_curGenerateMode = song->mode;
    g_curAutoGenerate = song->autoGenerated;
    g_curAutoEnrich = !song->autoGenerated && Config().autoGenerateEnrichSource;
    std::vector<unsigned char> ez;
    std::string report;
    if (!generateEzFromBms(chartPath, std::string(), song->id, &report, &ez)) {
        LogLine("[direct] parse failed id='%s' chart='%s'", song->id.c_str(), chartPath.c_str());
        return false;
    }

    std::vector<DirectRawEvent> tracks[64];
    uint32_t trackMax[64]{};
    float bpm = 120.0f, seconds = 0.0f;
    uint32_t resolution = kNativeTicksPerMeasure;
    if (!decodeDirectEz(ez, tracks, trackMax, bpm, seconds, resolution)) {
        LogLine("[direct] EZ-memory selfcheck failed id='%s'", song->id.c_str());
        return false;
    }

    std::map<int, std::string> wavDefinitions;
    std::map<int, std::string> bmpDefinitions;
    std::string explicitVideo;
    for (const auto& sibling : song->chartMeta) {
        ParsedBmsForEz parsed;
        if (!parseBmsForEz(JoinPath(song->songDir, sibling.file), parsed)) continue;
        for (const auto& wav : parsed.wavMap)
            if (!wavDefinitions.count(wav.first)) wavDefinitions[wav.first] = wav.second;
        for (const auto& bmp : parsed.bmpMap)
            if (!bmpDefinitions.count(bmp.first)) bmpDefinitions[bmp.first] = bmp.second;
        if (explicitVideo.empty() && !parsed.directVideo.empty()) explicitVideo = parsed.directVideo;
    }
    ParsedBmsForEz selected;
    if (!parseBmsForEz(chartPath, selected)) {
        LogLine("[direct] asset parse failed id='%s'", song->id.c_str());
        return false;
    }
    for (const auto& wav : selected.wavMap) wavDefinitions[wav.first] = wav.second;
    for (const auto& bmp : selected.bmpMap) bmpDefinitions[bmp.first] = bmp.second;
    if (!selected.directVideo.empty()) explicitVideo = selected.directVideo;
    selectDirectBmsVideo(*song, selected, bmpDefinitions, explicitVideo, bpm, tracks[0]);

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    using ClearSamples = void (__thiscall*)(void*);
    using LoadSample = void* (__thiscall*)(void*, const char*, int, int);
    using ConvertEvents = void (__cdecl*)(DirectRawEvent*, void*, int, int);
    using Finalize = void (__thiscall*)(void*, int, int);
    auto clearSamples = reinterpret_cast<ClearSamples>(base + 0x11050);
    auto loadSample = reinterpret_cast<LoadSample>(base + 0x0e3c0);
    auto convert = reinterpret_cast<ConvertEvents>(base + 0x10fd0);
    auto finalize = reinterpret_cast<Finalize>(base + 0x11310);

    clearSamples(songPackage);
    void* audio = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(songPackage) + 4);
    if (!audio) { LogLine("[direct] audio manager missing id='%s'", song->id.c_str()); return false; }

    bool referenced[0x800]{};
    for (const auto& lane : tracks) for (const auto& event : lane) if (event.type == 1) {
        int id = event.payload[0] | (event.payload[1] << 8);
        if (id > 0 && id < 0x800) referenced[id] = true;
    }
    int loaded = 0, missing = 0, unsupported = 0;
    // The keysounds the game's loader cannot read (ogg, mp3, odd wavs) are gathered and decoded
    // together -- from the hover prefetch when it has them, on several threads otherwise --
    // instead of one at a time in this loop. Sample creation itself stays on this thread.
    std::vector<DeferredSample> deferred;
    for (int id = 1; id < 0x800; ++id) {
        if (!referenced[id]) continue;
        auto definition = wavDefinitions.find(id);
        if (definition == wavDefinitions.end()) { ++missing; continue; }
        std::string source, unused;
        if (!resolveBmsAudioAsset(song->songDir, definition->second, source, unused)) { ++missing; continue; }
        void* sample = nullptr;
        if (endsWith(source, ".wav") || endsWith(source, ".ssf")) {
            std::string nativePath = source;
            std::string root = GetExeDirectory();
            std::string sourceLow = lowerPathLocal(source), rootLow = lowerPathLocal(root);
            if (sourceLow.size() > rootLow.size() && sourceLow.compare(0, rootLow.size(), rootLow) == 0 &&
                (sourceLow[rootLow.size()] == '\\' || sourceLow[rootLow.size()] == '/'))
                nativePath = source.substr(root.size() + 1);
            sample = loadSample(audio, nativePath.c_str(), 0, 0);
            if (!sample && nativePath != source) sample = loadSample(audio, source.c_str(), 0, 0);
        }
        if (!sample && endsWith(source, ".wav")) sample = loadDirectPcm(audio, source);
        if (sample) {
            *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(songPackage) + 0xc9d1c + id * 4) = sample;
            ++loaded;
            continue;
        }
        if (endsWith(source, ".ssf") || endsWith(source, ".ezw")) { ++unsupported; continue; }
        DeferredSample d;
        d.id = id;
        d.source = source;
        deferred.push_back(std::move(d));
    }
    size_t prefetched = 0;
    unsigned decodeThreads = 0;
    const DWORD decodeStart = GetTickCount();
    if (!deferred.empty()) {
        std::map<std::string, DecodedPcm> cache = takePrefetchedPcm(song->id);
        for (auto& d : deferred) {
            auto it = cache.find(lowerPathLocal(d.source));
            if (it == cache.end()) continue;
            d.out = std::move(it->second);
            d.ok = true;
            ++prefetched;
        }
        decodeThreads = decodeDeferredParallel(deferred);
        for (auto& d : deferred) {
            void* sample = d.ok ? createDirectSampleFromPcm(audio, d.source, d.out) : nullptr;
            if (!sample) { ++unsupported; continue; }
            *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(songPackage) + 0xc9d1c + d.id * 4) = sample;
            ++loaded;
        }
    }
    const DWORD decodeMs = GetTickCount() - decodeStart;

    unsigned char* object = reinterpret_cast<unsigned char*>(songPackage);
    memset(object + 8, 0, 0x1d1c - 8);
    memset(object + 0x1d1c, 0, 0x32000 * 4);
    lstrcpynA(reinterpret_cast<char*>(object + 8), selected.title.c_str(), 64);
    lstrcpynA(reinterpret_cast<char*>(object + 0x48), selected.artist.c_str(), 64);
    *reinterpret_cast<uint32_t*>(object + 0x88) = resolution;
    *reinterpret_cast<float*>(object + 0x8c) = bpm;
    *reinterpret_cast<float*>(object + 0x90) = seconds;
    *reinterpret_cast<uint32_t*>(object + 0x94) = 0;
    *reinterpret_cast<uint32_t*>(object + 0x98) = 64;

    for (int track = 0; track < 64; ++track) {
        unsigned char* meta = object + 0x9c + track * 0x4c;
        *reinterpret_cast<uint16_t*>(meta) = 0;
        *reinterpret_cast<uint32_t*>(meta + 0x44) = trackMax[track];
        *reinterpret_cast<uint32_t*>(meta + 0x48) = (uint32_t)tracks[track].size();
        uint32_t* index = reinterpret_cast<uint32_t*>(base + 0x99ee38 + track * 0x14008);
        index[0] = 0;
        index[1] = (uint32_t)tracks[track].size();
        if (!tracks[track].empty())
            convert(tracks[track].data(), reinterpret_cast<void*>(base + 0x98ae38 + track * 0x14008),
                    track, (int)tracks[track].size());
    }
    finalize(songPackage, 0, 64);
    LogLine("[direct] loaded id='%s' chart='%s' samples=%d missing=%d unsupported=%d %s",
            song->id.c_str(), chart->file.c_str(), loaded, missing, unsupported, report.c_str());
    LogLine("[direct] keysounds: %lu decoded here (%lu prefetched on hover, %u threads, %lu ms), load total %lu ms",
            (unsigned long)deferred.size(), (unsigned long)prefetched, decodeThreads,
            (unsigned long)decodeMs, (unsigned long)(GetTickCount() - loadStart));
    return true;
}



// ---- On-demand audio materialization -------------------------------------------------
// Huge BMS packs: decoding every keysound to PCM up-front made an 11GB cache + slow startup.
// The scan now writes only cheap metadata (.ez/.ezi/.ini/art); the heavy keysound .ssf bytes
// are decoded the first time the game opens a sample for a song, and the previously-played
// song's keysounds are evicted. Steady-state disk = current song's audio only.
// Hover previews accumulate (~0.5MB each); cap them if browsing thousands of songs bloats the cache.
// Per-song SPV generation remains only in the legacy disk fallback.
static std::string g_keysSongId, g_keysCacheDir;

static void DeleteSsfFiles(const std::string& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(JoinPath(dir, "*.ssf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do { DeleteFileA(JoinPath(dir, fd.cFileName).c_str()); ++n; } while (FindNextFileA(h, &fd));
    FindClose(h);
    LogLine("[cache] evicted %d ssf from '%s'", n, dir.c_str());
}

// Decode this song's keysounds now (first sample miss = the song is being played) and evict
// the previous song's. Idempotent within a session via g_keysSongId.
void EnsureSongKeysounds(const CustomBmsSong& song) {
    if (song.cacheDir.empty() || g_keysSongId == song.id) return;
    if (!g_keysCacheDir.empty() && g_keysCacheDir != song.cacheDir) DeleteSsfFiles(g_keysCacheDir);
    GeneratePlayableCacheV10(song, /*materializeAudio=*/true);
    g_keysSongId = song.id; g_keysCacheDir = song.cacheDir;
}

// Build the SongSelect hover preview on demand: decode just the largest source audio, trimmed.
void EnsureSongPreview(const CustomBmsSong& song) {
    if (song.cacheDir.empty()) return;
    ensureDir(JoinPath(GetExeDirectory(), Config().cacheRoot));
    ensureDir(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), song.mode));
    ensureDir(song.cacheDir);
    std::string dst = JoinPath(song.cacheDir, song.id + ".ssf");
    if (fileExistsLocal(dst)) return;
    std::string bestSrc; LONGLONG best = 0;
    auto consider = [&](const std::string& p) {
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &ad)) return;
        LONGLONG sz = ((LONGLONG)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
        if (sz > best) { best = sz; bestSrc = p; }
    };
    for (const auto& a : song.audioFiles) consider(JoinPath(song.songDir, a));
    // DJMax/O2Jam-style songs keep their audio in a sound\ subfolder (not the song root, so
    // song.audioFiles is empty). Their "MR" file (full BGM, e.g. *_mr.wav) is the biggest one there —
    // scan sound\ and pick the largest, which is the preview source. Keysound-only songs with NO big
    // BGM still fall through to "no source audio" (would need a bms2wav renderer — separate).
    for (const char* sub : {"sound", "Sound", "SOUND"}) {
        std::string dir = JoinPath(song.songDir, sub);
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(JoinPath(dir, "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (endsWithAudio(fd.cFileName)) consider(JoinPath(dir, fd.cFileName));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        break; // first existing sound-dir wins
    }
    if (bestSrc.empty()) { LogLine("[cache] preview id='%s' no source audio", song.id.c_str()); return; }
    std::string tmp = dst + ".tmp";
    bool ok = mediaFoundationToSsfLocal(bestSrc, tmp, 20); // decode only ~20s — preview, not the whole BGM
    if (ok && !MakeRandomPreviewSsf(tmp, dst, 12)) ok = copyFileSimple(tmp, dst);
    DeleteFileA(tmp.c_str());
    LogLine("[cache] preview id='%s' src='%s' ok=%d", song.id.c_str(), bestSrc.c_str(), ok ? 1 : 0);
}

const CustomBmsSong* FindCustomSongByCacheDir(const std::string& cacheDir) {
    for (const auto& s : g_songs) if (s.cacheDir == cacheDir) return &s;
    return nullptr;
}

static void ensureCacheSubdir(const std::string& cacheDir, const char* sub) {
    ensureDir(JoinPath(cacheDir, sub));
}

static std::wstring widePathLocal(const std::string& path) {
    int n = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &out[0], n);
    return out;
}

// GDI+ for callers outside the asset pipeline, whose own token lives in a function static. One
// token for the life of the process, same as there.
static bool EnsureGdiplusStarted() {
    static ULONG_PTR s_token = 0;
    static int s_state = -1;
    if (s_state < 0) {
        Gdiplus::GdiplusStartupInput input;
        s_state = Gdiplus::GdiplusStartup(&s_token, &input, nullptr) == Gdiplus::Ok ? 1 : 0;
        if (!s_state) LogLine("[overlay] GDI+ startup failed; category overlays disabled");
    }
    return s_state == 1;
}

// Decode any image GDI+ can read into raw BGR24 at an exact size, the layout StretchDIBits wants.
// Used for the per-category bg.png / ico.png overlays, which are drawn into surfaces we own.
bool RenderImageToBgr(const std::string& path, int w, int h, std::vector<unsigned char>& out) {
    out.clear();
    if (w <= 0 || h <= 0) return false;
    if (!EnsureGdiplusStarted()) return false;

    Gdiplus::Image img(widePathLocal(path).c_str());
    if (img.GetLastStatus() != Gdiplus::Ok || img.GetWidth() == 0) return false;

    HDC screen = GetDC(NULL);
    HDC hdc = CreateCompatibleDC(screen);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = -h;   // top-down
    bih.biPlanes = 1; bih.biBitCount = 24; bih.biCompression = BI_RGB;
    BITMAPINFO bi{}; bi.bmiHeader = bih;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool ok = false;
    if (bmp) {
        HBITMAP oldb = (HBITMAP)SelectObject(hdc, bmp);
        RECT full = {0, 0, w, h};
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &full, black);
        DeleteObject(black);
        {
            Gdiplus::Graphics g(hdc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            ok = g.DrawImage(&img, Gdiplus::Rect(0, 0, w, h),
                             0, 0, (INT)img.GetWidth(), (INT)img.GetHeight(),
                             Gdiplus::UnitPixel) == Gdiplus::Ok;
        }
        if (ok) {
            const int stride = ((w * 3 + 3) & ~3);
            out.assign((size_t)stride * h, 0);
            BITMAPINFOHEADER bu = bih; bu.biHeight = h; bu.biSizeImage = (DWORD)out.size();
            ok = GetDIBits(hdc, bmp, 0, h, out.data(), (BITMAPINFO*)&bu, DIB_RGB_COLORS) != 0;
            // GetDIBits with a positive height hands rows back bottom-up; flip to top-down so the
            // blit matches the negative-height BITMAPINFO it will be given.
            if (ok) {
                std::vector<unsigned char> flipped(out.size());
                for (int y = 0; y < h; ++y)
                    memcpy(&flipped[(size_t)y * stride], &out[(size_t)(h - 1 - y) * stride], stride);
                out.swap(flipped);
            }
        }
        SelectObject(hdc, oldb);
        DeleteObject(bmp);
    }
    DeleteDC(hdc);
    ReleaseDC(NULL, screen);
    if (!ok) out.clear();
    return ok;
}

// BMSData\<modeFolder>\<category>\bg.png (or ico.png), when the folder has one.
std::string CategoryOverlayPath(const std::string& mode, const std::string& category,
                                const char* which) {
    for (const auto& s : g_songs) {
        if (_stricmp(s.mode.c_str(), mode.c_str()) != 0) continue;
        if (_stricmp(s.category.c_str(), category.c_str()) != 0) continue;
        std::string dir = s.songDir;                       // ...\<category>\<song>
        size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) return std::string();
        dir.erase(slash);                                  // ...\<category>
        std::string p = JoinPath(dir, std::string(which) + ".png");
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return std::string();
        fclose(f);
        return p;
    }
    return std::string();
}


// Generate the required BMP inside the DLL using native GDI/GDI+.
static bool GenerateBmpFile(const std::string& path, int w, int h,
                             COLORREF bgColor, COLORREF fgColor,
                             const std::string& line1, const std::string& line2,
                             Gdiplus::Image* artwork = nullptr, bool styledText = false,
                             bool circularArtwork = false, int outputBpp = 24) {
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return false;
    {
        HDC hdc = CreateCompatibleDC(hdcScreen);
        if (!hdc) { ReleaseDC(NULL, hdcScreen); return false; }

        BITMAPINFOHEADER bih{};
        bih.biSize        = sizeof(bih);
        bih.biWidth       = w;
        bih.biHeight      = -h; // top-down for GDI drawing
        bih.biPlanes      = 1;
        bih.biBitCount    = (WORD)outputBpp;
        bih.biCompression = BI_RGB;
        bih.biXPelsPerMeter = 2835;
        bih.biYPelsPerMeter = 2835;
        BITMAPINFO bi{};
        bi.bmiHeader = bih;

        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!hBmp) { DeleteDC(hdc); ReleaseDC(NULL, hdcScreen); return false; }

        HBITMAP hOld = (HBITMAP)SelectObject(hdc, hBmp);

        HBRUSH hBrush = CreateSolidBrush(bgColor);
        RECT rc = {0, 0, w, h};
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);

        bool imageOk = true;
        if (artwork) {
            const UINT sw = artwork->GetWidth(), sh = artwork->GetHeight();
            imageOk = sw > 0 && sh > 0;
            if (imageOk) {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                // ponytail: eyecatch is a 1024x512 atlas; the game only samples the
                // top-left 640x480 region, the rest must stay black. Detect by canvas
                // size and draw the art fit into that sub-rect instead of cover-cropping
                // the whole canvas. If another asset ever needs 1024x512 non-eyecatch,
                // promote this to an explicit mode flag.
                if (w == 1024 && h == 512) {
                    const int dw = 640, dh = 480;
                    imageOk = graphics.DrawImage(artwork, Gdiplus::Rect(0, 0, dw, dh),
                                                 0, 0, (INT)sw, (INT)sh,
                                                 Gdiplus::UnitPixel) == Gdiplus::Ok;
                } else {
                    UINT sx = 0, sy = 0, cw = sw, ch = sh;
                    if ((uint64_t)sw * (uint64_t)h > (uint64_t)sh * (uint64_t)w) {
                        cw = (UINT)((uint64_t)sh * (uint64_t)w / (uint64_t)h);
                        sx = (sw - cw) / 2;
                    } else {
                        ch = (UINT)((uint64_t)sw * (uint64_t)h / (uint64_t)w);
                        sy = (sh - ch) / 2;
                    }
                    if (circularArtwork) {
                        Gdiplus::GraphicsPath circle;
                        circle.AddEllipse(1, 1, w - 2, h - 2);
                        graphics.SetClip(&circle);
                    }
                    imageOk = graphics.DrawImage(artwork, Gdiplus::Rect(0, 0, w, h),
                                                 (INT)sx, (INT)sy, (INT)cw, (INT)ch,
                                                 Gdiplus::UnitPixel) == Gdiplus::Ok;
                    if (circularArtwork) {
                        graphics.ResetClip();
                        // white outer ring over the circle edge (simple border effect).
                        // pen width scales with canvas so 256/128px discs look consistent.
                        Gdiplus::REAL pw = (Gdiplus::REAL)(w >= 256 ? 6 : 3);
                        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        Gdiplus::Pen ring(Gdiplus::Color(255, 255, 255, 255), pw);
                        graphics.DrawEllipse(&ring, pw * 0.5f, pw * 0.5f, w - pw, h - pw);
                    }
                }
            }
        } else if (styledText) {
            // ponytail: two tiny native gradients are enough; no theme engine needed.
            for (int y = 0; y < h; ++y) {
                int blue = 24 + (40 * y / std::max(1, h - 1));
                HBRUSH row = CreateSolidBrush(RGB(3, 12 + blue / 4, blue));
                RECT rr = {0, y, w, y + 1};
                FillRect(hdc, &rr, row);
                DeleteObject(row);
            }
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(72, 220, 255));
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, 0, 0, nullptr); LineTo(hdc, w, 0);
            MoveToEx(hdc, 0, h - 1, nullptr); LineTo(hdc, w, h - 1);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        if (!line1.empty() || !line2.empty()) {
            SetBkMode(hdc, TRANSPARENT);
            // plain = songname/category style: left-aligned, no shadow, no lines/gradient.
            // styled = the fancy bar (gradient + cyan lines) used elsewhere.
            const bool plain = !styledText;
            // plain: 128-wide category bar = CENTERED (matches image_samples/category_45);
            // 256-wide songname/totalresult = RIGHT. styled = centered.
            const UINT hAlign = !plain ? DT_CENTER : (w <= 128 ? DT_CENTER : DT_RIGHT);
            const UINT align = hAlign | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            // songname/totalresult (plain, 256-wide) = BOLD: thin glyphs looked pixelated
            // in-game. Categories (plain, 128-wide) stay regular.
            const bool plainBold = plain && w >= 256;
            const int plainWeight = plainBold ? FW_BOLD : FW_NORMAL;
            auto drawLine = [&](const std::string& text, RECT r, int maxPx) {
                if (text.empty()) return;
                HFONT font = nullptr;
                HFONT oldFont = nullptr;
                for (int px = maxPx; px >= 8; --px) {
                    // plain (songname/category): regular weight + ClearType, matches the thin
                    // ~10px native glyphs. styled: bold + plain AA.
                    font = CreateFontA(-px, 0, 0, 0, plain ? plainWeight : FW_BOLD, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       plain ? ANTIALIASED_QUALITY : ANTIALIASED_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, "Arial");
                    oldFont = (HFONT)SelectObject(hdc, font);
                    SIZE size{};
                    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &size);
                    if (px <= 8 || (size.cx <= r.right - r.left && size.cy <= r.bottom - r.top)) break;
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                    font = nullptr;
                }
                // The loop used to fall out with no font when even 8px was too wide, and then
                // return -- drawing NOTHING. That is why long titles came up blank in the wheel
                // instead of merely small. 8px is the floor; DT_END_ELLIPSIS trims what is left.
                if (!font) return;
                if (!plain) {                          // drop shadow only in styled mode
                    RECT shadow = r; OffsetRect(&shadow, 1, 1);
                    SetTextColor(hdc, RGB(0, 0, 0));
                    DrawTextA(hdc, text.c_str(), -1, &shadow, align);
                }
                SetTextColor(hdc, fgColor);
                DrawTextA(hdc, text.c_str(), -1, &r, align);
                SelectObject(hdc, oldFont);
                DeleteObject(font);
            };
            // plain caps the glyph near the native ~10px (32px bar) / ~10px (16px bar);
            // styled keeps the larger bold size.
            // plain: category(16h)=11; songname/totalresult(32h)=14 −10% → 13 (bold+smaller,
            // less pixelated in-game). styled keeps the larger bold size.
            int maxPx = plain ? (h <= 16 ? 11 : 13) : (h <= 16 ? 12 : h <= 32 ? 21 : 26);

            // A song title too wide for one line gets two rather than being shrunk into
            // illegibility or, past the 8px floor, clipped to an ellipsis. The canvas already has
            // the room -- native songname art is 256x32 with a single 10px band at y 13..22, so a
            // second 13px line fits inside it without touching either edge.
            //
            // The decision is measured, not guessed from a character count: the DC and the font are
            // both right here, so ask GDI whether the string fits and split only when it does not.
            // The split point is the word boundary that leaves the two halves most even, which
            // keeps the second line from being a lone trailing word.
            std::string l1 = line1, l2 = line2;
            const int avail = (plain ? w - 2 : w - 3) - 3;
            if (l2.empty() && h >= 32 && !l1.empty()) {
                HFONT probe = CreateFontA(-maxPx, 0, 0, 0, plain ? plainWeight : FW_BOLD, FALSE,
                                          FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                          DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT prev = (HFONT)SelectObject(hdc, probe);
                SIZE whole{};
                GetTextExtentPoint32A(hdc, l1.c_str(), (int)l1.size(), &whole);
                if (whole.cx > avail) {
                    size_t best = std::string::npos;
                    size_t bestDelta = l1.size();
                    for (size_t i = 0; i < l1.size(); ++i) {
                        if (l1[i] != ' ') continue;
                        const size_t left = i, right = l1.size() - i - 1;
                        const size_t delta = left > right ? left - right : right - left;
                        if (delta < bestDelta) { bestDelta = delta; best = i; }
                    }
                    if (best != std::string::npos) {
                        l2 = l1.substr(best + 1);
                        l1 = l1.substr(0, best);
                    }
                }
                SelectObject(hdc, prev);
                DeleteObject(probe);
            }

            // plain right-aligned: full-width rect, small right margin.
            RECT r1 = {3, 1, plain ? w - 2 : w - 3, l2.empty() ? h - 1 : h / 2};
            RECT r2 = {3, h / 2, plain ? w - 2 : w - 3, h - 1};
            drawLine(l1, r1, maxPx);
            // Both lines get the same size when the split was ours: they are one title, and a
            // smaller second line reads as a subtitle. Only a caller-supplied line2 is stepped down.
            drawLine(l2, r2, line2.empty() ? maxPx : std::max(8, maxPx - 3));
        }

        // Retrieve bottom-up pixel data (standard BMP storage)
        int rowBytes = ((w * outputBpp / 8 + 3) & ~3);
        int pixelBytes = rowBytes * h;
        std::vector<unsigned char> pixels(pixelBytes, 0);
        BITMAPINFOHEADER bihBU = bih;
        bihBU.biHeight = h; // positive = bottom-up for GetDIBits
        bihBU.biSizeImage = (DWORD)pixelBytes;
        GetDIBits(hdc, hBmp, 0, (UINT)h, pixels.data(), (BITMAPINFO*)&bihBU, DIB_RGB_COLORS);

        SelectObject(hdc, hOld);
        DeleteObject(hBmp);
        DeleteDC(hdc);
        ReleaseDC(NULL, hdcScreen);

        BITMAPFILEHEADER bfh{};
        bfh.bfType    = 0x4D42;
        bfh.bfOffBits = (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
        bfh.bfSize    = bfh.bfOffBits + (DWORD)pixelBytes;

        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        fwrite(&bfh,  1, sizeof(bfh),  f);
        fwrite(&bihBU,1, sizeof(bihBU),f);
        fwrite(pixels.data(), 1, pixelBytes, f);
        fclose(f);
        return imageOk;
    }
}

// Category version-splash: replicate native System\SongSelect\Version\Version_NN.abm — a 256x96 24bpp
// image with a big top line (category, white) and a smaller yellow bottom line (subtext), on black.
// DrawTextW so CJK / special chars render (the ANSI GenerateBmpFile can't). utf8→wide first.
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    if (n <= 0) { // not valid UTF-8 → treat as the system ANSI codepage
        n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w((size_t)(n > 0 ? n : 0), 0);
        if (n > 0) { MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n); w.resize((size_t)n - 1); }
        return w;
    }
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

static int CompareCatalogText(const std::string& a, const std::string& b) {
    std::wstring wa = Utf8ToWide(a), wb = Utf8ToWide(b);
    int result = CompareStringW(LOCALE_INVARIANT,
        NORM_IGNORECASE | NORM_IGNORENONSPACE | SORT_STRINGSORT,
        wa.c_str(), -1, wb.c_str(), -1);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    if (wa != wb) return wa < wb ? -1 : 1;
    return 0;
}

static bool SongTitleLess(const CustomBmsSong& a, const CustomBmsSong& b) {
    int byTitle = CompareCatalogText(a.title, b.title);
    if (byTitle != 0) return byTitle < 0;
    int byArtist = CompareCatalogText(a.artist, b.artist);
    if (byArtist != 0) return byArtist < 0;
    return CompareCatalogText(a.songDir, b.songDir) < 0;
}
// TWO version asset folders exist, feeding two different animation objects:
//     system/SongSelect/Version/version_NN.abm         128 x 16  -> anim at +0xBE8D8 slot 1
//     system/SongSelect/V_View/version/version_NN.abm  256 x 64  -> anim at +0xBEAF4 slot 2
// The label under the song title -- the one this is for -- is the FIRST one, so this asset is
// 128x16. An earlier 256x96 version of it did render there, just overflowing onto the title and the
// BPM, which was the clue that this slot draws at the texture's own size.
// alignRight+single-line = the small label under the song title (128x16).
// centred+two-line       = the big category-change overlay, which must match the mask it sits on
//                          (sortname_mask.abm is 256x64); rendering that one at 128x16 and letting
//                          the game scale it up 4x is what made the text pixelated and off-centre.
// twoLine is explicit rather than inferred from the alignment: the wheel tab is a single centred
// line that has to fill the whole 16px the way the native tabs do, and inferring it from alignRight
// left the tab boxed into the top 10px with a 10px font, which is why the custom tabs came out
// visibly smaller than "OTH" next to them.
// maxPxOverride: 0 keeps each branch's own sizing. The right-aligned branch fills the canvas
// (h-1), which is right for a 128x16 version label but enormous on a 256x32 songname -- 31px
// against the 13px the game's own song titles use. Callers drawing into a songname slot pass 13.
static bool RenderSplashBytes(std::vector<unsigned char>& out, const std::string& category,
                              const std::string& subtext, int w = 128, int h = 16,
                              bool alignRight = true, bool twoLine = false,
                              int maxPxOverride = 0, COLORREF inkOverride = 0) {
    HDC screen = GetDC(NULL);
    if (!screen) return false;
    HDC hdc = CreateCompatibleDC(screen);
    if (!hdc) { ReleaseDC(NULL, screen); return false; }
    BITMAPINFOHEADER bih{}; bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = -h;
    bih.biPlanes = 1; bih.biBitCount = 24; bih.biCompression = BI_RGB;
    BITMAPINFO bi{}; bi.bmiHeader = bih;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(hdc); ReleaseDC(NULL, screen); return false; }
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    RECT full = {0,0,w,h};
    HBRUSH black = CreateSolidBrush(RGB(0,0,0)); FillRect(hdc, &full, black); DeleteObject(black);
    SetBkMode(hdc, TRANSPARENT);
    // Fit by WIDTH only. Measuring height too is what made every custom label come out smaller than
    // the native art beside it: DrawText reports a full line box (~1.15em) even for one line, so a
    // 15px font never "fits" a 16px canvas and the loop shrank it several pixels. The native assets
    // are all-caps with no descenders, so a line box overhanging the canvas costs nothing.
    // The loop also always ends holding a font -- the old one returned with font==nullptr when the
    // text was too long even at 6px, silently rendering an empty sprite instead of an elided one.
    // condense: fit by narrowing the glyphs instead of shrinking them.
    //
    // The wheel row is drawn at roughly a third of the sprite's width, and a stroke only survives
    // that downscale if it is at least two pixels wide. Dropping the point size to make a long name
    // fit takes the strokes to one pixel: "ARC.SW" came out perfectly legible in the bitmap at 10px
    // and then vanished completely on screen, while its shorter neighbours -- still at 13px, still
    // two pixels thick -- were fine. Holding the height and squeezing the width keeps the weight.
    auto draw = [&](const std::wstring& txt, RECT r, int maxPx, COLORREF col, bool condense = false) {
        if (txt.empty()) return;
        HFONT font = nullptr, oldFont = nullptr;
        if (condense) {
            int width = 0;                       // 0 = the face's own proportions
            for (;;) {
                font = CreateFontW(-maxPx, width, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                   DEFAULT_PITCH | FF_SWISS, L"Arial");
                oldFont = (HFONT)SelectObject(hdc, font);
                RECT calc = r; DrawTextW(hdc, txt.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE);
                // The floor is a real one: past about a third of the height the glyphs stop being
                // distinguishable from each other, and ellipsis is the better answer.
                if ((calc.right - calc.left) <= (r.right - r.left) || width == maxPx / 3) break;
                SelectObject(hdc, oldFont); DeleteObject(font); font = nullptr;
                width = width ? width - 1 : maxPx * 2 / 3;
            }
        } else {
        for (int px = maxPx < 6 ? 6 : maxPx; ; --px) {
            font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Arial");
            oldFont = (HFONT)SelectObject(hdc, font);
            RECT calc = r; DrawTextW(hdc, txt.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE);
            if (px <= 6 || (calc.right - calc.left) <= (r.right - r.left)) break;
            SelectObject(hdc, oldFont); DeleteObject(font); font = nullptr;
        }
        }
        const UINT fmt = (alignRight ? DT_RIGHT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        // The native 256x64 art has symmetric antialiasing and no drop shadow; only the cramped
        // 128x16 slots need the extra pixel of contrast to stay readable.
        if (h <= 16) {
            SetTextColor(hdc, RGB(0,0,0));
            RECT sh = r; sh.top += 1; sh.bottom += 1;
            DrawTextW(hdc, txt.c_str(), -1, &sh, fmt);
        }
        SetTextColor(hdc, col);
        DrawTextW(hdc, txt.c_str(), -1, &r, fmt);
        SelectObject(hdc, oldFont); DeleteObject(font);
    };
    // CATEGORY on top in white, SUBTEXT under it in yellow -- the arrangement the native label in
    // this slot uses. With no details.txt both are empty and the slot renders blank on purpose,
    // rather than falling back to the folder name: a raw "PIU.1" in the song panel is noise. The
    // texture is still produced (empty) because a missing file leaves a null slot.
    if (!twoLine && alignRight) {
        // The label under the song title: one right-aligned line over the whole canvas. Native
        // category_NN.abm is 128x16 with its ink in y 2..12, i.e. 11px caps, which Arial Bold
        // reaches at an em of about h-1.
        draw(Utf8ToWide(category), RECT{2, 0, w - 3, h},
             maxPxOverride ? maxPxOverride : h - 1,
             inkOverride ? inkOverride : RGB(235,235,235));
    } else if (!twoLine) {
        // A wheel tab. The sprite is 128px but the game draws the row at roughly 45px of pitch, so
        // what matters is the INK width, not the canvas. Native names are three characters and come
        // out 21-27px wide; ours run to seven and reached 59px, which is why they ran into each
        // other on screen.
        //
        // Narrowing the box rather than the font is deliberate: the fit loop only shrinks text that
        // does not fit, so short names keep full size and native proportions while long ones step
        // down on their own. A 40px box measures: OTH 25px, ANX 27px, ARC.1 36px, ARC.SW 40px --
        // all inside the pitch, with the short ones untouched.
        const int box = 40;
        const int pad = (w - box) / 2 > 0 ? (w - box) / 2 : 2;
        draw(Utf8ToWide(category), RECT{pad, 0, w - pad, h}, 13, RGB(235,235,235), true);
    } else {
        // Category sprite: BOTH lines live in this one texture, exactly like the native art --
        // the name in white over a smaller subtitle in orange. The canvas was 128x16, to match every
        // other Sortimage asset, and at that size the two lines landed on top of each other: the
        // white name and the orange caption overlapped into an illegible smear, which the game then
        // scaled up. That is the "stretched, pixelated, no subtext" overlay -- the subtext was
        // always there, printed through the title.
        //
        // 256x64 was tried once and rejected for good reason: it changes the aspect from 8:1 to
        // 4:1 and the overlay came out stretched across the mask. 256x32 keeps the 8:1 exactly and
        // only doubles the resolution, which is about 1:1 with the size the game draws it at.
        // Geometry lifted off the shipped sortstyle_NN.bmp (256x64 24bpp), all 30 of which place
        // their ink identically: the white category name occupies y 18..31 (14px caps) centred on
        // x=128, and the orange caption y 43..50 (8px caps). Arial Bold reaches those cap heights
        // at ems of 20 and 11. Kept as fractions of the canvas so the 128x16 variant is the same
        // layout at a quarter scale. Orange is the native (255,174,0), not an approximation.
        const int pad = w / 16;
        draw(Utf8ToWide(category), RECT{pad, h * 6 / 64,  w - pad, h * 46 / 64}, h * 20 / 64, RGB(255,255,255));
        draw(Utf8ToWide(subtext),  RECT{pad, h * 34 / 64, w - pad, h * 61 / 64}, h * 11 / 64, RGB(255,174,0));
    }

    int rowBytes = ((w * 24 / 8 + 3) & ~3), pixelBytes = rowBytes * h;
    std::vector<unsigned char> pixels(pixelBytes, 0);
    BITMAPINFOHEADER bihBU = bih; bihBU.biHeight = h; bihBU.biSizeImage = (DWORD)pixelBytes;
    GetDIBits(hdc, bmp, 0, (UINT)h, pixels.data(), (BITMAPINFO*)&bihBU, DIB_RGB_COLORS);
    SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc); ReleaseDC(NULL, screen);
    BITMAPFILEHEADER bfh{}; bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + pixelBytes;
    out.clear();
    out.reserve(bfh.bfSize);
    const unsigned char* p1 = reinterpret_cast<const unsigned char*>(&bfh);
    out.insert(out.end(), p1, p1 + sizeof(bfh));
    const unsigned char* p2 = reinterpret_cast<const unsigned char*>(&bihBU);
    out.insert(out.end(), p2, p2 + sizeof(bihBU));
    out.insert(out.end(), pixels.begin(), pixels.end());
    return true;
}

// The .abm transform, applied to a BMP that never touched disk. Identical to GenerateAbmFromBmp:
// swap the magic, restate bpp/width/height, XOR four header dwords. Pixel data is untouched.
static bool AbmFromBmpBytes(std::vector<unsigned char>& b) {
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    uint16_t bpp = 0, width = 0, height = 0;
    memcpy(&bpp, b.data() + 28, 2);
    memcpy(&width, b.data() + 18, 2);
    memcpy(&height, b.data() + 22, 2);
    b[0] = 'A'; b[1] = 'W';
    memcpy(b.data() + 2, &bpp, 2);
    b[4] = 0; b[5] = 0;
    memcpy(b.data() + 6, &width, 2);
    memcpy(b.data() + 8, &height, 2);
    const uint32_t xors[] = {0x109a, 0xcfa1, 0x51ae, 0xb18f};
    const size_t offsets[] = {10, 18, 22, 28};
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        memcpy(&v, b.data() + offsets[i], 4);
        v ^= xors[i];
        memcpy(b.data() + offsets[i], &v, 4);
    }
    return true;
}

// Public entry: hand back a finished .abm for one custom category, built entirely in RAM.
// twoLine = the category sprite (name over subtitle, centred); otherwise the right-aligned label
// that sits under the song title.
static bool ReadCategoryDetails(const std::string& mode, const std::string& category,
                                std::string& categoryText, std::string& subText);

bool BuildCategoryAbmInMemory(const std::string& mode, size_t categoryIndex, int kind,
                              std::vector<unsigned char>& out) {
    // The RANDOM tile belongs to no category, so it is built before the lookup that would
    // reject it. 256x32 right-aligned is the shape of native songname art.
    if (kind == kCatAbmRandom)
        return RenderSplashBytes(out, "RANDOM", std::string(), 256, 32, true, false, 13,
                                 RGB(255, 174, 0)) &&
               AbmFromBmpBytes(out);

    // Empty text on the standard black ground: a plain 256x256 tile, the disc's own geometry.
    if (kind == kCatAbmPreviewBase)
        return RenderSplashBytes(out, std::string(), std::string(), 256, 256, true, false) &&
               AbmFromBmpBytes(out);
    if (kind == kCatAbmOverlayBg)
        return RenderSplashBytes(out, std::string(), std::string(), 640, 480, true, false) &&
               AbmFromBmpBytes(out);
    if (kind == kCatAbmOverlayIco)
        return RenderSplashBytes(out, std::string(), std::string(), 480, 480, true, false) &&
               AbmFromBmpBytes(out);
    if (kind == kCatAbmPanel)
        return RenderSplashBytes(out, std::string(), std::string(), 320, 72, true, false) &&
               AbmFromBmpBytes(out);
    if (kind == kCatAbmLabelEz)
        return RenderSplashBytes(out, "EZ", std::string(), 64, 16, false, false, 13, RGB(50, 214, 255)) &&
               AbmFromBmpBytes(out);

    std::vector<std::string> cats = GetCustomBmsCategoriesForMode(mode);
    if (categoryIndex >= cats.size()) return false;
    std::string cat, sub;
    bool haveDetails = ReadCategoryDetails(mode, cats[categoryIndex], cat, sub) && !cat.empty();
    if (!haveDetails) cat = cats[categoryIndex];
    bool ok = false;
    switch (kind) {
        case kCatAbmTab:
            // The wheel tab shows the FOLDER name -- ANX, ARC.1, BD.2 -- not the pretty name from
            // details.txt. Only the folder name is short enough for a tab, and it is what the strip
            // has always shown. A marker build swapped this to `cat` so the probe letter would be
            // visible, and putting the marker back left the swap behind: the strip filled up with
            // "Arcaea (...", "BanG Dr..." truncated mid-word. `cats[categoryIndex]` is the folder.
            ok = RenderSplashBytes(out, cats[categoryIndex], std::string(), 128, 16, false, false);
            break;
        case kCatAbmOverlay:  // wheel-side category sprite, 128x16 like every Sortimage asset
            ok = RenderSplashBytes(out, cat, sub, 128, 16, false, true);
            break;
        case kCatAbmSort:     // big overlay: 256x64 24bpp, matching the native sortstyle art
            ok = RenderSplashBytes(out, cat, sub, 256, 64, false, true);
            break;
        default:              // kCatAbmLabel: the right-aligned line under the song title
            ok = RenderSplashBytes(out, cat, std::string(), 128, 16, true, false);
            break;
    }
    return ok && AbmFromBmpBytes(out);
}

static bool GenerateAbmFromBmp(const std::string& bmpPath) {
    FILE* f = fopen(bmpPath.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    std::vector<unsigned char> b(size > 0 ? (size_t)size : 0);
    if (!b.empty()) fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;

    // Exact header transform used by EZ2Bmp2abm-FNEX; pixel data is unchanged.
    uint16_t bpp = 0, width = 0, height = 0;
    memcpy(&bpp, b.data() + 28, 2);
    memcpy(&width, b.data() + 18, 2);
    memcpy(&height, b.data() + 22, 2);
    b[0] = 'A'; b[1] = 'W';
    memcpy(b.data() + 2, &bpp, 2);
    b[4] = 0; b[5] = 0;
    memcpy(b.data() + 6, &width, 2);
    memcpy(b.data() + 8, &height, 2);
    const uint32_t xors[] = {0x109a, 0xcfa1, 0x51ae, 0xb18f};
    const size_t offsets[] = {10, 18, 22, 28};
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        memcpy(&v, b.data() + offsets[i], 4);
        v ^= xors[i];
        memcpy(b.data() + offsets[i], &v, 4);
    }
    bool ok = writeBinaryFile(replaceExtensionLocal(bmpPath, ".abm"), b);
    // The .bmp is a build-time intermediate; the game only ever opens the .abm. Drop it once the .abm
    // is written (25777 leftover .bmp = ~1.4GB of dead cache before this). Callers self-check the .abm,
    // not the .bmp, so this is safe. (eyecatch/splash already deleted theirs; now it's uniform.)
    if (ok) DeleteFileA(bmpPath.c_str());
    return ok;
}

static int GenerateVisualPair(const std::string& bmpPath, int w, int h,
                              COLORREF bg, COLORREF fg,
                              const std::string& line1, const std::string& line2,
                              Gdiplus::Image* artwork = nullptr, bool styledText = false,
                              bool circularArtwork = false, int outputBpp = 24) {
    if (!GenerateBmpFile(bmpPath, w, h, bg, fg, line1, line2, artwork, styledText, circularArtwork, outputBpp)) return 0;
    if (!GenerateAbmFromBmp(bmpPath)) return 1;
    std::string abmPath = replaceExtensionLocal(bmpPath, ".abm");
    unsigned char header[10] = {};
    FILE* f = fopen(abmPath.c_str(), "rb");
    size_t got = f ? fread(header, 1, sizeof(header), f) : 0;
    if (f) fclose(f);
    uint16_t bpp = 0, aw = 0, ah = 0;
    if (got == sizeof(header)) {
        memcpy(&bpp, header + 2, 2);
        memcpy(&aw, header + 6, 2);
        memcpy(&ah, header + 8, 2);
    }
    if (got != sizeof(header) || header[0] != 'A' || header[1] != 'W' || bpp != outputBpp || aw != w || ah != h) {
        DeleteFileA(abmPath.c_str());
        LogLine("[assets] ABM selfcheck failed file='%s' expected=%dbpp/%dx%d got=%ubpp/%ux%u",
                abmPath.c_str(), outputBpp, w, h, bpp, aw, ah);
        return 1;
    }
    return 2;
}

// First BGA-video trigger time, in seconds. The video is BMP id N (#BMPNN <file>.wmv/.mp4);
// its play time is the first channel-04/07 event that fires that id. We time it at constant
// BPM, 4/4. ponytail: ignores mid-song BPM changes (ch.03/08) and meter changes (ch.02);
// upgrade to a full bar-timeline if a song with tempo changes before the BGA desyncs.
static double BgaVideoStartSeconds(const std::string& chartPath) {
    FILE* f = fopen(chartPath.c_str(), "rb");
    if (!f) return 0.0;
    char line[2048];
    double bpm = 0.0;
    int videoBmpId = -1;           // the #BMPNN whose file is a video
    int firstBgaMeasure = -1; double firstBgaPosInMeasure = 1.0;
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.size() < 4 || s[0] != '#') continue;
        std::string u = s; std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c){ return (char)toupper(c); });
        if (u.rfind("#BPM", 0) == 0 && s.size() > 4 && !isalnum((unsigned char)s[4]))
            bpm = parseDoubleSafe(trim(s.substr(4)), bpm);
        else if (u.rfind("#BMP", 0) == 0 && s.size() >= 7) {
            int id = (int)strtol(s.substr(4, 2).c_str(), nullptr, 36);
            std::string fileU = u.substr(6);
            // keep in sync with endsWithVideo(): any format ffmpeg can decode is a valid BGA video.
            const char* vext[] = { ".MP4", ".AVI", ".WMV", ".MPG", ".MPEG", ".MOV" };
            for (const char* e : vext) if (fileU.find(e) != std::string::npos) { videoBmpId = id; break; }
        } else if (s.size() > 6 && s[6] == ':') {
            // channel data: #mmmCC:payload. BGA layers = 04 (base) and 07 (layer).
            int ch = (int)strtol(s.substr(4, 2).c_str(), nullptr, 10);
            if (ch == 4 || ch == 7) {
                int measure = (int)strtol(s.substr(1, 3).c_str(), nullptr, 10);
                std::string pay = s.substr(7);
                int slots = (int)pay.size() / 2;
                for (int i = 0; i < slots; ++i) {
                    int obj = (int)strtol(pay.substr(i * 2, 2).c_str(), nullptr, 36);
                    if (obj == 0) continue;
                    if (videoBmpId < 0 || obj == videoBmpId) {
                        double pos = slots ? (double)i / slots : 0.0;
                        if (firstBgaMeasure < 0 || measure < firstBgaMeasure ||
                            (measure == firstBgaMeasure && pos < firstBgaPosInMeasure)) {
                            firstBgaMeasure = measure; firstBgaPosInMeasure = pos;
                        }
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
    if (firstBgaMeasure < 0 || bpm <= 0.0) return 0.0;
    double secPerBeat = 60.0 / bpm;
    return (firstBgaMeasure + firstBgaPosInMeasure) * 4.0 * secPerBeat; // 4 beats/measure
}

// BMS BGA video → game .spv. Native .spv is just ASF/WMV (magic 30 26 B2 75…, codec
// wmv3, 256x256, video-only). ffmpeg has no VC-1/wmv3 ENCODER, so we use wmv2 (the game's
// DirectShow decodes both); 256x256, no audio (the chart audio plays separately). Output
// goes to cacheDir\<id>.spv — the FS-hook already routes Video\<id>\<id>.spv there.
// ponytail: shells out to ffmpeg; no in-proc transcode lib for a one-shot cache step.
// Returns the ffmpeg path AND whether it's a MODERN build (new CLI + working H264 decoder). Video
// conversion is an OFFLINE cache step on the authoring PC — it does NOT need to be the XP-era binary,
// only the game runtime does. Prefer a modern ffmpeg (C:\ffmpeg\bin) because the shipped 2008 build
// mis-decodes modern H264 .mp4 (corrupt output) and always injects a 3.1s ASF preroll → desynced BGA.
static std::string FfmpegExePath(bool& modernOut) {
    // modern first (system install), then the game's 2008 fallback.
    const char* modern[] = { "C:\\ffmpeg\\bin\\ffmpeg.exe", "ffmpeg-modern.exe" };
    for (const char* p : modern) if (fileExistsLocal(p)) { modernOut = true; return p; }
    for (const char* p : modern) if (fileExistsLocal(JoinPath(GetExeDirectory(), p))) { modernOut = true; return JoinPath(GetExeDirectory(), p); }
    const char* old8[] = { "ffmpeg.exe", "FFMPEG.INC.EXE" };
    for (const char* p : old8) if (fileExistsLocal(JoinPath(GetExeDirectory(), p))) { modernOut = false; return JoinPath(GetExeDirectory(), p); }
    modernOut = false;
    return std::string();
}
// capSeconds: 0 = keep the FULL source length (own-BGA — it must last the whole chart, else it freezes
// on the last frame partway through), >0 = trim (shared pool clips, where a cap saves disk).
// Parallel ffmpeg job pool: the wmv2 encoder is single-threaded, so at 707 videos the sequential
// system() calls used ~1 core of 12. Run up to N conversions at once (N = CPU cores, capped), each an
// independent ffmpeg process — same output params, just concurrent. ConvertVideoToSpv now ENQUEUES:
// it blocks only when N are already running; WaitAllVideoJobs() at the end of the scan drains the rest.
// ponytail: process pool, not a thread pool — ffmpeg is out-of-proc anyway, so no locking needed.
struct VideoJob { HANDLE proc; std::string dst; };
static std::vector<VideoJob> g_videoJobs;
static unsigned VideoJobLimit() {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    unsigned n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 4;
    if (n > 16) n = 16;   // diminishing returns + disk/IO bound past this
    if (n < 1)  n = 1;
    return n;
}
static void ReapVideoJob(VideoJob& j) {
    if (!j.proc) return;
    WaitForSingleObject(j.proc, INFINITE);
    DWORD code = 1; GetExitCodeProcess(j.proc, &code);
    CloseHandle(j.proc); j.proc = nullptr;
    LogLine("[video] done '%s' rc=%lu ok=%d", j.dst.c_str(), (unsigned long)code, (code == 0 && fileExistsLocal(j.dst)) ? 1 : 0);
}
void WaitAllVideoJobs() {
    for (auto& j : g_videoJobs) ReapVideoJob(j);
    g_videoJobs.clear();
}
static bool ConvertVideoToSpv(const std::string& srcVideo, const std::string& dstSpv, double startDelaySec, int capSeconds) {
    bool modern = false;
    std::string ff = FfmpegExePath(modern);
    if (ff.empty()) { LogLine("[video] ffmpeg not found — skipping"); return false; }
    // The BGA fires `startDelaySec` into the chart but the game plays the .spv from t=0 — delay the input
    // by that much (-itsoffset before -i) to keep video and chart in sync. `-muxpreload/-muxdelay 0`
    // strips the ASF preroll that otherwise offset the whole stream ~3.1s (the main "desynced BGA" bug).
    char off[48] = "";
    if (startDelaySec > 0.05) snprintf(off, sizeof(off), "-itsoffset %.3f ", startDelaySec);
    char cap[24] = "";
    if (capSeconds > 0) snprintf(cap, sizeof(cap), " -t %d", capSeconds);
    std::string cmd;
    if (modern) {
        cmd = std::string("\"") + ff + "\" -y -v error " + off + "-i \"" + srcVideo +
              "\" -an -vf scale=256:256 -c:v wmv2 -b:v 1200k" + cap + " -muxpreload 0 -muxdelay 0 -f asf \"" + dstSpv + "\"";
    } else {
        // 2008 fallback: old CLI, no muxpreload control (leaves a small ASF preroll). H264 may fail here.
        cmd = std::string("\"") + ff + "\" -y -v 0 " + off + "-i \"" + srcVideo +
              "\" -an -s 256x256 -vcodec wmv2 -b 1200k" + cap + " -f asf \"" + dstSpv + "\"";
    }
    // Block until a slot is free (reap finished jobs; if all still running, wait for the oldest).
    const unsigned limit = VideoJobLimit();
    for (;;) {
        for (auto it = g_videoJobs.begin(); it != g_videoJobs.end();) {
            if (WaitForSingleObject(it->proc, 0) == WAIT_OBJECT_0) { ReapVideoJob(*it); it = g_videoJobs.erase(it); }
            else ++it;
        }
        if (g_videoJobs.size() < limit) break;
        WaitForSingleObject(g_videoJobs.front().proc, INFINITE);
    }
    // Launch ffmpeg detached (no console window), keep the handle to reap later.
    STARTUPINFOA sinfo{}; sinfo.cb = sizeof(sinfo);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
    BOOL launched = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &sinfo, &pi);
    if (!launched) { LogLine("[video] launch failed '%s'", dstSpv.c_str()); return false; }
    CloseHandle(pi.hThread);
    g_videoJobs.push_back({ pi.hProcess, dstSpv });
    return true; // enqueued; result confirmed by WaitAllVideoJobs / ReapVideoJob
}

// GDI+ reads jpg/jpeg/png/bmp all the same, so any of them is valid source artwork (not just png).
static bool isArtworkImage(const std::string& f) {
    return endsWith(f, ".png") || endsWith(f, ".jpg") || endsWith(f, ".jpeg") || endsWith(f, ".bmp");
}



// What the process has left, for the moment rendering starts failing.
//
// GDI+ startup and plain GDI began failing together, which rules out the GDI handle table (that
// would not touch GdiplusStartup) and points at address space. Guessing has already cost several
// rounds here, so this reports the numbers instead: committed private bytes from a VirtualQuery
// walk, the largest free block left, and the GDI/USER handle counts for contrast. All of it is
// XP-safe -- VirtualQuery and GetGuiResources both date to NT4/2000.
struct ProcessFootprint {
    uint64_t committedBytes;
    uint64_t largestFreeBytes;
    uint32_t regions;
    uint32_t gdiObjects;
    uint32_t userObjects;
};

static ProcessFootprint MeasureProcessFootprint() {
    ProcessFootprint f{};
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
    unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT) f.committedBytes += mbi.RegionSize;
        else if (mbi.State == MEM_FREE && mbi.RegionSize > f.largestFreeBytes)
            f.largestFreeBytes = mbi.RegionSize;
        ++f.regions;
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    f.gdiObjects  = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    f.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    return f;
}

static void LogLargestRegions(const char* why);
static void LogMemoryAttribution(const char* why);
static inline void LogLargestRegionsFwd(const char* why) { LogLargestRegions(why); }

static void LogProcessFootprint(const char* why);
void ReportProcessFootprint(const char* why) { LogProcessFootprint(why); }

static void LogProcessFootprint(const char* why) {
    const ProcessFootprint f = MeasureProcessFootprint();
    LogLine("[mem] %s: committed=%luMB largestFreeBlock=%luMB regions=%lu gdiObjects=%lu userObjects=%lu",
            why, (unsigned long)(f.committedBytes / 1048576),
            (unsigned long)(f.largestFreeBytes / 1048576),
            (unsigned long)f.regions, (unsigned long)f.gdiObjects, (unsigned long)f.userObjects);
    LogLargestRegionsFwd(why);
    LogMemoryAttribution(why);
}

// The ten largest committed allocations, so a commit figure can be attributed rather than guessed
// at. Reservations are reported separately: a 512 MB reserve costs address space but no memory, and
// on a 32-bit target the difference decides whether the process fits at all.
// Who owns the committed memory.
//
// The region walk below says HOW MUCH is committed and of what kind, but not who asked for it, and
// that is the whole question: the largest single thing in this process was a 512MB private region
// that never appeared in the VirtualAlloc log (it turned out to be the game's .str clip pool,
// KProfile @0x530c2c, since put back to 64MB -- see mod_main). It could not have: our hook patches the IAT of
// EZ2B.exe, so a DLL calling VirtualAlloc through its own IAT is invisible -- and a HEAP growing is
// invisible to IAT hooking altogether, because ntdll does not import the function, it implements it.
//
// So instead of hooking more, attribute what is already there:
//
//   - MEM_IMAGE   -> the module loaded at that base
//   - MEM_MAPPED  -> the file behind it, via GetMappedFileName (psapi, XP-safe, loaded by name so
//                    it never becomes a hard import)
//   - MEM_PRIVATE -> a process heap if its base is one, otherwise a raw VirtualAlloc
//
// A 512MB region that turns out to be a heap and a 512MB region that turns out to be one
// VirtualAlloc call are completely different bugs, and this is what tells them apart.
static void LogMemoryAttribution(const char* why) {
    typedef DWORD (WINAPI *GetMappedFileNameA_t)(HANDLE, LPVOID, LPSTR, DWORD);
    static GetMappedFileNameA_t s_mappedName = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        // psapi.dll on XP; kernel32 forwards it on later Windows under a K32 name.
        HMODULE psapi = LoadLibraryA("psapi.dll");
        if (psapi)
            s_mappedName = (GetMappedFileNameA_t)GetProcAddress(psapi, "GetMappedFileNameA");
        if (!s_mappedName) {
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            if (k32)
                s_mappedName = (GetMappedFileNameA_t)GetProcAddress(k32, "K32GetMappedFileNameA");
        }
    }

    HANDLE heaps[64] = {};
    const DWORD heapCount = GetProcessHeaps(64, heaps);

    struct Owned { uintptr_t base; uint64_t bytes; const char* kind; char name[MAX_PATH]; };
    std::vector<Owned> owned;
    uint64_t heapBytes = 0, imageBytes = 0, mappedBytes = 0, anonBytes = 0;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
    unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi{};
    const HANDLE self = GetCurrentProcess();
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            Owned o{};
            o.base = (uintptr_t)mbi.AllocationBase ? (uintptr_t)mbi.AllocationBase
                                                   : (uintptr_t)mbi.BaseAddress;
            o.bytes = (uint64_t)mbi.RegionSize;
            if (mbi.Type == MEM_IMAGE) {
                o.kind = "image";
                imageBytes += o.bytes;
                GetModuleFileNameA((HMODULE)o.base, o.name, sizeof(o.name));
            } else if (mbi.Type == MEM_MAPPED) {
                o.kind = "mapped";
                mappedBytes += o.bytes;
                if (s_mappedName) s_mappedName(self, mbi.BaseAddress, o.name, sizeof(o.name));
            } else {
                o.kind = "anon";
                bool isHeap = false;
                for (DWORD h = 0; h < heapCount; ++h)
                    if ((uintptr_t)heaps[h] == o.base) { isHeap = true; break; }
                if (isHeap) { o.kind = "heap"; heapBytes += o.bytes; }
                else        { anonBytes += o.bytes; }
            }
            // Fold consecutive regions of one allocation into a single row.
            if (!owned.empty() && owned.back().base == o.base) owned.back().bytes += o.bytes;
            else owned.push_back(o);
        }
        if (next <= p) break;
        p = next;
    }

    LogLine("[mem] %s owners: heap=%luMB image=%luMB mapped=%luMB anon=%luMB (%lu heaps)",
            why, (unsigned long)(heapBytes / 1048576), (unsigned long)(imageBytes / 1048576),
            (unsigned long)(mappedBytes / 1048576), (unsigned long)(anonBytes / 1048576),
            (unsigned long)heapCount);

    std::sort(owned.begin(), owned.end(),
              [](const Owned& a, const Owned& b) { return a.bytes > b.bytes; });
    const size_t show = owned.size() < 12 ? owned.size() : 12;
    for (size_t i = 0; i < show; ++i) {
        const char* nm = owned[i].name[0] ? owned[i].name : "";
        const char* tail = nm;
        for (const char* c = nm; *c; ++c) if (*c == '\\' || *c == '/') tail = c + 1;
        LogLine("[mem]   %-6s %08lx %6luMB %s", owned[i].kind,
                (unsigned long)owned[i].base, (unsigned long)(owned[i].bytes / 1048576), tail);
    }
}

static void LogLargestRegions(const char* why) {
    struct Region { uintptr_t base; uint64_t bytes; uint32_t protect; uint32_t type; bool committed; };
    std::vector<Region> regions;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
    unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t reserved = 0, imageBytes = 0, mappedBytes = 0, privateBytes = 0;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT) {
            regions.push_back({(uintptr_t)mbi.BaseAddress, (uint64_t)mbi.RegionSize,
                               mbi.Protect, mbi.Type, true});
            if (mbi.Type == MEM_IMAGE)        imageBytes   += mbi.RegionSize;
            else if (mbi.Type == MEM_MAPPED)  mappedBytes  += mbi.RegionSize;
            else                              privateBytes += mbi.RegionSize;
        } else if (mbi.State == MEM_RESERVE) {
            reserved += mbi.RegionSize;
        }
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.bytes > b.bytes; });
    LogLine("[mem] %s breakdown: private=%luMB image=%luMB mapped=%luMB reservedOnly=%luMB",
            why, (unsigned long)(privateBytes / 1048576), (unsigned long)(imageBytes / 1048576),
            (unsigned long)(mappedBytes / 1048576), (unsigned long)(reserved / 1048576));
    const size_t show = regions.size() < 10 ? regions.size() : 10;
    for (size_t i = 0; i < show; ++i) {
        const char* kind = regions[i].type == MEM_IMAGE ? "image"
                         : regions[i].type == MEM_MAPPED ? "mapped" : "private";
        LogLine("[mem]   #%u %08lx %6luMB %s prot=%lx", (unsigned)(i + 1),
                (unsigned long)regions[i].base, (unsigned long)(regions[i].bytes / 1048576),
                kind, (unsigned long)regions[i].protect);
    }
}

// A valid .abm built without touching GDI, for when rendering one is not possible.
//
// Asset generation goes through GDI: GetDC, CreateCompatibleDC, CreateDIBSection. Every one of
// those can fail, and a process only gets 10,000 GDI handles. When they do fail the caller has no
// way to draw anything -- but it still must not leave the file missing, because the game's loader
// treats a .abm that will not open as a licence to run off the end of a 260-byte stack buffer
// (sub_407E50 -> sub_412F00 -> sub_407B60). A flat rectangle is a poor songname; a missing file is
// a crash. This writes the rectangle: header plus a memset, no handles of any kind.
static void writeSmallTextFileLocal(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static bool WriteFlatAbmFile(const std::string& abmPath, int w, int h, int bpp, unsigned char level) {
    if (w <= 0 || h <= 0 || (bpp != 16 && bpp != 24)) return false;
    const int bytesPerPixel = bpp / 8;
    const int rowBytes = ((w * bytesPerPixel + 3) & ~3);
    const size_t pixelBytes = (size_t)rowBytes * (size_t)h;
    std::vector<unsigned char> out(54 + pixelBytes, level);
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = 54;
    bfh.bfSize = (DWORD)out.size();
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = (WORD)bpp;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)pixelBytes;
    memcpy(out.data(), &bfh, sizeof(bfh));
    memcpy(out.data() + sizeof(bfh), &bih, sizeof(bih));
    if (!AbmFromBmpBytes(out)) return false;
    return writeBinaryFile(abmPath, out);
}

// One renderer at a time. The game thread renders what the cursor needs right now, the art
// prefetch thread renders what it will need next; the lock keeps them off the same files and
// makes the GDI+ start-up below a single event.
static CRITICAL_SECTION g_visualGenLock;
static bool g_visualGenLockReady = false;
static void VisualGenLockInit() {
    if (g_visualGenLockReady) return;
    InitializeCriticalSection(&g_visualGenLock);
    g_visualGenLockReady = true;
}
struct VisualGenGuard {
    VisualGenGuard() { VisualGenLockInit(); EnterCriticalSection(&g_visualGenLock); }
    ~VisualGenGuard() { LeaveCriticalSection(&g_visualGenLock); }
};

static void GenerateVisualAssetCacheLocked(const CustomBmsSong& song, const char* onlyRole);
static void GenerateVisualAssetCache(const CustomBmsSong& song, const char* onlyRole = nullptr) {
    VisualGenGuard guard;
    GenerateVisualAssetCacheLocked(song, onlyRole);
}

// ---- art prefetch ------------------------------------------------------------------------------
static bool RamAssetHas(const std::string& key);
static bool AbsorbFileToRam(const std::string& key, const std::string& path, std::vector<unsigned char>* out);
static std::vector<std::string> g_artQueue;          // ids still to render, front first
static CRITICAL_SECTION g_artQueueLock;
static bool g_artQueueLockReady = false;
static HANDLE g_artThread = nullptr;
static volatile LONG g_artBusy = 0;

static DWORD WINAPI artPrefetchProc(void*) {
    unsigned rendered = 0, seen = 0;
    const DWORD t0 = GetTickCount();
    for (;;) {
        std::string id;
        EnterCriticalSection(&g_artQueueLock);
        if (!g_artQueue.empty()) { id = g_artQueue.front(); g_artQueue.erase(g_artQueue.begin()); }
        LeaveCriticalSection(&g_artQueueLock);
        if (id.empty()) break;
        const CustomBmsSong* song = FindCustomSongById(id.c_str());
        if (!song || song->cacheDir.empty()) continue;
        ++seen;
        // Cheap when the art exists: each role checks the store / its .abm before rendering.
        for (const char* role : {"songname", "discsmall", "disc"}) {
            const std::string abm = JoinPath(JoinPath(song->cacheDir, role), song->id + ".abm");
            if (Config().assetsInMemory && RamAssetHas(song->id + std::string("|") + role)) continue;
            if (!fileExistsLocal(abm)) {
                ensureDir(JoinPath(GetExeDirectory(), Config().cacheRoot));
                ensureDir(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), song->mode));
                ensureDir(song->cacheDir);
                GenerateVisualAssetCache(*song, role);
                ++rendered;
            }
            if (Config().assetsInMemory && fileExistsLocal(abm))
                AbsorbFileToRam(song->id + std::string("|") + role, abm, nullptr);
        }
    }
    if (rendered)
        LogLine("[artprefetch] %u assets rendered for %u songs in %lu ms", rendered, seen,
                (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&g_artBusy, 0);
    return 0;
}

void ArtPrefetchPrioritize(const std::vector<std::string>& ids) {
    if (!Config().artPrefetch || ids.empty()) return;
    if (!g_artQueueLockReady) { InitializeCriticalSection(&g_artQueueLock); g_artQueueLockReady = true; }
    EnterCriticalSection(&g_artQueueLock);
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        auto at = std::find(g_artQueue.begin(), g_artQueue.end(), *it);
        if (at != g_artQueue.end()) g_artQueue.erase(at);
        g_artQueue.insert(g_artQueue.begin(), *it);
    }
    LeaveCriticalSection(&g_artQueueLock);
    if (InterlockedCompareExchange(&g_artBusy, 1, 0) != 0) return;   // the running thread picks them up
    if (g_artThread) { CloseHandle(g_artThread); g_artThread = nullptr; }
    g_artThread = CreateThread(nullptr, 0, artPrefetchProc, nullptr, CREATE_SUSPENDED, nullptr);
    if (!g_artThread) { InterlockedExchange(&g_artBusy, 0); return; }
    SetThreadPriority(g_artThread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(g_artThread);
}

void ArtPrefetchRequest(const std::vector<std::string>& ids) {
    if (!Config().artPrefetch) return;
    if (!g_artQueueLockReady) { InitializeCriticalSection(&g_artQueueLock); g_artQueueLockReady = true; }
    EnterCriticalSection(&g_artQueueLock);
    g_artQueue = ids;
    LeaveCriticalSection(&g_artQueueLock);
    if (InterlockedCompareExchange(&g_artBusy, 1, 0) != 0) return;   // the running thread drains the new list
    if (g_artThread) { CloseHandle(g_artThread); g_artThread = nullptr; }
    g_artThread = CreateThread(nullptr, 0, artPrefetchProc, nullptr, CREATE_SUSPENDED, nullptr);
    if (!g_artThread) { InterlockedExchange(&g_artBusy, 0); return; }
    SetThreadPriority(g_artThread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(g_artThread);
}

static void GenerateVisualAssetCacheLocked(const CustomBmsSong& song, const char* onlyRole) {
    if (song.cacheDir.empty()) return;
    const bool all = onlyRole == nullptr;
    const std::string role = onlyRole ? onlyRole : "";
    const bool wantDisc = all || role == "disc";
    const bool wantDiscSmall = all || role == "discsmall";
    const bool wantEye = all || role == "eyecatch";
    const bool wantResult = all || role == "resultdisc";
    const bool wantName = all || role == "songname";
    const bool wantVideo = all || role == "video";
    const bool wantArtwork = wantDisc || wantDiscSmall || wantEye || wantResult;

    std::string cacheRoot = JoinPath(GetExeDirectory(), Config().cacheRoot);
    ensureDir(cacheRoot);
    ensureDir(JoinPath(cacheRoot, song.mode));
    ensureDir(song.cacheDir);
    if (wantDisc) ensureCacheSubdir(song.cacheDir, "disc");
    if (wantDiscSmall) ensureCacheSubdir(song.cacheDir, "discsmall");
    if (wantEye) ensureCacheSubdir(song.cacheDir, "eyecatch");
    if (wantResult) ensureCacheSubdir(song.cacheDir, "resultdisc");
    if (wantName) ensureCacheSubdir(song.cacheDir, "songname");

    // GDI+ used to be started and shut down inside this function, which runs ONCE PER SONG. A
    // 168-song category preload therefore span GDI+ up and down 168 times -- 719 ms of it in the
    // last log, against 47-188 ms when categories were smaller -- and left generation failing in a
    // contiguous run of 91 songs once the churn caught up with it. One token for the life of the
    // process costs nothing and removes both the stall and the failures.
    static ULONG_PTR s_gdipToken = 0;
    static bool s_gdipStarted = false;
    if ((wantArtwork || wantName) && !s_gdipStarted) {
        Gdiplus::GdiplusStartupInput gdipInput;
        s_gdipStarted = Gdiplus::GdiplusStartup(&s_gdipToken, &gdipInput, nullptr) == Gdiplus::Ok;
        if (!s_gdipStarted) {
            static bool s_reported = false;
            LogLine("[assets] GDI+ startup FAILED; art will fall back to flat tiles");
            if (!s_reported) { s_reported = true; LogProcessFootprint("GDI+ startup refused"); }
        }
    }
    const bool gdipStarted = s_gdipStarted;
    std::string artworkPath;
    UINT artworkW = 0, artworkH = 0;
    uint64_t largestPixels = 0;
    if (gdipStarted && wantArtwork) {
        for (const auto& file : song.imageFiles) {
            if (!isArtworkImage(file)) continue;
            std::string candidatePath = JoinPath(song.songDir, file);
            std::wstring wide = widePathLocal(candidatePath);
            Gdiplus::Image candidate(wide.c_str());
            UINT cw = candidate.GetWidth(), ch = candidate.GetHeight();
            uint64_t pixels = (uint64_t)cw * (uint64_t)ch;
            if (candidate.GetLastStatus() == Gdiplus::Ok && pixels > largestPixels) {
                artworkPath = candidatePath; artworkW = cw; artworkH = ch; largestPixels = pixels;
            }
        }
    }
    std::unique_ptr<Gdiplus::Image> artwork;
    if (!artworkPath.empty()) {
        artwork.reset(new Gdiplus::Image(widePathLocal(artworkPath).c_str()));
        if (artwork->GetLastStatus() != Gdiplus::Ok) artwork.reset();
    }
    auto gen = [&](const char* sub, int w, int h, bool circular, int bpp, const std::string& text = std::string()) {
        std::string bmp = JoinPath(JoinPath(song.cacheDir, sub), song.id + ".bmp");
        std::string abm = replaceExtensionLocal(bmp, ".abm");
        const std::string marker = JoinPath(JoinPath(song.cacheDir, sub), song.id + ".fallback");
        const bool placeholder = fileExistsLocal(marker);
        if (fileExistsLocal(abm) && !placeholder) return 0;
        if (placeholder) DeleteFileA(marker.c_str());   // about to try for a real one
        return GenerateVisualPair(bmp, w, h, RGB(0,0,0), text.empty() ? RGB(180,245,255) : RGB(255,255,255),
                                  text, "", text.empty() ? artwork.get() : nullptr, false, circular, bpp);
    };

    // gen() returns 0 both when the asset was already cached and when rendering failed, so the
    // only trustworthy check is whether the file is there afterwards. Anything still missing gets a
    // flat tile, because the game cannot survive being told to open an .abm that is not there.
    int created = 0, rescued = 0;
    auto ensureOrFlatten = [&](const char* sub, int w, int h, int bpp) {
        const std::string dir = JoinPath(song.cacheDir, sub);
        const std::string abm = JoinPath(dir, song.id + ".abm");
        if (fileExistsLocal(abm)) return;
        if (WriteFlatAbmFile(abm, w, h, bpp, 0)) {
            ++rescued;
            // A flat tile is a placeholder, not a result. Marking it means the next visit re-renders
            // it properly instead of the cache treating one transient out-of-memory blip as this
            // song's permanent artwork -- which is what "textures disappear and never come back"
            // would look like from the wheel.
            writeSmallTextFileLocal(JoinPath(dir, song.id + ".fallback"), "1");
            static unsigned s_reported = 0;
            if (s_reported < 3) { ++s_reported; LogProcessFootprint("render failed"); }
            LogLine("[assets] RENDER FAILED id='%s' role='%s' -- flat %dx%d tile written and marked "
                    "for retry", song.id.c_str(), sub, w, h);
        } else {
            LogLine("[assets] RENDER FAILED id='%s' role='%s' and the fallback could not be written",
                    song.id.c_str(), sub);
        }
    };

    // 1,095 of the installed songs ship no image at all, and these four roles used to sit inside
    // an  if (artwork)  -- so for every one of them the game was left asking for an .abm that was
    // never going to exist. gen() already renders TEXT when it is given some, falling back to the
    // artwork only when the text is empty, so an artless song gets its title on the disc instead of
    // nothing at all. Readable beats black, and both beat a missing file.
    {
        const std::string label = artwork ? std::string() : (song.title.empty() ? song.id : song.title);
        if (wantDisc)      created += gen("disc", 256, 256, true, 24, label);
        if (wantDiscSmall) created += gen("discsmall", 128, 128, true, 24, label);
        if (wantEye)       created += gen("eyecatch", 1024, 512, false, 24, label);
        if (wantResult)    created += gen("resultdisc", 128, 128, true, 16, label);
    }
    if (wantName) {
        created += gen("songname", 256, 32, false, 24, song.title.empty() ? song.id : song.title);
        ensureOrFlatten("songname", 256, 32, 24);
    }
    // Art roles only exist when there was source artwork to build them from; the game asks for them
    // by name regardless, so the same guarantee applies.
    if (wantDisc)      ensureOrFlatten("disc", 256, 256, 24);
    if (wantDiscSmall) ensureOrFlatten("discsmall", 128, 128, 24);
    if (wantResult)    ensureOrFlatten("resultdisc", 128, 128, 16);

    // Legacy disk fallback only. The validated direct profile opens BMSData video in place.
    if (wantVideo && !g_directBmsEnabled && !song.videoFiles.empty()) {
        std::string vdir = JoinPath(JoinPath(GetExeDirectory(), "Video"), song.id);
        ensureDir(JoinPath(GetExeDirectory(), "Video")); ensureDir(vdir);
        std::string spv = JoinPath(vdir, song.id + ".spv");
        std::string src = JoinPath(song.songDir, song.videoFiles[0]);
        std::string iniPath = JoinPath(vdir, song.id + ".ini");
        if (!cacheIsFresh(spv, src)) {
            double delay = song.chartMeta.empty() ? 0.0 : BgaVideoStartSeconds(JoinPath(song.songDir, song.chartMeta[0].file));
            ConvertVideoToSpv(src, spv, delay, 0);
        }
        if (!fileExistsLocal(iniPath))
            FnexWriteEncryptedTextFile(iniPath, "PlayDelayTime = 35\r\nPlayStartPos = 0", FnexCryptoKind::Ini);
    }
    LogLine("[assets] lazy id='%s' role='%s' source='%s' size=%ux%u created=%d rescued=%d",
            song.id.c_str(), all ? "all" : role.c_str(), artworkPath.c_str(), artworkW, artworkH,
            created, rescued);
    artwork.reset();   // GDI+ itself stays up: see the startup note above
}

// ---- RAM asset store ---------------------------------------------------------------------------
// Generated art and previews, keyed "id|role", with an LRU byte budget. With AssetsInMemory the
// renderer still goes through a file (it is a GDI file pipeline), but the file is read into here
// and deleted straight away, so CustomCache holds nothing between sessions but the tiny .ini.
static CRITICAL_SECTION g_ramLock;
static bool g_ramLockReady = false;
struct RamAsset { std::vector<unsigned char> bytes; uint64_t stamp; };
static std::map<std::string, RamAsset> g_ramAssets;
static size_t g_ramBytes = 0;
static uint64_t g_ramStamp = 0;
static const size_t kRamAssetBudget = 256u << 20;

static void RamLock()   { if (!g_ramLockReady) { InitializeCriticalSection(&g_ramLock); g_ramLockReady = true; } EnterCriticalSection(&g_ramLock); }
static void RamUnlock() { LeaveCriticalSection(&g_ramLock); }

static bool RamAssetGet(const std::string& key, std::vector<unsigned char>& out) {
    RamLock();
    auto it = g_ramAssets.find(key);
    const bool hit = it != g_ramAssets.end();
    if (hit) { out = it->second.bytes; it->second.stamp = ++g_ramStamp; }
    RamUnlock();
    return hit;
}

static bool RamAssetHas(const std::string& key) {
    RamLock();
    const bool hit = g_ramAssets.find(key) != g_ramAssets.end();
    RamUnlock();
    return hit;
}

static void RamAssetPut(const std::string& key, std::vector<unsigned char>&& bytes) {
    RamLock();
    auto old = g_ramAssets.find(key);
    if (old != g_ramAssets.end()) { g_ramBytes -= old->second.bytes.size(); g_ramAssets.erase(old); }
    while (g_ramBytes + bytes.size() > kRamAssetBudget && !g_ramAssets.empty()) {
        auto victim = g_ramAssets.begin();
        for (auto it = g_ramAssets.begin(); it != g_ramAssets.end(); ++it)
            if (it->second.stamp < victim->second.stamp) victim = it;
        g_ramBytes -= victim->second.bytes.size();
        g_ramAssets.erase(victim);
    }
    g_ramBytes += bytes.size();
    RamAsset& a = g_ramAssets[key];
    a.bytes = std::move(bytes);
    a.stamp = ++g_ramStamp;
    RamUnlock();
}

// Read a just-rendered file into the store and remove it from disk.
static bool AbsorbFileToRam(const std::string& key, const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
    fclose(f);
    if (bytes.empty()) return false;
    DeleteFileA(path.c_str());
    if (out) *out = bytes;
    RamAssetPut(key, std::move(bytes));
    return true;
}

static const char* VisualRoleFor(const std::string& request) {
    std::string low = lowerPathLocal(request);
    return low.find("resultdisc") != std::string::npos ? "resultdisc"
         : low.find("discsmall") != std::string::npos ? "discsmall"
         : low.find("eyecatch") != std::string::npos ? "eyecatch"
         : low.find("songname") != std::string::npos ? "songname"
         : low.find("disc") != std::string::npos ? "disc" : nullptr;
}

bool SongVisualAssetReady(const CustomBmsSong& song, const char* role) {
    if (!role || song.cacheDir.empty()) return false;
    if (Config().assetsInMemory && RamAssetHas(song.id + "|" + role)) return true;
    return fileExistsLocal(JoinPath(JoinPath(song.cacheDir, role), song.id + ".abm"));
}

bool SongVisualAssetBytes(const CustomBmsSong& song, const std::string& request, std::vector<unsigned char>& out) {
    const char* role = VisualRoleFor(request);
    if (!role || song.cacheDir.empty()) return false;
    const std::string key = song.id + "|" + role;
    if (RamAssetGet(key, out)) return true;
    const std::string file = JoinPath(JoinPath(song.cacheDir, role), song.id + ".abm");
    if (fileExistsLocal(file)) return AbsorbFileToRam(key, file, &out);
    // Not rendered yet. The big disc the game reloads at every cursor stop must not wait for
    // the art thread (that wait was a hitch of up to a whole category's render): while the
    // renderer is busy the game gets a blank 256x256 tile now and the real one on the next
    // focus, and the song goes to the front of the queue. Any other role renders here.
    if (strcmp(role, "disc") == 0 && Config().artPrefetch) {
        VisualGenLockInit();
        if (!TryEnterCriticalSection(&g_visualGenLock)) {
            ArtPrefetchPrioritize(std::vector<std::string>(1, song.id));
            return RenderSplashBytes(out, std::string(), std::string(), 256, 256, true, false) && AbmFromBmpBytes(out);
        }
        LeaveCriticalSection(&g_visualGenLock);
    }
    const std::string path = EnsureSongVisualAsset(song, request);   // renders when missing
    if (path.empty()) return false;
    return AbsorbFileToRam(key, path, &out);
}

bool SongPreviewBytes(const CustomBmsSong& song, std::vector<unsigned char>& out) {
    if (song.cacheDir.empty()) return false;
    const std::string key = song.id + "|ssf";
    if (RamAssetGet(key, out)) return true;
    EnsureSongPreview(song);
    return AbsorbFileToRam(key, JoinPath(song.cacheDir, song.id + ".ssf"), &out);
}

// The preview is opened by the game's CRT fopen (openSample), which cannot take a memory
// handle, so in the RAM mode it is written back to disk for that one open -- the previous one
// deleted first, so at most one .ssf sits in the cache at a time.
std::string SongPreviewFileForCrt(const CustomBmsSong& song) {
    static std::string s_last;
    if (song.cacheDir.empty()) return std::string();
    const std::string path = JoinPath(song.cacheDir, song.id + ".ssf");
    if (fileExistsLocal(path)) return path;
    std::vector<unsigned char> bytes;
    if (!SongPreviewBytes(song, bytes)) return std::string();
    if (!s_last.empty() && s_last != path) DeleteFileA(s_last.c_str());
    ensureDir(song.cacheDir);
    if (!writeBinaryFile(path, bytes)) return std::string();
    s_last = path;
    return path;
}

std::string EnsureSongVisualAsset(const CustomBmsSong& song, const std::string& request) {
    std::string low = lowerPathLocal(request);
    const char* role = low.find("resultdisc") != std::string::npos ? "resultdisc"
                     : low.find("discsmall") != std::string::npos ? "discsmall"
                     : low.find("eyecatch") != std::string::npos ? "eyecatch"
                     : low.find("songname") != std::string::npos ? "songname"
                     : low.find("disc") != std::string::npos ? "disc" : nullptr;
    if (!role || song.cacheDir.empty()) return std::string();
    std::string path = JoinPath(JoinPath(song.cacheDir, role), song.id + ".abm");
    if (fileExistsLocal(path)) return path;
    GenerateVisualAssetCache(song, role);
    return fileExistsLocal(path) ? path : std::string();
}

static void BuildCacheManifest(CustomBmsSong& song) {
    std::string cacheRoot = JoinPath(GetExeDirectory(), Config().cacheRoot);
    ensureDir(cacheRoot);
    ensureDir(JoinPath(cacheRoot, song.mode));
    song.cacheDir = JoinPath(JoinPath(cacheRoot, song.mode), song.id);
    ensureDir(song.cacheDir);

    std::string manifest;
    manifest += "# EZ2CustomMod v21 cache manifest\r\n";
    manifest += "# Runtime bridge cache. Original song.bin and Stage.ini are not modified; virtual rows are injected in memory.\r\n";
    manifest += "id=" + song.id + "\r\n";
    manifest += "mode=" + song.mode + "\r\n";
    manifest += "category=" + song.category + "\r\n";
    manifest += "source=" + song.songDir + "\r\n";
    manifest += "title=" + song.title + "\r\n";
    manifest += "artist=" + song.artist + "\r\n";
    manifest += "charts=" + std::to_string(song.charts.size()) + "\r\n";
    for (size_t i=0;i<song.chartMeta.size();++i) {
        const auto& c = song.chartMeta[i];
        manifest += "chart" + std::to_string(i+1) + "=" + c.file + "|slot=" + std::to_string(c.slot) + "|diff=" + c.difficulty + "|level=" + c.playLevel + "|bpm=" + c.bpm + "\r\n";
    }
    manifest += "audioFiles=" + std::to_string(song.audioFiles.size()) + "\r\n";
    manifest += "imageFiles=" + std::to_string(song.imageFiles.size()) + "\r\n";
    manifest += "videoFiles=" + std::to_string(song.videoFiles.size()) + "\r\n";
    manifest += "songbinLevelNM=" + std::to_string(song.levelNM) + "\r\n";
    manifest += "songbinLevelHD=" + std::to_string(song.levelHD) + "\r\n";
    manifest += "songbinLevelSHD=" + std::to_string(song.levelSHD) + "\r\n";
    manifest += "songbinLevelEX=" + std::to_string(song.levelEX) + "\r\n";
    manifest += "songbinBpmNM=" + std::to_string(song.bpmNM) + "\r\n";
    manifest += "songbinBpmHD=" + std::to_string(song.bpmHD) + "\r\n";
    manifest += "songbinBpmSHD=" + std::to_string(song.bpmSHD) + "\r\n";
    manifest += "songbinBpmEX=" + std::to_string(song.bpmEX) + "\r\n";
    manifest += "\r\n# v10.3: virtual song.bin difficulty blocks are populated from each compatible BMS #PLAYLEVEL.\r\n";
    writeTextFile(JoinPath(song.cacheDir, "manifest.txt"), manifest);

    std::string ini;
    ini += "[General]\r\n";
    ini += "SongOriginalName=" + song.title + "\r\n";
    ini += "Artist=" + song.artist + "\r\n";
    ini += "BMSDataSource=" + song.songDir + "\r\n";
    ini += "Mode=" + song.mode + "\r\n";
    ini += "Category=" + song.category + "\r\n";
    ini += "\r\n[JudgmentDelta]\r\nCool=6\r\nGood=24\r\nMiss=36\r\nFail=72\r\n";
    writeTextFile(JoinPath(song.cacheDir, song.id + ".ini.planned"), ini);
    GenerateVisualAssetCache(song);
}


static std::string g_virtualStageMode;
static std::string g_virtualStagePath;

// A category is independent of any song: generate one ABM per (mode, category) into
// CustomCache\<mode>\_categories\category_NN.abm. NN = 48 + index, matching the slot
// name the game requests (native categories occupy 0..46, custom start at 47).
// Defined below, next to the details.txt parser they belong to.
static bool ReadCategoryDetails(const std::string& mode, const std::string& category,
                                std::string& categoryText, std::string& subText);



static bool ReadCategoryDetails(const std::string& mode, const std::string& category,
                                std::string& categoryText, std::string& subText) {
    for (const auto& s : g_songs) {
        if (_stricmp(s.mode.c_str(), mode.c_str()) != 0) continue;
        if (_stricmp(s.category.c_str(), category.c_str()) != 0) continue;
        // details.txt sits in the CATEGORY folder (BMSData/<mode>/<category>/details.txt), one level
        // above the song. Looking in songDir never found it, so every category silently fell back to
        // its folder name -- which is why the splash read "CYT-1" instead of "Cytus 1 (2012)".
        // Derived from songDir rather than rebuilt from the mode: a virtual copy carries a target
        // mode with no BMSData folder of its own, but its songDir still points at the real source.
        std::string categoryDir = s.songDir;
        size_t slash = categoryDir.find_last_of("\\/");
        if (slash != std::string::npos) categoryDir.erase(slash);
        FILE* f = fopen(JoinPath(categoryDir, "details.txt").c_str(), "rb");
        if (!f) f = fopen(JoinPath(s.songDir, "details.txt").c_str(), "rb"); // tolerate per-song files
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string t = trim(line);
            std::string u = t; std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c){ return (char)toupper(c); });
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string val = trim(t.substr(eq + 1));
            if (u.rfind("CATEGORY", 0) == 0) categoryText = val;
            else if (u.rfind("SUBTEXT", 0) == 0) subText = val;
        }
        fclose(f);
        return !categoryText.empty() || !subText.empty();
    }
    return false;
}

static void WriteVirtualStageIniForMode(const std::string& mode) {
    std::vector<CustomBmsSong> songs;
    for (const auto& s : g_songs) if (_stricmp(s.mode.c_str(), mode.c_str()) == 0) songs.push_back(s);
    if (songs.empty()) return;

    std::vector<std::string> categories;
    for (const auto& s : songs) {
        bool exists = false;
        for (const auto& c : categories) if (_stricmp(c.c_str(), s.category.c_str()) == 0) { exists = true; break; }
        if (!exists) categories.push_back(s.category);
    }
    std::string text;
    text += "[Stage]\r\n";
    text += "HID-HD0=";
    for (const auto& c : categories) text += c + ";";
    text += "\r\n";
    // The native Stage.ini/song.bin parser copies each category's ids into a FIXED 436-slot table
    // (stride 0x1b4); listing more here overruns into game state globals → crash (CONFIRMED via HW
    // write-watch: the parser's id-copy loop @rva 0x69e3e clobbered DAT_037dcecc with a song id).
    // So cap EVERY list at 436. ponytail: lift once the category id-table is relocated to a bigger
    // buffer that the parser writes too (not just the filter read).
    const size_t kCatCap = 436;
    text += "ALLSONG=";
    { size_t n = 0; for (const auto& s : songs) { if (n++ >= kCatCap) break; text += s.id + ";"; } }
    text += "\r\n";
    for (size_t ci = 0; ci < categories.size(); ++ci) {
        text += "HID" + std::to_string((unsigned long long)ci) + "=";
        size_t n = 0;
        for (const auto& s : songs)
            if (_stricmp(s.category.c_str(), categories[ci].c_str()) == 0) { if (n++ >= kCatCap) break; text += s.id + ";"; }
        text += "\r\n";
    }
    std::string dir = JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), mode);
    CreateDirectoryA(JoinPath(GetExeDirectory(), Config().cacheRoot).c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string plainPath = JoinPath(dir, "virtual_Stage.ini.plain.txt");
    std::string encPath = JoinPath(dir, "virtual_Stage.ini");
    writeTextFile(plainPath, text);
    FnexWriteEncryptedTextFile(encPath, text, FnexCryptoKind::Ini);
    g_virtualStageMode = mode;
    g_virtualStagePath = encPath;
    LogLine("[category] virtual encrypted Stage.ini mode='%s' categories=%d songs=%d path='%s'", mode.c_str(), (int)categories.size(), (int)songs.size(), encPath.c_str());
}

std::string GetVirtualStageIniPathForMode(const std::string& mode) {
    std::string n = NormalizeModeName(mode);
    if (!g_virtualStagePath.empty() && _stricmp(g_virtualStageMode.c_str(), n.c_str()) == 0) return g_virtualStagePath;
    return std::string();
}

// Persistent song-id map so a song's id (and thus its CustomCache\<mode>\<id> dir) is STABLE across
// runs and across adding/removing folders. Without this the id was the scan-order index, so inserting
// a folder shifted every later id → their caches mismatched → full re-conversion. Keyed on the stable
// relative path (mode/category/folder). File: CustomCache\idmap.txt ("id|key" per line).
static std::map<std::string, uint32_t> g_idMap;
static uint32_t g_idNext = 1;
static bool g_idMapLoaded = false;
static std::string g_idMapPath;
static void LoadIdMap() {
    if (g_idMapLoaded) return;
    g_idMapLoaded = true;
    g_idMapPath = JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), "idmap.txt");
    FILE* f = fopen(g_idMapPath.c_str(), "rb");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char* bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        uint32_t id = (uint32_t)strtoul(line, nullptr, 10);
        std::string key = bar + 1;
        while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
        if (id && !key.empty()) { g_idMap[key] = id; if (id >= g_idNext) g_idNext = id + 1; }
    }
    fclose(f);
}
static uint32_t GetStableSongId(const std::string& key) {
    LoadIdMap();
    auto it = g_idMap.find(key);
    if (it != g_idMap.end()) return it->second;
    uint32_t id = g_idNext++;
    g_idMap[key] = id;
    return id;
}
static void SaveIdMap() {
    if (g_idMapPath.empty()) return;
    ensureDir(JoinPath(GetExeDirectory(), Config().cacheRoot));
    FILE* f = fopen(g_idMapPath.c_str(), "wb");
    if (!f) return;
    for (const auto& kv : g_idMap) fprintf(f, "%lu|%s\n", (unsigned long)kv.second, kv.first.c_str());
    fclose(f);
}


static std::vector<std::string> configuredAutoTargetModes() {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&]() {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.erase(token.begin());
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
        std::string mode = NormalizeModeName(token);
        if (!mode.empty()) {
            bool exists = false;
            for (const auto& x : out) if (_stricmp(x.c_str(), mode.c_str()) == 0) { exists = true; break; }
            if (!exists) out.push_back(mode);
        }
        token.clear();
    };
    for (char c : Config().autoGenerateTargetModes) {
        if (c == ',' || c == ';' || c == '|') flush();
        else token.push_back(c);
    }
    flush();
    return out;
}

static int autoModeLevelBonus(const std::string& mode) {
    std::string m = NormalizeModeName(mode);
    if (m == "5keyMix" || m == "streetMix" || m == "turnTable") return 1;
    if (m == "7keyMix" || m == "cv2Mix") return 2;
    if (m == "clubMix") return 3;
    if (m == "spaceMix") return 4;
    return 0;
}

// One catalog key: mode + category + title + artist, case-folded, NUL-separated so no pair of
// fields can run together into a false match.
static std::string catalogKey(const std::string& mode, const std::string& category,
                              const std::string& title, const std::string& artist) {
    std::string k;
    k.reserve(mode.size() + category.size() + title.size() + artist.size() + 4);
    for (const std::string* part : {&mode, &category, &title, &artist}) {
        for (char c : *part) k += (char)tolower((unsigned char)c);
        k += '\0';
    }
    return k;
}

// This was a full linear scan of g_songs, run once per (source song x target mode) pair. At 19271
// songs and 8 targets that is 154168 x 19271 = ~3 BILLION _stricmp calls, which is why startup sat
// at "100%" for minutes with the catalog apparently frozen -- it was not hung, it was counting.
// Being quadratic, a 45% bigger library cost 110% more time.
//
// g_songs does not change while generation runs (new songs land in a separate vector and are
// appended afterwards), so the catalog is keyed once up front and each check is a hash lookup.
static std::unordered_set<std::string> g_catalogKeys;

static void buildCatalogKeys() {
    g_catalogKeys.clear();
    g_catalogKeys.reserve(g_songs.size() * 2);
    for (const auto& s : g_songs)
        g_catalogKeys.insert(catalogKey(s.mode, s.category, s.title, s.artist));
}

static bool catalogHasEquivalentSong(const std::string& mode, const CustomBmsSong& source) {
    return g_catalogKeys.count(catalogKey(mode, source.category, source.title, source.artist)) != 0;
}

static void ExpandAutoGeneratedModes() {
    if (!Config().autoGenerateModes) return;
    std::string sourceMode = NormalizeModeName(Config().autoGenerateSourceMode);
    if (sourceMode.empty()) sourceMode = "5keyOnly";
    std::vector<std::string> targets = configuredAutoTargetModes();
    if (targets.empty()) return;

    auto collectFor = [](const std::string& mode) {
        std::vector<CustomBmsSong> out;
        for (const auto& song : g_songs)
            if (_stricmp(song.mode.c_str(), mode.c_str()) == 0 && !song.autoGenerated) out.push_back(song);
        return out;
    };

    std::vector<CustomBmsSong> sources = collectFor(sourceMode);
    // The configured SourceMode may simply not exist on a real install: most libraries keep every
    // chart in BMSData\5keyMix and leave the other mode folders empty. Generating nothing is NOT a
    // safe no-op — InstallCustomCategoryPatches extends the wheel to 47+N categories GLOBALLY, so a
    // mode left with no custom songs still has those categories navigable, they resolve to zero
    // visible entries, and the game divides by that count (STATUS_INTEGER_DIVIDE_BY_ZERO, observed
    // at 0x441911 on entering SpaceMix). Fall back to the scanned mode that actually has charts.
    if (sources.empty()) {
        std::vector<std::string> scanned;
        for (const auto& song : g_songs) {
            if (song.autoGenerated) continue;
            bool seen = false;
            for (const auto& mode : scanned) if (_stricmp(mode.c_str(), song.mode.c_str()) == 0) { seen = true; break; }
            if (!seen) scanned.push_back(song.mode);
        }
        std::string best;
        for (const auto& mode : scanned) {
            std::vector<CustomBmsSong> cur = collectFor(mode);
            if (cur.size() > sources.size()) { sources.swap(cur); best = mode; }
        }
        if (!best.empty()) {
            LogLine("[autochart] configured sourceMode='%s' has no songs; using scanned mode='%s' songs=%lu",
                    sourceMode.c_str(), best.c_str(), (unsigned long)sources.size());
            sourceMode = best;
        }
    }
    if (sources.empty()) {
        LogLine("[autochart] no source songs for mode='%s'; no virtual modes generated", sourceMode.c_str());
        return;
    }

    buildCatalogKeys();   // must precede the loop; catalogHasEquivalentSong reads it
    std::vector<CustomBmsSong> generated;
    for (const auto& source : sources) {
        std::string folder = fileNameOnlyLocal(source.songDir);
        for (const auto& target : targets) {
            if (_stricmp(target.c_str(), sourceMode.c_str()) == 0) continue;
            if (catalogHasEquivalentSong(target, source)) continue; // explicit target-mode chart wins
            CustomBmsSong song = source;
            song.mode = target;
            song.modeFolder = target;
            song.autoGenerated = true;
            song.sourceMode = sourceMode;
            uint32_t idn = GetStableSongId("autogen/" + target + "/" + sourceMode + "/" + source.category + "/" + folder);
            char idbuf[32] = {};
            wsprintfA(idbuf, "bms%06lu", static_cast<unsigned long>(idn));
            song.id = idbuf;
            int bonus = autoModeLevelBonus(target);
            int* levels[] = {&song.levelNM, &song.levelHD, &song.levelSHD, &song.levelEX, &song.levelEZ};
            for (int* level : levels) if (*level > 0) { *level += bonus; if (*level > 18) *level = 18; }
            song.cacheDir = JoinPath(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), target), song.id);
            if (!g_directBmsEnabled) {
                BuildCacheManifest(song);
                GeneratePlayableCacheV10(song, /*materializeAudio=*/false);
            }
            generated.push_back(song);
            // One line per generated song was ~19MB of log per startup (142630 lines at 19271
            // songs) and it buried every later diagnostic. A sample is enough to confirm shape.
            static unsigned long s_sampled = 0;
            if (s_sampled++ < 200)
            LogLine("[autochart] virtual song id='%s' sourceId='%s' sourceMode='%s' targetMode='%s' title='%s' category='%s'",
                song.id.c_str(), source.id.c_str(), sourceMode.c_str(), target.c_str(), song.title.c_str(), song.category.c_str());
        }
    }
    g_songs.insert(g_songs.end(), generated.begin(), generated.end());
    LogLine("[autochart] expanded sourceSongs=%lu targets=%lu generated=%lu totalCatalog=%lu",
        static_cast<unsigned long>(sources.size()), static_cast<unsigned long>(targets.size()),
        static_cast<unsigned long>(generated.size()), static_cast<unsigned long>(g_songs.size()));
}


// ---- scan cache ---------------------------------------------------------------------------------
//
// Every launch re-opened and re-parsed every .bms in the library to recover the same title, artist,
// BPM and levels it recovered last time: 13-26 seconds at this size, thrown away on exit. The
// manifest written next to each cached song was never read back by anything.
//
// So the parse result is stored once and reloaded. Invalidation is per song and comes free: the
// directory walk that finds the folders already carries each one's last-write time in its
// WIN32_FIND_DATA, so a song whose folder has not changed since it was cached is taken from the
// cache without opening a single file, and one that has changed is parsed exactly as before.
//
// Only SCANNED songs are stored. The virtual per-mode copies are rebuilt by
// ExpandAutoGeneratedModes, which is a hash lookup per song now and costs nothing to redo.
//
// The format is length-prefixed binary rather than text: song titles contain tabs, quotes and
// newlines often enough that any delimiter needs escaping, and a wrong escape here silently
// corrupts a song's metadata instead of failing loudly.
static const char kScanCacheMagic[8] = { 'E','Z','2','S','C','A','N','2' };

struct ScanCacheEntry {
    uint64_t mtime = 0;
    CustomBmsSong song;
};
static std::map<std::string, ScanCacheEntry> g_scanCache;   // keyed by songDir, lowercased
static bool g_scanCacheLoaded = false;
static unsigned long g_scanCacheHits = 0, g_scanCacheMisses = 0;

static std::string scanCacheKey(const std::string& songDir) {
    std::string k = songDir;
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return k;
}

static std::string scanCachePath() {
    return JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), "scan.cache");
}

// ---- writing -------------------------------------------------------------------------------
static void cachePutStr(std::vector<unsigned char>& b, const std::string& s) {
    uint32_t n = (uint32_t)s.size();
    b.insert(b.end(), (unsigned char*)&n, (unsigned char*)&n + 4);
    b.insert(b.end(), s.begin(), s.end());
}
static void cachePutU32(std::vector<unsigned char>& b, uint32_t v) {
    b.insert(b.end(), (unsigned char*)&v, (unsigned char*)&v + 4);
}
static void cachePutF32(std::vector<unsigned char>& b, float v) {
    b.insert(b.end(), (unsigned char*)&v, (unsigned char*)&v + 4);
}
static void cachePutVec(std::vector<unsigned char>& b, const std::vector<std::string>& v) {
    cachePutU32(b, (uint32_t)v.size());
    for (const auto& s : v) cachePutStr(b, s);
}

// ---- reading -------------------------------------------------------------------------------
struct CacheReader {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;
    bool need(size_t n) { if ((size_t)(end - p) < n) { ok = false; return false; } return true; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
    float    f32() { if (!need(4)) return 0.f; float v; memcpy(&v, p, 4); p += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v; memcpy(&v, p, 8); p += 8; return v; }
    std::string str() {
        uint32_t n = u32();
        if (!ok || n > (1u << 20) || !need(n)) { ok = false; return std::string(); }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    std::vector<std::string> vec() {
        std::vector<std::string> v;
        uint32_t n = u32();
        if (!ok || n > (1u << 20)) { ok = false; return v; }
        v.reserve(n);
        for (uint32_t i = 0; i < n && ok; ++i) v.push_back(str());
        return v;
    }
};

static void LoadScanCache() {
    if (g_scanCacheLoaded) return;
    g_scanCacheLoaded = true;
    if (!Config().scanCache) return;

    FILE* f = fopen(scanCachePath().c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf;
    if (sz > 8) {
        buf.resize((size_t)sz);
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
    }
    fclose(f);
    if (buf.size() < 12 || memcmp(buf.data(), kScanCacheMagic, 8) != 0) {
        LogLine("[scancache] absent or from another version; a full parse will rebuild it");
        return;
    }

    CacheReader r{ buf.data() + 8, buf.data() + buf.size() };
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        ScanCacheEntry e;
        e.mtime = r.u64();
        CustomBmsSong& s = e.song;
        s.id = r.str(); s.mode = r.str(); s.modeFolder = r.str(); s.category = r.str();
        s.songDir = r.str(); s.cacheDir = r.str(); s.title = r.str(); s.artist = r.str();
        s.charts.mut()     = r.vec();
        s.audioFiles.mut() = r.vec();
        s.imageFiles.mut() = r.vec();
        s.videoFiles.mut() = r.vec();
        uint32_t nMeta = r.u32();
        if (nMeta > (1u << 20)) { r.ok = false; break; }
        s.chartMeta.mut().clear();
        for (uint32_t m = 0; m < nMeta && r.ok; ++m) {
            CustomBmsChart c;
            c.file = r.str(); c.title = r.str(); c.artist = r.str();
            c.stageFile = r.str(); c.banner = r.str(); c.bpm = r.str();
            c.playLevel = r.str(); c.difficulty = r.str();
            c.slot = (int)r.u32();
            s.chartMeta.mut().push_back(c);
        }
        s.levelNM = (int)r.u32(); s.levelHD = (int)r.u32();
        s.levelSHD = (int)r.u32(); s.levelEX = (int)r.u32();
        s.bpmNM = r.f32(); s.bpmHD = r.f32(); s.bpmSHD = r.f32(); s.bpmEX = r.f32();
        if (r.ok) g_scanCache[scanCacheKey(s.songDir)] = e;
    }
    if (!r.ok) {
        // A truncated or damaged cache is dropped whole rather than trusted in part: half a
        // catalog is worse than none, and rebuilding costs one slow launch.
        LogLine("[scancache] damaged after %lu entries; discarded", (unsigned long)g_scanCache.size());
        g_scanCache.clear();
        return;
    }
    LogLine("[scancache] loaded %lu songs", (unsigned long)g_scanCache.size());
}

static void SaveScanCache() {
    if (!Config().scanCache) return;
    std::vector<unsigned char> b;
    b.insert(b.end(), kScanCacheMagic, kScanCacheMagic + 8);
    uint32_t count = 0;
    for (const auto& s : g_songs) if (!s.autoGenerated) ++count;
    cachePutU32(b, count);
    for (const auto& s : g_songs) {
        if (s.autoGenerated) continue;                 // rebuilt by ExpandAutoGeneratedModes
        uint64_t mt = 0;
        auto it = g_scanCache.find(scanCacheKey(s.songDir));
        if (it != g_scanCache.end()) mt = it->second.mtime;
        else                         mt = fileMTimeLocal(s.songDir);
        b.insert(b.end(), (unsigned char*)&mt, (unsigned char*)&mt + 8);
        cachePutStr(b, s.id); cachePutStr(b, s.mode); cachePutStr(b, s.modeFolder);
        cachePutStr(b, s.category); cachePutStr(b, s.songDir); cachePutStr(b, s.cacheDir);
        cachePutStr(b, s.title); cachePutStr(b, s.artist);
        cachePutVec(b, s.charts);      cachePutVec(b, s.audioFiles);
        cachePutVec(b, s.imageFiles);  cachePutVec(b, s.videoFiles);
        cachePutU32(b, (uint32_t)s.chartMeta.size());
        for (const auto& c : s.chartMeta) {
            cachePutStr(b, c.file); cachePutStr(b, c.title); cachePutStr(b, c.artist);
            cachePutStr(b, c.stageFile); cachePutStr(b, c.banner); cachePutStr(b, c.bpm);
            cachePutStr(b, c.playLevel); cachePutStr(b, c.difficulty);
            cachePutU32(b, (uint32_t)c.slot);
        }
        cachePutU32(b, (uint32_t)s.levelNM); cachePutU32(b, (uint32_t)s.levelHD);
        cachePutU32(b, (uint32_t)s.levelSHD); cachePutU32(b, (uint32_t)s.levelEX);
        cachePutF32(b, s.bpmNM); cachePutF32(b, s.bpmHD);
        cachePutF32(b, s.bpmSHD); cachePutF32(b, s.bpmEX);
    }
    ensureDir(JoinPath(GetExeDirectory(), Config().cacheRoot));
    // Written to a temporary and renamed, so a crash mid-write cannot leave a half file that the
    // next launch would have to detect as damaged.
    const std::string finalPath = scanCachePath();
    const std::string tmpPath = finalPath + ".tmp";
    if (writeBinaryFile(tmpPath, b)) {
        DeleteFileA(finalPath.c_str());
        if (!MoveFileA(tmpPath.c_str(), finalPath.c_str())) DeleteFileA(tmpPath.c_str());
    }
    LogLine("[scancache] saved %lu songs (%.1f MB), %lu reused / %lu parsed this run",
            (unsigned long)count, b.size() / 1048576.0, g_scanCacheHits, g_scanCacheMisses);
}

static void scanFlatSongFolder(const std::string& mode, const std::string& modeFolder, const std::string& category, const std::string& songDir, const std::string& folderName) {
    // Unchanged folder: take last run's parse and open nothing. The metadata below is exactly
    // what the parse produces, so a hit and a miss are indistinguishable downstream.
    {
        auto it = g_scanCache.find(scanCacheKey(songDir));
        if (it != g_scanCache.end() && it->second.mtime && it->second.mtime == fileMTimeLocal(songDir)) {
            // Slots and levels are derived again from the cached chart list rather than trusted:
            // the slot rule has changed since caches were written (the EZ column), and it costs
            // a sort of at most a handful of charts.
            CustomBmsSong cached = it->second.song;
            AssignSongBinDifficultyFields(cached);
            FinalizeSongBinDifficultyFields(cached);
            g_songs.push_back(cached);
            ++g_scanCacheHits;
            return;
        }
    }
    ++g_scanCacheMisses;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(JoinPath(songDir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    CustomBmsSong song;
    song.mode = mode;
    song.modeFolder = modeFolder;
    song.category = category;
    song.songDir = songDir;
    song.title = folderName;
    song.artist = "BMSData";

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string file = fd.cFileName;
        if (endsWithBms(file)) {
            CustomBmsChart meta;
            meta.file = file;
            meta.difficulty = inferDifficulty(file);
            parseBmsMeta(JoinPath(songDir, file), meta);
            song.charts.push_back(file);
            song.chartMeta.push_back(meta);
            bool compatible = IsLikelyChartForMode(file, mode);
            if ((compatible && (song.title == folderName || song.artist == "BMSData")) || song.charts.size() == 1) {
                if (compatible && !meta.title.empty()) song.title = meta.title;
                if (compatible && !meta.artist.empty()) song.artist = meta.artist;
            }
            if (Config().traceBmsScan) LogLine("[bms] mode='%s' category='%s' songDir='%s' chart='%s' diff='%s' level='%s' compatible=%d",
                mode.c_str(), category.c_str(), songDir.c_str(), file.c_str(), meta.difficulty.c_str(), meta.playLevel.c_str(), compatible ? 1 : 0);
        } else if (endsWithAudio(file)) song.audioFiles.push_back(file);
        else if (endsWithImage(file)) song.imageFiles.push_back(file);
        else if (endsWithVideo(file)) song.videoFiles.push_back(file);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (!song.charts.empty()) {
        char idbuf[32] = {};
        // STABLE id keyed on the song's path (mode/category/folder), persisted in idmap.txt — so adding
        // folders never shifts existing ids and the cache survives across runs/DLL/EXE changes.
        uint32_t idn = GetStableSongId(mode + "/" + category + "/" + folderName);
        wsprintfA(idbuf, "bms%06lu", static_cast<unsigned long>(idn));
        song.id = idbuf;
        AssignSongBinDifficultyFields(song);
        for (const auto& chart : song.chartMeta) {
            if (chart.slot != 0) continue;
            if (!chart.title.empty()) song.title = chart.title;
            if (!chart.artist.empty()) song.artist = chart.artist;
            break;
        }
        FinalizeSongBinDifficultyFields(song);
        song.title = stripDifficultyTag(song.title); // songname is per-song: drop "[EASY]" etc.
        for (const auto& chart : song.chartMeta) {
            // One line per chart: 64262 of them at this library size. Gated like its neighbours,
            // with a sample kept so the shape is still visible without TraceBMSScan.
            static unsigned long s_assigned = 0;
            if (Config().traceBmsScan || s_assigned++ < 100)
            LogLine("[bms] assigned id='%s' chart='%s' slot=%d suffix='%s' playLevel='%s'",
                    song.id.c_str(), chart.file.c_str(), chart.slot, suffixForSlot(chart.slot), chart.playLevel.c_str());
        }
        if (g_directBmsEnabled) {
            // Path only: directories/files are created lazily by the first real asset request.
            song.cacheDir = JoinPath(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), song.mode), song.id);
        } else {
            // Executable profile mismatch: retain the proven disk bridge as a safe fallback.
            BuildCacheManifest(song);
            GeneratePlayableCacheV10(song, /*materializeAudio=*/false);
        }
        // Direct bridge keeps selector metadata in memory. Chart events and PCM are
        // materialized only inside the game's objects when the song starts.
        g_songs.push_back(song);
        // Likewise, one per song: 19271 lines. The totals are logged once at the end.
        static unsigned long s_found = 0;
        if (Config().traceBmsScan || s_found++ < 100)
        LogLine("[bms] found song id='%s' mode='%s' category='%s' title='%s' artist='%s' charts=%lu audio=%lu images=%lu videos=%lu levelNM=%d levelHD=%d levelSHD=%d levelEX=%d cache='%s'",
            song.id.c_str(), song.mode.c_str(), song.category.c_str(), song.title.c_str(), song.artist.c_str(),
            static_cast<unsigned long>(song.charts.size()), static_cast<unsigned long>(song.audioFiles.size()),
            static_cast<unsigned long>(song.imageFiles.size()), static_cast<unsigned long>(song.videoFiles.size()),
            song.levelNM, song.levelHD, song.levelSHD, song.levelEX, song.cacheDir.c_str());
        // One popup line per fully-processed song + drive the real progress bar.
        StartupProgress((int)g_songs.size(), g_scanTotal, (song.title + " [" + song.category + "]").c_str());
    }
}

// Cheap pre-pass: count 3rd-level dirs (mode/category/song) so the popup bar has a real total.
// Dir enumeration only, no parsing. Slight overcount (non-song folders) is fine for a bar.
static int CountSongFolders(const std::string& root) {
    int n = 0;
    WIN32_FIND_DATAA mf{};
    HANDLE hm = FindFirstFileA(JoinPath(root, "*").c_str(), &mf);
    if (hm == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(mf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(mf.cFileName, ".") || !strcmp(mf.cFileName, "..")) continue;
        if (NormalizeModeName(mf.cFileName).empty()) continue;
        std::string mp = JoinPath(root, mf.cFileName);
        WIN32_FIND_DATAA cf{};
        HANDLE hc = FindFirstFileA(JoinPath(mp, "*").c_str(), &cf);
        if (hc == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(cf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!strcmp(cf.cFileName, ".") || !strcmp(cf.cFileName, "..")) continue;
            std::string cp = JoinPath(mp, cf.cFileName);
            WIN32_FIND_DATAA sf{};
            HANDLE hs = FindFirstFileA(JoinPath(cp, "*").c_str(), &sf);
            if (hs == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (!strcmp(sf.cFileName, ".") || !strcmp(sf.cFileName, "..")) continue;
                ++n;
            } while (FindNextFileA(hs, &sf));
            FindClose(hs);
        } while (FindNextFileA(hc, &cf));
        FindClose(hc);
    } while (FindNextFileA(hm, &mf));
    FindClose(hm);
    return n;
}

// Legacy random BGA pool: shared clips in Video\_spv, selected by the quartz IAT redirect.
// PrepareBgaPool ONLY builds the shared clip pool (bg\*.mp4 -> Video\_spv\*.spv). NO per-song files:
// songs with no BGA get a random pool clip at PLAY TIME via the quartz.dll CreateFileW hook (mod_main
// HookQuartzCreateFileW) — DirectShow opens Video\<id>\<id>.spv through quartz's own CreateFileW, which
// we redirect dynamically to a random Video\_spv\ clip. 0 extra disk, chosen when the song is played.
static void PrepareBgaPool() {
    std::string videoDir = JoinPath(GetExeDirectory(), "Video");
    std::string spvDir = JoinPath(videoDir, "_spv");
    ensureDir(videoDir);
    ensureDir(spvDir);
    std::string bgDir = JoinPath(GetExeDirectory(), "bg");
    int made = 0, have = 0;
    if (fileExistsLocal(bgDir) || GetFileAttributesA(bgDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(JoinPath(bgDir, "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string f = fd.cFileName;
                if (!endsWithVideo(f)) continue;
                std::string spv = JoinPath(spvDir, stripExtensionLocal(f) + ".spv");
                if (!fileExistsLocal(spv)) { if (ConvertVideoToSpv(JoinPath(bgDir, f), spv, 0.0, /*cap=*/90)) made++; }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    WaitAllVideoJobs();
    { WIN32_FIND_DATAA pf{}; HANDLE ph = FindFirstFileA(JoinPath(spvDir, "*.spv").c_str(), &pf);
      if (ph != INVALID_HANDLE_VALUE) { do { if (!(pf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) have++; } while (FindNextFileA(ph, &pf)); FindClose(ph); } }
    // One shared default .ini (FNEX-encrypted) for the pool; the CreateFileA hook redirects each no-BGA
    // song's Video\<id>\<id>.ini here so DirectShow gets the play timing (else the BGA freezes partway).
    std::string defIni = JoinPath(spvDir, "default.ini");
    if (!fileExistsLocal(defIni))
        FnexWriteEncryptedTextFile(defIni, "PlayDelayTime = 35\r\nPlayStartPos = 0", FnexCryptoKind::Ini);
    LogLine("[bgpool] pool ready: %d clips in Video\\_spv (%d newly converted, redirect at play time)", have, made);
}

static void scanCategory(const std::string& mode, const std::string& modeFolder, const std::string& category, const std::string& catPath) {
    WIN32_FIND_DATAA songFd{};
    HANDLE hSong = FindFirstFileA(JoinPath(catPath, "*").c_str(), &songFd);
    if (hSong == INVALID_HANDLE_VALUE) return;
    do {
        if (!(songFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(songFd.cFileName, ".") == 0 || strcmp(songFd.cFileName, "..") == 0) continue;
        scanFlatSongFolder(mode, modeFolder, category, JoinPath(catPath, songFd.cFileName), songFd.cFileName);
    } while (FindNextFileA(hSong, &songFd));
    FindClose(hSong);
}

static void LogCatalogOrderCheck() {
    unsigned long alpha = 0, difficulty = 0, range = 0;
    std::vector<std::string> modes;
    for (const auto& song : g_songs) {
        bool seen = false;
        for (const auto& mode : modes) if (_stricmp(mode.c_str(), song.mode.c_str()) == 0) { seen = true; break; }
        if (!seen) modes.push_back(song.mode);
        int levels[4] = {song.levelNM, song.levelHD, song.levelSHD, song.levelEX};
        int previous = 0;
        for (int level : levels) {
            if (level == 0) continue; // zero means that slot is absent
            if (level < 1 || level > 18) ++range;
            if (previous > level) ++difficulty;
            previous = level;
        }
    }
    for (const auto& mode : modes) {
        std::vector<CustomBmsSong> songs = GetCustomBmsSongsForMode(mode);
        for (size_t i = 1; i < songs.size(); ++i)
            if (_stricmp(songs[i - 1].category.c_str(), songs[i].category.c_str()) == 0 &&
                SongTitleLess(songs[i], songs[i - 1])) ++alpha;
    }
    LogLine("[order-check] songs=%lu alphaViolations=%lu difficultyViolations=%lu rangeViolations=%lu ok=%d",
            (unsigned long)g_songs.size(), alpha, difficulty, range,
            alpha == 0 && difficulty == 0 && range == 0 ? 1 : 0);
}

static void DropCatalogueCaches();
// ---- where the memory actually goes --------------------------------------------------------------
//
// Estimating this from outside kept missing by a factor of seven: the catalogue's own records add up
// to about 100 MB and the per-song file lists to eight more, while the process reports 723 MB
// private. So the structures are walked and weighed rather than reasoned about.
//
// Strings are counted by capacity() and only once they have escaped the small-string buffer -- below
// sixteen characters a std::string carries its text inside itself and costs no heap at all, which is
// most ids, modes and titles here. Allocator overhead is taken as eight bytes a block.
static size_t StrHeap(const std::string& v) {
    return v.capacity() >= 16 ? v.capacity() + 1 + 8 : 0;
}

static void LogCatalogMemory(const char* why) {
    const double mb = 1048576.0;
    size_t recs = g_songs.size() * sizeof(CustomBmsSong);
    size_t strs = 0, shared = 0;
    std::set<const void*> seenVec;               // a SharedVec payload is counted once per group
    for (const auto& s : g_songs) {
        strs += StrHeap(s.id) + StrHeap(s.mode) + StrHeap(s.modeFolder) + StrHeap(s.category)
              + StrHeap(s.songDir) + StrHeap(s.cacheDir) + StrHeap(s.title) + StrHeap(s.artist)
              + StrHeap(s.sourceMode);
        auto weigh = [&](const auto& sv) {
            if (sv.empty()) return;
            if (!seenVec.insert((const void*)&sv[0]).second) return;
            for (size_t i = 0; i < sv.size(); ++i) shared += sizeof(sv[i]) + 24;
        };
        weigh(s.charts); weigh(s.audioFiles); weigh(s.imageFiles); weigh(s.videoFiles);
    }
    size_t keys = 0;
    for (const auto& k : g_catalogKeys) keys += 2 * sizeof(void*) + k.size() + StrHeap(k);
    size_t idmap = 0;
    for (const auto& kv : g_idMap) idmap += 40 + kv.first.size() + StrHeap(kv.first);
    size_t cache = 0;
    for (const auto& kv : g_scanCache) cache += 40 + StrHeap(kv.first) + sizeof(kv.second);

    const size_t named = recs + strs + shared + keys + idmap + cache;
    LogLine("[memacct] %s: songs=%lu  records=%.0fMB strings=%.0fMB sharedVecs=%.0fMB",
            why, (unsigned long)g_songs.size(), recs / mb, strs / mb, shared / mb);
    LogLine("[memacct]   catalogKeys=%.0fMB(%lu)  idMap=%.0fMB(%lu)  scanCache=%.0fMB(%lu)",
            keys / mb, (unsigned long)g_catalogKeys.size(), idmap / mb,
            (unsigned long)g_idMap.size(), cache / mb, (unsigned long)g_scanCache.size());
    LogLine("[memacct]   accounted=%.0fMB -- the [mem] line above has the process total",
            named / mb);
}


// The song-select page holds at most kWheelPageCap rows per category (the visible rows[] sit in
// the object before the live fields at 0xbc44c, and 872 x 0x374 is where they run into them).
// Rather than truncate a big folder -- songs quietly missing from the end of the list -- it is
// published as several wheel categories, "FOLDER 1/3", "FOLDER 2/3" ..., filled in title order.
// Everything downstream keys on song.category, and the details.txt / bg.png lookups derive the
// folder from songDir, so a part behaves exactly like a folder of its own. Runs after the
// auto-generated modes exist, so each mode is split on its own count, and after the scan cache
// is saved, so the cache keeps the real folder names.
static const size_t kWheelPageCap = 800;   // under the 872 rows, with the RANDOM row and margin

static void SplitOversizedCategories() {
    std::map<std::string, std::vector<size_t>> groups;      // "mode\category" (lowered) -> songs
    for (size_t i = 0; i < g_songs.size(); ++i)
        groups[lowerCopy(g_songs[i].mode) + "\\" + lowerCopy(g_songs[i].category)].push_back(i);
    unsigned split = 0;
    for (auto& g : groups) {
        std::vector<size_t>& idx = g.second;
        if (idx.size() <= kWheelPageCap) continue;
        std::stable_sort(idx.begin(), idx.end(), [](size_t a, size_t b) {
            return lowerCopy(g_songs[a].title) < lowerCopy(g_songs[b].title);
        });
        const size_t parts = (idx.size() + kWheelPageCap - 1) / kWheelPageCap;
        const size_t per = (idx.size() + parts - 1) / parts;
        const std::string folder = g_songs[idx[0]].category;
        for (size_t k = 0; k < idx.size(); ++k) {
            char name[128];
            snprintf(name, sizeof(name), "%s %lu/%lu", folder.c_str(), (unsigned long)(k / per + 1), (unsigned long)parts);
            g_songs[idx[k]].category = name;
        }
        LogLine("[category] '%s' (%s) has %lu songs: published as %lu parts of up to %lu",
                folder.c_str(), g_songs[idx[0]].mode.c_str(), (unsigned long)idx.size(),
                (unsigned long)parts, (unsigned long)per);
        ++split;
    }
    if (split) DropCatalogueCaches();
}

void ScanBMSData() {
    // Anything memoised from the catalogue dies with it. The per-mode category lists are keyed on
    // the catalogue's size, and a rescan that happened to land on the same count would otherwise
    // keep serving the old answer -- unlikely, but not something to leave to chance.
    DropCatalogueCaches();
    g_songs.clear();
    if (!Config().enableBmsData) return;
    DWORD scanStart = GetTickCount();
    LoadScanCache();
    std::string root = JoinPath(GetExeDirectory(), Config().bmsRoot);
    CreateDirectoryA(root.c_str(), nullptr);
    LearnNativeModeModels();
    g_scanTotal = CountSongFolders(root);  // for the popup progress bar
    if (Config().traceBmsScan) LogLine("[bms] scan root='%s' layout='mode/category/song(flat)' songFolders=%d", root.c_str(), g_scanTotal);

    WIN32_FIND_DATAA modeFd{};
    HANDLE hMode = FindFirstFileA(JoinPath(root, "*").c_str(), &modeFd);
    if (hMode == INVALID_HANDLE_VALUE) return;
    do {
        if (!(modeFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(modeFd.cFileName, ".") == 0 || strcmp(modeFd.cFileName, "..") == 0) continue;
        std::string modeFolder = modeFd.cFileName;
        std::string mode = NormalizeModeName(modeFolder);
        if (mode.empty()) {
            LogLine("[bms] skip first-level folder '%s' because it is not a known mode. Expected BMSData\\<Mode>\\<Category>\\<Song>", modeFolder.c_str());
            continue;
        }
        std::string modePath = JoinPath(root, modeFolder);
        WIN32_FIND_DATAA catFd{};
        HANDLE hCat = FindFirstFileA(JoinPath(modePath, "*").c_str(), &catFd);
        if (hCat == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(catFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (strcmp(catFd.cFileName, ".") == 0 || strcmp(catFd.cFileName, "..") == 0) continue;
            scanCategory(mode, modeFolder, catFd.cFileName, JoinPath(modePath, catFd.cFileName));
        } while (FindNextFileA(hCat, &catFd));
        FindClose(hCat);
    } while (FindNextFileA(hMode, &modeFd));
    FindClose(hMode);
    SaveScanCache();          // before the virtual copies exist; only scanned songs are stored
    ExpandAutoGeneratedModes();
    SplitOversizedCategories();
    LogCatalogOrderCheck();
    // Category labels/splashes are the only generated UI artifacts left. They are shared
    // per category (not per song) and keep the current native wheel integration readable.
    {
        std::vector<std::string> modes;
        for (const auto& s : g_songs) {
            bool seen = false;
            for (const auto& m : modes) if (_stricmp(m.c_str(), s.mode.c_str()) == 0) { seen = true; break; }
            if (!seen) modes.push_back(s.mode);
        }
        for (const auto& m : modes) {
            if (!g_directBmsEnabled) WriteVirtualStageIniForMode(m);
        }
    }
    SaveIdMap(); // persist stable ids so the cache survives the next scan / folder additions
    if (!g_directBmsEnabled) {
        WaitAllVideoJobs();
        PrepareBgaPool();
    }
    LogProcessFootprint("scan complete");
    LogCatalogMemory("scan complete");
    LogLine("[bms] indexed custom songs=%lu activeMode='%s' idmap=%lu entries scanMs=%lu",
            static_cast<unsigned long>(g_songs.size()), g_activeMode.c_str(), (unsigned long)g_idMap.size(),
            static_cast<unsigned long>(GetTickCount() - scanStart));
}

const std::vector<CustomBmsSong>& GetCustomBmsSongs() { return g_songs; }

std::vector<CustomBmsSong> GetCustomBmsSongsForMode(const std::string& mode) {
    std::string n = NormalizeModeName(mode);
    if (n.empty()) n = mode;
    std::vector<CustomBmsSong> out;
    for (const auto& s : g_songs) if (n.empty() || _stricmp(s.mode.c_str(), n.c_str()) == 0) out.push_back(s);
    std::vector<std::string> categories;
    for (const auto& song : out) {
        bool seen = false;
        for (const auto& category : categories)
            if (_stricmp(category.c_str(), song.category.c_str()) == 0) { seen = true; break; }
        if (!seen) categories.push_back(song.category);
    }
    auto categoryRank = [&](const std::string& category) {
        for (size_t i = 0; i < categories.size(); ++i)
            if (_stricmp(categories[i].c_str(), category.c_str()) == 0) return i;
        return categories.size();
    };
    std::stable_sort(out.begin(), out.end(), [&](const CustomBmsSong& a, const CustomBmsSong& b) {
        size_t ra = categoryRank(a.category), rb = categoryRank(b.category);
        return ra != rb ? ra < rb : SongTitleLess(a, b);
    });
    return out;
}

std::vector<CustomBmsSong> GetCustomBmsSongsForActiveMode() {
    return GetCustomBmsSongsForMode(g_activeMode);
}

// The categories a mode has songs in, memoised per mode.
//
// This is called from the CreateFileA hook -- every path containing "songselect" asks for it, and
// the game opens hundreds of those while song select comes up -- and it used to walk the whole
// catalogue every time. That catalogue is 179,424 entries after the auto-generated modes are
// expanded, not the 19,936 songs on disk, and the inner duplicate check compares each match against
// up to 84 category names, so one call was roughly 1.7 million case-insensitive compares. Another
// caller loops it over candidate modes, paying that several times over.
//
// The catalogue is fixed once scanning finishes, so the answer is computed once per mode and kept.
// It is keyed on the catalogue's size as well, so a rescan cannot serve a stale list.
static std::map<std::string, std::vector<std::string> > g_catsByMode;
static size_t g_catsByModeStamp = (size_t)-1;

static void DropCatalogueCaches() {
    g_catsByMode.clear();
    g_catsByModeStamp = (size_t)-1;
}

std::vector<std::string> GetCustomBmsCategoriesForMode(const std::string& mode) {
    std::string n = NormalizeModeName(mode);
    if (n.empty()) n = mode;

    if (g_catsByModeStamp != g_songs.size()) {
        g_catsByMode.clear();
        g_catsByModeStamp = g_songs.size();
    }
    std::map<std::string, std::vector<std::string> >::iterator it = g_catsByMode.find(n);
    if (it != g_catsByMode.end()) return it->second;

    // Built with a set rather than a linear duplicate check, so filling it is one pass.
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& s : g_songs) {
        if (_stricmp(s.mode.c_str(), n.c_str()) != 0) continue;
        std::string key = s.category;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        if (seen.insert(key).second) out.push_back(s.category);
    }
    g_catsByMode[n] = out;
    return out;
}

bool IsCustomSongId(const char* id) { return FindCustomSongById(id) != nullptr; }

const CustomBmsSong* FindCustomSongById(const char* id) {
    if (!id) return nullptr;
    // Rebuilt whenever the catalogue changes size: a linear _stricmp over 179k songs was 2-5 ms
    // per call, and the hover prefetch asks on every cursor stop.
    static std::unordered_map<std::string, size_t> s_index;
    static size_t s_stamp = (size_t)-1;
    if (s_stamp != g_songs.size()) {
        s_index.clear();
        s_index.reserve(g_songs.size() * 2);
        for (size_t i = 0; i < g_songs.size(); ++i) s_index.emplace(lowerCopy(g_songs[i].id), i);
        s_stamp = g_songs.size();
    }
    auto it = s_index.find(lowerCopy(id));
    return it == s_index.end() ? nullptr : &g_songs[it->second];
}

std::string GetCustomSongBackingId(const char* id) {
    if (!IsCustomSongId(id)) return std::string();
    return Config().redirectNativeId;
}
