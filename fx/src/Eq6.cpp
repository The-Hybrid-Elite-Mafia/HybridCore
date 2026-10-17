// HEM EQ-6 — six-band EQ. Idea (not code) from the classic "few smooth bands" channel EQs:
// low cut, low shelf, two bells, high shelf, high cut, output. Our design: RBJ biquads,
// cuts as 12 dB/oct (one Butterworth stage) or 24 dB/oct (two stages); every value glides
// (20 ms) and the coefficients are recomputed from the gliding values every 32 samples,
// counted inside the effect so the result never depends on the host's block size.
// A cut at its end stop (20 Hz / 20 kHz) is switched out: the EQ is then fully transparent.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { LowCut, LowSlope, LsFreq, LsGain, B1Freq, B1Gain, B1Q, B2Freq, B2Gain, B2Q, HsFreq, HsGain, HighCut, HighSlope, Output, kCount };
const char* const kSlopes[] = {"12 dB/oct", "24 dB/oct"};
const ParamInfo kParams[kCount] = {
    {"lowcut",    "Low Cut",         "Hz", 20.0f, 1000.0f, 20.0f, Taper::Log},
    {"lowslope",  "Low Cut Slope",   "",   0.0f, 1.0f, 0.0f, Taper::Choice, kSlopes},
    {"lsfreq",    "Low Shelf Freq",  "Hz", 30.0f, 500.0f, 100.0f, Taper::Log},
    {"lsgain",    "Low Shelf Gain",  "dB", -15.0f, 15.0f, 0.0f},
    {"b1freq",    "Bell 1 Freq",     "Hz", 40.0f, 8000.0f, 400.0f, Taper::Log},
    {"b1gain",    "Bell 1 Gain",     "dB", -15.0f, 15.0f, 0.0f},
    {"b1q",       "Bell 1 Q",        "",   0.3f, 8.0f, 1.0f, Taper::Log},
    {"b2freq",    "Bell 2 Freq",     "Hz", 200.0f, 16000.0f, 2500.0f, Taper::Log},
    {"b2gain",    "Bell 2 Gain",     "dB", -15.0f, 15.0f, 0.0f},
    {"b2q",       "Bell 2 Q",        "",   0.3f, 8.0f, 1.0f, Taper::Log},
    {"hsfreq",    "High Shelf Freq", "Hz", 1500.0f, 18000.0f, 8000.0f, Taper::Log},
    {"hsgain",    "High Shelf Gain", "dB", -15.0f, 15.0f, 0.0f},
    {"highcut",   "High Cut",        "Hz", 1000.0f, 20000.0f, 20000.0f, Taper::Log},
    {"highslope", "High Cut Slope",  "",   0.0f, 1.0f, 0.0f, Taper::Choice, kSlopes},
    {"output",    "Output",          "dB", -18.0f, 18.0f, 0.0f},
};
constexpr std::uint32_t kSub = 32;             // samples between coefficient updates
constexpr double kButterQ4[2] = {0.54119610, 1.30656296};   // 4th-order Butterworth stages
constexpr double kShelfQ = 0.70710678;

class Eq6 final : public Effect {
public:
    Eq6() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        for (int i = 0; i < kCount; ++i) sm_[i].prepare(fs_, 20.0, p(i));
        out_.prepare(fs_, 20.0, dbToGain(p(Output)));
        for (auto& f : lc_) f.clear();
        for (auto& f : hc_) f.clear();
        ls_.clear(); b1_.clear(); b2_.clear(); hs_.clear();
        counter_ = 0;
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        float tgt[kCount];
        for (int i = 0; i < kCount; ++i) tgt[i] = p(i);
        const float outT = dbToGain(p(Output));
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int k = 0; k < kCount; ++k) (void)sm_[k].next(tgt[k]);
            if (counter_ % kSub == 0) update();
            ++counter_;
            float l = L[i], r = R[i];
            if (lcOn_) for (int s = 0; s < lcStages_; ++s) { l = lc_[s].l.tick(l); r = lc_[s].r.tick(r); }
            l = ls_.l.tick(l); r = ls_.r.tick(r);
            l = b1_.l.tick(l); r = b1_.r.tick(r);
            l = b2_.l.tick(l); r = b2_.r.tick(r);
            l = hs_.l.tick(l); r = hs_.r.tick(r);
            if (hcOn_) for (int s = 0; s < hcStages_; ++s) { l = hc_[s].l.tick(l); r = hc_[s].r.tick(r); }
            const float g = out_.next(outT);
            L[i] = l * g;
            R[i] = r * g;
        }
    }

private:
    [[nodiscard]] double v(int i) const noexcept { return sm_[i].cur; }
    void update() noexcept {
        using T = BiquadCoefs::Type;
        const bool wasLc = lcOn_, wasHc = hcOn_;
        lcOn_ = v(LowCut) > 20.5;
        hcOn_ = v(HighCut) < 19900.0;
        lcStages_ = p(LowSlope) >= 0.5f ? 2 : 1;
        hcStages_ = p(HighSlope) >= 0.5f ? 2 : 1;
        if (lcOn_ && !wasLc) for (auto& f : lc_) f.clear();
        if (hcOn_ && !wasHc) for (auto& f : hc_) f.clear();
        for (int s = 0; s < 2; ++s) {
            const double q = lcStages_ == 2 ? kButterQ4[s] : kShelfQ;
            lc_[s].set(BiquadCoefs::make(T::HighPass, fs_, v(LowCut), q));
            const double qh = hcStages_ == 2 ? kButterQ4[s] : kShelfQ;
            hc_[s].set(BiquadCoefs::make(T::LowPass, fs_, v(HighCut), qh));
        }
        ls_.set(BiquadCoefs::make(T::LowShelf, fs_, v(LsFreq), kShelfQ, v(LsGain)));
        b1_.set(BiquadCoefs::make(T::Bell, fs_, v(B1Freq), v(B1Q), v(B1Gain)));
        b2_.set(BiquadCoefs::make(T::Bell, fs_, v(B2Freq), v(B2Q), v(B2Gain)));
        hs_.set(BiquadCoefs::make(T::HighShelf, fs_, v(HsFreq), kShelfQ, v(HsGain)));
    }

    double fs_ = 48000;
    Smoothed sm_[kCount];
    Smoothed out_;
    Biquad2 lc_[2], hc_[2], ls_, b1_, b2_, hs_;
    bool lcOn_ = false, hcOn_ = false;
    int lcStages_ = 1, hcStages_ = 1;
    std::uint32_t counter_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeEq6() { return std::make_unique<Eq6>(); }

} // namespace hc::fx
