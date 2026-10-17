#pragma once
// =============================================================================
//  ScopeTap — hands the Main output from the audio thread to the ANALYZER (UI thread).
//
//  Audio thread: push() copies a block into a wait-free SPSC ring (no allocation, no
//  locks, no waiting). When the ring is full (UI stalled) the rest of the block is
//  dropped and counted; audio is never held up. Off by default: push() returns at once
//  until the UI turns the tap on, so a hidden analyzer costs nothing.
//  UI thread: pop() drains what has arrived.
// =============================================================================
#include "hybrid/core/SpscRing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hc::analysis {

struct StereoFrame {
    float l = 0.0f, r = 0.0f;
};

class ScopeTap {
public:
    static constexpr std::size_t kFrames = std::size_t{1} << 16;   // 1.37 s at 48 kHz

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // Audio thread. `right` may equal `left` (mono).
    void push(const float* left, const float* right, std::uint32_t frames) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        for (std::uint32_t i = 0; i < frames; ++i) {
            if (!ring_.tryPush({left[i], right[i]})) {
                dropped_.fetch_add(frames - i, std::memory_order_relaxed);
                return;
            }
        }
    }

    // UI thread: up to `max` frames into `out`; returns how many.
    std::size_t pop(StereoFrame* out, std::size_t max) noexcept {
        std::size_t n = 0;
        while (n < max && ring_.tryPop(out[n])) ++n;
        return n;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool>          enabled_{false};
    std::atomic<std::uint64_t> dropped_{0};
    core::SpscRing<StereoFrame, kFrames> ring_;
};

} // namespace hc::analysis
