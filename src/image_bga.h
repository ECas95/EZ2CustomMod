#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>

// Image BGA: a BMS whose channel 04 points at pictures (#BMPxx a.png) instead of a movie. 980
// songs do this -- RHY, POPS, PIU.1, EZM2 -- almost all with one still image, a few EZM2 charts
// with an animation of several hundred frames. bms_scanner.cpp plays them through the same
// 640x480 BGR24 slot the ffmpeg video path fills; this is the arithmetic it needs.

struct BgaRect { int x, y, w, h; };

// Where a srcW x srcH picture goes on the dstW x dstH screen. 16:9 is handled as the video path
// handles it -- fill the height and crop the sides -- and anything else is fitted whole, centred,
// with black bars: a 512x512 square stretched to 4:3 would be visibly squashed.
inline BgaRect ImageBgaRect(int srcW, int srcH, int dstW = 640, int dstH = 480) {
    if (srcW <= 0 || srcH <= 0) return BgaRect{0, 0, dstW, dstH};
    const long long a = (long long)srcW * 9, b = (long long)srcH * 16;
    const bool wide = std::llabs(a - b) * 100 <= (a > b ? a : b) * 3;
    const double sx = (double)dstW / srcW, sy = (double)dstH / srcH;
    const double s = wide ? sy : (sx < sy ? sx : sy);
    const int w = (int)(srcW * s + 0.5), h = (int)(srcH * s + 0.5);
    return BgaRect{(dstW - w) / 2, (dstH - h) / 2, w, h};
}

// The picture on screen `elapsedMs` after the first one: the last event at or before that time.
// `ms` is sorted and starts at 0.
inline size_t ImageBgaFrameAt(const std::vector<uint32_t>& ms, uint32_t elapsedMs) {
    size_t lo = 0, hi = ms.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (ms[mid] <= elapsedMs) lo = mid; else hi = mid;
    }
    return lo;
}
