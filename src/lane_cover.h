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

// Turntable movement since the last look, from the board's raw 8-bit position (Obj40C180 +0x420220,
// the byte read from port 0x103), wrapped exactly as the engine wraps it (FUN_0040c4b0).
//
// The running total at +0x420238 is NOT usable for this: the engine zeroes it whenever it passes a
// limit of only 5, 10 or 20 counts (+0x420268, set per screen), so a steady turn read back as
// +3, +3, -3, +3, -3 ... and the cover shook in place instead of following the turntable.
inline int TurntableCounts(int prevRaw, int raw) {
    const int d = (raw - prevRaw) & 0xff;
    return d > 0x80 ? d - 0x100 : d;
}

inline int LaneCoverAfter(int value, int counts, int unitsPerCount) {
    const int v = value + counts * unitsPerCount;
    return v < 0 ? 0 : v > kLaneCoverMax ? kLaneCoverMax : v;
}

// Where the cover ends, in screen y: `value` thousandths of the way from the top of the lanes to
// the judge line.
inline float LaneCoverBottom(float top, float judgeY, int value) {
    return top + (judgeY - top) * (float)value / (float)kLaneCoverMax;
}
