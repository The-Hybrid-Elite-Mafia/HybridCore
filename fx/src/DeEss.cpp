// HEM DE-ESS — turns down harsh S and T sounds only while they jump out.
//   Detector: a band-pass (two stages) around FREQUENCY on L+R, 1 ms attack / 60 ms release,
//   read as a sine peak; gain reduction = how far it is over THRESHOLD, at most RANGE dB,
//   glided over 2 ms.
//   SPLIT mode: only the part above 0.7 × FREQUENCY is turned down (Linkwitz-Riley split:
//   with nothing to do the two halves add back flat). WIDE mode: the whole signal dips
//   (gentler on very bright cymbal-heavy material).
//   LISTEN plays only what is being taken away.
//   Ported from the Rework Playground's Elite De-Ess and improved: WIDE mode.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Freq, Threshold, Range, Mode, Listen, Output, kCount };
const char* const kModes[] = {"SPLIT", "WIDE"};
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"freq",      "Frequency", "Hz", 3000.0f, 12000.0f, 6500.0f, Taper::Log},
    {"threshold", "Threshold", "dB", -60.0f, 0.0f, -30.0f},
    {"range",     "Range",     "dBr", 0.0f, 20.0f, 10.0f},
    {"mode",      "Mode",      "", 0.0f, 1.0f, 0.0f, Taper::Choice, kModes},
    {"listen",    "Listen",    "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output",    "Output",    "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"ESS LEVEL", "dBFS", -60.0f, 0.0f}, {"REDUCTION", "dB", -20.0f, 0.0f}};
constexpr double kQ = 0.7071067811865476;

class DeEss final : public Effect {
public:
    DeEss() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        lastFreq_ = -1.0f;
        env_.set(fs, 1.0, 60.0);
        grCoef_ = onePoleCoef(fs, 2.0);
        reset();
    }
    void reset() noexcept override {
        for (auto* b : {&hpA_, &hpB_, &lpA_, &lpB_, &det_}) b->clear();
        env_.env = 0.0f;
        gr_ = 0.0f;
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, -120.0f); publishMeter(1, 0.0f);
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float f = p(Freq);
        if (f != lastFreq_) {
            const auto hp = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, f * 0.7, kQ);
            const auto lp = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, f * 0.7, kQ);
            hpA_.set(hp); hpB_.set(hp); lpA_.set(lp); lpB_.set(lp);
            const auto bp = BiquadCoefs::make(BiquadCoefs::Type::BandPass, fs_, f, 1.4);
            det_.l.c = bp; det_.r.c = bp;
            lastFreq_ = f;
        }
        const float thr = p(Threshold), range = p(Range), oT = dbToGain(p(Output));
        const bool wide = p(Mode) >= 0.5f, listen = p(Listen) >= 0.5f;
        float maxGr = 0.0f, maxLvl = -120.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            const float d = det_.r.tick(det_.l.tick(0.5f * (l + r)));
            const float e = env_.tick(std::fabs(d));
            if (env_.env < 1e-20f) env_.env = 0.0f;
            const float lvl = 20.0f * std::log10(e * 1.41421356f + 1e-9f);
            const float want = std::clamp(lvl - thr, 0.0f, range);
            gr_ = want + grCoef_ * (gr_ - want);
            const float g = std::exp(-gr_ * 0.1151292546f);
            // the split always runs (switching modes never clicks)
            const float hl = hpB_.l.tick(hpA_.l.tick(l)), hr = hpB_.r.tick(hpA_.r.tick(r));
            const float ll = lpB_.l.tick(lpA_.l.tick(l)), lr = lpB_.r.tick(lpA_.r.tick(r));
            const float o = out_.next(oT);
            if (wide) {
                L[i] = (listen ? l * (1.0f - g) : l * g) * o;
                R[i] = (listen ? r * (1.0f - g) : r * g) * o;
            } else {
                L[i] = (listen ? hl * (1.0f - g) : ll + hl * g) * o;
                R[i] = (listen ? hr * (1.0f - g) : lr + hr * g) * o;
            }
            maxGr = std::max(maxGr, gr_);
            maxLvl = std::max(maxLvl, lvl);
        }
        publishMeter(0, maxLvl);
        publishMeter(1, -maxGr);
        publishReduction(-maxGr);
    }

private:
    double fs_ = 48000;
    Biquad2 hpA_, hpB_, lpA_, lpB_, det_;
    Envelope env_;
    float lastFreq_ = -1, gr_ = 0, grCoef_ = 0;
    Smoothed out_;
};

} // namespace

std::unique_ptr<Effect> makeDeEss() { return std::make_unique<DeEss>(); }

} // namespace hc::fx
