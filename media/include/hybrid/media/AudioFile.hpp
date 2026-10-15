#pragma once
// =============================================================================
//  hc::media — audio file decoding and WAV writing (loader / message threads only;
//  never on the audio thread: these allocate and read files).
//
//  Decoders (format detected from the file's bytes, not its extension):
//    WAV / RF64 : PCM 8/16/24/32-bit integer, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE (own parser)
//    MP3        : dr_mp3 0.7.3 (gapless: LAME/Xing encoder delay and padding are trimmed)
//    FLAC       : dr_flac 0.13.3
//  Output is planar float at the file's own sample rate; mono is duplicated to both sides,
//  files with more than two channels keep the first two (reported in `note`).
// =============================================================================
#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hc::media {

struct DecodedAudio {
    std::uint32_t      sampleRate = 0;
    std::uint32_t      channels   = 0;   // in the file
    std::vector<float> left, right;      // same length; frames = left.size()
    std::string        format;           // "WAV 24-bit", "WAV 32-bit float", "MP3", "FLAC 16-bit"...
    std::string        note;             // e.g. "6 channels: using 1+2"
    [[nodiscard]] std::uint64_t frames() const noexcept { return left.size(); }
};

// progress(0..1) is called from the decoding thread; returning false cancels.
using Progress = std::function<bool(float)>;

[[nodiscard]] std::expected<DecodedAudio, std::string> decodeFile(const std::filesystem::path& file, const Progress& progress = {});
[[nodiscard]] std::expected<DecodedAudio, std::string> decodeMemory(const std::uint8_t* data, std::size_t size, const Progress& progress = {});

// "wav", "mp3", "flac" or "" from the first bytes.
[[nodiscard]] std::string sniffFormat(const std::uint8_t* data, std::size_t size) noexcept;

enum class WavEncoding : std::uint8_t { Pcm16, Pcm24, Float32 };
// Writes a RIFF WAV (mono when right == nullptr). PCM is TPDF-free plain rounding with clipping.
bool writeWav(const std::filesystem::path& file, const float* left, const float* right, std::uint64_t frames,
              std::uint32_t sampleRate, WavEncoding encoding);

// File extensions the loader offers in dialogs / accepts from drag and drop.
[[nodiscard]] bool isAudioFileName(const std::filesystem::path& file);

} // namespace hc::media
