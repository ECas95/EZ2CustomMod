#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CustomBmsChart {
    std::string file;
    std::string title;
    std::string artist;
    std::string stageFile;
    std::string banner;
    std::string bpm;
    std::string playLevel;
    std::string difficulty;
    int slot = -1;
    std::vector<std::string> wavRefs;
    std::vector<std::string> bmpRefs;
};

struct CustomBmsSong {
    std::string id;
    std::string mode;        // internal mode name, e.g. 5keyMix
    std::string modeFolder;  // folder name under BMSData, e.g. 5KeyMix
    std::string category;
    std::string songDir;
    std::string cacheDir;
    std::string title;
    std::string artist;
    std::vector<std::string> charts;
    std::vector<CustomBmsChart> chartMeta;
    std::vector<std::string> audioFiles;
    std::vector<std::string> imageFiles;
    std::vector<std::string> videoFiles;

    // Virtual song.bin-compatible difficulty/BPM fields.
    // Slots map to FNEX song.bin record blocks: NM, HD, SHD/MX, EX.
    int levelNM = 0;
    int levelHD = 0;
    int levelSHD = 0;
    int levelEX = 0;
    float bpmNM = 0.0f;
    float bpmHD = 0.0f;
    float bpmSHD = 0.0f;
    float bpmEX = 0.0f;
};

void ScanBMSData();
const std::vector<CustomBmsSong>& GetCustomBmsSongs();
std::vector<CustomBmsSong> GetCustomBmsSongsForMode(const std::string& mode);
std::vector<CustomBmsSong> GetCustomBmsSongsForActiveMode();
std::vector<std::string> GetCustomBmsCategoriesForMode(const std::string& mode);
bool IsCustomSongId(const char* id);
const CustomBmsSong* FindCustomSongById(const char* id);
const CustomBmsSong* FindCustomSongByCacheDir(const std::string& cacheDir);
std::string GetCustomSongBackingId(const char* id);

// On-demand cache: scan writes only metadata; these decode the heavy audio at play/hover.
void EnsureSongKeysounds(const CustomBmsSong& song); // first sample miss = song is played
void EnsureSongPreview(const CustomBmsSong& song);   // SongSelect hover preview

// Direct bridge: build the selected BMS chart in memory and feed its PCM WAVs to
// the game's native chart/audio objects. The selected BGA video remains the
// original BMSData file; the renderer bridge consumes these getters directly.
bool LoadCustomBmsDirect(void* songPackage, const char* request);
void SetDirectBmsEnabled(bool enabled);
bool DirectBmsEnabled();
void ResetDirectBmsVideo();
const std::string& DirectBmsVideoPath();
uint32_t DirectBmsVideoStartMs();
bool StartDirectBmsFfmpegVideo(const std::string& path);
bool ReadDirectBmsFfmpegFrame(void* buffer, uint32_t bytes);
void StopDirectBmsFfmpegVideo();

// Native BMP/ABM assets can be served straight from BMSData. Other image formats
// deliberately keep the game's safe fallback; global renderer hooks were unstable.
std::string ResolveDirectBmsVisual(const CustomBmsSong& song, const std::string& request);
std::string EnsureSongVisualAsset(const CustomBmsSong& song, const std::string& request);

void SetActiveMode(const std::string& mode, const char* reason);
void DetectActiveModeFromPath(const char* path, const char* reason);
const std::string& GetActiveMode();
std::string NormalizeModeName(const std::string& raw);
bool IsKnownModeFolder(const std::string& raw);
std::string GetVirtualStageIniPathForMode(const std::string& mode);
