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
static uint32_t g_customCategoryCount = 0;

// Version-label array relocation (details at InstallVersionArrayRelocation below).
static const uint32_t kVerArrayOrig  = 0xBFFC0;   // 196592 * 4
static const uint32_t kVerArrayReloc = 0x111000;  // clear of 0x110000/0x110800 (508 bytes each)
static const uint32_t kVerNativeCount = 19;
// Array A -- system/SongSelect/Version/ -- is the one behind the label under the song title:
//     3A104  mov eax,[esi+edx*4+0xBFF74]   ; A lookup
//     3A10D  lea edi,[esi+0xBE8D8]         ; its animation object, slot 1
// (B at 0xBFFC0 drives a different object at +0xBEAF4 slot 2.) Single read site, so one patch.
static const uint32_t kVerAOrig  = 0xBFF74;
static const uint32_t kVerAReloc = 0x112000;   // clear of B at 0x111000 (43 entries = 172 bytes)
static CategoryMemoryEdit g_verAEdits[] = {
    {0x3A107, kVerAOrig, kVerAReloc, 0, 4, false},
};
static bool g_verARelocated = false;

// ---------------------------------------------------------------------------------------------
// Sortstyle relocation -- the big 256x64 sprite the category-change overlay shows.
//
// Two separate things happen on a category change, and only the first was ever fixed:
//
//   Version_View  (obj+0xBEAF4)  the one-line label under the song title, gated to cat 2..18.
//   Sortstyle_View(obj+0xBED10)  the two-line overlay, gated to cat 0..1 and 19..46.
//
// Disassembled at 0x4447DF (and an identical twin at 0x44497B):
//     mov  eax,[esi+0xBC57C]        ; active category
//     cmp  eax,edi(0) / jl  ...     ; cat 0..1 take the special cases below
//     cmp  eax,1      / jle ...
//     cmp  eax,0x13   / jl  0x44483E   <- outer gate: below 19, nothing happens
//     cmp  eax,0x2e   / jg  0x44483E   <- and above 46, nothing happens
//     cmp  eax,0 / jne ; mov ecx,[esi+0xC000C]   ; cat 0 -> sortstyle[0]
//     cmp  eax,1 / jne ; mov edx,[esi+0xC0010]   ; cat 1 -> sortstyle[1]
//     cmp  eax,0x13 / jl / cmp eax,0x2e / jg     <- inner gate on the array read
//     mov  eax,[esi+eax*4+0xBFFC8]  ; cat 19..46 -> sortstyle[cat-17]
//     push eax / push 0 / lea ecx,[esi+0xBED10] / push 2 / call sub_402910   ; set texture
//     lea  ecx,[esi+0xBED10] / call sub_402990                               ; START the animation
//
// Custom categories are 47+, so the outer gate sent them straight past BOTH calls. Widening the
// draw dispatcher at 0x43604 (done earlier) made the renderer visit Sortstyle_View, but the object
// was never handed a texture and its animation was never started -- which is exactly "no sale
// nada, ni siquiera el mask".
//
// 0xBFFC8 is a BIASED base: it is only ever indexed by cat>=19, so effective addresses start at
// 0xC0014 = sortstyle[2]. The relocated array drops the bias and is indexed by category directly,
// so slot N belongs to category N for every N -- natives copied in, customs loaded on top.
static const uint32_t kSortBiasedOrig = 0xBFFC8;   // [base + cat*4], cat 19..46 -> sortstyle[2..29]
static const uint32_t kSortReloc      = 0x113000;  // clear of A at 0x112000; 128 entries = 512 bytes
static const uint32_t kSortNativeLo   = 19;        // first category served out of the array
static const uint32_t kSortNativeHi   = 46;        // last native one
static CategoryMemoryEdit g_sortEdits[] = {
    {0x44820, kSortBiasedOrig, kSortReloc, 0, 4, false},
    {0x449BC, kSortBiasedOrig, kSortReloc, 0, 4, false},
};
// Both gates, in both copies of the routine. The `cmp eax,0x13` floors stay: 47 clears them.
static CategoryMemoryEdit g_sortGuardEdits[] = {
    {0x447F5, 46, 127, 0, 1, false},   // outer gate, first copy
    {0x4481A, 46, 127, 0, 1, false},   // inner gate on the array read
    {0x44991, 46, 127, 0, 1, false},   // outer gate, twin
    {0x449B6, 46, 127, 0, 1, false},   // inner gate, twin
};
static bool g_sortRelocated = false;

static bool InstallSortstyleRelocation() {
    for (const auto& e : g_sortEdits)
        if (memcmp(Rva(e.rva), &e.expected, e.length) != 0) {
            LogLine("[sortstyle] reloc ABORTED: rva=0x%05lx does not hold base 0x%lx",
                    (unsigned long)e.rva, (unsigned long)e.expected);
            return false;
        }
    for (const auto& e : g_sortGuardEdits)
        if (memcmp(Rva(e.rva), &e.expected, e.length) != 0) {
            LogLine("[sortstyle] gate widen ABORTED: rva=0x%05lx is not the expected imm8 %lu",
                    (unsigned long)e.rva, (unsigned long)e.expected);
            return false;
        }
    for (auto& e : g_sortEdits) {
        memcpy(&e.original, Rva(e.rva), e.length);
        if (!WriteMemory(Rva(e.rva), &e.replacement, e.length)) return false;
        e.installed = true;
    }
    for (auto& e : g_sortGuardEdits) {
        memcpy(&e.original, Rva(e.rva), e.length);
        if (!WriteMemory(Rva(e.rva), &e.replacement, e.length)) return false;
        e.installed = true;
    }
    g_sortRelocated = true;
    LogLine("[sortstyle] array 0x%lx -> 0x%lx (2 reads, bias dropped), gates 19..46 -> 19..127",
            (unsigned long)kSortBiasedOrig, (unsigned long)kSortReloc);
    return true;
}

static void RemoveSortstyleRelocation() {
    for (auto& e : g_sortEdits) {
        if (!e.installed) continue;
        WriteMemory(Rva(e.rva), &e.original, e.length);
        e.installed = false;
    }
    for (auto& e : g_sortGuardEdits) {
        if (!e.installed) continue;
        WriteMemory(Rva(e.rva), &e.original, e.length);
        e.installed = false;
    }
    g_sortRelocated = false;
}
// ---------------------------------------------------------------------------------------------

static bool InstallVersionArrayARelocation() {
    for (const auto& e : g_verAEdits)
        if (memcmp(Rva(e.rva), &e.expected, e.length) != 0) {
            LogLine("[version] A reloc ABORTED: rva=0x%05lx not the expected base", (unsigned long)e.rva);
            return false;
        }
    for (auto& e : g_verAEdits) {
        memcpy(&e.original, Rva(e.rva), e.length);
        if (!WriteMemory(Rva(e.rva), &e.replacement, e.length)) return false;
        e.installed = true;
    }
    g_verARelocated = true;
    LogLine("[version] array A relocated 0x%lx -> 0x%lx", (unsigned long)kVerAOrig, (unsigned long)kVerAReloc);
    return true;
}

static void RemoveVersionArrayARelocation() {
    for (auto& e : g_verAEdits) {
        if (!e.installed) continue;
        WriteMemory(Rva(e.rva), &e.original, e.length);
        e.installed = false;
    }
    g_verARelocated = false;
}
static bool g_versionRelocated = false;

// The runtime updater refuses to touch the version label outside a category range:
//     mov eax,[esi+0xBC57C]   ; active category index (same value passed to FUN_00435070 as param2)
//     cmp eax,2   / jl  skip
//     cmp eax,18  / jg  skip  <- imm8 patched below
//     ... sub_402910(anim, 2, 0, this[ this[221*this[33]+295] + 196592 ])
// Custom categories are 47+, so the assignment never ran for them and the animation object simply
// kept whatever texture it was last handed -- a native label, which is exactly what showed over
// custom songs. Raising the ceiling lets the update fire for our slots too.
// Two sites, both immediately ahead of the relocated read; a third copy at 0x33322 feeds a
// different array and is deliberately left alone.
// 0x33322 is the one that matters: it sits in the same function as the master reads above and the
// relocated array read at 0x33367. The other two belong to a separate screen that shows the same
// label; widening them too keeps the two paths consistent.
static CategoryMemoryEdit g_verGuardEdits[] = {
    {0x33322, 18, 127, 0, 1, false},
    // 0x435DC picks WHICH animation object shows the category, and the ranges are not one gate but
    // two branches:
    //     cat 2..18            -> lea ecx,[esi+0xBEAF4]   Version_View, the one-line label
    //     cat 0..1 or 19..46   -> lea ecx,[esi+0xBED10]   Sortstyle_View, the two-line sprite
    //     anything else        -> neither branch, nothing is shown
    // Custom categories are 47+, so they fell off the end. Widening the FIRST range (as an earlier
    // attempt did) routed them to the one-line object; the two-line overlay is the second branch,
    // so the ceiling that has to move is the 46 at 0x43604 -- and the 18 stays as it was.
    {0x43604, 46, 127, 0, 1, false},
    {0x4479B, 18, 127, 0, 1, false},
    {0x44937, 18, 127, 0, 1, false},
};

static bool InstallVersionGuardWiden() {
    for (const auto& e : g_verGuardEdits) {
        if (memcmp(Rva(e.rva), &e.expected, e.length) != 0) {
            LogLine("[version] guard widen ABORTED: rva=0x%05lx is not the expected imm8 %lu",
                    (unsigned long)e.rva, (unsigned long)e.expected);
            return false;
        }
    }
    for (auto& e : g_verGuardEdits) {
        memcpy(&e.original, Rva(e.rva), e.length);
        if (!WriteMemory(Rva(e.rva), &e.replacement, e.length)) return false;
        e.installed = true;
    }
    LogLine("[version] label-update category range widened 2..18 -> 2..127 (%u sites)",
            (unsigned)(sizeof(g_verGuardEdits)/sizeof(g_verGuardEdits[0])));
    return true;
}

static void RemoveVersionGuardWiden() {
    for (auto& e : g_verGuardEdits) {
        if (!e.installed) continue;
        WriteMemory(Rva(e.rva), &e.original, e.length);
        e.installed = false;
    }
}

// Bumped whenever the inject rewrites master[], so the filter's id index rebuilds.
static uint32_t g_masterGen = 0;
// Published by the filter so a /GS failure report names the category that led into it.
static uint32_t g_gsLastCat   = 0xFFFFFFFFu;   // set by the filter, for correlation
static uint32_t g_gsLastCount = 0;
// master[] relocation delta.
//
// It used to be 0xEFDF4, putting the array at obj+0x14E0B0 -- chosen as "just past the original
// object end (~0x103540)". That end was wrong. The SongSelect object keeps live fields far above
// it: the decompilation shows accesses up to obj+0x210988, with a dense cluster around
// obj+0x20C000..0x20E900. Those fields land INSIDE master[] once the array grows past entry ~880:
//
//     this+2146528 -> master[880]     this+2150756 -> master[885]
//     this+2148028 -> master[882]     this+2152376 -> master[887]
//     this+2148052 -> master[882]     [esi+0x20E894] -> master[891]
//
// The game writes those every frame, so song entries in that band had their ids and level floats
// quietly zeroed. A zeroed level makes the filter skip the entry; a zeroed id makes the disc-art
// loader sprintf "system\disc\%s.bmp" over an unterminated field, which is what smashed a
// 260-byte stack buffer and produced the CYT-2.2 crash. With the stock 436 entries master ended at
// obj+0x1A0DA0, below the cluster, so this only ever appears on a grown library.
//
// 0x400000 puts the array at obj+0x400000, about 2 MB clear of the highest field anywhere in the
// decompilation, and the offsets our own arrays use (0x110000..0x1131FC) sit in a 1 MB hole that
// no field touches.
static const uint32_t kMasterOrigBase = 0x5E2BC;
static const uint32_t kMasterHome     = 0x400000;                        // where master[] now lives
static const uint32_t kMasterRel      = kMasterHome - kMasterOrigBase;   // 0x3A1D44
static bool g_capacityPatched = false;     // master-array cap raised + relocated (+kMasterRel)
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
    // master[] base: the capacity patch relocated the array by +kMasterRel, and the game now
    // reads/writes master there, so our inject MUST target the same place.
    uint32_t masterBase = ecx + kMasterOrigBase + (g_capacityPatched ? kMasterRel : 0u);
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
    // DIAGNOSTIC (one-shot): every custom entry is a byte-for-byte clone of master[0] with only id,
    // levels, version and the songname pointer overridden -- so it inherits every OTHER field of that
    // category is about to be filtered, so LoadCustomCategorySongnames loads only that category.
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
    // Only a pass that actually wrote entries invalidates the filter's id index. Bumping this on
    // every entry to the inject would rebuild a 60k-entry index on every category change, which is
    // precisely the cost the index exists to remove.
    if (appended) ++g_masterGen;
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
    // Definitive answer to "are custom songs leaking into the native categories?". Membership is the
    // id list, so walk the NATIVE categories (0..46) and count entries whose id starts with "bms".
    // Anything other than zero means the inject is writing outside its own slots.
    {
        std::string leaks;
        uint32_t leakTotal = 0;
        for (uint32_t c = 0; c < 47; ++c) {
            uint32_t cnt = SafeRead32(CatCountTable() + c * 4);
            if (!cnt || cnt > kCatNameStride) continue;
            uint32_t hits = 0;
            for (uint32_t k = 0; k < cnt; ++k) {
                uint32_t addr = CatNameTable() + (k + c * kCatNameStride) * kCatNameSize;
                if (!CanReadPtr(reinterpret_cast<void*>(addr), 4)) break;
                const char* id = reinterpret_cast<const char*>(addr);
                if (id[0] == 'b' && id[1] == 'm' && id[2] == 's') ++hits;
            }
            if (hits) {
                char buf[48];
                snprintf(buf, sizeof(buf), " cat=%lu:%lu/%lu", (unsigned long)c,
                         (unsigned long)hits, (unsigned long)cnt);
                leaks += buf; leakTotal += hits;
            }
        }
        LogLine("[catleak] custom ids inside native categories 0..46: %lu%s",
                (unsigned long)leakTotal, leakTotal ? leaks.c_str() : " (none - native categories are clean)");
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
        uint32_t mbase = kMasterOrigBase + (g_capacityPatched ? kMasterRel : 0u); // relocated master
        levels(ecx + 0x12C + 884u*i, "vis ");
        levels(ecx + mbase + 884u*i, "mstr");
    };
    for (uint32_t i = 0; i < 5 && i < master; ++i) dumpEntry(i);
    for (uint32_t i = (master > 3 ? master - 3 : 0); i < master; ++i) dumpEntry(i);
}

// Everything that has to happen before a category is filtered: inject the custom songs into
// master[], preload that category's songname textures, and fill the relocated wheel-icon,
// version and sortstyle arrays. Extracted from the 0x35070 trace hook so the replacement
// filter (FilterCategoryImpl) can run the same preparation -- it stands in for the original
// function, so nothing else is left to do it.
static void SongSelectPreFilter(uint32_t obj, uint32_t cat) {
    DumpSongSelectStructure(obj, cat);
    InjectCustomSongsIntoSongSelect(obj, "pre-filter", cat);
    LoadCustomCategorySongnames(obj, cat);
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
                // Small array = the tab row (short folder name). Big array = the large centre
                // label, which gets its own asset with the full details.txt name.
                loadTex(gfxCtx, fn, reinterpret_cast<void*>(obj + g_catTexSmallBase + slot * 4), nullptr);
                char bigFn[96] = {};
                snprintf(bigFn, sizeof(bigFn), "System\\SongSelect\\Sortimage\\categorybig_%02d.bmp", catNum);
                loadTex(gfxCtx, bigFn, reinterpret_cast<void*>(obj + g_catTexBigBase + slot * 4), nullptr);
            }
            // Version labels. The natives are copied rather than reloaded: the game's own loader
            // already filled them at the original base, and this array is only read, never
            // written, by the renderer. Done here and not at patch time because those textures
            // do not exist until SongSelect has run its loader.
            static uint32_t s_verFilledFor = 0;
            if (g_versionRelocated &&
                CanReadPtr(reinterpret_cast<void*>(obj + kVerArrayOrig), kVerNativeCount * 4)) {
                // The native half is re-copied on EVERY pass, on purpose. Copying it once was a
                // bug: on the first filter call the game's own loader had not filled the source
                // yet, so 19 zeros were copied and every native song lost its label -- while the
                // custom slots, loaded here directly, looked fine. Re-copying is 76 bytes and
                // self-heals as soon as the loader has run.
                SafeWriteBytes(obj + kVerArrayReloc,
                               reinterpret_cast<void*>(obj + kVerArrayOrig), kVerNativeCount * 4);
                uint32_t nativeOk = 0;
                for (uint32_t v = 0; v < kVerNativeCount; ++v)
                    if (SafeRead32(obj + kVerArrayReloc + v * 4)) ++nativeOk;
                // Array A gets the same treatment: natives re-copied every pass, customs once.
                if (g_verARelocated &&
                    CanReadPtr(reinterpret_cast<void*>(obj + kVerAOrig), kVerNativeCount * 4))
                    SafeWriteBytes(obj + kVerAReloc,
                                   reinterpret_cast<void*>(obj + kVerAOrig), kVerNativeCount * 4);
                // The custom textures DO load once: the pool is a bump allocator that never
                // frees, so reloading them per pass burned ~770KB each time.
                if (s_verFilledFor == obj) {
                    static uint32_t s_lastNativeOk = 0xffffffffu;
                    if (nativeOk != s_lastNativeOk) {
                        s_lastNativeOk = nativeOk;
                        LogLine("[version] native labels present: %lu/%u", (unsigned long)nativeOk,
                                (unsigned)kVerNativeCount);
                    }
                }
                // Re-entering SongSelect from the menu can reuse the same object address while
                // the texture pool has been reset underneath it -- our slots come back null and
                // the labels vanish, which is exactly the "it worked, I went to the menu, came
                // back and the text was gone" case. So the refill condition is not just "new
                // object" but also "our first custom slot lost its texture".
                bool slotsLost = g_customCategoryCount &&
                                 !SafeRead32(obj + kVerAReloc + kVerNativeCount * 4);
                if (s_verFilledFor != obj || slotsLost) {
                if (slotsLost && s_verFilledFor == obj)
                    LogLine("[version] custom slots were reset (menu round-trip); reloading");
                s_verFilledFor = obj;
                unsigned long ok = 0;
                for (uint32_t c = 0; c < g_customCategoryCount; ++c) {
                    uint32_t slotIdx = kVerNativeCount + c;
                    char vf[112] = {};
                    snprintf(vf, sizeof(vf),
                             "System\\SongSelect\\V_View\\version\\Version_%02lu.bmp",
                             (unsigned long)slotIdx);
                    loadTex(gfxCtx, vf,
                            reinterpret_cast<void*>(obj + kVerArrayReloc + slotIdx * 4), nullptr);
                    // The visible label lives in array A, so load it there too.
                    if (g_verARelocated) {
                        char af[112] = {};
                        snprintf(af, sizeof(af),
                                 "System\\SongSelect\\Version\\Version_%02lu.bmp",
                                 (unsigned long)slotIdx);
                        loadTex(gfxCtx, af,
                                reinterpret_cast<void*>(obj + kVerAReloc + slotIdx * 4), nullptr);
                    }
                    if (SafeRead32(obj + kVerAReloc + slotIdx * 4)) ++ok;
                }
                LogLine("[version] relocated array filled: %lu/%u native present + %lu/%lu custom loaded",
                        (unsigned long)nativeOk, (unsigned)kVerNativeCount, ok,
                        (unsigned long)g_customCategoryCount);
                }
            }
            // Sortstyle overlay, same shape as the version arrays above: the natives are
            // re-copied on every pass (on the first one the game's own loader may not have run
            // yet, and copying zeros once would blank every native category's overlay), the
            // customs are loaded once because the texture pool is a bump allocator that never
            // frees. Indexing is by category now that the 0xBFFC8 bias is gone, so slot 19..46
            // holds what sortstyle[2..29] used to and 47.. holds ours.
            if (g_sortRelocated) {
                const uint32_t nSort = kSortNativeHi - kSortNativeLo + 1;
                if (CanReadPtr(reinterpret_cast<void*>(obj + kSortBiasedOrig + kSortNativeLo * 4),
                               nSort * 4))
                    SafeWriteBytes(obj + kSortReloc + kSortNativeLo * 4,
                                   reinterpret_cast<void*>(obj + kSortBiasedOrig + kSortNativeLo * 4),
                                   nSort * 4);
                static uint32_t s_sortFilledFor = 0;
                bool sortLost = g_customCategoryCount && !SafeRead32(obj + kSortReloc + 47 * 4);
                if (s_sortFilledFor != obj || sortLost) {
                    s_sortFilledFor = obj;
                    unsigned long sok = 0;
                    for (uint32_t c = 0; c < g_customCategoryCount; ++c) {
                        const uint32_t cat = 47 + c;
                        char sf[128] = {};
                        snprintf(sf, sizeof(sf),
                                 "System\\SongSelect\\V_View\\sortstyle\\sortstyle_%02lu.bmp",
                                 (unsigned long)cat);
                        loadTex(gfxCtx, sf,
                                reinterpret_cast<void*>(obj + kSortReloc + cat * 4), nullptr);
                        if (SafeRead32(obj + kSortReloc + cat * 4)) ++sok;
                    }
                    LogLine("[sortstyle] %lu natives copied (cat 19..46) + %lu/%lu custom loaded",
                            (unsigned long)nSort, sok, (unsigned long)g_customCategoryCount);
                }
            }
            LogLine("[category] loaded %lu wheel icons into relocated slots 47.. small=%08lx big=%08lx",
                    (unsigned long)g_customCategoryCount,
                    (unsigned long)SafeRead32(obj + g_catTexSmallBase + 47 * 4),
                    (unsigned long)SafeRead32(obj + g_catTexBigBase + 47 * 4));
        }
    }
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
    if (h->rva == 0x35070) SongSelectPreFilter(r->ecx, a2);
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
// SongSelect master/song arrays by +kMasterRel so they don't overrun the object state at
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

static bool InstallSongCapacityPatches(uint32_t newCap) {
    if (g_capacityPatched) return true;
    const uint32_t REL = kMasterRel;              // see kMasterHome for why it is where it is
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
    // 0x332DB and 0x33304 are the version-label updater's own reads of the master version field:
    //     this[196471] = this[221*this[196470] + 96651];   // previous song
    //     this[196472] = this[221*this[33]      + 96651];   // current song
    // and the assignment below is gated on those two DIFFERING. Left un-relocated they both read
    // the vacated original master and returned 0, so "changed" was permanently false and the label
    // texture was never assigned -- no matter how correct the array, index and guard were.
    for (uintptr_t off : {0x39447u, 0x332DBu, 0x33304u}) ok &= w32(off, 0x5E62C + REL);
    for (uintptr_t off : {0x3946Du, 0x39301u}) ok &= w32(off, 0x5E4D0 + REL);
    ok &= w32(0x362B2, (uint32_t)(-(int32_t)(0x5E29C + REL)));
    ok &= w32(0x362EF, (uint32_t)(-(int32_t)(0x5E298 + REL)));

    // Texture arrays go in the RELOCATION GAP — the space between the original object end
    // (~0x103540) and the relocated master[] start (kMasterHome). That space is always
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

// Version-label array relocation.
//
// From the decompiled loader, ONE loop fills two adjacent arrays and a third follows:
//     A  System\\SongSelect\\Version\\Version_NN         dword 196573..196591  (19)
//     B  System\\SongSelect\\V_View\\version\\Version_NN  dword 196592..196610  (19)
//        System\\SongSelect\\V_View\\sortstyle\\...       dword 196611..196640  (30)
// The label the game draws comes from B:  this[ this[221*this[33] + 295] + 196592 ].
// Raising the loop bound to fit custom slots overran B and sortstyle, which is why custom labels
// appeared over native categories -- so the loop stays at 19 and the READS move instead.
//
// The addressing is object-relative ([obj + idx*4 + disp32]), so the new array has to live inside
// the object: the reloc gap between the original end (~0x103540) and the relocated master[]
// (0x14E0B0), the same committed region the category texture arrays already use at 0x110000/0x110800.
// The loader is deliberately NOT repointed: natives keep loading where they always did, and the
// inject copies those 19 entries across before adding ours at 19+.
// Read sites only (found by scanning the exe for the disp32; the 0x377B7 lea is the loader).
static CategoryMemoryEdit g_versionEdits[] = {
    {0x33367, kVerArrayOrig, kVerArrayReloc, 0, 4, false},
    {0x37900, kVerArrayOrig, kVerArrayReloc, 0, 4, false},
    {0x447C0, kVerArrayOrig, kVerArrayReloc, 0, 4, false},
    {0x4495C, kVerArrayOrig, kVerArrayReloc, 0, 4, false},
};

static bool InstallVersionArrayRelocation() {
    for (const auto& e : g_versionEdits) {
        if (memcmp(Rva(e.rva), &e.expected, e.length) != 0) {
            LogLine("[version] reloc ABORTED: rva=0x%05lx does not hold the expected base 0x%lx",
                    (unsigned long)e.rva, (unsigned long)e.expected);
            return false;
        }
    }
    for (auto& e : g_versionEdits) {
        memcpy(&e.original, Rva(e.rva), e.length);
        if (!WriteMemory(Rva(e.rva), &e.replacement, e.length)) return false;
        e.installed = true;
    }
    g_versionRelocated = true;
    LogLine("[version] label array relocated 0x%lx -> 0x%lx (%u read sites; loader untouched)",
            (unsigned long)kVerArrayOrig, (unsigned long)kVerArrayReloc,
            (unsigned)(sizeof(g_versionEdits)/sizeof(g_versionEdits[0])));
    return true;
}

static void RemoveVersionArrayRelocation() {
    for (auto& e : g_versionEdits) {
        if (!e.installed) continue;
        WriteMemory(Rva(e.rva), &e.original, e.length);
        e.installed = false;
    }
    g_versionRelocated = false;
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

// =============================================================================================
// Replacement for sub_435070 -- the category filter.
//
// The original builds the whole visible list in a 376.4 KB array ON THE STACK
// (mov eax,0x5E1C8 / call __chkstk), against a 1 MB process reserve, and the array is exactly
// 436 entries of 884 bytes plus one hand-written cookie dword. That array IS the 436-song
// per-category ceiling: there is no bounds check anywhere in the gather loop, so a category with
// more ids simply writes past it. The binary has no /GS, so nothing would catch that.
//
// What the original does, in order:
//   1. if a3, copy the currently selected song id out of the global at 0x1B2EB04 into char[32]
//      -- with an unbounded do/while that stops only at the NUL.
//   2. for each of catCount[cat] ids in the category name table, LINEARLY search master[] with
//      _stricmp until it matches, and copy the whole 884-byte entry into the stack array.
//      That is O(ids x masterCount) string compares: at 436 x 60000 it is already ~26M compares
//      per category change, and it grows with the library.
//   3. copy every gathered entry whose float at +0x214 is non-zero into the visible array at
//      obj+300 (that float is the level for the active mode; zero means "not playable here").
//   4. find the remembered id in the visible list and restore the selection and scroll position.
//
// This version keeps that behaviour exactly, minus the stack array (the gather is fused into the
// write, so nothing is buffered at all) and minus the linear search (a case-insensitive hash index
// over master[], rebuilt only when master changes).
//
// The two phases are kept SEPARATE and bug-compatible, deliberately. An earlier version fused them
// and "corrected" two things on the way: it reported the written count instead of the gathered one,
// and let the cap bound the write rather than the gather so no visible slot went to waste. Both
// looked strictly safer. The first category that exercised either -- 243 ids of which 3 had no
// chart for the active mode -- died on a /GS buffer-overrun check, right after eleven categories
// with no unplayable songs had been browsed without trouble. Whatever downstream structure is
// sized from the gathered count, it is not this function's to redefine, and the job here was to
// lift the 436 ceiling, not to change what the game counts. So: gather, publish the gathered
// total, then write the playable ones. Exactly as shipped.
//
// ONE deviation remains, and it removes undefined behaviour rather than changing a value:
//   - when a3 is 0 the original leaves its char[32] uninitialised and then _stricmp's against it.
//     Here the buffer is zeroed, which cannot match a real id, so the selection lands on 0 -- the
//     same outcome the garbage produced in practice, without the undefined read.
//
// The visible array itself is untouched, and it is still 436 entries: obj+300 + 436*884 lands
// exactly on the original master[] base, so visible[] and master[] were adjacent, equal-size
// arrays. master[] is relocated by +kMasterRel, which should leave that region free -- but a static
// scan could not prove it, so g_visibleCap stays at 436 and ProbeVacatedMaster watches the region
// for writes instead. Raising the cap is a one-line change once the probe reports it clean.
// =============================================================================================

static const uint32_t kEntryBytes    = 0x374;   // 884
static const uint32_t kEntryIdOff    = 0x10C;
static const uint32_t kEntryLevelOff = 0x214;
static const uint32_t kVisibleBase   = 300;     // obj+300, stride kEntryBytes
static const uint32_t kMasterOrig    = 0x5E2BC;
static const uint32_t kSelectedIdVar = 0x1B2EB04;

// Object fields the filter owns, as byte offsets (the decompiler shows them as this[N] dwords).
static const uint32_t kObjActiveCat   = 7  * 4;   // 0x1C
static const uint32_t kObjVisCount    = 31 * 4;   // 0x7C
static const uint32_t kObjMasterCount = 32 * 4;   // 0x80
static const uint32_t kObjSelected    = 33 * 4;   // 0x84
static const uint32_t kObjRetVal      = 34 * 4;   // 0x88
static const uint32_t kObjSelected2   = 35 * 4;   // 0x8C
static const uint32_t kObjRetCopy     = 36 * 4;   // 0x90
static const uint32_t kObjScrollA     = 49 * 4;   // 0xC4
static const uint32_t kObjScrollB     = 51 * 4;   // 0xCC

static uint32_t g_visibleCap = 436;   // see the note above before raising this

// ---- case-insensitive id index over master[] --------------------------------------------------
// Ids are at most 15 characters in a 16-byte field. Keys are stored lowercased in our own memory
// so a lookup never re-reads the game's object, and duplicates resolve to the LOWEST master index
// -- which is what the original's first-match linear scan returned.
struct MasterIdIndex {
    std::vector<char>    keys;      // 16 bytes per master entry
    std::vector<int32_t> table;     // open addressing, -1 = empty
    uint32_t mask  = 0;
    uint32_t base  = 0;
    uint32_t count = 0;
    uint32_t gen   = 0xFFFFFFFFu;
};
static MasterIdIndex g_midx;

static void NormalizeId(const char* src, char* dst16) {
    memset(dst16, 0, 16);
    for (int i = 0; i < 15 && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst16[i] = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
    }
}

static uint32_t HashId16(const char* k) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16 && k[i]; ++i) h = (h ^ (unsigned char)k[i]) * 16777619u;
    return h;
}

static void EnsureMasterIndex(uint32_t masterBase, uint32_t count) {
    if (g_midx.base == masterBase && g_midx.count == count && g_midx.gen == g_masterGen) return;
    g_midx.base = masterBase; g_midx.count = count; g_midx.gen = g_masterGen;
    g_midx.keys.assign((size_t)count * 16, 0);
    size_t cap = 64;
    while (cap < (size_t)count * 2) cap <<= 1;
    g_midx.table.assign(cap, -1);
    g_midx.mask = (uint32_t)(cap - 1);
    uint32_t dup = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t addr = masterBase + i * kEntryBytes + kEntryIdOff;
        if (!CanReadPtr(reinterpret_cast<void*>(addr), 16)) break;
        char* key = &g_midx.keys[(size_t)i * 16];
        NormalizeId(reinterpret_cast<const char*>(addr), key);
        if (!key[0]) continue;                       // empty slot, not addressable by id
        uint32_t h = HashId16(key) & g_midx.mask;
        bool exists = false;
        while (g_midx.table[h] >= 0) {
            if (memcmp(&g_midx.keys[(size_t)g_midx.table[h] * 16], key, 16) == 0) { exists = true; break; }
            h = (h + 1) & g_midx.mask;
        }
        if (exists) { ++dup; continue; }             // keep the first, like the original scan
        g_midx.table[h] = (int32_t)i;
    }
    LogLine("[filter] master id index rebuilt: %lu entries, %lu buckets, %lu duplicate id(s)",
            (unsigned long)count, (unsigned long)g_midx.table.size(), (unsigned long)dup);
}

static int32_t LookupMaster(const char* rawId) {
    if (g_midx.table.empty()) return -1;
    char key[16];
    NormalizeId(rawId, key);
    if (!key[0]) return -1;
    uint32_t h = HashId16(key) & g_midx.mask;
    while (g_midx.table[h] >= 0) {
        if (memcmp(&g_midx.keys[(size_t)g_midx.table[h] * 16], key, 16) == 0) return g_midx.table[h];
        h = (h + 1) & g_midx.mask;
    }
    return -1;
}

// ---- read-only probe of the region master[] used to occupy ------------------------------------
// visible[] ends exactly where master[] began, so freeing master[] is what would let visible[]
// grow past 436. A byte scan of .text found ~113 displacements inside that range, but almost all
// of them decode as data rather than instructions and none could be confirmed as a live field.
// Rather than guess, snapshot the region and report anything that writes to it. Purely read-only:
// this cannot itself break anything, and one session with category changes and a played song is
// enough to settle whether the cap can be raised.
static std::vector<unsigned char> g_vacatedSnap;
static uint32_t g_vacatedBase = 0;
static unsigned g_vacatedReports = 0;

static void ProbeVacatedMaster(uint32_t obj) {
    if (!g_capacityPatched) return;
    const uint32_t base = obj + kMasterOrig;
    const uint32_t len  = 436 * kEntryBytes;          // 385 424 bytes
    if (!CanReadPtr(reinterpret_cast<void*>(base), len)) return;
    if (g_vacatedBase != base) {
        g_vacatedBase = base;
        g_vacatedSnap.assign(len, 0);
        memcpy(g_vacatedSnap.data(), reinterpret_cast<void*>(base), len);
        LogLine("[vacated] snapshot taken at obj+0x%lX (%lu bytes = %u entries)",
                (unsigned long)kMasterOrig, (unsigned long)len, 436u);
        return;
    }
    if (g_vacatedReports >= 8) return;
    const unsigned char* now = reinterpret_cast<const unsigned char*>(base);
    uint32_t changed = 0, firstOff = 0xFFFFFFFFu, lastOff = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (now[i] == g_vacatedSnap[i]) continue;
        ++changed;
        if (firstOff == 0xFFFFFFFFu) firstOff = i;
        lastOff = i;
    }
    ++g_vacatedReports;
    if (!changed) {
        LogLine("[vacated] CLEAN - nothing wrote to the old master region since the snapshot");
    } else {
        LogLine("[vacated] %lu byte(s) changed, first at +%lu (entry %.2f field 0x%lX), last at +%lu"
                " -- the region is NOT free, keep g_visibleCap at 436",
                (unsigned long)changed, (unsigned long)firstOff,
                (double)firstOff / (double)kEntryBytes, (unsigned long)(firstOff % kEntryBytes),
                (unsigned long)lastOff);
        memcpy(g_vacatedSnap.data(), now, len);       // re-baseline so the next report is a delta
    }
}


// Compare every id the index recorded against what master[] holds now. The index is built once and
// only rebuilt when the inject actually appends, so a mismatch means something wrote over master[]
// afterwards -- which is exactly the shape of "a slot the index resolved comes back with an empty
// id". Bounded output; runs only when the filter saw an entry it could not explain.
static void VerifyMasterAgainstIndex(uint32_t cat) {
    if (g_midx.keys.empty()) return;
    uint32_t mismatched = 0, emptied = 0, firstBad = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < g_midx.count; ++i) {
        const char* want = &g_midx.keys[(size_t)i * 16];
        if (!want[0]) continue;                                  // never indexed
        const uint32_t addr = g_midx.base + i * kEntryBytes + kEntryIdOff;
        if (!CanReadPtr(reinterpret_cast<void*>(addr), 16)) break;
        char have[16];
        NormalizeId(reinterpret_cast<const char*>(addr), have);
        if (memcmp(have, want, 16) == 0) continue;
        ++mismatched;
        if (!have[0]) ++emptied;
        if (firstBad == 0xFFFFFFFFu) {
            firstBad = i;
            LogLine("[integrity] cat=%lu master[%lu] was '%.15s' at index time, now '%.15s'",
                    (unsigned long)cat, (unsigned long)i, want, have);
        }
    }
    if (mismatched)
        LogLine("[integrity] cat=%lu MASTER CORRUPTED: %lu/%lu ids changed since indexing"
                " (%lu now empty), first at master[%lu]",
                (unsigned long)cat, (unsigned long)mismatched, (unsigned long)g_midx.count,
                (unsigned long)emptied, (unsigned long)firstBad);
    else
        LogLine("[integrity] cat=%lu master[] intact: all %lu indexed ids still match",
                (unsigned long)cat, (unsigned long)g_midx.count);
}

// ---- the filter itself ------------------------------------------------------------------------
static uint32_t FilterCategoryImpl(uint32_t obj, uint32_t cat, uint32_t keepSelection) {
    SongSelectPreFilter(obj, cat);
    ProbeVacatedMaster(obj);

    const uint32_t masterCount = SafeRead32(obj + kObjMasterCount);
    const uint32_t masterBase  = obj + kMasterOrig + (g_capacityPatched ? kMasterRel : 0u);

    // The id to re-select once the list is rebuilt. Bounded, unlike the original's do/while.
    char want[16] = {};
    if (keepSelection && CanReadPtr(reinterpret_cast<void*>(kSelectedIdVar), 16))
        NormalizeId(reinterpret_cast<const char*>(kSelectedIdVar), want);

    // Phase 1 -- gather, exactly as the original did, but into the heap instead of 376 KB of stack.
    // The cap bounds THIS list, because the original's cap was the size of its stack array.
    static std::vector<uint32_t> s_gathered;
    s_gathered.clear();
    uint32_t missing = 0, unplayable = 0, available = 0;
    if (masterCount && masterCount <= g_songCapacity &&
        CanReadPtr(reinterpret_cast<void*>(masterBase), kEntryBytes)) {
        EnsureMasterIndex(masterBase, masterCount);
        uint32_t ids = SafeRead32(CatCountTable() + cat * 4);
        if (ids > kCatNameStride) ids = kCatNameStride;
        available = ids;
        for (uint32_t k = 0; k < ids; ++k) {
            const uint32_t nameAddr = CatNameTable() + (k + cat * kCatNameStride) * kCatNameSize;
            if (!CanReadPtr(reinterpret_cast<void*>(nameAddr), kCatNameSize)) break;
            const int32_t mi = LookupMaster(reinterpret_cast<const char*>(nameAddr));
            if (mi < 0) { ++missing; continue; }
            if (s_gathered.size() >= g_visibleCap) break;
            s_gathered.push_back((uint32_t)mi);
        }
    }

    const uint32_t gathered = (uint32_t)s_gathered.size();

    // Phase 2 -- copy across the entries that have a chart for the active mode.
    uint32_t written = 0;
    for (uint32_t g = 0; g < gathered; ++g) {
        const uint32_t entry = masterBase + s_gathered[g] * kEntryBytes;
        float level = 0.0f;
        memcpy(&level, reinterpret_cast<void*>(entry + kEntryLevelOff), 4);
        if (level == 0.0f) {
            // The inject guarantees NM >= 1 for every custom song it writes, so a zero here means
            // the entry did not come from the inject -- name the id and the master slot so it is
            // clear whether it is a native song, a truncated append, or something never written.
            if (unplayable < 6)
                LogLine("[filter] cat=%lu UNPLAYABLE master[%lu]%s id='%.31s'",
                        (unsigned long)cat, (unsigned long)s_gathered[g],
                        s_gathered[g] < 430 ? " (NATIVE)" : " (custom range)",
                        reinterpret_cast<const char*>(entry + kEntryIdOff));
            ++unplayable;
            continue;                                        // the game's own visibility test
        }
        SafeWriteBytes(obj + kVisibleBase + written * kEntryBytes,
                       reinterpret_cast<void*>(entry), kEntryBytes);
        ++written;
    }

    // The original publishes the GATHERED count but writes only the playable entries, and never
    // reconciles the two. visible[written .. gathered-1] therefore keeps whatever the previously
    // filtered category left in those slots. The wheel scrolls into that window quite happily and
    // the disc-art loader does:
    //
    //     sprintf(Buffer, "system\\disc\\%s.bmp", (char *)(this + 884 * a2 + 568));
    //
    // where 884*a2 + 568 is visible[a2].id -- so a stale or never-initialised id goes straight into
    // a path, the open fails, and the failure path smashes a 260-byte stack buffer. That is the
    // CYT-2.2 crash, and it needs BOTH a category with songs that have no chart for the active
    // mode (the only one of 25 that had any) and enough scrolling to reach the tail.
    //
    // Publishing `written` instead would be the tidy fix, but the count is load-bearing elsewhere,
    // so leave it alone and make the window harmless: repeat the last valid entry across it. Those
    // slots then hold a real, playable song rather than debris.
    if (written && written < gathered) {
        const uint32_t last = obj + kVisibleBase + (written - 1) * kEntryBytes;
        for (uint32_t i = written; i < gathered; ++i)
            SafeWriteBytes(obj + kVisibleBase + i * kEntryBytes,
                           reinterpret_cast<void*>(last), kEntryBytes);
        LogLine("[filter] cat=%lu tail %lu..%lu had no playable entry; filled from visible[%lu]",
                (unsigned long)cat, (unsigned long)written, (unsigned long)(gathered - 1),
                (unsigned long)(written - 1));
    }

    // An unplayable entry should be impossible -- the inject forces NM >= 1 on every song it
    // writes -- so if one shows up, check whether master[] still holds what we indexed.
    if (unplayable) VerifyMasterAgainstIndex(cat);

    // Nothing playable at all: publish zero rather than a count with no valid entry behind it.
    const uint32_t published = written ? gathered : 0;
    SafeWriteBytes(obj + kObjVisCount, &published, 4);

    // Restore the selection by id. The original searches this[31] entries -- the gathered count --
    // which can read past what phase 2 wrote; bound the search to what is actually there.
    uint32_t selected = 0;
    for (uint32_t i = 0; i < written; ++i) {
        const uint32_t idAddr = obj + kVisibleBase + i * kEntryBytes + kEntryIdOff;
        char have[16];
        NormalizeId(reinterpret_cast<const char*>(idAddr), have);
        if (want[0] && memcmp(have, want, 16) == 0) { selected = i; break; }
    }
    const float scroll = (float)selected * 100.0f;
    const uint32_t retVal = SafeRead32(obj + kObjRetVal);
    SafeWriteBytes(obj + kObjSelected,  &selected, 4);
    SafeWriteBytes(obj + kObjSelected2, &selected, 4);
    SafeWriteBytes(obj + kObjActiveCat, &cat, 4);
    SafeWriteBytes(obj + kObjScrollA,   &scroll, 4);
    SafeWriteBytes(obj + kObjScrollB,   &scroll, 4);
    SafeWriteBytes(obj + kObjRetCopy,   &retVal, 4);

    g_gsLastCat = cat;            // so a /GS failure names the category that led to it
    g_gsLastCount = published;
    static uint32_t s_lastCat = 0xFFFFFFFFu;
    if (cat != s_lastCat || missing || unplayable || gathered >= g_visibleCap) {
        s_lastCat = cat;
        LogLine("[filter] cat=%lu ids=%lu -> gathered=%lu written=%lu (missing=%lu unplayable=%lu)"
                " sel=%lu cap=%lu%s",
                (unsigned long)cat, (unsigned long)available, (unsigned long)published,
                (unsigned long)written, (unsigned long)missing, (unsigned long)unplayable,
                (unsigned long)selected, (unsigned long)g_visibleCap,
                gathered >= g_visibleCap ? "  *** TRUNCATED AT CAP ***" : "");
    }
    return retVal;
}

extern "C" __declspec(dllexport) uint32_t __cdecl EZ2FilterCategory(uint32_t obj, uint32_t cat,
                                                                    uint32_t keepSelection) {
    return FilterCategoryImpl(obj, cat, keepSelection);
}

// ---- installation ------------------------------------------------------------------------------
// The original is __thiscall(this, cat, keepSel, unused) and ends in `ret 0xC`. Its first
// instruction is `mov eax, 0x5E1C8` -- exactly 5 bytes -- so a jmp rel32 replaces it without
// splitting anything, and the expected bytes double as the version check.
static const unsigned char kFilterEntry[5] = { 0xB8, 0xC8, 0xE1, 0x05, 0x00 };
static unsigned char g_filterOriginal[5] = {};
static bool g_filterReplaced = false;

static bool InstallFilterReplacement() {
    if (g_filterReplaced) return true;
    void* target = Rva(0x35070);
    if (memcmp(target, kFilterEntry, sizeof(kFilterEntry)) != 0) {
        LogLine("[filter] replacement ABORTED: 0x35070 does not start with `mov eax,0x5E1C8`");
        return false;
    }
    unsigned char* stub = static_cast<unsigned char*>(AllocExec(32));
    if (!stub) { LogLine("[filter] replacement ABORTED: stub alloc failed"); return false; }
    unsigned char* p = stub;
    // esp is untouched on entry: [esp]=ret [esp+4]=cat [esp+8]=keepSel [esp+12]=unused, ecx=this
    EmitU8(p, 0x8B); EmitU8(p, 0x44); EmitU8(p, 0x24); EmitU8(p, 0x08);  // mov eax,[esp+8]
    EmitU8(p, 0x8B); EmitU8(p, 0x54); EmitU8(p, 0x24); EmitU8(p, 0x04);  // mov edx,[esp+4]
    EmitU8(p, 0x50);                                                     // push eax  (keepSel)
    EmitU8(p, 0x52);                                                     // push edx  (cat)
    EmitU8(p, 0x51);                                                     // push ecx  (this)
    EmitU8(p, 0xE8);
    void* next = p + 4;
    EmitRel32(p, next, reinterpret_cast<void*>(&EZ2FilterCategory));
    EmitU8(p, 0x83); EmitU8(p, 0xC4); EmitU8(p, 0x0C);                   // add esp,12
    EmitU8(p, 0xC2); EmitU8(p, 0x0C); EmitU8(p, 0x00);                   // ret 0xC

    memcpy(g_filterOriginal, target, sizeof(g_filterOriginal));
    unsigned char patch[5];
    patch[0] = 0xE9;
    const intptr_t rel = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(target) - 5;
    memcpy(&patch[1], &rel, 4);
    if (!WriteMemory(target, patch, sizeof(patch))) {
        LogLine("[filter] replacement ABORTED: could not write the jmp");
        return false;
    }
    g_filterReplaced = true;
    LogLine("[filter] sub_435070 REPLACED (heap gather, hashed lookup, cap=%lu) stub=%p",
            (unsigned long)g_visibleCap, (void*)stub);
    return true;
}

static void RemoveFilterReplacement() {
    if (!g_filterReplaced) return;
    WriteMemory(Rva(0x35070), g_filterOriginal, sizeof(g_filterOriginal));
    g_filterReplaced = false;
}

// =============================================================================================
// /GS failure probe.
//
// The binary IS built with /GS -- 275 functions call __security_check_cookie at RVA 0x7882F:
//
//     0047882F  cmp ecx, [0x4AFCA0]     ; ecx = the cookie the epilogue read back off the frame
//     00478835  jne 0x478838
//     00478837  ret                     ; intact -> return to the function that called it
//     00478838  jmp 0x4787FE            ; corrupted -> "Buffer overrun detected!" then abort(3)
//
// Every call site is a CALL (275 of them, zero tail-jumps), so on entry [esp] is a return address
// INSIDE the function whose frame was smashed. The abort handler runs far too late to see it --
// by the time ExitProcess is reached the frame is long gone, which is why the existing [EXIT]
// stack walk only ever showed CRT frames.
//
// The fast path here is byte-for-byte what the original does: compare, and return if it matches.
// Only a mismatch takes the detour, and the process is already dying at that point, so the cost of
// logging does not matter. Diagnostic only -- it changes no behaviour.
// =============================================================================================

static void __cdecl GsFailureReport(uint32_t* espAtEntry) {
    LogLine("[GS] ===== STACK COOKIE CORRUPTED =====");
    LogLine("[GS] last filtered category=%ld published count=%lu",
            (long)(int32_t)g_gsLastCat, (unsigned long)g_gsLastCount);
    if (!espAtEntry || !CanReadPtr(espAtEntry, 4)) { LogLine("[GS] esp unreadable"); return; }
    const uint32_t ret = espAtEntry[0];
    LogLine("[GS] return address into the SMASHED function: %08lx (rva 0x%05lx)",
            (unsigned long)ret, (unsigned long)(ret - 0x400000));
    // A few slots above, so the caller chain is visible too.
    for (int i = 1; i <= 12; ++i) {
        if (!CanReadPtr(espAtEntry + i, 4)) break;
        const uint32_t v = espAtEntry[i];
        if (v > 0x401000 && v < 0x487000)
            LogLine("[GS]   stack[+%02X] = %08lx (rva 0x%05lx, code)",
                    i * 4, (unsigned long)v, (unsigned long)(v - 0x400000));
    }
    LogLine("[GS] ===================================");
}

static const unsigned char kGsCheckEntry[6] = { 0x3B, 0x0D, 0xA0, 0xFC, 0x4A, 0x00 };
static unsigned char g_gsOriginal[6] = {};
static bool g_gsProbed = false;

static bool InstallGsFailureProbe() {
    if (g_gsProbed) return true;
    void* target = Rva(0x7882F);
    if (memcmp(target, kGsCheckEntry, sizeof(kGsCheckEntry)) != 0) {
        LogLine("[GS] probe ABORTED: 0x7882F is not `cmp ecx,[0x4AFCA0]`");
        return false;
    }
    unsigned char* stub = static_cast<unsigned char*>(AllocExec(48));
    if (!stub) { LogLine("[GS] probe ABORTED: stub alloc failed"); return false; }
    unsigned char* p = stub;
    for (unsigned char b : kGsCheckEntry) EmitU8(p, b);   // cmp ecx,[0x4AFCA0]
    EmitU8(p, 0x75); EmitU8(p, 0x01);                     // jne +1 -> the report path
    EmitU8(p, 0xC3);                                      // ret    (cookie intact: fast path)
    EmitU8(p, 0x60);                                      // pushad
    EmitU8(p, 0x8D); EmitU8(p, 0x44); EmitU8(p, 0x24); EmitU8(p, 0x20);  // lea eax,[esp+0x20]
    EmitU8(p, 0x50);                                      // push eax  (esp as it was on entry)
    EmitU8(p, 0xE8);
    void* callNext = p + 4;
    EmitRel32(p, callNext, reinterpret_cast<void*>(&GsFailureReport));
    EmitU8(p, 0x83); EmitU8(p, 0xC4); EmitU8(p, 0x04);    // add esp,4
    EmitU8(p, 0x61);                                      // popad
    EmitU8(p, 0xE9);
    void* jmpNext = p + 4;
    EmitRel32(p, jmpNext, Rva(0x78838));                  // fall into the game's own failure jmp

    memcpy(g_gsOriginal, target, sizeof(g_gsOriginal));
    unsigned char patch[6];
    patch[0] = 0xE9;
    const intptr_t rel = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(target) - 5;
    memcpy(&patch[1], &rel, 4);
    patch[5] = 0x90;                                      // pad out the 6-byte cmp
    if (!WriteMemory(target, patch, sizeof(patch))) {
        LogLine("[GS] probe ABORTED: could not write the jmp");
        return false;
    }
    g_gsProbed = true;
    LogLine("[GS] failure probe armed at __security_check_cookie (0x7882F), stub=%p", (void*)stub);
    return true;
}

static void RemoveGsFailureProbe() {
    if (!g_gsProbed) return;
    WriteMemory(Rva(0x7882F), g_gsOriginal, sizeof(g_gsOriginal));
    g_gsProbed = false;
}

// =============================================================================================
// sub_412F00 -- "copy the filename stem" -- replaced, because it computes a negative length.
//
//     v3 = strrchr(Str, '\\'); v3 = v3 ? v3 + 1 : Str;   // start of the filename
//     v4 = strrchr(Str, '.');                            // the extension
//     if (!v4) v4 = Str + strlen(Str);                   // no dot at all -> whole name
//     result = v4 - v3;                                  // <-- SIGNED subtraction...
//     qmemcpy(a2, v3, result);                           // ...consumed as unsigned
//
// The author handled "no dot anywhere" but not "the only dot is in a DIRECTORY component". For
// a path like  System\SongSelect\CYT-2.2\icon  the last dot sits inside CYT-2.2, before the
// filename starts, so v4 - v3 is negative, becomes ~4 GB unsigned, and qmemcpy runs off the end
// of the caller's buffer.
//
// The caller that matters is sub_407E50, the .abm loader. Its destination is CHAR chText[260] at
// ebp-0x114 with the /GS cookie immediately after it at ebp-0x10, so the very first bytes past the
// buffer hit the cookie: "Buffer overrun detected!" and abort(3). That is precisely the crash seen
// on the CYT-2.2 category, and it only fires on the path where opening the .abm FAILED -- which is
// why it took a category whose folder name contains a dot AND a missing asset to surface it.
//
// Same signature, same __cdecl convention, so the patch is a plain 5-byte jmp: arguments stay on
// the caller's stack exactly where they were, the result comes back in eax, and the caller still
// cleans up. The clamp at 255 is belt and braces -- every caller's buffer is 260 bytes.
static unsigned long g_stemNegatives = 0;

extern "C" uint32_t __cdecl EZ2BasenameStem(const char* str, char* out) {
    if (!str || !out) return 0;
    const char* slash = strrchr(str, '\\');
    const char* begin = slash ? slash + 1 : str;
    const char* dot   = strrchr(str, '.');
    if (!dot || dot < begin) {
        // No extension on the filename itself. The original only checked for "no dot at all".
        if (dot && dot < begin && g_stemNegatives < 8) {
            ++g_stemNegatives;
            LogLine("[stem] negative length averted: '%s' (dot at %ld, name starts at %ld) -- the"
                    " original would have copied ~4GB into a 260-byte stack buffer",
                    str, (long)(dot - str), (long)(begin - str));
        }
        dot = str + strlen(str);
    }
    size_t n = (size_t)(dot - begin);
    if (n > 255) n = 255;
    memcpy(out, begin, n);
    out[n] = 0;
    return (uint32_t)n;
}

static const unsigned char kStemEntry[5] = { 0x53, 0x56, 0x57, 0x8B, 0x7C };  // push ebx/esi/edi; mov edi,...
static unsigned char g_stemOriginal[5] = {};
static bool g_stemReplaced = false;

static bool InstallStemFix() {
    if (g_stemReplaced) return true;
    void* target = Rva(0x12F00);
    if (memcmp(target, kStemEntry, sizeof(kStemEntry)) != 0) {
        LogLine("[stem] fix ABORTED: 0x12F00 does not start with the expected prologue");
        return false;
    }
    memcpy(g_stemOriginal, target, sizeof(g_stemOriginal));
    unsigned char patch[5];
    patch[0] = 0xE9;
    const intptr_t rel = reinterpret_cast<intptr_t>(&EZ2BasenameStem)
                       - reinterpret_cast<intptr_t>(target) - 5;
    memcpy(&patch[1], &rel, 4);
    if (!WriteMemory(target, patch, sizeof(patch))) {
        LogLine("[stem] fix ABORTED: could not write the jmp");
        return false;
    }
    g_stemReplaced = true;
    LogLine("[stem] sub_412F00 replaced (negative-length stem copy fixed, 5 call sites)");
    return true;
}

static void RemoveStemFix() {
    if (!g_stemReplaced) return;
    WriteMemory(Rva(0x12F00), g_stemOriginal, sizeof(g_stemOriginal));
    g_stemReplaced = false;
}

void InstallTraceHooks() {
    // The stem fix is a correctness patch for a real defect in the original, so it always goes
    // in. The /GS probe is pure diagnostics and is opt-in.
    if (Config().gsProbe) InstallGsFailureProbe();
    InstallStemFix();
    // Judge hooks fire per judgement and their stub costs pushad + a 0x214-byte frame +
    // fxsave/fxrstor on every hit -- paid even when the handler's first act is to return
    // because tracing is off. Decide here instead, so an unwanted hook is never patched in.
    if (!Config().traceJudge) {
        for (auto& h : g_hooks)
            if (h.rva == 0x26500 || h.rva == 0x26700 || h.rva == 0x268C0 || h.rva == 0x26AB0)
                h.enabled = false;
    }
    // The replacement owns 0x35070 outright, so it has to claim those 5 bytes before the
    // trace hook writes its own jmp over them -- and the trace hook must then stay away.
    if (InstallFilterReplacement()) {
        for (auto& h : g_hooks)
            if (h.rva == 0x35070) { h.enabled = false; break; }
    }
    if (g_traceInstalled) return;
    if (!Config().enableFunctionTrace) {
        LogLine("[trace] function trace disabled by config");
        return;
    }
    bool customCategory = Config().enableSongSelectInject && Config().enableBmsData && !GetCustomBmsSongs().empty();
    if (customCategory) {
        // HARD CEILING: the SongSelect object is a per-class POOL allocation (operator-new via
        // 0x4016a0), and the pool caps the block at ~2.3MB no matter how big a size we patch into
        // the `push` sites. master[] is relocated +kMasterRel into that block, so it physically holds
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
    if (customCategory && g_capacityPatched) {
        InstallVersionArrayRelocation();
        InstallVersionArrayARelocation();
        InstallVersionGuardWiden();
        InstallSortstyleRelocation();
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
    // Custom categories only work if we own the SongSelect filter entry point, because that is
    // where the inject runs. The last g_hooks entry IS 0x35070 -- but when the replacement claimed
    // that function it also stood that hook down, so "hook installed" alone is no longer the right
    // question. Either owner satisfies the requirement; testing only the hook is what silently
    // disabled every custom category the moment the replacement went in.
    const bool songSelectOwned =
        g_hooks[sizeof(g_hooks) / sizeof(g_hooks[0]) - 1].installed || g_filterReplaced;
    if (customCategory && !songSelectOwned) {
        RemoveCustomCategoryPatches();
        LogLine("[category] new category disabled: SongSelect entry point unavailable");
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
    RemoveVersionArrayRelocation();
    RemoveVersionArrayARelocation();
    RemoveVersionGuardWiden();
    RemoveSortstyleRelocation();
    RemoveFilterReplacement();
    RemoveGsFailureProbe();
    RemoveStemFix();
    g_traceInstalled = false;
}
