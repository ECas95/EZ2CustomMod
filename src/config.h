#pragma once
#include <string>

struct ModConfig {
    bool coreEnable = true;
    bool enableLog = true;
    bool traceGameLog = false;
    bool traceFileApi = false;
    bool traceBmsScan = false;
    bool traceJudge = false;
    // Diagnostics that cost something on the target machine, so both default to OFF.
    // gsProbe sits on the return path of 275 functions; hangWatchdog suspends and resumes
    // every thread in the process every two seconds, which on a single-core cabinet is a
    // stutter you can feel.
    bool gsProbe = false;
    bool hangWatchdog = false;
    // Loops a few seconds of a song's BGA video in the song-select disc while the cursor rests
    // on it. Costs one 256x256 surface and a ~12MB frame buffer, both reused for every song.
    bool bgaPreview = true;
    // Per-category bg.png / ico.png drawn over the song-select background.
    bool categoryOverlay = true;
    bool hudPanel = true;          // the mod's own notices in song select (sort, jumps, speed)
    bool categorySort = true;      // EFX4 cycles the sort of the current custom category
    bool constantSpeed = true;     // keep the scroll speed in BPM terms across songs
    bool customTop5 = true;        // show the game's own Top-5 files for custom songs
    int  hudPanelX = 296, hudPanelY = 86;    // where the 320x72 notice lands on the 640x480 screen
    int  fpsLimit = 0;             // hold frames to 1/N s at the frame end; 0 = the display paces
    int  judgeOffsetMs = 0;        // +N: presses count N ms earlier (for a late-feeling setup)
    // The two caches that dominate our share of the address space. They were fixed at these sizes,
    // and a session that browsed a lot reached 1470 MB committed across 10896 regions -- fine while
    // the EXE is large-address aware on a 64-bit host, but XP 32-bit stops at 2 GB without /3GB,
    // and one disc render had already failed for want of a contiguous block.
    // 0 = sized from the machine (MemoryBudgetFor below); any other value is taken as given.
    int  keysoundCacheMB = 0;      // decoded keysound PCM held for the song under the cursor
    int  assetCacheMB = 0;         // generated art and previews, LRU
    const char* memoryTier = "";   // which MemoryBudgetFor tier the auto sizes came from (for the log)
    // Rate charts from their own note data instead of trusting #PLAYLEVEL. The declared level
    // stops discriminating at the top: 46% of songs call their hardest chart 18, and inside that
    // one level the peak density runs from 13 to 83 notes a second.
    bool levelFromNotes = true;
    bool detailPanels = true;      // the song-select stats block and density graph
    bool textureRing = true;       // custom art reuses a fixed set of textures instead of one per song
    bool englishTextures = true;   // Translation\system\... replaces the Korean UI textures when present
    bool virtualCategories = true; // FAVORITES / RECENT / MOST PLAYED at the head of the custom categories
    // Frames a second for the BGA preview on the disc in song select. The clip is decoded whole
    // into memory before it plays, so this is a straight trade: 256x256x3 is 192 KB a frame, and
    // the buffer holds PreviewFps x 12 seconds of them (10 fps = 23 MB, 20 fps = 46 MB).
    int  previewFps = 20;
    bool artPrefetch = true;       // render a category's wheel art on a background thread
    // Reuse the previous run's BMS parse for folders that have not changed.
    bool scanCache = true;
    bool enableFunctionTrace = true;
    bool altEnter = true;
    bool borderlessOnly = true;
    bool enableBmsData = true;
    bool showInjectedMessageBox = false;
    bool showProgressPopup = true;
    bool captureExistingWindow = true;
    std::string bmsRoot = "BMSData";
    std::string cacheRoot = "CustomCache";
    // Generated art and previews live in RAM (an LRU, regenerated when evicted) and the disk is
    // only scratch while one is rendered. 0 keeps them as files under cacheRoot across sessions.
    bool assetsInMemory = true;
    int pageSize = 400;
    bool enableSongSelectInject = true;
    bool bridgeMode = true;
    bool modifyOriginalFiles = false;
    bool useNativeSongBinAsBase = true;
    bool useNativeStageIniAsBase = true;
    bool flatSongFolder = true;
    bool recursiveSongAssets = false;
    bool inferAssetsFromBMS = true;
    bool inferMetadataFromBMS = true;
    bool generateCacheManifest = true;
    bool redirectCustomToNative = false;
    std::string redirectNativeId = "babydance";
    bool pinCustomAtTop = false;
    bool forceSelectCustom = false;

    // Procedural mode conversion. Source charts remain untouched; virtual copies are
    // exposed in the requested EZ2AC modes and redistributed at load time.
    bool autoGenerateModes = true;
    // Source = the mode our customs actually live in (the mod injects BMSData\5KeyMix); there is no
    // 5keyOnly folder on real installs. Generating a mode from itself is skipped in ExpandAutoGeneratedModes.
    std::string autoGenerateSourceMode = "5keyMix";
    std::string autoGenerateTargetModes = "7keyMix,clubMix,spaceMix,rubyMix,ez2Catch,turnTable,cv2Mix";
    int autoGenerateIntensity = 100;
    int autoGenerateSeed = 1337;
    // The source mode's songs are the author's own charts, so they are never redistributed. But a
    // raw 5-key BMS has no scratch or pedal, while 5-key standard really has both -- enrich adds
    // only those two controls and leaves the authored lane pattern untouched. 0 = play them raw.
    bool autoGenerateEnrichSource = true;

    // Statistical model learned from native FNEX .es/.ez charts under SOUND.
    bool learnNativeCharts = true;
    std::string nativeChartRoot = "SOUND";
    int nativeModelMaxChartsPerMode = 192;
};

// Automatic cache sizes. What limits us is the process's address space before physical memory:
// 2 GB on XP 32-bit (3 GB with /3GB), 4 GB for this large-address-aware EXE on a 64-bit host. A
// session that browsed a lot reached ~1.5 GB committed with 384 + 256, which leaves nothing on a
// 2 GB process, so the smaller tiers cut the caches and the preview clip (the clip is decoded
// whole: 192 KB a frame for 12 s) to fit. A small physical memory takes the tier below too.
struct MemoryBudget { int keysoundMB; int assetMB; int previewFpsCap; const char* tier; };
inline MemoryBudget MemoryBudgetFor(unsigned long long totalVirtual, unsigned long long totalPhys) {
    const unsigned long long mb = 1ull << 20;
    if (totalVirtual <= 2200 * mb || (totalPhys && totalPhys < 1536 * mb)) return {128, 96, 10, "2GB"};
    if (totalVirtual <= 3200 * mb || (totalPhys && totalPhys < 3072 * mb)) return {256, 160, 20, "3GB"};
    return {384, 256, 30, "4GB"};
}

ModConfig LoadConfig();
const ModConfig& Config();
void SetConfig(const ModConfig& cfg);
std::string GetExeDirectory();
std::string JoinPath(const std::string& a, const std::string& b);
