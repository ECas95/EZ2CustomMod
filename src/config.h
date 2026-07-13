#pragma once
#include <string>

struct ModConfig {
    bool coreEnable = true;
    bool enableLog = true;
    bool traceGameLog = false;
    bool traceFileApi = false;
    bool traceBmsScan = false;
    bool traceJudge = false;
    bool enableFunctionTrace = true;
    bool altEnter = true;
    bool borderlessOnly = true;
    bool enableBmsData = true;
    bool showInjectedMessageBox = false;
    bool showProgressPopup = true;
    bool captureExistingWindow = true;
    std::string bmsRoot = "BMSData";
    std::string cacheRoot = "CustomCache";
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
};

ModConfig LoadConfig();
const ModConfig& Config();
void SetConfig(const ModConfig& cfg);
std::string GetExeDirectory();
std::string JoinPath(const std::string& a, const std::string& b);
