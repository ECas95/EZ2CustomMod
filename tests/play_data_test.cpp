// Favourites and plays behind the FAVORITES / RECENT / MOST PLAYED categories (src/play_data.h).
#include "../src/play_data.h"
#include <cassert>
#include <cstdio>

int main() {
    PlayData d;
    assert(d.toggleFavorite("5keymix\\anx\\a") && d.toggleFavorite("5keymix\\anx\\b"));
    assert(d.isFavorite("5keymix\\anx\\a"));
    assert(!d.toggleFavorite("5keymix\\anx\\a"));                 // second press removes it
    assert(!d.isFavorite("5keymix\\anx\\a"));
    d.toggleFavorite("5keymix\\anx\\c");
    assert((d.favorites(10) == std::vector<std::string>{"5keymix\\anx\\c", "5keymix\\anx\\b"}));   // newest first
    assert(d.favorites(1).size() == 1);

    d.recordPlay("x"); d.recordPlay("y"); d.recordPlay("x"); d.recordPlay("z");
    assert((d.recent(10) == std::vector<std::string>{"z", "x", "y"}));
    assert((d.mostPlayed(10) == std::vector<std::string>{"x", "z", "y"}));      // tie z/y: z played later
    assert(d.plays("x") == 2 && d.plays("nothing") == 0);

    // Round trip, and the clock carries on past what was loaded.
    PlayData e;
    e.parse(d.serialize());
    assert(e.favorites(10) == d.favorites(10) && e.recent(10) == d.recent(10) && e.mostPlayed(10) == d.mostPlayed(10));
    e.recordPlay("y");
    assert(e.recent(1)[0] == "y");

    // Junk lines and CRLF do not lose the good ones.
    PlayData f;
    f.parse("garbage\r\nF\t7\tsong one\r\nP\tx\r\nP\t3\t9\tsong two\r\n\r\n");
    assert(f.isFavorite("song one") && f.plays("song two") == 3);
    std::puts("play_data_test OK");
    return 0;
}
