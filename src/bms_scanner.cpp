#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "bms_scanner.h"
#include "config.h"
#include "logger.h"
#include "fnex_crypto.h"
#include "progress_popup.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <utility>
#include <map>
#include <memory>

static std::vector<CustomBmsSong> g_songs;
static std::string g_activeMode;
static int g_scanTotal = 0;   // song folders to process this scan — drives the popup bar
static bool g_directBmsEnabled = false;
static std::string g_directBmsVideoPath;
static uint32_t g_directBmsVideoStartMs = 0;

void SetDirectBmsEnabled(bool enabled) { g_directBmsEnabled = enabled; }
bool DirectBmsEnabled() { return g_directBmsEnabled; }
void ResetDirectBmsVideo() { StopDirectBmsFfmpegVideo(); g_directBmsVideoPath.clear(); g_directBmsVideoStartMs = 0; }
const std::string& DirectBmsVideoPath() { return g_directBmsVideoPath; }
uint32_t DirectBmsVideoStartMs() { return g_directBmsVideoStartMs; }

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
    if (l == "5keymix" || l == "streetmix" || l == "5k" || l == "5kstandard") return "5keyMix";
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
    const char* candidates[] = {"5keyonly", "5keymix", "7keymix", "clubmix", "spacemix", "rubymix", "ez2catch", "turntable", "scratchmix", "cv2mix"};
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

static uint32_t bmsTickToEzTick(const ParsedBmsForEz& bms, int measure, int index, int div) {
    double pos = 0.0;
    for (int m = 0; m < measure; ++m) {
        double scale = 1.0;
        if (m < (int)bms.measureScale.size()) scale = bms.measureScale[m];
        pos += 192.0 * scale;
    }
    double scale = 1.0;
    if (measure < (int)bms.measureScale.size()) scale = bms.measureScale[measure];
    pos += ((double)index / (double)div) * 192.0 * scale;
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

static bool startFfmpegPipe(const std::string& src, const std::string& outputArgs,
                            PROCESS_INFORMATION& process, HANDLE& readPipe, bool realtime = false) {
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

static bool generateEzFromBms(const std::string& bmsPath, const std::string& ezPath,
                              const std::string& baseName, std::string* report,
                              std::vector<unsigned char>* memoryOut = nullptr) {
    ParsedBmsForEz bms;
    if (!parseBmsForEz(bmsPath, bms)) return false;
    std::vector<EzRawEventOut> byTrack[64];
    uint32_t totalTicks = 192;
    for (const auto& e : bms.events) {
        int tr = bmsEventToEzTrack5Key(e);
        if (tr < 0 || tr >= 64) continue;
        uint32_t tick = bmsTickToEzTick(bms, e.measure, e.index, e.div);
        EzRawEventOut out{tick, tr, e.wavId, 6, e.channel, 1, 0.0f};
        byTrack[tr].push_back(out);
        if (tick + 192 > totalTicks) totalTicks = tick + 192;
    }

    // Native EZ stores BPM changes as type-3 events in track 0.
    byTrack[0].push_back({0, 0, 0, 0, 0, 3, (float)bms.bpm});
    std::vector<std::pair<uint32_t, double>> tempos;
    tempos.push_back({0, bms.bpm});
    for (const auto& te : bms.tempoEvents) {
        uint32_t tick = bmsTickToEzTick(bms, te.measure, te.index, te.div);
        byTrack[0].push_back({tick, 0, 0, 0, 0, 3, (float)te.bpm});
        tempos.push_back({tick, te.bpm});
        if (tick + 192 > totalTicks) totalTicks = tick + 192;
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
                if (events[(size_t)open].len < 7) events[(size_t)open].len = 7;
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

    // End of chart = the last tick REACHED by any note, counting a long note's tail (tick+len), not just
    // its head. The earlier totalTicks only saw note heads (tick+192), so a chart ending on a held long
    // declared itself over mid-hold → forced result before the release. Recompute from resolved events,
    // THEN pad ~3s so no note type (tap or long) snaps straight to the result screen.
    for (int t = 0; t < 64; ++t)
        for (const auto& ev : byTrack[t]) {
            if (ev.type == 3) continue; // BPM marker, not a note
            uint32_t endTick = ev.tick + (uint32_t)(ev.len > 192 ? ev.len : 192);
            if (endTick > totalTicks) totalTicks = endTick;
        }
    { uint32_t pad = (uint32_t)(3.0 * 0.8 * (bms.bpm > 0.0 ? bms.bpm : 120.0)); if (pad < 192) pad = 192; totalTicks += pad; }

    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    double durationSeconds = 0.0;
    double currentBpm = bms.bpm;
    uint32_t cursor = 0;
    for (const auto& tempo : tempos) {
        uint32_t tick = std::min(tempo.first, totalTicks);
        if (tick > cursor && currentBpm > 0.0) durationSeconds += (tick - cursor) * 240.0 / (192.0 * currentBpm);
        cursor = tick;
        if (tempo.second > 0.0) currentBpm = tempo.second;
    }
    if (totalTicks > cursor && currentBpm > 0.0) durationSeconds += (totalTicks - cursor) * 240.0 / (192.0 * currentBpm);

    std::vector<unsigned char> out;
    out.resize(0x96, 0);
    out[0] = 'E'; out[1] = 'Z'; out[2] = 'F'; out[3] = 'F';
    out[4] = 0; out[5] = 8; // v23: native FNEX charts are version 8; basename must match the .ez/.es filename.
    putFixedString(out, 0x06, 0x40, baseName);
    putFixedString(out, 0x46, 0x40, bms.artist);
    putU16(out, 0x86, 192);
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
                putU16(out, off + 10, (uint16_t)ev.len);
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
    return fileExistsLocal(path) ? path : std::string();
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

static const char* suffixForSlot(int slot) {
    static const char* suffixes[4] = {"", "-hd", "-shd", "-ex"};
    return slot >= 0 && slot < 4 ? suffixes[slot] : "";
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
    song.levelNM = song.levelHD = song.levelSHD = song.levelEX = 0;
    song.bpmNM = song.bpmHD = song.bpmSHD = song.bpmEX = 0.0f;
    std::vector<size_t> ordered;
    for (size_t i = 0; i < song.chartMeta.size(); ++i) {
        auto& chart = song.chartMeta[i];
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

    // FNEX exposes four slots. For larger packs keep the easiest, hardest and two evenly
    // spaced charts between them; their slot order always follows authored #PLAYLEVEL.
    size_t selected = ordered.size() < 4 ? ordered.size() : 4;
    for (size_t slot = 0; slot < selected; ++slot) {
        size_t pos = ordered.size() <= 4 ? slot : (slot * (ordered.size() - 1) + 1) / 3;
        song.chartMeta[ordered[pos]].slot = (int)slot;
    }

    int* levels[4] = {&song.levelNM, &song.levelHD, &song.levelSHD, &song.levelEX};
    float* bpms[4] = {&song.bpmNM, &song.bpmHD, &song.bpmSHD, &song.bpmEX};
    for (const auto& chart : song.chartMeta) {
        if (chart.slot < 0 || chart.slot >= 4) continue;
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
}

static std::string modePrefixForCache(const std::string& mode) {
    std::string m = NormalizeModeName(mode);
    if (m == "5keyMix") return "5keyMix1p-";
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

static void GeneratePlayableCacheV10(const CustomBmsSong& song, bool materializeAudio) {
    // All EZ2 modes share the same .ez/.ezi format and channel→track mapping (see
    // bmsChannelToEzTrackMode); the mode only sets the cache prefix and the 7-key flag. Catch is
    // still charts-as-lanes here (playable, but the "catch" gameplay uses X-position, not lanes —
    // ponytail: good enough to appear/play; faithful catch positions would need a separate encoder).
    g_curModeSevenKey = modeIsSevenKey(song.mode);
    std::vector<CustomBmsChart> charts = song.chartMeta;
    charts.erase(std::remove_if(charts.begin(), charts.end(), [](const CustomBmsChart& c){ return c.slot < 0 || c.slot >= 4; }), charts.end());
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
        if (!materializeAudio && cacheIsFresh(ezPath, src) && cacheIsFresh(eziPath, src)) { emitted++; continue; }
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
        std::string ini;
        ini += "[General]\r\n";
        ini += "Level = " + (charts.empty() ? std::string("1") : charts[0].playLevel.empty() ? std::string("1") : charts[0].playLevel) + "\r\n";
        ini += "MeasureScale = 1.6\r\n\r\n";
        ini += "[SongOriginalName]\r\n";
        ini += "Name = " + song.title + "\r\n";
        ini += "Artist = " + song.artist + "\r\n\r\n";
        ini += "[JudgmentDelta]\r\nKool = 6\r\nCool = 24\r\nGood = 50\r\nMiss = 70\r\n\r\n";
        ini += "[GaugeUpDownRate]\r\nCool = 0.2\r\nGood = 0.1\r\nMiss = -1.8\r\nFail = -4.8\r\n";
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
                           uint32_t (&trackMax)[64], float& bpm, float& seconds) {
    if (ez.size() < 0x96 || memcmp(ez.data(), "EZFF", 4) != 0 || directU16(ez.data() + 0x8c) != 64)
        return false;
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

static void* loadDirectCompressed(void* audio, const std::string& path) {
    std::vector<unsigned char> pcm;
    uint32_t channels = 0, rate = 0, bits = 0;
    if (!mediaFoundationDecodePcmLocal(path, 0, pcm, channels, rate, bits)) return nullptr;
    DirectWaveFormat format{};
    format.formatTag = 1;
    format.channels = (uint16_t)channels;
    format.sampleRate = rate;
    format.bits = (uint16_t)bits;
    format.blockAlign = (uint16_t)(channels * bits / 8);
    format.averageBytes = rate * format.blockAlign;
    return createDirectSample(audio, path, format, pcm.data(), (uint32_t)pcm.size());
}

static const CustomBmsChart* selectDirectChart(const CustomBmsSong& song, const char* request) {
    std::string low = lowerCopy(request ? request : "");
    int slot = low.find("-shd") != std::string::npos ? 2
             : low.find("-hd")  != std::string::npos ? 1
             : low.find("-ex")  != std::string::npos ? 3 : 0;
    for (const auto& chart : song.chartMeta) if (chart.slot == slot) return &chart;
    for (const auto& chart : song.chartMeta) if (chart.slot >= 0 && chart.slot < 4) return &chart;
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
    if (selected.empty()) selectRandomBgaPoolVideo(selected);
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

bool LoadCustomBmsDirect(void* songPackage, const char* request) {
    if (!songPackage || !request) return false;
    const CustomBmsSong* song = nullptr;
    std::string lowRequest = lowerCopy(request);
    for (const auto& candidate : g_songs)
        if (lowRequest.find(lowerCopy(candidate.id)) != std::string::npos) { song = &candidate; break; }
    if (!song) return false;
    ResetDirectBmsVideo();
    const CustomBmsChart* chart = selectDirectChart(*song, request);
    if (!chart) { LogLine("[direct] no playable chart id='%s'", song->id.c_str()); return false; }

    std::string chartPath = JoinPath(song->songDir, chart->file);
    g_curModeSevenKey = modeIsSevenKey(song->mode);
    std::vector<unsigned char> ez;
    std::string report;
    if (!generateEzFromBms(chartPath, std::string(), song->id, &report, &ez)) {
        LogLine("[direct] parse failed id='%s' chart='%s'", song->id.c_str(), chartPath.c_str());
        return false;
    }

    std::vector<DirectRawEvent> tracks[64];
    uint32_t trackMax[64]{};
    float bpm = 120.0f, seconds = 0.0f;
    if (!decodeDirectEz(ez, tracks, trackMax, bpm, seconds)) {
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
        if (!sample && !endsWith(source, ".ssf") && !endsWith(source, ".ezw"))
            sample = loadDirectCompressed(audio, source);
        if (!sample) { ++unsupported; continue; }
        *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(songPackage) + 0xc9d1c + id * 4) = sample;
        ++loaded;
    }

    unsigned char* object = reinterpret_cast<unsigned char*>(songPackage);
    memset(object + 8, 0, 0x1d1c - 8);
    memset(object + 0x1d1c, 0, 0x32000 * 4);
    lstrcpynA(reinterpret_cast<char*>(object + 8), selected.title.c_str(), 64);
    lstrcpynA(reinterpret_cast<char*>(object + 0x48), selected.artist.c_str(), 64);
    *reinterpret_cast<uint32_t*>(object + 0x88) = 192;
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
                                       plain ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, "Arial");
                    oldFont = (HFONT)SelectObject(hdc, font);
                    SIZE size{};
                    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &size);
                    if (size.cx <= r.right - r.left && size.cy <= r.bottom - r.top) break;
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                    font = nullptr;
                }
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
            // plain right-aligned: full-width rect, small right margin.
            RECT r1 = {3, 1, plain ? w - 2 : w - 3, line2.empty() ? h - 1 : h / 2};
            RECT r2 = {3, h / 2, plain ? w - 2 : w - 3, h - 1};
            drawLine(line1, r1, maxPx);
            drawLine(line2, r2, std::max(8, maxPx - 3));
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
static bool GenerateSplashBmp(const std::string& bmpPath, const std::string& category, const std::string& subtext) {
    const int w = 256, h = 96;
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
    auto draw = [&](const std::wstring& txt, RECT r, int maxPx, COLORREF col) {
        if (txt.empty()) return;
        HFONT font = nullptr, oldFont = nullptr;
        for (int px = maxPx; px >= 8; --px) {
            font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Arial");
            oldFont = (HFONT)SelectObject(hdc, font);
            RECT calc = r; DrawTextW(hdc, txt.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK);
            if ((calc.right - calc.left) <= (r.right - r.left) && (calc.bottom - calc.top) <= (r.bottom - r.top)) break;
            SelectObject(hdc, oldFont); DeleteObject(font); font = nullptr;
        }
        if (!font) return;
        SetTextColor(hdc, col);
        DrawTextW(hdc, txt.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        SelectObject(hdc, oldFont); DeleteObject(font);
    };
    draw(Utf8ToWide(category), RECT{6, 8, w-6, 58}, 26, RGB(255,255,255));   // top: category, white
    draw(Utf8ToWide(subtext),  RECT{6, 60, w-6, h-6}, 18, RGB(255,220,0));   // bottom: subtext, yellow

    int rowBytes = ((w * 24 / 8 + 3) & ~3), pixelBytes = rowBytes * h;
    std::vector<unsigned char> pixels(pixelBytes, 0);
    BITMAPINFOHEADER bihBU = bih; bihBU.biHeight = h; bihBU.biSizeImage = (DWORD)pixelBytes;
    GetDIBits(hdc, bmp, 0, (UINT)h, pixels.data(), (BITMAPINFO*)&bihBU, DIB_RGB_COLORS);
    SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc); ReleaseDC(NULL, screen);
    BITMAPFILEHEADER bfh{}; bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + pixelBytes;
    FILE* f = fopen(bmpPath.c_str(), "wb");
    if (!f) return false;
    fwrite(&bfh, 1, sizeof(bfh), f); fwrite(&bihBU, 1, sizeof(bihBU), f);
    fwrite(pixels.data(), 1, pixelBytes, f); fclose(f);
    return true;
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

static void GenerateVisualAssetCache(const CustomBmsSong& song, const char* onlyRole = nullptr) {
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

    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    bool gdipStarted = (wantArtwork || wantName) &&
        Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) == Gdiplus::Ok;
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
        if (fileExistsLocal(abm)) return 0;
        return GenerateVisualPair(bmp, w, h, RGB(0,0,0), text.empty() ? RGB(180,245,255) : RGB(255,255,255),
                                  text, "", text.empty() ? artwork.get() : nullptr, false, circular, bpp);
    };

    int created = 0;
    if (artwork) {
        if (wantDisc) created += gen("disc", 256, 256, true, 24);
        if (wantDiscSmall) created += gen("discsmall", 128, 128, true, 24);
        if (wantEye) created += gen("eyecatch", 1024, 512, false, 24);
        if (wantResult) created += gen("resultdisc", 128, 128, true, 16);
    }
    if (wantName)
        created += gen("songname", 256, 32, false, 24, song.title.empty() ? song.id : song.title);

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
    LogLine("[assets] lazy id='%s' role='%s' source='%s' size=%ux%u created=%d",
            song.id.c_str(), all ? "all" : role.c_str(), artworkPath.c_str(), artworkW, artworkH, created);
    artwork.reset();
    if (gdipStarted) Gdiplus::GdiplusShutdown(gdipToken);
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
static void GenerateCategoryAssetsForMode(const std::string& mode) {
    std::vector<std::string> categories = GetCustomBmsCategoriesForMode(mode);
    if (categories.empty()) return;
    std::string dir = JoinPath(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), mode), "_categories");
    CreateDirectoryA(JoinPath(GetExeDirectory(), Config().cacheRoot).c_str(), nullptr);
    CreateDirectoryA(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), mode).c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);

    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    bool gdipStarted = Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) == Gdiplus::Ok;
    int made = 0;
    for (size_t i = 0; i < categories.size(); ++i) {
        char fn[32] = {};
        wsprintfA(fn, "category_%02d.bmp", 48 + (int)i);
        std::string bmpPath = JoinPath(dir, fn);
        std::string abmPath = replaceExtensionLocal(bmpPath, ".abm");
        if (fileExistsLocal(abmPath)) continue; // already cached
        // 128x16 24bpp, black bg + white antialiased text — matches native category style.
        made += GenerateVisualPair(bmpPath, 128, 16, RGB(0, 0, 0), RGB(255, 255, 255),
                                   categories[i].empty() ? "CUSTOM" : categories[i], "",
                                   nullptr, false, false, 24);
    }
    if (gdipStarted) Gdiplus::GdiplusShutdown(gdipToken);
    LogLine("[category] category assets mode='%s' categories=%d made=%d dir='%s'",
            mode.c_str(), (int)categories.size(), made, dir.c_str());
}

// Read a category's details.txt (any song folder in it carries one) -> CATEGORY / SUBTEXT lines.
// Returns false if no details.txt found. Values are raw UTF-8 (kept for CJK/special chars).
static bool ReadCategoryDetails(const std::string& mode, const std::string& category,
                                std::string& categoryText, std::string& subText) {
    for (const auto& s : g_songs) {
        if (_stricmp(s.mode.c_str(), mode.c_str()) != 0) continue;
        if (_stricmp(s.category.c_str(), category.c_str()) != 0) continue;
        FILE* f = fopen(JoinPath(s.songDir, "details.txt").c_str(), "rb");
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

// One Version_NN.abm per custom category from its details.txt, into the mode's _versions cache dir.
// NN = 19 + categoryIndex (native versions are 0..18). The inject writes each custom song's version
// field to its category's NN, and the FS-hook redirects System\SongSelect\...\Version_NN.abm (NN>18)
// here. Index order MUST match GetCustomBmsCategoriesForMode (same order the inject/wheel uses).
static void GenerateVersionSplashForMode(const std::string& mode) {
    std::vector<std::string> categories = GetCustomBmsCategoriesForMode(mode);
    if (categories.empty()) return;
    std::string dir = JoinPath(JoinPath(JoinPath(GetExeDirectory(), Config().cacheRoot), mode), "_versions");
    CreateDirectoryA(dir.c_str(), nullptr);
    int made = 0;
    for (size_t i = 0; i < categories.size(); ++i) {
        int nn = 19 + (int)i;
        char fn[32] = {}; wsprintfA(fn, "Version_%02d.bmp", nn);
        std::string bmpPath = JoinPath(dir, fn);
        std::string abmPath = replaceExtensionLocal(bmpPath, ".abm");
        std::string catText, subText;
        if (!ReadCategoryDetails(mode, categories[i], catText, subText))
            catText = categories[i]; // no details.txt → just show the category folder name
        if (fileExistsLocal(abmPath)) continue; // cached
        if (GenerateSplashBmp(bmpPath, catText, subText) && GenerateAbmFromBmp(bmpPath)) { made++; DeleteFileA(bmpPath.c_str()); }
    }
    LogLine("[version] splash assets mode='%s' categories=%d made=%d dir='%s'",
            mode.c_str(), (int)categories.size(), made, dir.c_str());
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

static void scanFlatSongFolder(const std::string& mode, const std::string& modeFolder, const std::string& category, const std::string& songDir, const std::string& folderName) {
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

void ScanBMSData() {
    g_songs.clear();
    if (!Config().enableBmsData) return;
    DWORD scanStart = GetTickCount();
    std::string root = JoinPath(GetExeDirectory(), Config().bmsRoot);
    CreateDirectoryA(root.c_str(), nullptr);
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
            GenerateCategoryAssetsForMode(m);
            GenerateVersionSplashForMode(m);
        }
    }
    SaveIdMap(); // persist stable ids so the cache survives the next scan / folder additions
    if (!g_directBmsEnabled) {
        WaitAllVideoJobs();
        PrepareBgaPool();
    }
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

std::vector<std::string> GetCustomBmsCategoriesForMode(const std::string& mode) {
    std::vector<std::string> out;
    std::string n = NormalizeModeName(mode);
    if (n.empty()) n = mode;
    for (const auto& s : g_songs) {
        if (_stricmp(s.mode.c_str(), n.c_str()) != 0) continue;
        bool seen = false;
        for (const auto& c : out) if (_stricmp(c.c_str(), s.category.c_str()) == 0) { seen = true; break; }
        if (!seen) out.push_back(s.category);
    }
    return out;
}

bool IsCustomSongId(const char* id) { return FindCustomSongById(id) != nullptr; }

const CustomBmsSong* FindCustomSongById(const char* id) {
    if (!id) return nullptr;
    for (const auto& s : g_songs) if (_stricmp(id, s.id.c_str()) == 0) return &s;
    return nullptr;
}

std::string GetCustomSongBackingId(const char* id) {
    if (!IsCustomSongId(id)) return std::string();
    return Config().redirectNativeId;
}
