#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Custom courses for the Radio Mix modes, built from the custom songs.
//
// A Radio Mix "channel" is a course: four songs played back to back on one gauge, listed in
// system\<family>radiomix\Stage.ini as `"<channel>"="<stage>;<stage>;<stage>;<stage>"` where a stage
// is a song id plus its chart's suffix (bms000123-hd). Courses here are cut from level bands --
// four songs from the same band, easiest first -- so a course is a fair climb rather than a lottery.
// Each band gives as many courses as it has songs for without repeating one inside the band; the
// bands take turns until the requested count is reached. Deterministic for a given seed, so a
// course keeps its songs (and its ranking) from one start to the next as long as the library does.

struct CourseCandidate {
    std::string stage;   // "<id>" or "<id>-hd|-shd|-ex"
    int level;           // the chart's display level, 1..20
    size_t song;         // which song it belongs to (one chart per song in a course)
};

struct CourseDef {
    std::string name;                 // the channel key, e.g. "bmsc5_03"
    int number = 0;                   // 1-based, for the art
    int low = 0, high = 0;            // the level band
    int level = 0;                    // the hardest stage
    std::vector<std::string> stages;  // easiest first
    std::vector<size_t> songs;        // parallel to stages
};

static const int kCourseStages = 4;
static const int kCourseBands[][2] = {{1, 4}, {5, 8}, {9, 12}, {13, 16}, {17, 20}};

inline uint32_t CourseRand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

inline std::vector<CourseDef> BuildCourses(const std::vector<CourseCandidate>& candidates,
                                           const std::string& prefix, size_t count, uint32_t seed) {
    const int bands = (int)(sizeof(kCourseBands) / sizeof(kCourseBands[0]));
    // Per band: each song once (its first chart in the band), in a seeded shuffle.
    std::vector<std::vector<const CourseCandidate*>> pool(bands);
    for (int b = 0; b < bands; ++b) {
        std::vector<size_t> seen;
        for (const auto& c : candidates) {
            if (c.level < kCourseBands[b][0] || c.level > kCourseBands[b][1]) continue;
            if (std::find(seen.begin(), seen.end(), c.song) != seen.end()) continue;
            seen.push_back(c.song);
            pool[b].push_back(&c);
        }
        uint32_t s = seed ^ (0x9E3779B9u * (uint32_t)(b + 1));
        if (!s) s = 1;
        for (size_t i = pool[b].size(); i > 1; --i) std::swap(pool[b][i - 1], pool[b][CourseRand(s) % i]);
    }

    std::vector<CourseDef> out;
    std::vector<size_t> taken(bands, 0);
    bool progress = true;
    while (out.size() < count && progress) {
        progress = false;
        for (int b = 0; b < bands && out.size() < count; ++b) {
            if (pool[b].size() - taken[b] < (size_t)kCourseStages) continue;
            std::vector<const CourseCandidate*> pick(pool[b].begin() + (long)taken[b],
                                                     pool[b].begin() + (long)taken[b] + kCourseStages);
            taken[b] += kCourseStages;
            std::stable_sort(pick.begin(), pick.end(),
                             [](const CourseCandidate* a, const CourseCandidate* c) { return a->level < c->level; });
            CourseDef d;
            d.number = (int)out.size() + 1;
            char name[32];
            snprintf(name, sizeof(name), "%s%02d", prefix.c_str(), d.number);
            d.name = name;
            d.low = kCourseBands[b][0];
            d.high = kCourseBands[b][1];
            for (const auto* p : pick) {
                d.stages.push_back(p->stage);
                d.songs.push_back(p->song);
                d.level = std::max(d.level, p->level);
            }
            out.push_back(d);
            progress = true;
        }
    }
    return out;
}

// The Stage.ini text for these courses: appended to the native channel list and given their own
// entries. `nativeText` is the decrypted original; its "channel_normal" line gains the new names.
inline std::string AppendCoursesToStageIni(const std::string& nativeText, const std::vector<CourseDef>& courses) {
    if (courses.empty()) return nativeText;
    std::string text = nativeText;
    const std::string key = "\"channel_normal\"";
    const size_t k = text.find(key);
    if (k == std::string::npos) return nativeText;
    // The value's closing quote: the last '"' on that line.
    const size_t eol = text.find_first_of("\r\n", k);
    const size_t close = text.rfind('"', eol == std::string::npos ? std::string::npos : eol - 1);
    if (close == std::string::npos || close <= k + key.size()) return nativeText;
    std::string names;
    for (const auto& c : courses) names += ";" + c.name;
    text.insert(close, names);
    std::string entries = "\r\n";
    for (const auto& c : courses) {
        std::string list;
        for (const auto& s : c.stages) list += (list.empty() ? "" : ";") + s;
        char level[16];
        snprintf(level, sizeof(level), "%d", c.level);
        entries += "\r\n\"" + c.name + "\"=\"" + list + "\"\r\n\"" + c.name + "_Level\"=\"" + level + "\"\r\n";
    }
    return text + entries;
}
