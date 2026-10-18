// HEM STRIP — channel strip. Idea (not code) from the classic all-in-one channel strip:
// low cut → gate / expander → compressor → 3-band EQ → output, in that order.
// Our design: 12 dB/oct low cut; a gate with 3 dB hysteresis, fast open (0.5 ms) and smooth
// close (80 ms) down to its range; a stereo-linked soft-knee compressor (peak detector,
// attack / release in dB); low shelf 100 Hz, mid bell (sweepable, Q 0.9), high shelf 10 kHz.
// Filter coefficients follow 20 ms-smoothed settings, recomputed every 32 samples.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { LowCut, GateThr, GateRange, CompThr, Ratio, Attack, Release, Low, Mid, MidFreq, High, Output, kCount };
const ParamInfo kParams[kCount] = {
    {"lowcut", "Low Cut", "Hz", 20.0f, 400.0f, 20.0f, Taper::Log},
    {"gatethr", "Gate Threshold", "dB", -80.0f, 0.0f, -80.0f},
    {"gaterange", "Gate Range", "dBr", 0.0f, 80.0f, 60.0f},
    {"compthr", "Comp Threshold", "dB", -60.0f, 0.0f, 0.0f},
    {"ratio", "Comp Ratio", ":1", 1.0f, 20.0f, 3.0f, Taper::Log},
    {"attack", "Comp Attack", "ms", 0.1f, 100.0f, 10.0f, Taper::Log},
    {"release", "Comp Release", "ms", 10.0f, 1000.0f, 120.0f, Taper::Log},
    {"low", "Low", "dB", -15.0f, 15.0f, 0.0f},
    {"mid", "Mid", "dB", -15.0f, 15.0f, 0.0f},
    {"midfreq", "Mid Freq", "Hz", 200.0f, 8000.0f, 1000.0f, Taper::Log},
    {"high", "High", "dB", -15.0f, 15.0f, 0.0f},
    {"output", "Output", "dB", -24.0f, 24.0f, 0.0f},
};

constexpr float kLowHz = 100.0f, kHighHz = 10000.0f, kMidQ = 0.9f, kKneeDb = 6.0f, kGateOff = -79.9f;

inline float gainComputer(float x, float t, float r, float w) noexcept {
    const float o = x - t;
    if (2.0f * o < -w) return 0.0f;
    if (2.0f * o > w) return o / r - o;
    const float k = o + w * 0.5f;
    return (1.0f / r - 1.0f) * k * k / (2.0f * w);
}

class Strip final : public Effect {
public:
    Strip() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        hpf_.clear(); low_.clear(); mid_.clear(); high_.clear();
        sLowCut_.prepare(fs_, 20, p(LowCut)); sLow_.prepare(fs_, 20, p(Low)); sMid_.prepare(fs_, 20, p(Mid));
        sMidF_.prepare(fs_, 20, p(MidFreq)); sHigh_.prepare(fs_, 20, p(High)); out_.prepare(fs_, 20, dbToGain(p(Output)));
        updateFilters();
        gateEnv_ = 0.0f; gateOpen_ = false; gateGain_ = p(GateThr) <= kGateOff ? 1.0f : 0.0f;
        gr_ = 0.0f; tick_ = 0;
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float gThr = p(GateThr), gFloor = dbToGain(-p(GateRange));
        const bool gateOn = gThr > kGateOff;
        const float cThr = p(CompThr), ratio = p(Ratio);
        const float att = onePoleCoef(fs_, p(Attack)), rel = onePoleCoef(fs_, p(Release));
        const float gEnvAtt = onePoleCoef(fs_, 1.0), gEnvRel = onePoleCoef(fs_, 50.0);
        const float gOpen = onePoleCoef(fs_, 0.5), gClose = onePoleCoef(fs_, 80.0);
        const float oT = dbToGain(p(Output));
        float deepest = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            // Settings glide per sample; filters follow every 32 samples (block-size independent).
            (void)sLowCut_.next(p(LowCut)); (void)sLow_.next(p(Low)); (void)sMid_.next(p(Mid));
            (void)sMidF_.next(p(MidFreq)); (void)sHigh_.next(p(High));
            if ((tick_++ & 31u) == 0) updateFilters();
            float l = hpf_.l.tick(L[i]), r = hpf_.r.tick(R[i]);
            const float a = std::max(std::fabs(l), std::fabs(r));
            // Gate.
            gateEnv_ = a + (a > gateEnv_ ? gEnvAtt : gEnvRel) * (gateEnv_ - a);
            float gTarget = 1.0f;
            if (gateOn) {
                const float envDb = gainToDb(gateEnv_);
                if (gateOpen_ ? envDb < gThr - 3.0f : envDb > gThr) gateOpen_ = !gateOpen_;
                gTarget = gateOpen_ ? 1.0f : gFloor;
            }
            gateGain_ = gTarget + (gTarget > gateGain_ ? gOpen : gClose) * (gateGain_ - gTarget);
            // Compressor (detects after the gate).
            const float target = gainComputer(gainToDb(a * gateGain_), cThr, ratio, kKneeDb);
            gr_ = target + (target < gr_ ? att : rel) * (gr_ - target);
            deepest = std::min(deepest, gr_ + std::max(-80.0f, gainToDb(gateGain_)));   // the meter shows the gate too
            const float g = gateGain_ * dbToGain(gr_);
            l *= g; r *= g;
            // EQ, output.
            l = high_.l.tick(mid_.l.tick(low_.l.tick(l)));
            r = high_.r.tick(mid_.r.tick(low_.r.tick(r)));
            const float o = out_.next(oT);
            L[i] = l * o; R[i] = r * o;
        }
        publishReduction(deepest);
    }

private:
    void updateFilters() noexcept {
        using T = BiquadCoefs::Type;
        hpf_.set(BiquadCoefs::make(T::HighPass, fs_, sLowCut_.cur, 0.7071));
        low_.set(BiquadCoefs::make(T::LowShelf, fs_, kLowHz, 0.7071, sLow_.cur));
        mid_.set(BiquadCoefs::make(T::Bell, fs_, sMidF_.cur, kMidQ, sMid_.cur));
        high_.set(BiquadCoefs::make(T::HighShelf, fs_, kHighHz, 0.7071, sHigh_.cur));
    }
    double fs_ = 48000;
    Biquad2 hpf_, low_, mid_, high_;
    Smoothed sLowCut_, sLow_, sMid_, sMidF_, sHigh_, out_;
    float gateEnv_ = 0.0f, gateGain_ = 1.0f, gr_ = 0.0f;
    bool gateOpen_ = false;
    std::uint32_t tick_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeStrip() { return std::make_unique<Strip>(); }

} // namespace hc::fx
