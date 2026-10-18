// HEM WIDE — stereo width with a mono low end.
//   Mid / Side: the Side is scaled by WIDTH (0 % = mono, 100 % = untouched, 200 % = twice as wide).
//   MONO BELOW: the Side goes through a 24 dB/oct high-pass, so the bass sits dead centre
//   (20 Hz = off). SIDE TONE tilts the Side only (darker or brighter width).
//   SAFE: when the result starts to cancel in mono (correlation below 0), the extra width is
//   eased back automatically until it is mono-safe again.
//   Meters: correlation (+1 mono .. -1 out of phase) and how much of the energy is Side.
//   Ported from the Rework Playground's Elite Width and improved: Side tone, SAFE.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Width, MonoBelow, SideTone, Safe, Output, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"width",     "Width",      "%", 0.0f, 200.0f, 100.0f},
    {"monobelow", "Mono Below", "Hz", 20.0f, 500.0f, 120.0f, Taper::Log},
    {"sidetone",  "Side Tone",  "%", -100.0f, 100.0f, 0.0f},
    {"safe",      "Safe",       "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output",    "Output",     "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"CORRELATION", "corr", -1.0f, 1.0f, true}, {"SIDE", "%", 0.0f, 100.0f}};
constexpr double kQ = 0.7071067811865476;

class Wide final : public Effect {
public:
    Wide() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        lastMono_ = -1.0f;
        corrCoef_ = onePoleCoef(fs, 300.0);
        safeCoef_ = onePoleCoef(fs, 150.0);
        tiltLp_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 2000.0 / fs));
        reset();
    }
    void reset() noexcept override {
        hp1_.clear(); hp2_.clear();
        lr_ = ll_ = rr_ = sideE_ = totalE_ = 0.0;
        tiltZ_ = 0.0f;
        safeCorr_ = 1.0f;
        width_.prepare(fs_, 30, p(Width) * 0.01f);
        tone_.prepare(fs_, 30, p(SideTone) * 0.01f);
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, 1.0f); publishMeter(1, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float mono = p(MonoBelow);
        if (mono != lastMono_) {
            const auto c = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, mono, kQ);
            hp1_.c = c; hp2_.c = c;
            lastMono_ = mono;
        }
        const bool monoOn = mono > 20.5f, safe = p(Safe) >= 0.5f;
        const float wT = p(Width) * 0.01f, tT = p(SideTone) * 0.01f, oT = dbToGain(p(Output));
        const double c = corrCoef_;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float m = 0.5f * (L[i] + R[i]);
            float s = 0.5f * (L[i] - R[i]);
            const float sIn = s;
            const float hpS = hp2_.tick(hp1_.tick(s));   // always run, so switching it on never clicks
            if (monoOn) s = hpS;
            // Side tone: tilt around 2 kHz, ±6 dB at the ends.
            tiltZ_ = s + tiltLp_ * (tiltZ_ - s);
            if (std::fabs(tiltZ_) < 1e-20f) tiltZ_ = 0.0f;
            const float tv = tone_.next(tT);
            s = tiltZ_ * std::exp(-0.6907755f * tv) + (s - tiltZ_) * std::exp(0.6907755f * tv);
            float w = width_.next(wT);
            if (safe && w > 1.0f) {
                // ease the extra width back while the output cancels in mono
                const float k = std::clamp(safeCorr_ * 4.0f, 0.0f, 1.0f);   // corr <= 0 -> 0, corr >= 0.25 -> 1
                w = 1.0f + (w - 1.0f) * k;
            }
            s *= w;
            const float o = out_.next(oT);
            const float l = (m + s) * o, r = (m - s) * o;
            L[i] = l; R[i] = r;
            lr_ = c * lr_ + (1 - c) * static_cast<double>(l) * r;
            ll_ = c * ll_ + (1 - c) * static_cast<double>(l) * l;
            rr_ = c * rr_ + (1 - c) * static_cast<double>(r) * r;
            sideE_  = c * sideE_ + (1 - c) * static_cast<double>(s) * s;
            totalE_ = c * totalE_ + (1 - c) * (static_cast<double>(m) * m + static_cast<double>(s) * s);
            const double den = std::sqrt(ll_ * rr_);
            const float corrNow = den > 1e-12 ? static_cast<float>(lr_ / den) : 1.0f;
            safeCorr_ = corrNow + safeCoef_ * (safeCorr_ - corrNow);
            (void)sIn;
        }
        const double den = std::sqrt(ll_ * rr_);
        publishMeter(0, den > 1e-12 ? static_cast<float>(lr_ / den) : 1.0f);
        publishMeter(1, totalE_ > 1e-12 ? static_cast<float>(100.0 * sideE_ / totalE_) : 0.0f);
        if (lr_ != 0 && std::fabs(lr_) < 1e-30) lr_ = 0;
        if (ll_ < 1e-30) ll_ = 0;
        if (rr_ < 1e-30) rr_ = 0;
        if (sideE_ < 1e-30) sideE_ = 0;
        if (totalE_ < 1e-30) totalE_ = 0;
    }

private:
    double fs_ = 48000;
    Biquad hp1_, hp2_;
    float lastMono_ = -1, tiltLp_ = 0, tiltZ_ = 0, safeCoef_ = 0, safeCorr_ = 1;
    double corrCoef_ = 0, lr_ = 0, ll_ = 0, rr_ = 0, sideE_ = 0, totalE_ = 0;
    Smoothed width_, tone_, out_;
};

} // namespace

std::unique_ptr<Effect> makeWide() { return std::make_unique<Wide>(); }

} // namespace hc::fx
