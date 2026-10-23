// LNTYPE 1 pairing that survives a missing event (src/ln_pairing.h).
#include "../src/ln_pairing.h"
#include <cassert>
#include <cstdio>

int main() {
    using V = std::vector<int>;
    // Well formed: plain alternation.
    {
        V r = PairLongNotes<int>({10, 20, 30, 40, 50, 60}, {});
        assert((r == V{kLnHead, kLnTail, kLnHead, kLnTail, kLnHead, kLnTail}));
    }
    // Well formed with taps between holds: unchanged.
    {
        V r = PairLongNotes<int>({10, 20, 30, 40}, {25, 45});
        assert((r == V{kLnHead, kLnTail, kLnHead, kLnTail}));
    }
    // One event missing early (the O2J case): alternation would invert every later hold and put
    // the taps at 25 and 45 inside them. One event is let go instead.
    {
        //            head tail head tail(missing)   head tail
        V r = PairLongNotes<int>({10, 20, 30, 50, 60}, {25, 45});
        // 10-20 hold, 30 alone (its tail was lost), 50-60 hold; nothing encloses 25 or 45.
        assert((r == V{kLnHead, kLnTail, kLnAlone, kLnHead, kLnTail}));
    }
    // Odd count at the end: the last event is a tap.
    {
        V r = PairLongNotes<int>({10, 20, 30}, {});
        assert((r == V{kLnHead, kLnTail, kLnAlone}));
    }
    // A missing HEAD at the start: the orphan tail stands alone, the rest pairs.
    {
        V r = PairLongNotes<int>({5, 10, 20, 30, 40}, {7, 25});
        assert(r[0] == kLnAlone && r[1] == kLnHead && r[2] == kLnTail && r[3] == kLnHead && r[4] == kLnTail);
    }
    assert(PairLongNotes<int>({}, {1, 2}).empty());
    std::puts("ln_pairing_test OK");
    return 0;
}
