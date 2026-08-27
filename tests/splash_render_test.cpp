// Checks that a generated category sprite puts its ink where the shipped art puts it.
//
// The complaint this guards against was visual: "OTH es el tamanio correcto y las custom estan con
// otro tamanio menor", and later an overlay that drew nothing at all. Both were geometry, so the
// geometry is measured here instead of judged by eye.
//
// Reference measurements, taken from the game's own assets:
//   System/SongSelect/Sortimage/category_NN.abm   128x16, ink in y 2..12           (11px caps)
//   System/SongSelect/V_View/sortstyle/sortstyle_NN.bmp
//                                                 256x64, white  y 18..31          (14px caps)
//                                                         orange y 43..50          (8px caps)
// All 30 sortstyle files place their ink identically, so these are the format, not one sample.
//
// The render body is copied verbatim from RenderSplashBytes in bms_scanner.cpp (which is static),
// the same way lane_assign_test.cpp copies chooseAutoLane.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o splash_render_test tests/splash_render_test.cpp -lgdi32 -luser32
//   ./splash_render_test
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ---- verbatim from bms_scanner.cpp ----------------------------------------------------------
static bool RenderSplashBytes(std::vector<unsigned char>& out, const std::wstring& category,
                              const std::wstring& subtext, int w, int h,
                              bool alignRight, bool twoLine) {
    HDC screen = GetDC(NULL);
    if (!screen) return false;
    HDC hdc = CreateCompatibleDC(screen);
    if (!hdc) { ReleaseDC(NULL, screen); return false; }
    BITMAPINFOHEADER bih{}; bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = -h;
    bih.biPlanes = 1; bih.biBitCount = 24; bih.biCompression = BI_RGB;
    BITMAPINFO bi{}; bi.bmiHeader = bih;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(hdc); ReleaseDC(NULL, screen); return false; }
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    RECT full = {0,0,w,h};
    HBRUSH black = CreateSolidBrush(RGB(0,0,0)); FillRect(hdc, &full, black); DeleteObject(black);
    SetBkMode(hdc, TRANSPARENT);
    auto draw = [&](const std::wstring& txt, RECT r, int maxPx, COLORREF col) {
        if (txt.empty()) return;
        HFONT font = nullptr, oldFont = nullptr;
        for (int px = maxPx < 6 ? 6 : maxPx; ; --px) {
            font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Arial");
            oldFont = (HFONT)SelectObject(hdc, font);
            RECT calc = r; DrawTextW(hdc, txt.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE);
            if (px <= 6 || (calc.right - calc.left) <= (r.right - r.left)) break;
            SelectObject(hdc, oldFont); DeleteObject(font); font = nullptr;
        }
        const UINT fmt = (alignRight ? DT_RIGHT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        if (h <= 16) {
            SetTextColor(hdc, RGB(0,0,0));
            RECT sh = r; sh.top += 1; sh.bottom += 1;
            DrawTextW(hdc, txt.c_str(), -1, &sh, fmt);
        }
        SetTextColor(hdc, col);
        DrawTextW(hdc, txt.c_str(), -1, &r, fmt);
        SelectObject(hdc, oldFont); DeleteObject(font);
    };
    if (!twoLine) {
        draw(category, RECT{2, 0, w - (alignRight ? 3 : 2), h}, h - 1, RGB(235,235,235));
    } else {
        const int pad = w / 16;
        draw(category, RECT{pad, h * 6 / 64,  w - pad, h * 46 / 64}, h * 20 / 64, RGB(255,255,255));
        draw(subtext,  RECT{pad, h * 34 / 64, w - pad, h * 61 / 64}, h * 11 / 64, RGB(255,174,0));
    }
    int rowBytes = ((w * 24 / 8 + 3) & ~3), pixelBytes = rowBytes * h;
    out.assign(pixelBytes, 0);
    BITMAPINFOHEADER bihBU = bih; bihBU.biHeight = h; bihBU.biSizeImage = (DWORD)pixelBytes;
    GetDIBits(hdc, bmp, 0, (UINT)h, out.data(), (BITMAPINFO*)&bihBU, DIB_RGB_COLORS);
    SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc); ReleaseDC(NULL, screen);
    return true;
}
// ---------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

struct Band { int top, bottom, left, right; bool any; };

// Rows are stored bottom-up; y here is measured from the top, like the reference measurements.
static Band inkBand(const std::vector<unsigned char>& px, int w, int h, int rMin, int gMin, int bMax) {
    const int row = ((w * 3 + 3) & ~3);
    Band b{h, -1, w, -1, false};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px.data() + (h - 1 - y) * row + x * 3;
            int B = p[0], G = p[1], R = p[2];
            if (R < rMin || G < gMin || B > bMax) continue;
            b.any = true;
            if (y < b.top) b.top = y;
            if (y > b.bottom) b.bottom = y;
            if (x < b.left) b.left = x;
            if (x > b.right) b.right = x;
        }
    return b;
}

static void report(const char* name, const Band& b) {
    if (!b.any) { std::printf("   %-8s <no ink>\n", name); return; }
    std::printf("   %-8s y[%d..%d] h=%d  x[%d..%d] cx=%.1f\n", name, b.top, b.bottom,
                b.bottom - b.top + 1, b.left, b.right, (b.left + b.right) / 2.0);
}

int main() {
    std::vector<unsigned char> px;

    // Wheel tab: the element the user compared against the native "OTH".
    {
        check(RenderSplashBytes(px, L"CYT-1", L"", 128, 16, false, false), "tab: rendered");
        Band b = inkBand(px, 128, 16, 120, 120, 255);
        report("tab", b);
        check(b.any, "tab: has ink");
        // Native ink is y 2..12. A pixel of slack either side covers font metric differences.
        check(b.top >= 1 && b.top <= 4, "tab: ink starts near the native y=2");
        check(b.bottom - b.top + 1 >= 9, "tab: caps at least 9px, close to the native 11");
        check(b.left > 8 && b.right < 120, "tab: text stays inside the canvas");
    }

    // Category-change overlay against the shipped sortstyle geometry.
    {
        check(RenderSplashBytes(px, L"CYTUS", L"CYTUS CHAPTER 1", 256, 64, false, true),
              "overlay: rendered");
        Band white  = inkBand(px, 256, 64, 200, 200, 255);
        Band orange = inkBand(px, 256, 64, 200, 100, 90);
        report("white", white);
        report("orange", orange);
        check(white.any, "overlay: category line present");
        check(orange.any, "overlay: subtext line present");
        // Native: white y 18..31, orange y 43..50, +/-2px of slack.
        check(white.top >= 16 && white.top <= 20, "overlay: white line starts near native y=18");
        check(white.bottom >= 29 && white.bottom <= 33, "overlay: white line ends near native y=31");
        check(orange.top >= 41 && orange.top <= 45, "overlay: orange line starts near native y=43");
        check(orange.bottom >= 48 && orange.bottom <= 52, "overlay: orange line ends near native y=50");
        check(white.bottom < orange.top, "overlay: the two lines do not touch");
        // Native art is centred on x=128 in both bands.
        check(std::abs((white.left + white.right) / 2 - 128) <= 6, "overlay: white line is centred");
        check(std::abs((orange.left + orange.right) / 2 - 128) <= 6, "overlay: orange line is centred");
    }

    // A name far too long for the canvas must still render something. The old fit loop exited with
    // no font selected in this case and produced a completely blank sprite.
    {
        check(RenderSplashBytes(px, L"AN ABSURDLY LONG CATEGORY NAME THAT CANNOT POSSIBLY FIT",
                                L"", 128, 16, false, false), "overlong: rendered");
        Band b = inkBand(px, 128, 16, 120, 120, 255);
        check(b.any, "overlong: elided rather than blank");
    }

    // An empty subtext leaves the orange line blank without disturbing the white one.
    {
        check(RenderSplashBytes(px, L"NOSUB", L"", 256, 64, false, true), "nosub: rendered");
        Band white  = inkBand(px, 256, 64, 200, 200, 255);
        Band orange = inkBand(px, 256, 64, 200, 100, 90);
        check(white.any, "nosub: category line still drawn");
        check(!orange.any, "nosub: no orange line when details.txt has no subtext");
    }

    if (g_failures == 0) std::printf("splash render: ok\n");
    return g_failures ? 1 : 0;
}
