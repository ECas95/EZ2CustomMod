// The "already one of ours" guard on MaybeRedirectCustomPath.
//
// The game's CRT fopen opens the path we hand it with CreateFileA, which lands back in our own
// CreateFileA hook. Resolving it a second time is never right, and for the preview it was fatal:
// the in-RAM branch answered with a virtual (CreateEventA) handle that the CRT cannot turn into a
// FILE*, so _fsopen returned NULL and every custom preview fell back to the silent 0Blank --
// 656 "Open Error : OpenSample" lines in one session, one per hover.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o cache_path_guard_test tests/cache_path_guard_test.cpp
//   ./cache_path_guard_test
#include <cstdio>
#include <string>

// ---- must match mod_main.cpp -------------------------------------------------------------------
static std::string LowerAscii(const std::string& in) {
    std::string out = in;
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

static bool PathIsUnderDirectory(const std::string& path, const std::string& dir) {
    if (dir.empty() || path.size() <= dir.size() + 1) return false;
    if (LowerAscii(path).compare(0, dir.size(), dir) != 0) return false;
    const char sep = path[dir.size()];
    return sep == '\\' || sep == '/';
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main() {
    const std::string root = "c:\\ez2ac\\mod\\ez2ac_fnex\\customcache";

    check(PathIsUnderDirectory("C:\\EZ2AC\\Mod\\EZ2AC_FNEX\\CustomCache\\5keyMix\\bms1\\bms1.ssf", root),
          "a cache file is ours, whatever its case");
    check(PathIsUnderDirectory("c:\\ez2ac\\mod\\ez2ac_fnex\\customcache/5keyMix/bms1.ssf", root),
          "forward slashes count as a separator too");

    // What must still be redirected.
    check(!PathIsUnderDirectory("System\\Preview\\bms019691.ssf", root),
          "the game's own request is not ours");
    check(!PathIsUnderDirectory("5keyMix1p-bms1-shd.ini", root),
          "a bare chart name is not ours");
    check(!PathIsUnderDirectory("C:\\EZ2AC\\Mod\\EZ2AC_FNEX\\BMSData\\x\\y.wav", root),
          "a source BMS file is not ours");
    check(!PathIsUnderDirectory("c:\\ez2ac\\mod\\ez2ac_fnex\\customcacheold\\bms1.ssf", root),
          "a sibling directory with a longer name is not ours");
    check(!PathIsUnderDirectory(root, root),
          "the root itself is a directory, not a file in it");
    check(!PathIsUnderDirectory(root + "\\", root),
          "the root with a trailing slash is not a file");
    check(!PathIsUnderDirectory("", root), "an empty path is not ours");
    check(!PathIsUnderDirectory("c:\\ez2ac\\mod\\ez2ac_fnex\\customcache\\a.ssf", ""),
          "no root configured: never ours");

    std::printf(failures ? "\nFAILED\n" : "\nall ok\n");
    return failures ? 1 : 0;
}
