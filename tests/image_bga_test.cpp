// Layout and timeline of image BGA (src/image_bga.h).
#include "../src/image_bga.h"
#include <cassert>
#include <cstdio>

static bool Is(BgaRect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }

int main() {
    assert(Is(ImageBgaRect(640, 480), 0, 0, 640, 480));
    assert(Is(ImageBgaRect(256, 192), 0, 0, 640, 480));       // 4:3 scales up to fill
    assert(Is(ImageBgaRect(512, 512), 80, 0, 480, 480));      // square: bars left and right
    assert(Is(ImageBgaRect(256, 256), 80, 0, 480, 480));
    assert(Is(ImageBgaRect(600, 400), 0, 26, 640, 427));      // 3:2: thin bars top and bottom
    assert(Is(ImageBgaRect(1280, 720), -106, 0, 853, 480));   // 16:9: sides cropped, like video
    assert(Is(ImageBgaRect(854, 480), -107, 0, 854, 480));
    assert(Is(ImageBgaRect(0, 0), 0, 0, 640, 480));

    const std::vector<uint32_t> ms = {0, 100, 100, 250};
    assert(ImageBgaFrameAt(ms, 0) == 0);
    assert(ImageBgaFrameAt(ms, 99) == 0);
    assert(ImageBgaFrameAt(ms, 100) == 2);                    // same tick: the later event wins
    assert(ImageBgaFrameAt(ms, 249) == 2);
    assert(ImageBgaFrameAt(ms, 100000) == 3);
    assert(ImageBgaFrameAt(std::vector<uint32_t>{0}, 5) == 0);
    std::puts("image_bga_test OK");
    return 0;
}
