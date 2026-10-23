// Guards how a song title is laid out on the 256x32 songname asset.
//
// Two things went wrong with long titles, and only one of them was cosmetic:
//
//   - the fit loop stepped the font down to 8px and, if the string still did not fit, fell out
//     holding no font and RETURNED. Titles past that length drew nothing at all. That is the
//     blank rows in the wheel, not a rendering subtlety.
//   - everything else was squeezed onto one line until it stopped being readable.
//
// Native songname art is 256x32 with a single band of 10px caps at y 13..22, right-aligned, ink
// ending around x=247 -- so there is room for a second line, and these checks make sure the two of
// them stay inside the canvas and keep a legible cap height.
//
// The layout is copied from GenerateBmpFile in bms_scanner.cpp.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o songname_layout_test tests/songname_layout_test.cpp -lgdi32 -luser32
//   ./songname_layout_test
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

// ---- the layout, copied from GenerateBmpFile ---------------------------------------------------
struct Rendered { int lines; int inkTop, inkBottom, inkLeft, inkRight; int capHeight; bool any; };

static Rendered render(const std::string& title, int w = 256, int h = 32) {
    Rendered out{};
    HDC screen = GetDC(NULL);
    HDC hdc = CreateCompatibleDC(screen);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = -h;
    bih.biPlanes = 1; bih.biBitCount = 24; bih.biCompression = BI_RGB;
    BITMAPINFO bi{}; bi.bmiHeader = bih;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldb = (HBITMAP)SelectObject(hdc, bmp);
    RECT full = {0, 0, w, h};
    HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &full, br);
    DeleteObject(br);
    SetBkMode(hdc, TRANSPARENT);

    const int plainWeight = (w >= 256) ? FW_BOLD : FW_NORMAL;
    const UINT align = (w <= 128 ? DT_CENTER : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    const int maxPx = (h <= 16) ? 11 : 13;
    const int avail = (w - 2) - 3;

    std::string l1 = title, l2;
    if (h >= 32 && !l1.empty()) {
        HFONT probe = CreateFontA(-maxPx, 0, 0, 0, plainWeight, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, "Arial");
        HFONT prev = (HFONT)SelectObject(hdc, probe);
        SIZE whole{};
        GetTextExtentPoint32A(hdc, l1.c_str(), (int)l1.size(), &whole);
        if (whole.cx > avail) {
            size_t best = std::string::npos, bestDelta = l1.size();
            for (size_t i = 0; i < l1.size(); ++i) {
                if (l1[i] != ' ') continue;
                const size_t left = i, right = l1.size() - i - 1;
                const size_t delta = left > right ? left - right : right - left;
                if (delta < bestDelta) { bestDelta = delta; best = i; }
            }
            if (best != std::string::npos) { l2 = l1.substr(best + 1); l1 = l1.substr(0, best); }
        }
        SelectObject(hdc, prev);
        DeleteObject(probe);
    }
    out.lines = l2.empty() ? 1 : 2;

    auto drawLine = [&](const std::string& text, RECT r, int mx) {
        if (text.empty()) return;
        HFONT font = nullptr, oldFont = nullptr;
        for (int px = mx; ; --px) {
            font = CreateFontA(-px, 0, 0, 0, plainWeight, FALSE, FALSE, FALSE, ANSI_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, "Arial");
            oldFont = (HFONT)SelectObject(hdc, font);
            SIZE size{};
            GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &size);
            if (px <= 8 || (size.cx <= r.right - r.left && size.cy <= r.bottom - r.top)) break;
            SelectObject(hdc, oldFont); DeleteObject(font); font = nullptr;
        }
        if (!font) return;
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextA(hdc, text.c_str(), -1, &r, align);
        SelectObject(hdc, oldFont); DeleteObject(font);
    };
    RECT r1 = {3, 1, w - 2, l2.empty() ? h - 1 : h / 2};
    RECT r2 = {3, h / 2, w - 2, h - 1};
    drawLine(l1, r1, maxPx);
    drawLine(l2, r2, maxPx);

    const int row = ((w * 3 + 3) & ~3);
    std::vector<unsigned char> px2((size_t)row * h);
    BITMAPINFOHEADER bu = bih; bu.biHeight = h; bu.biSizeImage = row * h;
    GetDIBits(hdc, bmp, 0, h, px2.data(), (BITMAPINFO*)&bu, DIB_RGB_COLORS);
    out.inkTop = h; out.inkBottom = -1; out.inkLeft = w; out.inkRight = -1;
    std::vector<int> rowsWithInk;
    for (int y = 0; y < h; ++y) {
        bool ink = false;
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px2.data() + (size_t)(h - 1 - y) * row + x * 3;
            if (p[0] + p[1] + p[2] <= 60) continue;
            ink = true; out.any = true;
            if (y < out.inkTop) out.inkTop = y;
            if (y > out.inkBottom) out.inkBottom = y;
            if (x < out.inkLeft) out.inkLeft = x;
            if (x > out.inkRight) out.inkRight = x;
        }
        if (ink) rowsWithInk.push_back(y);
    }
    // Cap height of the tallest single band, which is what "legible" comes down to here.
    int best = 0, run = 0;
    for (size_t i = 0; i < rowsWithInk.size(); ++i) {
        run = (i && rowsWithInk[i] == rowsWithInk[i - 1] + 1) ? run + 1 : 1;
        if (run > best) best = run;
    }
    out.capHeight = best;
    SelectObject(hdc, oldb); DeleteObject(bmp); DeleteDC(hdc); ReleaseDC(NULL, screen);
    return out;
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

int main() {
    // Real titles from the installed library, shortest to longest.
    const char* titles[] = {
        "%X",
        "AnXiens (2023)",
        "Waku Waku Week",
        "Across the Sky",
        "Alla Turca(TSAR remix )",
        "WAO WAO Powerful Mix",
        "Wake up , Challenger (Per Ver ) Another Ver",
        "Come Lead Beautiful Festival ( 2/2 ) Extended Long Title Version",
    };
    std::printf("  native reference: 256x32, one band of 10px caps at y 13..22, ink to x=247\n\n");
    std::printf("  %-46s %s\n", "title", "lines  ink y      ink x        cap");
    for (const char* t : titles) {
        Rendered r = render(t);
        std::printf("  %-46.46s   %d    y[%2d..%2d]  x[%3d..%3d]  %2dpx\n",
                    t, r.lines, r.inkTop, r.inkBottom, r.inkLeft, r.inkRight, r.capHeight);

        char msg[160];
        std::snprintf(msg, sizeof(msg), "'%.40s' renders something at all", t);
        check(r.any, msg);

        // Nothing may touch the edges of the 256x32 canvas.
        std::snprintf(msg, sizeof(msg), "'%.40s' stays inside the canvas", t);
        check(r.any && r.inkTop >= 1 && r.inkBottom <= 30 && r.inkLeft >= 1 && r.inkRight <= 254, msg);

        // And it has to stay readable: native caps are 10px, so hold a floor near that.
        std::snprintf(msg, sizeof(msg), "'%.40s' keeps a legible cap height", t);
        check(r.capHeight >= 7, msg);
    }

    // Short titles must NOT be split -- the second line is for titles that need it.
    check(render("%X").lines == 1, "a short title stays on one line");
    check(render("AnXiens (2023)").lines == 1, "a normal title stays on one line");

    // Long ones must be.
    check(render("Wake up , Challenger (Per Ver ) Another Ver").lines == 2,
          "a title too wide for one line is split across two");

    // The pathological case: no spaces to split on, so it has to fall back to one clipped line
    // rather than drawing nothing.
    {
        Rendered r = render("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        check(r.lines == 1, "a title with no spaces cannot be split");
        check(r.any, "and it is still drawn, clipped, rather than left blank");
    }

    // Two lines must not overlap each other.
    {
        Rendered r = render("Wake up , Challenger (Per Ver ) Another Ver");
        check(r.inkBottom - r.inkTop + 1 <= 30, "both lines fit within the canvas height");
    }

    if (g_failures == 0) std::printf("\nsongname layout: ok\n");
    return g_failures ? 1 : 0;
}
