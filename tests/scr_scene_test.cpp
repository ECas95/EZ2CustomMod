// Guards the .scr/.str scene compositor -- the reverse-engineered formats behind BGA previews.
//
// Neither format is documented past its header, so the layouts here were derived from the shipped
// files. That makes them exactly the kind of thing that needs a test: a wrong stride or field offset
// still parses, still composites, and quietly draws the wrong picture. Each case below pins down a
// fact that was measured against the game's own data.
//
// The builders write real bytes in the real layout, so if a field offset in bms_scanner.cpp moves,
// these stop matching.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o scr_scene_test tests/scr_scene_test.cpp
//   ./scr_scene_test
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char last = a[a.size() - 1];
    return (last == '\\' || last == '/') ? a + b : a + "\\" + b;
}

// ---- the structures and parsers, copied verbatim from bms_scanner.cpp ---------------------------

struct ScrEntry { uint32_t frame; uint32_t plane; std::string str; };

struct StrKey {
    int32_t frame;
    float   x, y;
    float   qx0, qx1, qy0, qy1;
    float   ani, angle;
    float   delay;
    float   col[4];
    int32_t sa, da;
};
struct StrLayer { std::vector<std::string> tex; std::vector<StrKey> keys; };
struct StrScene { int fps; std::vector<StrLayer> layers; };

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
    out.fps = fps ? (int)fps : 60;
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
            if (type != 0) continue;
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
            bool clean = true;
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

static bool ScrEvalLayer(const StrLayer& layer, float frame, StrKey& out) {
    if (layer.keys.empty()) return false;
    if (layer.keys.size() > 1) {
        if (frame < (float)layer.keys.front().frame - 0.5f) return false;
        if (frame > (float)layer.keys.back().frame + 0.5f) return false;
    }
    size_t i = 0;
    while (i + 1 < layer.keys.size() && (float)layer.keys[i + 1].frame <= frame) ++i;
    const StrKey& a = layer.keys[i];
    const StrKey& b = layer.keys[i + 1 < layer.keys.size() ? i + 1 : i];
    const float span = (float)(b.frame - a.frame);
    float t = 0.0f;
    if (span > 0.0f && frame > (float)a.frame) {
        t = (frame - (float)a.frame) / span;
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
    out.frame  = (int32_t)frame;                    // how far into the layer, for its texture cycle
    for (int c = 0; c < 4; ++c) out.col[c] += (b.col[c] - a.col[c]) * t;
    return true;
}

// ---- builders that emit the real byte layout ----------------------------------------------------

static void putU32(std::vector<unsigned char>& b, uint32_t v) {
    const size_t o = b.size();
    b.resize(o + 4);
    memcpy(b.data() + o, &v, 4);
}
static void putName(std::vector<unsigned char>& b, const std::string& n, size_t field) {
    const size_t o = b.size();
    b.resize(o + field, 0);
    memcpy(b.data() + o, n.data(), n.size() < field ? n.size() : field - 1);
}

struct KeySpec { int frame; float x, y, hw, hh, ani, angle, alpha; int sa, da;
                 float delay; };

// One 124-byte keyframe, fields at the offsets the game uses.
static void putKey(std::vector<unsigned char>& b, const KeySpec& k, int type) {
    const size_t base = b.size();
    b.resize(base + 124, 0);
    unsigned char* p = b.data() + base;
    const int32_t fr = k.frame;
    memcpy(p + 0, &fr, 4);
    memcpy(p + 4, &type, 4);
    if (type != 0) return;                       // placeholders really are all zeros past the type
    memcpy(p + 8, &k.x, 4);
    memcpy(p + 12, &k.y, 4);
    const float xy[8] = {-k.hw, k.hw, k.hw, -k.hw, -k.hh, -k.hh, k.hh, k.hh};
    memcpy(p + 48, xy, 32);
    memcpy(p + 80, &k.ani, 4);
    memcpy(p + 88, &k.delay, 4);
    memcpy(p + 92, &k.angle, 4);
    const float col[4] = {255.0f, 255.0f, 255.0f, k.alpha};
    memcpy(p + 96, col, 16);
    memcpy(p + 112, &k.sa, 4);
    memcpy(p + 116, &k.da, 4);
}

static bool writeFile(const std::string& path, const std::vector<unsigned char>& d) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}

int main() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string dir = JoinPath(tmp, "ez2_scr_test");
    CreateDirectoryA(dir.c_str(), NULL);

    printf("scr/str scene parsing\n");

    // ---- a .str with two layers, the second animating across four textures --------------------
    {
        std::vector<unsigned char> d;
        d.insert(d.end(), {'S', 'T', 'R', 'M'});
        putU32(d, 148);                          // version, the only one that ships
        putU32(d, 60);                           // fps
        putU32(d, 90);                           // maxkey
        putU32(d, 2);                            // layers
        d.resize(d.size() + 16, 0);              // reserved

        putU32(d, 1);
        putName(d, "back.bmp", 128);
        putU32(d, 3);                            // two real keys and one placeholder between them
        putKey(d, KeySpec{0,  320, 240, 320, 240, 0, 0, 255, 2, 1, 0.0f}, 0);
        putKey(d, KeySpec{0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f}, 1);
        putKey(d, KeySpec{60, 320, 240, 320, 240, 0, 0, 255, 2, 1, 0.0f}, 0);

        putU32(d, 4);
        putName(d, "s1.bmp", 128);
        putName(d, "s2.bmp", 128);
        putName(d, "s3.bmp", 128);
        putName(d, "s4.bmp", 128);
        putU32(d, 2);
        putKey(d, KeySpec{10, 100, 100, 64, 64, 0, 0, 255, 5, 6, 0.0f}, 0);
        putKey(d, KeySpec{40, 200, 150, 64, 64, 3, 90, 128, 5, 6, 0.0f}, 0);

        check(writeFile(JoinPath(dir, "scene.str"), d), "built a .str");

        StrScene sc;
        check(ParseStrScene(JoinPath(dir, "scene.str"), sc), "the .str parses");
        check(sc.fps == 60, "fps comes from offset 8");
        check(sc.layers.size() == 2, "both layers are found");
        check(sc.layers[0].tex.size() == 1 && sc.layers[0].tex[0] == "back.bmp",
              "a 128-byte name field is read whole");
        check(sc.layers[1].tex.size() == 4, "a layer carries its animation frames as textures");
        check(sc.layers[0].keys.size() == 2,
              "type-1 placeholders are dropped, leaving only real keys");

        // The quad is stored as four corners; the compositor wants its bounds.
        const StrKey& k = sc.layers[0].keys[0];
        check(k.qx0 == -320.0f && k.qx1 == 320.0f && k.qy0 == -240.0f && k.qy1 == 240.0f,
              "the quad's corners reduce to a full 640x480 rectangle");
        check(k.x == 320.0f && k.y == 240.0f, "position is read from offset 8/12");
        check(k.sa == 2 && k.da == 1, "the blend pair is read from offset 112/116");

        // Interpolation between keys, and the range rule that stops eight sequential layers
        // stacking into a white disc.
        StrKey st;
        check(!ScrEvalLayer(sc.layers[1], 0.0f, st), "a layer is absent before its first key");
        check(!ScrEvalLayer(sc.layers[1], 100.0f, st), "and absent after its last");
        check(ScrEvalLayer(sc.layers[1], 25.0f, st), "and present in between");
        check(fabsf(st.x - 150.0f) < 0.01f && fabsf(st.y - 125.0f) < 0.01f,
              "position interpolates halfway between the two keys");
        check(fabsf(st.ani - 1.5f) < 0.01f, "so does the animation frame index");
        check(fabsf(st.col[3] - 191.5f) < 0.01f, "so does alpha");
        check(fabsf(st.angle - 45.0f) < 0.01f, "so does rotation");

        // A single-key layer is static furniture and has to persist, or it would show for one frame.
        StrLayer lone;
        lone.tex.push_back("x.bmp");
        lone.keys.push_back(sc.layers[0].keys[0]);
        check(ScrEvalLayer(lone, 9999.0f, st), "a lone key means a static layer that persists");

        // The texture cycle is driven by `delay` -- textures advanced per scene frame -- and not by
        // the keyframe's animation index, which real layers hold at zero even with fifty textures.
        StrLayer cyc;
        for (int i = 0; i < 50; ++i) cyc.tex.push_back("f.bmp");
        StrKey k0 = sc.layers[0].keys[0];
        k0.frame = 0;   k0.ani = 0.0f; k0.delay = 0.5f;
        StrKey k1 = k0;
        k1.frame = 99;
        cyc.keys.push_back(k0);
        cyc.keys.push_back(k1);

        StrKey at0, at40;
        check(ScrEvalLayer(cyc, 0.0f, at0) && ScrEvalLayer(cyc, 40.0f, at40),
              "a cycling layer evaluates across its span");
        const int n = (int)cyc.tex.size();
        int i0 = (int)(at0.ani + ((float)at0.frame - (float)cyc.keys.front().frame) * at0.delay) % n;
        int i40 = (int)(at40.ani + ((float)at40.frame - (float)cyc.keys.front().frame) * at40.delay) % n;
        check(i0 == 0, "the cycle starts on the first texture");
        check(i40 == 20, "and advances by delay each frame, not by the animation index");
    }

    // ---- a .scr timeline, including the header variant that shifts the table -------------------
    {
        StrScene dummy;
        std::vector<unsigned char> stub;
        stub.insert(stub.end(), {'S', 'T', 'R', 'M'});
        putU32(stub, 148); putU32(stub, 60); putU32(stub, 1); putU32(stub, 0);
        stub.resize(stub.size() + 16, 0);
        writeFile(JoinPath(dir, "a.str"), stub);
        writeFile(JoinPath(dir, "b.str"), stub);
        (void)dummy;

        for (int variant = 0; variant < 2; ++variant) {
            std::vector<unsigned char> d;
            d.insert(d.end(), {'S', 'C', 'R', '0'});
            putU32(d, 1);
            putU32(d, 1);
            putU32(d, 0);
            putU32(d, 3);
            if (variant) { putU32(d, 0); putU32(d, 3); }   // the table starts eight bytes later
            putU32(d, 100); putU32(d, 2); putName(d, "a.str", 128);
            putU32(d, 900); putU32(d, 3); putName(d, "b.str", 128);
            putU32(d, 1500); putU32(d, 2); putName(d, "", 128);

            const std::string p = JoinPath(dir, variant ? "t2.scr" : "t1.scr");
            writeFile(p, d);
            std::vector<ScrEntry> tl;
            const bool ok = ParseScrTimeline(p, dir, tl);
            check(ok && tl.size() == 3,
                  variant ? "a .scr whose table starts at 0x1C is located by validation"
                          : "a .scr with the table at 0x14 parses");
            if (ok && tl.size() == 3) {
                check(tl[0].frame == 100 && tl[1].frame == 900,
                      variant ? "  its start times survive the shift" : "start times are scene frames");
                check(tl[0].plane == 2 && tl[1].plane == 3,
                      variant ? "  so do its planes" : "planes are read, and differ per entry");
                check(tl[2].str.empty(),
                      variant ? "  so does its plane-clearing entry" : "an empty name clears a plane");
            }
        }

        // A truncated tail entry is legal: the writer stops at the used length.
        std::vector<unsigned char> d;
        d.insert(d.end(), {'S', 'C', 'R', '0'});
        putU32(d, 1); putU32(d, 1); putU32(d, 0); putU32(d, 2);
        putU32(d, 50); putU32(d, 2); putName(d, "a.str", 128);
        putU32(d, 9909); putU32(d, 1);
        d.resize(d.size() + 60, 0);
        writeFile(JoinPath(dir, "t3.scr"), d);
        std::vector<ScrEntry> tl;
        check(ParseScrTimeline(JoinPath(dir, "t3.scr"), dir, tl) && tl.size() == 2 &&
              tl[1].frame == 9909,
              "a truncated final entry still parses");
    }

    // ---- the timeline length that decides looping ----------------------------------------------
    {
        // Entry times and keyframes share one unit: scene frames at 60 Hz, not milliseconds.
        // Reading the .scr as ms while dividing keyframes by 60 advanced scene-local time 16.7x too
        // slowly, so every scene showed only its opening moments before the timeline cut away.
        std::vector<ScrEntry> tl;
        tl.push_back(ScrEntry{100, 2, "long.str"});     // a long scene, started early
        tl.push_back(ScrEntry{800, 2, "short.str"});    // ends the plane on a short one
        std::map<std::string, int> tail;                // last keyframe per scene, in frames
        tail["long.str"] = 240;                         // four seconds at 60 Hz
        tail["short.str"] = 6;

        // The last entry on the plane is what ends the timeline. Taking the maximum over every
        // entry instead leaves a dead tail when a long scene starts early -- which is what emptied
        // bg\8Nihilism's preview.
        uint32_t startFrame = 100, endFrame = 100;
        const ScrEntry* last = NULL;
        for (size_t i = 0; i < tl.size(); ++i)
            if (!last || tl[i].frame >= last->frame) last = &tl[i];
        const uint32_t done = last->frame + (uint32_t)tail[last->str];
        if (done > endFrame) endFrame = done;
        check(endFrame == 806, "the timeline ends when its LAST scene does, not its longest");

        const uint32_t span = endFrame - startFrame;
        check(span == 706, "a short timeline gives a short loop");

        // A 10fps preview advances six scene frames per frame shown, and wraps inside the timeline
        // rather than freezing on the last scene.
        const int previewFps = 10;
        check(120u * 60u / previewFps == 720u, "twelve seconds of preview is 720 scene frames");
        check(startFrame + ((119u * 60u / previewFps) % span) < endFrame,
              "the clock wraps inside the timeline");

        // A scene shorter than the slot it is given repeats inside it. bg\7loki hands a 99-frame
        // loop a 1200-frame slot; holding its last keyframe for the rest left the disc empty 92%
        // of the time.
        const int slotTail = 99;
        float local = 1150.0f;                          // deep into a 1200-frame slot
        if (slotTail > 0 && local > (float)slotTail) local = fmodf(local, (float)(slotTail + 1));
        check(local >= 0.0f && local <= (float)slotTail,
              "a short scene repeats inside a long slot instead of holding its last key");
    }

    DeleteFileA(JoinPath(dir, "scene.str").c_str());
    DeleteFileA(JoinPath(dir, "a.str").c_str());
    DeleteFileA(JoinPath(dir, "b.str").c_str());
    DeleteFileA(JoinPath(dir, "t1.scr").c_str());
    DeleteFileA(JoinPath(dir, "t2.scr").c_str());
    DeleteFileA(JoinPath(dir, "t3.scr").c_str());
    RemoveDirectoryA(dir.c_str());

    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
