#pragma once
#include <cstdint>
#include <string>

// Which lanes a BMS chart is written for, and the modes that can play it exactly as written.
//
// The other modes are generated from the 5-key source charts (ApplyModeAutoGeneration), which
// throws away where the author put each note. That is the only option for a 5-key chart, but the
// library also holds charts written for these layouts -- surveyed 2026-09-26:
//   - 7 keys (BME 11-15 + 18/19): 503 songs (MUS 317, POPS 118, KRA 67, EZM2 1) -> 7keyMix
//   - double, 5 + 5 (11-15 + 21-25): 97 songs (COS 94, RHYS 3)                  -> clubMix
//   - double with a 6th/7th key on either side (18/19/28/29): 51 songs (COS)     -> spaceMix
// Those modes take the song's matching charts and play them on the lanes they were written for.
//
// Lane mask: bit 0..8 = channels 11..19, bit 9..17 = 21..29 (long-note 5x/6x count as 1x/2x).

inline int LaneBit(int channel) {
    if (channel >= 0x51 && channel <= 0x69) channel -= 0x40;
    if (channel >= 0x11 && channel <= 0x19) return channel - 0x11;
    if (channel >= 0x21 && channel <= 0x29) return 9 + (channel - 0x21);
    return -1;
}

inline uint32_t LaneMaskOf(int channel) {
    const int b = LaneBit(channel);
    return b < 0 ? 0u : 1u << b;
}

enum class ChartLayout { Five, Seven, Double10, Double14 };

inline ChartLayout ClassifyLanes(uint32_t mask) {
    auto has = [mask](int ch) { return (mask & LaneMaskOf(ch)) != 0; };
    const bool secondSide = has(0x21) || has(0x22) || has(0x23) || has(0x24) || has(0x25) ||
                            has(0x28) || has(0x29);
    const bool extraKeys = has(0x18) || has(0x19) || has(0x28) || has(0x29);
    if (secondSide) return extraKeys ? ChartLayout::Double14 : ChartLayout::Double10;
    return has(0x18) || has(0x19) ? ChartLayout::Seven : ChartLayout::Five;
}

// The mode (internal name) that plays this layout as written, or "" for the 5-key one.
inline const char* NativeModeForLayout(ChartLayout layout) {
    switch (layout) {
        case ChartLayout::Seven:    return "7keyMix";
        case ChartLayout::Double10: return "clubMix";
        case ChartLayout::Double14: return "spaceMix";
        default:                    return "";
    }
}

// The EZ track a channel lands on when the chart is played as written, in the native layouts the
// shipped charts use (logs/EZ2NativeChartModel.txt): 1P keys T3..T9, 2P keys T12..T18, the
// turntable T10 (both sides' -- no native layout here has a second one). The free zone (17/27)
// and anything unplayable is background audio, T22; -1 = not a note channel at all.
inline int NativeLayoutTrack(int channel) {
    int base = channel;
    if (base >= 0x51 && base <= 0x69) base -= 0x40;
    if (base >= 0x11 && base <= 0x15) return 3 + (base - 0x11);
    if (base == 0x18 || base == 0x19) return 8 + (base - 0x18);
    if (base >= 0x21 && base <= 0x25) return 12 + (base - 0x21);
    if (base == 0x28 || base == 0x29) return 17 + (base - 0x28);
    if (base == 0x16 || base == 0x26) return 10;
    if (base == 0x17 || base == 0x27 || channel == 1) return 22;
    return -1;
}
