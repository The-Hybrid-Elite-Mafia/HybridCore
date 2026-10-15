#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/dsp/Kernels.hpp"

#include <cmath>
#include <numbers>

namespace hc::engine {

namespace {
constexpr double kToneLevel = 0.031622776601683794;   // -30 dBFS: 13 tones summed stay below 0 dBFS
} // namespace

MixerProcessor::MixerProcessor(Source source) noexcept : source_(source) {}

double MixerProcessor::testToneHz(std::uint32_t stem) noexcept {
    return 110.0 * std::pow(2.0, 3.0 * static_cast<double>(stem) / 12.0);
}

void MixerProcessor::prepare(const io::StreamConfig& config) {
    maxFrames_ = config.blockFrames;
    matrix_.prepare(maxFrames_);
    toneData_.allocate(static_cast<std::size_t>(dsp::StemMatrix::kStems) * maxFrames_);
    for (std::uint32_t s = 0; s < dsp::StemMatrix::kStems; ++s) {
        const double w = 2.0 * std::numbers::pi * testToneHz(s) / static_cast<double>(config.sampleRate);
        osc_[s] = Osc{std::cos(w), std::sin(w), 1.0, 0.0};
    }
}

void MixerProcessor::process(const ProcessContext& ctx) noexcept {
    if (ctx.frames == 0 || ctx.frames > maxFrames_) return;
    constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;

    std::array<const float*, kStems> L{};
    std::array<const float*, kStems> R{};

    if (source_.load(std::memory_order_relaxed) == Source::TestTones) {
        for (std::uint32_t s = 0; s < kStems; ++s) {
            float* out = toneData_.data() + static_cast<std::size_t>(s) * maxFrames_;
            Osc&   o   = osc_[s];
            double x = o.x, y = o.y;
            for (std::uint32_t i = 0; i < ctx.frames; ++i) {
                const double nx = o.c * x - o.s * y;
                y = o.s * x + o.c * y;
                x = nx;
                out[i] = static_cast<float>(y * kToneLevel);
            }
            const double r = 1.0 / std::sqrt(x * x + y * y);
            o.x = x * r;
            o.y = y * r;
            L[s] = R[s] = out;
        }
    } else {
        for (std::uint32_t s = 0; s < kStems; ++s) {
            const std::uint32_t l = 2 * s, r = 2 * s + 1;
            L[s] = (l < ctx.numInputs && ctx.inputs) ? ctx.inputs[l] : nullptr;
            R[s] = (r < ctx.numInputs && ctx.inputs) ? ctx.inputs[r] : L[s];
        }
    }

    matrix_.process(L.data(), R.data(), ctx.frames);

    for (std::uint32_t b = 0; b < dsp::StemMatrix::kBuses; ++b)
        for (std::uint32_t c = 0; c < 2; ++c) {
            const std::uint32_t out = 2 * b + c;
            if (out < ctx.numOutputs) dsp::copy(ctx.outputs[out], matrix_.busChannel(b, c), ctx.frames);
        }
}

} // namespace hc::engine
