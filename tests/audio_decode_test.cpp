// Guards the in-process keysound decoders (stb_vorbis / minimp3) and the PCM converter.
//
// Compressed keysounds used to go through Media Foundation one file at a time -- and on XP,
// where there is no Media Foundation, through one ffmpeg.exe process per file. These decode in
// process. Pinned here: the converter's arithmetic, and (when the library is present) that a
// real .ogg and a real .mp3 decode to plausible audio.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -static -o audio_decode_test tests/audio_decode_test.cpp src/audio_decode.cpp
//   ./audio_decode_test [path\to\file.ogg] [path\to\file.mp3]
#include "../src/audio_decode.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// First file with the extension under a directory tree, or "".
static std::string findFirst(const std::string& dir, const char* ext, int depth = 0) {
    if (depth > 5) return "";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found;
    do {
        if (fd.cFileName[0] == '.') continue;
        std::string p = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) found = findFirst(p, ext, depth + 1);
        else {
            size_t n = strlen(ext), l = p.size();
            if (l > n && _stricmp(p.c_str() + l - n, ext) == 0) found = p;
        }
    } while (found.empty() && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

int main(int argc, char** argv) {
    printf("audio decode\n");

    // ---- the converter, on synthetic input ---------------------------------------------------
    {
        // 1 second of mono 22050 Hz: a ramp, so interpolation is checkable.
        std::vector<unsigned char> pcm(22050 * 2);
        short* s = reinterpret_cast<short*>(pcm.data());
        for (int i = 0; i < 22050; ++i) s[i] = (short)(i % 1000);
        uint32_t ch = 1, rate = 22050;
        ConvertPcm16(pcm, ch, rate, 2, 44100);
        check(ch == 2 && rate == 44100, "mono 22050 becomes stereo 44100");
        check(pcm.size() == 44100 * 2 * 2, "and holds exactly one second of it");
        const short* o = reinterpret_cast<const short*>(pcm.data());
        check(o[0] == o[1], "mono is duplicated into both channels");
        check(o[2] == 0 && o[4] == 1 && o[6] == 1, "frames in between are interpolated (0, 0.5->0, 1)");
        uint32_t ch2 = 2, rate2 = 44100;
        size_t before = pcm.size();
        ConvertPcm16(pcm, ch2, rate2, 2, 44100);
        check(pcm.size() == before, "already-matching audio is left alone");
    }
    {
        std::vector<unsigned char> pcm;
        uint32_t ch = 0, rate = 0, bits = 0;
        check(!DecodeAudioInProcess("nothing.wav", 0, pcm, ch, rate, bits), "a .wav is not ours to decode");
        check(!DecodeAudioInProcess("missing.ogg", 0, pcm, ch, rate, bits), "a missing .ogg fails cleanly");
    }

    // ---- real files, when the library is around ----------------------------------------------
    std::string root = "C:\\EZ2AC\\EZ2CustomMod_v25_category_gate_silent_load_fix\\EZ2AC_FNEX\\BMSData";
    std::string ogg = argc > 1 ? argv[1] : findFirst(root, ".ogg");
    std::string mp3 = argc > 2 ? argv[2] : findFirst(root, ".mp3");
    for (int pass = 0; pass < 2; ++pass) {
        const std::string& f = pass ? mp3 : ogg;
        const char* kind = pass ? "mp3" : "ogg";
        if (f.empty()) { printf("  (no .%s found to test; skipped)\n", kind); continue; }
        std::vector<unsigned char> pcm;
        uint32_t ch = 0, rate = 0, bits = 0;
        DWORD t0 = GetTickCount();
        bool ok = DecodeAudioInProcess(f, 0, pcm, ch, rate, bits);
        DWORD ms = GetTickCount() - t0;
        printf("  %s: %s -> ch=%lu rate=%lu bits=%lu bytes=%lu in %lu ms\n", kind, f.c_str(),
               (unsigned long)ch, (unsigned long)rate, (unsigned long)bits, (unsigned long)pcm.size(), (unsigned long)ms);
        check(ok && !pcm.empty(), pass ? "a real .mp3 decodes" : "a real .ogg decodes");
        check(ch >= 1 && ch <= 2 && rate >= 8000 && rate <= 48000 && bits == 16, "to a plausible 16-bit format");
        // Something in there is not silence.
        const short* s = reinterpret_cast<const short*>(pcm.data());
        bool loud = false;
        for (size_t i = 0; i < pcm.size() / 2; ++i) if (s[i] > 500 || s[i] < -500) { loud = true; break; }
        check(loud, "and is not silence");
        std::vector<unsigned char> head;
        ok = DecodeAudioInProcess(f, 1, head, ch, rate, bits);
        check(ok && head.size() <= (size_t)rate * ch * 2 + 8192 && head.size() <= pcm.size(),
              "maxSeconds=1 stops early");
    }

    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
