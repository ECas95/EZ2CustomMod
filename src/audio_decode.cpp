#include "audio_decode.h"
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// Both decoders are compiled into this one translation unit and nowhere else.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "../third_party/stb_vorbis.c"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "../third_party/minimp3_ex.h"
#pragma GCC diagnostic pop

static bool endsWithNoCase(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

// Whole file into memory. Shared read: the game itself, the scanner and a prefetch thread may all
// have the same keysound open at once.
static bool readWholeFile(const std::string& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 30);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD total = 0;
        while (total < out.size()) {
            DWORD got = 0;
            if (!ReadFile(h, out.data() + total, (DWORD)(out.size() - total), &got, nullptr) || !got) { ok = false; break; }
            total += got;
        }
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

static bool decodeOgg(const std::vector<unsigned char>& file, uint32_t maxSeconds,
                      std::vector<unsigned char>& pcm, uint32_t& channels, uint32_t& rate) {
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(file.data(), (int)file.size(), &error, nullptr);
    if (!v) return false;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    channels = (uint32_t)info.channels;
    rate = info.sample_rate;
    if (!channels || channels > 8 || !rate) { stb_vorbis_close(v); return false; }
    const unsigned total = stb_vorbis_stream_length_in_samples(v);
    const size_t maxFrames = maxSeconds ? (size_t)maxSeconds * rate : (size_t)-1;
    pcm.reserve((size_t)std::min<size_t>(total ? total : 4096, maxFrames) * channels * 2);
    short chunk[4096 * 2];
    const int chunkFrames = (int)(sizeof(chunk) / sizeof(chunk[0]) / channels);
    size_t frames = 0;
    for (;;) {
        int got = stb_vorbis_get_samples_short_interleaved(v, (int)channels, chunk, chunkFrames * (int)channels);
        if (got <= 0) break;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(chunk);
        pcm.insert(pcm.end(), b, b + (size_t)got * channels * 2);
        frames += (size_t)got;
        if (frames >= maxFrames) break;
    }
    stb_vorbis_close(v);
    return !pcm.empty();
}

static bool decodeMp3(const std::vector<unsigned char>& file, uint32_t maxSeconds,
                      std::vector<unsigned char>& pcm, uint32_t& channels, uint32_t& rate) {
    mp3dec_ex_t dec;
    if (mp3dec_ex_open_buf(&dec, file.data(), file.size(), MP3D_SEEK_TO_SAMPLE) != 0) return false;
    channels = (uint32_t)dec.info.channels;
    rate = (uint32_t)dec.info.hz;
    if (!channels || channels > 2 || !rate) { mp3dec_ex_close(&dec); return false; }
    // dec.samples counts interleaved samples across channels.
    size_t want = (size_t)dec.samples;
    if (maxSeconds) want = std::min<size_t>(want, (size_t)maxSeconds * rate * channels);
    pcm.resize(want * 2);
    size_t got = mp3dec_ex_read(&dec, reinterpret_cast<mp3d_sample_t*>(pcm.data()), want);
    mp3dec_ex_close(&dec);
    pcm.resize(got * 2);
    return got > 0;
}

bool DecodeAudioInProcess(const std::string& path, uint32_t maxSeconds,
                          std::vector<unsigned char>& pcm,
                          uint32_t& channels, uint32_t& rate, uint32_t& bits) {
    pcm.clear(); channels = rate = bits = 0;
    const bool ogg = endsWithNoCase(path, ".ogg");
    const bool mp3 = endsWithNoCase(path, ".mp3");
    if (!ogg && !mp3) return false;
    std::vector<unsigned char> file;
    if (!readWholeFile(path, file)) return false;
    bool ok = ogg ? decodeOgg(file, maxSeconds, pcm, channels, rate)
                  : decodeMp3(file, maxSeconds, pcm, channels, rate);
    if (!ok) { pcm.clear(); channels = rate = 0; return false; }
    bits = 16;
    return true;
}

void ConvertPcm16(std::vector<unsigned char>& pcm, uint32_t& channels, uint32_t& rate,
                  uint32_t outChannels, uint32_t outRate) {
    if (!channels || !rate || !outChannels || !outRate) return;
    if (channels == outChannels && rate == outRate) return;
    const short* in = reinterpret_cast<const short*>(pcm.data());
    const size_t inFrames = pcm.size() / (channels * 2);
    if (inFrames < 2) { pcm.clear(); channels = outChannels; rate = outRate; return; }
    const size_t outFrames = (size_t)((unsigned long long)inFrames * outRate / rate);
    std::vector<unsigned char> out(outFrames * outChannels * 2);
    short* o = reinterpret_cast<short*>(out.data());
    // ponytail: linear interpolation, no low-pass -- fine for a 12-second hover preview and for
    // keysounds whose rate already sits near 44.1k; a windowed-sinc resampler if it ever shows.
    for (size_t f = 0; f < outFrames; ++f) {
        const double pos = (double)f * rate / outRate;
        size_t i0 = (size_t)pos;
        if (i0 >= inFrames - 1) i0 = inFrames - 2;
        const double t = pos - (double)i0;
        for (uint32_t c = 0; c < outChannels; ++c) {
            // Fold or duplicate channels: mono -> both, stereo -> stereo, more -> first two.
            const uint32_t sc = c < channels ? c : (channels == 1 ? 0 : c % channels);
            const double a = in[i0 * channels + sc], b = in[(i0 + 1) * channels + sc];
            o[f * outChannels + c] = (short)(a + (b - a) * t);
        }
    }
    pcm.swap(out);
    channels = outChannels;
    rate = outRate;
}
