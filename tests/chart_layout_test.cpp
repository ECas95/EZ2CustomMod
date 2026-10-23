// Chart layouts and the modes that play them as written (src/chart_layout.h).
#include "../src/chart_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t Mask(std::initializer_list<int> chs) {
    uint32_t m = 0;
    for (int c : chs) m |= LaneMaskOf(c);
    return m;
}

int main() {
    assert(LaneBit(0x11) == 0 && LaneBit(0x19) == 8 && LaneBit(0x21) == 9 && LaneBit(0x29) == 17);
    assert(LaneBit(0x51) == 0 && LaneBit(0x68) == 16);          // long-note channels fold
    assert(LaneBit(0x01) == -1 && LaneBit(0x04) == -1);

    // The layouts the library survey found.
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15})) == ChartLayout::Five);
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x16})) == ChartLayout::Five);
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19})) == ChartLayout::Seven);
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x18})) == ChartLayout::Seven);   // MUS 6T
    assert(ClassifyLanes(Mask({0x12, 0x13, 0x14, 0x15, 0x18})) == ChartLayout::Seven);         // KRA
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x21, 0x22, 0x23, 0x24, 0x25})) == ChartLayout::Double10);
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x23})) == ChartLayout::Double10);
    assert(ClassifyLanes(Mask({0x11, 0x12, 0x13, 0x14, 0x15, 0x21, 0x22, 0x23, 0x24, 0x25, 0x28})) == ChartLayout::Double14);
    assert(ClassifyLanes(Mask({0x11, 0x18, 0x21})) == ChartLayout::Double14);
    assert(ClassifyLanes(0) == ChartLayout::Five);

    assert(!strcmp(NativeModeForLayout(ChartLayout::Seven), "7keyMix"));
    assert(!strcmp(NativeModeForLayout(ChartLayout::Double10), "clubMix"));
    assert(!strcmp(NativeModeForLayout(ChartLayout::Double14), "spaceMix"));
    assert(!strcmp(NativeModeForLayout(ChartLayout::Five), ""));

    // As-written tracks: keys 1-7 on T3..T9, 2P on T12..T18, turntable T10, free zone to background.
    const int p1[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19};
    for (int i = 0; i < 7; ++i) assert(NativeLayoutTrack(p1[i]) == 3 + i);
    const int p2[] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x28, 0x29};
    for (int i = 0; i < 7; ++i) assert(NativeLayoutTrack(p2[i]) == 12 + i);
    assert(NativeLayoutTrack(0x53) == 5 && NativeLayoutTrack(0x69) == 18);
    assert(NativeLayoutTrack(0x16) == 10 && NativeLayoutTrack(0x26) == 10);
    assert(NativeLayoutTrack(0x17) == 22 && NativeLayoutTrack(0x01) == 22);
    assert(NativeLayoutTrack(0x04) == -1 && NativeLayoutTrack(0x03) == -1);
    std::puts("chart_layout_test OK");
    return 0;
}
