// HEM BUS PRESS — bus / glue compressor. Idea (not code) from the classic console bus
// compressor: stereo-linked VCA-style gain, a few musical ratios, auto release, mix knob.
// Our design: detector high-pass (so the kick doesn't pump the mix) → peak level in dB →
// soft-knee (6 dB) gain computer → attack / release smoothing in dB (auto release = a fast
// and a slow release stage, the deeper one wins) → makeup → dry / wet mix. No latency.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Threshold, Ratio, Attack, Release, AutoRel, Makeup, Mix, ScLowCut, kCount };
const char* const kRatioNames[] = {"1.5:1", "2:1", "4:1", "10:1"};
constexpr float kRatios[] = {1.5f, 2.0f, 4.0f, 10.0f};
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"threshold", "Threshold", "dB", -40.0f, 0.0f, -12.0f},
    {"ratio", "Ratio", "", 0.0f, 3.0f, 1.0f, Taper::Choice, kRatioNames},
    {"attack", "Attack", "ms", 0.1f, 30.0f, 10.0f, Taper::Log},
    {"release", "Release", "ms", 50.0f, 1200.0f, 300.0f, Taper::Log},
    {"auto", "Auto Release", "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"makeup", "Makeup", "dB", -6.0f, 24.0f, 0.0f},
    {"mix", "Mix", "%", 0.0f, 100.0f, 100.0f},
    {"sclowcut", "SC Low Cut", "Hz", 20.0f, 300.0f, 20.0f, Taper::Log},
};

constexpr float kKneeDb = 6.0f;

// Gain change in dB (<= 0) for a level x dB.
inline float gainComputer(float x, float t, float r, float w) noexcept {
    const float o = x - t;
    if (2.0f * o < -w) return 0.0f;
    if (2.0f * o > w) return o / r - o;
    const float k = o + w * 0.5f;
    return (1.0f / r - 1.0f) * k * k / (2.0f * w);
}

class BusPress final : public Effect {
public:
    BusPress() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        scL_.clear(); scR_.clear();
        scHz_ = -1.0f;
        updateFilter();
        gr_ = grSlow_ = 0.0f;
        makeup_.prepare(fs_, 20, dbToGain(p(Makeup)));
        mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        tick_ = 0;
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float thr = p(Threshold), ratio = kRatios[std::clamp(static_cast<int>(p(Ratio)), 0, 3)];
        const bool autoRel = p(AutoRel) >= 0.5f;
        const float att = onePoleCoef(fs_, p(Attack)), rel = onePoleCoef(fs_, p(Release));
        const float relFast = onePoleCoef(fs_, 60.0), relSlow = onePoleCoef(fs_, 1200.0), attSlow = onePoleCoef(fs_, 250.0);
        const float mkT = dbToGain(p(Makeup)), mxT = p(Mix) * 0.01f;
        float deepest = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            if ((tick_++ & 31u) == 0) updateFilter();
            const float l = L[i], r = R[i];
            const float lvl = std::max(std::fabs(scL_.tick(l)), std::fabs(scR_.tick(r)));
            const float target = gainComputer(gainToDb(lvl), thr, ratio, kKneeDb);
            if (autoRel) {
                gr_ = target + (target < gr_ ? att : relFast) * (gr_ - target);
                grSlow_ = target + (target < grSlow_ ? attSlow : relSlow) * (grSlow_ - target);
            } else {
                gr_ = target + (target < gr_ ? att : rel) * (gr_ - target);
                grSlow_ = 0.0f;
            }
            const float g = std::min(gr_, grSlow_);
            deepest = std::min(deepest, g);
            const float wet = dbToGain(g) * makeup_.next(mkT), m = mix_.next(mxT);
            L[i] = l * (1.0f - m) + l * wet * m;
            R[i] = r * (1.0f - m) + r * wet * m;
        }
        publishReduction(deepest);
    }

private:
    void updateFilter() noexcept {
        const float hz = p(ScLowCut);
        if (hz == scHz_) return;
        scHz_ = hz;
        const auto c = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, hz, 0.7071);
        scL_.c = c; scR_.c = c;
    }
    double fs_ = 48000;
    Biquad scL_, scR_;
    float scHz_ = -1.0f;
    float gr_ = 0.0f, grSlow_ = 0.0f;
    Smoothed makeup_, mix_;
    std::uint32_t tick_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeBusPress() { return std::make_unique<BusPress>(); }

} // namespace hc::fx
