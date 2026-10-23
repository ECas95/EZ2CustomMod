#pragma once
// The pure half of the custom-art texture ring (mod_main.cpp, HookLoadTexture): which ring a
// texture request belongs to and which slot of that ring it gets. Kept apart so it can be tested
// without the game.
#include <cctype>
#include <string>
#include <vector>

struct RingSlot {
    void* tex = nullptr;       // the KTexture the slot owns, once created
    void* surf = nullptr;      // tex's surface when the slot took it (see DropStaleRingSlots)
    std::string id;            // the song whose art it holds now
    unsigned long stamp = 0;   // last use, for least-recently-used recycling
    bool placeholder = false;  // filled before the real art existed: refill on the next request
};

// Playing a song tears the texture pool down and rebuilds it WITHOUT recreating song select, and
// the pool recycles its KTexture objects -- often at the very addresses they had. A slot still
// holding a pre-song pointer then "owns" whatever texture now lives there: TextureRingHolds found
// that stale slot first, denied the row its (valid, new) texture, the wheel reloaded it, and the
// next frame denied it again, so the names around the cursor never filled and the wheel churned.
// A recycled texture has a new surface, so a slot whose texture no longer points at the surface
// it took is from the old pool and is emptied. `surfaceOf` reads a texture's current surface.
template <class SurfaceOf>
inline unsigned DropStaleRingSlots(std::vector<RingSlot>& slots, SurfaceOf surfaceOf) {
    unsigned dropped = 0;
    for (RingSlot& s : slots)
        if (s.tex && surfaceOf(s.tex) != s.surf) { s = RingSlot(); ++dropped; }
    return dropped;
}

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
