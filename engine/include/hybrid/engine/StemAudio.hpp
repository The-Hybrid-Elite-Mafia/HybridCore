#pragma once
// =============================================================================
//  StemAudio — one decoded stem, ready for the audio thread: stereo float at the
//  rig rate (hc::kSampleRate), 64-byte aligned. Built on a loader thread, immutable
//  once handed to MixerProcessor::submitStem(), deleted on the message thread after
//  the audio thread releases it. The audio thread reads only `frames`, `left`, `right`.
// =============================================================================
#include "hybrid/core/AlignedBuffer.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::engine {

struct StemAudio {
    std::uint64_t              frames = 0;     // at hc::kSampleRate
    core::AlignedBuffer<float> left;
    core::AlignedBuffer<float> right;          // == left content for mono sources

    // Description (message thread only).
    std::string   name;                        // file name without extension (UTF-8)
    std::string   path;                        // full path (UTF-8)
    std::string   format;                      // e.g. "WAV 24-bit", "MP3", "FLAC 16-bit"
    std::uint32_t sourceRate     = 0;
    std::uint32_t sourceChannels = 0;
    std::vector<float> overview;               // kOverviewBins peak values 0..1 across the stem (for the song overview)
    static constexpr std::size_t kOverviewBins = 2048;

    StemAudio() noexcept { liveCount().fetch_add(1, std::memory_order_relaxed); }
    ~StemAudio() { liveCount().fetch_sub(1, std::memory_order_relaxed); }
    StemAudio(const StemAudio&)            = delete;
    StemAudio& operator=(const StemAudio&) = delete;

    [[nodiscard]] std::uint64_t bytes() const noexcept { return (left.size() + right.size()) * sizeof(float); }
    // Number of StemAudio objects alive in the process (leak checks in tests).
    static std::atomic<std::int64_t>& liveCount() noexcept {
        static std::atomic<std::int64_t> n{0};
        return n;
    }
};

} // namespace hc::engine
