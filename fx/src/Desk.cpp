// HEM DESK — mixing-desk glue: channel drive, transformer weight and air, a touch of crosstalk.
//   Crosstalk: each side bleeds a dull (3 kHz one-pole) copy of the other in, up to -20 dB.
//   Drive: 4x oversampled soft curve, then divided back by the drive and by the curve's
//   slope at rest, so quiet parts keep their level and only loud peaks round off and pick
//   up harmonics. The asymmetry (even harmonics) comes from the desk CHARACTER; DC removed.
//   CHARACTER also sets a gentle low shelf (weight) and high shelf (air / roll-off):
//     CLEAN DESK     : drive only
//     BRITISH IRON   : +1 dB @100 Hz, +1.5 dB @12 kHz, light asymmetry
//     AMERICAN PUNCH : +1.5 dB @80 Hz, +0.5 dB @10 kHz
//     TUBE BUS       : +1 dB @120 Hz, -1 dB @8 kHz, strong asymmetry (warm, rounder top)
//   Ported from the Rework Playground's Elite Console and improved: 4x oversampling (no
//   aliasing fizz when driven hard), MIX.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Drive, Character, Crosstalk, Mix, Output, kCount };
const char* const kDesks[] = {"CLEAN DESK", "BRITISH IRON", "AMERICAN PUNCH", "TUBE BUS"};
const ParamInfo kParams[kCount] = {
    {"drive",     "Drive",     "dB", 0.0f, 18.0f, 6.0f},
    {"character", "Character", "", 0.0f, 3.0f, 1.0f, Taper::Choice, kDesks},
    {"crosstalk", "Crosstalk", "%", 0.0f, 100.0f, 30.0f},
    {"mix",       "Mix",       "%", 0.0f, 100.0f, 100.0f},
    {"output",    "Output",    "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"SATURATION", "dB", -12.0f, 0.0f}};

struct Desk { float bias, lowHz, lowDb, highHz, highDb; };
Desk deskFor(int c) noexcept {
    switch (c) {
    case 1:  return {0.08f, 100.0f, 1.0f, 12000.0f, 1.5f};
    case 2:  return {0.04f, 80.0f, 1.5f, 10000.0f, 0.5f};
    case 3:  return {0.18f, 120.0f, 1.0f, 8000.0f, -1.0f};
    default: return {0.0f, 100.0f, 0.0f, 12000.0f, 0.0f};
    }
}
constexpr float kMaxCrosstalk = 0.1f;

class DeskFx final : public Effect {
public:
    DeskFx() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return Oversampler4::latency(); }

    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        for (auto& b : up_) b.assign(static_cast<std::size_t>(std::min(maxBlock, kMaxBlock)) * Oversampler4::kFactor, 0.0f);
        for (auto& d : dry_) d.prepare(latency());
        xtCoef_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 3000.0 / fs));
        dcCoef_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 8.0 / (fs * Oversampler4::kFactor)));
        lastChar_ = -1;
        reset();
    }
    void reset() noexcept override {
        for (auto& o : os_) o.clear();
        for (auto& d : dry_) d.clear();
        low_.clear(); high_.clear();
        for (int c = 0; c < 2; ++c) xtZ_[c] = dcX_[c] = dcY_[c] = 0.0f;
        drive_.prepare(fs_ * Oversampler4::kFactor, 30, dbToGain(p(Drive)));
        xt_.prepare(fs_, 30, p(Crosstalk) * 0.01f * kMaxCrosstalk);
        mix_.prepare(fs_, 30, p(Mix) * 0.01f);
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const int c = static_cast<int>(p(Character));
        if (c != lastChar_) {
            const Desk d = deskFor(c);
            low_.set(BiquadCoefs::make(BiquadCoefs::Type::LowShelf, fs_, d.lowHz, 0.7071, d.lowDb));
            high_.set(BiquadCoefs::make(BiquadCoefs::Type::HighShelf, fs_, d.highHz, 0.7071, d.highDb));
            bias_ = d.bias;
            lastChar_ = c;
        }
        const float dT = dbToGain(p(Drive)), xT = p(Crosstalk) * 0.01f * kMaxCrosstalk;
        const float mT = p(Mix) * 0.01f, oT = dbToGain(p(Output));
        // 1) crosstalk at the base rate (into the curve), keep the time-aligned dry
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            const float x = xt_.next(xT);
            xtZ_[0] = r + xtCoef_ * (xtZ_[0] - r);
            xtZ_[1] = l + xtCoef_ * (xtZ_[1] - l);
            for (float& z : xtZ_) if (std::fabs(z) < 1e-20f) z = 0.0f;
            scratch_[0][i] = dry_[0].process(l, latency());
            scratch_[1][i] = dry_[1].process(r, latency());
            L[i] = l + x * xtZ_[0];
            R[i] = r + x * xtZ_[1];
        }
        // 2) the curve, 4x oversampled
        os_[0].up(L, up_[0].data(), n);
        os_[1].up(R, up_[1].data(), n);
        const std::uint32_t n4 = n * Oversampler4::kFactor;
        const float offset = std::tanh(bias_), slope = 1.0f - offset * offset;
        float peakIn = 0.0f, peakOut = 0.0f;
        Smoothed dEnd = drive_;
        for (int ch = 0; ch < 2; ++ch) {
            Smoothed d = drive_;
            float* u = up_[ch].data();
            for (std::uint32_t i = 0; i < n4; ++i) {
                const float g = d.next(dT);
                const float x = u[i];
                float y = (std::tanh(x * g + bias_) - offset) / (g * slope);
                peakIn = std::max(peakIn, std::fabs(x));
                peakOut = std::max(peakOut, std::fabs(y));
                const float yd = y - dcX_[ch] + dcCoef_ * dcY_[ch];
                dcX_[ch] = y; dcY_[ch] = yd;
                if (std::fabs(dcY_[ch]) < 1e-20f) dcY_[ch] = 0.0f;
                u[i] = yd;
            }
            dEnd = d;
        }
        drive_ = dEnd;
        float* ch[2] = {L, R};
        os_[0].down(up_[0].data(), L, n);
        os_[1].down(up_[1].data(), R, n);
        // 3) desk tone, mix with the aligned dry, output
        Smoothed mEnd = mix_, oEnd = out_;
        for (int k = 0; k < 2; ++k) {
            Biquad& lo = k == 0 ? low_.l : low_.r;
            Biquad& hi = k == 0 ? high_.l : high_.r;
            Smoothed m = mix_, o = out_;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float wet = hi.tick(lo.tick(ch[k][i]));
                const float w = m.next(mT);
                ch[k][i] = (scratch_[k][i] + w * (wet - scratch_[k][i])) * o.next(oT);
            }
            mEnd = m; oEnd = o;
        }
        mix_ = mEnd; out_ = oEnd;
        publishMeter(0, peakIn > 1e-6f && peakOut > 1e-9f ? -std::max(0.0f, gainToDb(peakIn) - gainToDb(peakOut)) : 0.0f);
    }

private:
    double fs_ = 48000;
    Oversampler4 os_[2];
    std::vector<float> up_[2];
    DelayLine dry_[2];
    std::array<std::array<float, kMaxBlock>, 2> scratch_{};
    Biquad2 low_, high_;
    float xtCoef_ = 0, dcCoef_ = 0, bias_ = 0;
    float xtZ_[2]{}, dcX_[2]{}, dcY_[2]{};
    int lastChar_ = -1;
    Smoothed drive_, xt_, mix_, out_;
};

} // namespace

std::unique_ptr<Effect> makeDesk() { return std::make_unique<DeskFx>(); }

} // namespace hc::fx
