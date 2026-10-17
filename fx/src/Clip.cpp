// HEM CLIP — clipper. Idea (not code) from the "bus clipper" style of plug-in: shave the
// peaks off before a limiter so the mix gets louder without the limiter pumping.
// Our design: input drive → 4x oversampled clipper with an adjustable knee (hard … smooth
// tanh-like curve) at a ceiling → back to 48 kHz → output gain. Stereo-linked optional.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Drive, Ceiling, Knee, Link, Output, Mix, kCount };
const char* const kLinkNames[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"drive",   "Drive",   "dB", 0.0f, 24.0f, 0.0f},
    {"ceiling", "Ceiling", "dB", -24.0f, 0.0f, -0.3f},
    {"knee",    "Knee",    "%", 0.0f, 100.0f, 30.0f},
    {"link",    "Stereo Link", "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kLinkNames},
    {"output",  "Output",  "dB", -24.0f, 12.0f, 0.0f},
    {"mix",     "Mix",     "%", 0.0f, 100.0f, 100.0f},
};

// Clipping curve, x and result in units of the ceiling (|y| <= 1 always).
// knee k in 0..1: below 1-k straight; from 1-k to 1+k a quadratic bend into 1; above flat.
inline float clipCurve(float x, float k) noexcept {
    const float a = std::fabs(x);
    float y;
    if (k <= 1e-4f) y = std::min(a, 1.0f);
    else if (a <= 1.0f - k) y = a;
    else if (a >= 1.0f + k) y = 1.0f;
    else { const float d = a - (1.0f - k); y = a - d * d / (4.0f * k); }
    return std::copysign(y, x);
}

class Clip final : public Effect {
public:
    Clip() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        for (auto& b : up_) b.assign(static_cast<std::size_t>(maxBlock) * Oversampler4::kFactor, 0.0f);
        dry_[0].prepare(Oversampler4::latency());
        dry_[1].prepare(Oversampler4::latency());
        reset();
    }
    void reset() noexcept override {
        os_[0].clear(); os_[1].clear(); dry_[0].clear(); dry_[1].clear();
        drive_.prepare(fs_, 20, dbToGain(p(Drive))); out_.prepare(fs_, 20, dbToGain(p(Output)));
        ceil_.prepare(fs_, 20, dbToGain(p(Ceiling))); mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        publishReduction(0.0f);
    }
    std::uint32_t latency() const noexcept override { return Oversampler4::latency(); }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float k = std::clamp(p(Knee) * 0.01f, 0.0f, 1.0f) * 0.5f;
        const bool link = p(Link) >= 0.5f;
        const float dT = dbToGain(p(Drive)), cT = dbToGain(p(Ceiling)), oT = dbToGain(p(Output)), mT = p(Mix) * 0.01f;
        float* ch[2] = {L, R};
        // Drive into the oversampler (smoothed per sample); keep the dry, delayed to line up.
        for (int c = 0; c < 2; ++c) {
            Smoothed d = drive_;
            for (std::uint32_t i = 0; i < n; ++i) ch[c][i] *= d.next(dT);
            if (c == 1) drive_ = d;
            os_[c].up(ch[c], up_[c].data(), n);
        }
        const std::uint32_t n4 = n * Oversampler4::kFactor;
        float maxIn = 0;
        Smoothed cs = ceil_;
        for (std::uint32_t i = 0; i < n4; ++i) {
            const float ce = (i % 4 == 0) ? cs.next(cT) : cs.cur;
            const float inv = 1.0f / ce;
            float l = up_[0][i] * inv, r = up_[1][i] * inv;
            maxIn = std::max(maxIn, std::max(std::fabs(l), std::fabs(r)));
            if (link) {
                const float m = std::max(std::fabs(l), std::fabs(r));
                const float g = m > 1e-9f ? std::fabs(clipCurve(m, k)) / m : 1.0f;
                l *= g; r *= g;
            } else {
                l = clipCurve(l, k); r = clipCurve(r, k);
            }
            up_[0][i] = l * ce; up_[1][i] = r * ce;
        }
        ceil_ = cs;
        // Back down; mix with the (driven, delayed) dry; output gain.
        Smoothed o = out_, m = mix_;
        for (int c = 0; c < 2; ++c) {
            // The dry path is the driven signal delayed by the oversampler latency.
            for (std::uint32_t i = 0; i < n; ++i) scratchDry_[i] = dry_[c].process(ch[c][i], Oversampler4::latency());
            os_[c].down(up_[c].data(), ch[c], n);
            Smoothed oo = out_, mm = mix_;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float wet = mm.next(mT);
                ch[c][i] = (wet * ch[c][i] + (1.0f - wet) * scratchDry_[i]) * oo.next(oT);
            }
            o = oo; m = mm;
        }
        out_ = o; mix_ = m;
        publishReduction(maxIn > 1.0f ? -gainToDb(maxIn) : 0.0f);
    }

private:
    double fs_ = 48000;
    Oversampler4 os_[2];
    std::vector<float> up_[2];
    DelayLine dry_[2];
    std::array<float, kMaxBlock> scratchDry_{};
    Smoothed drive_, out_, ceil_, mix_;
};

} // namespace

std::unique_ptr<Effect> makeClip() { return std::make_unique<Clip>(); }

} // namespace hc::fx
