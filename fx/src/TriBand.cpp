// HEM TRIBAND — three-band compressor (lows / mids / highs on their own).
//   Split : Linkwitz-Riley 24 dB/oct at LOW/MID and MID/HIGH. The low band also passes the
//           upper crossover's all-pass, so with no compression the three bands add back flat.
//   Each band: RMS detector (10 ms), soft knee (6 dB), threshold / ratio, attack / release on
//           the gain reduction, then its own gain. Stereo-linked per band.
//   LISTEN plays one band alone (to set the crossovers by ear). MIX blends against the
//   split-and-summed dry (same phase, so parallel use never combs).
//   Ported from the Rework Playground's Elite Multiband and improved: band LISTEN, per-band
//   meters on the faceplate.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { LowX, HighX, LowThr, LowRatio, LowGain, MidThr, MidRatio, MidGain, HighThr, HighRatio, HighGain,
         Attack, Release, Listen, Mix, Output, kCount };
const char* const kListen[] = {"ALL", "LOW", "MID", "HIGH"};
const ParamInfo kParams[kCount] = {
    {"lowx",      "Low / Mid",      "Hz", 40.0f, 1000.0f, 180.0f, Taper::Log},
    {"highx",     "Mid / High",     "Hz", 1000.0f, 12000.0f, 3500.0f, Taper::Log},
    {"lowthr",    "Low Threshold",  "dB", -50.0f, 0.0f, 0.0f},
    {"lowratio",  "Low Ratio",      ":1", 1.0f, 10.0f, 2.0f},
    {"lowgain",   "Low Gain",       "dB", -12.0f, 12.0f, 0.0f},
    {"midthr",    "Mid Threshold",  "dB", -50.0f, 0.0f, 0.0f},
    {"midratio",  "Mid Ratio",      ":1", 1.0f, 10.0f, 2.0f},
    {"midgain",   "Mid Gain",       "dB", -12.0f, 12.0f, 0.0f},
    {"highthr",   "High Threshold", "dB", -50.0f, 0.0f, 0.0f},
    {"highratio", "High Ratio",     ":1", 1.0f, 10.0f, 2.0f},
    {"highgain",  "High Gain",      "dB", -12.0f, 12.0f, 0.0f},
    {"attack",    "Attack",         "ms", 0.5f, 100.0f, 15.0f, Taper::Log},
    {"release",   "Release",        "ms", 20.0f, 1000.0f, 150.0f, Taper::Log},
    {"listen",    "Listen",         "", 0.0f, 3.0f, 0.0f, Taper::Choice, kListen},
    {"mix",       "Mix",            "%", 0.0f, 100.0f, 100.0f},
    {"output",    "Output",         "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"LOW GR", "dB", -18.0f, 0.0f}, {"MID GR", "dB", -18.0f, 0.0f}, {"HIGH GR", "dB", -18.0f, 0.0f}};
constexpr double kQ = 0.7071067811865476;

struct Lr4 {   // two Butterworth stages = Linkwitz-Riley 24 dB/oct
    Biquad a, b;
    void clear() noexcept { a.clear(); b.clear(); }
    void set(const BiquadCoefs& c) noexcept { a.c = c; b.c = c; }
    [[nodiscard]] float tick(float x) noexcept { return b.tick(a.tick(x)); }
};

struct BandComp {
    float rms = 0, gr = 0;   // mean square, gain reduction dB (>= 0)
    float rmsCoef = 0, att = 0, rel = 0;
    void clear() noexcept { rms = gr = 0; }
    // Returns the gain reduction (dB, >= 0) for this sample.
    float tick(float l, float r, float thr, float ratio) noexcept {
        const float x = 0.5f * (l * l + r * r);
        rms = x + rmsCoef * (rms - x);
        if (rms < 1e-20f) rms = 0.0f;
        const float lvl = 10.0f * std::log10(rms * 2.0f + 1e-20f);   // as a sine peak
        constexpr float knee = 6.0f;
        const float over = lvl - thr;
        float want = 0.0f;
        const float slope = 1.0f - 1.0f / ratio;
        if (over >= knee * 0.5f) want = over * slope;
        else if (over > -knee * 0.5f) { const float t = over + knee * 0.5f; want = slope * t * t / (2.0f * knee); }
        gr = want + (want > gr ? att : rel) * (gr - want);
        return gr;
    }
};

class TriBand final : public Effect {
public:
    TriBand() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        lastLo_ = lastHi_ = -1.0f;
        lastAtk_ = lastRel_ = -1.0f;
        reset();
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { lp1_[c].clear(); hp1_[c].clear(); lp2_[c].clear(); hp2_[c].clear(); apLo_[c].clear(); apHi_[c].clear(); }
        for (auto& b : comp_) b.clear();
        const double rmsMs = 10.0;
        for (auto& b : comp_) b.rmsCoef = onePoleCoef(fs_, rmsMs);
        for (int b = 0; b < 3; ++b) gain_[b].prepare(fs_, 20, dbToGain(p(LowGain + 3 * b)));
        mix_.prepare(fs_, 20, p(Mix) * 0.01f);
        out_.prepare(fs_, 20, dbToGain(p(Output)));
        for (int b = 0; b < 3; ++b) publishMeter(b, 0.0f);
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float lo = p(LowX), hi = std::max(p(HighX), lo * 1.5f);
        if (lo != lastLo_ || hi != lastHi_) {
            const auto lp1 = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, lo, kQ), hp1 = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, lo, kQ);
            const auto lp2 = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, hi, kQ), hp2 = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, hi, kQ);
            for (int c = 0; c < 2; ++c) {
                lp1_[c].set(lp1); hp1_[c].set(hp1); lp2_[c].set(lp2); hp2_[c].set(hp2);
                apLo_[c].set(lp2); apHi_[c].set(hp2);
            }
            lastLo_ = lo; lastHi_ = hi;
        }
        const float atk = p(Attack), rel = p(Release);
        if (atk != lastAtk_ || rel != lastRel_) {
            for (auto& b : comp_) { b.att = onePoleCoef(fs_, atk); b.rel = onePoleCoef(fs_, rel); }
            lastAtk_ = atk; lastRel_ = rel;
        }
        float thr[3], ratio[3], gT[3];
        for (int b = 0; b < 3; ++b) {
            thr[b] = p(LowThr + 3 * b);
            ratio[b] = std::max(1.0f, p(LowRatio + 3 * b));
            gT[b] = dbToGain(p(LowGain + 3 * b));
        }
        const int listen = static_cast<int>(p(Listen));
        const float mT = p(Mix) * 0.01f, oT = dbToGain(p(Output));
        float maxGr[3] = {0, 0, 0};
        for (std::uint32_t i = 0; i < n; ++i) {
            float band[3][2];
            const float in[2] = {L[i], R[i]};
            for (int c = 0; c < 2; ++c) {
                const float low  = lp1_[c].tick(in[c]);
                const float rest = hp1_[c].tick(in[c]);
                band[1][c] = lp2_[c].tick(rest);
                band[2][c] = hp2_[c].tick(rest);
                band[0][c] = apLo_[c].tick(low) + apHi_[c].tick(low);   // same phase turn as mid + high
            }
            float wet[2] = {0, 0}, dry[2] = {0, 0};
            for (int b = 0; b < 3; ++b) {
                const float grDb = comp_[b].tick(band[b][0], band[b][1], thr[b], ratio[b]);
                maxGr[b] = std::max(maxGr[b], grDb);
                const float g = std::exp(-grDb * 0.1151292546f) * gain_[b].next(gT[b]);
                const bool on = listen == 0 || listen == b + 1;
                for (int c = 0; c < 2; ++c) {
                    if (on) wet[c] += band[b][c] * g;
                    if (on) dry[c] += band[b][c];
                }
            }
            const float m = mix_.next(mT), o = out_.next(oT);
            L[i] = (dry[0] + m * (wet[0] - dry[0])) * o;
            R[i] = (dry[1] + m * (wet[1] - dry[1])) * o;
        }
        for (int b = 0; b < 3; ++b) publishMeter(b, -maxGr[b]);
        publishReduction(-std::max({maxGr[0], maxGr[1], maxGr[2]}));
    }

private:
    double fs_ = 48000;
    Lr4 lp1_[2], hp1_[2], lp2_[2], hp2_[2], apLo_[2], apHi_[2];
    BandComp comp_[3];
    float lastLo_ = -1, lastHi_ = -1, lastAtk_ = -1, lastRel_ = -1;
    Smoothed gain_[3], mix_, out_;
};

} // namespace

std::unique_ptr<Effect> makeTriBand() { return std::make_unique<TriBand>(); }

} // namespace hc::fx
