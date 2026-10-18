#pragma once
// =============================================================================
//  StemMatrix — the 13-stem × 4-bus stereo routing matrix.
//
//  Per stem   : trim (dB) -> ChannelStrip (EQ + compressor) -> polarity invert,
//               balance (-1 L .. +1 R), mute, solo, send level to every bus (dB, -inf = off)
//  Per bus    : gain (dB), mute
//  Metering   : peak + RMS per stem (post-trim/EQ/compressor, pre-fader) and per bus channel;
//               compressor gain reduction per stem
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
#include "hybrid/dsp/ChannelStrip.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace hc::dsp {

// Effect inserts after a stem's channel strip (engine::FxRack). Called on the RT thread only.
class StemInsertHook {
public:
    virtual ~StemInsertHook() = default;
    virtual void beginBlock() noexcept {}                       // once per process() call, before the stems
    [[nodiscard]] virtual bool active(std::uint32_t stem) noexcept = 0;   // this stem goes through process() this block
    // In place, separate L / R buffers, frames <= the matrix's maxFrames.
    virtual void process(std::uint32_t stem, float* left, float* right, std::uint32_t frames) noexcept = 0;
};

class StemMatrix {
public:
    static constexpr std::uint32_t kStems = hc::kStemCount;
    static constexpr std::uint32_t kBuses = hc::kBusCount;
    static constexpr float         kOffDb = -144.0f;   // at/below this a gain is exactly 0

    StemMatrix() noexcept;
    StemMatrix(const StemMatrix&)            = delete;
    StemMatrix& operator=(const StemMatrix&) = delete;

    // ---- message thread ----------------------------------------------------
    void prepare(std::uint32_t maxFrames, double sampleRate = hc::kSampleRate);
    // Offline rendering: start every gain and strip setting at its current target instead of
    // ramping in from silence / defaults on the first block. Message thread, before process().
    void snapToTargets() noexcept;

    void setStemTrimDb(std::uint32_t stem, float db) noexcept;
    void setStemBalance(std::uint32_t stem, float balance) noexcept;   // -1 .. +1
    void setStemInvert(std::uint32_t stem, bool invert) noexcept;
    void setStemMute(std::uint32_t stem, bool mute) noexcept;
    void setStemSolo(std::uint32_t stem, bool solo) noexcept;
    void setSendDb(std::uint32_t stem, std::uint32_t bus, float db) noexcept;
    void setBusGainDb(std::uint32_t bus, float db) noexcept;
    void setBusMute(std::uint32_t bus, bool mute) noexcept;

    // Effect inserts (nullptr = none). Set from the message thread; the hook must outlive the matrix's use.
    void setInsertHook(StemInsertHook* hook) noexcept { hook_.store(hook, std::memory_order_release); }

    // Per-stem EQ + compressor (setters are thread-safe atomics).
    [[nodiscard]] ChannelStrip&       strip(std::uint32_t stem) noexcept { return strips_[stem]; }
    [[nodiscard]] const ChannelStrip& strip(std::uint32_t stem) const noexcept { return strips_[stem]; }

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

    std::array<ChannelStrip, kStems>   strips_{};
    std::atomic<StemInsertHook*>       hook_{nullptr};

    // RT-owned ramp state
    float prevSend_[kStems][kBuses][2]{};
    float prevTrim_[kStems]{};
    bool  viaStrip_[kStems]{};          // last block went through the strip (trim applied in the buffer)
    core::AlignedBuffer<float> stripData_;   // 2 × maxFrames scratch for the stem being processed
    float prevBus_[kBuses]{};

    core::AlignedBuffer<float> busData_;
    std::uint32_t              maxFrames_ = 0;
};

} // namespace hc::dsp
