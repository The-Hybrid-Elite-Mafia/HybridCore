#pragma once
// =============================================================================
//  BusLimiter — look-ahead brick-wall limiter on the Main bus, after the matrix.
//
//  All 4 buses (8 channels) go through the same delay (1 ms = 48 samples at 48 kHz),
//  so Main and the Aux buses stay sample-aligned; only Main is limited.
//  The delay is always there, on or off, so switching the limiter never jumps in time;
//  "off" just lets the gain glide back to unity.
//
//  Gain: required gain per sample = ceiling / peak(L,R) (stereo-linked); a sliding
//  minimum over the look-ahead window, instant attack + 100 ms release, then a moving
//  average over the window. The average starts falling one window before a peak arrives,
//  so the delayed peak is already at or under the ceiling (a final clamp catches float
//  rounding). No allocation in process(): every buffer is sized in prepare().
// =============================================================================
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/Config.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace hc::dsp {

class BusLimiter {
public:
    static constexpr std::uint32_t kChannels    = hc::kBusCount * 2;
    static constexpr std::uint32_t kMaxLook     = 256;     // enough for 1 ms at 192 kHz
    static constexpr float         kLookaheadMs = 1.0f;
    static constexpr float         kReleaseMs   = 100.0f;

    BusLimiter() noexcept;
    BusLimiter(const BusLimiter&)            = delete;
    BusLimiter& operator=(const BusLimiter&) = delete;

    void prepare(double sampleRate, std::uint32_t maxFrames);   // message thread

    void setEnabled(bool on) noexcept       { enabled_.store(on, std::memory_order_relaxed); }
    void setCeilingDb(float db) noexcept;
    [[nodiscard]] bool  enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    [[nodiscard]] float gainReductionDb() const noexcept { return grPub_.load(std::memory_order_relaxed); }   // >= 0
    [[nodiscard]] std::uint32_t latency() const noexcept { return look_; }
    // Peak of the Main output (after the limiter) in the last block, channel 0 = L, 1 = R.
    [[nodiscard]] float mainPeak(std::uint32_t c) const noexcept { return outPeak_[c & 1].load(std::memory_order_relaxed); }

    // Audio thread. in[2b + c] = bus b channel c (nullptr = silence); frames <= maxFrames.
    void process(const float* const* in, std::uint32_t frames) noexcept;
    [[nodiscard]] const float* channel(std::uint32_t bus, std::uint32_t c) const noexcept {
        return out_.data() + static_cast<std::size_t>(bus * 2 + c) * maxFrames_;
    }

private:
    void resetState() noexcept;

    std::atomic<bool>  enabled_{false};
    std::atomic<float> ceiling_{1.0f};
    std::atomic<float> grPub_{0.0f};
    std::array<std::atomic<float>, 2> outPeak_{};

    core::AlignedBuffer<float> out_;
    std::uint32_t maxFrames_ = 0;
    std::uint32_t look_      = 48;
    float         relK_      = 0.0f;

    // Audio-thread state.
    std::array<std::array<float, kMaxLook>, kChannels> delay_{};
    std::array<float, kMaxLook>          envRing_{};
    std::array<float, 2 * kMaxLook>      dqVal_{};    // sliding-minimum queue (ring)
    std::array<std::uint64_t, 2 * kMaxLook> dqIdx_{};
    std::uint32_t dqHead_ = 0, dqTail_ = 0;           // tail = one past the newest
    std::uint32_t pos_    = 0;
    std::uint64_t n_      = 0;
    float         dist_   = 0.0f;   // 1 - envelope
    double        sum_    = 0.0;
};

} // namespace hc::dsp
