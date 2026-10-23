// Panels drawn at s x size and averaged back down.
#include "../src/supersample.h"
#include <cassert>
#include <cstdio>

int main() {
    // 2x1 output from a 4x2 source (s = 2): left block all 200, right block half 0 / half 255.
    const unsigned char src[] = {
        200,200,200, 200,200,200,   0,  0,  0, 255,255,255,
        200,200,200, 200,200,200,   0,  0,  0, 255,255,255,
    };
    std::vector<unsigned char> out;
    BoxDownsample(src, 2, 1, 2, out);
    assert(out.size() == 6);
    assert(out[0] == 200 && out[1] == 200 && out[2] == 200);
    assert(out[3] == 128 && out[4] == 128 && out[5] == 128);   // (0+255+0+255)/4 rounds to 128

    // Channels stay apart (BGR order preserved).
    const unsigned char bgr[] = { 10, 20, 30 };
    BoxDownsample(bgr, 1, 1, 1, out);
    assert(out[0] == 10 && out[1] == 20 && out[2] == 30);
    std::puts("supersample_test OK");
    return 0;
}
