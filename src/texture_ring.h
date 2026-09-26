#pragma once
// The pure half of the custom-art texture ring (mod_main.cpp, HookLoadTexture): which ring a
// texture request belongs to and which slot of that ring it gets. Kept apart so it can be tested
// without the game.
#include <cctype>
#include <string>
#include <vector>

struct RingSlot {
    void* tex = nullptr;       // the KTexture the slot owns, once created
    std::string id;            // the song whose art it holds now
    unsigned long stamp = 0;   // last use, for least-recently-used recycling
    bool placeholder = false;  // filled before the real art existed: refill on the next request
};

// "system\songname\bms000123.bmp" with dirs {"songname", ...} -> index of "songname", id
// "bms000123". -1 for anything that is not a custom song's art in one of the ring directories.
inline int RingRoleOf(const char* name, const char* const* dirs, int count, std::string& id) {
    if (!name) return -1;
    std::string low(name);
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    for (int r = 0; r < count; ++r) {
        const std::string needle = std::string("\\") + dirs[r] + "\\";
        const size_t at = low.find(needle);
        if (at == std::string::npos) continue;
        const size_t start = at + needle.size();
        const size_t dot = low.find('.', start);
        id = low.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (id.compare(0, 3, "bms") != 0 || id.find('\\') != std::string::npos) return -1;
        return r;
    }
    return -1;
}

// The slot already showing `id` (same = true), else an unused one, else the least recently used.
inline RingSlot* PickRingSlot(std::vector<RingSlot>& slots, const std::string& id, bool& same) {
    same = false;
    if (slots.empty()) return nullptr;
    for (RingSlot& s : slots) if (s.tex && s.id == id) { same = true; return &s; }
    for (RingSlot& s : slots) if (!s.tex) return &s;
    RingSlot* oldest = &slots[0];
    for (RingSlot& s : slots) if (s.stamp < oldest->stamp) oldest = &s;
    return oldest;
}
