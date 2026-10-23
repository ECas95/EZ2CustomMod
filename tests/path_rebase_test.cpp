// Scan cache entries made in one install folder, loaded from another (the cabinet copy).
#include "../src/path_rebase.h"
#include <cassert>
#include <cstdio>

int main() {
    const std::string oldExe = "C:\\EZ2AC\\Mod\\EZ2AC_FNEX";
    const std::string song = oldExe + "\\BMSData\\5KeyMix\\ANX\\Bat - Hommarju";
    const std::string art = oldExe + "\\CustomCache\\5keyMix\\bms000013";

    std::string base = CachedInstallBase(song, "BMSData", "5KeyMix");
    assert(base == oldExe);
    assert(CachedInstallBase(song, "bmsdata", "5keymix") == oldExe);      // case does not matter
    assert(RebasePath(song, base, "D:\\000 - MAIN") == "D:\\000 - MAIN\\BMSData\\5KeyMix\\ANX\\Bat - Hommarju");
    assert(RebasePath(art, base, "D:\\000 - MAIN") == "D:\\000 - MAIN\\CustomCache\\5keyMix\\bms000013");

    // A song whose own name contains "\BMSData\5KeyMix\" further down keeps the install prefix.
    const std::string odd = oldExe + "\\BMSData\\5KeyMix\\X\\BMSData\\5KeyMix\\y";
    assert(CachedInstallBase(odd, "BMSData", "5KeyMix") == oldExe + "\\BMSData\\5KeyMix\\X");

    // An absolute Root= outside the install: nothing to rebase.
    assert(CachedInstallBase("E:\\Songs\\5KeyMix\\a", "E:\\Songs", "5KeyMix").empty());
    // Only whole folder prefixes move: "...\EZ2AC_FNEX2" is not under "...\EZ2AC_FNEX".
    assert(RebasePath(oldExe + "2\\BMSData\\x", oldExe, "D:\\M") == oldExe + "2\\BMSData\\x");
    assert(RebasePath(song, "", "D:\\M") == song);
    std::puts("path_rebase_test OK");
    return 0;
}
