#pragma once
#include <cctype>
#include <string>

// The game keeps a score file per song and difficulty -- sound\rank_<mode>_<id>[-hd|-shd|-ex].bin,
// and e_rank_ for the course variant -- and creates one for every song the result screen or the
// ranking panel touches (gfresultrank.cpp: `_access` fails -> fopen "wb" an empty table). For the
// custom songs that filled the game's own Sound folder with files named after our ids. They live
// in CustomCache\rank instead; the game is pointed there by the file hooks, for its existence check
// (GetFileAttributesA, what the CRT's _access calls) as well as the open -- redirecting only the
// open would make the check fail and the game would overwrite the table with an empty one.
//
// The file name of a custom song's rank table, or "" for anything else (native songs keep theirs).
inline std::string CustomRankFileName(const std::string& path) {
    std::string p;
    for (char c : path) p += (char)std::tolower((unsigned char)(c == '/' ? '\\' : c));
    const size_t slash = p.find_last_of('\\');
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
    if (dir != "sound" && (dir.size() < 6 || dir.compare(dir.size() - 6, 6, "\\sound") != 0)) return "";
    size_t i = name.compare(0, 2, "e_") == 0 ? 2 : 0;
    if (name.compare(i, 5, "rank_") != 0) return "";
    i += 5;
    const size_t us = name.find('_', i);                  // after the mode name
    if (us == std::string::npos || name.compare(us + 1, 3, "bms") != 0) return "";
    size_t j = us + 4, digits = 0;
    while (j < name.size() && std::isdigit((unsigned char)name[j])) { ++j; ++digits; }
    if (!digits) return "";
    const std::string rest = name.substr(j);
    if (rest != ".bin" && rest != "-ez.bin" && rest != "-hd.bin" && rest != "-shd.bin" && rest != "-ex.bin") return "";
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

// Where that file lives now: <exe>\<CacheRoot>\rank\<name> (folder created on first use), or "" when
// `path` is not a custom song's rank table. mod_main.cpp.
std::string CustomRankPath(const char* path);
