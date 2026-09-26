#include "config.h"
#include <windows.h>
#include <algorithm>

static ModConfig g_cfg;

static bool readBool(const char* section, const char* key, bool def, const std::string& ini) {
    char buf[16] = {};
    GetPrivateProfileStringA(section, key, def ? "1" : "0", buf, sizeof(buf), ini.c_str());
    return _stricmp(buf, "1") == 0 || _stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0 || _stricmp(buf, "on") == 0;
}

static std::string readString(const char* section, const char* key, const char* def, const std::string& ini) {
    char buf[MAX_PATH] = {};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), ini.c_str());
    return std::string(buf);
}

static int readInt(const char* section, const char* key, int def, const std::string& ini) {
    return GetPrivateProfileIntA(section, key, def, ini.c_str());
}

std::string GetExeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    return std::string(path);
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

ModConfig LoadConfig() {
    ModConfig cfg;
    std::string ini = JoinPath(GetExeDirectory(), "BMSMod.ini");
    cfg.coreEnable = readBool("Core", "Enable", true, ini);
    cfg.enableLog = readBool("Debug", "EnableLog", true, ini);
    cfg.traceGameLog = readBool("Debug", "TraceGameLog", false, ini);
    cfg.traceFileApi = readBool("Debug", "TraceFileApi", false, ini);
    cfg.traceBmsScan = readBool("Debug", "TraceBMSScan", false, ini);
    cfg.traceJudge = readBool("Debug", "TraceJudge", false, ini);
    cfg.gsProbe = readBool("Debug", "GsProbe", false, ini);
    cfg.hangWatchdog = readBool("Debug", "HangWatchdog", false, ini);
    cfg.bgaPreview = readBool("Visuals", "BgaPreview", true, ini);
    cfg.categoryOverlay = readBool("Visuals", "CategoryOverlay", true, ini);
    cfg.hudPanel = readBool("Gameplay", "HudPanel", true, ini);
    cfg.categorySort = readBool("Gameplay", "CategorySort", true, ini);
    cfg.constantSpeed = readBool("Gameplay", "ConstantSpeed", true, ini);
    cfg.customTop5 = readBool("Gameplay", "CustomTop5", true, ini);
    cfg.hudPanelX = readInt("Gameplay", "HudPanelX", 296, ini);
    cfg.hudPanelY = readInt("Gameplay", "HudPanelY", 86, ini);
    cfg.fpsLimit = readInt("Gameplay", "FpsLimit", 0, ini);
    cfg.judgeOffsetMs = readInt("Gameplay", "JudgeOffsetMs", 0, ini);
    cfg.keysoundCacheMB = readInt("CustomSongs", "KeysoundCacheMB", 0, ini);
    cfg.assetCacheMB = readInt("CustomSongs", "AssetCacheMB", 0, ini);
    cfg.levelFromNotes = readBool("CustomSongs", "LevelFromNotes", true, ini);
    cfg.detailPanels = readBool("Gameplay", "DetailPanels", true, ini);
    cfg.textureRing = readBool("Gameplay", "TextureRing", true, ini);
    cfg.previewFps = readInt("Gameplay", "PreviewFps", 20, ini);
    if (cfg.previewFps < 5) cfg.previewFps = 5;
    if (cfg.previewFps > 30) cfg.previewFps = 30;
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        const bool known = GlobalMemoryStatusEx(&ms) != 0;
        const MemoryBudget b = MemoryBudgetFor(known ? ms.ullTotalVirtual : (2ull << 30),
                                               known ? ms.ullTotalPhys : 0);
        cfg.memoryTier = b.tier;
        if (cfg.keysoundCacheMB <= 0) cfg.keysoundCacheMB = b.keysoundMB;
        if (cfg.assetCacheMB <= 0) cfg.assetCacheMB = b.assetMB;
        if (cfg.previewFps > b.previewFpsCap) cfg.previewFps = b.previewFpsCap;
    }
    if (cfg.keysoundCacheMB < 16) cfg.keysoundCacheMB = 16;
    if (cfg.assetCacheMB < 16) cfg.assetCacheMB = 16;
    cfg.artPrefetch = readBool("Gameplay", "ArtPrefetch", true, ini);
    cfg.scanCache = readBool("BMSData", "ScanCache", true, ini);
    cfg.enableFunctionTrace = readBool("Debug", "EnableFunctionTrace", true, ini);
    cfg.altEnter = readBool("Video", "AltEnter", true, ini);
    cfg.borderlessOnly = readBool("Video", "BorderlessOnly", true, ini);
    cfg.showInjectedMessageBox = readBool("Debug", "ShowInjectedMessageBox", false, ini);
    cfg.showProgressPopup = readBool("Debug", "ShowProgressPopup", true, ini);
    cfg.captureExistingWindow = readBool("Video", "CaptureExistingWindow", true, ini);
    cfg.enableBmsData = readBool("CustomSongs", "EnableBMSData", true, ini);
    cfg.bmsRoot = readString("CustomSongs", "Root", "BMSData", ini);
    cfg.assetsInMemory = readBool("CustomSongs", "AssetsInMemory", true, ini);
    cfg.cacheRoot = readString("CustomSongs", "CacheRoot", "CustomCache", ini);
    cfg.pageSize = readInt("CustomSongs", "PageSize", 400, ini);
    cfg.enableSongSelectInject = readBool("CustomSongs", "EnableSongSelectInject", true, ini);
    cfg.bridgeMode = readBool("CustomSongs", "BridgeMode", true, ini);
    cfg.modifyOriginalFiles = readBool("CustomSongs", "ModifyOriginalFiles", false, ini);
    cfg.useNativeSongBinAsBase = readBool("CustomSongs", "UseNativeSongBinAsBase", true, ini);
    cfg.useNativeStageIniAsBase = readBool("CustomSongs", "UseNativeStageIniAsBase", true, ini);
    cfg.flatSongFolder = readBool("CustomSongs", "FlatSongFolder", true, ini);
    cfg.recursiveSongAssets = readBool("CustomSongs", "RecursiveSongAssets", false, ini);
    cfg.inferAssetsFromBMS = readBool("CustomSongs", "InferAssetsFromBMS", true, ini);
    cfg.inferMetadataFromBMS = readBool("CustomSongs", "InferMetadataFromBMS", true, ini);
    cfg.generateCacheManifest = readBool("CustomSongs", "GenerateCacheManifest", true, ini);
    cfg.redirectCustomToNative = readBool("CustomSongs", "RedirectCustomToNative", false, ini);
    cfg.redirectNativeId = readString("CustomSongs", "RedirectNativeId", "babydance", ini);
    cfg.pinCustomAtTop = readBool("CustomSongs", "PinCustomAtTop", false, ini);
    cfg.forceSelectCustom = readBool("CustomSongs", "ForceSelectCustom", false, ini);

    cfg.autoGenerateModes = readBool("AutoChart", "Enable", true, ini);
    cfg.autoGenerateSourceMode = readString("AutoChart", "SourceMode", "5keyMix", ini);
    cfg.autoGenerateTargetModes = readString("AutoChart", "TargetModes",
        "7keyMix,clubMix,spaceMix,rubyMix,ez2Catch,turnTable,cv2Mix", ini);
    cfg.autoGenerateIntensity = readInt("AutoChart", "Intensity", 100, ini);
    if (cfg.autoGenerateIntensity < 0) cfg.autoGenerateIntensity = 0;
    if (cfg.autoGenerateIntensity > 200) cfg.autoGenerateIntensity = 200;
    cfg.autoGenerateSeed = readInt("AutoChart", "Seed", 1337, ini);
    cfg.autoGenerateEnrichSource = readBool("AutoChart", "EnrichSourceMode", true, ini);
    cfg.learnNativeCharts = readBool("AutoChart", "LearnNativeCharts", true, ini);
    cfg.nativeChartRoot = readString("AutoChart", "NativeChartRoot", "SOUND", ini);
    cfg.nativeModelMaxChartsPerMode = readInt("AutoChart", "NativeModelMaxChartsPerMode", 192, ini);
    if (cfg.nativeModelMaxChartsPerMode < 8) cfg.nativeModelMaxChartsPerMode = 8;
    if (cfg.nativeModelMaxChartsPerMode > 2048) cfg.nativeModelMaxChartsPerMode = 2048;
    return cfg;
}

const ModConfig& Config() { return g_cfg; }
void SetConfig(const ModConfig& cfg) { g_cfg = cfg; }
