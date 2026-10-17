// HEM SUBSTACK — bass enhancer. Idea (not code) from the "psychoacoustic bass" plug-ins:
// small speakers can't play the sub, but the ear rebuilds a missing fundamental from its
// 2nd and 3rd harmonics. Our design:
//   Sub        = a low shelf at Freq (RBJ, Q 0.71): the whole band below moves by Sub dB
//   Harmonics  = the mid (L+R) low band (4th-order low-pass at Freq), normalised by its own
//                envelope, through Chebyshev curves T2 = 2x²−1 (pure 2nd harmonic of a sine)
//                and T3 = 4x³−3x (pure 3rd), scaled back by the envelope (so they follow the
//                bass level), band-passed to 0.9 × Freq … 4 × Freq, added to both sides
//   Mono Below = the side (L−R) signal high-passed at Freq (4th order): no stereo below it
//   All at rest (Sub 0 dB, Harmonics 0 %, Mono off) the output is the input exactly.
// Coefficients follow Freq every 32 samples (internal counter, block-size independent).
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Freq, Sub, Harmonics, MonoBelow, Output, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"freq",      "Freq",       "Hz", 40.0f, 150.0f, 90.0f, Taper::Log},
    {"sub",       "Sub",        "dB", -12.0f, 12.0f, 0.0f},
    {"harmonics", "Harmonics",  "%", 0.0f, 100.0f, 0.0f},
    {"monobelow", "Mono Below", "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output",    "Output",     "dB", -18.0f, 12.0f, 0.0f},
};
constexpr std::uint32_t kSub = 32;
constexpr double kButterQ = 0.70710678;
constexpr double kButterQ4[2] = {0.54119610, 1.30656296};

class SubStack final : public Effect {
public:
    SubStack() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        freq_.prepare(fs_, 30.0, p(Freq));
        subDbS_.prepare(fs_, 20.0, p(Sub));
        subDb_ = p(Sub);
        harm_.prepare(fs_, 20.0, p(Harmonics) * 0.01f);
        mono_.prepare(fs_, 20.0, p(MonoBelow));
        out_.prepare(fs_, 20.0, dbToGain(p(Output)));
        for (auto& f : lp_) f.clear();
        for (auto& f : sideHp_) f.clear();
        for (auto& f : shelf_) f.clear();
        hp_.clear(); hlp_.clear();
        env_.env = 0.0f;
        env_.set(fs_, 1.0, 250.0);
        counter_ = 0;
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float fT = p(Freq), sDbT = p(Sub), hT = p(Harmonics) * 0.01f, mT = p(MonoBelow), oT = dbToGain(p(Output));
        for (std::uint32_t i = 0; i < n; ++i) {
            (void)freq_.next(fT);
            if (counter_ % kSub == 0) update();
            ++counter_;
            subDb_ = subDbS_.next(sDbT);
            const float hg = harm_.next(hT), mono = mono_.next(mT), og = out_.next(oT);
            // Sub: low shelf per side.
            float x[2] = {shelf_[0].tick(L[i]), shelf_[1].tick(R[i])};
            // Mono Below: high-pass the side.
            const float mid = 0.5f * (x[0] + x[1]), side = 0.5f * (x[0] - x[1]);
            const float sideHp = sideHp_[1].tick(sideHp_[0].tick(side));
            const float sideOut = side + mono * (sideHp - side);
            // Harmonics from the mid low band.
            const float low = lp_[1].tick(lp_[0].tick(mid));
            float e = env_.tick(std::fabs(low));
            if (e < 1e-12f) { e = 0.0f; env_.env = 0.0f; }
            float h = 0.0f;
            if (e > 0.0f) {
                const float u = std::clamp(low / e, -1.0f, 1.0f);
                const float t2 = 2.0f * u * u - 1.0f, t3 = (4.0f * u * u - 3.0f) * u;
                h = (0.6f * t2 + 0.5f * t3) * e;
            }
            h = hlp_.tick(hp_.tick(h)) * hg * 1.5f;
            const float y[2] = {mid + sideOut + h, mid - sideOut + h};
            L[i] = y[0] * og;
            R[i] = y[1] * og;
        }
    }

private:
    void update() noexcept {
        using T = BiquadCoefs::Type;
        const double f = freq_.cur;
        const auto lp = BiquadCoefs::make(T::LowPass, fs_, f, kButterQ4[0]), lp2 = BiquadCoefs::make(T::LowPass, fs_, f, kButterQ4[1]);
        const auto sh = BiquadCoefs::make(T::HighPass, fs_, f, kButterQ4[0]), sh2 = BiquadCoefs::make(T::HighPass, fs_, f, kButterQ4[1]);
        lp_[0].c = lp; lp_[1].c = lp2;
        sideHp_[0].c = sh; sideHp_[1].c = sh2;
        hp_.c = BiquadCoefs::make(T::HighPass, fs_, std::max(30.0, 0.9 * f), kButterQ);
        hlp_.c = BiquadCoefs::make(T::LowPass, fs_, 4.0 * f, kButterQ);
        const auto ls = BiquadCoefs::make(T::LowShelf, fs_, f, kButterQ, subDb_);
        shelf_[0].c = ls; shelf_[1].c = ls;
    }

    double fs_ = 48000;
    Smoothed freq_, subDbS_, harm_, mono_, out_;
    float subDb_ = 0.0f;
    Biquad lp_[2], sideHp_[2], shelf_[2], hp_, hlp_;
    Envelope env_;
    std::uint32_t counter_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeSubStack() { return std::make_unique<SubStack>(); }

} // namespace hc::fx
