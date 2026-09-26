#pragma once
#include <cstdlib>

// SUDDEN+ -- IIDX's lane cover -- on the keys panel: a curtain over the top of the lanes that the
// player lowers until notes only appear a comfortable distance above the judge line. Its size is
// IIDX's "white number", 0..1000 of the way from the top of the screen down to the judge line;
// 0 is no cover. mod_main.cpp draws it and wires the input; this is the part that can be tested.

static const int kLaneCoverMax = 1000;

// EFX1 toggles bass boost in play. Holding it and turning the turntable moves the cover instead,
// so the effector tick is shown the button as merely "held" while it is down, and the press it
// did not see is replayed at release -- but only when the turntable did not move. A tap still
// toggles bass boost (on release, a frame late); a hold that moved the cover changes nothing else.
class LaneCoverEfx {
public:
    static const int kPressed = 2, kHeld = 3;   // Obj40C180 slot states; below 2 is up

    // The state the effector tick is given for the button, from the real one.
    int effectorState(int raw) {
        if (raw >= kPressed) {
            if (!held_) { held_ = true; moved_ = false; }
            return kHeld;
        }
        if (held_) {
            held_ = false;
            if (!moved_) return kPressed;
        }
        return raw;
    }
    // The cover moved while the button was down: the release must not toggle bass boost.
    void moved() { held_ = true; moved_ = true; }

private:
    bool held_ = false, moved_ = false;
};

// Turntable movement since the last look, from the board's running total (Obj40C180 accum[0]).
// The engine zeroes the total once it passes its limit and adds the new movement on top, so a
// jump far larger than a hand can turn in a frame is that reset, and the total is the movement.
inline int TurntableCounts(int prevAccum, int accum) {
    const int d = accum - prevAccum;
    return std::abs(d) > 200 ? accum : d;
}

inline int LaneCoverAfter(int value, int counts, int unitsPerCount) {
    const int v = value + counts * unitsPerCount;
    return v < 0 ? 0 : v > kLaneCoverMax ? kLaneCoverMax : v;
}

// Where the cover ends, in screen y: `value` thousandths of the way from the top to the judge line.
inline float LaneCoverBottom(float judgeY, int value) {
    return judgeY * (float)value / (float)kLaneCoverMax;
}
