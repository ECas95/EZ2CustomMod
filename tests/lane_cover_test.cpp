// SUDDEN+ input and geometry (src/lane_cover.h).
#include "../src/lane_cover.h"
#include <cassert>
#include <cstdio>

int main() {
    // A tap: the effector sees "held" while it is down and the press at release -- once.
    {
        LaneCoverEfx e;
        assert(e.effectorState(0) == 0);
        assert(e.effectorState(2) == 3);
        assert(e.effectorState(3) == 3);
        assert(e.effectorState(1) == 2);
        assert(e.effectorState(1) == 1);
        assert(e.effectorState(0) == 0);
    }
    // A hold that moved the cover: no press at all, so bass boost is left alone.
    {
        LaneCoverEfx e;
        assert(e.effectorState(2) == 3);
        e.moved();
        assert(e.effectorState(3) == 3);
        assert(e.effectorState(1) == 1);
        // ...and the next tap works again.
        assert(e.effectorState(2) == 3);
        assert(e.effectorState(0) == 2);
    }
    // The cover moved before the effector tick saw the press (same frame, other order).
    {
        LaneCoverEfx e;
        e.moved();
        assert(e.effectorState(2) == 3);
        assert(e.effectorState(0) == 0);
    }

    assert(TurntableCounts(0, 5) == 5);
    assert(TurntableCounts(40, 32) == -8);
    assert(TurntableCounts(250, 3) == 9);        // the 8-bit position wrapped forwards
    assert(TurntableCounts(3, 250) == -9);       // ...and backwards
    assert(TurntableCounts(0, 128) == 128);      // the engine's own wrap: +0x80 stays positive
    assert(TurntableCounts(0, 129) == -127);
    // A steady forward turn of 3 counts a frame must add up, every frame, whatever the position --
    // the running total it replaced read this as +3, +3, -3, +3, -3 (zeroed past a limit of 5).
    {
        int raw = 250, prev = raw, sum = 0;
        for (int i = 0; i < 100; ++i) { raw = (raw + 3) & 0xff; sum += TurntableCounts(prev, raw); prev = raw; }
        assert(sum == 300);
    }

    assert(LaneCoverAfter(0, 10, 3) == 30);
    assert(LaneCoverAfter(20, -10, 3) == 0);
    assert(LaneCoverAfter(990, 10, 3) == 1000);

    assert(LaneCoverBottom(0.0f, 400.0f, 0) == 0.0f);
    assert(LaneCoverBottom(40.0f, 440.0f, 500) == 240.0f);
    assert(LaneCoverBottom(0.0f, 400.0f, 250) == 100.0f);
    assert(LaneCoverBottom(40.0f, 400.0f, 1000) == 400.0f);
    std::puts("lane_cover_test OK");
    return 0;
}
