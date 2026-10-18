// HEM BALANCE — automatic tone balance. Listens to the mix for a few seconds, compares ten
// frequency regions (31 Hz .. 16 kHz, one octave apart) to a smooth target line and gently
// corrects the ones that stick out or are missing.
//   Analysis: L+R through ten one-octave band-passes, mean square over SPEED (3 s / 1 s / 0.3 s).
//   Target: a straight line of TILT dB per octave through 1 kHz (0 = flat like pink noise,
//   negative = a darker mix); its height is fitted to the mix, so overall loudness never moves.
//   Correction per band = -(distance from the line) × AMOUNT, at most ±RANGE, approached
//   gradually (8 % per update, every 256 samples), applied by ten peaking filters.
//   Quieter than -70 dBFS in every band, or FREEZE on: the correction stays where it is.
//   LOW / HIGH limit which bands may move (e.g. leave the sub alone).
//   Ported from the Rework Playground's Elite Balance and improved: 10 bands instead of 8,
//   LOW / HIGH limits, band corrections on the faceplate.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

constexpr int kBands = 10;
enum P { Amount, Tilt, Range, Speed, LowBand, HighBand, Freeze, Output, kCount };
const char* const kSpeeds[] = {"SLOW", "MEDIUM", "FAST"};
const char* const kBandNames[] = {"31", "63", "125", "250", "500", "1K", "2K", "4K", "8K", "16K"};
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"amount", "Amount",    "%", 0.0f, 100.0f, 50.0f},
    {"tilt",   "Tilt",      "dB/oct", -6.0f, 3.0f, -1.5f},
    {"range",  "Range",     "dB", 1.0f, 12.0f, 6.0f},
    {"speed",  "Speed",     "", 0.0f, 2.0f, 1.0f, Taper::Choice, kSpeeds},
    {"low",    "Lowest Band",  "", 0.0f, 9.0f, 1.0f, Taper::Choice, kBandNames},
    {"high",   "Highest Band", "", 0.0f, 9.0f, 9.0f, Taper::Choice, kBandNames},
    {"freeze", "Freeze",    "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output", "Output",    "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[kBands] = {
    {"31", "dB", -12, 12, true}, {"63", "dB", -12, 12, true}, {"125", "dB", -12, 12, true}, {"250", "dB", -12, 12, true},
    {"500", "dB", -12, 12, true}, {"1K", "dB", -12, 12, true}, {"2K", "dB", -12, 12, true}, {"4K", "dB", -12, 12, true},
    {"8K", "dB", -12, 12, true}, {"16K", "dB", -12, 12, true}};
constexpr double kBandQ = 1.41;
constexpr float kSilenceDb = -70.0f, kFollow = 0.08f;
[[nodiscard]] double bandHz(int b) noexcept { return 31.25 * std::exp2(b); }
[[nodiscard]] double secondsFor(int s) noexcept { return s == 0 ? 3.0 : s == 2 ? 0.3 : 1.0; }

class Balance final : public Effect {
public:
    Balance() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        nyqOk_ = 0;
        for (int b = 0; b < kBands; ++b) {
            if (bandHz(b) < fs * 0.45) nyqOk_ = b + 1;
            bp_[b].c = BiquadCoefs::make(BiquadCoefs::Type::BandPass, fs, std::min(bandHz(b), fs * 0.45), kBandQ);
        }
        updateEvery_ = std::max(64, static_cast<int>(std::lround(256.0 * fs / 48000.0)));
        reset();
    }
    void reset() noexcept override {
        for (auto& b : bp_) b.clear();
        for (auto& b : eq_) { b.clear(); b.set({}); }
        power_.fill(0.0f);
        applied_.fill(0.0f);
        since_ = 0;
        warm_ = 0.0;
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        for (int b = 0; b < kBands; ++b) publishMeter(b, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float env = onePoleCoef(fs_, secondsFor(static_cast<int>(p(Speed))) * 1000.0);
        const bool freeze = p(Freeze) >= 0.5f;
        const float oT = dbToGain(p(Output));
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            if (!freeze) {
                const float m = 0.5f * (l + r);
                for (int b = 0; b < nyqOk_; ++b) {
                    const float y = bp_[b].tick(m);
                    power_[static_cast<std::size_t>(b)] = y * y + env * (power_[static_cast<std::size_t>(b)] - y * y);
                    if (power_[static_cast<std::size_t>(b)] < 1e-30f) power_[static_cast<std::size_t>(b)] = 0.0f;
                }
                warm_ += 1.0;
            }
            if (++since_ >= updateEvery_) { since_ = 0; update(freeze); }
            float ol = l, orr = r;
            for (int b = 0; b < nyqOk_; ++b) { ol = eq_[b].l.tick(ol); orr = eq_[b].r.tick(orr); }
            const float o = out_.next(oT);
            L[i] = ol * o; R[i] = orr * o;
        }
    }

private:
    void update(bool freeze) noexcept {
        const int lo = static_cast<int>(p(LowBand)), hi = std::min(static_cast<int>(p(HighBand)), nyqOk_ - 1);
        if (!freeze && warm_ > 0.5 * fs_ && hi >= lo) {
            std::array<float, kBands> dev{};
            float mean = 0.0f, loudest = -200.0f;
            const float tilt = p(Tilt);
            for (int b = lo; b <= hi; ++b) {
                const float level = 10.0f * std::log10(power_[static_cast<std::size_t>(b)] + 1e-20f);
                loudest = std::max(loudest, level);
                dev[static_cast<std::size_t>(b)] = level - tilt * static_cast<float>(b - 5);   // the line goes through 1 kHz
                mean += dev[static_cast<std::size_t>(b)];
            }
            mean /= static_cast<float>(hi - lo + 1);
            if (loudest > kSilenceDb) {
                const float amount = p(Amount) * 0.01f, range = p(Range);
                for (int b = 0; b < kBands; ++b) {
                    const float want = (b >= lo && b <= hi) ? std::clamp(-(dev[static_cast<std::size_t>(b)] - mean) * amount, -range, range) : 0.0f;
                    applied_[static_cast<std::size_t>(b)] += kFollow * (want - applied_[static_cast<std::size_t>(b)]);
                }
            }
        }
        for (int b = 0; b < kBands; ++b) {
            const float a = applied_[static_cast<std::size_t>(b)];
            if (b < nyqOk_) eq_[b].set(BiquadCoefs::make(BiquadCoefs::Type::Bell, fs_, bandHz(b), kBandQ, a));
            publishMeter(b, a);
        }
    }

    double fs_ = 48000, warm_ = 0;
    int nyqOk_ = kBands, since_ = 0, updateEvery_ = 256;
    Biquad bp_[kBands];
    Biquad2 eq_[kBands];
    std::array<float, kBands> power_{}, applied_{};
    Smoothed out_;
};

} // namespace

std::unique_ptr<Effect> makeBalance() { return std::make_unique<Balance>(); }

} // namespace hc::fx
