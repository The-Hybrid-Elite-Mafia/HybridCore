#include "hybrid/engine/ToneProcessor.hpp"

#include <cmath>
#include <cstring>
#include <numbers>

namespace hc::engine {

ToneProcessor::ToneProcessor(double frequencyHz, double levelDbfs) noexcept
    : frequency_(frequencyHz), amplitude_(std::pow(10.0, levelDbfs / 20.0)) {}

void ToneProcessor::prepare(const io::StreamConfig& config) {
    const double w = 2.0 * std::numbers::pi * frequency_ / static_cast<double>(config.sampleRate);
    cosW_ = std::cos(w);
    sinW_ = std::sin(w);
    x_    = 1.0;
    y_    = 0.0;
}

void ToneProcessor::process(const ProcessContext& ctx) noexcept {
    if (ctx.numOutputs == 0) return;
    float* out = ctx.outputs[0];
    double x = x_, y = y_;
    for (std::uint32_t i = 0; i < ctx.frames; ++i) {
        const double nx = cosW_ * x - sinW_ * y;
        const double ny = sinW_ * x + cosW_ * y;
        x = nx;
        y = ny;
        out[i] = static_cast<float>(y * amplitude_);
    }
    // Renormalise once per block to cancel rounding drift in the rotation.
    const double r = 1.0 / std::sqrt(x * x + y * y);
    x_ = x * r;
    y_ = y * r;

    for (std::uint32_t ch = 1; ch < ctx.numOutputs; ++ch)
        std::memcpy(ctx.outputs[ch], out, ctx.frames * sizeof(float));
}

} // namespace hc::engine
