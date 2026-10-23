// Song text decoding (src/text_codec.h): UTF-8, Shift-JIS, EUC-KR and Latin titles.
#include "../src/text_codec.h"
#include <cassert>
#include <cstdio>

int main() {
    // The EZM2 "Tokyo" #TITLE, Shift-JIS bytes: 空想都市トーキョー.
    const std::string tokyo = "\x8b\xf3\x91z\x93s\x8es\x83g\x81[\x83L\x83\x87\x81[";
    assert(DecodeTitleBytes(tokyo) == L"空想都市トーキョー");
    // UTF-8 as written by the importer: 夕闇の地平線.
    assert(DecodeTitleBytes("\xe5\xa4\x95\xe9\x97\x87\xe3\x81\xae\xe5\x9c\xb0\xe5\xb9\xb3\xe7\xb7\x9a") ==
           L"夕闇の地平線");
    // EUC-KR: 안녕 (be c8 b3 e7).
    assert(DecodeTitleBytes("\xbe\xc8\xb3\xe7") == L"안녕");
    // Latin-1 accents stay on the ANSI code page (1252 here).
    assert(DecodeTitleBytes("Caf\xe9") == L"Café");
    assert(DecodeTitleBytes("Plain ASCII") == L"Plain ASCII");
    assert(DecodeTitleBytes("").empty());

    int s = 0, k = 0;
    LegacyCjkScores(tokyo, s, k);
    assert(s > 0 && k == 0);

    assert(NeedsCjkFace(L"ト") && NeedsCjkFace(L"안") && !NeedsCjkFace(L"Café"));
    assert(FaceFor(L"ASCII") == std::wstring(L"Arial"));
    std::puts("text_codec_test OK");
    return 0;
}
