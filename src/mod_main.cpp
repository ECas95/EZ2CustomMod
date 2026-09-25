#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <map>
#include <cstdarg>
#include "config.h"
#include "logger.h"
#include "patch.h"
#include "iat.h"
#include "window_hook.h"
#include "bms_scanner.h"
#include "trace_hooks.h"
#include "progress_popup.h"
#include <string>
#include <unordered_map>
#include <cctype>
#include <algorithm>

static JumpPatch g_gameLogPatch;
static void* g_fsopenGateway = nullptr;
static JumpPatch g_directBmsPatch;
static void* g_directBmsGateway = nullptr;
static JumpPatch g_directBackgroundPatch;
static void* g_directBackgroundGateway = nullptr;
static JumpPatch g_directVideoFramePatch;
static void* g_directVideoFrameGateway = nullptr;
using GameFsOpen_t = void* (__cdecl*)(const char*, const char*);
static GameFsOpen_t RealGameFsOpen = nullptr;
using SongPackage_t = int (__thiscall*)(void*, const char*);
static SongPackage_t RealSongPackage = nullptr;
using BackgroundLoad_t = int (__thiscall*)(void*, const char*);
using VideoFrame_t = void (__thiscall*)(void*);
static BackgroundLoad_t RealBackgroundLoad = nullptr;
static VideoFrame_t RealVideoFrame = nullptr;

// The SongSelect object is a ~0x210000 VirtualAlloc'd region ([grow] showed allocBase==ecx,
// regionSize=0x210000) → master[] is capped ~1084 entries. Grow it: when the game reserves a block
// in the object-size window, inflate the request to 8MB so master[] (relocated +0x14E0B0) fits
// thousands. Logged so we can confirm which alloc became the object (ret == the [grow] ecx).
using VirtualAlloc_t = LPVOID (WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
static VirtualAlloc_t RealVirtualAlloc = nullptr;
// Allocation histogram.
//
// The per-call log only fires at 1 MB and up, so the 41,800 committed regions that appeared while
// browsing -- 2.5 GB of them -- were completely invisible: they are all smaller than that. Counting
// by size bucket costs two increments per call and says which band the growth is in, which is the
// difference between fixing the right allocator and guessing at one.
static volatile LONG g_allocCount[8] = {};
static volatile LONG g_allocMB[8] = {};
static const char* const kAllocBucketName[8] = {
    "<4KB", "4-64KB", "64-256KB", "256KB-1MB", "1-4MB", "4-16MB", "16-64MB", ">=64MB"
};

static int AllocBucket(SIZE_T size) {
    if (size < 4u * 1024u) return 0;
    if (size < 64u * 1024u) return 1;
    if (size < 256u * 1024u) return 2;
    if (size < 1024u * 1024u) return 3;
    if (size < 4u * 1024u * 1024u) return 4;
    if (size < 16u * 1024u * 1024u) return 5;
    if (size < 64u * 1024u * 1024u) return 6;
    return 7;
}

void ReportAllocHistogram(const char* why) {
    for (int i = 0; i < 8; ++i) {
        const long n = InterlockedCompareExchange(&g_allocCount[i], 0, 0);
        if (!n) continue;
        LogLine("[valloc] %s: %-10s count=%ld total=%ldMB", why, kAllocBucketName[i], n,
                (long)InterlockedCompareExchange(&g_allocMB[i], 0, 0));
    }
}

static LPVOID WINAPI HookVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    // The object alloc is VirtualAlloc(NULL, ~0x20f2cc, MEM_COMMIT) — addr==NULL implies reserve,
    // so don't require the MEM_RESERVE flag (the game passes MEM_COMMIT alone).
    bool inflate = addr == nullptr && size >= 0x200000 && size <= 0x220000;
    // Size it from the patched capacity so the buffer follows the library. This was a fixed 16MB,
    // which held 14234 entries -- and the capacity patch was free to ask for more than that, with
    // the shortfall coming out as songs silently missing from the end of the list.
    SIZE_T want = size;
    if (inflate) {
        want = RequiredObjectBytes();
        if (want < (SIZE_T)(16u*1024u*1024u)) want = (SIZE_T)(16u*1024u*1024u);  // floor
    }
    LPVOID r = RealVirtualAlloc(addr, want, type, prot);
    if (r && (type & MEM_COMMIT)) {
        const int b = AllocBucket(size);
        InterlockedIncrement(&g_allocCount[b]);
        InterlockedExchangeAdd(&g_allocMB[b], (LONG)(size / 1048576));
    }
    if (size >= 0x100000) {
        char grew[48] = "";
        if (inflate) _snprintf(grew, sizeof(grew) - 1, " ->%luMB", (unsigned long)(want / 1048576));
        LogLine("[valloc] req=0x%lx%s type=0x%lx -> %p", (unsigned long)size, grew,
                (unsigned long)type, r);
    }
    return r;
}

// The category-change "crash" is NOT a CPU exception (no VEH/SEH fires) yet our DLL detach still
// runs → the game calls ExitProcess on a DETECTED error (e.g. a CRT abort / its own error handler).
// Hook the exit APIs and dump the caller chain so we see WHAT decided to quit.
static void LogStackWalkFrom(void* framePtr, const char* tag);
using ExitProcess_t = VOID (WINAPI*)(UINT);
using TerminateProcess_t = BOOL (WINAPI*)(HANDLE, UINT);
static ExitProcess_t RealExitProcess = nullptr;
static TerminateProcess_t RealTerminateProcess = nullptr;
static VOID WINAPI HookExitProcess(UINT code) {
    LogLine("[EXIT] ExitProcess(code=%u) — caller chain:", code);
    // Flush the execution tracer's tail (abort(3) exits via ExitProcess, not an exception, so the
    // tracer's fault-flush never runs and the last buffer — the abort path — would be lost).
    if (HMODULE t = GetModuleHandleA("ez2tracer.dll")) {
        if (auto f = (void(*)())GetProcAddress(t, "Ez2TraceFlush")) f();
    }
    LogStackWalkFrom(__builtin_frame_address(0), "EXIT");
    // Terminate via TerminateProcess (real kernel32 from our DLL's import) — never re-enter the
    // inline-patched ExitProcess. (void)RealExitProcess keeps the IAT-hook var referenced.
    (void)RealExitProcess;
    TerminateProcess(GetCurrentProcess(), code);
}
static JumpPatch g_exitProcessInline; // inline hook on kernel32!ExitProcess (catches CRT/all callers)
static BOOL WINAPI HookTerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || h == (HANDLE)-1) {
        LogLine("[EXIT] TerminateProcess(self, code=%u) — caller chain:", code);
        LogStackWalkFrom(__builtin_frame_address(0), "EXIT");
    }
    return RealTerminateProcess ? RealTerminateProcess(h, code) : TerminateProcess(h, code);
}

using CreateFileA_t = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using FindFirstFileA_t = HANDLE (WINAPI*)(LPCSTR,LPWIN32_FIND_DATAA);
using FindNextFileA_t = BOOL (WINAPI*)(HANDLE,LPWIN32_FIND_DATAA);
using SetCurrentDirectoryA_t = BOOL (WINAPI*)(LPCSTR);
using GetFileAttributesA_t = DWORD (WINAPI*)(LPCSTR);
static CreateFileA_t RealCreateFileA = nullptr;
static FindFirstFileA_t RealFindFirstFileA = nullptr;
static FindNextFileA_t RealFindNextFileA = nullptr;
static SetCurrentDirectoryA_t RealSetCurrentDirectoryA = nullptr;
static std::string g_lastCustomCacheDir;


extern "C" __declspec(dllexport) void hook_init(void) {}

static void WriteMarkerFile(const char* phase) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = '\0';
    lstrcatA(exe, "EZ2CustomMod_injected.marker.txt");
    HANDLE h = CreateFileA(exe, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st{}; GetLocalTime(&st);
        char line[512] = {};
        wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d phase=%s pid=%lu base=%p\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  phase ? phase : "?", GetCurrentProcessId(), GetModuleHandleA(nullptr));
        DWORD written = 0;
        WriteFile(h, line, lstrlenA(line), &written, nullptr);
        CloseHandle(h);
    }
}

static bool CanReadGamePtr(const void* ptr, size_t bytes = 1) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    DWORD p = mbi.Protect & 0xff;
    if (p == PAGE_NOACCESS || p == PAGE_EXECUTE) return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return a + bytes <= base + mbi.RegionSize;
}

static bool CanWriteGamePtr(const void* ptr, size_t bytes = 1) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
        return false;
    DWORD p = mbi.Protect & 0xff;
    if (p != PAGE_READWRITE && p != PAGE_WRITECOPY &&
        p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return a + bytes <= base + mbi.RegionSize;
}

static bool LooksLikeFormatString(const char* fmt) {
    if (!CanReadGamePtr(fmt, 1)) return false;
    int printable = 0;
    for (int i = 0; i < 8; ++i) {
        if (!CanReadGamePtr(fmt + i, 1)) break;
        unsigned char c = static_cast<unsigned char>(fmt[i]);
        if (c == 0) break;
        if (c >= 0x20 && c <= 0x7e) printable++;
    }
    return printable >= 2;
}

static bool IsSoundErrorFormat(const char* fmt) {
    return fmt && (strstr(fmt, "LoadSample") || strstr(fmt, "OpenSample") ||
                   strstr(fmt, "CreateSample") || strstr(fmt, "LockSample"));
}

static void __cdecl GameLogHook(const char* fmt, ...) {
    if (!LooksLikeFormatString(fmt)) return; // many destructor/unwind calls use FUN_00402880 without a valid format arg
    if (!Config().traceGameLog && !IsSoundErrorFormat(fmt)) return;
    va_list ap;
    va_start(ap, fmt);
    LogV("[game] ", fmt, ap);
    va_end(ap);
}


static bool HasSlashOrDrive(const std::string& p) {
    return p.find('\\') != std::string::npos || p.find('/') != std::string::npos || p.find(':') != std::string::npos;
}

static bool HasSongFileExtension(const std::string& p) {
    std::string l = p;
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const char* exts[] = {".ez", ".ezi", ".ezz", ".es", ".ini", ".int"};
    for (const char* e : exts) {
        size_t n = strlen(e);
        if (l.size() >= n && l.compare(l.size() - n, n, e) == 0) return true;
    }
    return false;
}

static bool HasSampleFileExtension(const std::string& p) {
    std::string l = p;
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const char* exts[] = {".wav", ".ogg", ".mp3", ".flac", ".ssf", ".ezw", ".wma"};
    for (const char* e : exts) {
        size_t n = strlen(e);
        if (l.size() >= n && l.compare(l.size() - n, n, e) == 0) return true;
    }
    return false;
}

static bool LocalFileExists(const std::string& p) {
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool EndsWithCaseInsensitive(const std::string& p, const char* suffix) {
    std::string a = p;
    std::string b = suffix ? suffix : "";
    for (char& c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

static bool ModeLooksWritable(const char* mode) {
    if (!mode) return false;
    for (const char* p = mode; *p; ++p) {
        if (*p == 'w' || *p == 'a') return true;
    }
    return false;
}


static bool TokenPrintableAscii(const char* a, const char* b) {
    if (!a || !b || b <= a) return false;
    bool any = false;
    for (const char* q = a; q < b; ++q) {
        unsigned char c = static_cast<unsigned char>(*q);
        if (c < 0x21 || c > 0x7e) return false;
        any = true;
    }
    return any;
}

static bool IsPlaintextEziMapFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    int checked = 0;
    bool found = false;
    while (fgets(line, sizeof(line), f) && checked < 32) {
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p == 0 || *p == ';' || *p == '#') continue;
        ++checked;

        // Accept old diagnostic/source format: sample_id sample_type sample.ssf
        char* end1 = nullptr;
        long sampleId = strtol(p, &end1, 10);
        if (end1 != p) {
            while (*end1 == ' ' || *end1 == '\t') ++end1;
            char* end2 = nullptr;
            long sampleType = strtol(end1, &end2, 10);
            if (end2 != end1) {
                while (*end2 == ' ' || *end2 == '\t') ++end2;
                char* nameEnd = end2;
                while (*nameEnd && *nameEnd != '\r' && *nameEnd != '\n') ++nameEnd;
                if (sampleId >= 0 && sampleId <= 0x7ff && sampleType >= 0 && sampleType <= 9 && TokenPrintableAscii(end2, nameEnd)) {
                    found = true;
                    break;
                }
            }
        }

        // Accept FNEX LoadEZI plaintext .ezz format: sample_id sample_type sample.ssf
        char* nameStart = p;
        char* nameEnd = p;
        while (*nameEnd && *nameEnd != ' ' && *nameEnd != '\t' && *nameEnd != '\r' && *nameEnd != '\n') ++nameEnd;
        if (!TokenPrintableAscii(nameStart, nameEnd)) { fclose(f); return false; }
        char* idStart = nameEnd;
        while (*idStart == ' ' || *idStart == '\t') ++idStart;
        char* idEnd = nullptr;
        long id2 = strtol(idStart, &idEnd, 10);
        if (idEnd != idStart && id2 >= 0 && id2 <= 0x7ff) {
            found = true;
            break;
        }

        fclose(f);
        return false;
    }
    fclose(f);
    return found;
}


static std::string SoundDirForNativeBacking() {
    return JoinPath(JoinPath(GetExeDirectory(), "SOUND"), Config().redirectNativeId);
}

static std::string FileNameOnly(std::string p) {
    size_t a = p.find_last_of("\\/");
    if (a != std::string::npos) return p.substr(a + 1);
    return p;
}
static std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

static bool PathContainsLower(const std::string& p, const char* part) {
    return LowerAscii(p).find(LowerAscii(part ? part : "")) != std::string::npos;
}

// Difficulty-independent art (disc/discsmall/eyecatch/resultdisc) is now generated ONCE as
// <id>.abm; map the per-difficulty requests <id>-hd/-ez/-shd/-ex.abm onto that single base file.
static std::string StripVisualDiffSuffix(const std::string& fn) {
    size_t dot = fn.rfind('.');
    std::string stem = (dot == std::string::npos) ? fn : fn.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? std::string() : fn.substr(dot);
    for (const char* suf : {"-shd", "-hd", "-ex", "-ez"}) {
        size_t n = strlen(suf);
        if (stem.size() >= n && stem.compare(stem.size() - n, n, suf) == 0)
            return stem.substr(0, stem.size() - n) + ext;
    }
    return fn;
}

// Lazy-disc gate. The >~2300 abort root is the surface POOL filling (inject songnames + disc loads),
// which makes SongSelect::OnCreateGame's Credits_mask.bmp load fail → _purecall. Skipping custom discs
// here removes ~1900 disc-surface attempts so the pool keeps headroom for the UI. false = skip (blank
// custom discs, draw-guard renders them safely). Re-enable per-song on-demand once stable.
bool g_discLazyAllowSelected = false;
static std::string VisualCacheRedirectForPath(const CustomBmsSong& s, const std::string& originalPath) {
    if (s.cacheDir.empty()) return std::string();
    std::string fn = FileNameOnly(originalPath);
    std::string vfn = StripVisualDiffSuffix(fn); // disc/eyecatch/... share one base file
    // Always return the canonical subfolder path — do NOT gate on LocalFileExists.
    // The file is generated during ScanBMSData (before CreateWindowExA unblocks the game),
    // so it exists by the time any image load fires. Gating caused the fallback generic path
    // (cacheDir\filename.bmp) to be used instead of the correct subfolder, showing a random image.
    // Big custom art is served again: the old "lazyart" skip existed because the texture pools
    // overflowed at ~4k textures — that was the 1MB texture-OBJECT pool (EXE-patched to 16MB).
    // Past that budget the insert/draw guards degrade to blank art, no crash. Each class maps to its cache subfolder (generated by GenerateVisualAssetCache);
    // -hd/-ez/... suffixes already stripped into vfn (one base file each).
    // Return the canonical path, but BUILD the asset first if it is not there.
    //
    // This used to hand back the path and trust that the scan had produced the file. That held
    // for the roles the game asks for by name; discsmall is not one of them. The wheel reads its
    // small-disc texture straight out of the entry, so nothing ever requested the file, nothing
    // generated it, and the slot kept whatever a native song had left in it -- a stray disc on
    // every change-song animation. EnsureSongVisualAsset is a no-op when the file already
    // exists, so the common path is one stat.
    auto serve = [&](const char* role) {
        std::string path = JoinPath(JoinPath(s.cacheDir, role), vfn);
        if (LocalFileExists(path)) return path;
        std::string built = EnsureSongVisualAsset(s, originalPath);
        return built.empty() ? path : built;
    };
    if (PathContainsLower(originalPath, "system\\disc\\")) return serve("disc");
    if (PathContainsLower(originalPath, "system\\discsmall\\")) return serve("discsmall");
    if (PathContainsLower(originalPath, "system\\eyecatch\\")) return serve("eyecatch");
    if (PathContainsLower(originalPath, "resultdisc")) return serve("resultdisc");
    if (PathContainsLower(originalPath, "system\\totalresult\\") ||
        PathContainsLower(originalPath, "system\\result")) {
        // no cache asset is generated for these result-screen images → blank (draw guard handles it)
        return JoinPath(s.cacheDir, "__lazyart_skip");
    }
    if (PathContainsLower(originalPath, "songname")) {
        return JoinPath(JoinPath(s.cacheDir, "songname"), fn);
    }
    return std::string();
}




// ---- finding which custom song a path belongs to -------------------------------------------------
//
// Every CreateFileA the game makes passes through MaybeRedirectCustomPath, and that used to ask,
// for each of the catalogue's songs, whether its id appeared anywhere in the path. With the
// auto-generated modes expanded the catalogue is 179,424 entries, so one file open cost 179,424
// substring searches -- and song select opens hundreds of files as it comes up. That is where the
// startup minutes went, and why it got worse as the library grew and slower again on the way out
// of a song.
//
// Custom ids are written by exactly one format, "bms%06lu", so the path can be asked which ids it
// contains instead: scan it once for "bms" followed by six digits, and look those up. One pass over
// an eighty-character path plus a hash lookup, in place of a scan of the whole catalogue.
static std::unordered_map<std::string, size_t> g_songByIdIndex;
static size_t g_songByIdStamp = (size_t)-1;

static void EnsureSongIdIndex(const std::vector<CustomBmsSong>& songs) {
    if (g_songByIdStamp == songs.size()) return;
    g_songByIdIndex.clear();
    g_songByIdIndex.reserve(songs.size() * 2);
    for (size_t i = 0; i < songs.size(); ++i) {
        std::string k = songs[i].id;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        g_songByIdIndex.emplace(k, i);          // first wins, matching the old scan order
    }
    g_songByIdStamp = songs.size();
}

// Catalogue indices for every custom id the path mentions, in catalogue order -- the order the old
// loop visited them in, so the first match is still the same song.
static std::vector<size_t> CustomSongsMentionedIn(const std::string& path,
                                                  const std::vector<CustomBmsSong>& songs) {
    std::vector<size_t> out;
    EnsureSongIdIndex(songs);
    std::string lower = LowerAscii(path);
    for (size_t i = 0; i + 9 <= lower.size(); ++i) {
        if (lower.compare(i, 3, "bms") != 0) continue;
        bool digits = true;
        for (size_t d = 3; d < 9; ++d)
            if (!isdigit((unsigned char)lower[i + d])) { digits = false; break; }
        if (!digits) continue;
        std::unordered_map<std::string, size_t>::const_iterator it =
            g_songByIdIndex.find(lower.substr(i, 9));
        if (it != g_songByIdIndex.end()) out.push_back(it->second);
        i += 8;                                  // ids cannot overlap
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static std::string MaybeRedirectCustomDirectory(LPCSTR name) {
    if (!name) return std::string();
    std::string p = name;
    const auto& songs = GetCustomBmsSongs();
    const std::vector<size_t> mentioned = CustomSongsMentionedIn(p, songs);
    for (size_t mi = 0; mi < mentioned.size(); ++mi) {
        const auto& s = songs[mentioned[mi]];
        size_t pos = p.find(s.id);
        if (pos == std::string::npos) continue;  // matched case-folded; this exact path did not

        if (Config().redirectCustomToNative && !Config().bridgeMode) {
            std::string out = p;
            out.replace(pos, s.id.size(), Config().redirectNativeId);
            LogLine("[custom] redirect cwd native '%s' -> '%s'", p.c_str(), out.c_str());
            return out;
        }

        if (!DirectBmsEnabled() && !s.cacheDir.empty()) {
            g_lastCustomCacheDir = s.cacheDir;
            LogLine("[custom] redirect cwd cache '%s' -> '%s'", p.c_str(), s.cacheDir.c_str());
            return s.cacheDir;
        }
    }
    return std::string();
}

// Legacy random BGA pool: DirectShow opens SPV through quartz.dll, whose IAT is redirected below.
// This path is disabled entirely when the native direct-BGA profile is active.

// Bytes an in-RAM redirect hands back through MaybeRedirectCustomPath: the marker "\x01ram" as the
// path, the content here, turned into a virtual handle by the CreateFileA hook.
static thread_local std::vector<unsigned char> g_pendingVirtualBytes;
static thread_local bool g_redirectForCrt = false;   // the caller is fopen: no memory handles

// The shipped one-second silent preview, and the reason a custom preview request must never come
// back empty. On a failed load the game replaces m_sndPreview with a sample created FROM 0Blank
// (songselectinput.cpp), and createSample sizes the pool buffer from the file it is given -- so
// the preview player is left with a one-second buffer that no real preview fits into, and every
// song from then on, custom or native, plays silence. Handing back the blank keeps the load
// successful: that one song is quiet, the next one is not.
static const std::string& BlankPreviewFile() {
    static const std::string path = JoinPath(GetExeDirectory(), "System\\Preview\\0Blank.ssf");
    return path;
}

// Is `path` a file inside `dir`? Case-folded, and the character after the prefix has to be a
// separator so that a sibling directory with a longer name ("CustomCacheOld") is not swallowed.
// `dir` is expected already lower-cased.
static bool PathIsUnderDirectory(const std::string& path, const std::string& dir) {
    if (dir.empty() || path.size() <= dir.size() + 1) return false;
    if (LowerAscii(path).compare(0, dir.size(), dir) != 0) return false;
    const char sep = path[dir.size()];
    return sep == '\\' || sep == '/';
}

static std::string MaybeRedirectCustomPath(LPCSTR name) {
    if (!name) return std::string();
    std::string p = name;

    // A path already inside our cache is already resolved -- hand it straight through.
    //
    // This is what silenced every custom preview. The game opens a preview with the CRT
    // (openSample -> fopen -> the _fsopen wrapper we hook), we answer with the real
    // CustomCache path, and then the CRT opens THAT path with CreateFileA -- which lands back
    // in our own CreateFileA hook. Second time round the in-RAM branch matched the same .ssf
    // and answered with a virtual handle, and a virtual handle is a CreateEventA handle that
    // only this DLL knows how to read: the CRT cannot build a FILE* out of it, so _fsopen
    // returned NULL, the game logged "Open Error : OpenSample" and fell back to the silent
    // System\Preview\0Blank. 656 of those in one session, one per hover, and the fallback also
    // leaks a pool sample each time.
    //
    // The guard is at the top because re-resolving an already-resolved path is never right for
    // any asset, not just the preview.
    {
        static const std::string cacheRoot = LowerAscii(JoinPath(GetExeDirectory(), Config().cacheRoot));
        if (PathIsUnderDirectory(p, cacheRoot)) return std::string();
    }

    // v24 crash fix: DO NOT redirect system\<mode>\Stage.ini to a synthetic file.
    // The v23/v23.1 file-level Stage.ini redirect made the game enter SongSelect with
    // a reduced synthetic category table and caused a crash immediately after selecting
    // 5KeyMix. Keep the generated virtual_Stage.ini as a diagnostic artifact only;
    // category injection remains memory-only in FUN_00435070 until the exact native
    // Stage.ini category row format is fully replicated.
    {
        std::string lp = LowerAscii(p);
        if (lp.find("stage.ini") != std::string::npos && lp.find("system\\") != std::string::npos) {
            LogLine("[category] Stage.ini native path preserved crashfix '%s'", p.c_str());
        }
    }

    const auto& songs = GetCustomBmsSongs();

    // Legacy no-BGA pool timing. It is shared, never per-song.
    if (!DirectBmsEnabled() && PathContainsLower(p, "\\video\\") && PathContainsLower(p, ".ini")) {
        std::string lpv = LowerAscii(p);
        size_t vp = lpv.find("\\video\\");
        size_t idStart = vp + 7, slash = lpv.find('\\', idStart);
        if (slash != std::string::npos) {
            std::string id = p.substr(idStart, slash - idStart);
            std::string idl = LowerAscii(id);
            EnsureSongIdIndex(songs);
            std::unordered_map<std::string, size_t>::const_iterator hit = g_songByIdIndex.find(idl);
            if (hit != g_songByIdIndex.end() && songs[hit->second].videoFiles.empty()) {
                std::string def = JoinPath(JoinPath(JoinPath(GetExeDirectory(), "Video"), "_spv"), "default.ini");
                if (LocalFileExists(def)) return def;
            }
        }
    }

    const std::vector<size_t> mentioned = CustomSongsMentionedIn(p, songs);
    for (size_t mi = 0; mi < mentioned.size(); ++mi) {
        const auto& s = songs[mentioned[mi]];
        size_t pos = p.find(s.id);
        if (pos == std::string::npos) continue;  // matched case-folded; this exact path did not

        if (Config().redirectCustomToNative && !Config().bridgeMode) {
            std::string out = p;
            out.replace(pos, s.id.size(), Config().redirectNativeId);
            if (!HasSlashOrDrive(out) && HasSongFileExtension(out)) {
                out = JoinPath(SoundDirForNativeBacking(), out);
            }
            LogLine("[custom] redirect file native '%s' -> '%s'", p.c_str(), out.c_str());
            return out;
        }

        // Stable subset from BMSBridge v0.2.9: serve compatible BMP/ABM art directly.
        std::string directVisual = ResolveDirectBmsVisual(s, p);
        if (!directVisual.empty()) {
            LogLine("[direct] visual '%s' -> '%s'", p.c_str(), directVisual.c_str());
            return directVisual;
        }

        if (DirectBmsEnabled()) {
            std::string fn = FileNameOnly(p);
            if (Config().assetsInMemory) {
                // The preview is opened through the CRT (openSample -> fopen), which needs a file.
                if (g_redirectForCrt && !s.cacheDir.empty() && fn == s.id + ".ssf") {
                    const std::string file = SongPreviewFileForCrt(s);
                    // Only a failure is worth a line: this runs on every hover, and a preview that
                    // works was logging four of them per song.
                    if (file.empty()) {
                        LogLine("[custom] preview id='%s' has none; playing the blank", s.id.c_str());
                        return BlankPreviewFile();
                    }
                    return file;
                }
                // The preview is never served from RAM, whoever is asking: the only thing that
                // ever reads it is the CRT-based sample loader, which needs a real file.
                if (!s.cacheDir.empty() && fn == s.id + ".ssf") {
                    const std::string file = SongPreviewFileForCrt(s);
                    return file.empty() ? BlankPreviewFile() : file;
                }
                // Art from the RAM store: the caller turns the bytes into a handle.
                std::vector<unsigned char> bytes;
                if (!g_redirectForCrt && SongVisualAssetBytes(s, p, bytes)) {
                    g_pendingVirtualBytes.swap(bytes);
                    return std::string("\x01ram");
                }
            }
            std::string visual = EnsureSongVisualAsset(s, p);
            if (!visual.empty()) {
                LogLine("[assets] lazy visual '%s' -> '%s'", p.c_str(), visual.c_str());
                return visual;
            }
            if (!s.cacheDir.empty() && fn == s.id + ".ssf") {
                EnsureSongPreview(s);
                std::string preview = JoinPath(s.cacheDir, fn);
                if (LocalFileExists(preview)) return preview;
                return std::string();
            }
            // Chart-load .gds: "<mode>1p-<id>[-hd].ini" -- judgement windows, gauge rates, level.
            // KParser::readFile decrypts it to a sibling ".int" and reads that, so both go to cache.
            if (!s.cacheDir.empty() && !HasSlashOrDrive(p)) {
                if (EndsWithCaseInsensitive(fn, ".ini")) {
                    std::string ini = EnsureSongIni(s, fn);
                    if (!ini.empty()) return ini;
                } else if (EndsWithCaseInsensitive(fn, ".int")) {
                    return JoinPath(s.cacheDir, fn);
                }
            }
        }

        // v19: context-aware visual cache. system\disc and system\eyecatch can request the
        // same file name (bms000001.abm), but they are different asset classes.
        if (!DirectBmsEnabled() && !s.cacheDir.empty()) {
            std::string visual = VisualCacheRedirectForPath(s, p);
            if (!visual.empty()) {
                g_lastCustomCacheDir = s.cacheDir;
                LogLine("[custom] redirect visual cache '%s' -> '%s'", p.c_str(), visual.c_str());
                return visual;
            }
        }

        // v9 real bridge path: route every custom-id file request to CustomCache\<mode>\<song_id>.
        // The binary converters are intentionally separate; this hook is the loader bridge.
        if (!DirectBmsEnabled() && !s.cacheDir.empty()) {
            g_lastCustomCacheDir = s.cacheDir;
            std::string fn = FileNameOnly(p);
            // Hover preview (<id>.ssf): generate on demand (12s window of the BGM, mono 22050 16-bit =
            // the game's SSF format) and serve it. (The old disable blamed it for the category-change
            // abort — that was a RED HERRING; the abort was the texture pool, now fixed by the EXE patch.)
            if (fn == s.id + ".ssf") {
                EnsureSongPreview(s);
                std::string pv = JoinPath(s.cacheDir, fn);
                if (LocalFileExists(pv)) { g_lastCustomCacheDir = s.cacheDir; return pv; }
                return std::string(); // generation failed → game falls back to System\Preview\0Blank.wav
            }
            std::string out = JoinPath(s.cacheDir, fn);
            LogLine("[custom] redirect file cache '%s' -> '%s'", p.c_str(), out.c_str());
            return out;
        }
    }

    // v10.2: once a custom chart/EZI/EZZ has been routed, sample loader opens bare
    // filenames such as by.wav/by.ssf. Route those to the same CustomCache dir.
    if (!DirectBmsEnabled() && !g_lastCustomCacheDir.empty() && !HasSlashOrDrive(p) && HasSampleFileExtension(p)) {
        std::string direct = JoinPath(g_lastCustomCacheDir, p);
        // First miss on a keysound = the song is being played: materialize all its keysounds
        // now (deferred from scan) and evict the previously-played song's. See bms_scanner.cpp.
        if (!LocalFileExists(direct)) {
            const CustomBmsSong* s = FindCustomSongByCacheDir(g_lastCustomCacheDir);
            if (s) EnsureSongKeysounds(*s);
        }
        if (LocalFileExists(direct)) {
            LogLine("[custom] redirect sample cache '%s' -> '%s'", p.c_str(), direct.c_str());
            return direct;
        }
    }
    return std::string();
}

static void EmitU8Local(unsigned char*& p, unsigned char v) { *p++ = v; }
static void EmitU32Local(unsigned char*& p, unsigned long v) { memcpy(p, &v, 4); p += 4; }
static void* AllocExecLocal(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static bool BuildSimpleGateway(void* target, size_t overwriteLen, const unsigned char* original, void** gatewayOut) {
    if (!target || !gatewayOut || overwriteLen < 5) return false;
    unsigned char* mem = static_cast<unsigned char*>(AllocExecLocal(overwriteLen + 8));
    if (!mem) return false;
    memcpy(mem, original, overwriteLen);
    unsigned char* q = mem + overwriteLen;
    EmitU8Local(q, 0xE9);
    intptr_t rel = (reinterpret_cast<intptr_t>(static_cast<unsigned char*>(target) + overwriteLen) - reinterpret_cast<intptr_t>(q + 4));
    EmitU32Local(q, static_cast<unsigned long>(rel));
    FlushInstructionCache(GetCurrentProcess(), mem, overwriteLen + 5);
    *gatewayOut = mem;
    return true;
}

// BMSBridge v0.3.5's useful BGA subset: let the game's DirectShow graph open the original
// BMS video, copy RGB frames into one exact 640x480 DirectDraw surface, then run the native draw
// phase. No global texture hook and no per-song SPV are involved.
using DirectLoadTexture_t = int (__thiscall*)(void*, const char*, void**, const char*);
using DirectCreateSurface_t = void* (__thiscall*)(void*, int*, int, int, int);
using DirectVideoReset_t = void* (__thiscall*)(void*);
using DirectVideoClose_t = void (__thiscall*)(void*);
using DirectVideoPlay_t = int (__thiscall*)(void*, int);
using DirectVideoStop_t = void (__thiscall*)(void*);
using DirectVideoReady_t = int (__thiscall*)(void*);
using DirectVideoGetFrame_t = HRESULT (__stdcall*)(void*, DWORD*, void*);
using DirectSurfaceGetDC_t = HRESULT (__stdcall*)(void*, HDC*);
using DirectSurfaceReleaseDC_t = HRESULT (__stdcall*)(void*, HDC);
using DirectSurfaceRelease_t = ULONG (__stdcall*)(void*);

static void* g_directVideoObject = nullptr;
static char g_directVideoPath[MAX_PATH] = {};
static bool g_directVideoDisabled = false;
static bool g_directVideoTargetReady = false;
static bool g_directVideoDelayArmed = false;
static bool g_directVideoStarted = false;
static bool g_directVideoOpenLogged = false;
static bool g_directVideoFirstFrameLogged = false;
static DWORD g_directVideoDelayClock = 0;
static DWORD g_directVideoStartMs = 0;
static DWORD g_directVideoTargetW = 640, g_directVideoTargetH = 480;
static DWORD g_directVideoCropX = 0, g_directVideoCropY = 0;
static DWORD g_directVideoCropW = 0, g_directVideoCropH = 0;


// ---- per-category bg.png / ico.png overlays ----------------------------------------------------
//
// Drawn OVER the game's own animated background, not instead of it. That distinction is what makes
// this cheap: songselect_bg.str is a 75-frame loop of 512x512 tiles, and replacing it by redirecting
// those frames would cost ~59MB of texture per category, because the pool keys on filename and never
// frees. Two surfaces of our own, refilled on category change, cost 1.6MB for the whole session.
//
// Both go through the game's own sprite blitter:
//
//     sub_403F80(ctx, texture, xBits, yBits, w, h, argb)
//
// x and y arrive as float BIT PATTERNS in int slots (the decompiler shows 1130692608 for 229.0),
// w and h as real floats, and the last argument is an ARGB modulation colour -- the 40 plain calls
// pass -1, which is 0xFFFFFFFF. So 75% opacity is not something to implement: it is 0xBFFFFFFF.
static const uint32_t kRenderCtx   = 0x1B2E320;   // *(_DWORD ***)ArgList
static const uint32_t kBgArgb      = 0x80FFFFFFu; // 50% alpha, no tint
static const uint32_t kIcoArgb     = 0xFFFFFFFFu;
static const int      kBgW = 640, kBgH = 480;     // the game's base resolution
static const int      kIcoSrc = 480;              // ico.png is square
static const int      kIcoDraw = 64;              // how big it lands in the corner
static const int      kIcoPad  = 6;

typedef int (__thiscall* DrawSpriteFn)(void* ctx, uint32_t tex, uint32_t xBits, uint32_t yBits,
                                       float w, float h, uint32_t argb);

// sub_40A490(ctx, src, dst) -- SetRenderState 19 and 20, which are SRCBLEND and DESTBLEND.
//
// The modulation alpha alone changed nothing because the blend factors around our draw are the
// game's default pair, (2,1) = ONE, ZERO: a straight opaque copy, where the source alpha has no
// effect at all. The four call sites in the binary only ever use (2,1) opaque, (2,2) additive and
// (9,6); there is no ALPHABLENDENABLE setter anywhere, so blending is on and it is these two that
// decide. (5,6) is SRCALPHA / INVSRCALPHA -- ordinary transparency -- and the pair is put back to
// (2,1) afterwards so nothing the HUD draws next inherits it.
typedef int (__thiscall* SetBlendFn)(void* ctx, int src, int dst);
static const int kBlendOne = 2, kBlendZero = 1, kBlendSrcAlpha = 5, kBlendInvSrcAlpha = 6;

static void*    g_ovBgTex = nullptr;
static void*    g_ovBgSurf = nullptr;
static void*    g_ovIcoTex = nullptr;
static void*    g_ovIcoSurf = nullptr;
static bool     g_ovInitTried = false;
static std::string g_ovCategory;      // what the surfaces currently hold
static std::string g_ovWantCategory;  // what the wheel is showing
static bool     g_ovHaveBg = false, g_ovHaveIco = false;

static uint32_t FloatBits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

// altName is the pool's key when one is given, so the two overlays MUST pass different names.
// Sharing it handed both calls the same texture object -- the diagnostic caught it as
// bgTex == icoTex == 27f940f0, which would have drawn the icon's contents as the background.
static bool MakeOverlaySurface(void* renderer, const char* baseName, const char* altName,
                               int w, int h, void** texOut, void** surfOut) {
    auto loadTexture   = reinterpret_cast<DirectLoadTexture_t>(Rva(0x08d00));
    auto createSurface = reinterpret_cast<DirectCreateSurface_t>(Rva(0x07450));
    void* texture = nullptr;
    if (!loadTexture(renderer, baseName, &texture, altName) || !texture) return false;
    int desc[31]{};
    void* surface = createSurface(renderer, desc, w, h, 0);
    if (!surface) return false;
    // The node's current surface is the blank tile the loader just made (a fresh node) or the
    // surface of ours it held before (a node that survived because the pool did): live either
    // way, and about to be displaced, so it is released here rather than leaked once per screen.
    void* displaced = *reinterpret_cast<void**>(texture);
    if (displaced && displaced != surface && !IsBadReadPtr(displaced, sizeof(void*))) {
        void** vt = *reinterpret_cast<void***>(displaced);
        if (vt && !IsBadReadPtr(vt, 3 * sizeof(void*)))
            reinterpret_cast<DirectSurfaceRelease_t>(vt[2])(displaced);
    }
    *reinterpret_cast<void**>(texture) = surface;
    *texOut = texture;
    *surfOut = surface;
    return true;
}

// Our texture is only usable while it still points at our own surface. When the pool is rebuilt the
// object is recycled for something else -- which is how the song's disc ended up being drawn as the
// background -- and this is the cheapest reliable way to notice.
static bool OverlayTexUsable(void* tex, void* surf) {
    if (!tex || !surf || IsBadReadPtr(tex, sizeof(void*))) return false;
    return *reinterpret_cast<void**>(tex) == surf;
}

// Playing a song rebuilds the texture pool WITHOUT recreating the SongSelect object -- the log
// shows the same ecx=40620000 before and after -- so tying this to the object pointer never fired.
//
// And the rebuild takes our surfaces with it: KGraphics::m408c80 (decomp kgraphics.cpp) walks
// every pooled KTexture and calls Release() on its surface UNTIL THE COUNT HITS ZERO, ours
// included. Repointing a fresh texture at the old surface pointer -- what this used to do -- put
// a freed surface into SetTexture, which is the crash inside dgVoodoo after a song. So a texture
// that no longer points at our surface means the whole set is gone: drop it and make it again.
static void RefreshOverlayTextures() {
    g_ovBgTex = g_ovBgSurf = g_ovIcoTex = g_ovIcoSurf = nullptr;
    g_ovInitTried = false;
    g_ovHaveBg = g_ovHaveIco = false;
    g_ovCategory.clear();               // repainted below, since the surfaces are new
}

static bool EnsureOverlaySurfaces() {
    if (g_ovBgTex && g_ovIcoTex) return true;
    if (g_ovInitTried) return false;
    void* renderer = reinterpret_cast<void*>(*reinterpret_cast<uint32_t*>(0x1b2f054));
    if (!renderer) return false;                 // song select not up yet; try again next frame
    g_ovInitTried = true;
    // Base tiles nothing else requests -- the same lesson as the preview, where building on the
    // game's own initdisc.bmp handed us a shared texture and freeing its surface crashed SBX.
    bool ok = MakeOverlaySurface(renderer, "system\\disc\\bmsoverlaybg.bmp", "bms-overlay-bg",
                                 kBgW, kBgH, &g_ovBgTex, &g_ovBgSurf) &&
              MakeOverlaySurface(renderer, "system\\disc\\bmsoverlayico.bmp", "bms-overlay-ico",
                                 kIcoSrc, kIcoSrc, &g_ovIcoTex, &g_ovIcoSurf);
    LogLine("[overlay] surfaces %s", ok ? "ready" : "FAILED");
    return ok;
}


// ---- pixel upload without GDI ------------------------------------------------------------------
//
// Every frame we used to push into a DirectDraw surface went through GetDC + StretchDIBits. Under
// dgVoodoo a GetDC on a texture surface is a GPU readback, a stall, and an upload on ReleaseDC --
// measured at ~7 ms per call, i.e. a third of the frame budget for the BGA video alone and the
// bulk of the stutter. Lock/Unlock hands us the pixels directly; the conversion to the surface's
// own format is ours. Nearest-neighbour when the source rectangle and the surface differ in size.
//
// IDirectDrawSurface7 vtable: 25 = Lock(rect, desc, flags, event), 32 = Unlock(rect).
// DDSURFACEDESC2 (0x7c bytes): lPitch @16, lpSurface @36, ddpfPixelFormat.dwRGBBitCount @84,
// dwRBitMask @88, dwGBitMask @92, dwBBitMask @96.
typedef HRESULT (__stdcall* DirectSurfaceLock_t)(void*, void*, void*, DWORD, HANDLE);
typedef HRESULT (__stdcall* DirectSurfaceUnlock_t)(void*, void*);

static void MaskShift(uint32_t mask, int& shift, int& bits) {
    shift = 0; bits = 0;
    if (!mask) return;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    while (mask & 1) { mask >>= 1; ++bits; }
}

// src: rows of `srcBits` (24 = BGR, 32 = BGRA) pixels, `srcStride` bytes apart; `topDown` says row 0
// is the top. The rectangle (cropX, cropY, cropW, cropH) of the source is scaled onto the whole
// surface (w x h).
static bool UploadPixelsToSurface(void* surface, const unsigned char* src, int srcW, int srcH,
                                  int srcBits, int srcStride, bool topDown,
                                  int cropX, int cropY, int cropW, int cropH, int w, int h) {
    if (!surface || !src || srcW <= 0 || srcH <= 0 || w <= 0 || h <= 0 || cropW <= 0 || cropH <= 0) return false;
    if (srcBits != 24 && srcBits != 32) return false;
    void** vt = *reinterpret_cast<void***>(surface);
    auto lock   = reinterpret_cast<DirectSurfaceLock_t>(vt[25]);
    auto unlock = reinterpret_cast<DirectSurfaceUnlock_t>(vt[32]);
    unsigned char desc[0x7c] = {};
    *reinterpret_cast<uint32_t*>(desc) = 0x7c;
    // DDLOCK_WAIT | DDLOCK_WRITEONLY: we overwrite everything, so no readback is needed.
    if (FAILED(lock(surface, nullptr, desc, 0x1 | 0x20, nullptr))) return false;
    const int pitch = *reinterpret_cast<int32_t*>(desc + 16);
    unsigned char* dst = *reinterpret_cast<unsigned char**>(desc + 36);
    const uint32_t dstBits = *reinterpret_cast<uint32_t*>(desc + 84);
    const uint32_t rMask = *reinterpret_cast<uint32_t*>(desc + 88);
    const uint32_t gMask = *reinterpret_cast<uint32_t*>(desc + 92);
    const uint32_t bMask = *reinterpret_cast<uint32_t*>(desc + 96);
    bool ok = dst != nullptr && pitch > 0 && (dstBits == 32 || dstBits == 24 || dstBits == 16);
    if (ok) {
        int rs, rb, gs, gb, bs, bb;
        MaskShift(rMask, rs, rb); MaskShift(gMask, gs, gb); MaskShift(bMask, bs, bb);
        const bool plain32 = dstBits == 32 && rMask == 0xff0000 && gMask == 0xff00 && bMask == 0xff;
        const int sbpp = srcBits / 8;
        for (int y = 0; y < h; ++y) {
            const int sy = cropY + (int)((long long)y * cropH / h);
            const int row = topDown ? sy : (srcH - 1 - sy);
            const unsigned char* s = src + (size_t)row * srcStride;
            unsigned char* d = dst + (size_t)y * pitch;
            if (plain32 && cropW == w) {
                for (int x = 0; x < w; ++x) {
                    const unsigned char* p = s + (size_t)(cropX + x) * sbpp;
                    reinterpret_cast<uint32_t*>(d)[x] = 0xff000000u | (p[2] << 16) | (p[1] << 8) | p[0];
                }
                continue;
            }
            for (int x = 0; x < w; ++x) {
                const int sx = cropX + (int)((long long)x * cropW / w);
                const unsigned char* p = s + (size_t)sx * sbpp;
                const uint32_t b = p[0], g = p[1], r = p[2];
                if (dstBits == 32) {
                    reinterpret_cast<uint32_t*>(d)[x] = 0xff000000u | ((r >> (8 - rb)) << rs) | ((g >> (8 - gb)) << gs) | ((b >> (8 - bb)) << bs);
                } else if (dstBits == 24) {
                    d[x * 3 + 0] = (unsigned char)b; d[x * 3 + 1] = (unsigned char)g; d[x * 3 + 2] = (unsigned char)r;
                } else {
                    reinterpret_cast<uint16_t*>(d)[x] = (uint16_t)(((r >> (8 - rb)) << rs) | ((g >> (8 - gb)) << gs) | ((b >> (8 - bb)) << bs));
                }
            }
        }
    }
    unlock(surface, nullptr);
    return ok;
}

static bool BlitBgrToSurface(void* surface, const std::vector<unsigned char>& px, int w, int h) {
    if (px.empty() || px.size() < (size_t)w * h * 3) return false;
    return UploadPixelsToSurface(surface, px.data(), w, h, 24, w * 3, true, 0, 0, w, h, w, h);
}

static bool BlitImageToSurface(void* surface, const std::string& png, int w, int h) {
    std::vector<unsigned char> px;
    if (!RenderImageToBgr(png, w, h, px) || px.empty()) return false;
    return BlitBgrToSurface(surface, px, w, h);
}

// Told by the filter which category the wheel moved to.
void CategoryOverlaySetCategory(const char* name) {
    g_ovWantCategory = name ? name : "";
}

// Called at the top of the song-select frame draw, so everything it renders lands on top.
void CategoryOverlayDraw() {
    if (!Config().categoryOverlay) return;
    if (!EnsureOverlaySurfaces()) return;

    {   // one-shot: what the draw actually sees, so a silent no-op names its own cause
        static unsigned s_diag = 0;
        if (s_diag++ < 3)
            LogLine("[overlay] tick want='%s' have='%s' bgTex=%p icoTex=%p",
                    g_ovWantCategory.c_str(), g_ovCategory.c_str(), g_ovBgTex, g_ovIcoTex);
    }
    // A pool rebuild can land between category changes -- playing a song does exactly that -- so
    // the textures are checked every frame, not only when the category moves.
    if (!OverlayTexUsable(g_ovBgTex, g_ovBgSurf) || !OverlayTexUsable(g_ovIcoTex, g_ovIcoSurf)) {
        RefreshOverlayTextures();
        if (!EnsureOverlaySurfaces()) return;
    }
    if (g_ovCategory != g_ovWantCategory) {
        g_ovCategory = g_ovWantCategory;
        g_ovHaveBg = g_ovHaveIco = false;
        if (!g_ovCategory.empty()) {
            const std::string mode = GetActiveMode();
            const std::string bg  = CategoryOverlayPath(mode, g_ovCategory, "bg");
            const std::string ico = CategoryOverlayPath(mode, g_ovCategory, "ico");
            // A category with no files just keeps the stock screen: nothing is drawn, nothing fails.
            if (!bg.empty())  g_ovHaveBg  = BlitImageToSurface(g_ovBgSurf,  bg,  kBgW,   kBgH);
            if (!ico.empty()) g_ovHaveIco = BlitImageToSurface(g_ovIcoSurf, ico, kIcoSrc, kIcoSrc);
            LogLine("[overlay] category='%s' bg=%d ico=%d", g_ovCategory.c_str(),
                    (int)g_ovHaveBg, (int)g_ovHaveIco);
        }
    }
    if (!g_ovHaveBg && !g_ovHaveIco) return;

    auto draw     = reinterpret_cast<DrawSpriteFn>(Rva(0x03f80));
    auto setBlend = reinterpret_cast<SetBlendFn>(Rva(0x0a490));
    void* ctx = reinterpret_cast<void*>(kRenderCtx);

    setBlend(ctx, kBlendSrcAlpha, kBlendInvSrcAlpha);
    if (g_ovHaveBg)
        draw(ctx, reinterpret_cast<uint32_t>(g_ovBgTex), FloatBits(0.0f), FloatBits(0.0f),
             (float)kBgW, (float)kBgH, kBgArgb);
    if (g_ovHaveIco)
        draw(ctx, reinterpret_cast<uint32_t>(g_ovIcoTex),
             FloatBits((float)(kBgW - kIcoDraw - kIcoPad)),
             FloatBits((float)(kBgH - kIcoDraw - kIcoPad)),
             (float)kIcoDraw, (float)kIcoDraw, kIcoArgb);
    setBlend(ctx, kBlendOne, kBlendZero);   // back to the opaque pair the HUD expects
}

// The object died: the texture pool went with it.
void CategoryOverlayReset() {
    g_ovBgTex = g_ovBgSurf = g_ovIcoTex = g_ovIcoSurf = nullptr;
    g_ovInitTried = false;
    g_ovCategory.clear();
    g_ovHaveBg = g_ovHaveIco = false;
    HudPanelReset();
    DetailPanelsReset();
}

// ---- HUD panel ---------------------------------------------------------------------------------
//
// The mod's own notice in song select, built the way the game builds its panels (decoded from
// System\SongSelect: TT_course_notice1, TT_UseKey2_panel, Rank\p_rank_panel + _mask): a rounded
// black card with a thin grey border, cyan title, white bold body behind a yellow triangle, and the
// red key badges the game itself uses for the effector buttons. Two textures, drawn like the
// natives draw theirs: the MASK first with the (DESTCOLOR, INVSRCALPHA) pair, which multiplies the
// background down to near black inside the shape and leaves it alone outside, then the ART with the
// additive pair, where black adds nothing -- so the corners are round and the fade is a modulation
// colour, with no alpha channel anywhere. Drawn at the end of the song-select frame (doOp7(1)), so
// it lands on top of the HUD.
static const int kPanelW = 320, kPanelH = 72;
static const int kPanelRadius = 9;
static const COLORREF kPanelFill    = RGB(12, 14, 20);
static const COLORREF kPanelBorder  = RGB(110, 114, 120);   // the key panel's grey rim
static const COLORREF kPanelTitle   = RGB(50, 214, 255);    // "CATEGORY 01" cyan
static const COLORREF kPanelText    = RGB(255, 255, 255);
static const COLORREF kPanelSub     = RGB(200, 204, 210);
static const COLORREF kPanelArrow   = RGB(255, 208, 0);     // the effector rows' triangle
static const COLORREF kBadgeRed     = RGB(190, 16, 26), kBadgeRedHi = RGB(236, 40, 52), kBadgeRedRim = RGB(90, 8, 12);
static const COLORREF kBadgeGrey    = RGB(110, 114, 122), kBadgeGreyHi = RGB(160, 164, 172), kBadgeGreyRim = RGB(50, 52, 56);
static const COLORREF kMaskInside   = RGB(30, 30, 30);      // what the background is multiplied by
static const DWORD    kPanelFadeIn = 120, kPanelFadeOut = 220;
static const int      kBlendDestColor = 9;                  // with kBlendInvSrcAlpha: the mask pair

static void*       g_panelTex = nullptr;
static void*       g_panelSurf = nullptr;
static void*       g_ezLabelTex = nullptr;     // the EZ column's label, see EzColumnDraw
static void*       g_ezLabelSurf = nullptr;    // what it pointed at when loaded: a rebuilt pool changes it
static void*       g_panelMaskTex = nullptr;
static void*       g_panelMaskSurf = nullptr;
static bool        g_panelInitTried = false;
static std::string g_panelTitle, g_panelLine, g_panelSub, g_panelHint;
static bool        g_panelDirty = false;
static DWORD       g_panelShownAt = 0, g_panelHoldMs = 0;
static bool        g_panelVisible = false;
static bool        g_panelPosOverride = false;   // draw at the gameplay spot instead of the config one

static std::wstring PanelWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0 && !MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n))
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static HFONT PanelFont(int px, int weight) {
    // Slightly condensed, like the game's own sprite type.
    return CreateFontW(-px, (int)(px * 0.42f), 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Arial");
}

static void PanelText(HDC hdc, const std::string& s, int px, int weight, COLORREF col, RECT r, UINT fmt, bool shadow) {
    if (s.empty()) return;
    HFONT font = PanelFont(px, weight);
    HGDIOBJ old = SelectObject(hdc, font);
    std::wstring w = PanelWide(s);
    if (shadow) {
        RECT sh = r; OffsetRect(&sh, 1, 1);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, w.c_str(), -1, &sh, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SetTextColor(hdc, col);
    DrawTextW(hdc, w.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
    DeleteObject(font);
}

// A key badge in the game's style: glossy rounded square, white bold glyph. Red for the effector
// buttons, grey for the pedal. Returns the width used.
static int PanelBadge(HDC hdc, int x, int y, const std::string& label, bool red) {
    const int h = 15;
    const int w = label.size() <= 2 ? 19 : 8 + (int)label.size() * 7;
    HBRUSH rim = CreateSolidBrush(red ? kBadgeRedRim : kBadgeGreyRim);
    HBRUSH lo  = CreateSolidBrush(red ? kBadgeRed : kBadgeGrey);
    HBRUSH hi  = CreateSolidBrush(red ? kBadgeRedHi : kBadgeGreyHi);
    HPEN   none = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldPen = SelectObject(hdc, none);
    HGDIOBJ oldBrush = SelectObject(hdc, rim);
    RoundRect(hdc, x, y, x + w + 1, y + h + 1, 5, 5);
    SelectObject(hdc, lo);
    RoundRect(hdc, x + 1, y + 1, x + w, y + h, 4, 4);
    SelectObject(hdc, hi);
    RECT top = {x + 2, y + 2, x + w - 1, y + h / 2};
    FillRect(hdc, &top, hi);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(rim); DeleteObject(lo); DeleteObject(hi);
    PanelText(hdc, label, 10, FW_BOLD, RGB(255, 255, 255), RECT{x, y, x + w, y + h}, DT_CENTER | DT_VCENTER, false);
    return w;
}

// "E4", "PEDAL+E3": badges joined by a small plus, laid out right-to-left from `right`.
static void PanelHint(HDC hdc, const std::string& hint, int right, int y) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : hint) { if (c == '+') { parts.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) parts.push_back(cur);
    int width = 0;
    for (size_t i = 0; i < parts.size(); ++i) width += (parts[i].size() <= 2 ? 19 : 8 + (int)parts[i].size() * 7) + (i ? 10 : 0);
    int x = right - width;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) { PanelText(hdc, "+", 11, FW_BOLD, kPanelText, RECT{x - 1, y - 1, x + 10, y + 15}, DT_CENTER | DT_VCENTER, false); x += 10; }
        const bool red = parts[i].size() <= 2;      // E1..E4; anything longer is the pedal
        x += PanelBadge(hdc, x, y, parts[i], red);
    }
}

// Both textures' pixels: the art (black outside the card, content inside) and the mask (white
// outside, dark inside). GDI into top-down 24-bit DIBs.
static bool RenderPanelBgr(std::vector<unsigned char>& art, std::vector<unsigned char>& mask) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kPanelW;
    info.bmiHeader.biHeight = -kPanelH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; ++pass) {
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { ok = false; break; }
        HGDIOBJ oldBmp = SelectObject(hdc, bmp);
        RECT all = {0, 0, kPanelW, kPanelH};
        HBRUSH ground = CreateSolidBrush(pass == 0 ? RGB(0, 0, 0) : RGB(255, 255, 255));
        FillRect(hdc, &all, ground);
        DeleteObject(ground);
        HPEN none = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ oldPen = SelectObject(hdc, none);
        if (pass == 1) {
            HBRUSH inside = CreateSolidBrush(kMaskInside);
            HGDIOBJ ob = SelectObject(hdc, inside);
            RoundRect(hdc, 0, 0, kPanelW + 1, kPanelH + 1, kPanelRadius * 2, kPanelRadius * 2);
            SelectObject(hdc, ob);
            DeleteObject(inside);
        } else {
            HBRUSH rim = CreateSolidBrush(kPanelBorder);
            HGDIOBJ ob = SelectObject(hdc, rim);
            RoundRect(hdc, 0, 0, kPanelW + 1, kPanelH + 1, kPanelRadius * 2, kPanelRadius * 2);
            HBRUSH fill = CreateSolidBrush(kPanelFill);
            SelectObject(hdc, fill);
            RoundRect(hdc, 2, 2, kPanelW - 1, kPanelH - 1, kPanelRadius * 2 - 3, kPanelRadius * 2 - 3);
            SelectObject(hdc, ob);
            DeleteObject(rim); DeleteObject(fill);
            SetBkMode(hdc, TRANSPARENT);
            PanelText(hdc, g_panelTitle, 14, FW_BOLD, kPanelTitle, RECT{14, 7, kPanelW - 120, 24}, DT_LEFT, true);
            PanelHint(hdc, g_panelHint, kPanelW - 14, 6);
            // The triangle the effector rows put before their text.
            POINT tri[3] = {{14, 30}, {14, 42}, {22, 36}};
            HBRUSH arrow = CreateSolidBrush(kPanelArrow);
            HGDIOBJ oa = SelectObject(hdc, arrow);
            Polygon(hdc, tri, 3);
            SelectObject(hdc, oa);
            DeleteObject(arrow);
            PanelText(hdc, g_panelLine, 19, FW_BOLD, kPanelText, RECT{28, 24, kPanelW - 12, 49}, DT_LEFT, true);
            PanelText(hdc, g_panelSub, 12, FW_NORMAL, kPanelSub, RECT{28, 50, kPanelW - 12, 66}, DT_LEFT, true);
        }
        SelectObject(hdc, oldPen);
        GdiFlush();
        std::vector<unsigned char>& out = pass == 0 ? art : mask;
        out.assign(static_cast<unsigned char*>(bits), static_cast<unsigned char*>(bits) + kPanelW * kPanelH * 3);
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(hdc);
    return ok;
}


// ---- song-select detail panels ------------------------------------------------------------------
//
// Two read-outs for the chart under the cursor, in the two places the layout leaves free: the
// figures go above the disc and the density graph along the bottom, between FREE PLAY and the
// logo. Both are built the way the notice card is -- a GDI render into an art buffer and a mask
// buffer, uploaded to a pair of pooled surfaces and drawn in two passes, so they sit over the
// wheel without a rectangle of background around them.
//
// Positions are game coordinates (the game renders 640x480 and the window scales it).
// The band between the category tabs and the disc, and it really is only that: the tab row sits
// at about y 60..75 and the disc's top edge at about y 160, so the plate starts below the tabs
// and its 76 rows stop clear of both. Growing it upward for more room put it straight through
// the tabs. The content is sized to this, not the other way round.
static const int kStatsX = 300, kStatsY = 80;
static const int kGraphX = 385, kGraphY = 410;

static void* g_statsTex = nullptr;  static void* g_statsSurf = nullptr;
static void* g_statsMaskTex = nullptr;  static void* g_statsMaskSurf = nullptr;
static void* g_graphTex = nullptr;  static void* g_graphSurf = nullptr;
static void* g_graphMaskTex = nullptr;  static void* g_graphMaskSurf = nullptr;
static bool  g_detailInitTried = false;
static std::string g_detailKey;          // id + slot currently rendered into the surfaces

void DetailPanelsReset() {
    g_statsTex = g_statsSurf = g_statsMaskTex = g_statsMaskSurf = nullptr;
    g_graphTex = g_graphSurf = g_graphMaskTex = g_graphMaskSurf = nullptr;
    g_detailInitTried = false;
    g_detailKey.clear();
}

static bool EnsureDetailSurfaces() {
    if (g_statsTex && g_graphTex) return true;
    if (g_detailInitTried) return false;
    void* renderer = reinterpret_cast<void*>(*reinterpret_cast<uint32_t*>(0x1b2f054));
    if (!renderer) return false;
    g_detailInitTried = true;
    const bool ok =
        MakeOverlaySurface(renderer, "system\\disc\\bmsstats.bmp", "bms-stats",
                           kStatsPanelW, kStatsPanelH, &g_statsTex, &g_statsSurf) &&
        MakeOverlaySurface(renderer, "system\\disc\\bmsstatsmask.bmp", "bms-stats-mask",
                           kStatsPanelW, kStatsPanelH, &g_statsMaskTex, &g_statsMaskSurf) &&
        MakeOverlaySurface(renderer, "system\\disc\\bmsgraph.bmp", "bms-graph",
                           kGraphPanelW, kGraphPanelH, &g_graphTex, &g_graphSurf) &&
        MakeOverlaySurface(renderer, "system\\disc\\bmsgraphmask.bmp", "bms-graph-mask",
                           kGraphPanelW, kGraphPanelH, &g_graphMaskTex, &g_graphMaskSurf);
    LogLine("[detail] surfaces %s", ok ? "ready" : "FAILED");
    return ok;
}

// A rounded plate in the notice card's own convention, which is NOT symmetric: the ART is black
// outside the shape and the card's colour inside, the MASK is WHITE outside and dark inside.
// The mask is drawn with the destination-colour blend, so white leaves the wheel untouched around
// the plate and the dark inside multiplies it down to near black; the art is then added on top.
//
// Getting those two the wrong way round is what let the custom background show straight through
// these panels while blacking out a rectangle around them.
static void DetailPlate(HDC art, HDC mask, int w, int h) {
    RECT all = {0, 0, w, h};
    HBRUSH artGround = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(art, &all, artGround);
    DeleteObject(artGround);
    HBRUSH maskGround = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mask, &all, maskGround);
    DeleteObject(maskGround);

    HPEN none = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldMaskPen = SelectObject(mask, none);
    HBRUSH inside = CreateSolidBrush(kMaskInside);
    HGDIOBJ om = SelectObject(mask, inside);
    RoundRect(mask, 0, 0, w + 1, h + 1, kPanelRadius * 2, kPanelRadius * 2);
    SelectObject(mask, om);
    SelectObject(mask, oldMaskPen);
    DeleteObject(inside);

    HGDIOBJ oldArtPen = SelectObject(art, none);
    HBRUSH rim = CreateSolidBrush(kPanelBorder);
    HGDIOBJ oa = SelectObject(art, rim);
    RoundRect(art, 0, 0, w + 1, h + 1, kPanelRadius * 2, kPanelRadius * 2);
    HBRUSH fill = CreateSolidBrush(kPanelFill);
    SelectObject(art, fill);
    RoundRect(art, 2, 2, w - 1, h - 1, kPanelRadius * 2 - 3, kPanelRadius * 2 - 3);
    SelectObject(art, oa);
    SelectObject(art, oldArtPen);
    DeleteObject(rim); DeleteObject(fill);
}

static bool RenderDetailBgr(const ChartStats& st, bool graph,
                            std::vector<unsigned char>& artOut, std::vector<unsigned char>& maskOut) {
    const int w = graph ? kGraphPanelW : kStatsPanelW;
    const int h = graph ? kGraphPanelH : kStatsPanelH;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    bool ok = false;
    HDC dcA = CreateCompatibleDC(screen), dcM = CreateCompatibleDC(screen);
    void* bitsA = nullptr; void* bitsM = nullptr;
    HBITMAP bmA = dcA ? CreateDIBSection(dcA, &info, DIB_RGB_COLORS, &bitsA, nullptr, 0) : nullptr;
    HBITMAP bmM = dcM ? CreateDIBSection(dcM, &info, DIB_RGB_COLORS, &bitsM, nullptr, 0) : nullptr;
    if (dcA && dcM && bmA && bmM && bitsA && bitsM) {
        HGDIOBJ oldA = SelectObject(dcA, bmA), oldM = SelectObject(dcM, bmM);
        SetBkMode(dcA, TRANSPARENT);
        DetailPlate(dcA, dcM, w, h);

        if (!graph) {
            // Laid out against the plate's 70 usable rows, with the label and the value column
            // kept apart. They used to share a band -- the big note count was drawn over the rect
            // "NOTES" had just been written into, which is what bit the corner off the label --
            // and the four rows ran past the bottom rim, so JACK fell off the panel entirely.
            // No heading: at this size it cost a row and said nothing the figures do not.
            const int pad = 8, right = w - pad, split = w / 2 + 2;
            char buf[32];

            // The note count keeps the headline size but shares its band with its label instead of
            // taking a row of its own -- the two sit in different columns, so the height costs once.
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.notes);
            PanelText(dcA, "NOTES", 9, FW_BOLD, kPanelTitle, RECT{pad, 8, split, 20}, DT_LEFT, true);
            PanelText(dcA, buf, 15, FW_BOLD, kPanelText, RECT{split, 5, right, 24}, DT_RIGHT, true);

            struct Row { const char* label; const char* fmt; double value; };
            const Row rows[] = {
                {"NPS",   "%.1f",   st.nps},
                {"PEAK",  "%.1f",   st.peakNps},
                {"CHORD", "%.0f%%", st.chordPct},
                {"JACK",  "%.0f%%", st.jackPct},
            };
            int y = 26;                          // 26 + 4 rows of 11 ends at 70, inside the 76 plate
            for (const Row& r : rows) {
                snprintf(buf, sizeof(buf), r.fmt, r.value);
                PanelText(dcA, r.label, 9, FW_NORMAL, kPanelSub, RECT{pad, y, split, y + 11}, DT_LEFT, true);
                PanelText(dcA, buf, 10, FW_BOLD, kPanelText, RECT{split, y, right, y + 11}, DT_RIGHT, true);
                y += 11;
            }
        } else {
            // The bars. Height is the slice's share of the busiest one, the colour goes from the
            // panel's cyan up to a hot orange so a burst reads at a glance, and the mask is painted
            // to match so the bars are the only thing that lands on screen.
            const int left = (w - kDensityBuckets * 2) / 2;
            const int base = h - 8, top = 16;
            const int tall = base - top;
            PanelText(dcA, "DENSITY", 9, FW_BOLD, kPanelTitle, RECT{8, 3, w - 8, 15}, DT_LEFT, true);
            char len[24];
            snprintf(len, sizeof(len), "%d:%02d", (int)st.seconds / 60, (int)st.seconds % 60);
            PanelText(dcA, len, 9, FW_NORMAL, kPanelSub, RECT{8, 3, w - 8, 15}, DT_RIGHT, true);
            for (int i = 0; i < kDensityBuckets; ++i) {
                const int v = st.density[i];
                int bh = (v * tall + 127) / 255;
                if (v && bh < 1) bh = 1;
                if (!bh) continue;
                const int x = left + i * 2;
                RECT bar = {x, base - bh, x + 2, base};
                const int heat = v;
                // Art only: the plate's mask has already darkened everything under it, and painting
                // the bars into the mask too would punch the background back through them.
                HBRUSH b = CreateSolidBrush(RGB(50 + heat * 205 / 255, 214 - heat * 80 / 255, 255 - heat * 200 / 255));
                FillRect(dcA, &bar, b);
                DeleteObject(b);
            }
            HBRUSH line = CreateSolidBrush(RGB(60, 78, 96));
            RECT axis = {left, base, left + kDensityBuckets * 2, base + 1};
            FillRect(dcA, &axis, line);
            DeleteObject(line);
        }

        GdiFlush();
        artOut.assign(static_cast<unsigned char*>(bitsA), static_cast<unsigned char*>(bitsA) + (size_t)w * h * 3);
        maskOut.assign(static_cast<unsigned char*>(bitsM), static_cast<unsigned char*>(bitsM) + (size_t)w * h * 3);
        SelectObject(dcA, oldA); SelectObject(dcM, oldM);
        ok = true;
    }
    if (bmA) DeleteObject(bmA);
    if (bmM) DeleteObject(bmM);
    if (dcA) DeleteDC(dcA);
    if (dcM) DeleteDC(dcM);
    ReleaseDC(nullptr, screen);
    return ok;
}

static void DrawDetailPair(void* maskTex, void* tex, int x, int y, int w, int h) {
    auto draw     = reinterpret_cast<DrawSpriteFn>(Rva(0x03f80));
    auto setBlend = reinterpret_cast<SetBlendFn>(Rva(0x0a490));
    void* ctx = reinterpret_cast<void*>(kRenderCtx);
    const uint32_t white = 0xffffffffu;
    setBlend(ctx, kBlendDestColor, kBlendInvSrcAlpha);
    draw(ctx, reinterpret_cast<uint32_t>(maskTex), FloatBits((float)x), FloatBits((float)y), (float)w, (float)h, white);
    setBlend(ctx, kBlendOne, kBlendOne);
    draw(ctx, reinterpret_cast<uint32_t>(tex), FloatBits((float)x), FloatBits((float)y), (float)w, (float)h, white);
    setBlend(ctx, kBlendOne, kBlendZero);
}

// Called from the song-select frame end with the row under the cursor and the difficulty column
// it is showing. Nothing is drawn for a native song or a chart that was never measured.
void DetailPanelsDraw(const char* id, int slot) {
    if (!Config().detailPanels) return;
    const ChartStats* st = ChartStatsFor(id, slot);
    if (!st) { g_detailKey.clear(); return; }
    if (!EnsureDetailSurfaces()) return;
    if (!OverlayTexUsable(g_statsTex, g_statsSurf) || !OverlayTexUsable(g_statsMaskTex, g_statsMaskSurf) ||
        !OverlayTexUsable(g_graphTex, g_graphSurf) || !OverlayTexUsable(g_graphMaskTex, g_graphMaskSurf)) {
        DetailPanelsReset();                      // the pool was rebuilt; make them again
        if (!EnsureDetailSurfaces()) return;
    }
    char key[64];
    snprintf(key, sizeof(key), "%.31s|%d", id ? id : "", slot);
    if (g_detailKey != key) {
        std::vector<unsigned char> art, mask;
        if (RenderDetailBgr(*st, false, art, mask)) {
            BlitBgrToSurface(g_statsSurf, art, kStatsPanelW, kStatsPanelH);
            BlitBgrToSurface(g_statsMaskSurf, mask, kStatsPanelW, kStatsPanelH);
        }
        if (RenderDetailBgr(*st, true, art, mask)) {
            BlitBgrToSurface(g_graphSurf, art, kGraphPanelW, kGraphPanelH);
            BlitBgrToSurface(g_graphMaskSurf, mask, kGraphPanelW, kGraphPanelH);
        }
        g_detailKey = key;
    }
    DrawDetailPair(g_statsMaskTex, g_statsTex, kStatsX, kStatsY, kStatsPanelW, kStatsPanelH);
    DrawDetailPair(g_graphMaskTex, g_graphTex, kGraphX, kGraphY, kGraphPanelW, kGraphPanelH);
}

static bool EnsurePanelSurface() {
    if (g_panelTex && g_panelMaskTex) return true;
    if (g_panelInitTried) return false;
    void* renderer = reinterpret_cast<void*>(*reinterpret_cast<uint32_t*>(0x1b2f054));
    if (!renderer) return false;
    g_panelInitTried = true;
    bool ok = MakeOverlaySurface(renderer, "system\\disc\\bmspanel.bmp", "bms-panel",
                                 kPanelW, kPanelH, &g_panelTex, &g_panelSurf) &&
              MakeOverlaySurface(renderer, "system\\disc\\bmspanelmask.bmp", "bms-panel-mask",
                                 kPanelW, kPanelH, &g_panelMaskTex, &g_panelMaskSurf);
    LogLine("[panel] surfaces %s", ok ? "ready" : "FAILED");
    return ok;
}

// Show (or update) the notice. Calling again while it is up replaces the text and restarts the
// hold, so a button held down reads as one live card rather than a stack of them.
void HudPanelShow(const char* title, const char* line, const char* sub, const char* hint, DWORD holdMs) {
    if (!Config().hudPanel) return;
    std::string t = title ? title : "", l = line ? line : "", s = sub ? sub : "", h = hint ? hint : "";
    if (t != g_panelTitle || l != g_panelLine || s != g_panelSub || h != g_panelHint) {
        g_panelTitle = t; g_panelLine = l; g_panelSub = s; g_panelHint = h;
        g_panelDirty = true;
    }
    const DWORD now = GetTickCount();
    // A card already fully up just gets its hold restarted; one still fading in keeps its start.
    if (!g_panelVisible) g_panelShownAt = now;
    else if (now - g_panelShownAt > kPanelFadeIn) g_panelShownAt = now - kPanelFadeIn;
    g_panelHoldMs = holdMs;
    g_panelVisible = true;
}

void HudPanelHide() { g_panelVisible = false; }

// Called at the end of the song-select frame, after the HUD.
void HudPanelDraw() {
    if (!g_panelVisible || !Config().hudPanel) return;
    const DWORD now = GetTickCount();
    const DWORD age = now - g_panelShownAt;
    if (age > kPanelFadeIn + g_panelHoldMs + kPanelFadeOut) { g_panelVisible = false; return; }
    if (!EnsurePanelSurface()) return;
    // The pool was rebuilt (a song was played): its teardown released our surfaces along with
    // everything else, so both textures and both surfaces are made again -- never repointed.
    if (!OverlayTexUsable(g_panelTex, g_panelSurf) || !OverlayTexUsable(g_panelMaskTex, g_panelMaskSurf)) {
        HudPanelReset();
        g_panelVisible = true;
        if (!EnsurePanelSurface()) return;
    }
    if (g_panelDirty) {
        std::vector<unsigned char> art, mask;
        if (RenderPanelBgr(art, mask)) {
            BlitBgrToSurface(g_panelSurf, art, kPanelW, kPanelH);
            BlitBgrToSurface(g_panelMaskSurf, mask, kPanelW, kPanelH);
        }
        g_panelDirty = false;
    }
    float k = 1.0f;                               // 0..1 visibility
    if (age < kPanelFadeIn) k = (float)age / (float)kPanelFadeIn;
    else if (age > kPanelFadeIn + g_panelHoldMs) k = 1.0f - (float)(age - kPanelFadeIn - g_panelHoldMs) / (float)kPanelFadeOut;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    const uint32_t a = (uint32_t)(k * 255.0f);
    const uint32_t grey = (a << 24) | (a << 16) | (a << 8) | a;   // fades both passes, see above
    const float x = g_panelPosOverride ? 312.0f : (float)Config().hudPanelX;
    const float y = (g_panelPosOverride ? 396.0f : (float)Config().hudPanelY) - (1.0f - k) * 10.0f;   // settles down as it appears
    auto draw     = reinterpret_cast<DrawSpriteFn>(Rva(0x03f80));
    auto setBlend = reinterpret_cast<SetBlendFn>(Rva(0x0a490));
    void* ctx = reinterpret_cast<void*>(kRenderCtx);
    setBlend(ctx, kBlendDestColor, kBlendInvSrcAlpha);
    draw(ctx, reinterpret_cast<uint32_t>(g_panelMaskTex), FloatBits(x), FloatBits(y), (float)kPanelW, (float)kPanelH, grey);
    setBlend(ctx, kBlendOne, kBlendOne);
    draw(ctx, reinterpret_cast<uint32_t>(g_panelTex), FloatBits(x), FloatBits(y), (float)kPanelW, (float)kPanelH, grey);
    setBlend(ctx, kBlendOne, kBlendZero);
}

void HudPanelReset() {
    g_panelTex = g_panelSurf = g_panelMaskTex = g_panelMaskSurf = nullptr;
    g_panelInitTried = false;
    g_panelDirty = true;
    g_panelVisible = false;
    g_ezLabelTex = g_ezLabelSurf = nullptr;
}

// ---- the EZ column -----------------------------------------------------------------------------
//
// drawLevels (decomp songselect.cpp @0x4343b0) draws the four level digits at x = 169/249/329/409
// and, when the cursor sits on the dormant leftmost column (g_4abd14 == 0), draws nothing at all.
// The column itself is native: the stepper accepts it while the row's entry+0x210 float is set.
// So this draws what the game leaves out -- the EZ digit at x = 89 always, lit when chosen, and
// the four native digits (dim) while the cursor is on EZ -- from the game's own digit textures at
// obj+0xc040c, with the banner's slide (obj+0xc0470) so it rides in and out with the rest.
void EzColumnDraw(uint32_t obj) {
    if (!obj) return;
    const uint32_t* row = nullptr;
    const uint32_t sel = *reinterpret_cast<uint32_t*>(obj + 0x84);
    const uint32_t visible = *reinterpret_cast<uint32_t*>(obj + 0x7c);
    if (sel >= visible) return;
    row = reinterpret_cast<const uint32_t*>(obj + 0x12c + sel * 0x374);
    float ez = 0.0f, slide = 0.0f;
    memcpy(&ez, reinterpret_cast<const char*>(row) + 0x210, 4);
    memcpy(&slide, reinterpret_cast<const void*>(obj + 0xc0470), 4);
    if (!(ez > 0.0f && ez < 100.0f)) return;
    const float y = 207.0f - (512.0f - slide);
    if (y < -40.0f) return;                              // the banner is out
    const int column = *reinterpret_cast<int*>(0x4abd14);
    auto digit = [&](float level) -> uint32_t {
        int lv = (int)level;
        if (lv < 0) lv = 0;
        if (lv > 21) lv = 21;
        return *reinterpret_cast<uint32_t*>(obj + 0xc040c + (uint32_t)lv * 4);
    };
    auto draw     = reinterpret_cast<DrawSpriteFn>(Rva(0x03f80));
    auto setBlend = reinterpret_cast<SetBlendFn>(Rva(0x0a490));
    void* ctx = reinterpret_cast<void*>(kRenderCtx);
    const uint32_t lit = 0xff46e1ffu, dim = 0xffffffffu;
    setBlend(ctx, kBlendOne, kBlendOne);
    if (uint32_t t = digit(ez))
        draw(ctx, t, FloatBits(89.0f), FloatBits(y), 0.0f, 0.0f, column == 0 ? lit : dim);
    if (column == 0) {
        const float xs[4] = {169.0f, 249.0f, 329.0f, 409.0f};
        const uint32_t offs[4] = {0x214, 0x220, 0x22c, 0x238};
        for (int i = 0; i < 4; ++i) {
            float lv = 0.0f;
            memcpy(&lv, reinterpret_cast<const char*>(row) + offs[i], 4);
            if (uint32_t t = digit(lv)) draw(ctx, t, FloatBits(xs[i]), FloatBits(y), 0.0f, 0.0f, dim);
        }
    }
    // The label, once per pool life: a pooled texture of our own, additive so its black is clear.
    if (!OverlayTexUsable(g_ezLabelTex, g_ezLabelSurf)) {
        void* renderer = reinterpret_cast<void*>(*reinterpret_cast<uint32_t*>(0x1b2f054));
        auto loadTexture = reinterpret_cast<DirectLoadTexture_t>(Rva(0x08d00));
        void* t = nullptr;
        g_ezLabelTex = g_ezLabelSurf = nullptr;
        if (renderer && loadTexture(renderer, "system\\disc\\bmslabelez.bmp", &t, "bms-label-ez") && t) {
            g_ezLabelTex = t;
            g_ezLabelSurf = *reinterpret_cast<void**>(t);
        }
    }
    if (g_ezLabelTex)
        draw(ctx, reinterpret_cast<uint32_t>(g_ezLabelTex), FloatBits(90.0f), FloatBits(y - 36.0f), 0.0f, 0.0f, column == 0 ? lit : dim);
    setBlend(ctx, kBlendOne, kBlendZero);
}

// ---- song-select BGA preview -------------------------------------------------------------------
//
// The selected song's disc is a single texture slot on the object, obj+260, filled by
//
//     sprintf(Buffer, "system\\disc\\%s.bmp", (const char *)(this + 884 * a2 + 568));
//     sub_408E80(*(_DWORD **)ArgList, "HotSpotDisc", Buffer, this + 260);
//
// -- a NAMED load, so the game reuses that slot per song rather than accumulating. That makes it
// the right place to put a preview: one slot, one surface, and the original pointer comes back
// when the cursor moves.
//
// What is deliberately NOT done here: painting into the song's own disc texture. The texture pool
// is a bump allocator with no eviction, so overwriting a disc destroys that song's art for the rest
// of the process. Everything below draws into a surface we own and swaps the pointer instead.
//
// Cost is fixed, not per-song: one 256x256 surface and one frame buffer (~22MB at 10fps x 12s),
// reused for every preview. That matters here more than usual -- browsing already costs the process
// hundreds of megabytes in wheel textures, and this must not scale with the library on top of that.
static const int      kPreviewSize    = 256;      // matches initdisc.abm and the disc geometry
// Was a flat 10, which read as a slideshow next to the 30 fps the source clips run at. Now the
// setting, clamped 5..30 -- past 30 there is nothing left to gain, since the clips are 30 fps.
static int PreviewFps() { return Config().previewFps; }
// 12s at 10fps is 120 frames, 22.5MB. Measured: the decode still takes ~420ms, the same as six
// seconds did -- the cost is process spawn and seek, not the length -- so the whole price of the
// longer loop is the extra 11MB, and that stays fixed no matter how big the library grows.
static const int      kPreviewSeconds = 12;
static const DWORD    kPreviewDwellMs = 400;      // how long the cursor must rest before starting

static void*    g_previewTexture = nullptr;       // our texture object, slotted into obj+260
static void*    g_previewSurface = nullptr;       // the surface we paint frames into
static bool     g_previewInitTried = false;
static std::vector<unsigned char> g_previewFrames;
static uint32_t g_previewFrameCount = 0;
static uint32_t g_previewIndex = 0;
static DWORD    g_previewNextFrameMs = 0;
static DWORD    g_previewDwellSince = 0;
static std::string g_previewWantId;               // what the cursor is resting on
static std::string g_previewHaveId;               // what the frame buffer holds
static uint32_t g_previewObj = 0;
static uint32_t g_previewSavedSlot[5] = {};       // the disc pointers we displaced, per difficulty
static bool     g_previewSlotSwapped = false;
static HANDLE   g_previewThread = nullptr;
static volatile LONG g_previewDecoding = 0;
static std::string g_previewDecodePath;
static std::string g_previewDecodeId;
static DWORD g_previewDecodeStartMs = 0;

// The disc sprites f_104[5] (decomp songselectpage.cpp m431ae0): 0 NM "HotSpotDisc", 1 HD, 3 SHD,
// 4 EX (2 unused). The renderer draws f_104[f_f4] once the swing passes 180, so every difficulty
// slot has to carry the preview or a difficulty change brings the game's own disc back.
static const uint32_t kObjDiscSlot = 260;         // f_104[0]
static const int kPreviewSlots[4] = {0, 1, 3, 4};

// Build the surface once. The texture object is created from initdisc.abm purely to get a real
// 256x256 24bpp texture of the right shape; its surface is then replaced by ours, which is the
// same swap the gameplay BGA path does.
static bool EnsurePreviewSurface() {
    if (g_previewTexture && g_previewSurface) return true;
    if (g_previewInitTried) return false;
    g_previewInitTried = true;

    void* renderer = reinterpret_cast<void*>(*reinterpret_cast<uint32_t*>(0x1b2f054));
    if (!renderer) { g_previewInitTried = false; return false; }   // song select not up yet

    auto loadTexture   = reinterpret_cast<DirectLoadTexture_t>(Rva(0x08d00));
    auto createSurface = reinterpret_cast<DirectCreateSurface_t>(Rva(0x07450));

    // A name nothing else in the game ever requests. The first version of this built on
    // initdisc.bmp, which the game loads too -- and since the texture pool is keyed by filename,
    // that handed us a SHARED texture whose surface we then released out from under the game.
    // That is the SBX crash: a freed surface, its vtable pointer read back as null, and the
    // ReleaseDC lookup at vt[26] faulting on address 0x68.
    void* texture = nullptr;
    if (!loadTexture(renderer, "system\\disc\\bmspreviewbase.bmp", &texture, "bms-preview-disc") ||
        !texture) {
        LogLine("[preview] base texture failed");
        return false;
    }
    int desc[31]{};
    void* surface = createSurface(renderer, desc, kPreviewSize, kPreviewSize, 0);
    if (!surface) { LogLine("[preview] CreateSurface %dx%d failed", kPreviewSize, kPreviewSize); return false; }

    // Not released, for the same reason as the overlay: after a pool rebuild this pointer is
    // recycled memory and its vtable is not one, which crashed at pc=00000000. We did not create
    // that surface, so it is not ours to free.
    *reinterpret_cast<void**>(texture) = surface;
    g_previewTexture = texture;
    g_previewSurface = surface;
    LogLine("[preview] surface ready %dx%d texture=%p", kPreviewSize, kPreviewSize, texture);
    return true;
}

// g_previewDecoding is the gate that stops two decodes running at once, and it is cleared here.
// If this thread ever left without clearing it -- an exception out of a large allocation is the way
// that happens, and the .scr path reserves tens of megabytes -- the gate stayed shut for the rest of
// the session and no preview ever appeared again, whatever song was selected. That is the "changed
// difficulty and it never came back" report. So the clear is unconditional now.
static DWORD WINAPI PreviewDecodeThread(LPVOID) {
    std::vector<unsigned char> frames;
    uint32_t count = 0;
    try {
        // A video if the song has one; otherwise its .scr background, whose frames are already
        // images on disk.
        if (!g_previewDecodePath.empty())
            DecodeBgaPreview(g_previewDecodePath, kPreviewSize, PreviewFps(), kPreviewSeconds,
                             frames, count);
        // Native songs without a video animate their background with a .scr instead.
        //
        // There is deliberately no third source. Custom songs mostly have no preview material at
        // all: of 20050, only 1839 ship a video, and the rest do NOT have an animated BGA to fall
        // back on -- sampling 400 charts found 300 with a #STAGEFILE and 275 with a #BANNER, both
        // single title images, and a median of one image file in the whole folder. The 73 with a
        // #BMP table use it to declare the video. An image-sequence preview was built for those and
        // measured at zero songs, so it was removed rather than left to cost a decode per song.
        if (!count)
            DecodeScrPreview(g_previewDecodeId, kPreviewSize, PreviewFps(), kPreviewSeconds,
                             frames, count);
    } catch (...) {
        frames.clear();
        count = 0;
        LogLine("[preview] decode threw for id='%s'; no preview for it", g_previewDecodeId.c_str());
    }
    g_previewFrames.swap(frames);
    g_previewFrameCount = count;
    g_previewIndex = 0;
    // The id is recorded even when nothing decoded. Leaving it empty meant "not decoded yet", so a
    // song with neither a video nor a .scr was re-attempted on every single frame -- a thread per
    // tick, forever. Recording it says "tried, there is nothing", and the retry stops.
    g_previewHaveId = g_previewDecodeId;
    InterlockedExchange(&g_previewDecoding, 0);
    return 0;
}

// Put the song's own disc back.
//
// `onObj` is the object the caller is looking at right now. The restore only happens when that is
// still the object we swapped on -- leaving song select FREES it, and writing the saved pointer
// back into freed memory is the menu round-trip crash: a write to obj+0x104, which is exactly
// kObjDiscSlot on an object that no longer exists.
//
// Pass 0 for "no object in hand", which drops the bookkeeping without writing anything.
static void PreviewRestoreSlot(uint32_t onObj) {
    if (g_previewSlotSwapped && g_previewObj && onObj == g_previewObj)
        for (int k : kPreviewSlots)
            *reinterpret_cast<uint32_t*>(g_previewObj + kObjDiscSlot + 4 * k) = g_previewSavedSlot[k];
    g_previewSlotSwapped = false;
    for (int k : kPreviewSlots) g_previewSavedSlot[k] = 0;
}

// True when the surface still looks like a live COM object. DirectDraw surfaces can be lost --
// a mode switch, a device reset, the wrapper recreating them -- and the preview has to notice
// rather than call through a stale vtable.
static bool PreviewSurfaceUsable() {
    if (!g_previewSurface) return false;
    if (IsBadReadPtr(g_previewSurface, sizeof(void*))) return false;
    void** vt = *reinterpret_cast<void***>(g_previewSurface);
    if (!vt || IsBadReadPtr(vt, 27 * sizeof(void*))) return false;
    return vt[17] && vt[26];
}

// The surface check above is not enough on its own, and this is the same trap the category
// overlay already fell into: playing a song, or loading new disc art on a difficulty change,
// rebuilds the texture POOL without destroying the SongSelect object. Our surface is ours and
// survives, so PreviewSurfaceUsable keeps saying yes and every blit keeps reporting success --
// while the texture object the pool owned has been freed underneath us. Writing that dead pointer
// into the disc slot draws nothing, which is exactly "the preview disappears when I change
// difficulty and never comes back".
//
// The link is what proves it: we pointed the texture at our surface when we built it, so if that
// pointer no longer reads back as our surface, the object is not ours any more.
static bool PreviewTexUsable() {
    if (!g_previewTexture || IsBadReadPtr(g_previewTexture, sizeof(void*))) return false;
    return *reinterpret_cast<void**>(g_previewTexture) == g_previewSurface;
}

static void PreviewBlitFrame() {
    const size_t frameBytes = (size_t)kPreviewSize * kPreviewSize * 3u;
    if (g_previewIndex >= g_previewFrameCount) g_previewIndex = 0;
    const unsigned char* px = g_previewFrames.data() + (size_t)g_previewIndex * frameBytes;

    UploadPixelsToSurface(g_previewSurface, px, kPreviewSize, kPreviewSize, 24, kPreviewSize * 3, true,
                          0, 0, kPreviewSize, kPreviewSize, kPreviewSize, kPreviewSize);
    ++g_previewIndex;
}

// Called every frame from the song-select renderer hook with whatever the cursor is on.
void BgaPreviewTick(uint32_t obj, const char* selectedId) {
    if (!Config().bgaPreview) return;
    const std::string id = selectedId ? selectedId : "";

    // The cursor moved: put the disc back and start the dwell timer again. Restoring FIRST means a
    // fast scroll never leaves someone else's video sitting in the slot.
    if (obj != g_previewObj) {
        // A different SongSelect object: the previous one has been destroyed. Nothing of ours
        // survives that -- the texture pool is rebuilt with it, which is the same reset that used
        // to blank the version labels on a menu round-trip -- so drop everything and start clean.
        PreviewRestoreSlot(0);                 // 0: do not write to the object that just died
        CategoryOverlayReset();                // the overlay surfaces came from the same pool
        g_previewTexture = nullptr;
        g_previewSurface = nullptr;
        g_previewInitTried = false;
        g_previewHaveId.clear();
        g_previewFrameCount = 0;
        g_previewObj = obj;
        g_previewWantId = id;
        g_previewDwellSince = GetTickCount();
        return;
    }
    if (id != g_previewWantId) {
        PreviewRestoreSlot(obj);
        g_previewWantId = id;
        g_previewDwellSince = GetTickCount();
        return;
    }
    // Native ids are welcome here: they keep their BGA in Video\<id>\<id>.spv, which is the same
    // ASF/WMV the custom ones use. Rejecting anything not starting with "bms" was why the preview
    // only ever appeared on custom songs.
    if (id.empty()) return;
    if (GetTickCount() - g_previewDwellSince < kPreviewDwellMs) return;   // still settling
    // A custom song the cursor has really settled on is likely to be played, so its compressed
    // keysounds start decoding now, off this thread. Longer than the preview's dwell: a decode is
    // seconds of CPU, and a wheel being scrolled through must not queue one per row.
    if (id.compare(0, 3, "bms") == 0 && GetTickCount() - g_previewDwellSince >= 1200)
        KeysoundPrefetchRequest(id.c_str());

    // Decode once per song, off the game's thread: ffmpeg takes hundreds of milliseconds and the
    // wheel must keep animating through it.
    if (g_previewHaveId != id) {
        if (InterlockedCompareExchange(&g_previewDecoding, 1, 0) != 0) {
            // Belt and braces for the gate above: if a decode has somehow been in flight for far
            // longer than one can take, assume its thread is gone and reopen it. Costs one wasted
            // decode in the worst case; the alternative is a preview that never returns.
            if (GetTickCount() - g_previewDecodeStartMs > 30000) {
                InterlockedExchange(&g_previewDecoding, 0);
                LogLine("[preview] decode gate was stuck; reopened");
            }
            return;
        }
        g_previewDecodeStartMs = GetTickCount();
        // An empty path is no longer a dead end: the song may still have a .scr background, and
        // the decode thread falls through to that. Only a song with neither ends up with no frames.
        g_previewDecodePath = BgaVideoPathForSong(id);
        g_previewDecodeId = id;
        if (g_previewThread) { CloseHandle(g_previewThread); g_previewThread = nullptr; }
        g_previewThread = CreateThread(nullptr, 0, PreviewDecodeThread, nullptr, 0, nullptr);
        if (!g_previewThread) InterlockedExchange(&g_previewDecoding, 0);
        return;
    }
    if (!g_previewFrameCount || InterlockedCompareExchange(&g_previewDecoding, 0, 0)) return;
    if (!EnsurePreviewSurface()) return;
    if (!PreviewSurfaceUsable()) {
        // Drop everything and let it be rebuilt: holding a dead surface is how this crashed.
        PreviewRestoreSlot(obj);
        g_previewSurface = nullptr;
        g_previewTexture = nullptr;
        g_previewInitTried = false;
        static unsigned s_lost = 0;
        if (s_lost++ < 5) LogLine("[preview] surface lost; rebuilding");
        return;
    }
    if (!PreviewTexUsable()) {
        // The pool took the texture back. The saved disc pointer came from that same pool and is
        // just as dead, so it is NOT written back -- the game has already put a live disc in the
        // slot, and restoring the old one over it is how a freed texture reaches the renderer.
        g_previewSlotSwapped = false;
        g_previewTexture = nullptr;
        g_previewSurface = nullptr;
        g_previewInitTried = false;
        static unsigned s_pool = 0;
        if (s_pool++ < 5) LogLine("[preview] texture reclaimed by the pool; rebuilding");
        return;                                  // rebuilt next tick, and the preview resumes
    }

    // Hold the slot every tick rather than only on the swap: the game refills obj+260 whenever it
    // reloads the disc, and that would otherwise silently end the preview mid-loop.
    if (!g_previewSlotSwapped) {
        for (int k : kPreviewSlots)
            g_previewSavedSlot[k] = *reinterpret_cast<uint32_t*>(obj + kObjDiscSlot + 4 * k);
        g_previewSlotSwapped = true;
        g_previewNextFrameMs = 0;
        LogLine("[preview] running id='%s'", id.c_str());   // once per song, not once per session
    }
    // Did anyone overwrite the slot since the last frame?
    //
    // The decodes keep succeeding and none of the guards fire, yet the preview is only ever seen
    // once -- so the frames exist and something is undoing the swap rather than breaking it. This
    // says whether that is the game reloading the disc, and how often, instead of leaving it to be
    // guessed at. Logged a few times, then counted silently.
    {
        const uint32_t live = *reinterpret_cast<uint32_t*>(obj + kObjDiscSlot);
        const uint32_t ours = reinterpret_cast<uint32_t>(g_previewTexture);
        static uint32_t s_stolen = 0, s_logged = 0;
        if (live != ours) {
            ++s_stolen;
            if (s_logged < 6) {
                ++s_logged;
                LogLine("[preview] disc slot was %08lx, not ours (%08lx) -- steal #%lu",
                        (unsigned long)live, (unsigned long)ours, (unsigned long)s_stolen);
            }
        }
    }
    for (int k : kPreviewSlots)
        *reinterpret_cast<uint32_t*>(obj + kObjDiscSlot + 4 * k) = reinterpret_cast<uint32_t>(g_previewTexture);

    const DWORD now = GetTickCount();
    if (now >= g_previewNextFrameMs) {
        PreviewBlitFrame();
        g_previewNextFrameMs = now + (DWORD)(1000 / PreviewFps());
    }
}

// The list is about to be rebuilt under the preview. `obj` is the object the caller holds, so
// the disc pointer goes back only if that is still the one we swapped on.
void BgaPreviewStop(uint32_t obj) {
    PreviewRestoreSlot(obj);
    g_previewWantId.clear();
    g_previewHaveId.clear();
    g_previewFrameCount = 0;
}

static DWORD g_directVideoValidFrames = 0, g_directVideoFrameFailures = 0;
static unsigned char* g_directVideoFrameBuffer = nullptr;
static DWORD g_directVideoFrameCapacity = 0;
static bool g_directVideoFfmpeg = false;
static bool g_directVideoLoop = false;      // generic pool clip: restart instead of freezing
static DWORD g_directVideoLoopClock = 0;
static DWORD g_directVideoLoops = 0;

static void ResetDirectBmsVideoRuntime() {
    StopDirectBmsFfmpegVideo();
    g_directVideoObject = nullptr;
    g_directVideoPath[0] = 0;
    g_directVideoDisabled = false;
    g_directVideoTargetReady = false;
    g_directVideoDelayArmed = false;
    g_directVideoStarted = false;
    g_directVideoOpenLogged = false;
    g_directVideoFirstFrameLogged = false;
    g_directVideoDelayClock = 0;
    g_directVideoStartMs = 0;
    g_directVideoTargetW = 640; g_directVideoTargetH = 480;
    g_directVideoCropX = g_directVideoCropY = 0;
    g_directVideoCropW = g_directVideoCropH = 0;
    g_directVideoValidFrames = g_directVideoFrameFailures = 0;
    g_directVideoFfmpeg = false;
}

static bool EnsureDirectVideoFrameBuffer(DWORD bytes) {
    if (!bytes || bytes > 128u * 1024u * 1024u) return false;
    if (g_directVideoFrameBuffer && g_directVideoFrameCapacity >= bytes) return true;
    if (g_directVideoFrameBuffer) VirtualFree(g_directVideoFrameBuffer, 0, MEM_RELEASE);
    g_directVideoFrameBuffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_directVideoFrameCapacity = g_directVideoFrameBuffer ? bytes : 0;
    return g_directVideoFrameBuffer != nullptr;
}

static bool LoadDirectVideoTarget(void* videoObject, DWORD width, DWORD height) {
    if (!videoObject || !width || !height) return false;
    unsigned char* video = static_cast<unsigned char*>(videoObject);
    void* renderer = *reinterpret_cast<void**>(video + 1104);
    if (!renderer) return false;
    std::string blank = JoinPath(GetExeDirectory(), "System\\Common\\video_blank_512.bmp");
    if (!LocalFileExists(blank)) blank = JoinPath(GetExeDirectory(), "System\\Common\\video_blank_512.abm");
    auto loadTexture = reinterpret_cast<DirectLoadTexture_t>(Rva(0x08d00));
    auto createSurface = reinterpret_cast<DirectCreateSurface_t>(Rva(0x07450));
    void* texture = nullptr;
    if (!loadTexture(renderer, blank.c_str(), &texture, "bms-bga-target-viewport-640x480") || !texture) {
        LogLine("[bga-direct] target texture failed source='%s'", blank.c_str());
        return false;
    }
    int desc[31]{};
    void* surface = createSurface(renderer, desc, (int)width, (int)height, 0);
    if (!surface) {
        LogLine("[bga-direct] CreateSurface 640x480 failed");
        return false;
    }
    void* oldSurface = *reinterpret_cast<void**>(texture);
    if (oldSurface && oldSurface != surface) {
        void** vtable = *reinterpret_cast<void***>(oldSurface);
        if (vtable && vtable[2]) reinterpret_cast<DirectSurfaceRelease_t>(vtable[2])(oldSurface);
    }
    unsigned char* tex = static_cast<unsigned char*>(texture);
    *reinterpret_cast<void**>(tex) = surface;
    *reinterpret_cast<float*>(tex + 128) = (float)width;
    *reinterpret_cast<float*>(tex + 132) = (float)height;
    *reinterpret_cast<DWORD*>(tex + 136) = 0;
    *reinterpret_cast<DWORD*>(tex + 140) = 0;
    *reinterpret_cast<DWORD*>(tex + 144) = 0;
    *reinterpret_cast<float*>(tex + 148) = 1.0f;
    *reinterpret_cast<float*>(tex + 152) = 1.0f;
    *reinterpret_cast<void**>(video + 1232) = texture;
    g_directVideoTargetW = width; g_directVideoTargetH = height;
    g_directVideoTargetReady = true;
    LogLine("[bga-direct] exact viewport surface ready %lux%lu host='%s'",
            (unsigned long)width, (unsigned long)height, blank.c_str());
    return true;
}

static bool DirectVideoIs16x9(DWORD width, DWORD height) {
    if (!width || !height) return false;
    uint64_t a = (uint64_t)width * 9u, b = (uint64_t)height * 16u;
    uint64_t diff = a > b ? a - b : b - a;
    uint64_t largest = a > b ? a : b;
    return diff * 100u <= largest * 3u;
}

static bool PrepareDirectVideoObject(void* videoObject) {
    if (!videoObject) return false;
    unsigned char* video = static_cast<unsigned char*>(videoObject);
    DWORD sourceW = *reinterpret_cast<DWORD*>(video + 0x10);
    DWORD sourceH = *reinterpret_cast<DWORD*>(video + 0x14);
    if (!sourceW || !sourceH || sourceW > 16384 || sourceH > 16384) return false;
    if (!LoadDirectVideoTarget(videoObject, 640, 480)) return false;
    g_directVideoObject = videoObject;
    g_directVideoCropX = g_directVideoCropY = 0;
    g_directVideoCropW = sourceW; g_directVideoCropH = sourceH;
    const char* layout = "STRETCH";
    if (DirectVideoIs16x9(sourceW, sourceH)) {
        DWORD cropW = (DWORD)((uint64_t)sourceH * 640u / 480u);
        if (cropW > sourceW) cropW = sourceW;
        if (!cropW) cropW = 1;
        g_directVideoCropX = (sourceW - cropW) / 2;
        g_directVideoCropW = cropW;
        layout = "CENTER";
    }
    g_directVideoValidFrames = 0;
    g_directVideoFirstFrameLogged = false;
    g_directVideoFrameFailures = 0;
    g_directVideoDelayArmed = false;
    g_directVideoStarted = g_directVideoStartMs == 0;
    g_directVideoDelayClock = 0;
    LogLine("[bga-direct] layout=%s source=%lux%lu crop=%lu,%lu %lux%lu target=640x480",
            layout, (unsigned long)sourceW, (unsigned long)sourceH,
            (unsigned long)g_directVideoCropX, (unsigned long)g_directVideoCropY,
            (unsigned long)g_directVideoCropW, (unsigned long)g_directVideoCropH);
    return true;
}

static bool UploadFfmpegVideoFrame(void* videoObject) {
    static const DWORD width = 640, height = 480, bytes = width * height * 3;
    if (!EnsureDirectVideoFrameBuffer(bytes) ||
        !ReadDirectBmsFfmpegFrame(g_directVideoFrameBuffer, bytes)) return false;
    void* texture = *reinterpret_cast<void**>(static_cast<unsigned char*>(videoObject) + 1232);
    void* surface = texture ? *reinterpret_cast<void**>(texture) : nullptr;
    if (!surface) return false;
    if (!UploadPixelsToSurface(surface, g_directVideoFrameBuffer, (int)width, (int)height, 24, (int)width * 3, true,
                               0, 0, (int)width, (int)height, (int)width, (int)height)) return false;
    ++g_directVideoValidFrames;
    if (!g_directVideoFirstFrameLogged) {
        LogLine("[bga-direct] first frame backend=ffmpeg source=640x480 bits=24 target=640x480");
        g_directVideoFirstFrameLogged = true;
    }
    return true;
}

static void DrawFfmpegVideoNative(void* videoObject) {
    if (!videoObject || !g_directVideoValidFrames) return;
    unsigned char* video = static_cast<unsigned char*>(videoObject);
    void* renderer = *reinterpret_cast<void**>(video + 1104);
    void* texture = *reinterpret_cast<void**>(video + 1232);
    if (!renderer || !texture) return;
    using SetRenderState = void (__thiscall*)(void*, int, int);
    using DrawTexture = void (__thiscall*)(void*, void*, float, float, float, float, DWORD);
    reinterpret_cast<SetRenderState>(Rva(0x0a490))(renderer, 2, 1);
    reinterpret_cast<DrawTexture>(Rva(0x03f80))(renderer, texture, 0.0f, 0.0f, 640.0f, 480.0f, 0x00ffffff);
}

static void RenderFfmpegVideoFrame(void* videoObject) {
    if (!videoObject || !g_directVideoTargetReady) return;
    if (!g_directVideoStarted) {
        if (!g_directVideoDelayArmed) {
            g_directVideoDelayClock = GetTickCount();
            g_directVideoDelayArmed = true;
            if (!StartDirectBmsFfmpegVideo(g_directVideoPath)) {
                LogLine("[bga-direct] ffmpeg fallback failed source='%s'", g_directVideoPath);
                g_directVideoDisabled = true;
                return;
            }
            LogLine("[bga-direct] ffmpeg stream opened, holding frame 1 for %lu ms",
                    (unsigned long)g_directVideoStartMs);
        }
        if (GetTickCount() - g_directVideoDelayClock < g_directVideoStartMs) return;
        g_directVideoStarted = true;
        ReleaseDirectBmsFfmpegVideo();     // from here the reader paces itself in real time
        LogLine("[bga-direct] ffmpeg video started %lu ms after the stage began",
                (unsigned long)(GetTickCount() - g_directVideoDelayClock));
    }
    UploadFfmpegVideoFrame(videoObject);
    DrawFfmpegVideoNative(videoObject);
}

static int __fastcall HookDirectBackgroundLoad(void* self, void*, const char* script) {
    if (!self || !g_directVideoPath[0] || g_directVideoDisabled)
        return RealBackgroundLoad ? RealBackgroundLoad(self, script) : 0;
    unsigned char* manager = static_cast<unsigned char*>(self);
    void* videoObject = manager + 0x1c0;
    if (g_directVideoFfmpeg && g_directVideoTargetReady && videoObject == g_directVideoObject) return 1;
    auto ready = reinterpret_cast<DirectVideoReady_t>(Rva(0x0b970));
    auto resetVideo = reinterpret_cast<DirectVideoReset_t>(Rva(0x0b180));
    auto closeVideo = reinterpret_cast<DirectVideoClose_t>(Rva(0x0b2e0));
    if (ready(videoObject)) {
        if (g_directVideoTargetReady && videoObject == g_directVideoObject) return 1;
        closeVideo(videoObject);
        resetVideo(videoObject); // reconstruct COM interfaces before opening the next source
        g_directVideoObject = nullptr;
        g_directVideoTargetReady = false;
    }

    lstrcpynA(reinterpret_cast<char*>(manager + 184), g_directVideoPath, MAX_PATH);
    DWORD* useBackground = reinterpret_cast<DWORD*>(ExeBase() + 0x172ef40);
    DWORD oldUse = 0;
    bool haveUseFlag = CanWriteGamePtr(useBackground, sizeof(*useBackground));
    if (haveUseFlag) { oldUse = *useBackground; *useBackground = 1; }
    int result = RealBackgroundLoad ? RealBackgroundLoad(self, script) : 0;
    if (haveUseFlag) *useBackground = oldUse;
    bool opened = ready(videoObject) != 0;
    bool targetReady = opened && PrepareDirectVideoObject(videoObject);
    if (!g_directVideoOpenLogged || opened) {
        LogLine("[bga-direct] %s source='%s' script='%s' ready=%d target=%d startMs=%lu",
                opened && targetReady ? "opened" : "open failed", g_directVideoPath,
                script ? script : "<null>", opened ? 1 : 0, targetReady ? 1 : 0,
                (unsigned long)g_directVideoStartMs);
        g_directVideoOpenLogged = true;
    }
    if (opened && targetReady) {
        *reinterpret_cast<DWORD*>(manager + 0xa8) = 1;
        return 1;
    }
    if (!opened && LoadDirectVideoTarget(videoObject, 640, 480)) {
        g_directVideoObject = videoObject;
        g_directVideoFfmpeg = true;
        g_directVideoStarted = false;
        g_directVideoDelayArmed = false;
        g_directVideoValidFrames = 0;
        *reinterpret_cast<DWORD*>(manager + 0xa8) = 1;
        LogLine("[bga-direct] DirectShow unavailable; ffmpeg fallback prepared source='%s'", g_directVideoPath);
        return 1;
    }
    if (opened) { closeVideo(videoObject); resetVideo(videoObject); }
    g_directVideoDisabled = true;
    g_directVideoObject = nullptr;
    g_directVideoTargetReady = false;
    return result;
}

static void DrawDirectVideoNative(void* self) {
    if (!self || !g_directVideoValidFrames || !RealVideoFrame) return;
    unsigned char* video = static_cast<unsigned char*>(self);
    *reinterpret_cast<DWORD*>(video + 0x428) = 0;
    *reinterpret_cast<DWORD*>(video + 0x42c) = 0;
    *reinterpret_cast<float*>(video + 0x430) = 640.0f;
    *reinterpret_cast<float*>(video + 0x434) = 480.0f;
    DWORD state = *reinterpret_cast<DWORD*>(video);
    *reinterpret_cast<DWORD*>(video) = 1; // gateway performs only the native draw phase
    RealVideoFrame(self);
    *reinterpret_cast<DWORD*>(video) = state;
}

static void RenderDirectVideoFrame(void* self) {
    if (!self || self != g_directVideoObject || !g_directVideoTargetReady) return;
    unsigned char* video = static_cast<unsigned char*>(self);
    if (!*reinterpret_cast<DWORD*>(video + 1080)) return;
    void* texture = *reinterpret_cast<void**>(video + 1232);
    if (!*reinterpret_cast<void**>(video + 1104) || !texture) return;

    DWORD startMs = g_directVideoStartMs;
    if (!g_directVideoStarted && startMs) {
        auto stopVideo = reinterpret_cast<DirectVideoStop_t>(Rva(0x0b8f0));
        auto playVideo = reinterpret_cast<DirectVideoPlay_t>(Rva(0x0b8b0));
        if (!g_directVideoDelayArmed) {
            stopVideo(self);
            g_directVideoDelayClock = GetTickCount();
            g_directVideoDelayArmed = true;
            LogLine("[bga-direct] sync delay armed %lu ms", (unsigned long)startMs);
        }
        if (GetTickCount() - g_directVideoDelayClock < startMs) return;
        playVideo(self, 0);
        g_directVideoStarted = true;
        LogLine("[bga-direct] video started after BGA delay");
    } else if (!g_directVideoStarted) {
        g_directVideoStarted = true;
    }

    if (!*reinterpret_cast<DWORD*>(video)) {
        DWORD bytes = *reinterpret_cast<DWORD*>(video + 1224);
        void* grabber = *reinterpret_cast<void**>(video + 1120);
        if (grabber && EnsureDirectVideoFrameBuffer(bytes)) {
            void** grabberVtable = *reinterpret_cast<void***>(grabber);
            auto getFrame = reinterpret_cast<DirectVideoGetFrame_t>(grabberVtable[7]);
            DWORD actual = bytes;
            HRESULT hr = getFrame(grabber, &actual, g_directVideoFrameBuffer);
            if (SUCCEEDED(hr)) {
                unsigned char* media = *reinterpret_cast<unsigned char**>(video + 1212);
                BITMAPINFOHEADER* source = media ? reinterpret_cast<BITMAPINFOHEADER*>(media + 48) : nullptr;
                void* surface = *reinterpret_cast<void**>(texture);
                if (source && surface) {
                    LONG rawW = source->biWidth, rawH = source->biHeight;
                    uint64_t absW = rawW < 0 ? (uint64_t)(-(int64_t)rawW) : (uint64_t)rawW;
                    uint64_t absH = rawH < 0 ? (uint64_t)(-(int64_t)rawH) : (uint64_t)rawH;
                    DWORD sourceW = absW <= 16384 ? (DWORD)absW : 0;
                    DWORD sourceH = absH <= 16384 ? (DWORD)absH : 0;
                    WORD bits = source->biBitCount;
                    uint64_t stride64 = (((uint64_t)sourceW * bits + 31u) / 32u) * 4u;
                    uint64_t expected64 = stride64 * sourceH;
                    if (sourceW && sourceH && (bits == 24 || bits == 32) &&
                        source->biCompression == BI_RGB && expected64 <= 0xffffffffu && actual >= expected64) {
                        {
                            DWORD cropX = g_directVideoCropX, cropY = g_directVideoCropY;
                            DWORD cropW = g_directVideoCropW ? g_directVideoCropW : sourceW;
                            DWORD cropH = g_directVideoCropH ? g_directVideoCropH : sourceH;
                            if (cropX + cropW > sourceW) { cropX = 0; cropW = sourceW; }
                            if (cropY + cropH > sourceH) { cropY = 0; cropH = sourceH; }
                            // DIB rows: a positive height is bottom-up.
                            const bool copied = UploadPixelsToSurface(surface, g_directVideoFrameBuffer, (int)sourceW, (int)sourceH,
                                (int)bits, (int)stride64, rawH < 0, (int)cropX, (int)cropY, (int)cropW, (int)cropH,
                                (int)g_directVideoTargetW, (int)g_directVideoTargetH);
                            if (copied) {
                                ++g_directVideoValidFrames;
                                if (!g_directVideoFirstFrameLogged) {
                                    LogLine("[bga-direct] first frame source=%lux%lu bits=%u bytes=%lu target=640x480 crop=%lu,%lu %lux%lu",
                                            (unsigned long)sourceW, (unsigned long)sourceH, (unsigned)bits,
                                            (unsigned long)actual, (unsigned long)cropX, (unsigned long)cropY,
                                            (unsigned long)cropW, (unsigned long)cropH);
                                    g_directVideoFirstFrameLogged = true;
                                }
                            } else if (g_directVideoFrameFailures++ < 3) {
                                LogLine("[bga-direct] surface upload failed");
                            }
                        }
                    } else if (g_directVideoFrameFailures++ < 3) {
                        LogLine("[bga-direct] invalid RGB frame %lux%lu bits=%u actual=%lu expected=%lu",
                                (unsigned long)sourceW, (unsigned long)sourceH, (unsigned)bits,
                                (unsigned long)actual, (unsigned long)expected64);
                    }
                }
            } else if (hr == (HRESULT)0x80040227 /* VFW_E_WRONG_STATE */ && g_directVideoLoop) {
                // The clip ran out. The player does not rewind on its own -- it just stops
                // handing out frames, which is why a generic clip shorter than the song left
                // its last frame frozen on screen for the rest of the play. Restart it with
                // the game's own stop/play so the graph state is whatever it expects.
                // Rate-limited: if the restart itself fails, this must not spin every frame.
                const DWORD now = GetTickCount();
                if (now - g_directVideoLoopClock > 250) {
                    g_directVideoLoopClock = now;
                    auto stopVideo = reinterpret_cast<DirectVideoStop_t>(Rva(0x0b8f0));
                    auto playVideo = reinterpret_cast<DirectVideoPlay_t>(Rva(0x0b8b0));
                    stopVideo(self);
                    playVideo(self, 0);
                    if (++g_directVideoLoops <= 3)
                        LogLine("[bga-direct] generic clip ended; looped (%lu)",
                                (unsigned long)g_directVideoLoops);
                }
            } else if (g_directVideoFrameFailures++ < 3) {
                LogLine("[bga-direct] GetCurrentBuffer failed hr=0x%08lx", (unsigned long)hr);
            }
        }
    }
    DrawDirectVideoNative(self);
}

static void __fastcall HookDirectVideoFrame(void* self, void*) {
    const long long t0 = ModTimeNow();
    if (!g_directVideoDisabled && g_directVideoFfmpeg && self == g_directVideoObject) {
        RenderFfmpegVideoFrame(self);
        ModTimeAdd(kModTimeVideo, ModTimeNow() - t0);
        return;
    }
    if (!g_directVideoDisabled && g_directVideoTargetReady && self == g_directVideoObject) {
        RenderDirectVideoFrame(self);
        ModTimeAdd(kModTimeVideo, ModTimeNow() - t0);
        return;
    }
    if (RealVideoFrame) RealVideoFrame(self);
}

// ---- constant scroll speed ---------------------------------------------------------------------
//
// The game's speed is a percentage (g_speedPercent @0x1b2e890, 50..999 in steps of 25) that the
// player adjusts with the effector buttons in play, and it stays as set from song to song -- so a
// 90 BPM ballad and a 200 BPM chart scroll at the same multiplier. This keeps what the player
// actually tuned, BPM x speed, constant: each chart load reads the speed the previous song was
// left at, converts it with that song's BPM, and re-applies it for the new one. The first load of
// a session changes nothing.
static const uint32_t kSpeedPercentVar = 0x1b2e890;
static float g_selBpm = 0.0f;           // the row under the cursor, per song select frame
static float g_lastLoadBpm = 0.0f;      // the BPM the current speed was tuned against

void ConstantSpeedNoteSelection(float bpm, const char*) {
    if (bpm > 20.0f && bpm < 1000.0f) g_selBpm = bpm;
}

// What ApplyConstantSpeed would set for a chart at `bpm`, or 0 when it would leave the speed
// alone (first song of the session, or the feature off). Song select shows it on the card.
int ConstantSpeedPredict(float bpm) {
    if (!Config().constantSpeed || !(bpm > 20.0f && bpm < 1000.0f) || !(g_lastLoadBpm > 0.0f)) return 0;
    const int* speed = reinterpret_cast<const int*>(kSpeedPercentVar);
    if (IsBadReadPtr(speed, 4) || *speed < 50 || *speed > 999) return 0;
    int want = (int)((float)*speed * g_lastLoadBpm / bpm + 12.5f) / 25 * 25;
    if (want < 50) want = 50;
    if (want > 999) want = 999;
    return want;
}

// ---- judge offset ------------------------------------------------------------------------------
//
// The press time the judge compares is KEZPlayer vtable slot 22 (@0x40f450): `max(clock - 5, 1)`,
// the 5 being the developers' fixed input-latency compensation in chart ticks. [Gameplay]
// JudgeOffsetMs shifts it: positive means "my presses arrive late, count them earlier". It is
// turned into ticks at every chart load from that chart's BPM and ticks per beat, because a tick
// is 1/192 of a beat on a native chart and 1/384 on ours.
static const uint32_t kPressTimeVtableSlot = 0x887d0 + 22 * 4;
static volatile int g_judgeOffsetTicks = 0;
static float g_judgeTickMs = 0.0f;   // one engine tick of the chart now loaded, in ms

float JudgeTickMs() { return g_judgeTickMs; }
int   JudgeOffsetTicks() { return g_judgeOffsetTicks; }

static int __fastcall PressTimeWithOffset(void* player, void*) {
    const int t = *reinterpret_cast<int*>(static_cast<char*>(player) + 0x7258) - 5 - g_judgeOffsetTicks;
    return t >= 0 ? t : 1;
}

static void InstallJudgeOffset() {
    if (Config().judgeOffsetMs == 0) return;
    const uint32_t original = *reinterpret_cast<uint32_t*>(Rva(kPressTimeVtableSlot));
    if (original != reinterpret_cast<uint32_t>(Rva(0x0f450))) {
        LogLine("[judge] offset NOT installed: vtable slot 22 points elsewhere (%08lx), another patch owns it",
                (unsigned long)original);
        return;
    }
    void* fn = reinterpret_cast<void*>(&PressTimeWithOffset);
    if (WriteMemory(Rva(kPressTimeVtableSlot), &fn, 4))
        LogLine("[judge] offset hook installed: %d ms", Config().judgeOffsetMs);
}

static void UpdateJudgeOffsetForChart(void* chart) {
    if (!chart) return;
    float bpm = 0.0f; uint32_t res = 0;
    memcpy(&bpm, static_cast<const char*>(chart) + 0x8c, 4);
    memcpy(&res, static_cast<const char*>(chart) + 0x88, 4);
    if (!(bpm > 20.0f && bpm < 1000.0f) || res == 0 || res > 4096) return;
    // +0x88 on disk is ticks per measure; the loader's x4 makes that the ticks per beat the
    // engine runs at (192 native, 384 ours), so one tick lasts 60000 / (res * bpm) ms.
    g_judgeTickMs = 60000.0f / ((float)res * bpm);
    if (Config().judgeOffsetMs == 0) { g_judgeOffsetTicks = 0; return; }
    const int ticks = (int)((double)Config().judgeOffsetMs / g_judgeTickMs + (Config().judgeOffsetMs > 0 ? 0.5 : -0.5));
    g_judgeOffsetTicks = ticks;
    LogLine("[judge] offset %d ms = %d ticks at %.1f BPM, %lu ticks/beat (%.3f ms/tick)",
            Config().judgeOffsetMs, ticks, bpm, (unsigned long)res, g_judgeTickMs);
}

static void ApplyConstantSpeed(void* chart, bool custom) {
    if (!Config().constantSpeed || !chart) return;
    float bpm = 0.0f;
    memcpy(&bpm, reinterpret_cast<const char*>(chart) + 0x8c, 4);   // EzChart::f8c
    // f8c is the chart's OPENING tempo, which some charts leave after a measure. For our own
    // charts the loader has already worked out the one the song actually runs at.
    if (custom) {
        const float dominant = DirectBmsDominantBpm();
        if (dominant > 20.0f && dominant < 1000.0f) bpm = dominant;
    }
    if (!(bpm > 20.0f && bpm < 1000.0f)) bpm = g_selBpm;
    if (!(bpm > 20.0f && bpm < 1000.0f)) return;
    int* speed = reinterpret_cast<int*>(kSpeedPercentVar);
    if (IsBadReadPtr(speed, 4) || IsBadWritePtr(speed, 4)) return;
    const int before = *speed;
    if (g_lastLoadBpm > 0.0f && before >= 50 && before <= 999) {
        const float effective = (float)before * g_lastLoadBpm;      // percent x BPM, the tuned value
        int want = (int)(effective / bpm + 12.5f) / 25 * 25;
        if (want < 50) want = 50;
        if (want > 999) want = 999;
        if (want != before) *speed = want;
        LogLine("[speed] %s bpm %.1f -> %.1f: speed %d%% -> %d%% (keeps %.0f BPM-equivalent)",
                custom ? "custom" : "native", g_lastLoadBpm, bpm, before, want, effective / 100.0f);
    }
    g_lastLoadBpm = bpm;
}

// The EZ column publishes the same plain name as NM (decomp songselecttier.cpp m434de0 maps
// readout 0 to the bare name), so the column cursor g_4abd14 == 0 is what tells them apart at
// chart load. The request gets "-ez" before its extension for our own chart pick, and the game's
// text slot -- which its .ini load and rank files are built from next -- is rewritten to match.
static std::string EzRequestFor(const char* request) {
    std::string r = request ? request : "";
    if (*reinterpret_cast<int*>(0x4abd14) != 0) return r;
    std::string low = LowerAscii(r);
    if (low.find("-hd") != std::string::npos || low.find("-shd") != std::string::npos ||
        low.find("-ex") != std::string::npos || low.find("-ez") != std::string::npos) return r;
    size_t dot = r.rfind('.');
    if (dot == std::string::npos) return r;
    return r.substr(0, dot) + "-ez" + r.substr(dot);
}

static void PublishEzTextSlot(const char* request) {
    // g_textSlots[g_elemCount] (decomp modename.cpp): 32 x 128 at 0x1b2ec18, index at 0x1b2e718.
    const uint32_t elem = *reinterpret_cast<uint32_t*>(0x1b2e718);
    if (elem >= 32) return;
    char* slot = reinterpret_cast<char*>(0x1b2ec18 + elem * 128);
    std::string r = request ? request : "";
    size_t dash = LowerAscii(r).find("1p-");
    size_t dot = r.rfind('.');
    if (dash == std::string::npos || dot == std::string::npos || dot <= dash + 3) return;
    std::string name = r.substr(dash + 3, dot - dash - 3);      // "<id>-ez"
    lstrcpynA(slot, name.c_str(), 127);
    LogLine("[ez] column chosen: text slot -> '%s'", slot);
}

static int __fastcall HookSongPackage(void* self, void*, const char* request) {
    ResetDirectBmsVideo();
    ResetDirectBmsVideoRuntime();
    const std::string ezRequest = EzRequestFor(request);
    const bool ezChosen = ezRequest != (request ? request : "");
    if (LoadCustomBmsDirect(self, ezChosen ? ezRequest.c_str() : request)) {
        if (ezChosen) PublishEzTextSlot(ezRequest.c_str());
        ApplyConstantSpeed(self, true);
        UpdateJudgeOffsetForChart(self);
        const std::string& videoPath = DirectBmsVideoPath();
        if (!videoPath.empty()) {
            lstrcpynA(g_directVideoPath, videoPath.c_str(), MAX_PATH);
            g_directVideoStartMs = DirectBmsVideoStartMs();
            g_directVideoLoop = DirectBmsVideoIsFallback();
            g_directVideoLoopClock = 0;
            g_directVideoLoops = 0;
        }
        return 1;
    }
    ResetDirectBmsVideo();
    ResetDirectBmsVideoRuntime();
    const int r = RealSongPackage ? RealSongPackage(self, request) : 0;
    if (r) { ApplyConstantSpeed(self, false); UpdateJudgeOffsetForChart(self); }
    return r;
}

static bool InstallDirectBmsHook() {
    void* target = Rva(0x11720);
    const unsigned char songPackage[] = {0x81,0xec,0x10,0x03,0x00,0x00};
    const unsigned char clearSamples[] = {0x57,0x8b,0xf9,0x83,0x7f,0x04};
    const unsigned char loadSample[]   = {0x83,0xec,0x14,0x53,0x8b,0x5c};
    const unsigned char createSample[] = {0x6a,0xff,0x68,0xeb,0x40,0x48};
    const unsigned char registerSample[]={0x53,0x8b,0x5c,0x24,0x08,0x56};
    const unsigned char convertEvents[]= {0x53,0x8b,0x5c,0x24,0x14,0x85};
    const unsigned char finalize[]     = {0x83,0xec,0x10,0x8b,0x44,0x24};
    const unsigned char background[]   = {0x6a,0xff,0x68,0xa8,0x44,0x48,0x00};
    const unsigned char videoFrame[]   = {0x51,0x56,0x8b,0xf1,0x83,0xbe,0x38,0x04,0x00,0x00,0x00};
    const unsigned char loadTexture[]  = {0x81,0xec,0x8c,0x01,0x00,0x00};
    const unsigned char createSurface[]= {0x81,0xec,0x40,0x01,0x00,0x00};
    const unsigned char videoReset[]   = {0x0f,0x57,0xc0,0x53,0x33,0xdb,0x55,0x56,0x8b,0xf1};
    const unsigned char videoClose[]   = {0x56,0x8b,0xf1,0x83,0xbe,0x38,0x04,0x00,0x00,0x00};
    const unsigned char videoPlay[]    = {0x8b,0x44,0x24,0x04,0x56,0x8b,0xf1};
    const unsigned char videoStop[]    = {0x56,0x8b,0xf1,0x8b,0x86,0x58,0x04,0x00,0x00};
    const unsigned char videoReady[]   = {0x8b,0x81,0x38,0x04,0x00,0x00,0xc3};
    const unsigned char setRenderState[]={0x8b,0x54,0x24,0x04,0x56,0x8b,0xf1};
    const unsigned char drawTexture[]  = {0x56,0x8b,0xf1,0x8b,0x4c,0x24,0x08};
    struct Signature { uintptr_t rva; const unsigned char* bytes; size_t size; };
    const Signature signatures[] = {
        {0x11720, songPackage, sizeof(songPackage)}, {0x11050, clearSamples, sizeof(clearSamples)},
        {0x0e3c0, loadSample, sizeof(loadSample)}, {0x0e910, createSample, sizeof(createSample)},
        {0x0dea0, registerSample, sizeof(registerSample)}, {0x10fd0, convertEvents, sizeof(convertEvents)},
        {0x11310, finalize, sizeof(finalize)}, {0x1b900, background, sizeof(background)},
        {0x0b710, videoFrame, sizeof(videoFrame)}, {0x08d00, loadTexture, sizeof(loadTexture)},
        {0x07450, createSurface, sizeof(createSurface)}, {0x0b180, videoReset, sizeof(videoReset)},
        {0x0b2e0, videoClose, sizeof(videoClose)},
        {0x0b8b0, videoPlay, sizeof(videoPlay)}, {0x0b8f0, videoStop, sizeof(videoStop)},
        {0x0b970, videoReady, sizeof(videoReady)},
        {0x0a490, setRenderState, sizeof(setRenderState)}, {0x03f80, drawTexture, sizeof(drawTexture)}
    };
    for (const auto& signature : signatures) {
        void* address = Rva(signature.rva);
        if (!CanReadGamePtr(address, signature.size) || memcmp(address, signature.bytes, signature.size) != 0) {
            LogLine("[direct] executable profile mismatch rva=0x%05lx; using disk fallback",
                    (unsigned long)signature.rva);
            return false;
        }
    }
    memcpy(g_directBmsPatch.original, target, sizeof(songPackage));
    g_directBmsPatch.target = target;
    g_directBmsPatch.length = sizeof(songPackage);
    if (!BuildSimpleGateway(target, sizeof(songPackage), g_directBmsPatch.original, &g_directBmsGateway))
        return false;
    void* backgroundTarget = Rva(0x1b900);
    void* videoFrameTarget = Rva(0x0b710);
    if (!BuildSimpleGateway(backgroundTarget, sizeof(background), background, &g_directBackgroundGateway) ||
        !BuildSimpleGateway(videoFrameTarget, sizeof(videoFrame), videoFrame, &g_directVideoFrameGateway))
        return false;
    RealSongPackage = reinterpret_cast<SongPackage_t>(g_directBmsGateway);
    RealBackgroundLoad = reinterpret_cast<BackgroundLoad_t>(g_directBackgroundGateway);
    RealVideoFrame = reinterpret_cast<VideoFrame_t>(g_directVideoFrameGateway);

    bool backgroundOk = InstallJmp(backgroundTarget, reinterpret_cast<void*>(&HookDirectBackgroundLoad),
                                   g_directBackgroundPatch, sizeof(background));
    bool frameOk = backgroundOk && InstallJmp(videoFrameTarget, reinterpret_cast<void*>(&HookDirectVideoFrame),
                                               g_directVideoFramePatch, sizeof(videoFrame));
    bool songOk = frameOk && InstallJmp(target, reinterpret_cast<void*>(&HookSongPackage),
                                        g_directBmsPatch, sizeof(songPackage));
    if (!songOk) {
        if (g_directVideoFramePatch.installed) RemoveJmp(g_directVideoFramePatch);
        if (g_directBackgroundPatch.installed) RemoveJmp(g_directBackgroundPatch);
        if (g_directBmsPatch.installed) RemoveJmp(g_directBmsPatch);
    }
    LogLine("[direct] BMS chart/audio + native BGA hooks ok=%d song=%p background=%p frame=%p",
            songOk ? 1 : 0, g_directBmsGateway, g_directBackgroundGateway, g_directVideoFrameGateway);
    return songOk;
}

static void* __cdecl HookGameFsOpen(const char* name, const char* mode) {
    g_redirectForCrt = true;
    std::string redirected = MaybeRedirectCustomPath(name);
    g_redirectForCrt = false;
    if (redirected == "\x01ram") { g_pendingVirtualBytes.clear(); redirected.clear(); }

    // v10.6 hybrid mode: support both native encrypted .ezi and already-plaintext .ezz.
    // If the cache .ezz is plaintext EZI-map text, protect it from FNEX's encrypted-.ezi
    // unpack step. If no plaintext .ezz exists, allow the native write so encrypted .ezi
    // files can still be converted by the game.
    if (!redirected.empty() && EndsWithCaseInsensitive(redirected, ".ezz") && ModeLooksWritable(mode)) {
        if (IsPlaintextEziMapFile(redirected)) {
            std::string discard = redirected + ".discard.tmp";
            LogLine("[custom] hybrid ezz: plaintext detected, divert write '%s' -> '%s' mode='%s'", redirected.c_str(), discard.c_str(), mode ? mode : "<null>");
            return RealGameFsOpen ? RealGameFsOpen(discard.c_str(), mode) : nullptr;
        }
        LogLine("[custom] hybrid ezz: no plaintext cache, allow native write '%s' mode='%s'", redirected.c_str(), mode ? mode : "<null>");
    }

    // v26 bypass: if a pre-written plaintext .es exists in cache, protect it from the game's
    // .ez decryptor (FUN_00410a40) which would overwrite it with a bad decryption if the key
    // doesn't match. FUN_00411460 will then open our pre-written .es directly.
    if (!redirected.empty() && EndsWithCaseInsensitive(redirected, ".es") && ModeLooksWritable(mode)) {
        if (LocalFileExists(redirected)) {
            std::string discard = redirected + ".discard.tmp";
            LogLine("[custom] hybrid es: pre-written es protected, divert write '%s' -> discard", redirected.c_str());
            return RealGameFsOpen ? RealGameFsOpen(discard.c_str(), mode) : nullptr;
        }
    }

    const char* finalName = redirected.empty() ? name : redirected.c_str();
    if (!redirected.empty()) {
        LogLine("[custom] fsopen redirect '%s' -> '%s' mode='%s'", name ? name : "<null>", finalName ? finalName : "<null>", mode ? mode : "<null>");
    }
    return RealGameFsOpen ? RealGameFsOpen(finalName, mode) : nullptr;
}

static bool InstallGameFsOpenHook() {
    // FUN_0047960e is the game's CRT fopen wrapper:
    //   __fsopen(path, mode, 0x40)
    // Hooking it fixes redirects for .ezi/.ezz/.ez/.ini because these loaders use FILE* APIs, not only CreateFileA.
    void* target = Rva(0x7960e);
    JumpPatch patch{};
    const size_t len = 10; // push 40; push [esp+0c]; push [esp+0c]
    if (!target) return false;
    memcpy(patch.original, target, len);
    patch.target = target;
    patch.length = len;
    if (!BuildSimpleGateway(target, len, patch.original, &g_fsopenGateway)) {
        LogLine("[custom] fsopen gateway failed");
        return false;
    }
    RealGameFsOpen = reinterpret_cast<GameFsOpen_t>(g_fsopenGateway);
    bool ok = InstallJmp(target, reinterpret_cast<void*>(&HookGameFsOpen), patch, len);
    LogLine("[custom] fsopen hook FUN_0047960e ok=%d gateway=%p", ok ? 1 : 0, g_fsopenGateway);
    return ok;
}

// FUN_00404950 is the game's generic textured-quad draw (thiscall ecx=gfx ctx, arg0=texture ptr,
// then 10 dwords of args → `ret 0x28`). At >~2000 songs the wheel hands it a NULL disc-texture ptr
// (the texture budget filled at ~656 surfaces, so high songs never got a disc loaded) → it derefs
// [ptr+0x80] at 0x404972 → access violation on category change. Guard it: if the texture ptr is
// null/garbage, skip the draw (mimic the function's own `ret 0x28`). Drawing a null texture is always
// a bug, so this is safe game-wide; songs past the texture budget just show a blank disc, no crash.
// ponytail: real art for those needs lazy/windowed disc loading — separate, bigger change.
static void* g_drawGateway = nullptr;
static JumpPatch g_drawGuardPatch;
static bool InstallDrawNullGuard() {
    void* target = Rva(0x4950);
    if (!target) return false;
    const size_t len = 6; // movss xmm5,[esp+0x10]
    memcpy(g_drawGuardPatch.original, target, len);
    g_drawGuardPatch.target = target;
    g_drawGuardPatch.length = len;
    if (!BuildSimpleGateway(target, len, g_drawGuardPatch.original, &g_drawGateway)) return false;
    unsigned char* stub = static_cast<unsigned char*>(AllocExecLocal(32));
    if (!stub) return false;
    unsigned char* p = stub;
    EmitU8Local(p, 0x8B); EmitU8Local(p, 0x44); EmitU8Local(p, 0x24); EmitU8Local(p, 0x04); // mov eax,[esp+4] (arg0=texptr)
    EmitU8Local(p, 0x3D); EmitU32Local(p, 0x00010000);                                       // cmp eax,0x10000
    EmitU8Local(p, 0x73); EmitU8Local(p, 0x03);                                               // jae +3 (valid → draw)
    EmitU8Local(p, 0xC2); EmitU8Local(p, 0x28); EmitU8Local(p, 0x00);                         // ret 0x28 (skip)
    EmitU8Local(p, 0xE9);                                                                     // jmp gateway (original)
    EmitU32Local(p, static_cast<unsigned long>((intptr_t)g_drawGateway - (intptr_t)(p + 4)));
    bool ok = InstallJmp(target, stub, g_drawGuardPatch, len);
    LogLine("[drawguard] FUN_00404950 null-tex guard ok=%d gateway=%p stub=%p", ok ? 1 : 0, g_drawGateway, (void*)stub);
    return ok;
}

// sub_407A50 (RVA 0x7a50) renders text into a texture object passed as arg4 ([esp+0x10] at entry; the
// function is __stdcall with 4 dwords of args → `ret 0x10`). At >~3000 songs, when the KResourceContainer
// texture pool overflows ("Exceed pool size"), a text-texture object up the call stack comes back NULL and
// is passed down as arg4 → 0x407a8d `movss [esi+0x80],xmm0` writes to [null+0x80] → access violation on
// 5Key SongSelect entry. Guard it: if arg4 is null/garbage, return null (eax=0), which is exactly the
// engine's own create-failed value — the caller does `mov esi,eax; ...; mov eax,esi; ret`, propagating the
// null as a return value with no deref, so this is safe. Missing text (a bpm digit) instead of a crash.
static void* g_textGateway = nullptr;
static JumpPatch g_textGuardPatch;
static bool InstallTextRenderNullGuard() {
    void* target = Rva(0x7a50);
    if (!target) return false;
    const size_t len = 6; // sub esp,0x90
    memcpy(g_textGuardPatch.original, target, len);
    g_textGuardPatch.target = target;
    g_textGuardPatch.length = len;
    if (!BuildSimpleGateway(target, len, g_textGuardPatch.original, &g_textGateway)) return false;
    unsigned char* stub = static_cast<unsigned char*>(AllocExecLocal(32));
    if (!stub) return false;
    unsigned char* p = stub;
    EmitU8Local(p, 0x8B); EmitU8Local(p, 0x44); EmitU8Local(p, 0x24); EmitU8Local(p, 0x10); // mov eax,[esp+0x10] (arg4=dest tex obj)
    EmitU8Local(p, 0x3D); EmitU32Local(p, 0x00010000);                                       // cmp eax,0x10000
    EmitU8Local(p, 0x73); EmitU8Local(p, 0x05);                                               // jae +5 (valid → render)
    EmitU8Local(p, 0x33); EmitU8Local(p, 0xC0);                                               // xor eax,eax (return null)
    EmitU8Local(p, 0xC2); EmitU8Local(p, 0x10); EmitU8Local(p, 0x00);                         // ret 0x10 (skip)
    EmitU8Local(p, 0xE9);                                                                     // jmp gateway (original)
    EmitU32Local(p, static_cast<unsigned long>((intptr_t)g_textGateway - (intptr_t)(p + 4)));
    bool ok = InstallJmp(target, stub, g_textGuardPatch, len);
    LogLine("[textguard] sub_407A50 null-dest guard ok=%d gateway=%p stub=%p", ok ? 1 : 0, g_textGateway, (void*)stub);
    return ok;
}

// Category-wheel slot render (FUN_00432200 @0x4323c0). It fetches the slot's texture object
// esi = [ecx + idx*0x64*4 + 0x130] and, at 0x4323fc, derefs [esi+0x84]/[esi+0x80] to draw it. On a
// category with songs whose disc texture never loaded (e.g. a preview/audio miss → no disc), that slot
// pointer is NULL → AV on category change. Guard at the deref: if esi is null/garbage, skip the whole
// draw block and jump to the function epilogue (0x4324a2 `pop esi; ret 0x1c`) — stack is balanced there
// (only the prologue's `push esi` is pending), so the slot just renders blank instead of crashing.
static void* g_wheelGateway = nullptr;
static JumpPatch g_wheelGuardPatch;
static bool InstallCategoryWheelGuard() {
    void* target = Rva(0x323fc);             // movss xmm2,[esi+0x84] (8 bytes)
    if (!target) return false;
    const size_t len = 8;
    memcpy(g_wheelGuardPatch.original, target, len);
    g_wheelGuardPatch.target = target;
    g_wheelGuardPatch.length = len;
    if (!BuildSimpleGateway(target, len, g_wheelGuardPatch.original, &g_wheelGateway)) return false;
    unsigned char* stub = static_cast<unsigned char*>(AllocExecLocal(32));
    if (!stub) return false;
    unsigned char* p = stub;
    EmitU8Local(p, 0x81); EmitU8Local(p, 0xFE); EmitU32Local(p, 0x00010000);                 // cmp esi,0x10000
    EmitU8Local(p, 0x73); EmitU8Local(p, 0x05);                                               // jae +5 (valid → draw)
    EmitU8Local(p, 0xE9); EmitU32Local(p, (unsigned long)((intptr_t)Rva(0x324a2) - (intptr_t)(p + 4))); // jmp epilogue (pop esi; ret 0x1c)
    EmitU8Local(p, 0xE9);                                                                     // jmp gateway (original movss)
    EmitU32Local(p, static_cast<unsigned long>((intptr_t)g_wheelGateway - (intptr_t)(p + 4)));
    bool ok = InstallJmp(target, stub, g_wheelGuardPatch, len);
    LogLine("[wheelguard] FUN_00432200 null-slot guard ok=%d gateway=%p stub=%p", ok ? 1 : 0, g_wheelGateway, (void*)stub);
    return ok;
}

// Texture-pool overflow guard (no crash, NO surface leak). The KResourceContainer bump allocator, when
// its fixed pool is full, returns NULL → the tree-insert null-derefs at 0x41455b → crash. Flushing the
// pool "works" but LEAKS DDraw surfaces (each reload makes a new one → 20k+ surfaces / GBs → textures
// stop loading). The right fix: when full, DON'T load a new texture at all — return null, let the draw
// guard render blank. Two patches: (A) the tree-insert (0x414554): if the pool-alloc returned NULL, bail
// to the function epilogue returning 0 instead of deref-crashing. (B) the loader (0x408d81): if the
// insert returned 0, skip the texture-load call and jump to the game's own "load failed" path (0x408de0)
// — no new DDraw surface. Result: songs past the pool's capacity show blank art but never crash/leak.
// NOTE: an earlier "loader guard" at 0x408d81 (route null insert → 0x408de0) was REMOVED: 0x8de0 is NOT
// a fail path, it's the missing-file text-placeholder renderer and it derefs the (null) entry → it traded
// one crash for another. The real fix was the ROOT CAUSE: the overflow was a SECOND static
// KTextureContainer (ctor @0x4865a0, object 0xd49c98) with a 1MB pool = ~4370 texture entries (0xf0 each)
// — full at ~3100 songnames + native UI. EXE-patched to 16MB (file offset 0x865a1) = ~70k textures.
static JumpPatch g_insertGuardPatch;
static bool patchJmpStub(uintptr_t targetRva, size_t len, unsigned char* stub, JumpPatch& patch) {
    void* target = Rva(targetRva);
    if (!target) return false;
    memcpy(patch.original, target, len);
    patch.target = target; patch.length = len;
    return InstallJmp(target, stub, patch, len);
}
static bool InstallPoolOverflowGuard() {
    // (A) insert guard @0x414554 (overwrite 7 bytes: mov ecx,[esp+0x14]; lea esi,[eax+8])
    unsigned char* s1 = static_cast<unsigned char*>(AllocExecLocal(32)); if (!s1) return false;
    unsigned char* p = s1;
    EmitU8Local(p, 0x85); EmitU8Local(p, 0xC0);                                        // test eax,eax
    EmitU8Local(p, 0x0F); EmitU8Local(p, 0x84); EmitU32Local(p, (unsigned long)((intptr_t)Rva(0x1456e) - (intptr_t)(p + 4))); // jz epilogue (ret 0)
    EmitU8Local(p, 0x8B); EmitU8Local(p, 0x4C); EmitU8Local(p, 0x24); EmitU8Local(p, 0x14); // mov ecx,[esp+0x14]
    EmitU8Local(p, 0x8D); EmitU8Local(p, 0x70); EmitU8Local(p, 0x08);                  // lea esi,[eax+8]
    EmitU8Local(p, 0xE9); EmitU32Local(p, (unsigned long)((intptr_t)Rva(0x1455b) - (intptr_t)(p + 4))); // jmp back
    bool a = patchJmpStub(0x14554, 7, s1, g_insertGuardPatch);
    LogLine("[poolguard] insert-guard=%d (pool-full -> null, no crash inside insert)", a);
    return a;
}

// The two KProfile arenas (decomp kprofile.cpp / loadtexture.cpp / loadstr.cpp), both bump
// allocators reset by KProfile::init at every screen teardown, both sized by a `push imm32` in the
// static-ctor thunk of EZ2B.exe:
//   0xd49c98  texture OBJECTS, 0xf0 bytes each; pixels live in DirectDraw surfaces, not here.
//             file 0x865a1: 1MB -> 16MB (~70k textures) -- this is the one the songnames filled.
//   0x530c2c  .str animation CLIPS, 0x2b210 + file size each; the biggest native BGA needs 18MB
//             and customs use none. file 0x86561: was raised to 512MB on the belief it held
//             pixels; that made 512MB resident (init memsets the whole arena) for nothing. Now 64MB.

bool g_songInjectRan = false;  // set by the song inject; gates the tracer arm to the load window
static void MaybeArmTracerOnCredits(LPCSTR name) {
    // Arm the execution tracer on the LAST credits file (Credits_mask) — it loads right before the
    // >~2300 abort, so the single-step trace window is short (fast). Only after the inject (post mode
    // select), so a boot-time credits load doesn't arm far from the abort and freeze the game.
    static bool s_armed = false;
    if (s_armed || !g_songInjectRan || !name) return;
    if (!PathContainsLower(name, "credits_mask")) return;
    HMODULE t = GetModuleHandleA("ez2tracer.dll");
    auto arm = t ? (void(*)())GetProcAddress(t, "Ez2TraceArm") : nullptr;
    LogLine("[tracer] credits arm: dll=%p Ez2TraceArm=%p", (void*)t, (void*)arm);
    if (arm) { s_armed = true; arm(); LogLine("[tracer] armed at Credits_mask (tracing to abort)"); }
}
// ---- Random BGA via DirectShow (quartz.dll) CreateFileW hook -----------------------------------
// The game plays Video\<id>\<id>.spv through quartz.dll, which opens it with ITS OWN CreateFileW (not
// the exe's IAT, so our CreateFileA hook never sees it). We hook quartz's CreateFileW import: when it
// asks for a custom song's .spv that has NO on-disk file, redirect to a RANDOM Video\_spv\ clip. No
// per-song files, chosen at play time. srand'd once so the pick is stable within a play but varies.
using CreateFileW_t = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t RealQuartzCreateFileW = nullptr;

static std::vector<std::wstring>& BgPoolW() {
    static std::vector<std::wstring> pool;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::string dir = JoinPath(JoinPath(GetExeDirectory(), "Video"), "_spv");
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(JoinPath(dir, "*.spv").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string full = JoinPath(dir, fd.cFileName);
                int n = MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, nullptr, 0);
                if (n > 0) {
                    std::wstring w((size_t)n, L'\0');
                    MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, &w[0], n);
                    w.resize((size_t)n - 1);
                    pool.push_back(w);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        LogLine("[bgpool] loaded %d shared BG clips for the quartz redirect", (int)pool.size());
    }
    return pool;
}
// If wpath is a custom song's ...\Video\<id>\<id>.spv with no real file, return a random pool clip.
static const std::wstring* MaybeRedirectSpvW(LPCWSTR wpath) {
    if (!wpath || DirectBmsEnabled()) return nullptr;
    std::wstring lw(wpath);
    for (auto& c : lw) c = (wchar_t)towlower(c);
    size_t vp = lw.find(L"\\video\\");
    if (vp == std::wstring::npos || lw.size() < 4 || lw.compare(lw.size() - 4, 4, L".spv") != 0) return nullptr;
    // extract the <id> folder between \Video\ and the trailing \...
    size_t idStart = vp + 7, slash = lw.find(L'\\', idStart);
    if (slash == std::wstring::npos) return nullptr;
    std::wstring wid = lw.substr(idStart, slash - idStart);
    // narrow the id to match against song ids (ascii bmsNNNNNN)
    char id[64] = {}; WideCharToMultiByte(CP_ACP, 0, wid.c_str(), -1, id, sizeof(id) - 1, nullptr, nullptr);
    const std::vector<CustomBmsSong>& all = GetCustomBmsSongs();
    EnsureSongIdIndex(all);
    const CustomBmsSong* song = nullptr;
    std::unordered_map<std::string, size_t>::const_iterator hit =
        g_songByIdIndex.find(LowerAscii(id));
    if (hit != g_songByIdIndex.end()) song = &all[hit->second];
    if (!song || !song->videoFiles.empty()) return nullptr;
    if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) return nullptr; // a real .spv exists → use it
    auto& pool = BgPoolW();
    if (pool.empty()) return nullptr;
    return &pool[(size_t)rand() % pool.size()];
}
static HANDLE WINAPI HookQuartzCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                           DWORD creation, DWORD flags, HANDLE templ) {
    const std::wstring* bg = MaybeRedirectSpvW(name);
    LPCWSTR finalName = bg ? bg->c_str() : name;
    if (bg && Config().traceFileApi) LogLine("[bgpool] quartz redirect no-BGA .spv -> pool clip");
    return RealQuartzCreateFileW ? RealQuartzCreateFileW(finalName, access, share, sa, creation, flags, templ)
                                 : CreateFileW(finalName, access, share, sa, creation, flags, templ);
}
// quartz.dll loads lazily on the first video. Try to hook its IAT once it's present; idempotent.
static void EnsureQuartzHook() {
    static bool done = false;
    if (done) return;
    HMODULE q = GetModuleHandleA("quartz.dll");
    if (!q) return; // not loaded yet — retried from the file hooks during gameplay
    done = true;
    bool ok = HookModuleIAT(q, "KERNEL32.dll", "CreateFileW",
                            reinterpret_cast<void*>(HookQuartzCreateFileW),
                            reinterpret_cast<void**>(&RealQuartzCreateFileW));
    LogLine("[bgpool] quartz.dll CreateFileW hook=%d", ok ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------
// In-memory assets. The ABM loader is plain Win32: CreateFileA, then GetFileSize / ReadFile /
// CloseHandle on the returned handle (decompiled, sub_413180 calls GetFileSize(this[1],0) and
// sub_413120 calls ReadFile(this[1],...)). So a category sprite never has to exist as a file --
// the DLL renders it, hands back a handle of its own, and answers those calls out of RAM.
//
// The handle is a real kernel object (an unsignalled event) rather than an invented value: unique,
// safe to pass around, and CloseHandle disposes of it normally if code we do not hook touches it.
struct VirtualAsset { std::vector<unsigned char> data; size_t pos; };
static std::map<HANDLE, VirtualAsset> g_virtualFiles;
static CRITICAL_SECTION g_virtualLock;
static bool g_virtualLockReady = false;

static void EnsureVirtualLock() {
    if (g_virtualLockReady) return;
    InitializeCriticalSection(&g_virtualLock);
    g_virtualLockReady = true;
}

// Recognise the three sprites we own and report which one was asked for. Returns the category
// index, or -1 when the path is not ours. Longest prefix first: "category_" also matches inside
// "categorybig_".
static int VirtualAssetKind(const std::string& lowerPath, size_t catCount, int& kindOut) {
    struct Entry { const char* prefix; int kind; int base; };
    static const Entry kinds[] = {
        { "categorybig_", kCatAbmOverlay, 48 },
        { "category_",    kCatAbmTab,     48 },
        { "version_",     kCatAbmLabel,   19 },
        // The two-line category-change overlay, 256x64 24bpp like the 30 shipped sortstyle_NN.
        // The game never asks for these by name -- it bulk-loads 00..29 at init and then only
        // indexes the array -- so the request comes from our own loadTex call, which numbers the
        // file after the category (47+) to keep the array index and the asset name the same thing.
        { "sortstyle_",   kCatAbmSort,    47 },
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        size_t at = lowerPath.find(kinds[i].prefix);
        if (at == std::string::npos) continue;
        int nn = atoi(lowerPath.c_str() + at + strlen(kinds[i].prefix));
        int idx = nn - kinds[i].base;
        if (idx < 0 || (size_t)idx >= catCount) return -1;
        kindOut = kinds[i].kind;
        return idx;
    }
    return -1;
}

static HANDLE OpenVirtualBytes(std::vector<unsigned char>&& bytes) {
    if (bytes.empty()) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!h) return INVALID_HANDLE_VALUE;
    EnsureVirtualLock();
    EnterCriticalSection(&g_virtualLock);
    VirtualAsset& v = g_virtualFiles[h];
    v.data.swap(bytes);
    v.pos = 0;
    LeaveCriticalSection(&g_virtualLock);
    return h;
}

static HANDLE OpenVirtualAsset(const std::string& mode, size_t index, int kind) {
    std::vector<unsigned char> bytes;
    if (!BuildCategoryAbmInMemory(mode, index, kind, bytes) || bytes.empty())
        return INVALID_HANDLE_VALUE;
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!h) return INVALID_HANDLE_VALUE;
    EnsureVirtualLock();
    EnterCriticalSection(&g_virtualLock);
    VirtualAsset& v = g_virtualFiles[h];
    v.data.swap(bytes);
    v.pos = 0;
    LeaveCriticalSection(&g_virtualLock);
    return h;
}

typedef DWORD (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef BOOL  (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL  (WINAPI *CloseHandle_t)(HANDLE);
typedef DWORD (WINAPI *SetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
static GetFileSize_t RealGetFileSize = nullptr;
static ReadFile_t RealReadFile = nullptr;
static CloseHandle_t RealCloseHandle = nullptr;
static SetFilePointer_t RealSetFilePointer = nullptr;

static VirtualAsset* FindVirtualLocked(HANDLE h) {
    std::map<HANDLE, VirtualAsset>::iterator it = g_virtualFiles.find(h);
    return it == g_virtualFiles.end() ? nullptr : &it->second;
}

static DWORD WINAPI HookGetFileSize(HANDLE h, LPDWORD hi) {
    if (g_virtualLockReady) {
        EnterCriticalSection(&g_virtualLock);
        VirtualAsset* v = FindVirtualLocked(h);
        DWORD sz = v ? (DWORD)v->data.size() : 0;
        bool ours = v != nullptr;
        LeaveCriticalSection(&g_virtualLock);
        if (ours) { if (hi) *hi = 0; return sz; }
    }
    return RealGetFileSize ? RealGetFileSize(h, hi) : GetFileSize(h, hi);
}

static BOOL WINAPI HookReadFile(HANDLE h, LPVOID buf, DWORD want, LPDWORD got, LPOVERLAPPED ov) {
    if (g_virtualLockReady) {
        EnterCriticalSection(&g_virtualLock);
        VirtualAsset* v = FindVirtualLocked(h);
        bool ours = v != nullptr;
        if (ours) {
            size_t left = v->data.size() > v->pos ? v->data.size() - v->pos : 0;
            DWORD n = (DWORD)(want < left ? want : left);
            if (n && buf) memcpy(buf, v->data.data() + v->pos, n);
            v->pos += n;
            if (got) *got = n;
        }
        LeaveCriticalSection(&g_virtualLock);
        if (ours) return TRUE;
    }
    return RealReadFile ? RealReadFile(h, buf, want, got, ov) : ReadFile(h, buf, want, got, ov);
}

static DWORD WINAPI HookSetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    if (g_virtualLockReady) {
        EnterCriticalSection(&g_virtualLock);
        VirtualAsset* v = FindVirtualLocked(h);
        bool ours = v != nullptr;
        DWORD out = 0;
        if (ours) {
            long long base = method == FILE_CURRENT ? (long long)v->pos
                           : method == FILE_END     ? (long long)v->data.size() : 0;
            long long np = base + lo;
            if (np < 0) np = 0;
            if (np > (long long)v->data.size()) np = (long long)v->data.size();
            v->pos = (size_t)np;
            out = (DWORD)np;
        }
        LeaveCriticalSection(&g_virtualLock);
        if (ours) { if (hi) *hi = 0; return out; }
    }
    return RealSetFilePointer ? RealSetFilePointer(h, lo, hi, method) : SetFilePointer(h, lo, hi, method);
}

static BOOL WINAPI HookCloseHandle(HANDLE h) {
    if (g_virtualLockReady) {
        EnterCriticalSection(&g_virtualLock);
        std::map<HANDLE, VirtualAsset>::iterator it = g_virtualFiles.find(h);
        if (it != g_virtualFiles.end()) g_virtualFiles.erase(it);
        LeaveCriticalSection(&g_virtualLock);
    }
    return RealCloseHandle ? RealCloseHandle(h) : CloseHandle(h);
}
// ---------------------------------------------------------------------------------------------

static HANDLE WINAPI HookCreateFileABody(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ);

static HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    const long long t0 = ModTimeNow();
    HANDLE h = HookCreateFileABody(name, access, share, sa, creation, flags, templ);
    ModTimeAdd(kModTimeFiles, ModTimeNow() - t0);
    return h;
}

static HANDLE WINAPI HookCreateFileABody(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    if (!DirectBmsEnabled()) EnsureQuartzHook();
    DetectActiveModeFromPath(name, "CreateFileA");
    MaybeArmTracerOnCredits(name);
    // Our own sprites never reach the filesystem: build the .abm in RAM and answer with a handle
    // the read hooks know how to serve.
    if (name && (creation == OPEN_EXISTING || creation == OPEN_ALWAYS)) {
        std::string lower = LowerAscii(name);
        // The RANDOM row's songname. It is not a song, so it has no cache directory to redirect
        // to -- the tile is built in RAM on request, exactly like the category sprites below.
        // trace_hooks asks for this once and keeps the texture handle.
        if (lower.find("bmsoverlaybg") != std::string::npos ||
            lower.find("bmsoverlayico") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0,
                                         lower.find("bmsoverlaybg") != std::string::npos
                                             ? kCatAbmOverlayBg : kCatAbmOverlayIco);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmspreviewbase") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmPreviewBase);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmspanel") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmPanel);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmsstats") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmStats);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmsgraph") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmGraph);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmslabelez") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmLabelEz);
            if (vh != INVALID_HANDLE_VALUE) return vh;
        }
        if (lower.find("bmsrandom") != std::string::npos) {
            HANDLE vh = OpenVirtualAsset(GetActiveMode(), 0, kCatAbmRandom);
            if (vh != INVALID_HANDLE_VALUE) {
                static bool logged = false;
                if (!logged) { logged = true; LogLine("[random] songname tile served from memory"); }
                return vh;
            }
        }
        if (lower.find("songselect") != std::string::npos) {
            std::string mode = GetActiveMode();
            if (mode.empty()) mode = "5keyOnly";
            std::vector<std::string> cats = GetCustomBmsCategoriesForMode(mode);
            int kind = kCatAbmTab;
            int idx = cats.empty() ? -1 : VirtualAssetKind(lower, cats.size(), kind);
            if (idx >= 0) {
                HANDLE vh = OpenVirtualAsset(mode, (size_t)idx, kind);
                if (vh != INVALID_HANDLE_VALUE) {
                    static int s_logged = 0;
                    if (s_logged < 12) { ++s_logged;
                        LogLine("[asset] served from memory: '%s' cat='%s' kind=%d", name, cats[idx].c_str(), kind); }
                    return vh;
                }
            }
        }
    }
    std::string redirected = MaybeRedirectCustomPath(name);
    if (redirected == "\x01ram") {
        HANDLE vh = OpenVirtualBytes(std::move(g_pendingVirtualBytes));
        g_pendingVirtualBytes.clear();
        if (vh != INVALID_HANDLE_VALUE) return vh;
        redirected.clear();
    }
    LPCSTR finalName = redirected.empty() ? name : redirected.c_str();
    if (Config().traceFileApi && finalName) LogLine("[file] CreateFileA '%s' access=0x%08lx creation=%lu", finalName, access, creation);
    return RealCreateFileA ? RealCreateFileA(finalName, access, share, sa, creation, flags, templ)
                           : CreateFileA(finalName, access, share, sa, creation, flags, templ);
}

static HANDLE WINAPI HookFindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
    DetectActiveModeFromPath(pattern, "FindFirstFileA");
    if (Config().traceFileApi && pattern) LogLine("[file] FindFirstFileA '%s'", pattern);
    return RealFindFirstFileA ? RealFindFirstFileA(pattern, data) : FindFirstFileA(pattern, data);
}

static BOOL WINAPI HookFindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
    BOOL ok = RealFindNextFileA ? RealFindNextFileA(h, data) : FindNextFileA(h, data);
    if (Config().traceFileApi && ok && data) LogLine("[file] FindNextFileA -> '%s'", data->cFileName);
    return ok;
}

static BOOL WINAPI HookSetCurrentDirectoryA(LPCSTR name) {
    DetectActiveModeFromPath(name, "SetCurrentDirectoryA");
    std::string redirected = MaybeRedirectCustomDirectory(name);
    LPCSTR finalName = redirected.empty() ? name : redirected.c_str();
    if (Config().traceFileApi && finalName) LogLine("[file] SetCurrentDirectoryA '%s'", finalName);
    return RealSetCurrentDirectoryA ? RealSetCurrentDirectoryA(finalName) : SetCurrentDirectoryA(finalName);
}

static void InstallFileHooks() {
    bool va = HookIAT("KERNEL32.dll", "VirtualAlloc", reinterpret_cast<void*>(HookVirtualAlloc), reinterpret_cast<void**>(&RealVirtualAlloc));
    LogLine("[valloc] VirtualAlloc IAT hook=%d real=%p", va?1:0, RealVirtualAlloc);
    bool a = HookIAT("KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(HookCreateFileA), reinterpret_cast<void**>(&RealCreateFileA));
    bool b = HookIAT("KERNEL32.dll", "FindFirstFileA", reinterpret_cast<void*>(HookFindFirstFileA), reinterpret_cast<void**>(&RealFindFirstFileA));
    bool c = HookIAT("KERNEL32.dll", "FindNextFileA", reinterpret_cast<void*>(HookFindNextFileA), reinterpret_cast<void**>(&RealFindNextFileA));
    bool d = HookIAT("KERNEL32.dll", "SetCurrentDirectoryA", reinterpret_cast<void*>(HookSetCurrentDirectoryA), reinterpret_cast<void**>(&RealSetCurrentDirectoryA));
    LogLine("[file] IAT hooks CreateFile=%d FindFirst=%d FindNext=%d SetCwd=%d realCreate=%p realSetCwd=%p", a?1:0, b?1:0, c?1:0, d?1:0, RealCreateFileA, RealSetCurrentDirectoryA);
    bool g1 = HookIAT("KERNEL32.dll", "GetFileSize", reinterpret_cast<void*>(HookGetFileSize), reinterpret_cast<void**>(&RealGetFileSize));
    bool g2 = HookIAT("KERNEL32.dll", "ReadFile", reinterpret_cast<void*>(HookReadFile), reinterpret_cast<void**>(&RealReadFile));
    bool g3 = HookIAT("KERNEL32.dll", "CloseHandle", reinterpret_cast<void*>(HookCloseHandle), reinterpret_cast<void**>(&RealCloseHandle));
    bool g4 = HookIAT("KERNEL32.dll", "SetFilePointer", reinterpret_cast<void*>(HookSetFilePointer), reinterpret_cast<void**>(&RealSetFilePointer));
    LogLine("[asset] in-memory hooks GetFileSize=%d ReadFile=%d CloseHandle=%d SetFilePointer=%d",
            g1?1:0, g2?1:0, g3?1:0, g4?1:0);
    bool ep = HookIAT("KERNEL32.dll", "ExitProcess", reinterpret_cast<void*>(HookExitProcess), reinterpret_cast<void**>(&RealExitProcess));
    bool tp = HookIAT("KERNEL32.dll", "TerminateProcess", reinterpret_cast<void*>(HookTerminateProcess), reinterpret_cast<void**>(&RealTerminateProcess));
    // Inline-hook kernel32!ExitProcess too: the game's CRT calls it NOT via the exe IAT, so the IAT
    // hook missed the error-exit. Inline catches every caller. We never return to the original
    // (HookExitProcess terminates), so no gateway/relocation needed.
    bool epi = false;
    if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
        if (void* xp = (void*)GetProcAddress(k32, "ExitProcess"))
            epi = InstallJmp(xp, reinterpret_cast<void*>(HookExitProcess), g_exitProcessInline, 5);
    }
    LogLine("[exit] hooks ExitProcess(IAT=%d inline=%d) TerminateProcess(IAT=%d) — catch error-exit", ep?1:0, epi?1:0, tp?1:0);
}

// Crash logger: on the FATAL unhandled crash, log the faulting RVA (off EZ2AC base
// 0x400000) + access address so "it crashed" becomes a precise fun_<VA> mappable in the
// decomp. SetUnhandledExceptionFilter fires only when nothing else handled it, so the
// game's heavy SEH first-chance traffic is NOT logged — only the real death.
// ponytail: re-arm each crash since the game may reset the top-level filter.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
static bool CanReadDword(const void* p, uint32_t* out) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    *out = *(const uint32_t*)p;
    return true;
}
// Walk the caller chain from a frame pointer: EBP chain first, then a raw stack scan as fallback
// (catches return addresses even with optimized/FPO frames). Logs game-code RVAs.
static void LogStackWalkFrom(void* framePtr, const char* tag) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    uint32_t fp = (uint32_t)(uintptr_t)framePtr, prev = 0;
    for (int d = 0; d < 24; ++d) {
        uint32_t ret = 0, next = 0;
        if (fp <= prev || !CanReadDword((void*)(uintptr_t)(fp + 4), &ret) || !CanReadDword((void*)(uintptr_t)fp, &next)) break;
        if (ret >= base + 0x1000 && ret < base + 0x87000)
            LogLine("[%s]   frame[%d] ret=%08lx rva=%08lx", tag, d, (unsigned long)ret, (unsigned long)(ret - base));
        prev = fp; fp = next;
    }
    // Raw stack scan: the CRT is dynamic (msvcrt.dll), so abort()'s immediate frames are outside the
    // exe — scan a wide window for the GAME's .text return addresses (the code that triggered abort).
    // VALIDATE each candidate is preceded by a CALL instruction → filters the stack-garbage noise so
    // the logged list is the real caller chain (most-recent first).
    uint32_t* sp = (uint32_t*)framePtr;
    int logged = 0;
    for (int i = 0; i < 8192 && logged < 40; ++i) {
        uint32_t v = 0;
        if (!CanReadDword(sp + i, &v)) break;
        if (v < base + 0x1007 || v >= base + 0x87000) continue;
        unsigned char b[7];
        bool ok = true;
        for (int k = 0; k < 7; ++k) { if (IsBadReadPtr((void*)(uintptr_t)(v - 7 + k), 1)) { ok = false; break; } b[k] = *(unsigned char*)(uintptr_t)(v - 7 + k); }
        if (!ok) continue;
        // b[2]=v-5 (E8 call rel32); FF /2 indirect call ending at v-2/-3/-6/-7 (modrm reg field == 2)
        auto ffcall = [](unsigned char op, unsigned char modrm) { return op == 0xFF && (modrm & 0x38) == 0x10; };
        bool isRet = (b[2] == 0xE8) || ffcall(b[5], b[6]) || ffcall(b[4], b[5]) || ffcall(b[1], b[2]) || ffcall(b[0], b[1]);
        if (!isRet) continue;
        LogLine("[%s]   call[+%04x] -> rva %08lx (from %s)", tag, i*4, (unsigned long)(v - base),
                b[2] == 0xE8 ? "E8" : "FF/2");
        ++logged;
    }
}
// Dump up to 8 dwords from `addr` (the in-process "debugger": structure context to the txt log).
static void CrashDumpMem(const char* tag, uintptr_t addr) {
    if (!addr) { LogLine("[CRASH]   %s=00000000 <null>", tag); return; }
    uint32_t v0;
    if (!CanReadDword((void*)addr, &v0)) { LogLine("[CRASH]   %s=%08lx <unreadable>", tag, (unsigned long)addr); return; }
    char buf[256]; char asc[40]; int off = 0, a = 0;
    off += _snprintf(buf, sizeof(buf), "[CRASH]   %s=%08lx ->", tag, (unsigned long)addr);
    for (int i = 0; i < 8; ++i) {
        uint32_t v;
        if (!CanReadDword((void*)(addr + i*4), &v)) break;
        off += _snprintf(buf+off, sizeof(buf)-off, " %08lx", (unsigned long)v);
        for (int b = 0; b < 4 && a < 36; ++b) { unsigned char ch = (v >> (b*8)) & 0xff; asc[a++] = (ch>=0x20&&ch<0x7f)?(char)ch:'.'; }
    }
    asc[a] = 0;
    LogLine("%s |%s|", buf, asc);
}
static void LogCrashContext(EXCEPTION_POINTERS* ep, const char* tag) {
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        uintptr_t pc = (uintptr_t)er->ExceptionAddress;
        uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        (void)tag;
        uintptr_t access = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
        int rw = er->NumberParameters >= 1 ? (int)er->ExceptionInformation[0] : -1;
        LogLine("[CRASH] code=%08lx pc=%08lx rva=%08lx access=%08lx rw=%d(0=rd,1=wr,8=dep) base=%08lx",
                (unsigned long)er->ExceptionCode, (unsigned long)pc, (unsigned long)(pc - base),
                (unsigned long)access, rw, (unsigned long)base);
        CONTEXT* c = ep->ContextRecord;
        if (c) {
            LogLine("[CRASH] eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
                    c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, c->Esp);
            // Crude stack walk: log dword values on the stack that look like EZ2AC code
            // addresses (0x401000..0x487000) = return addresses → caller chain RVAs.
            // Raw top of stack: [esp+0]=return addr of the faulting fn, [esp+4..]=its args.
            uint32_t* sp = (uint32_t*)c->Esp;
            for (int i = 0; i < 6; ++i) {
                uint32_t v = 0;
                if (!CanReadDword(sp + i, &v)) break;
                LogLine("[CRASH]   [esp+%02x]=%08lx (rva=%08lx)", i*4, (unsigned long)v,
                        (v >= base + 0x1000 && v < base + 0x87000) ? (unsigned long)(v - base) : 0xffffffff);
            }
            // Printable runs on the stack. A path built by sprintf into a fixed stack buffer is
            // the shape of failure here, and its LENGTH is the whole question -- a %s that runs
            // past an unterminated field produces a path far longer than the buffer holds. Walk
            // 1 KB of stack and report every printable run of 8+ characters with its length.
            {
                const unsigned char* b = (const unsigned char*)sp;
                char run[600];
                int runLen = 0, reported = 0;
                for (int i = 0; i < 1024 && reported < 6; ++i) {
                    uint32_t probe = 0;
                    if ((i & 3) == 0 && !CanReadDword((void*)(b + i), &probe)) break;
                    const unsigned char ch = b[i];
                    const bool printable = (ch >= 0x20 && ch < 0x7F);
                    if (printable && runLen < (int)sizeof(run) - 1) {
                        run[runLen++] = (char)ch;
                        continue;
                    }
                    if (runLen >= 8) {
                        run[runLen] = 0;
                        ++reported;
                        LogLine("[CRASH]   stack string @esp+%03x len=%d: '%.400s'",
                                i - runLen, runLen, run);
                    }
                    runLen = 0;
                }
            }
            // EBP frame-pointer chain → reliable caller RVAs (frame = [ebp]=prev, [ebp+4]=ret).
            uint32_t fp = c->Ebp, prevfp = 0;
            for (int depth = 0; depth < 10; ++depth) {
                uint32_t ret = 0, next = 0;
                if (fp <= prevfp || !CanReadDword((void*)(fp + 4), &ret) || !CanReadDword((void*)fp, &next)) break;
                if (ret >= base + 0x1000 && ret < base + 0x87000)
                    LogLine("[CRASH]   frame[%d] ret=%08lx rva=%08lx", depth, (unsigned long)ret, (unsigned long)(ret - base));
                prevfp = fp; fp = next;
            }
            // Structure context: dump memory the faulting instruction and its pointer regs touch.
            // (e.g. a null sub-object, a clobbered table, the SongSelect object header.)
            CrashDumpMem("@access", access);
            CrashDumpMem("@eax", c->Eax); CrashDumpMem("@ecx", c->Ecx); CrashDumpMem("@edx", c->Edx);
            CrashDumpMem("@esi", c->Esi); CrashDumpMem("@edi", c->Edi);
            CrashDumpMem("@ebx", c->Ebx); CrashDumpMem("@ebp", c->Ebp);
        }
    }
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    LogCrashContext(ep, "CRASH");
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
// First-chance VEH: the game's own SEH swallows some faults (then exits), so the unhandled filter
// never fires. Log access violations whose PC is in the game's .text so we still capture them, then
// let the normal handler chain run. Capped to avoid spamming on any benign handled AVs.
static LONG CALLBACK FirstChanceCrashVEH(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        // C++ throw: the abort(3) crashes are uncaught C++ exceptions; the [EXIT] stack is post-unwind
        // (useless). Log the THROWER's caller chain here (first-chance, before unwind). The LAST [throw]
        // before [EXIT] is the fatal one → tells us if it's the texture/disc loader (bad_alloc) or other.
        if (code == 0xE06D7363UL) {
            static unsigned long cppN = 0;
            if (cppN++ < 14) {
                // Decode the MSVC C++ exception TYPE NAME (tells us bad_alloc vs length_error vs custom):
                // ThrowInfo(+0xC)->CatchableTypeArray(+4)->CatchableType(+4)->TypeDescriptor(+8)=name.
                char tn[96] = "?";
                EXCEPTION_RECORD* er = ep->ExceptionRecord;
                if (er->NumberParameters >= 3 && er->ExceptionInformation[0] == 0x19930520u) {
                    uint32_t ti = (uint32_t)er->ExceptionInformation[2], cta = 0, ct = 0, td = 0;
                    if (ti && CanReadDword((void*)(ti + 0xC), &cta) && CanReadDword((void*)(cta + 4), &ct)
                        && CanReadDword((void*)(ct + 4), &td) && td && !IsBadReadPtr((void*)(td + 8), 8)) {
                        const char* nm = (const char*)(td + 8);
                        for (int k = 0; k < 95 && nm[k] && !IsBadReadPtr((void*)(nm + k), 1); ++k) tn[k] = nm[k], tn[k+1] = 0;
                    }
                }
                LogLine("[throw] #%lu C++ exception type='%s'", cppN, tn);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Skip high-frequency BENIGN first-chance exceptions so the real fault isn't buried:
        // C++ EH, OutputDebugString, breakpoints, and our own DR single-step watch. Log EVERYTHING
        // else (AV in ANY module incl. ntdll, stack overflow, illegal/priv instr, int div, ...)
        // with full register + memory context. Capped so a fault loop can't fill the disk.
        switch (code) {
            case 0x40010006UL:           // OutputDebugStringA
            case 0x406D1388UL:           // SetThreadName (NVIDIA NVFBCAsyncThread etc.) — benign, was eating the cap
            case (DWORD)EXCEPTION_BREAKPOINT:
            case (DWORD)EXCEPTION_SINGLE_STEP:
                return EXCEPTION_CONTINUE_SEARCH;
        }
        static unsigned long n = 0;
        if (n++ < 16) {
            uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
            uintptr_t pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
            LogLine("[CRASH] first-chance code=%08lx pc=%08lx rva=%08lx (game SEH may catch it)",
                    (unsigned long)code, (unsigned long)pc,
                    (pc >= base + 0x1000 && pc < base + 0x87000) ? (unsigned long)(pc - base) : 0xffffffffUL);
            LogCrashContext(ep, "CRASH");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH; // let the game's/our normal handlers run
}
static void InstallCrashLogger() {
    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
    AddVectoredExceptionHandler(0, FirstChanceCrashVEH); // 0 = run last among VEHs, before SEH unwind
}


// ---------------------------------------------------------------------------------------------
// Freeze watchdog.
//
// A hang leaves no trace: no exception, no log line, nothing to read afterwards. This samples the
// instruction pointer of every thread in the process every two seconds, and when one of them has
// not moved out of a 64-byte window across four samples it reports where it is stuck, with a stack
// walk. Eight seconds of a thread sitting on the same handful of instructions is a loop or a wait,
// not work.
//
// Suspend/resume around the sample is unavoidable -- GetThreadContext on a running thread returns
// nothing useful -- but it lasts microseconds and only fires every two seconds. Threads that are
// legitimately blocked in the kernel (audio, video, the message pump waiting on input) report an
// EIP inside a system DLL, which is why only addresses inside the game module are considered.
struct ThreadSample { DWORD tid; uint32_t lastEip; int stuckCount; bool reported; };

static HANDLE g_watchdogThread = nullptr;
static volatile LONG g_watchdogStop = 0;

static void WatchdogReport(HANDLE h, const CONTEXT& c, DWORD tid, uintptr_t base) {
    LogLine("[hang] ===== THREAD %lu HAS NOT MOVED FOR ~8s =====", (unsigned long)tid);
    LogLine("[hang] eip=%08lx rva=0x%05lx esp=%08lx ebp=%08lx",
            (unsigned long)c.Eip, (unsigned long)(c.Eip - base),
            (unsigned long)c.Esp, (unsigned long)c.Ebp);
    LogLine("[hang] eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx",
            (unsigned long)c.Eax, (unsigned long)c.Ecx, (unsigned long)c.Edx,
            (unsigned long)c.Ebx, (unsigned long)c.Esi, (unsigned long)c.Edi);
    // Return addresses on the stack name the call chain that led into the loop.
    uint32_t* sp = reinterpret_cast<uint32_t*>(c.Esp);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 12; ++i) {
        uint32_t v = 0;
        if (!CanReadDword(sp + i, &v)) break;
        if (v <= base + 0x1000 || v >= base + 0x87000) continue;
        LogLine("[hang]   stack[+%03x] -> rva 0x%05lx", i * 4, (unsigned long)(v - base));
        ++shown;
    }
    (void)h;
    LogLine("[hang] ==========================================");
}

static DWORD WINAPI WatchdogProc(LPVOID) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    ThreadSample samples[32] = {};
    unsigned nSamples = 0;
    while (!InterlockedCompareExchange(&g_watchdogStop, 0, 0)) {
        Sleep(2000);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te = {};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) continue;
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (SuspendThread(h) != (DWORD)-1) {
                    const bool got = GetThreadContext(h, &c) != 0;
                    ResumeThread(h);
                    if (got && c.Eip > base + 0x1000 && c.Eip < base + 0x87000) {
                        ThreadSample* s = nullptr;
                        for (unsigned i = 0; i < nSamples; ++i)
                            if (samples[i].tid == te.th32ThreadID) { s = &samples[i]; break; }
                        if (!s && nSamples < 32) { s = &samples[nSamples++]; s->tid = te.th32ThreadID; }
                        if (s) {
                            if ((c.Eip & ~0x3Fu) == (s->lastEip & ~0x3Fu)) ++s->stuckCount;
                            else { s->stuckCount = 0; s->reported = false; }
                            s->lastEip = (uint32_t)c.Eip;
                            if (s->stuckCount >= 4 && !s->reported) {
                                s->reported = true;
                                WatchdogReport(h, c, te.th32ThreadID, base);
                            }
                        }
                    }
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    return 0;
}

static void StartFreezeWatchdog() {
    if (g_watchdogThread || !Config().hangWatchdog) return;
    g_watchdogThread = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
    LogLine("[hang] freeze watchdog started (samples every 2s, reports after ~8s of no progress)");
}
// ---------------------------------------------------------------------------------------------

static DWORD WINAPI InitThread(LPVOID) {
    WriteMarkerFile("InitThread.begin");
    SetConfig(LoadConfig());
    if (Config().showInjectedMessageBox) {
        MessageBoxA(nullptr, "EZ2CustomMod.dll was injected successfully.", "EZ2CustomMod", MB_OK | MB_ICONINFORMATION);
    }
    if (!Config().coreEnable) {
        WriteMarkerFile("Core.disabled");
        return 0;
    }
    LogInit();
    srand(GetTickCount() ^ GetCurrentProcessId());
    InstallCrashLogger();
    // Optional execution tracer (ez2tracer.dll next to EZ2B.exe). Loads only if present. The mod arms
    // it from the song inject (Ez2TraceArm) and flushes it on exit (Ez2TraceFlush) — see those calls.
    if (GetFileAttributesA("ez2tracer.dll") != INVALID_FILE_ATTRIBUTES) {
        HMODULE t = LoadLibraryA("ez2tracer.dll");
        LogLine("[init] ez2tracer.dll load=%p", (void*)t);
    }
    LogLine("[init] base=%p exeDir='%s'", GetModuleHandleA(nullptr), GetExeDirectory().c_str());
        LogLine("[config] bridgeMode=%d redirectCustomToNative=%d pinCustomAtTop=%d forceSelectCustom=%d bmsRoot='%s' cacheRoot='%s'", Config().bridgeMode?1:0, Config().redirectCustomToNative?1:0, Config().pinCustomAtTop?1:0, Config().forceSelectCustom?1:0, Config().bmsRoot.c_str(), Config().cacheRoot.c_str());
        LogLine("[autochart-config] enable=%d source='%s' targets='%s' intensity=%d seed=%d learnNative=%d nativeRoot='%s' maxCharts=%d",
                Config().autoGenerateModes ? 1 : 0, Config().autoGenerateSourceMode.c_str(),
                Config().autoGenerateTargetModes.c_str(), Config().autoGenerateIntensity,
                Config().autoGenerateSeed, Config().learnNativeCharts ? 1 : 0,
                Config().nativeChartRoot.c_str(), Config().nativeModelMaxChartsPerMode);
        LogLine("[version] EZ2CustomMod_v36_0_0_native_autochart");
        LogLine("[log] reset_on_start=1");

    // FUN_00402880 logger. RVA 0x2880. Original is logger stub in this executable.
    bool logPatchOk = InstallJmp(Rva(0x2880), reinterpret_cast<void*>(&GameLogHook), g_gameLogPatch, 5);
    LogLine("[patch] game logger FUN_00402880 patch ok=%d", logPatchOk ? 1 : 0);

    InstallWindowApiHooks();
    InstallFileHooks();
    InstallGameFsOpenHook();
    InstallDrawNullGuard();     // stop the >~2000-song wheel AV on a null disc texture (FUN_00404950)
    InstallPoolOverflowGuard(); // texture pool full → flush+retry instead of null-deref crash (0x41455b)
    InstallTextRenderNullGuard();// stop the 5Key SongSelect AV: pool-full → null text-texture → sub_407A50
    InstallCategoryWheelGuard(); // stop the category-change AV: null disc-texture slot → FUN_00432200 deref
    // InstallTexPoolGrow() removed: the 32MB KResourceContainer texture pool is allocated before our
    // init and hands out ABSOLUTE pointers into its buffer, so it can't be grown or relocated without
    // dangling every texture handle (lost native discs). The songname budget caps loads to fit it.
    bool directBms = InstallDirectBmsHook();
    SetDirectBmsEnabled(directBms);
    InstallJudgeOffset();
    if (Config().showProgressPopup) ShowStartupPopup();
    ScanBMSData();
    InstallTraceHooks();
    // SetWriteWatch4(0x037dcecc); // diag tool (kept below) — found the Group A song.bin-parser overrun
    if (Config().showProgressPopup) CloseStartupPopup();
    ReleaseStartupGate();
    if (Config().captureExistingWindow) CaptureExistingProcessWindow();

    LogLine("[init] hooks installed");
    StartFreezeWatchdog();
    WriteMarkerFile("InitThread.end");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        WriteMarkerFile("DLL_PROCESS_ATTACH");
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        WriteMarkerFile("DLL_PROCESS_DETACH");
        ShutdownWindowHook();
        RemoveTraceHooks();
        RemoveJmp(g_directVideoFramePatch);
        RemoveJmp(g_directBackgroundPatch);
        RemoveJmp(g_directBmsPatch);
        if (g_directVideoFrameBuffer) {
            VirtualFree(g_directVideoFrameBuffer, 0, MEM_RELEASE);
            g_directVideoFrameBuffer = nullptr;
            g_directVideoFrameCapacity = 0;
        }
        RemoveJmp(g_gameLogPatch);
        LogClose();
    }
    return TRUE;
}
