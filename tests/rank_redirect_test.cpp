// Which files the rank redirect moves to CustomCache\rank (src/rank_redirect.h).
#include "../src/rank_redirect.h"
#include <cassert>
#include <cstdio>

int main() {
    assert(CustomRankFileName("sound\\rank_5keyMix_bms000035-ex.bin") == "rank_5keyMix_bms000035-ex.bin");
    assert(CustomRankFileName("SOUND\\rank_5keyMix_bms000680.bin") == "rank_5keyMix_bms000680.bin");
    assert(CustomRankFileName("G:\\000 - MAIN\\Sound\\e_rank_14RadioMix_bms12-hd.bin") == "e_rank_14RadioMix_bms12-hd.bin");
    assert(CustomRankFileName("sound/rank_clubMix_bms000001-shd.bin") == "rank_clubMix_bms000001-shd.bin");
    // Native songs, the game's odd empty-id file, other folders and other files stay where they are.
    assert(CustomRankFileName("sound\\rank_5keyMix_11stargazer.bin").empty());
    assert(CustomRankFileName("sound\\rank_5keyMix_.bin").empty());
    assert(CustomRankFileName("sound\\rank_5keyMix_bmsx.bin").empty());
    assert(CustomRankFileName("system\\rank_5keyMix_bms000001.bin").empty());
    assert(CustomRankFileName("sound\\rank_5keyMix_bms000001.bin.bak").empty());
    assert(CustomRankFileName("sound\\bms000001.ssf").empty());
    std::puts("rank_redirect_test OK");
    return 0;
}
