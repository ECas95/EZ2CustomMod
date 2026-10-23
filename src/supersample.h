#pragma once
#include <vector>
#include <cstddef>

// GDI will not antialias Arial at the 9-15 px the mod's panels use (the font's own gasp table
// turns smoothing off there), so text and rounded corners came out as hard 1-bit edges next to
// the game's pre-smoothed art. The panels are drawn at s times the size and averaged back down.
//
// src: 24-bit rows of w*s pixels (w*s*3 bytes, a multiple of 4 whenever w*3 is), h*s rows.
inline void BoxDownsample(const unsigned char* src, int w, int h, int s, std::vector<unsigned char>& out) {
    out.assign((size_t)w * h * 3, 0);
    const size_t stride = (size_t)w * s * 3;
    const unsigned n = (unsigned)(s * s);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                unsigned sum = 0;
                for (int dy = 0; dy < s; ++dy) {
                    const unsigned char* row = src + (size_t)(y * s + dy) * stride + (size_t)x * s * 3 + c;
                    for (int dx = 0; dx < s; ++dx) sum += row[dx * 3];
                }
                out[((size_t)y * w + x) * 3 + c] = (unsigned char)((sum + n / 2) / n);
            }
}
