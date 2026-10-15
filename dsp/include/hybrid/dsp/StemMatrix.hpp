#pragma once
// =============================================================================
//  StemMatrix — the 13-stem × 4-bus stereo routing matrix.
//
//  Per stem   : trim (dB), polarity invert, balance (-1 L .. +1 R), mute, solo,
//               send level to every bus (dB, -inf = off)
//  Per bus    : gain (dB), mute
//  Metering   : peak + RMS per stem (post-trim, pre-fader) and per bus channel
//
//  Threading
//    Setters / meter getters : any non-RT thread (message/UI). Wait-free atomics.
//    prepare()               : message thread, before the matrix is used on the RT thread.
//    process()               : RT thread only. No allocation, no locks.
//  Every gain change is ramped linearly across one block (no zipper noise).
//  Silent routes (gain 0 before and after) are skipped entirely.
// =============================================================================
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/Config.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace hc::dsp {

class StemMatrix {
public:
    static constexpr std::uint32_t kStems = hc::kStemCount;
    static constexpr std::uint32_t kBuses = hc::kBusCount;
    static constexpr float         kOffDb = -144.0f;   // at/below this a gain is exactly 0

    StemMatrix() noexcept;
    StemMatrix(const StemMatrix&)            = delete;
    StemMatrix& operator=(const StemMatrix&) = delete;

    // ---- message thread ----------------------------------------------------
    void prepare(std::uint32_t maxFrames);

    void setStemTrimDb(std::uint32_t stem, float db) noexcept;
    void setStemBalance(std::uint32_t stem, float balance) noexcept;   // -1 .. +1
    void setStemInvert(std::uint32_t stem, bool invert) noexcept;
    void setStemMute(std::uint32_t stem, bool mute) noexcept;
    void setStemSolo(std::uint32_t stem, bool solo) noexcept;
    void setSendDb(std::uint32_t stem, std::uint32_t bus, float db) noexcept;
    void setBusGainDb(std::uint32_t bus, float db) noexcept;
    void setBusMute(std::uint32_t bus, bool mute) noexcept;

    [[nodiscard]] float stemPeak(std::uint32_t stem) const noexcept;
    [[nodiscard]] float stemRms(std::uint32_t stem) const noexcept;
    [[nodiscard]] float busPeak(std::uint32_t bus, std::uint32_t channel) const noexcept;
    [[nodiscard]] float busRms(std::uint32_t bus, std::uint32_t channel) const noexcept;

    // ---- RT thread -----------------------------------------------------------
    // stemL/stemR: kStems pointers each; nullptr = silent stem; stemR[s] may equal stemL[s] (mono).
    void process(const float* const* stemL, const float* const* stemR, std::uint32_t frames) noexcept;

    // Bus output written by the last process() call. channel 0 = L, 1 = R.
    [[nodiscard]] const float* busChannel(std::uint32_t bus, std::uint32_t channel) const noexcept {
        return busData_.data() + (static_cast<std::size_t>(bus) * 2 + channel) * maxFrames_;
    }
    [[nodiscard]] std::uint32_t maxFrames() const noexcept { return maxFrames_; }

    // Pure helpers (exposed for tests).
    [[nodiscard]] static float dbToGain(float db) noexcept;
    static void balanceGains(float balance, float& gL, float& gR) noexcept;

private:
    struct StemParams {
        std::atomic<float> trim{1.0f};
        std::atomic<float> balL{1.0f};
        std::atomic<float> balR{1.0f};
        std::atomic<bool>  invert{false};
        std::atomic<bool>  mute{false};
        std::atomic<bool>  solo{false};
        std::array<std::atomic<float>, kBuses> send{};
    };
    struct BusParams {
        std::atomic<float> gain{1.0f};
        std::atomic<bool>  mute{false};
    };
    struct Meter {
        std::atomic<float> peak{0.0f};
        std::atomic<float> rms{0.0f};
    };

    std::array<StemParams, kStems>     stems_{};
    std::array<BusParams, kBuses>      buses_{};
    std::array<Meter, kStems>          stemMeters_{};
    std::array<Meter, kBuses * 2>      busMeters_{};

    // RT-owned ramp state
    float prevSend_[kStems][kBuses][2]{};
    float prevBus_[kBuses]{};

    core::AlignedBuffer<float> busData_;
    std::uint32_t              maxFrames_ = 0;
};

} // namespace hc::dsp
