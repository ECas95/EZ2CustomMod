#pragma once
#include <cstdint>
#include <string>
#include <vector>

// In-process decoders for the two compressed formats the library uses: .ogg (stb_vorbis) and
// .mp3 (minimp3). Both are single-file public-domain C, so they run on XP and cost no process
// spawn or Media Foundation start-up per file. Output is 16-bit interleaved PCM at the file's own
// rate and channel count. Returns false for any other extension or a damaged file, so the caller
// can fall through to Media Foundation / ffmpeg.
//
// maxSeconds > 0 stops decoding after that much audio (the hover preview only needs a few seconds).
// `cancel`, when given, is polled between chunks (about a second of audio apiece); a nonzero
// value abandons the decode and returns false, so a background decode can be stopped quickly.
bool DecodeAudioInProcess(const std::string& path, uint32_t maxSeconds,
                          std::vector<unsigned char>& pcm,
                          uint32_t& channels, uint32_t& rate, uint32_t& bits,
                          volatile long* cancel = nullptr);

// Linear resample + channel fold/duplicate of 16-bit PCM into 16-bit `outChannels` at `outRate`.
// The .ssf writer needs stereo 44.1k because the engine's preview mixer ignores the header rate.
void ConvertPcm16(std::vector<unsigned char>& pcm, uint32_t& channels, uint32_t& rate,
                  uint32_t outChannels, uint32_t outRate);
