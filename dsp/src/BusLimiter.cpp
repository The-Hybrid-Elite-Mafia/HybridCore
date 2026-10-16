#include "hybrid/dsp/BusLimiter.hpp"
#include "hybrid/dsp/Kernels.hpp"

#include <algorithm>
#include <cmath>

namespace hc::dsp {

BusLimiter::BusLimiter() noexcept { resetState(); }

void BusLimiter::setCeilingDb(float db) noexcept {
    ceiling_.store(std::pow(10.0f, std::clamp(db, -24.0f, 0.0f) / 20.0f), std::memory_order_relaxed);
}

void BusLimiter::prepare(double sampleRate, std::uint32_t maxFrames) {
    const double fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
    look_      = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(fs * kLookaheadMs * 1e-3)), 1, kMaxLook);
    relK_      = 1.0f - std::exp(-1.0f / static_cast<float>(fs * kReleaseMs * 1e-3));
    maxFrames_ = maxFrames;
    out_.allocate(static_cast<std::size_t>(kChannels) * maxFrames_);
    resetState();
}

void BusLimiter::resetState() noexcept {
    for (auto& d : delay_) d.fill(0.0f);
    envRing_.fill(1.0f);
    dqHead_ = dqTail_ = 0;
    pos_ = 0;
    n_   = 0;
    dist_ = 0.0f;
    sum_ = static_cast<double>(look_);
    grPub_.store(0.0f, std::memory_order_relaxed);
}

void BusLimiter::process(const float* const* in, std::uint32_t frames) noexcept {
    if (frames == 0 || frames > maxFrames_) return;
    constexpr std::uint32_t kQ = 2 * kMaxLook;
    const std::uint32_t L    = look_;
    const bool          on   = enabled_.load(std::memory_order_relaxed);
    const float         ceil = ceiling_.load(std::memory_order_relaxed);
    const float         invL = 1.0f / static_cast<float>(L);
    float               minGain = 1.0f;

    // ---- Main bus: limiter + delay.
    const float* inL = in ? in[0] : nullptr;
    const float* inR = in ? in[1] : nullptr;
    float* oL = out_.data();
    float* oR = oL + maxFrames_;
    auto&  dL = delay_[0];
    auto&  dR = delay_[1];
    std::uint32_t pos = pos_;
    for (std::uint32_t i = 0; i < frames; ++i, ++n_) {
        const float xl = inL ? inL[i] : 0.0f;
        const float xr = inR ? inR[i] : 0.0f;
        const float pk = std::max(std::fabs(xl), std::fabs(xr));
        const float req = (on && pk > ceil) ? ceil / pk : 1.0f;

        // Sliding minimum of the required gain over the last L+1 samples.
        while (dqTail_ != dqHead_ && dqVal_[(dqTail_ + kQ - 1) % kQ] >= req) dqTail_ = (dqTail_ + kQ - 1) % kQ;
        dqVal_[dqTail_] = req;
        dqIdx_[dqTail_] = n_;
        dqTail_ = (dqTail_ + 1) % kQ;
        while (dqIdx_[dqHead_] + L < n_) dqHead_ = (dqHead_ + 1) % kQ;
        const float w = dqVal_[dqHead_];

        // Instant attack, smooth release, then the moving average over L samples.
        // The envelope is kept as its distance below unity, which decays geometrically on release
        // (a float envelope near 1.0 would stall short of unity); under 1e-7 it lands on exactly 1.
        dist_ = std::max(1.0f - w, dist_ * (1.0f - relK_));
        if (dist_ < 1e-7f) dist_ = 0.0f;
        const float env = 1.0f - dist_;
        sum_ += static_cast<double>(env) - envRing_[pos];
        envRing_[pos] = env;
        const float g = std::min(1.0f, static_cast<float>(sum_) * invL);
        minGain = std::min(minGain, g);

        float yl = dL[pos] * g, yr = dR[pos] * g;
        if (on) { yl = std::clamp(yl, -ceil, ceil); yr = std::clamp(yr, -ceil, ceil); }
        oL[i] = yl;
        oR[i] = yr;
        dL[pos] = xl;
        dR[pos] = xr;
        if (++pos == L) {   // once per window: re-add the sum exactly so rounding never drifts
            pos = 0;
            double s = 0.0;
            for (std::uint32_t k = 0; k < L; ++k) s += envRing_[k];
            sum_ = s;
        }
    }

    // ---- Aux buses: the same delay, no gain change.
    for (std::uint32_t c = 2; c < kChannels; ++c) {
        const float* x = in ? in[c] : nullptr;
        float* o = out_.data() + static_cast<std::size_t>(c) * maxFrames_;
        auto&  d = delay_[c];
        std::uint32_t p = pos_;
        for (std::uint32_t i = 0; i < frames; ++i) {
            o[i] = d[p];
            d[p] = x ? x[i] : 0.0f;
            if (++p == L) p = 0;
        }
    }
    pos_ = pos;
    outPeak_[0].store(peakAbs(oL, frames), std::memory_order_relaxed);
    outPeak_[1].store(peakAbs(oR, frames), std::memory_order_relaxed);
    grPub_.store(minGain < 1.0f ? -20.0f * std::log10(minGain) : 0.0f, std::memory_order_relaxed);
}

} // namespace hc::dsp
