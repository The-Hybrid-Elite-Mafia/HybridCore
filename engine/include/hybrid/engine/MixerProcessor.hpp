#pragma once
// =============================================================================
//  MixerProcessor — runs the 13-stem matrix inside the engine.
//
//  Stem sources (until file playback arrives in Phase 6):
//    Hardware  : stem s <- input channels 2s / 2s+1 (mono if only 2s exists, silent if neither)
//    TestTones : stem s <- internal sine, A2 (110 Hz) stacked in minor thirds, -30 dBFS each
//  Bus b is written to output channels 2b / 2b+1 when the device has them (bus 0 = Main = outs 1/2).
//
//  matrix() setters and meters are callable from the message thread while the processor
//  is installed in the engine; lifetime ends when the engine retires the processor.
// =============================================================================
#include "hybrid/engine/AudioProcessor.hpp"
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/dsp/StemMatrix.hpp"

#include <array>
#include <atomic>

namespace hc::engine {

class MixerProcessor final : public AudioProcessor {
public:
    enum class Source : std::uint8_t { Hardware, TestTones };

    explicit MixerProcessor(Source source = Source::Hardware) noexcept;

    void setSource(Source s) noexcept { source_.store(s, std::memory_order_relaxed); }
    [[nodiscard]] Source source() const noexcept { return source_.load(std::memory_order_relaxed); }
    [[nodiscard]] dsp::StemMatrix&       matrix() noexcept { return matrix_; }
    [[nodiscard]] const dsp::StemMatrix& matrix() const noexcept { return matrix_; }

    // Test-tone frequency for a stem (Hz). Exposed for tests and UI labels.
    [[nodiscard]] static double testToneHz(std::uint32_t stem) noexcept;

    void prepare(const io::StreamConfig& config) override;
    void process(const ProcessContext& ctx) noexcept override;

private:
    struct Osc { double c = 1.0, s = 0.0, x = 1.0, y = 0.0; };

    std::atomic<Source>                               source_;
    dsp::StemMatrix                                   matrix_;
    core::AlignedBuffer<float>                        toneData_;
    std::array<Osc, dsp::StemMatrix::kStems>          osc_{};
    std::uint32_t                                     maxFrames_ = 0;
};

} // namespace hc::engine
