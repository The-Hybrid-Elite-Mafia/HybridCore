// HEM DOUBLE — turns one vocal (or guitar) into a wide double-track.
//   A low-cut mono copy feeds a delay line; 2 or 4 read heads sit a little late (DELAY, each
//   voice later than the one before) and drift slowly (DRIFT, three unrelated slow wobbles
//   per head, up to ±1.5 ms) so the copies never line up the same way twice. Heads alternate
//   left / right by WIDTH. The original stays untouched in the middle; AMOUNT = copies' level.
//   Ported from the Rework Playground's Elite Doubler and improved: 4-voice mode, each copy
//   slightly detuned by its drift only (no pitch shifter = no artefacts).
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Amount, Width, Delay, Drift, Voices, LowCut, Output, kCount };
const char* const kVoices[] = {"2 VOICES", "4 VOICES"};
const ParamInfo kParams[kCount] = {
    {"amount", "Amount",  "%", 0.0f, 100.0f, 50.0f},
    {"width",  "Width",   "%", 0.0f, 100.0f, 100.0f},
    {"delay",  "Delay",   "ms", 5.0f, 40.0f, 18.0f, Taper::Log},
    {"drift",  "Drift",   "%", 0.0f, 100.0f, 40.0f},
    {"voices", "Voices",  "", 0.0f, 1.0f, 0.0f, Taper::Choice, kVoices},
    {"lowcut", "Low Cut", "Hz", 20.0f, 1000.0f, 150.0f, Taper::Log},
    {"output", "Output",  "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"SPREAD", "ms", 0.0f, 30.0f}};
constexpr double kQ = 0.7071067811865476;
constexpr float kMaxDriftMs = 1.5f;
constexpr float kLater[4] = {1.0f, 1.35f, 1.17f, 1.55f};          // each head's delay × DELAY
constexpr float kPan[4]   = {1.0f, -1.0f, 0.55f, -0.55f};         // + left, - right (× WIDTH)
constexpr double kRate[4][3] = {{0.31, 0.13, 0.07}, {0.47, 0.11, 0.05}, {0.23, 0.17, 0.09}, {0.39, 0.19, 0.06}};

class Double final : public Effect {
public:
    Double() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        const double maxMs = 40.0 * kLater[3] + kMaxDriftMs + 2.0;
        line_.prepare(static_cast<std::uint32_t>(std::ceil(maxMs * 0.001 * fs)) + 4);
        lastCut_ = -1.0f;
        reset();
    }
    void reset() noexcept override {
        line_.clear();
        cut_.clear();
        for (int v = 0; v < 4; ++v)
            for (int k = 0; k < 3; ++k) phase_[v][k] = 0.137 * (v + 1) + 0.29 * k;
        amount_.prepare(fs_, 50, p(Amount) * 0.01f);
        width_.prepare(fs_, 50, p(Width) * 0.01f);
        delay_.prepare(fs_, 50, p(Delay));
        drift_.prepare(fs_, 50, p(Drift) * 0.01f);
        four_.prepare(fs_, 50, p(Voices) >= 0.5f ? 1.0f : 0.0f);
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float cut = p(LowCut);
        if (cut != lastCut_) { cut_.set(BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, cut, kQ)); lastCut_ = cut; }
        const float aT = p(Amount) * 0.01f, wT = p(Width) * 0.01f, dT = p(Delay), drT = p(Drift) * 0.01f;
        const float fT = p(Voices) >= 0.5f ? 1.0f : 0.0f, oT = dbToGain(p(Output));
        const double twoPi = 2.0 * std::numbers::pi;
        const float msToS = static_cast<float>(fs_ * 0.001);
        float spread = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            const float src = cut_.r.tick(cut_.l.tick(0.5f * (l + r)));
            line_.write(src);
            const float d = delay_.next(dT), dr = drift_.next(drT) * kMaxDriftMs, four = four_.next(fT);
            const float w = width_.next(wT), a = amount_.next(aT), o = out_.next(oT);
            float wl = 0.0f, wr = 0.0f, dMin = 1e9f, dMax = 0.0f;
            for (int v = 0; v < 4; ++v) {
                const float level = v < 2 ? 1.0f : four;
                float wob = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    wob += (k == 0 ? 0.6f : k == 1 ? 0.3f : 0.1f) * static_cast<float>(std::sin(twoPi * phase_[v][k]));
                    phase_[v][k] += kRate[v][k] / fs_;
                    if (phase_[v][k] >= 1.0) phase_[v][k] -= 1.0;
                }
                if (level <= 0.0f) continue;
                const float ms = d * kLater[v] + dr * wob;
                const float x = line_.read(std::max(1.0f, ms * msToS)) * level;
                const float pan = kPan[v] * w;   // +1 = left
                wl += x * (0.5f + 0.5f * pan);
                wr += x * (0.5f - 0.5f * pan);
                if (level > 0.5f) { dMin = std::min(dMin, ms); dMax = std::max(dMax, ms); }
            }
            const float norm = 1.0f / (1.0f + four);   // 4 voices are not twice as loud
            L[i] = (l + a * wl * norm * 1.0f) * o;
            R[i] = (r + a * wr * norm * 1.0f) * o;
            spread = dMax - dMin;
        }
        publishMeter(0, spread);
    }

private:
    double fs_ = 48000;
    DelayLine line_;
    Biquad2 cut_;
    float lastCut_ = -1;
    double phase_[4][3]{};
    Smoothed amount_, width_, delay_, drift_, four_, out_;
};

} // namespace

std::unique_ptr<Effect> makeDouble() { return std::make_unique<Double>(); }

} // namespace hc::fx
