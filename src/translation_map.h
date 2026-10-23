#pragma once
#include <string>

// English textures: EZ2AC_FNEX\Translation\system\<path> stands in for system\<path>. The originals
// are never touched; removing the Translation folder (or EnglishTextures=0) brings the Korean back.
//
// The game names its files either relative to the install ("system\SongSelect\x.abm", the working
// directory is the install) or, through some of the mod's own paths, absolute. Both reduce to the
// same lowercase key, "system\songselect\x.abm", which is what the index of translated files holds.
inline std::string TranslationKey(const std::string& path, const std::string& exeDirLower) {
    std::string p;
    p.reserve(path.size());
    for (char c : path) {
        char l = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        p += (l == '/') ? '\\' : l;
    }
    if (!exeDirLower.empty() && p.size() > exeDirLower.size() + 1 &&
        p.compare(0, exeDirLower.size(), exeDirLower) == 0 && p[exeDirLower.size()] == '\\')
        p.erase(0, exeDirLower.size() + 1);
    while (p.size() >= 2 && p[0] == '.' && p[1] == '\\') p.erase(0, 2);
    return p.compare(0, 7, "system\\") == 0 ? p : std::string();
}
