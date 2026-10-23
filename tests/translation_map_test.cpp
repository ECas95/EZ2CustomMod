// Which file names the English texture layer answers for (src/translation_map.h).
#include "../src/translation_map.h"
#include <cassert>
#include <cstdio>

int main() {
    const std::string exe = "g:\\000 - main";
    assert(TranslationKey("system\\SongSelect\\TT_UseKey2_panel.abm", exe) == "system\\songselect\\tt_usekey2_panel.abm");
    assert(TranslationKey("System/HowToPlay/skip.abm", exe) == "system\\howtoplay\\skip.abm");
    assert(TranslationKey(".\\system\\warning.abm", exe) == "system\\warning.abm");
    assert(TranslationKey("G:\\000 - MAIN\\system\\songname\\feel.abm", exe) == "system\\songname\\feel.abm");
    // Anything outside system\ is not ours: songs, the cache, another install.
    assert(TranslationKey("BMSData\\5KeyMix\\x\\a.bmp", exe).empty());
    assert(TranslationKey("G:\\000 - MAIN\\CustomCache\\5keyMix\\bms000001\\disc\\x.abm", exe).empty());
    assert(TranslationKey("C:\\other\\system\\warning.abm", exe).empty());
    assert(TranslationKey("", exe).empty());
    std::puts("translation_map_test OK");
    return 0;
}
