// HEM VOX ONE — one-knob vocal chain. Idea (not code) from the one-knob vocal processors:
// turn one control and the vocal gets cleaner, more even and more forward.
// Our design, all driven by AMOUNT (0 = only the low cut, 100 = full treatment):
//   low cut (90 Hz; 150 Hz for BACKING) →
//   opto-style leveler: RMS-ish detector, threshold -6 → -36 dBFS, ratio 1:1 → 6:1,
//     10 ms attack, two-stage program-dependent release (60 ms / 1.5 s, the deeper wins),
//     automatic makeup for a -18 dBFS vocal →
//   de-esser: a 4th-order 5 kHz high band is watched; when it is loud (absolute, and against
//     the whole signal) a 5.5 kHz shelf dips by up to 12 dB (0 dB at rest = untouched) →
//   presence shelf (4 kHz) and air shelf (12 kHz) lifted with AMOUNT →
//   mix, output.
// MODE: SING (smooth), RAP (faster, firmer, more presence), BACKING (thinner, more air).
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Amount, Mode, Output, Mix, kCount };
const char* const kModes[] = {"SING", "RAP", "BACKING"};
const ParamInfo kParams[kCount] = {
    {"amount", "Amount", "%", 0.0f, 100.0f, 40.0f},
    {"mode", "Mode", "", 0.0f, 2.0f, 0.0f, Taper::Choice, kModes},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f},
    {"mix", "Mix", "%", 0.0f, 100.0f, 100.0f},
};

struct ModeShape { float lowCut, ratioMul, attackMs, presence, air; };
constexpr ModeShape kShape[] = {{90.0f, 1.0f, 10.0f, 4.0f, 3.0f}, {90.0f, 1.4f, 5.0f, 5.0f, 2.5f}, {150.0f, 1.2f, 10.0f, 2.5f, 4.5f}};

inline float gainComputer(float x, float t, float r, float w) noexcept {
    const float o = x - t;
    if (2.0f * o < -w) return 0.0f;
    if (2.0f * o > w) return o / r - o;
    const float k = o + w * 0.5f;
    return (1.0f / r - 1.0f) * k * k / (2.0f * w);
}

class VoxOne final : public Effect {
public:
    VoxOne() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        hpf_.clear(); for (auto& b : ds_) b.clear(); pres_.clear(); air_.clear(); dess_.clear();
        amount_.prepare(fs_, 30, p(Amount) * 0.01f);
        out_.prepare(fs_, 20, dbToGain(p(Output)));
        mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        mode_ = static_cast<int>(p(Mode));
        lowCut_ = kShape[mode_].lowCut;
        presDb_ = amount_.cur * kShape[mode_].presence;
        airDb_ = amount_.cur * kShape[mode_].air;
        rms_ = 0.0f; gr_ = grSlow_ = 0.0f; dsEnv_ = fullEnv_ = 0.0f; dsGr_ = 0.0f; tick_ = 0;
        updateFilters(true);
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        mode_ = std::clamp(static_cast<int>(p(Mode)), 0, 2);
        const ModeShape& sh = kShape[mode_];
        const float aT = p(Amount) * 0.01f, oT = dbToGain(p(Output)), mT = p(Mix) * 0.01f;
        const float rmsC = onePoleCoef(fs_, 5.0);
        const float att = onePoleCoef(fs_, sh.attackMs), relFast = onePoleCoef(fs_, 60.0), relSlow = onePoleCoef(fs_, 1500.0),
                    attSlow = onePoleCoef(fs_, 300.0);
        const float dsAtt = onePoleCoef(fs_, 1.0), dsRel = onePoleCoef(fs_, 60.0), envRel = onePoleCoef(fs_, 30.0);
        const float glide = onePoleCoef(fs_, 20.0);
        float deepest = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float a = amount_.next(aT);
            lowCut_ = sh.lowCut + glide * (lowCut_ - sh.lowCut);
            presDb_ = a * sh.presence + glide * (presDb_ - a * sh.presence);
            airDb_ = a * sh.air + glide * (airDb_ - a * sh.air);
            if ((tick_++ & 31u) == 0) updateFilters(false);
            const float dl = L[i], dr = R[i];
            float l = hpf_.l.tick(dl), r = hpf_.r.tick(dr);

            // Leveler.
            const float sq = 0.5f * (l * l + r * r);
            rms_ = sq + rmsC * (rms_ - sq);
            const float lvlDb = 10.0f * std::log10(std::max(rms_ * 2.0f, 1e-18f));   // sine peak-equivalent
            const float thr = -6.0f - 30.0f * a, ratio = 1.0f + 5.0f * a * sh.ratioMul;
            const float target = gainComputer(lvlDb, thr, ratio, 10.0f);
            gr_ = target + (target < gr_ ? att : relFast) * (gr_ - target);
            grSlow_ = target + (target < grSlow_ ? attSlow : relSlow) * (grSlow_ - target);
            const float g = std::min(gr_, grSlow_);
            deepest = std::min(deepest, g);
            const float makeup = -0.5f * gainComputer(-18.0f, thr, ratio, 10.0f);
            const float lg = dbToGain(g + makeup);
            l *= lg; r *= lg;

            // De-esser: split the 5 kHz+ band, turn only it down.
            const float hl = ds_[1].tick(ds_[0].tick(l)), hr = ds_[3].tick(ds_[2].tick(r));
            const float band = std::max(std::fabs(hl), std::fabs(hr)), full = std::max(std::fabs(l), std::fabs(r));
            dsEnv_ = band + (band > dsEnv_ ? dsAtt : envRel) * (dsEnv_ - band);
            fullEnv_ = full + (full > fullEnv_ ? dsAtt : envRel) * (fullEnv_ - full);
            const float dsThr = std::max(-20.0f - 15.0f * a, gainToDb(fullEnv_) - 10.0f);
            const float excess = gainToDb(dsEnv_) - dsThr;
            const float dsRatio = 1.0f + 4.0f * a;
            const float dsTarget = excess > 0.0f ? std::max(-12.0f, -excess * (1.0f - 1.0f / dsRatio)) : 0.0f;
            dsGr_ = dsTarget + (dsTarget < dsGr_ ? dsAtt : dsRel) * (dsGr_ - dsTarget);
            l = dess_.l.tick(l); r = dess_.r.tick(r);   // dynamic 5.5 kHz shelf, gain = dsGr_ (0 dB at rest = untouched)

            // Presence and air.
            l = air_.l.tick(pres_.l.tick(l));
            r = air_.r.tick(pres_.r.tick(r));

            const float m = mix_.next(mT), o = out_.next(oT);
            L[i] = (dl * (1.0f - m) + l * m) * o;
            R[i] = (dr * (1.0f - m) + r * m) * o;
        }
        publishReduction(deepest);
    }

private:
    void updateFilters(bool all) noexcept {
        using T = BiquadCoefs::Type;
        hpf_.set(BiquadCoefs::make(T::HighPass, fs_, lowCut_, 0.7071));
        pres_.set(BiquadCoefs::make(T::HighShelf, fs_, 4000.0, 0.7071, presDb_));
        air_.set(BiquadCoefs::make(T::HighShelf, fs_, 12000.0, 0.7071, airDb_));
        dess_.set(BiquadCoefs::make(T::HighShelf, fs_, 5500.0, 0.7071, dsGr_));
        if (all) {   // fixed 4th-order (two Butterworth-Q sections) 5 kHz high band
            const auto c1 = BiquadCoefs::make(T::HighPass, fs_, 5000.0, 0.5412), c2 = BiquadCoefs::make(T::HighPass, fs_, 5000.0, 1.3066);
            ds_[0].c = c1; ds_[1].c = c2; ds_[2].c = c1; ds_[3].c = c2;
        }
    }
    double fs_ = 48000;
    Biquad2 hpf_, pres_, air_, dess_;
    Biquad ds_[4];
    Smoothed amount_, out_, mix_;
    int mode_ = 0;
    float lowCut_ = 90.0f, presDb_ = 0.0f, airDb_ = 0.0f;
    float rms_ = 0.0f, gr_ = 0.0f, grSlow_ = 0.0f, dsEnv_ = 0.0f, fullEnv_ = 0.0f, dsGr_ = 0.0f;
    std::uint32_t tick_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeVoxOne() { return std::make_unique<VoxOne>(); }

} // namespace hc::fx
