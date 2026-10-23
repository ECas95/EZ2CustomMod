// Guards the replacement for sub_412F00, the game's "copy the filename stem" helper.
//
// The original:
//     v3 = strrchr(Str, '\\'); v3 = v3 ? v3 + 1 : Str;   // start of the filename
//     v4 = strrchr(Str, '.');                            // the extension
//     if (!v4) v4 = Str + strlen(Str);                   // no dot at all -> whole name
//     result = v4 - v3;                                  // SIGNED subtraction...
//     qmemcpy(a2, v3, result);                           // ...used as unsigned
//
// "No dot at all" is handled. "The only dot is in a DIRECTORY component" is not: then v4 points
// before v3, the subtraction goes negative, and the unsigned qmemcpy runs ~4 GB off the end of the
// caller's buffer. sub_407E50 (the .abm loader) calls this with CHAR chText[260] whose /GS cookie
// sits immediately after it, so the overrun trips the cookie instantly -- the "Buffer overrun
// detected!" abort seen on the CYT-2.2 category, whose folder name contains a dot.
//
// EZ2BasenameStem is copied verbatim from trace_hooks.cpp.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o basename_stem_test tests/basename_stem_test.cpp
//   ./basename_stem_test
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

static unsigned long g_stemNegatives = 0;
static void LogLine(const char*, ...) {}                     // stubbed for the test

// ---- verbatim from trace_hooks.cpp ------------------------------------------------------------
extern "C" uint32_t EZ2BasenameStem(const char* str, char* out) {
    if (!str || !out) return 0;
    const char* slash = strrchr(str, '\\');
    const char* begin = slash ? slash + 1 : str;
    const char* dot   = strrchr(str, '.');
    if (!dot || dot < begin) {
        if (dot && dot < begin && g_stemNegatives < 8) {
            ++g_stemNegatives;
            LogLine("[stem] negative length averted");
        }
        dot = str + strlen(str);
    }
    size_t n = (size_t)(dot - begin);
    if (n > 255) n = 255;
    memcpy(out, begin, n);
    out[n] = 0;
    return (uint32_t)n;
}
// -----------------------------------------------------------------------------------------------

// What the original computed, as a signed length so a test can SEE it go negative.
static long OriginalLength(const char* str) {
    const char* slash = strrchr(str, '\\');
    const char* begin = slash ? slash + 1 : str;
    const char* dot   = strrchr(str, '.');
    if (!dot) dot = str + strlen(str);
    return (long)(dot - begin);
}

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static std::string stem(const char* path) {
    char buf[512];
    memset(buf, 0xCD, sizeof(buf));
    uint32_t n = EZ2BasenameStem(path, buf);
    check(n == strlen(buf), "returned length matches the string it wrote");
    return std::string(buf);
}

int main() {
    // ---- the crash case ---------------------------------------------------------------------
    {
        const char* p = "System\\SongSelect\\CYT-2.2\\icon";
        check(OriginalLength(p) < 0, "the original really does go negative on a dotted directory");
        check(stem(p) == "icon", "replacement returns the filename when only a directory has a dot");
    }
    {
        const char* p = "CustomCache\\5keyMix\\DJM.1\\songname";
        check(OriginalLength(p) < 0, "control: a second dotted-directory path is also negative");
        check(stem(p) == "songname", "and it too resolves to the plain filename");
    }

    // ---- everything the original got right must be unchanged --------------------------------
    check(stem("system\\songname\\bms000675.abm") == "bms000675", "normal path: stem without extension");
    check(stem("bms000675.abm") == "bms000675", "no directory component");
    check(stem("system\\songname\\noext") == "noext", "no dot anywhere at all");
    check(stem("C:\\EZ2AC\\EZ2AC_FNEX\\CustomCache\\5keyMix\\bms052745\\songname\\bms052745.abm")
          == "bms052745", "a long real path from the log");
    check(stem("a.b.c\\d.e.f.abm") == "d.e.f", "several dots: the LAST one is the extension");
    check(stem("dir.with.dots\\file.abm") == "file", "dots in both directory and filename");

    // ---- degenerate inputs ------------------------------------------------------------------
    check(stem("") == "", "empty string");
    check(stem("\\") == "", "a lone separator");
    check(stem(".abm") == "", "a name that is nothing but an extension");
    check(stem("dir.x\\") == "", "trailing separator after a dotted directory");
    check(EZ2BasenameStem(nullptr, nullptr) == 0, "null arguments are refused");

    // ---- the clamp protects the caller's 260-byte buffer ------------------------------------
    {
        std::string huge = "dir\\" + std::string(4000, 'x');
        char buf[512];
        memset(buf, 0xCD, sizeof(buf));
        uint32_t n = EZ2BasenameStem(huge.c_str(), buf);
        check(n == 255, "an absurdly long stem is clamped to 255");
        check(buf[255] == 0, "and still NUL-terminated inside the caller's 260 bytes");
        check((unsigned char)buf[256] == 0xCD, "nothing was written past the clamp");
    }

    if (g_failures == 0) std::printf("basename stem: ok\n");
    return g_failures ? 1 : 0;
}
