// Round-trip check for the native-model cache format added to bms_scanner.cpp.
// The cache lets a cabinet skip the ~165k-file SOUND walk on every cold boot, so a silent format
// bug here would not fail loudly — it would quietly feed a wrong/empty model to chart generation.
// The struct, the field visitor and both cache functions are extracted verbatim; only the path is
// a parameter here instead of GetExeDirectory()/Config().cacheRoot.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o native_model_cache_test tests/native_model_cache_test.cpp
//   ./native_model_cache_test
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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
    std::array<uint64_t, 64> primaryTrackNotes{};
    std::array<uint64_t, 64> secondTrackNotes{};
    std::array<uint64_t, 64> scratchTrackNotes{};
    std::array<uint64_t, 64> pedalTrackNotes{};
};

static std::map<std::string, NativeModeModel> g_nativeModeModels;

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

static void writeNativeModelCache(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
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

static bool readNativeModelCache(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
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

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// Deterministic filler so every field gets a distinct value: a field that the writer or the reader
// skips, or that the two order differently, lands on a neighbour's number and is caught.
static NativeModeModel makeModel(const std::string& mode, uint64_t seed) {
    NativeModeModel m;
    m.mode = mode;
    m.charts = static_cast<uint32_t>(seed * 7 + 1);
    uint64_t n = seed * 1000;
    visitNativeModelFields(m, [&](uint64_t& v) { v = ++n; });
    return m;
}

int main() {
    const std::string path = "native_model_cache_test.tmp";

    g_nativeModeModels["5keyMix"] = makeModel("5keyMix", 1);
    g_nativeModeModels["cv2Mix"] = makeModel("cv2Mix", 2);   // the mode that never reaches the cap
    g_nativeModeModels["turnTable"] = makeModel("turnTable", 3);
    std::map<std::string, NativeModeModel> expected = g_nativeModeModels;

    writeNativeModelCache(path);
    g_nativeModeModels.clear();
    check(readNativeModelCache(path), "cache reads back");
    check(g_nativeModeModels.size() == expected.size(), "mode count survives");

    size_t fieldsChecked = 0;
    for (auto& kv : expected) {
        auto it = g_nativeModeModels.find(kv.first);
        if (it == g_nativeModeModels.end()) { check(false, "mode present after reload"); continue; }
        check(it->second.mode == kv.second.mode, "mode name survives");
        check(it->second.charts == kv.second.charts, "charts survives");
        std::vector<uint64_t> want, got;
        visitNativeModelFields(kv.second, [&](uint64_t& v) { want.push_back(v); });
        visitNativeModelFields(it->second, [&](uint64_t& v) { got.push_back(v); });
        check(want == got, "every numeric field survives in order");
        fieldsChecked += want.size();
    }
    // 13 scalars + 16 + 16 + 64 * 4 = 301 numeric fields per mode.
    check(fieldsChecked == 301 * expected.size(), "expected field count visited");

    // A truncated cache must be rejected outright, not partially applied: half a model is worse
    // than no model, because the walk would be skipped and the gaps read as real zeros.
    g_nativeModeModels.clear();
    g_nativeModeModels["5keyMix"] = makeModel("5keyMix", 9);
    writeNativeModelCache(path);
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        std::string text; char buf[4096]; size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
        std::fclose(f);
        text.resize(text.size() / 2);                      // cut mid-model
        f = std::fopen(path.c_str(), "wb");
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    g_nativeModeModels.clear();
    check(!readNativeModelCache(path), "truncated cache is rejected");
    check(g_nativeModeModels.empty(), "rejected cache leaves the model untouched");

    // A header from a future format version must not be parsed with today's field order.
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fprintf(f, "EZ2NativeModelCache 3 1\n5keyMix 1 2 3\n");
        std::fclose(f);
    }
    check(!readNativeModelCache(path), "unknown version is rejected");

    std::remove(path.c_str());
    if (g_failures == 0) std::printf("native model cache: round-trip ok (%zu fields)\n", fieldsChecked);
    return g_failures ? 1 : 0;
}
