#include "trace_hooks.h"
#include "config.h"
#include "logger.h"
#include "patch.h"
#include "bms_scanner.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

struct TraceHook {
    const char* name;
    uintptr_t rva;
    size_t overwriteLen;
    bool enabled;
    void* target = nullptr;
    void* gateway = nullptr;
    void* stub = nullptr;
    unsigned char original[32] = {};
    bool installed = false;
    unsigned long hitCount = 0;
};

// PUSHAD layout at ESP after PUSHAD on x86:
// [0]=EDI [1]=ESI [2]=EBP [3]=original ESP [4]=EBX [5]=EDX [6]=ECX [7]=EAX
struct PushadRegs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t originalEsp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
};

static TraceHook g_hooks[] = {
    {"FUN_00423a90 Song/chart loading", 0x23A90, 6, true},
    {"FUN_00411460 Load .ez chart",     0x11460, 6, true},
    {"FUN_00411090 Load .ezi audio map",0x11090, 6, true},
    {"FUN_00426500 Judge timing",       0x26500, 6, true},
    {"FUN_00426700 Tap judge",          0x26700, 8, true},
    {"FUN_004268C0 LN/release judge",   0x268C0, 8, true},
    {"FUN_00426AB0 Miss sweep",         0x26AB0, 6, true},
    {"FUN_00451F10 Result/score",       0x51F10, 7, true},
    {"FUN_00456B20 Ranking song top5",  0x56B20, 6, true},
    {"FUN_004570A0 Ranking global top99",0x570A0,6, true},
    {"FUN_00457A90 Ranking UI",         0x57A90, 7, true},
    {"FUN_00433D50 SongSelect top5 draw",0x33D50,9, true},
    {"FUN_00435070 SongSelect list/filter",0x35070,5, true},
    // 0x35390 hook tried & REVERTED: at its ENTRY the master array isn't built yet
    // (435390 itself parses ALLSONG then preloads), so our inject bailed (master=0) and
    // corrupted a half-built object → CRT string crash. The preload is mid-435390.
    // 0x31660 removed for the diagnostic pass — it drove the fabricated-entry inject.
};

static bool g_traceInstalled = false;

struct CategoryMemoryEdit {
    uintptr_t rva;
    uint32_t expected;
    uint32_t replacement;
    uint32_t original;
    size_t length;
    bool installed;
};

// ponytail: x86 uses signed imm8 loop bounds, so 127 total categories is the native ceiling.
static uint32_t g_categoryTextures[127] = {};
static unsigned char g_categoryBaseOriginal[2][6] = {};
static bool g_categoryBaseInstalled = false;
static uint32_t g_customCategoryCount = 0;
static bool g_capacityPatched = false;     // master-array cap raised + relocated (+0xEFDF4)
static uint32_t g_songCapacity = 436;      // raised by InstallSongCapacityPatches

// Relocated category texture arrays (object-relative). The native arrays are EXACTLY 47
// entries: small at +0xbc4b8, big at +0xc0084, and the field right after small (+0xbc574)
// is live render state. To add custom slots we move BOTH arrays into the object's free tail,
// placed in the 0x20000 headroom the capacity patch reserves AFTER the relocated master[]
// (page-aligned). Set by InstallSongCapacityPatches (needs newCap); 0 = not relocated.
static uint32_t g_catTexSmallBase = 0;
static uint32_t g_catTexBigBase   = 0;

struct LazySongnameEntry {
    uint32_t masterEntry;
    std::string id;
};
static uint32_t g_lazySongnameObject = 0;
static std::vector<std::vector<LazySongnameEntry>> g_lazySongnames;
static std::vector<unsigned char> g_lazySongnameLoaded;

static CategoryMemoryEdit g_categoryEdits[] = {
    {0x372f0, 0x2f,       0,          0, 1, false}, // [0] loader count
    {0x322db, 0x2f,       0,          0, 1, false}, // [1] renderer count
    {0x446f6, 0x2e,       0,          0, 1, false}, // [2] forward max index (cmp eax,0x2e imm8)
    // backward wrap: `mov [esi+0xbc57c], 0x2e` = c7 86 |7c c5 0b 00| |2e 00 00 00|. The imm32
    // STARTS at 0x448d6 (0x448d9 is its last byte). length 4 so we set the full DWORD to 48.
    {0x448d6, 0x2e,       0,          0, 4, false}, // [3] backward wrap selected-index reset (imm32)
    {0x91ad0, 0x4592e000, 0,          0, 4, false}, // [4] wheel span 4700
    {0x91ad4, 0x4512e000, 0,          0, 4, false}, // [5] half span 2350
    {0x91acc, 0xc512e000, 0,          0, 4, false}, // [6] negative half span
    {0x93a94, 0xc592e800, 0,          0, 4, false}, // [7] -span-1
    {0x93a98, 0xc592d800, 0,          0, 4, false}, // [8] -span+1
    // [9..12] texture-array base relocation (disp32 in `[ebx+disp]`, length-preserving):
    {0x32224, 0xbc4b8,    0,          0, 4, false}, // [9]  small array — renderer (fun_432200)
    {0x372b7, 0xbc4b8,    0,          0, 4, false}, // [10] small array — loader (fun_43673d)
    {0x322ea, 0xc0084,    0,          0, 4, false}, // [11] big array — renderer post-loop
    {0x382a6, 0xc0084,    0,          0, 4, false}, // [12] big array — loader
};

static void EmitU8(unsigned char*& p, unsigned char v) { *p++ = v; }
static void EmitU32(unsigned char*& p, uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
static void EmitRel32(unsigned char*& p, void* fromNextInstruction, void* to) {
    intptr_t rel = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(fromNextInstruction);
    uint32_t v = static_cast<uint32_t>(rel);
    EmitU32(p, v);
}

static bool LooksLikeExecutable(void* ptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    DWORD p = mbi.Protect & 0xff;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static bool CanReadPtr(const void* ptr, size_t bytes = 4) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xff;
    if (p == PAGE_NOACCESS || p == PAGE_EXECUTE) return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return a + bytes <= base + mbi.RegionSize;
}

static uint32_t SafeRead32(uint32_t addr, uint32_t def = 0) {
    if (!CanReadPtr(reinterpret_cast<const void*>(addr), 4)) return def;
    return *reinterpret_cast<const uint32_t*>(addr);
}

static std::string SafeReadAscii(uint32_t addr, size_t maxLen = 96) {
    if (!CanReadPtr(reinterpret_cast<const void*>(addr), 1)) return "<badptr>";
    std::string out;
    out.reserve(maxLen);
    for (size_t i = 0; i < maxLen; ++i) {
        const char* p = reinterpret_cast<const char*>(addr + i);
        if (!CanReadPtr(p, 1)) break;
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) out.push_back('.');
        else out.push_back(static_cast<char>(c));
    }
    if (out.empty()) return "";
    return out;
}

static bool SafeWriteBytes(uint32_t addr, const void* src, size_t bytes) {
    if (!CanReadPtr(reinterpret_cast<void*>(addr), bytes)) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), bytes, PAGE_READWRITE, &oldProtect)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), src, bytes);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), bytes, oldProtect, &tmp);
    return true;
}

static void WriteEntrySongId(uint32_t entryBase, const std::string& id) {
    char buf[64] = {};
    lstrcpynA(buf, id.c_str(), sizeof(buf));
    SafeWriteBytes(entryBase + 0x10C, buf, sizeof(buf));
}

static void WriteEntrySongBinLikeFields(uint32_t entryBase, const CustomBmsSong& song) {
    // SongSelect entries carry a song.bin-like record at entry+0x10C.
    // Native song.bin record layout confirmed from FUN_00469c30/FUN_00469f40:
    //   +0x00 char[0x32] song id
    //   +0x32 NM level,  +0x33 dword flags/reserved, +0x37 float BPM
    //   +0x3B HD level,  +0x3C dword flags/reserved, +0x40 float BPM
    //   +0x44 SHD level, +0x45 dword flags/reserved, +0x49 float BPM
    //   +0x4D EX level,  +0x4E dword flags/reserved, +0x52 float BPM
    // The visible levels must come from each BMS chart's #PLAYLEVEL, not filename heuristics.
    uint32_t rec = entryBase + 0x10C;
    WriteEntrySongId(entryBase, song.id);

    const uint32_t levelOff[4] = {0x32, 0x3B, 0x44, 0x4D};
    const uint32_t reservedOff[4] = {0x33, 0x3C, 0x45, 0x4E};
    const uint32_t bpmOff[4] = {0x37, 0x40, 0x49, 0x52};
    unsigned char levels[4] = {
        (unsigned char)(song.levelNM  < 0 ? 0 : (song.levelNM  > 255 ? 255 : song.levelNM)),
        (unsigned char)(song.levelHD  < 0 ? 0 : (song.levelHD  > 255 ? 255 : song.levelHD)),
        (unsigned char)(song.levelSHD < 0 ? 0 : (song.levelSHD > 255 ? 255 : song.levelSHD)),
        (unsigned char)(song.levelEX  < 0 ? 0 : (song.levelEX  > 255 ? 255 : song.levelEX))
    };
    float bpms[4] = {song.bpmNM, song.bpmHD, song.bpmSHD, song.bpmEX};
    float fallback = 120.0f;
    for (int i = 0; i < 4; ++i) { if (bpms[i] > 0.0f) { fallback = bpms[i]; break; } }
    uint32_t zero = 0;
    for (int i = 0; i < 4; ++i) {
        float bpm = bpms[i] > 0.0f ? bpms[i] : fallback;
        SafeWriteBytes(rec + levelOff[i], &levels[i], 1);
        SafeWriteBytes(rec + reservedOff[i], &zero, 4);
        SafeWriteBytes(rec + bpmOff[i], &bpm, 4);
    }
}

static bool EntryIdEquals(uint32_t entryBase, const std::string& id) {
    std::string cur = SafeReadAscii(entryBase + 0x10C, 64);
    return _stricmp(cur.c_str(), id.c_str()) == 0;
}


// v20: normal SongSelect categories are parsed from system\<mode>\Stage.ini inside FUN_00435070.
// Decomp notes for non-radio modes:
//   - ALLSONG is copied to master id records at ecx + 0x5e3c8 + idx*0x374.
//   - HID-HD%d category labels are copied into records of 9 dwords:
//       name   at int-index 0x836a0 + idx*9  (32-byte string area)
//       row/id at int-index 0x836a8 + idx*9  (the Stage.ini row that owns the label)
//       count  at int-index 0x83a24.
// These are high offsets because the decompiler typed ECX as int*. We write them as byte
// addresses ecx + intIndex*4. This is still a memory-only overlay: Stage.ini is untouched.
static uint32_t ObjDwordIndexedAddress(uint32_t ecx, uint32_t intIndex) {
    return ecx + intIndex * 4u;
}

static bool WriteObjectDwordIndexed(uint32_t ecx, uint32_t intIndex, const void* src, size_t bytes) {
    return SafeWriteBytes(ObjDwordIndexedAddress(ecx, intIndex), src, bytes);
}

static uint32_t ReadObjectDwordIndexedU32(uint32_t ecx, uint32_t intIndex, uint32_t def = 0) {
    return SafeRead32(ObjDwordIndexedAddress(ecx, intIndex), def);
}

static std::string ReadObjectDwordIndexedAscii(uint32_t ecx, uint32_t intIndex, size_t maxLen = 32) {
    return SafeReadAscii(ObjDwordIndexedAddress(ecx, intIndex), maxLen);
}

static std::string SanitizeCategoryName(const std::string& raw) {
    std::string out;
    out.reserve(31);
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7e) continue;
        if (c == ';' || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
        out.push_back(c);
        if (out.size() >= 31) break;
    }
    if (out.empty()) out = "CUSTOM";
    return out;
}

static std::vector<std::string> UniqueCustomCategories(const std::vector<CustomBmsSong>& songs) {
    std::vector<std::string> cats;
    for (const auto& s : songs) {
        std::string cat = SanitizeCategoryName(s.category.empty() ? "CUSTOM" : s.category);
        bool exists = false;
        for (const auto& c : cats) {
            if (_stricmp(c.c_str(), cat.c_str()) == 0) { exists = true; break; }
        }
        if (!exists) cats.push_back(cat);
    }
    return cats;
}

static std::vector<std::string> CustomCategoriesForModeOrLargest(const std::string& preferred) {
    std::vector<std::string> best;
    std::string mode = NormalizeModeName(preferred);
    if (!mode.empty()) best = GetCustomBmsCategoriesForMode(mode);
    if (!best.empty()) return best;
    const char* modes[] = {"5keyOnly","5keyMix","streetMix","7keyMix","clubMix","spaceMix","rubyMix","ez2Catch","turnTable","cv2Mix"};
    for (const char* candidate : modes) {
        std::vector<std::string> cur = GetCustomBmsCategoriesForMode(candidate);
        if (cur.size() > best.size()) best.swap(cur);
    }
    return best;
}

static void WriteVirtualCategoryDiagnostics(const std::vector<CustomBmsSong>& songs) {
    // Leave a human-readable record in each CustomCache folder so the Stage.ini overlay
    // can be verified even though the real Stage.ini is never modified.
    for (const auto& s : songs) {
        if (s.cacheDir.empty()) continue;
        std::string cat = SanitizeCategoryName(s.category.empty() ? "CUSTOM" : s.category);
        std::string text;
        text += "; EZ2CustomMod v20 virtual category overlay.\r\n";
        text += "; Original system\\" + s.mode + "\\Stage.ini and song.bin are not modified.\r\n";
        text += "Mode=" + s.mode + "\r\n";
        text += "Category=" + cat + "\r\n";
        text += "SongId=" + s.id + "\r\n";
        text += "Title=" + s.title + "\r\n";
        text += "Artist=" + s.artist + "\r\n";
        text += "Levels=" + std::to_string(s.levelNM) + ";" + std::to_string(s.levelHD) + ";" + std::to_string(s.levelSHD) + ";" + std::to_string(s.levelEX) + "\r\n";
        std::string path = s.cacheDir + "\\virtual_stage_category_" + cat + ".ini";
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
            CloseHandle(h);
        }
    }
}

static void InjectCustomStageCategories(uint32_t ecx, const std::vector<CustomBmsSong>& songs, const char* phase) {
    if (songs.empty() || !ecx) return;

    // Only standard songselect modes use this Stage.ini category table. Radio-style modes
    // use channel_normal/channel_secret and different arrays.
    const std::string mode = GetActiveMode();
    if (_stricmp(mode.c_str(), "5keyMix") != 0 && _stricmp(mode.c_str(), "5keyOnly") != 0 &&
        _stricmp(mode.c_str(), "streetMix") != 0 &&
        _stricmp(mode.c_str(), "7keyMix") != 0 && _stricmp(mode.c_str(), "clubMix") != 0 &&
        _stricmp(mode.c_str(), "spaceMix") != 0 && _stricmp(mode.c_str(), "rubyMix") != 0 &&
        _stricmp(mode.c_str(), "ez2Catch") != 0 && _stricmp(mode.c_str(), "turnTable") != 0 &&
        _stricmp(mode.c_str(), "cv2Mix") != 0) {
        // If mode detection was empty, still attempt for 5keyMix scans because the object
        // itself is already inside FUN_00435070.
        if (!mode.empty()) return;
    }

    const uint32_t kCatCountIndex = 0x83a24;
    const uint32_t kCatNameBaseIndex = 0x836a0;
    const uint32_t kCatRowBaseIndex  = 0x836a8;
    const uint32_t kCatStrideDwords = 9;
    const uint32_t kMaxCats = 100;

    uint32_t catCountAddr = ObjDwordIndexedAddress(ecx, kCatCountIndex);
    if (!CanReadPtr(reinterpret_cast<void*>(catCountAddr), 4)) {
        LogLine("[category] %s skip: Stage.ini category count unreadable ecx=%08lx addr=%08lx note='FUN_00435070 category table not resident yet'",
                phase, static_cast<unsigned long>(ecx), static_cast<unsigned long>(catCountAddr));
        return;
    }

    uint32_t count = ReadObjectDwordIndexedU32(ecx, kCatCountIndex, 0);
    if (count > kMaxCats) count = 0;

    auto cats = UniqueCustomCategories(songs);
    uint32_t added = 0;
    for (const auto& cat : cats) {
        bool exists = false;
        uint32_t existingIndex = 0;
        for (uint32_t i = 0; i < count && i < kMaxCats; ++i) {
            std::string cur = ReadObjectDwordIndexedAscii(ecx, kCatNameBaseIndex + i * kCatStrideDwords, 32);
            if (_stricmp(cur.c_str(), cat.c_str()) == 0) {
                exists = true;
                existingIndex = i;
                break;
            }
        }
        if (exists) {
            LogLine("[category] %s exists name='%s' index=%lu count=%lu", phase, cat.c_str(),
                    static_cast<unsigned long>(existingIndex), static_cast<unsigned long>(count));
            continue;
        }
        if (count >= kMaxCats) break;

        char nameBuf[32] = {};
        lstrcpynA(nameBuf, cat.c_str(), sizeof(nameBuf));
        uint32_t rowIndex = 0x80000000u | count; // synthetic row marker; not a real Stage.ini row.
        uint32_t nameIndex = kCatNameBaseIndex + count * kCatStrideDwords;
        uint32_t rowIndexAddr = kCatRowBaseIndex + count * kCatStrideDwords;
        bool okName = WriteObjectDwordIndexed(ecx, nameIndex, nameBuf, sizeof(nameBuf));
        bool okRow  = WriteObjectDwordIndexed(ecx, rowIndexAddr, &rowIndex, sizeof(rowIndex));
        if (okName && okRow) {
            count++;
            WriteObjectDwordIndexed(ecx, kCatCountIndex, &count, sizeof(count));
            added++;
            LogLine("[category] %s injected name='%s' index=%lu syntheticRow=0x%08lx count=%lu note='from BMSData\\%s\\%s'",
                    phase, cat.c_str(), static_cast<unsigned long>(count - 1), static_cast<unsigned long>(rowIndex),
                    static_cast<unsigned long>(count), songs[0].modeFolder.c_str(), cat.c_str());
        } else {
            LogLine("[category] %s failed name='%s' nameAddr=%08lx rowAddr=%08lx okName=%d okRow=%d",
                    phase, cat.c_str(), static_cast<unsigned long>(ObjDwordIndexedAddress(ecx, nameIndex)),
                    static_cast<unsigned long>(ObjDwordIndexedAddress(ecx, rowIndexAddr)), okName ? 1 : 0, okRow ? 1 : 0);
        }
    }

    if (added > 0) {
        WriteVirtualCategoryDiagnostics(songs);
        LogLine("[category] %s virtual Stage.ini category overlay added=%lu total=%lu first='%s' originalStageIniUntouched=1",
                phase, static_cast<unsigned long>(added), static_cast<unsigned long>(count), cats.empty() ? "" : cats[0].c_str());
    }
}


static bool SafeMoveBytes(uint32_t dst, uint32_t src, size_t bytes) {
    if (bytes == 0) return true;
    if (!CanReadPtr(reinterpret_cast<void*>(dst), bytes)) return false;
    if (!CanReadPtr(reinterpret_cast<void*>(src), bytes)) return false;
    uint32_t lo = dst < src ? dst : src;
    uint32_t hi = (dst + bytes) > (src + bytes) ? (dst + bytes) : (src + bytes);
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(lo), hi - lo, PAGE_READWRITE, &oldProtect)) return false;
    std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), bytes);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(lo), hi - lo, oldProtect, &tmp);
    return true;
}


static void PinCustomSongInMasterAtTop(uint32_t ecx, const CustomBmsSong& song, uint32_t& master, uint32_t& injectedMaster) {
    const uint32_t kEntrySize = 0x374;
    const uint32_t kCapacity = 436;
    uint32_t masterBase = ecx + 0x5e2bc;
    if (!CanReadPtr(reinterpret_cast<void*>(masterBase), kEntrySize)) return;
    if (master > kCapacity) return;

    int existingIndex = -1;
    for (uint32_t i = 0; i < master && i < kCapacity; ++i) {
        if (EntryIdEquals(masterBase + i * kEntrySize, song.id)) {
            existingIndex = static_cast<int>(i);
            break;
        }
    }

    unsigned char temp[0x374] = {};
    if (existingIndex == 0) {
        WriteEntrySongBinLikeFields(masterBase, song);
        return;
    }

    if (existingIndex > 0) {
        std::memcpy(temp, reinterpret_cast<void*>(masterBase + existingIndex * kEntrySize), kEntrySize);
        if (SafeMoveBytes(masterBase + kEntrySize, masterBase, static_cast<size_t>(existingIndex) * kEntrySize)) {
            SafeWriteBytes(masterBase, temp, kEntrySize);
            WriteEntrySongBinLikeFields(masterBase, song);
        }
        return;
    }

    // Clone the current first native row as a complete structural template, then move
    // the native master table down and put the virtual song.bin-like record at index 0.
    // This is still memory-only; system\\<mode>\\song.bin is not modified.
    if (master >= kCapacity) return;
    std::memcpy(temp, reinterpret_cast<void*>(masterBase), kEntrySize);
    if (SafeMoveBytes(masterBase + kEntrySize, masterBase, static_cast<size_t>(master) * kEntrySize)) {
        SafeWriteBytes(masterBase, temp, kEntrySize);
        WriteEntrySongBinLikeFields(masterBase, song);
        master++;
        injectedMaster++;
    }
}

static void PinCustomSongAtTop(uint32_t ecx, const CustomBmsSong& song, uint32_t& visible, uint32_t& injectedVisible) {
    const uint32_t kEntrySize = 0x374;
    const uint32_t kCapacity = 436;
    uint32_t visibleBase = ecx + 0x12c;
    uint32_t cloneMaster = ecx + 0x5e2bc;

    // v23 allows rebuilding an empty visible table from master[0].
    if (!CanReadPtr(reinterpret_cast<void*>(visibleBase), kEntrySize)) return;

    int existingIndex = -1;
    for (uint32_t i = 0; i < visible && i < kCapacity; ++i) {
        if (EntryIdEquals(visibleBase + i * kEntrySize, song.id)) {
            existingIndex = static_cast<int>(i);
            break;
        }
    }

    unsigned char temp[0x374] = {};
    if (existingIndex >= 0) {
        std::memcpy(temp, reinterpret_cast<void*>(visibleBase + existingIndex * kEntrySize), kEntrySize);
        if (existingIndex > 0) {
            SafeMoveBytes(visibleBase + kEntrySize, visibleBase, static_cast<size_t>(existingIndex) * kEntrySize);
            SafeWriteBytes(visibleBase, temp, kEntrySize);
        }
    } else {
        if (visible >= kCapacity) return;
        // Clone master[0], which v19 has already turned into the virtual song.bin-like row.
        // This avoids visible[0] inheriting a random native song template.
        uint32_t clone = cloneMaster;
        if (!CanReadPtr(reinterpret_cast<void*>(clone), kEntrySize)) clone = visibleBase;
        std::memcpy(temp, reinterpret_cast<void*>(clone), kEntrySize);
        if (SafeMoveBytes(visibleBase + kEntrySize, visibleBase, static_cast<size_t>(visible) * kEntrySize)) {
            SafeWriteBytes(visibleBase, temp, kEntrySize);
            WriteEntrySongBinLikeFields(visibleBase, song);
            visible++;
            injectedVisible++;
        }
    }

    // Make the custom row immediately actionable.
    WriteEntrySongBinLikeFields(visibleBase, song);
    if (Config().forceSelectCustom) {
        uint32_t zero = 0;
        SafeWriteBytes(ecx + 0x84, &zero, 4); // selected index
        SafeWriteBytes(ecx + 0x8c, &zero, 4); // previous selected index
    }
}

// Native category tables (confirmed globals, image base 0x00400000, only read by the
// category filter FUN_00435070; never written by the game, so safe to populate):
//   count[c]      = *(int*)(0x03789bd4 + c*4)
//   idstr[c][k]   =  (char*)(0x03789c90 + (k + c*0x1b4)*0x10)   // 0x10 bytes/id, NUL-term
// The filter compares each idstr against master[i].name at ecx+0x5e2bc + i*0x374 + 0x10C
// ( == ecx+0x5e3c8 for i=0 ). So custom songs must ALSO exist in master[] with their id.
static const uint32_t kCatCountTableBase = 0x03789bd4;
static const uint32_t kCatNameTableBase  = 0x03789c90;
static const uint32_t kCatNameStride = 0x1b4; // entries per category
static const uint32_t kCatNameSize   = 0x10;  // bytes per id string
// When the capacity patch ran, the game's filter (FUN_00435070 @ 0x35093/0x35112) reads these
// category globals from a SHIFTED base (+0x56*(cap-436)); our inject writes the same shifted base.
// ponytail: with many custom categories the shifted writes still overrun .data globals (crash
// 0x43858e). The real fix is relocating these tables to our own buffer, but that surfaced a deeper
// wheel-render null-sub-object crash (0x4466d1) — needs a live debugger; reverted for now.
// Native tables hold only 47 categories; custom categories (47+) overran .data globals (crash
// 0x43858e). Relocate count+id to our own 256-category buffer and repoint the filter's two
// table-base immediates (0x35093/0x35112, absolute addressing). Copy the native 0..46 from the
// CURRENT (Group-B-shifted) base where the game populated/reads them. 0 = not yet relocated.
static uint32_t g_catCountBuf = 0;
static uint32_t g_catIdBuf    = 0;
static uint32_t CatShift() { return 0u; } // Group B removed → tables stay at native base (no shift)
static uint32_t CatCountTable() { return g_catCountBuf ? g_catCountBuf : (kCatCountTableBase + CatShift()); }
static uint32_t CatNameTable()  { return g_catIdBuf    ? g_catIdBuf    : (kCatNameTableBase  + CatShift()); }
static void EnsureCategoryTablesRelocated() {
    if (g_catIdBuf) return;
    const uint32_t kCats = 256;
    void* cnt = VirtualAlloc(nullptr, kCats * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* ids = VirtualAlloc(nullptr, kCats * kCatNameStride * kCatNameSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!cnt || !ids) { LogLine("[category] reloc alloc FAILED"); return; }
    uint32_t shift = CatShift();
    memcpy(cnt, reinterpret_cast<void*>(kCatCountTableBase + shift), 47u * 4u);
    memcpy(ids, reinterpret_cast<void*>(kCatNameTableBase  + shift), 47u * kCatNameStride * kCatNameSize);
    uint32_t c = reinterpret_cast<uint32_t>(cnt), i = reinterpret_cast<uint32_t>(ids);
    bool ok = WriteMemory(Rva(0x35093), &c, 4) && WriteMemory(Rva(0x35112), &i, 4);
    g_catCountBuf = c; g_catIdBuf = i;
    LogLine("[category] tables relocated count=%08lx id=%08lx ok=%d (copied 47 native from shift=0x%lx)",
            (unsigned long)c, (unsigned long)i, ok ? 1 : 0, (unsigned long)shift);
}

static void RefreshRelocatedNativeCategoryTables() {
    if (!g_catCountBuf || !g_catIdBuf) return;
    // Native categories 0..46 can change per mode, so re-sync them from the live native globals (using
    // the same CatShift() base the one-shot relocation copied from). Slots 47+ are custom categories,
    // OWNED by InjectCustomSongsIntoSongSelect which runs right after — do NOT zero them here, or the
    // second SongSelect build (mode switch) wipes every custom category to count 0 → the game's filter
    // divides by that count on category change → STATUS_INTEGER_DIVIDE_BY_ZERO (0xc0000094).
    uint32_t shift = CatShift();
    std::memcpy(reinterpret_cast<void*>(g_catCountBuf), reinterpret_cast<void*>(kCatCountTableBase + shift), 47u * 4u);
    std::memcpy(reinterpret_cast<void*>(g_catIdBuf), reinterpret_cast<void*>(kCatNameTableBase + shift),
                47u * kCatNameStride * kCatNameSize);
}

// One-shot: append custom songs to the master array and populate the native category
// tables so the game's own filter lists them under their custom category. After this the
// mod never touches visible[] — the native filter rebuilds it correctly on every entry.
static void InjectCustomSongsIntoSongSelect(uint32_t ecx, const char* phase, uint32_t categoryParam) {
    (void)categoryParam;
    if (!Config().enableSongSelectInject || !Config().enableBmsData) return;
    if (GetActiveMode().empty()) return;
    std::vector<CustomBmsSong> allSongs = GetCustomBmsSongsForActiveMode();
    if (allSongs.empty()) {
        static unsigned long emptyLogged = 0;
        if (emptyLogged++ < 5) {
            LogLine("[custom] no BMSData songs for activeMode='%s'", GetActiveMode().c_str());
            // InstallCustomCategoryPatches extends the wheel to 47+N categories GLOBALLY, not per mode.
            // With no songs here those slots stay navigable, resolve to 0 visible entries, and the game
            // divides by that count (0xc0000094 at 0x441911, seen entering SpaceMix). Autogen normally
            // populates every target mode — if it did not, say so instead of arming a silent crash.
            if (g_customCategoryCount)
                LogLine("[custom] WARNING: wheel still advertises %lu custom categories in this mode; they resolve"
                        " to 0 songs and the game divides by that count (0xc0000094). Add this mode to"
                        " [AutoChart] TargetModes so autogen populates it.", (unsigned long)g_customCategoryCount);
        }
        return;
    }
    if (!ecx || !CanReadPtr(reinterpret_cast<void*>(ecx), 0x90)) return;

    const uint32_t kEntrySize = 0x374;
    uint32_t kCapacity = g_songCapacity; // raised from 436; clamped below to committed memory
    // CONFIRMED total-song ceiling: above ~2000 total, the game's SongSelect throws an uncaught C++
    // exception on category change → abort(3) (the per-category count and the 9 categories are fine;
    // only the TOTAL matters — almost certainly the texture preload exhausting the pool/VRAM: ~2212
    // songs loaded 4358 surfaces / 316MB). 1900 is the proven-safe ceiling. ponytail: lifting this
    // needs LAZY/windowed song-texture loading (don't preload all songs' disc/songname), not another
    // size bump — the game preloads every song's textures, which is the real wall.
    // PRACTICAL CEILING = 1900 (proven: all 9 categories navigable, no crash). Bisect found the wall:
    // ~2066 master AVs at 0x404972 (movss [ecx+0x80] on a null/garbage song-entry ptr) on category
    // change — a DIFFERENT wheel/visible structure than the earlier abort, NOT textures (only 656
    // surfaces). Each higher count surfaces yet another fragile fixed structure. ponytail: this is the
    // engine's practical ceiling; going beyond is one-RE-battle-per-crash with diminishing returns,
    // and 100k-song scale needs a from-scratch dynamic SongSelect (beatoraja), not patching EZ2AC.
    if (kCapacity > 60000u) kCapacity = 60000u; // 64MB object inflate + LAA 4GB; real walls now = the
                                                // songname pool (~7000 titles), categories (<=80), memory
    uint32_t master = SafeRead32(ecx + 0x80);
    // master[] base: when the capacity patch relocated the array by +0xEFDF4, the game now
    // reads/writes master at ecx+0x5e2bc+0xEFDF4, so our inject MUST target the same place.
    uint32_t masterBase = ecx + 0x5e2bc + (g_capacityPatched ? 0xEFDF4u : 0u);
    if (master == 0 || master > kCapacity || !CanReadPtr(reinterpret_cast<void*>(masterBase), kEntrySize)) return;

    // Idempotency by DATA: the heap reuses ecx across SongSelect recreate, AND this hook
    // fires multiple times per object. Find our injection by scanning master[] for the
    // first custom id (natives never start with our id prefix). If already present, RE-RUN
    // would (a) read OOB when master==cap and (b) zero the category tables → div-by-zero
    // crash (0x440439). So: locate the native baseline = first slot in [0,master) holding a
    // custom id; if found, we're already injected → bail. Else inject starting at `master`.
    if (!allSongs.empty()) {
        const std::string& firstId = allSongs.front().id;
        for (uint32_t i = 0; i < master; ++i) {
            std::string id = SafeReadAscii(masterBase + i * kEntrySize + 0x10C, 32);
            if (_stricmp(id.c_str(), firstId.c_str()) == 0) return; // already injected
        }
    }

    // The object is grown by the VirtualAlloc-inflate hook (mod_main) to 8MB. Here we just clamp
    // kCapacity to the object's actually-committed extent so the append + the game's preload never
    // touch memory outside it. CRITICAL: only commit pages that are RESERVED *within the object's
    // own region*; NEVER VirtualAlloc(COMMIT) on MEM_FREE addresses — that creates new regions past
    // the object and corrupts the address space (ntdll crash 0x77xxxxxx). Stop at the region end.
    {
        uintptr_t want = (uintptr_t)masterBase + (uintptr_t)kCapacity * kEntrySize + 0x10000;
        uintptr_t got  = ecx;
        for (uintptr_t p = (uintptr_t)ecx & ~uintptr_t(0xFFF); p < want; p += 0x1000) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery((void*)p, &m, sizeof(m)) || m.State == MEM_FREE) break; // past the object
            if (m.State == MEM_RESERVE && !VirtualAlloc((void*)p, 0x1000, MEM_COMMIT, PAGE_READWRITE)) break;
            got = p + 0x1000;
        }
        uint32_t committedCap = (got > masterBase) ? (uint32_t)((got - masterBase) / kEntrySize) : 0;
        if (committedCap < kCapacity) kCapacity = committedCap;
        static uint32_t s_grow = 0;
        if (s_grow++ < 3) {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery((void*)ecx, &mbi, sizeof(mbi));
            LogLine("[grow] ecx=%08lx allocBase=%08lx committedTo=+0x%lx capacity=%lu regionSize=0x%lx state=0x%lx",
                    (unsigned long)ecx, (unsigned long)(uintptr_t)mbi.AllocationBase,
                    (unsigned long)(got - ecx), (unsigned long)kCapacity,
                    (unsigned long)mbi.RegionSize, (unsigned long)mbi.State);
        }
    }

    // Confirmed layout (diag [diag] mstr[] vs Hex-Rays sub_404950): a master entry holds
    // the id at +0x10C and NM/HD/SHD/EX as FLOAT levels at +0x214/0x220/0x22C/0x238.
    // We write ONLY those fields into appended slots — NO babydance clone (that copied
    // chart/disc pointers from +0x000 → wrong difficulties + crash on re-render). The
    // real chart/disc load happens via the file-redirect hooks when the song is selected.
    // A valid 0x374 entry has many pre-built sub-object pointers (+0/+4 texture objects,
    // etc.) that the wheel render + draw (fun_432200/fun_403f80) dereference deeply. Building
    // them from scratch needs the full song-load pipeline. Pragmatic: CLONE a native entry's
    // full struct (draw-safe, already built) and override only the display-independent fields
    // — id (+0x10C) and level floats (+0x214/0x220/0x22C/0x238). The actual song still loads
    // via the file-redirect hooks on selection; this clone is only the list/wheel struct.
    // (Earlier clone crashed on gameplay-return via the wheel TEXTURE-REDIRECT patch, now
    // removed — the clone itself is draw-safe.) ponytail: shared native visuals until a
    // per-song entry builder exists.
    unsigned char nativeTemplate[0x374];
    memcpy(nativeTemplate, reinterpret_cast<void*>(masterBase), 0x374);
    // Custom SongName textures are intentionally NOT loaded here. FUN_00435070 tells us which
    // category is about to be filtered, so LoadCustomCategorySongnames loads only that category.
    // This removes thousands of synchronous texture loads from SongSelect startup.
    // Category version-splash: the game shows Version[ record.versionField ] for the selected song.
    // CRITICAL (fixed after the splash showed blank): the version field is INSIDE the 0x374 record at
    // offset +0x370, NOT a separate array. The filter FUN_00435070 qmemcpy's each entry's 0x374 bytes
    // into the visible array (this+300, stride 221 dwords), and the render reads version at
    // this[221*visibleIdx+295] — which is dword 220 (=0x370) of the copied entry. Writing it OUTSIDE the
    // entry (the old ecx+0x49C) never got copied → the game read 0 → blank splash. Write it in-entry so
    // it travels with the song through the filter. 19+categoryIndex selects our Version_NN.abm (NN>18).
    const uint32_t kVersionFieldOff = 0x370; // within the 0x374 record
    auto writeEntry = [&](uint32_t entry, const CustomBmsSong& s, uint32_t slotIdx, int versionNN){
        (void)slotIdx;
        SafeWriteBytes(entry, nativeTemplate, 0x374);
        if (versionNN >= 0) {
            uint32_t vv = (uint32_t)versionNN;
            SafeWriteBytes(entry + kVersionFieldOff, &vv, 4);
        }
        char idbuf[0x20] = {};
        strncpy(idbuf, s.id.c_str(), sizeof(idbuf) - 1);
        SafeWriteBytes(entry + 0x10C, idbuf, (uint32_t)strlen(idbuf) + 1); // id (disc/songname use this; abs 568)
        uint32_t noSongname = 0;
        SafeWriteBytes(entry + 0x004, &noSongname, 4);
        const uint32_t lvlOff[4] = {0x214, 0x220, 0x22C, 0x238};
        int lvl[4] = {s.levelNM, s.levelHD, s.levelSHD, s.levelEX};
        // CRITICAL: the song-list filter (FUN_00435070) copies an entry into the visible display
        // array ONLY if its NM level (+0x214) != 0, yet it counts ALL matches in +0x7c. A BMS song
        // with no Easy chart (NM=0) is counted-but-not-displayed → the wheel render hits a null
        // visible entry → crash (0x4323fc), intermittently by wheel position. Guarantee NM != 0 by
        // falling back to the lowest available difficulty.
        if (lvl[0] <= 0) lvl[0] = (lvl[1] > 0) ? lvl[1] : (lvl[2] > 0) ? lvl[2] : (lvl[3] > 0) ? lvl[3] : 1;
        for (int i = 0; i < 4; ++i) { float f = (float)(lvl[i] < 0 ? 0 : lvl[i]); SafeWriteBytes(entry + lvlOff[i], &f, 4); }
    };

    EnsureCategoryTablesRelocated(); // move category tables off the native globals before writing customs
    RefreshRelocatedNativeCategoryTables();
    std::vector<std::string> categories = CustomCategoriesForModeOrLargest(GetActiveMode());
    g_lazySongnameObject = ecx;
    g_lazySongnames.assign(categories.size(), {});
    g_lazySongnameLoaded.assign(categories.size(), 0);
    uint32_t slot = master;            // append after the native songs
    uint32_t appended = 0;
    // Even per-category budget so a few huge categories can't eat the whole (capped) master array
    // and starve later categories to empty — an empty wheel category crashes (garbage/zero count).
    // GetCustomBmsCategoriesForMode only returns categories that HAVE songs, so perCat>=1 keeps
    // every wheel category populated. ponytail: lifts to the real per-category 436 once master[]
    // is relocated out of the capped pool block.
    uint32_t budget = (kCapacity > master) ? (kCapacity - master) : 0;
    uint32_t numCats = (uint32_t)categories.size();
    // Extend the version-splash load loop (0x437815 `cmp edi,0x13`, i.e. <19) to 19+numCats so the game
    // loads our custom Version_19..NN.abm too (else those texture slots are null → blank splash). The
    // limit is a single byte at RVA 0x37817; clamp to 255 (it's compared as a byte). One-shot.
    {
        static bool s_verLoopPatched = false;
        if (!s_verLoopPatched) {
            uint32_t total = 19 + numCats;
            if (total > 255) total = 255;
            uint8_t b = (uint8_t)total;
            if (WriteMemory(Rva(0x37817), &b, 1)) { s_verLoopPatched = true;
                LogLine("[version] load-loop limit 19 -> %u (numCats=%u)", total, numCats); }
        }
    }
    // If master[] can hold ALL custom songs (the normal case — newCap is sized to fit them), give
    // each category its full per-category limit (436 stride) so big folders aren't truncated. Only
    // when budget can't fit everything (newCap clamp hit) fall back to an even split so no wheel
    // category ends up empty (empty category → crash). ponytail: even split was a leftover from the
    // ~1084 object-cap era; the 8MB object lifted that.
    // EMPIRICAL per-category ceiling: at 1929 songs/7 cats with <=285 songs/category it worked; at
    // 2435/9 cats with up to 331/category the game's category-render throws an uncaught C++ exception
    // → abort(3) on category change (confirmed: ExitProcess code 3, stack walk to the filter caller
    // 0x4448fe + C++ unwind frames). So cap per-category at 256 (clear margin under the working 285).
    // ponytail: raise toward the real limit once found (bisect), or relocate the render's per-category
    // structure like we did for master[]/the category tables. A folder with more than this shows 256.
    const uint32_t kSafePerCat = 436u; // the filter's display stack array holds 436 entries (its max)
    uint32_t perCat = (budget >= (uint32_t)allSongs.size()) ? kSafePerCat
                                                            : (numCats ? budget / numCats : 0);
    if (perCat == 0) perCat = 1;
    if (perCat > kSafePerCat) perCat = kSafePerCat;
    for (size_t ci = 0; ci < categories.size() && (47 + ci) < 256; ++ci) {
        const uint32_t cat = 47 + (uint32_t)ci;
        uint32_t k = 0, available = 0;
        for (const auto& song : allSongs) {
            if (_stricmp(song.category.c_str(), categories[ci].c_str()) != 0) continue;
            // Count every match even past the limit: dropping songs silently is how a growing
            // library loses entries with no error anywhere. `continue`, not `break`, so `available`
            // is the true total and the truncation below can be reported.
            ++available;
            if (slot >= kCapacity || k >= perCat) continue;
            // 1) master[slot]: id + level floats only (entry zero-init by the game). versionNN =
            // 19+categoryIndex so the version splash matches the category (Version_NN.abm via redirect).
            uint32_t entry = masterBase + slot * kEntrySize;
            writeEntry(entry, song, slot, 19 + (int)ci);
            g_lazySongnames[ci].push_back({entry, song.id});
            // 2) native category table: id string + advance
            char idbuf[kCatNameSize] = {};
            strncpy(idbuf, song.id.c_str(), kCatNameSize - 1);
            uint32_t nameAddr = CatNameTable() + (k + cat * kCatNameStride) * kCatNameSize;
            SafeWriteBytes(nameAddr, idbuf, kCatNameSize);
            ++k; ++slot; ++appended;
        }
        // 3) native count — only if non-zero. A 0-song category is NAVIGABLE but makes the
        // game do `% 0` (div-by-zero crash 0x440439), so never register an empty category.
        if (k > 0) {
            SafeWriteBytes(CatCountTable() + cat * 4, &k, 4);
            LogLine("[category] %s native-table cat=%lu name='%s' songs=%lu",
                    phase, (unsigned long)cat, categories[ci].c_str(), (unsigned long)k);
            if (available > k)
                LogLine("[category] WARNING cat=%lu name='%s' TRUNCATED %lu -> %lu songs: %lu dropped."
                        " Per-category ceiling is %lu (the filter's %lu-entry display array)."
                        " Split this folder under BMSData\\<mode>\\ into more categories to keep them all.",
                        (unsigned long)cat, categories[ci].c_str(), (unsigned long)available, (unsigned long)k,
                        (unsigned long)(available - k), (unsigned long)perCat, (unsigned long)kSafePerCat);
        } else {
            LogLine("[category] %s SKIP empty cat=%lu name='%s' (capacity full at master=%lu/%lu)",
                    phase, (unsigned long)cat, categories[ci].c_str(), (unsigned long)slot, (unsigned long)kCapacity);
        }
    }

    // grow ONLY the master count (+0x80) so the filter scans the appended songs. Do NOT
    // touch +0x7c: that's the PER-CATEGORY visible song count the filter (FUN_00435070)
    // sets itself; forcing it to the master total corrupted it and on the 3rd SongSelect
    // entry left it 0 → the game's `% [+0x7c]` modulo (fun_43ff00) divided by zero →
    // INT_DIVIDE_BY_ZERO crash (0x440439). The filter manages +0x7c correctly on its own.
    SafeWriteBytes(ecx + 0x80, &slot, 4);
    LogLine("[category] %s native inject done master=%lu->%lu appended=%lu",
            phase, (unsigned long)master, (unsigned long)slot, (unsigned long)appended);
    size_t lazyIndexed = 0;
    for (const auto& category : g_lazySongnames) lazyIndexed += category.size();
    LogLine("[assets-lazy] indexed categories=%lu songs=%lu eager=0 ok=%d",
            (unsigned long)g_lazySongnames.size(), (unsigned long)lazyIndexed,
            lazyIndexed == appended ? 1 : 0);
    // Mark that the song inject ran — the tracer arms on the next Credits_mask load (which is right
    // before the abort), but only after this point, so a boot-time Credits load doesn't arm too early
    // (arming far from the abort = millions of single-steps = the game appears frozen). See mod_main.
    extern bool g_songInjectRan; g_songInjectRan = true;
    // TEXDIAG: the wheel draws  obj.tex[(int)master[i].field_0x340]  from the array at obj+0xc040c
    // (decomp 34594). The >~2000 AV is a bad pointer there. Report the index range + how many songs
    // map to a NULL slot → tells size-overflow (huge maxTexIdx) vs failed-texture-load (many nulls).
    {
        uint32_t maxIdx = 0, nullCnt = 0, garbageCnt = 0, scanned = 0;
        for (uint32_t i = 0; i < slot && i < kCapacity; ++i) {
            uint32_t raw = SafeRead32(masterBase + i * kEntrySize + 0x340);
            float f; std::memcpy(&f, &raw, 4);
            int idx = (int)f;
            if (idx < 0 || idx > 1000000) { garbageCnt++; continue; }
            if ((uint32_t)idx > maxIdx) maxIdx = (uint32_t)idx;
            if (!SafeRead32(ecx + 0xc040c + (uint32_t)idx * 4)) nullCnt++;
            scanned++;
        }
        uint32_t sel = SafeRead32(ecx + 0x84);
        uint32_t selRaw = SafeRead32(masterBase + sel * kEntrySize + 0x340);
        float sf; std::memcpy(&sf, &selRaw, 4);
        LogLine("[texdiag] songs=%lu scanned=%lu maxTexIdx=%lu nullSlots=%lu garbageIdx=%lu | sel=%lu selTexIdx=%d",
                (unsigned long)slot, (unsigned long)scanned, (unsigned long)maxIdx,
                (unsigned long)nullCnt, (unsigned long)garbageCnt, (unsigned long)sel, (int)sf);
        // KResourceContainer texture pool (static object @0x530c2c): +8 size, +0xc base, +0x10 bump.
        // The "Exceed pool size" overflow → null text-texture → 5Key crash. This tells the TRUE runtime
        // pool size (did the 1GB EXE-const patch take?) and how full it is at SongSelect entry.
        uint32_t pSize = SafeRead32(0x530c2c + 8), pBase = SafeRead32(0x530c2c + 0xc), pBump = SafeRead32(0x530c2c + 0x10);
        LogLine("[texpool] size=%luMB base=%08lx used=%luMB",
                (unsigned long)(pSize >> 20), (unsigned long)pBase,
                (unsigned long)(((pBase && pBump >= pBase) ? (pBump - pBase) : 0) >> 20));
    }
    // DIAG: the filter FUN_00435070 builds a 436-entry STACK array; a category whose count exceeds
    // that overruns it → /GS terminates the process with no catchable exception (the unexplained
    // category-change "crash"). Dump every non-zero category count (natives 0..46 copied + customs
    // 47+) so we can see if any is >436. Reading our own table directly — no stack-arg guesswork.
    for (uint32_t c = 0; c < 64; ++c) {
        uint32_t cnt = SafeRead32(CatCountTable() + c * 4);
        if (cnt) LogLine("[catcount] cat=%lu count=%lu%s", (unsigned long)c, (unsigned long)cnt,
                         cnt > 436u ? "  <<<< OVERRUNS the 436-entry filter stack array" : "");
    }
}

static void LoadCustomCategorySongnames(uint32_t ecx, uint32_t categoryParam) {
    if (ecx != g_lazySongnameObject || categoryParam < 47) return;
    size_t categoryIndex = (size_t)(categoryParam - 47);
    if (categoryIndex >= g_lazySongnames.size() || g_lazySongnameLoaded[categoryIndex]) return;
    uint32_t gfxCtx = SafeRead32(0x1b2f054);
    if (!gfxCtx) return;

    DWORD started = GetTickCount();
    // Keep one native-owned texture per song in categories actually visited. Sharing aliases across
    // entries survived one SongSelect but corrupted ownership when that object was destroyed/recreated.
    typedef int(__thiscall* LoadTexFn)(uint32_t ctx, const char* file, void* dest, const char* alt);
    LoadTexFn loadTex = (LoadTexFn)Rva(0x8d00);
    unsigned long loaded = 0;
    for (const auto& item : g_lazySongnames[categoryIndex]) {
        char request[MAX_PATH] = {};
        snprintf(request, sizeof(request), "system\\songname\\%s.bmp", item.id.c_str());
        loadTex(gfxCtx, request, reinterpret_cast<void*>(item.masterEntry + 0x004), nullptr);
        if (SafeRead32(item.masterEntry + 0x004)) ++loaded;
    }
    g_lazySongnameLoaded[categoryIndex] = 1;
    std::vector<std::string> categories = CustomCategoriesForModeOrLargest(GetActiveMode());
    const char* name = categoryIndex < categories.size() ? categories[categoryIndex].c_str() : "?";
    LogLine("[assets-lazy] category=%lu name='%s' requested=%lu loaded=%lu loadMs=%lu categoryCache=1",
            (unsigned long)categoryParam, name,
            (unsigned long)g_lazySongnames[categoryIndex].size(), loaded,
            (unsigned long)(GetTickCount() - started));
}

// NOTE: there is NO separate "staging buffer". Disassembly of EZ2B.exe (the
// songname/discsmall preload loop inside FUN_00435390) shows the loop walks
// master[] directly: cursor starts at `ebx + 0x5e3c8` (== master[0]+0x10C),
// advances `add edi,0x374` (master stride), bound `cmp esi,[ebx+0x80]` (master
// count, re-read each iteration), and reads the id from master[i]+0x10C for the
// `sound\<id>` SetCurrentDirectory. So the master[] inject we already do is all
// that's needed — the only catch is timing: this loop runs before our 0x35070
// hook (which the decomp shows is called at the END of FUN_00435390). The
// FUN_00431660 hook (its only call site, at the top of this loop) lets us run
// the master[] inject BEFORE the loop scans, so custom songname/disc are
// requested during the first list build. See [[verify-hooks-with-binary]].

// DIAGNOSTIC dump (informe §10): map the live SongSelect structure once per object.
// No writes — read-only. Logs category count/names, active category (param2), master
// count, first 10 master ids, and a hex view of each entry's +0x100..+0x160 region so
// the REAL difficulty offsets (vs the assumed +0x10C song.bin layout) can be read off.
static void DumpSongSelectStructure(uint32_t ecx, uint32_t param2) {
    if (!ecx || !CanReadPtr(reinterpret_cast<void*>(ecx), 0x90)) return;
    static uint32_t s_dumpedForObject = 0;
    if (s_dumpedForObject == ecx) return;
    s_dumpedForObject = ecx;

    uint32_t master = SafeRead32(ecx + 0x80);
    LogLine("[diag] ===== SongSelect dump ecx=%08lx param2=%08lx masterCount=%lu =====",
            (unsigned long)ecx, (unsigned long)param2, (unsigned long)master);

    // category count + names via the Stage.ini category table (dword-indexed offsets
    // from the decomp: count 0x83a24, names 0x836a0 stride 9 dwords).
    uint32_t catCount = SafeRead32(ecx + 0x83a24u * 4u);
    if (catCount > 100) { LogLine("[diag] catCount=%lu (capped/garbage, table maybe not resident)", (unsigned long)catCount); catCount = 0; }
    else LogLine("[diag] catCount=%lu", (unsigned long)catCount);
    for (uint32_t i = 0; i < catCount && i < 60; ++i) {
        std::string nm = SafeReadAscii(ecx + (0x836a0u + i * 9u) * 4u, 32);
        uint32_t row = SafeRead32(ecx + (0x836a8u + i * 9u) * 4u);
        LogLine("[diag]   cat[%lu] name='%s' row=%08lx", (unsigned long)i, nm.c_str(), (unsigned long)row);
    }

    if (master == 0 || master > 436) return;
    // PRIMARY song array (Hex-Rays sub_404950 / disc loader): base = object, stride 884
    // (0x374). id at +568 (0x238), level floats NM/HD/SHD/EX at +832/844/856/868
    // (0x340/0x34C/0x358/0x364). master[] at +0x5e2bc is the SEPARATE filter array.
    // Dump first 5 + last 3 to confirm levels match the in-game display.
    // Entry-relative layout (Hex-Rays, stride 884): the primary array starts at +0x12C
    // (300), so abs offset 832 = entry +532 (0x214) = NM level float; HD/SHD/EX follow at
    // +0x220/+0x22C/+0x238. id at +0x10C (matches master). Read BOTH arrays to confirm
    // which is populated at filter time: primary [+0x12C..), master [+0x5e2bc..).
    auto dumpEntry = [&](uint32_t i){
        auto levels = [&](uint32_t entry, const char* tag){
            float l[4];
            for (int k=0;k<4;++k){ uint32_t v=SafeRead32(entry + 0x214 + k*12); memcpy(&l[k],&v,4); }
            std::string id = SafeReadAscii(entry + 0x10C, 32);
            LogLine("[diag]   %s[%lu] id='%s' NM=%.1f HD=%.1f SHD=%.1f EX=%.1f",
                    tag, (unsigned long)i, id.c_str(), l[0],l[1],l[2],l[3]);
        };
        uint32_t mbase = 0x5e2bc + (g_capacityPatched ? 0xEFDF4u : 0u); // relocated master
        levels(ecx + 0x12C + 884u*i, "vis ");
        levels(ecx + mbase + 884u*i, "mstr");
    };
    for (uint32_t i = 0; i < 5 && i < master; ++i) dumpEntry(i);
    for (uint32_t i = (master > 3 ? master - 3 : 0); i < master; ++i) dumpEntry(i);
}

static void LogKnownFunctionArgs(TraceHook* h, PushadRegs* r, unsigned long hit) {
    if (!h || !r) return;
    const uint32_t esp = r->originalEsp;
    const uint32_t ret = SafeRead32(esp + 0);
    const uint32_t a1 = SafeRead32(esp + 4);
    const uint32_t a2 = SafeRead32(esp + 8);
    const uint32_t a3 = SafeRead32(esp + 12);
    const uint32_t a4 = SafeRead32(esp + 16);

    LogLine("[trace] hit=%lu rva=0x%05lx %s eax=%08lx ecx=%08lx edx=%08lx ret=%08lx a1=%08lx a2=%08lx a3=%08lx a4=%08lx",
            hit, static_cast<unsigned long>(h->rva), h->name,
            static_cast<unsigned long>(r->eax), static_cast<unsigned long>(r->ecx), static_cast<unsigned long>(r->edx),
            static_cast<unsigned long>(ret), static_cast<unsigned long>(a1), static_cast<unsigned long>(a2),
            static_cast<unsigned long>(a3), static_cast<unsigned long>(a4));

    if (h->rva == 0x11460 || h->rva == 0x11090 || h->rva == 0x2DE0) {
        LogLine("[trace] path arg1='%s'", SafeReadAscii(a1, 240).c_str());
    }
    if (h->rva == 0x35070) {
        DumpSongSelectStructure(r->ecx, a2);
        InjectCustomSongsIntoSongSelect(r->ecx, "pre-filter", a2);
        LoadCustomCategorySongnames(r->ecx, a2);
        // Wheel category icons: the texture arrays are relocated to the grown object tail
        // (kCatTexSmallReloc/kCatTexBigReloc), so slots 47.. are safe free memory. The native
        // loader fills [0..46]; we load OUR category_NN.bmp into [47..] of both arrays via the
        // game's loader (thiscall, ecx=gfx ctx [0x1b2f054]) — same call as songname. Redirect
        // serves category_48/49.bmp from our _categories cache. Only when relocated.
        if (g_customCategoryCount > 0 && g_capacityPatched && g_catTexSmallBase) {
            typedef int(__thiscall* LoadTexFn)(uint32_t ctx, const char* file, void* dest, const char* alt);
            LoadTexFn loadTex = (LoadTexFn)Rva(0x8d00);
            uint32_t gfxCtx = SafeRead32(0x1b2f054);
            if (gfxCtx) {
                for (uint32_t c = 0; c < g_customCategoryCount; ++c) {
                    int catNum = 48 + (int)c;             // native uses 01..47, custom start at 48
                    char fn[80];
                    snprintf(fn, sizeof(fn), "System\\SongSelect\\Sortimage\\category_%02d.bmp", catNum);
                    uint32_t slot = 47 + c;
                    loadTex(gfxCtx, fn, reinterpret_cast<void*>(r->ecx + g_catTexSmallBase + slot * 4), nullptr);
                    loadTex(gfxCtx, fn, reinterpret_cast<void*>(r->ecx + g_catTexBigBase + slot * 4), nullptr);
                }
                LogLine("[category] loaded %lu wheel icons into relocated slots 47.. small=%08lx big=%08lx",
                        (unsigned long)g_customCategoryCount,
                        (unsigned long)SafeRead32(r->ecx + g_catTexSmallBase + 47 * 4),
                        (unsigned long)SafeRead32(r->ecx + g_catTexBigBase + 47 * 4));
            }
        }
    }
    // 0x31660 staging-preload inject removed: it called the same fabrication.
    if (h->rva == 0x23A90) {
        LogLine("[songload] enter ecx=%08lx object probe +148=%08lx +14c=%08lx +154=%08lx",
                static_cast<unsigned long>(r->ecx),
                static_cast<unsigned long>(SafeRead32(r->ecx + 0x148)),
                static_cast<unsigned long>(SafeRead32(r->ecx + 0x14c)),
                static_cast<unsigned long>(SafeRead32(r->ecx + 0x154)));
    }
}

extern "C" __declspec(dllexport) void __cdecl EZ2TraceHitEx(TraceHook* h, PushadRegs* regs) {
    if (!h) return;
    h->hitCount++;

    // Custom songs have no native Top-5 record. Let the renderer draw the normal frame, but clear
    // its final `showRanking` argument before it interprets unrelated visible-entry bytes as glyphs.
    if (h->rva == 0x33D50 && regs) {
        // BuildStub does pushfd before pushad, so PUSHAD's saved ESP is function-entry ESP - 4.
        uint32_t functionEsp = regs->originalEsp + 4;
        uint32_t selected = *reinterpret_cast<uint32_t*>(functionEsp + 12);
        uint32_t visible = *reinterpret_cast<uint32_t*>(regs->ecx + 0x7C);
        if (selected < visible && selected < 436) {
            const char* id = reinterpret_cast<const char*>(regs->ecx + 0x238 + selected * 0x374);
            if (id[0] == 'b' && id[1] == 'm' && id[2] == 's') {
                *reinterpret_cast<uint32_t*>(functionEsp + 20) = 0;
                static bool logged = false;
                if (!logged) { logged = true; LogLine("[rank] custom Top-5 suppressed id='%.15s' selected=%lu", id, (unsigned long)selected); }
            }
        }
        return; // called every frame; never trace-spam this renderer
    }

    const bool isJudge = (std::strstr(h->name, "Judge") || std::strstr(h->name, "Miss sweep") || std::strstr(h->name, "Tap judge") || std::strstr(h->name, "LN/release"));
    if (isJudge && !Config().traceJudge) return;

    static thread_local bool inTrace = false;
    if (inTrace) return;
    inTrace = true;

    unsigned long n = h->hitCount;
    if (n <= 20 || (n % 1000) == 0 || !isJudge) {
        LogKnownFunctionArgs(h, regs, n);
    }

    inTrace = false;
}

static void* AllocExec(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static bool BuildGateway(TraceHook& h) {
    unsigned char* mem = static_cast<unsigned char*>(AllocExec(h.overwriteLen + 5));
    if (!mem) return false;
    std::memcpy(mem, h.original, h.overwriteLen);
    unsigned char* p = mem + h.overwriteLen;
    EmitU8(p, 0xE9);
    void* next = p + 4;
    EmitRel32(p, next, static_cast<unsigned char*>(h.target) + h.overwriteLen);
    h.gateway = mem;
    return true;
}

static bool BuildStub(TraceHook& h) {
    // x86 stub:
    //   pushfd / pushad
    //   save XMM/FPU with fxsave
    //   pass TraceHook* and PushadRegs* to EZ2TraceHitEx
    //   restore XMM/FPU, regs, flags
    //   jump original gateway
    unsigned char* mem = static_cast<unsigned char*>(AllocExec(192));
    if (!mem) return false;
    unsigned char* p = mem;
    EmitU8(p, 0x9C);              // pushfd
    EmitU8(p, 0x60);              // pushad

    EmitU8(p, 0x81); EmitU8(p, 0xEC); EmitU32(p, 0x214); // sub esp, 0x214
    EmitU8(p, 0x89); EmitU8(p, 0xE0);                    // mov eax, esp
    EmitU8(p, 0x83); EmitU8(p, 0xC0); EmitU8(p, 0x0F);    // add eax, 0x0f
    EmitU8(p, 0x83); EmitU8(p, 0xE0); EmitU8(p, 0xF0);    // and eax, -16
    EmitU8(p, 0x89); EmitU8(p, 0x84); EmitU8(p, 0x24); EmitU32(p, 0x210); // mov [esp+210], eax
    EmitU8(p, 0x0F); EmitU8(p, 0xAE); EmitU8(p, 0x00);    // fxsave [eax]

    // lea eax, [esp+0x214] ; pointer to PUSHAD regs
    EmitU8(p, 0x8D); EmitU8(p, 0x84); EmitU8(p, 0x24); EmitU32(p, 0x214);
    EmitU8(p, 0x50);                                      // push eax
    EmitU8(p, 0x68); EmitU32(p, reinterpret_cast<uint32_t>(&h)); // push h
    EmitU8(p, 0xE8);
    void* callNext = p + 4;
    EmitRel32(p, callNext, reinterpret_cast<void*>(&EZ2TraceHitEx));
    EmitU8(p, 0x83); EmitU8(p, 0xC4); EmitU8(p, 0x08);    // add esp,8

    EmitU8(p, 0x8B); EmitU8(p, 0x84); EmitU8(p, 0x24); EmitU32(p, 0x210); // mov eax, [esp+210]
    EmitU8(p, 0x0F); EmitU8(p, 0xAE); EmitU8(p, 0x08);    // fxrstor [eax]
    EmitU8(p, 0x81); EmitU8(p, 0xC4); EmitU32(p, 0x214);  // add esp, 0x214

    EmitU8(p, 0x61);                                      // popad
    EmitU8(p, 0x9D);                                      // popfd
    EmitU8(p, 0xE9);
    void* jmpNext = p + 4;
    EmitRel32(p, jmpNext, h.gateway);
    h.stub = mem;
    return true;
}

static bool PatchTargetToStub(TraceHook& h) {
    unsigned char patch[32] = {};
    patch[0] = 0xE9;
    intptr_t rel = reinterpret_cast<intptr_t>(h.stub) - reinterpret_cast<intptr_t>(h.target) - 5;
    *reinterpret_cast<int32_t*>(&patch[1]) = static_cast<int32_t>(rel);
    for (size_t i = 5; i < h.overwriteLen; ++i) patch[i] = 0x90;
    return WriteMemory(h.target, patch, h.overwriteLen);
}

static bool InstallOne(TraceHook& h) {
    h.target = Rva(h.rva);
    if (!LooksLikeExecutable(h.target)) {
        LogLine("[trace] skip %s rva=0x%05lx target=%p not executable", h.name, static_cast<unsigned long>(h.rva), h.target);
        return false;
    }
    if (h.overwriteLen > sizeof(h.original)) {
        LogLine("[trace] skip %s overwriteLen too large", h.name);
        return false;
    }
    std::memcpy(h.original, h.target, h.overwriteLen);
    if (!BuildGateway(h)) {
        LogLine("[trace] gateway failed %s", h.name);
        return false;
    }
    if (!BuildStub(h)) {
        LogLine("[trace] stub failed %s", h.name);
        return false;
    }
    if (!PatchTargetToStub(h)) {
        LogLine("[trace] patch failed %s target=%p stub=%p", h.name, h.target, h.stub);
        return false;
    }
    h.installed = true;
    LogLine("[trace] installed rva=0x%05lx len=%u target=%p gateway=%p stub=%p name=%s",
            static_cast<unsigned long>(h.rva), static_cast<unsigned>(h.overwriteLen), h.target, h.gateway, h.stub, h.name);
    return true;
}

// Raise the hardcoded 436-song master-array cap so hundreds of custom songs fit.
// Patch table is the FNEX section of EZ2Tools/ez2SongdbEditor (a tool that patches the
// EXE on disk); we apply the SAME edits IN MEMORY (no EXE modification). It relocates the
// SongSelect master/song arrays by +0xEFDF4 so they don't overrun the object state at
// +0x371FE, and widens every hardcoded song-count immediate. file_offset == RVA in .text.
//   newCap = total songs the array must hold (native ~430 + custom). Keep <= ~700 so the
//   0x56*newCap displacements stay sane; we write full dwords so >16-bit values are fine.
// ---- SongSelect object allocation replacement -----------------------------------------------
// The game's object pool (0x4016a0) hands out a FIXED ~2MB block (it ignores the requested size),
// which caps master[] at ~1084 entries. To go past that we replace the allocation itself: detour
// the 2 SongSelect ctor call-sites (call 0x4016a0 @ 0x1bb55 / 0x5cf95) to return our OWN large
// VirtualAlloc'd buffer, so `this` (ecx) is big and the relocated master[] at +0x14E0B0 fits
// thousands. The object's ctor still runs on it (the call site does mov ecx,eax; call ctor). The
// game's matching free (0x401720) is a no-op when [obj+0xc] bit0==0, which our zeroed buffer
// satisfies — so no free hook / no pool corruption. SongSelect is a singleton menu (one alive at a
// time, 2 entry paths = same class), so a small ring of reused buffers avoids any leak.
// ponytail: ring of 4; if two SongSelect objects ever coexist, bump the ring.
static const size_t kObjBufSize = 8u * 1024u * 1024u; // holds master[] up to ~6000 songs
static void* g_objBufs[4] = {};
static int   g_objBufIdx = 0;
static void* __stdcall SongSelectObjAlloc(uint32_t size) {
    (void)size;
    int i = g_objBufIdx; g_objBufIdx = (g_objBufIdx + 1) & 3;
    if (!g_objBufs[i]) g_objBufs[i] = VirtualAlloc(nullptr, kObjBufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_objBufs[i]) memset(g_objBufs[i], 0, kObjBufSize); // ctor expects fresh zeroed memory on reuse
    LogLine("[objbuf] SongSelect object -> %p slot=%d size=%uMB", g_objBufs[i], i, (unsigned)(kObjBufSize >> 20));
    return g_objBufs[i];
}
static void PatchObjectAllocSites() {
    for (uintptr_t rva : {0x1bb55u, 0x5cf95u}) {
        uint8_t* site = reinterpret_cast<uint8_t*>(Rva(rva));
        if (site[0] != 0xE8) { LogLine("[objbuf] site 0x%lx not a call (0x%02x) — skip", (unsigned long)rva, site[0]); continue; }
        int32_t rel = (int32_t)((uintptr_t)&SongSelectObjAlloc - ((uintptr_t)site + 5));
        WriteMemory(site + 1, &rel, 4);
        LogLine("[objbuf] detoured alloc call @rva 0x%lx -> SongSelectObjAlloc", (unsigned long)rva);
    }
}

static bool InstallSongCapacityPatches(uint32_t newCap) {
    if (g_capacityPatched) return true;
    const uint32_t REL = 0xEFDF4;                 // relocation delta (tested by the tool)
    const uint32_t bytes = 0x56 * newCap;         // song.bin record area size
    auto w32 = [](uintptr_t rva, uint32_t v) { return WriteMemory(Rva(rva), &v, 4); };

    bool ok = true;
    // Group 0 — grow the SongSelect object allocation so the RELOCATED master array
    // (base 0x5e2bc + REL, newCap entries * 0x374) fits. Two `push 0x103540` alloc sites.
    // Required size = REL + 0x5e2bc + newCap*0x374, rounded up with headroom.
    uint32_t needSize = REL + 0x5E2BC + newCap * 0x374 + 0x20000;
    uint32_t allocSize = (needSize + 0xFFFFF) & ~0xFFFFFu;  // round up to 1MB
    for (uintptr_t off : {0x1bb47u, 0x5cf87u}) {
        uint32_t cur = 0; memcpy(&cur, Rva(off), 4);
        if (cur == 0x103540) ok &= w32(off, allocSize);     // only patch the matching alloc
    }
    LogLine("[capacity] object alloc 0x103540 -> 0x%lx (need 0x%lx)", (unsigned long)allocSize, (unsigned long)needSize);
    // Group A REMOVED — CONFIRMED corruptor (HW write-watch: the song.bin parser id-copy loop @rva
    // 0x69e3e clobbers DAT_037dcecc). It widened the native song.bin parser to 0x56*newCap, which
    // makes it copy WAY more category ids than the fixed 436-slot table holds → .data overrun. We
    // don't add songs to song.bin (custom go into master[] via the inject), so the native parser
    // must stay at its native counts. (void)bytes; below avoids the now-unused warning.
    (void)bytes;
    // Group B REMOVED — its filter-read shift only made sense paired with Group A. Filter reads the
    // native category tables; the inject relocates them to our own buffer (EnsureCategoryTablesRelocated)
    // and repoints the filter there, so custom categories don't overrun the native 47-slot region.
    // Group C — songs parsed during category init (16-bit field; write low word only).
    { uint16_t n = (uint16_t)newCap; ok &= WriteMemory(Rva(0x35F54), &n, 2); }
    // Group D — RELOCATE master/song arrays by +REL (the crux: moves them off the state var).
    for (uintptr_t off : {0x3515Bu, 0x375B9u}) ok &= w32(off, 0x5E2BC + REL);
    for (uintptr_t off : {0x35118u, 0x35F80u, 0x360EFu, 0x36198u}) ok &= w32(off, 0x5E3C8 + REL);
    for (uintptr_t off : {0x30F69u, 0x394BAu}) ok &= w32(off, 0x5E2C4 + REL);
    ok &= w32(0x39447, 0x5E62C + REL);
    for (uintptr_t off : {0x3946Du, 0x39301u}) ok &= w32(off, 0x5E4D0 + REL);
    ok &= w32(0x362B2, (uint32_t)(-(int32_t)(0x5E29C + REL)));
    ok &= w32(0x362EF, (uint32_t)(-(int32_t)(0x5E298 + REL)));

    // Texture arrays go in the RELOCATION GAP — the space between the original object end
    // (~0x103540) and the relocated master[] start (0x5e2bc+REL = 0x14E0B0). That space is always
    // committed and FIXED, so it does NOT move with newCap. This decouples the texarrays from
    // master[], letting master[] grow (commit-on-inject) without pushing the texarrays into
    // uncommitted memory. 127 categories * 4 bytes each fits easily in the ~0x4AB70 gap.
    if (ok) {
        g_catTexSmallBase = 0x110000;            // in [0x103540, 0x14E0B0], page-aligned
        g_catTexBigBase   = 0x110800;
        LogLine("[capacity] category texture arrays in reloc-gap small=0x%lx big=0x%lx (alloc 0x%lx)",
                (unsigned long)g_catTexSmallBase, (unsigned long)g_catTexBigBase, (unsigned long)allocSize);
    }
    // (object growth now via the VirtualAlloc-inflate hook in mod_main — PatchObjectAllocSites hit
    //  the wrong call sites: those allocate a different class, not the SongSelect object.)
    g_capacityPatched = ok;
    g_songCapacity = ok ? newCap : 436;
    LogLine("[capacity] master array %s newCap=%lu relocation=+0x%lx (was 436)",
            ok ? "RAISED" : "FAILED", (unsigned long)g_songCapacity, (unsigned long)REL);
    return ok;
}

static void RemoveCustomCategoryPatches();

static bool InstallCustomCategoryPatches() {
    std::vector<std::string> categories = CustomCategoriesForModeOrLargest(std::string());
    g_customCategoryCount = (uint32_t)std::min<size_t>(categories.size(), 80);
    if (g_customCategoryCount == 0) return true;
    static const unsigned char top5Prologue[] = {0x83, 0xEC, 0x10, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x14};
    if (std::memcmp(Rva(0x33D50), top5Prologue, sizeof(top5Prologue)) != 0) {
        LogLine("[category] top5 guard signature mismatch — custom SongSelect disabled");
        return false;
    }

    // Wheel-extend, FIXED (decomp fun_432200): the OLD code also redirected the texture
    // array `[ebx+0xbc4b8]` to a zeroed g_categoryTextures → every slot null → crash. We
    // now ONLY extend the loop count + geometry (9 immediates) and leave the object's own
    // texture array intact. Slots 47/48 of that object array are filled per-object in the
    // inject (copied from a native slot), so the render reads a valid handle → no crash.
    const uint32_t categoryCount = 47 + g_customCategoryCount;
    const uint32_t lastCategory = categoryCount - 1;
    g_categoryEdits[0].replacement = categoryCount;   // loader count (load 49 icons)
    g_categoryEdits[1].replacement = categoryCount;   // renderer count
    g_categoryEdits[2].replacement = lastCategory;
    g_categoryEdits[3].replacement = lastCategory;

    // Renderer STAYS at 47 — the wheel texture array at obj+0xbc4b8 is EXACTLY 47 entries:
    // obj+0xbc574 (== 0xbc4b8 + 47*4) is the NEXT field (selected-category render state, set
    // each frame by `*(ebx+0xbc574)=*(ebx+0xbc57c)` in fun_432200). Loading a texture into
    // "slot 47" clobbers that field. FIX: relocate both texture arrays into the grown object
    // tail so slots 47.. are real memory, then extend the renderer. Only when the object is
    // actually grown (capacity patched); otherwise keep the wheel at 47 (no icon, stable).
    if (g_capacityPatched && g_catTexSmallBase && g_catTexBigBase) {
        g_categoryEdits[1].replacement  = categoryCount;     // [1] renderer draws 47+N
        g_categoryEdits[9].replacement  = g_catTexSmallBase; // small base → free tail
        g_categoryEdits[10].replacement = g_catTexSmallBase;
        g_categoryEdits[11].replacement = g_catTexBigBase;   // big base → free tail
        g_categoryEdits[12].replacement = g_catTexBigBase;
    } else {
        g_categoryEdits[1].replacement  = 47;                // no growth → can't relocate
        g_categoryEdits[9].replacement  = g_categoryEdits[9].expected;  // no-op (write back)
        g_categoryEdits[10].replacement = g_categoryEdits[10].expected;
        g_categoryEdits[11].replacement = g_categoryEdits[11].expected;
        g_categoryEdits[12].replacement = g_categoryEdits[12].expected;
    }
    float span = (float)categoryCount * 100.0f;
    float half = span * 0.5f;
    float negativeHalf = -half;
    float negativeSpanMinusOne = -span - 1.0f;
    float negativeSpanPlusOne = -span + 1.0f;
    memcpy(&g_categoryEdits[4].replacement, &span, 4);
    memcpy(&g_categoryEdits[5].replacement, &half, 4);
    memcpy(&g_categoryEdits[6].replacement, &negativeHalf, 4);
    memcpy(&g_categoryEdits[7].replacement, &negativeSpanMinusOne, 4);
    memcpy(&g_categoryEdits[8].replacement, &negativeSpanPlusOne, 4);

    // texture-base redirect REMOVED — that was the crash. Object keeps its own array.
    for (const auto& edit : g_categoryEdits) {
        if (memcmp(Rva(edit.rva), &edit.expected, edit.length) != 0) {
            LogLine("[category] patch validation failed rva=0x%05lx", (unsigned long)edit.rva);
            return false;
        }
    }
    for (auto& edit : g_categoryEdits) {
        memcpy(&edit.original, Rva(edit.rva), edit.length);
        if (!WriteMemory(Rva(edit.rva), &edit.replacement, edit.length)) {
            RemoveCustomCategoryPatches();
            return false;
        }
        edit.installed = true;
    }
    LogLine("[category] native wheel extended categories=%lu custom=%lu slots=47..%lu reloc=%d small=0x%lx big=0x%lx",
            static_cast<unsigned long>(categoryCount), static_cast<unsigned long>(g_customCategoryCount),
            static_cast<unsigned long>(lastCategory), (g_capacityPatched && g_catTexSmallBase) ? 1 : 0,
            (unsigned long)g_catTexSmallBase, (unsigned long)g_catTexBigBase);
    if (categories.size() > g_customCategoryCount) {
        LogLine("[category] ignored categories=%lu signed-wheel-limit=127", static_cast<unsigned long>(categories.size() - g_customCategoryCount));
    }
    return true;
}

static void RemoveCustomCategoryPatches() {
    for (auto& edit : g_categoryEdits) {
        if (!edit.installed) continue;
        WriteMemory(Rva(edit.rva), &edit.original, edit.length);
        edit.installed = false;
    }
    g_customCategoryCount = 0;
}

void InstallTraceHooks() {
    if (g_traceInstalled) return;
    if (!Config().enableFunctionTrace) {
        LogLine("[trace] function trace disabled by config");
        return;
    }
    bool customCategory = Config().enableSongSelectInject && Config().enableBmsData && !GetCustomBmsSongs().empty();
    if (customCategory) {
        // HARD CEILING: the SongSelect object is a per-class POOL allocation (operator-new via
        // 0x4016a0), and the pool caps the block at ~2.3MB no matter how big a size we patch into
        // the `push` sites. master[] is relocated +0xEFDF4 into that block, so it physically holds
        // only ~1085 entries. Past that the game's songname/discsmall texture preload writes past
        // the object → crash (0x408e60). So clamp the cap to what the pool fits; per-category songs
        // are then distributed evenly in the inject so no wheel category ends up empty (that's the
        // OTHER crash: garbage/zero count → 0x4832a9 / div-by-zero).
        // ponytail: real fix for "1000s of songs" = put master[] in our own VirtualAlloc'd buffer
        // and repoint the game's accessors, instead of living inside the capped pool block.
        // The object's ~2.3MB cap is now lifted by committing its reserved tail on demand in the
        // inject (it bounds itself to whatever actually commits). So aim high. Kept <= 3000 because
        // the category-global-table SHIFT (Group B) = 0x56*(newCap-436) grows with newCap and would
        // eventually push those tables out of valid .data; 3000 keeps the shift near the tested 2203.
        // ponytail: >3000 needs the song.bin parse area + category tables relocated too (the next
        // structural limit after the object). The inject's commit-bound makes any shortfall safe.
        // The object is a FIXED ~2MB pool block (commit-on-inject confirmed it has no reserved
        // tail: [grow] committedTo stuck at ~+0x238000 → ~1084 entries). So master[] tops out at
        // ~650 custom songs no matter the cap. Keep newCap near that real ceiling; the inject's
        // commit-bound clamps to whatever actually committed. ponytail: >~650 needs master[] moved
        // into our own VirtualAlloc buffer (the pool block can't grow) — the remaining big fix.
        // Object is now our own 8MB buffer (PatchObjectAllocSites), so master[] is no longer capped
        // by the ~2MB pool block — aim high. Category overrun handled by EnsureCategoryTablesRelocated.
        uint32_t custom = 0;
        const char* capacityModes[] = {"5keyOnly","5keyMix","streetMix","7keyMix","clubMix","spaceMix","rubyMix","ez2Catch","turnTable","cv2Mix"};
        for (const char* mode : capacityModes) {
            uint32_t count = static_cast<uint32_t>(GetCustomBmsSongsForMode(mode).size());
            if (count > custom) custom = count;
        }
        uint32_t newCap = 436u + custom + 64u;
        if (newCap < 700u)    newCap = 700u;
        if (newCap > 60000u)  newCap = 60000u; // 64MB object inflate holds ~70k master entries (LAA=4GB)
        InstallSongCapacityPatches(newCap);
    }
    if (customCategory && !InstallCustomCategoryPatches()) {
        LogLine("[category] new category disabled: executable layout mismatch");
        return;
    }

    unsigned installed = 0;
    for (auto& h : g_hooks) {
        if (!h.enabled) continue;
        if (InstallOne(h)) installed++;
    }
    if (customCategory && !g_hooks[sizeof(g_hooks) / sizeof(g_hooks[0]) - 1].installed) {
        RemoveCustomCategoryPatches();
        LogLine("[category] new category disabled: SongSelect hook unavailable");
    }
    g_traceInstalled = true;
    LogLine("[trace] installed total=%u", installed);
}

void RemoveTraceHooks() {
    if (!g_traceInstalled) return;
    for (auto& h : g_hooks) {
        if (h.installed && h.target) {
            WriteMemory(h.target, h.original, h.overwriteLen);
            h.installed = false;
        }
    }
    RemoveCustomCategoryPatches();
    g_traceInstalled = false;
}
