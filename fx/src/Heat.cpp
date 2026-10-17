// HEM HEAT — saturation. Idea (not code) from analog-modelled saturators: push the sound
// into a curve that rounds the peaks and adds harmonics, chosen by character:
//   TAPE       : soft, symmetric (odd harmonics), with a gentle high roll-off as it's pushed
//   TUBE       : asymmetric (even harmonics, warm), DC removed after the curve
//   TRANSISTOR : firmer knee, brighter, more odd harmonics
// 4x oversampled (no aliasing fizz), Tone tilts dark/bright after the curve, AUTO GAIN keeps
// the level about the same as Drive goes up, Mix blends the time-aligned dry.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Mode, Drive, Tone, AutoGain, Mix, Output, kCount };
const char* const kModes[] = {"TAPE", "TUBE", "TRANSISTOR"};
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"mode",     "Character", "", 0.0f, 2.0f, 0.0f, Taper::Choice, kModes},
    {"drive",    "Drive",     "dB", 0.0f, 36.0f, 6.0f},
    {"tone",     "Tone",      "%", -100.0f, 100.0f, 0.0f},
    {"autogain", "Auto Gain", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"mix",      "Mix",       "%", 0.0f, 100.0f, 100.0f},
    {"output",   "Output",    "dB", -24.0f, 12.0f, 0.0f},
};

constexpr float kBias = 0.25f;

inline float shape(int mode, float x) noexcept {
    switch (mode) {
    case 1: return std::tanh(x + kBias) - std::tanh(kBias);                      // tube: asymmetric
    case 2: { const float a = std::fabs(x); return x / std::pow(1.0f + a * a * a, 1.0f / 3.0f); }   // transistor
    default: return std::tanh(x);                                                // tape
    }
}

class Heat final : public Effect {
public:
    Heat() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::uint32_t latency() const noexcept override { return Oversampler4::latency(); }

    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        for (auto& b : up_) b.assign(static_cast<std::size_t>(std::min(maxBlock, kMaxBlock)) * Oversampler4::kFactor, 0.0f);
        for (auto& d : dry_) d.prepare(latency());
        const double fs4 = fs * Oversampler4::kFactor;
        dcCoef_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 10.0 / fs4));
        tapeLp_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 14000.0 / fs4));
        toneLp_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 900.0 / fs));
        reset();
    }
    void reset() noexcept override {
        for (auto& o : os_) o.clear();
        for (auto& d : dry_) d.clear();
        for (int c = 0; c < 2; ++c) { dcX_[c] = dcY_[c] = tapeZ_[c] = toneZ_[c] = 0.0f; }
        drive_.prepare(fs_ * 4, 20, dbToGain(p(Drive)));
        comp_.prepare(fs_ * 4, 20, compFor(static_cast<int>(p(Mode)), dbToGain(p(Drive)), p(AutoGain) >= 0.5f));
        tone_.prepare(fs_, 20, p(Tone) * 0.01f);
        mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        out_.prepare(fs_, 20, dbToGain(p(Output)));
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const int mode = static_cast<int>(p(Mode));
        const float dT = dbToGain(p(Drive)), cT = compFor(mode, dT, p(AutoGain) >= 0.5f);
        const float tT = p(Tone) * 0.01f, mT = p(Mix) * 0.01f, oT = dbToGain(p(Output));
        float* ch[2] = {L, R};
        os_[0].up(L, up_[0].data(), n);
        os_[1].up(R, up_[1].data(), n);
        const std::uint32_t n4 = n * Oversampler4::kFactor;
        const float tapeAmt = mode == 0 ? std::clamp((gainToDb(dT)) / 36.0f, 0.0f, 1.0f) : 0.0f;
        Smoothed dEnd = drive_, cEnd = comp_;
        for (int c = 0; c < 2; ++c) {
            Smoothed d = drive_, k = comp_;
            float* u = up_[c].data();
            for (std::uint32_t i = 0; i < n4; ++i) {
                float y = shape(mode, u[i] * d.next(dT)) * k.next(cT);
                if (mode == 1) {   // DC blocker after the asymmetric curve
                    const float x = y;
                    y = x - dcX_[c] + dcCoef_ * dcY_[c];
                    dcX_[c] = x; dcY_[c] = y;
                } else if (mode == 0) {   // tape: highs soften as it's pushed
                    tapeZ_[c] = y + tapeLp_ * (tapeZ_[c] - y);
                    y += tapeAmt * 0.5f * (tapeZ_[c] - y);
                }
                if (std::fabs(y) < 1e-20f) y = 0.0f;
                u[i] = y;
            }
            dEnd = d; cEnd = k;
        }
        drive_ = dEnd; comp_ = cEnd;
        Smoothed tEnd = tone_, mEnd = mix_, oEnd = out_;
        for (int c = 0; c < 2; ++c) {
            for (std::uint32_t i = 0; i < n; ++i) scratch_[i] = dry_[c].process(ch[c][i], latency());
            os_[c].down(up_[c].data(), ch[c], n);
            Smoothed t = tone_, m = mix_, o = out_;
            for (std::uint32_t i = 0; i < n; ++i) {
                // Tone: tilt around ~900 Hz, ±6 dB at the ends.
                const float x = ch[c][i];
                toneZ_[c] = x + toneLp_ * (toneZ_[c] - x);
                const float tv = t.next(tT);
                const float lo = toneZ_[c], hi = x - lo;
                const float wet = lo * dbToGainFast(-6.0f * tv) + hi * dbToGainFast(6.0f * tv);
                const float w = m.next(mT);
                ch[c][i] = (w * wet + (1.0f - w) * scratch_[i]) * o.next(oT);
                if (std::fabs(toneZ_[c]) < 1e-20f) toneZ_[c] = 0.0f;
            }
            tEnd = t; mEnd = m; oEnd = o;
        }
        tone_ = tEnd; mix_ = mEnd; out_ = oEnd;
    }

private:
    // Gain after the curve so a -18 dBFS sine comes out about where it went in.
    static float compFor(int mode, float drive, bool on) noexcept {
        if (!on) return 1.0f;
        const float x = 0.177f;   // -15 dBFS peak
        const float y = std::fabs(shape(mode, x * drive) - shape(mode, -x * drive)) * 0.5f;
        return y > 1e-6f ? x / y : 1.0f;
    }
    static float dbToGainFast(float db) noexcept { return std::exp(db * 0.1151292546f); }

    double fs_ = 48000;
    Oversampler4 os_[2];
    std::vector<float> up_[2];
    DelayLine dry_[2];
    std::array<float, kMaxBlock> scratch_{};
    float dcCoef_ = 0, tapeLp_ = 0, toneLp_ = 0;
    float dcX_[2]{}, dcY_[2]{}, tapeZ_[2]{}, toneZ_[2]{};
    Smoothed drive_, comp_, tone_, mix_, out_;
};

} // namespace

std::unique_ptr<Effect> makeHeat() { return std::make_unique<Heat>(); }

} // namespace hc::fx
