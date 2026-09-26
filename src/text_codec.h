#pragma once
// Song text (a BMS #TITLE / #ARTIST, a category name) into something GDI can draw on any machine.
//
// DECODING. A BMS declares no encoding. Newer packs are UTF-8, the classic Japanese ones Shift-JIS
// (cp932), Korean ones EUC-KR (cp949). The old fallback for "not UTF-8" was the system ANSI code
// page, which is 1252 on both the dev PC and the XP cabinet, so every Shift-JIS title came out as
// Latin mojibake ("空想都市トーキョー" drew as "‹ó'z"s"Žsƒg[ƒLƒ‡["). Both legacy code pages
// accept most of each other's byte pairs, so the choice is made by what the pairs spell: kana
// (lead 0x82/0x83) and kanji leads for Shift-JIS, the 2,350 common Hangul syllables
// (lead 0xB0-0xC8, trail 0xA1-0xFE) for EUC-KR. Neither found -> the ANSI code page, which is
// right for the accented Latin titles.
//
// GLYPHS. Arial has no kana, kanji or Hangul, and the cabinet's XP has no East Asian font to link
// to, so decoded text still drew as boxes there. The mod ships WenQuanYi Micro Hei (Apache 2.0,
// TrueType collection: Latin + CJK + kana + Hangul in one face) next to EZ2B.exe and loads it
// privately to the process -- nothing is installed in Windows. It has to carry Latin too: XP does
// no font fallback for a private face, and titles mix scripts ("stem / HALLPBE.S VS チョビ");
// Droid Sans Fallback alone drew the Latin half as boxes. Lines that need it use it on every
// machine, so the PC and the cabinet render the same.
#include <windows.h>
#include <string>

static const wchar_t kCjkFace[] = L"WenQuanYi Micro Hei";
static const char kCjkFontFile[] = "wqy-microhei.ttc";

// Byte pairs that read as Japanese (Shift-JIS) or as Korean (EUC-KR). Pure, for the test.
inline void LegacyCjkScores(const std::string& s, int& sjis, int& euckr) {
    sjis = euckr = 0;
    const size_t n = s.size();
    for (size_t i = 0; i + 1 < n;) {
        const unsigned char a = (unsigned char)s[i];
        if ((a >= 0x81 && a <= 0x9F) || (a >= 0xE0 && a <= 0xFC)) {
            if (a == 0x82 || a == 0x83) sjis += 3;                  // hiragana / katakana
            else if ((a >= 0x88 && a <= 0x9F) || (a >= 0xE0 && a <= 0xEA)) sjis += 1;   // kanji
            i += 2;
        } else ++i;
    }
    for (size_t i = 0; i + 1 < n;) {
        const unsigned char a = (unsigned char)s[i], b = (unsigned char)s[i + 1];
        if (a >= 0x81) {
            if (a >= 0xB0 && a <= 0xC8 && b >= 0xA1 && b <= 0xFE) euckr += 3;   // common Hangul
            else if (a >= 0xCA && a <= 0xFD && b >= 0xA1 && b <= 0xFE) euckr += 1;  // Hanja
            i += 2;
        } else ++i;
    }
}

inline bool DecodeStrict(UINT cp, const std::string& s, std::wstring& out) {
    const int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.assign((size_t)n, L'\0');
    return MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), &out[0], n) == n;
}

inline std::wstring DecodeTitleBytes(const std::string& s) {
    std::wstring w;
    if (s.empty() || DecodeStrict(CP_UTF8, s, w)) return w;
    int sjis = 0, euckr = 0;
    LegacyCjkScores(s, sjis, euckr);
    if (sjis > 0 && sjis >= euckr && DecodeStrict(932, s, w)) return w;
    if (euckr > 0 && DecodeStrict(949, s, w)) return w;
    if (sjis > 0 && DecodeStrict(932, s, w)) return w;
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    w.assign((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline bool NeedsCjkFace(const std::wstring& w) {
    // Anything past Latin Extended-B other than ordinary punctuation: CJK, kana, Hangul, full width,
    // and also symbols (star, note, arrows) -- the XP cabinet's Arial 2.x has far fewer of those
    // than the PC's, and this keeps both machines on the same glyphs.
    for (wchar_t c : w) if (c > 0x024F && !(c >= 0x2000 && c <= 0x206F)) return true;
    return false;
}

// The face for this line: the shipped CJK font when the line needs it and it loaded, else Arial.
inline const wchar_t* FaceFor(const std::wstring& w) {
    static int loaded = -1;
    if (!NeedsCjkFace(w)) return L"Arial";
    if (loaded < 0) {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path = exe;
        path = path.substr(0, path.find_last_of("\\/") + 1) + kCjkFontFile;
        loaded = AddFontResourceExA(path.c_str(), FR_PRIVATE, nullptr) > 0 ? 1 : 0;
    }
    return loaded ? kCjkFace : L"Arial";
}
