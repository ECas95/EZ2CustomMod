// Guards WriteFlatAbmFile, the last-resort asset the mod writes when rendering one is impossible.
//
// Asset generation runs on GDI: GetDC, CreateCompatibleDC, CreateDIBSection. All of them can fail,
// and a process only gets 10,000 GDI handles. When generation failed it used to return 0 -- which
// is also what it returns for "already cached" -- and left no file behind. 91 songs in one category
// ended up with an empty songname directory that way, and the game's loader treats an .abm that
// will not open as a licence to run off the end of a 260-byte stack buffer.
//
// So a failure now writes a flat tile instead of nothing. That file has to be a genuinely valid
// .abm, because a malformed one is the same crash by another road. WriteFlatAbmFile and
// AbmFromBmpBytes are copied verbatim from bms_scanner.cpp.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o flat_abm_test tests/flat_abm_test.cpp -lgdi32
//   ./flat_abm_test
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static std::vector<unsigned char> g_written;
static std::string g_writtenPath;

// stand-in for the real one; the test only cares about the bytes
static bool writeBinaryFile(const std::string& path, const std::vector<unsigned char>& data) {
    g_writtenPath = path;
    g_written = data;
    return true;
}

// ---- verbatim from bms_scanner.cpp ------------------------------------------------------------
static bool AbmFromBmpBytes(std::vector<unsigned char>& b) {
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    uint16_t bpp = 0, width = 0, height = 0;
    memcpy(&bpp, b.data() + 28, 2);
    memcpy(&width, b.data() + 18, 2);
    memcpy(&height, b.data() + 22, 2);
    b[0] = 'A'; b[1] = 'W';
    memcpy(b.data() + 2, &bpp, 2);
    b[4] = 0; b[5] = 0;
    memcpy(b.data() + 6, &width, 2);
    memcpy(b.data() + 8, &height, 2);
    const uint32_t xors[] = {0x109a, 0xcfa1, 0x51ae, 0xb18f};
    const size_t offsets[] = {10, 18, 22, 28};
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        memcpy(&v, b.data() + offsets[i], 4);
        v ^= xors[i];
        memcpy(b.data() + offsets[i], &v, 4);
    }
    return true;
}

static bool WriteFlatAbmFile(const std::string& abmPath, int w, int h, int bpp, unsigned char level) {
    if (w <= 0 || h <= 0 || (bpp != 16 && bpp != 24)) return false;
    const int bytesPerPixel = bpp / 8;
    const int rowBytes = ((w * bytesPerPixel + 3) & ~3);
    const size_t pixelBytes = (size_t)rowBytes * (size_t)h;
    std::vector<unsigned char> out(54 + pixelBytes, level);
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = 54;
    bfh.bfSize = (DWORD)out.size();
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = (WORD)bpp;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)pixelBytes;
    memcpy(out.data(), &bfh, sizeof(bfh));
    memcpy(out.data() + sizeof(bfh), &bih, sizeof(bih));
    if (!AbmFromBmpBytes(out)) return false;
    return writeBinaryFile(abmPath, out);
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// What the game's loader reads back out of the header: magic, then bpp/width/height at the
// positions the .abm transform restates them in.
struct AbmHeader { char magic[2]; uint16_t bpp, width, height; size_t size; };
static AbmHeader parse(const std::vector<unsigned char>& b) {
    AbmHeader h{};
    h.magic[0] = (char)b[0]; h.magic[1] = (char)b[1];
    memcpy(&h.bpp, b.data() + 2, 2);
    memcpy(&h.width, b.data() + 6, 2);
    memcpy(&h.height, b.data() + 8, 2);
    h.size = b.size();
    return h;
}

int main() {
    // Every size the mod actually writes a fallback for.
    struct Case { const char* role; int w, h, bpp; };
    const Case cases[] = {
        { "songname",   256,  32, 24 },
        { "disc",       256, 256, 24 },
        { "discsmall",  128, 128, 24 },
        { "resultdisc", 128, 128, 16 },
    };
    for (const Case& c : cases) {
        check(WriteFlatAbmFile("x.abm", c.w, c.h, c.bpp, 0), "writes a tile");
        AbmHeader h = parse(g_written);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s: magic is AW", c.role);
        check(h.magic[0] == 'A' && h.magic[1] == 'W', msg);
        std::snprintf(msg, sizeof(msg), "%s: header says %dx%d", c.role, c.w, c.h);
        check(h.width == (uint16_t)c.w && h.height == (uint16_t)c.h, msg);
        std::snprintf(msg, sizeof(msg), "%s: header says %d bpp", c.role, c.bpp);
        check(h.bpp == (uint16_t)c.bpp, msg);

        const int rowBytes = ((c.w * (c.bpp / 8) + 3) & ~3);
        std::snprintf(msg, sizeof(msg), "%s: size is 54 + %d rows of %d", c.role, c.h, rowBytes);
        check(h.size == 54u + (size_t)rowBytes * (size_t)c.h, msg);
    }

    // The transform must actually have run: a plain BMP would still start with "BM".
    {
        WriteFlatAbmFile("x.abm", 256, 32, 24, 0);
        check(g_written[0] != 'B' || g_written[1] != 'M', "the BMP magic is gone, so the XOR ran");
        // Offsets 10/18/22/28 are XORed, so at least one must differ from the plain BMP value.
        uint32_t off10 = 0;
        memcpy(&off10, g_written.data() + 10, 4);
        check(off10 != 54u, "the pixel-data offset was XORed rather than left plain");
    }

    // A fill level reaches the pixels, and only the pixels.
    {
        WriteFlatAbmFile("x.abm", 64, 8, 24, 0x7F);
        bool allFilled = true;
        for (size_t i = 54; i < g_written.size(); ++i)
            if (g_written[i] != 0x7F) allFilled = false;
        check(allFilled, "every pixel byte carries the requested level");
    }

    // Refusals: anything it cannot express must fail rather than write a malformed file.
    {
        g_written.clear();
        check(!WriteFlatAbmFile("x.abm", 0, 32, 24, 0), "a zero width is refused");
        check(!WriteFlatAbmFile("x.abm", 256, 0, 24, 0), "a zero height is refused");
        check(!WriteFlatAbmFile("x.abm", 256, 32, 8, 0), "an unsupported depth is refused");
        check(!WriteFlatAbmFile("x.abm", -4, 32, 24, 0), "a negative width is refused");
        check(g_written.empty(), "and none of those wrote anything");
    }

    // Odd widths still land on a 4-byte row stride, which the loader assumes.
    {
        WriteFlatAbmFile("x.abm", 13, 5, 24, 0);
        const int rowBytes = ((13 * 3 + 3) & ~3);
        check(rowBytes % 4 == 0, "row stride is 4-byte aligned");
        check(g_written.size() == 54u + (size_t)rowBytes * 5u, "odd width still sizes correctly");
    }

    if (g_failures == 0) std::printf("flat abm: ok\n");
    return g_failures ? 1 : 0;
}
