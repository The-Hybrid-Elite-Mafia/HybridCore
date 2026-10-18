// HEM DUST — old tape and old sampler character.
//   WOW (0.55 Hz, up to 4 ms) and FLUTTER (6.3 Hz, up to 0.4 ms): the read point of a short
//   delay line swings between "now" and twice the depth back (so with both at 0 the sound is
//   not delayed at all).
//   SAMPLE RATE: hold each sample until a slower clock ticks (top = off at any session rate).
//   BITS: round to 2^(bits-1) steps (16 = off).
//   VINYL: hiss (white, 400 Hz high-passed) and crackle (random pops, up to 8 a second,
//   deterministic so renders repeat). The vinyl FOLLOWS the music: it fades in while
//   something plays and fades out within ~2 s after it stops, so a quiet track or a stopped
//   transport is not left hissing.
//   TONE: 12 dB/oct low-pass. MIX blends with the dry (MIX 0 = untouched).
//   Ported from the Rework Playground's Elite Lo-Fi and improved: vinyl follows the music.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Wow, Flutter, Bits, Rate, Vinyl, Tone, Mix, Output, kCount };
const ParamInfo kParams[kCount] = {
    {"wow",     "Wow",         "%", 0.0f, 100.0f, 20.0f},
    {"flutter", "Flutter",     "%", 0.0f, 100.0f, 15.0f},
    {"bits",    "Bits",        "bit", 4.0f, 16.0f, 16.0f},
    {"rate",    "Sample Rate", "Hz", 2000.0f, 48000.0f, 48000.0f, Taper::Log},
    {"vinyl",   "Vinyl",       "%", 0.0f, 100.0f, 20.0f},
    {"tone",    "Tone",        "Hz", 1000.0f, 20000.0f, 12000.0f, Taper::Log},
    {"mix",     "Mix",         "%", 0.0f, 100.0f, 100.0f},
    {"output",  "Output",      "dB", -24.0f, 12.0f, 0.0f},
};
constexpr double kQ = 0.7071067811865476;
constexpr float kWowMs = 4.0f, kFlutterMs = 0.4f;
constexpr double kWowHz = 0.55, kFlutterHz = 6.3;
constexpr std::uint32_t kSeed = 0x48454D21u;

class Dust final : public Effect {
public:
    Dust() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        const auto size = static_cast<std::uint32_t>(std::ceil(2.0 * (kWowMs + kFlutterMs) * 0.001 * fs)) + 8;
        line_[0].prepare(size); line_[1].prepare(size);
        lastTone_ = -1.0f;
        noiseHp_.c = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs, 400.0, kQ);
        crackleDecay_ = onePoleCoef(fs, 0.6);
        follow_.set(fs, 5.0, 300.0);
        reset();
    }
    void reset() noexcept override {
        line_[0].clear(); line_[1].clear();
        wowPh_ = flutPh_ = 0.0;
        hold_[0] = hold_[1] = 0.0f; holdCount_ = 1.0f;
        tone_.clear(); noiseHp_.clear();
        rng_ = kSeed;
        crackle_ = 0.0f;
        follow_.env = 0.0f;
        mix_.prepare(fs_, 30, p(Mix) * 0.01f);
        out_.prepare(fs_, 30, dbToGain(p(Output)));
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float tone = p(Tone);
        if (tone != lastTone_) { tone_.set(BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, tone, kQ)); lastTone_ = tone; }
        const float wowMs = p(Wow) * 0.01f * kWowMs, flutMs = p(Flutter) * 0.01f * kFlutterMs;
        const float bits = p(Bits);
        const bool crush = bits < 15.5f;
        const float steps = std::exp2(std::round(bits) - 1.0f);
        const float rate = p(Rate);
        const float holdStep = rate >= 47999.0f ? 1.0f : std::clamp(static_cast<float>(rate / fs_), 0.0f, 1.0f);
        const float vinyl = p(Vinyl) * 0.01f;
        const float hiss = 0.004f * vinyl;
        const float popsPerSample = static_cast<float>(8.0 * vinyl / fs_);
        const float mT = p(Mix) * 0.01f, oT = dbToGain(p(Output));
        const double twoPi = 2.0 * std::numbers::pi;
        const float msToS = static_cast<float>(fs_ * 0.001);
        for (std::uint32_t i = 0; i < n; ++i) {
            const float dl = L[i], dr = R[i];
            line_[0].write(dl); line_[1].write(dr);
            const float d = (wowMs * (1.0f + static_cast<float>(std::sin(twoPi * wowPh_))) +
                             flutMs * (1.0f + static_cast<float>(std::sin(twoPi * flutPh_)))) * msToS;
            float l = line_[0].read(d), r = line_[1].read(d);
            wowPh_ += kWowHz / fs_;   if (wowPh_ >= 1.0) wowPh_ -= 1.0;
            flutPh_ += kFlutterHz / fs_; if (flutPh_ >= 1.0) flutPh_ -= 1.0;
            holdCount_ += holdStep;
            if (holdCount_ >= 1.0f) { holdCount_ -= 1.0f; hold_[0] = l; hold_[1] = r; }
            l = hold_[0]; r = hold_[1];
            if (crush) { l = std::round(l * steps) / steps; r = std::round(r * steps) / steps; }
            l = tone_.l.tick(l); r = tone_.r.tick(r);
            // vinyl, following the music
            const float lvl = follow_.tick(std::max(std::fabs(dl), std::fabs(dr)));
            if (follow_.env < 1e-20f) follow_.env = 0.0f;
            const float playing = std::clamp((20.0f * std::log10(lvl + 1e-9f) + 60.0f) * 0.05f, 0.0f, 1.0f);   // -60 dBFS: none .. -40 dBFS: full
            const float white = nextRandom() * 2.0f - 1.0f;
            const float popDraw = nextRandom(), popAmp = nextRandom(), popSign = nextRandom();
            if (vinyl > 0.0f) {
                if (popDraw < popsPerSample) crackle_ = 0.05f + 0.25f * popAmp;
                const float pop = crackle_ * (popSign * 2.0f - 1.0f);
                crackle_ *= crackleDecay_;
                if (crackle_ < 1e-20f) crackle_ = 0.0f;
                const float noise = (noiseHp_.tick(white * hiss) + pop * vinyl) * playing;
                l += noise; r += noise;
            }
            const float m = mix_.next(mT), o = out_.next(oT);
            L[i] = (dl + m * (l - dl)) * o;
            R[i] = (dr + m * (r - dr)) * o;
        }
    }

private:
    float nextRandom() noexcept {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return static_cast<float>(rng_) * (1.0f / 4294967296.0f);
    }
    double fs_ = 48000;
    DelayLine line_[2];
    double wowPh_ = 0, flutPh_ = 0;
    float hold_[2]{}, holdCount_ = 1;
    float lastTone_ = -1, crackleDecay_ = 0, crackle_ = 0;
    Biquad2 tone_;
    Biquad noiseHp_;
    Envelope follow_;
    std::uint32_t rng_ = kSeed;
    Smoothed mix_, out_;
};

} // namespace

std::unique_ptr<Effect> makeDust() { return std::make_unique<Dust>(); }

} // namespace hc::fx
