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

static void __cdecl GameLogHook(const char* fmt, ...) {
    if (!Config().traceGameLog) return;
    if (!LooksLikeFormatString(fmt)) return; // many destructor/unwind calls use FUN_00402880 without a valid format arg
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
    // overflowed at ~4k textures — that was the 1MB texture-OBJECT pool (EXE-patched to 16MB) plus
    // the 32MB pixel pool (EXE-patched to 512MB — sized for XP's 2GB address space; a 1GB reserve
    // wouldn't fit there). Past the pixel budget the insert/draw guards degrade to blank art, no
    // crash. Each class maps to its cache subfolder (generated by GenerateVisualAssetCache);
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



static std::string MaybeRedirectCustomDirectory(LPCSTR name) {
    if (!name) return std::string();
    std::string p = name;
    const auto& songs = GetCustomBmsSongs();
    for (const auto& s : songs) {
        size_t pos = p.find(s.id);
        if (pos == std::string::npos) continue;

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

static std::string MaybeRedirectCustomPath(LPCSTR name) {
    if (!name) return std::string();
    std::string p = name;

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
            for (const auto& s : songs) {
                if (_stricmp(s.id.c_str(), id.c_str()) != 0) continue;
                if (!s.videoFiles.empty()) break; // legacy own BGA has a real INI
                std::string def = JoinPath(JoinPath(JoinPath(GetExeDirectory(), "Video"), "_spv"), "default.ini");
                if (LocalFileExists(def)) return def;
                break;
            }
        }
    }

    for (const auto& s : songs) {
        size_t pos = p.find(s.id);
        if (pos == std::string::npos) continue;

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
            std::string visual = EnsureSongVisualAsset(s, p);
            if (!visual.empty()) {
                LogLine("[assets] lazy visual '%s' -> '%s'", p.c_str(), visual.c_str());
                return visual;
            }
            std::string fn = FileNameOnly(p);
            if (!s.cacheDir.empty() && fn == s.id + ".ssf") {
                EnsureSongPreview(s);
                std::string preview = JoinPath(s.cacheDir, fn);
                if (LocalFileExists(preview)) return preview;
                return std::string();
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
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -(LONG)height; // ffmpeg rawvideo is top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = bytes;
    void** vtable = *reinterpret_cast<void***>(surface);
    auto getDC = reinterpret_cast<DirectSurfaceGetDC_t>(vtable[17]);
    auto releaseDC = reinterpret_cast<DirectSurfaceReleaseDC_t>(vtable[26]);
    HDC dc = nullptr;
    if (FAILED(getDC(surface, &dc)) || !dc) return false;
    int copied = StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height,
                               g_directVideoFrameBuffer, &info, DIB_RGB_COLORS, SRCCOPY);
    releaseDC(surface, dc);
    if (!copied || (DWORD)copied == GDI_ERROR) return false;
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
            if (g_directVideoStartMs)
                LogLine("[bga-direct] ffmpeg sync delay armed %lu ms", (unsigned long)g_directVideoStartMs);
        }
        if (GetTickCount() - g_directVideoDelayClock < g_directVideoStartMs) return;
        if (!StartDirectBmsFfmpegVideo(g_directVideoPath)) {
            LogLine("[bga-direct] ffmpeg fallback failed source='%s'", g_directVideoPath);
            g_directVideoDisabled = true;
            return;
        }
        g_directVideoStarted = true;
        LogLine("[bga-direct] ffmpeg video started");
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
                        BITMAPINFO info{};
                        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        info.bmiHeader.biWidth = (LONG)sourceW;
                        info.bmiHeader.biHeight = rawH < 0 ? -(LONG)sourceH : (LONG)sourceH;
                        info.bmiHeader.biPlanes = 1;
                        info.bmiHeader.biBitCount = bits;
                        info.bmiHeader.biCompression = BI_RGB;
                        info.bmiHeader.biSizeImage = (DWORD)expected64;
                        void** surfaceVtable = *reinterpret_cast<void***>(surface);
                        auto getDC = reinterpret_cast<DirectSurfaceGetDC_t>(surfaceVtable[17]);
                        auto releaseDC = reinterpret_cast<DirectSurfaceReleaseDC_t>(surfaceVtable[26]);
                        HDC dc = nullptr;
                        if (SUCCEEDED(getDC(surface, &dc)) && dc) {
                            SetStretchBltMode(dc, COLORONCOLOR);
                            DWORD cropX = g_directVideoCropX, cropY = g_directVideoCropY;
                            DWORD cropW = g_directVideoCropW ? g_directVideoCropW : sourceW;
                            DWORD cropH = g_directVideoCropH ? g_directVideoCropH : sourceH;
                            if (cropX + cropW > sourceW) { cropX = 0; cropW = sourceW; }
                            if (cropY + cropH > sourceH) { cropY = 0; cropH = sourceH; }
                            int copied = StretchDIBits(dc, 0, 0, (int)g_directVideoTargetW, (int)g_directVideoTargetH,
                                (int)cropX, (int)cropY, (int)cropW, (int)cropH, g_directVideoFrameBuffer,
                                &info, DIB_RGB_COLORS, SRCCOPY);
                            releaseDC(surface, dc);
                            if (copied != 0 && (DWORD)copied != GDI_ERROR) {
                                ++g_directVideoValidFrames;
                                if (!g_directVideoFirstFrameLogged) {
                                    LogLine("[bga-direct] first frame source=%lux%lu bits=%u bytes=%lu target=640x480 crop=%lu,%lu %lux%lu",
                                            (unsigned long)sourceW, (unsigned long)sourceH, (unsigned)bits,
                                            (unsigned long)actual, (unsigned long)cropX, (unsigned long)cropY,
                                            (unsigned long)cropW, (unsigned long)cropH);
                                    g_directVideoFirstFrameLogged = true;
                                }
                            } else if (g_directVideoFrameFailures++ < 3) {
                                LogLine("[bga-direct] StretchDIBits failed result=%d", copied);
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
    if (!g_directVideoDisabled && g_directVideoFfmpeg && self == g_directVideoObject) {
        RenderFfmpegVideoFrame(self);
        return;
    }
    if (!g_directVideoDisabled && g_directVideoTargetReady && self == g_directVideoObject) {
        RenderDirectVideoFrame(self);
        return;
    }
    if (RealVideoFrame) RealVideoFrame(self);
}

static int __fastcall HookSongPackage(void* self, void*, const char* request) {
    ResetDirectBmsVideo();
    ResetDirectBmsVideoRuntime();
    if (LoadCustomBmsDirect(self, request)) {
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
    return RealSongPackage ? RealSongPackage(self, request) : 0;
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
    std::string redirected = MaybeRedirectCustomPath(name);

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

// KResourceContainer texture pool: FUN_004143e0 (thiscall) allocates a FIXED pool of [this+8] bytes
// (VirtualAlloc) on first use; loads bump-allocate into it and FAIL when it's full ("Exceed pool size"
// → null texture → SongSelect::OnCreateGame Credits_mask abort). Grow [this+8] before the alloc so all
// custom songname textures fit. Only the big texture pool (>64MB) is grown; small pools untouched.
// ponytail: +headroom is sized for songnames (~32KB each); discs (~463KB) stay lazy — fitting ALL discs
// would be ~900MB and risk the 32-bit 2GB address-space cap.
static const uint32_t kTexPoolSize = 0x08000000u; // 128 MB (all custom songnames ~70MB + UI; safe in 2GB)

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
    const CustomBmsSong* song = nullptr;
    for (const auto& s : GetCustomBmsSongs()) if (_stricmp(s.id.c_str(), id) == 0) { song = &s; break; }
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

static HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    if (!DirectBmsEnabled()) EnsureQuartzHook();
    DetectActiveModeFromPath(name, "CreateFileA");
    MaybeArmTracerOnCredits(name);
    // Our own sprites never reach the filesystem: build the .abm in RAM and answer with a handle
    // the read hooks know how to serve.
    if (name && (creation == OPEN_EXISTING || creation == OPEN_ALWAYS)) {
        std::string lower = LowerAscii(name);
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
        MessageBoxA(nullptr, "EZ2CustomMod.dll fue inyectado correctamente.", "EZ2CustomMod", MB_OK | MB_ICONINFORMATION);
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
