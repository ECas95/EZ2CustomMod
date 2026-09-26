#pragma once
#include <string>
#include <cctype>

// The scan cache stores each song's folder (and its art folder) as an absolute path. Copying the
// game to another drive or folder -- the cabinet's disk -- would turn every entry into a miss and
// re-parse the whole library, which on XP-era hardware takes hours. The install folder a cached
// entry was made in is whatever precedes "\<bmsRoot>\<modeFolder>\" in its song folder; swapping
// that for the current install keeps the entry.

inline std::string LowerAscii(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// "" when the song folder is not under <base>\<bmsRoot>\<modeFolder>\ (an absolute Root= setting).
inline std::string CachedInstallBase(const std::string& songDir, const std::string& bmsRoot,
                                     const std::string& modeFolder) {
    const std::string marker = LowerAscii("\\" + bmsRoot + "\\" + modeFolder + "\\");
    size_t pos = LowerAscii(songDir).rfind(marker);
    return pos == std::string::npos ? std::string() : songDir.substr(0, pos);
}

inline std::string RebasePath(const std::string& path, const std::string& oldBase,
                              const std::string& newBase) {
    if (oldBase.empty() || path.size() <= oldBase.size() || path[oldBase.size()] != '\\' ||
        LowerAscii(path.substr(0, oldBase.size())) != LowerAscii(oldBase))
        return path;
    return newBase + path.substr(oldBase.size());
}
