#pragma once
// =============================================================================
//  MixExporter — renders the song to a WAV file, offline, on a background thread.
//
//  The stems are decoded again from their files (the live copies belong to the audio
//  thread) and run through their own MixerProcessor with a snapshot of every parameter
//  taken when the export starts — the same matrix, EQ, compressors and limiter code as
//  live playback, so the file is what you hear (minus your audio interface).
//  Output: the Main bus, 48 kHz stereo, 24-bit PCM (rounded) or 32-bit float, exactly
//  as long as the longest stem (or the Beat Forge song, plus 2 s for its reverb / delay tails),
//  starting on its first sample (the limiter look-ahead is compensated; no fade-in).
//  Optional last step: raise the mix to a loudness target (see Loudness.hpp).
//  Rendering never touches the live audio thread.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/Loudness.hpp"

#include <array>
#include <atomic>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hc::media {

struct ExportRequest {
    std::array<std::filesystem::path, hc::kStemCount> stems{};   // empty = slot not used
    std::string           params;                                // ParameterTree::serialize() snapshot (mixer + forge.*)
    std::string           forgeSong;                             // forge::songToText(); used when forge.on
    std::string           fxRack;                                // engine::FxRack::toText(): effect inserts on the stems
    std::filesystem::path out;
    WavEncoding           encoding = WavEncoding::Pcm24;
    // Loudness step after the render: raise a quiet mix to masterSettings.targetLufs with a
    // limiter at the ceiling (never turns a loud mix down). Off = the file is the Main bus as-is.
    bool                  master = false;
    MasterSettings        masterSettings{};
};

struct ExportResult {
    std::filesystem::path file;
    std::uint64_t frames   = 0;
    float         peak     = 0.0f;   // linear, of the written mix
    std::uint64_t clipped  = 0;      // samples over full scale (only possible with the limiter off; PCM clips them)
    double        seconds  = 0.0;    // wall time
    std::string   warning;           // e.g. stems that could not be read
    MasterResult  master;            // loudness before / after (lufsBefore is measured even with master off)
};

// Synchronous render (used by the exporter thread and the tests). progress(0..1) may cancel.
[[nodiscard]] std::expected<ExportResult, std::string> renderMix(const ExportRequest& request, const Progress& progress = {});

class MixExporter {
public:
    MixExporter() = default;
    ~MixExporter();   // cancels and joins
    MixExporter(const MixExporter&)            = delete;
    MixExporter& operator=(const MixExporter&) = delete;

    // False if an export is already running.
    bool start(ExportRequest request);
    void cancel() noexcept { cancel_.store(true); }
    [[nodiscard]] bool  busy() const noexcept { return busy_.load(); }
    [[nodiscard]] float progress() const noexcept { return progress_.load(); }   // 0..1, -1 when idle
    // Takes the finished result once (message thread).
    [[nodiscard]] std::optional<std::expected<ExportResult, std::string>> poll();

private:
    std::thread         thread_;
    std::atomic<bool>   busy_{false};
    std::atomic<bool>   cancel_{false};
    std::atomic<float>  progress_{-1.0f};
    std::mutex          mutex_;
    std::optional<std::expected<ExportResult, std::string>> done_;
};

} // namespace hc::media
