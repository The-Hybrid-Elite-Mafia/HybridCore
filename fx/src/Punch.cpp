// HEM PUNCH — transient shaper. More (or less) hit, more (or less) ring-out, without touching
// the level in between. Design:
//   Three envelope followers on the louder side (stereo-linked, so the image never shifts):
//     fast  : fast attack, slow release  — jumps on a hit
//     slow  : slow attack, slow release  — the gap fast - slow (dB) = the hit
//     hold  : fast attack, fast release  — the gap fast - hold (dB) = the ring-out
//   The detector listens through an 80 Hz high-pass (FOCUS on) so a long sub tail does not
//   smear the hits. Gain = Attack × hit + Sustain × ring, ±15 dB at most. No latency.
//   Ported from the Rework Playground's Elite Transient and improved: focus filter, CLIP GUARD
//   (a soft ceiling at 0 dBFS so a boosted hit can't overshoot), stereo-linked detection.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Attack, Sustain, Speed, Focus, Guard, Mix, Output, kCount };
const char* const kSpeeds[] = {"FAST", "MEDIUM", "SLOW"};
const char* const kOnOff[]  = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"attack",  "Attack",     "%", -100.0f, 100.0f, 0.0f},
    {"sustain", "Sustain",    "%", -100.0f, 100.0f, 0.0f},
    {"speed",   "Speed",      "", 0.0f, 2.0f, 1.0f, Taper::Choice, kSpeeds},
    {"focus",   "Focus",      "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"guard",   "Clip Guard", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"mix",     "Mix",        "%", 0.0f, 100.0f, 100.0f},
    {"output",  "Output",     "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"SHAPE", "dB", -15.0f, 15.0f, true}};

struct Times { double fastAtk, fastRel, slowAtk, slowRel, holdRel; };
Times timesFor(int speed) noexcept {
    switch (speed) {
    case 0:  return {0.3, 25.0, 8.0, 60.0, 6.0};      // tight drums, hats
    case 2:  return {1.5, 80.0, 30.0, 250.0, 30.0};   // bass, full mix
    default: return {0.6, 45.0, 15.0, 120.0, 12.0};
    }
}
constexpr float kMaxDb = 15.0f;

inline float follow(float env, float x, float atk, float rel) noexcept { return x + (x > env ? atk : rel) * (env - x); }
inline float toDb(float x) noexcept { return 20.0f * std::log10(x + 1e-9f); }
// Soft ceiling: untouched below 0.9, rounds into 1.0.
inline float guard(float x) noexcept {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float over = a - 0.9f;
    return std::copysign(0.9f + 0.1f * std::tanh(over * 10.0f), x);
}

class Punch final : public Effect {
public:
    Punch() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        focus_.set(BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs, 80.0, 0.7071));
        lastSpeed_ = -1;
        reset();
    }
    void reset() noexcept override {
        focus_.clear();
        envFast_ = envSlow_ = envHold_ = 0.0f;
        mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        out_.prepare(fs_, 20, dbToGain(p(Output)));
        publishMeter(0, 0.0f);
    }
    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const int speed = static_cast<int>(p(Speed));
        if (speed != lastSpeed_) {
            const Times t = timesFor(speed);
            aFast_ = onePoleCoef(fs_, t.fastAtk); rFast_ = onePoleCoef(fs_, t.fastRel);
            aSlow_ = onePoleCoef(fs_, t.slowAtk); rSlow_ = onePoleCoef(fs_, t.slowRel);
            rHold_ = onePoleCoef(fs_, t.holdRel);
            lastSpeed_ = speed;
        }
        const float attack = p(Attack) * 0.01f, sustain = p(Sustain) * 0.01f;
        const bool focus = p(Focus) >= 0.5f;
        const bool guardOn = p(Guard) >= 0.5f && (attack > 0.0f || sustain > 0.0f);   // only while something is boosted
        const float mT = p(Mix) * 0.01f, oT = dbToGain(p(Output));
        float shown = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            float det = std::max(std::fabs(l), std::fabs(r));
            const float hp = focus_.l.tick(0.5f * (l + r));   // always run: switching FOCUS never clicks
            if (focus) det = std::fabs(hp) * 1.41f;
            envFast_ = follow(envFast_, det, aFast_, rSlow_);
            envSlow_ = follow(envSlow_, det, aSlow_, rSlow_);
            envHold_ = follow(envHold_, det, aFast_, rHold_);
            const float fastDb = toDb(envFast_);
            const float hitDb  = std::clamp(fastDb - toDb(envSlow_), 0.0f, 24.0f);
            const float ringDb = std::clamp(fastDb - toDb(envHold_), 0.0f, 24.0f);
            const float gDb = std::clamp(attack * hitDb * 0.75f + sustain * ringDb * 0.75f, -kMaxDb, kMaxDb);
            const float g = std::exp(gDb * 0.1151292546f);
            const float m = mix_.next(mT), o = out_.next(oT);
            float yl = (l + m * (l * g - l)) * o, yr = (r + m * (r * g - r)) * o;
            if (guardOn) { yl = guard(yl); yr = guard(yr); }
            L[i] = yl; R[i] = yr;
            if (std::fabs(gDb) > std::fabs(shown)) shown = gDb;
            if (envFast_ < 1e-20f) envFast_ = 0.0f;
            if (envSlow_ < 1e-20f) envSlow_ = 0.0f;
            if (envHold_ < 1e-20f) envHold_ = 0.0f;
        }
        publishMeter(0, shown);
    }

private:
    double fs_ = 48000;
    Biquad2 focus_;
    float envFast_ = 0, envSlow_ = 0, envHold_ = 0;
    float aFast_ = 0, rFast_ = 0, aSlow_ = 0, rSlow_ = 0, rHold_ = 0;
    int lastSpeed_ = -1;
    Smoothed mix_, out_;
};

} // namespace

std::unique_ptr<Effect> makePunch() { return std::make_unique<Punch>(); }

} // namespace hc::fx
