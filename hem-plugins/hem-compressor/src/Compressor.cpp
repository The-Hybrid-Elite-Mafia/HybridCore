// HEM COMPRESSOR: stereo-linked peak compressor with parallel mix, optional saturation, detector
// high-pass, trim and a fade. Our own design. Every guess it makes is in include/hem/compressor/CompressorConfig.hpp.
// Signal path: detector (optional 12 dB/oct high-pass, loudest side) -> level in dB -> 6 dB soft-knee
// gain computer -> attack / release smoothing in dB (AUTO release follows how long the peak lasted)
// -> gain -> optional tanh saturation -> make-up -> parallel mix -> IN cross-fade -> trim * fade.
// Audio thread: no allocation, no locks, no waiting. Parameters are read once per block.
#include "hem/compressor/CompressorConfig.hpp"
#include "hem/compressor/HemCompressor.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <cmath>

namespace hem::compressor {
namespace {
using namespace hc::fx;

enum P { Threshold, Ratio, Attack, Release, ScFilter, Mix, Makeup, Trim, Analog, In, Fade, FadeTime, kCount };

const char* const kRatioNames[]   = {"2:1", "4:1", "10:1"};
const char* const kAttackNames[]  = {"0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"};
const char* const kReleaseNames[] = {"0.1 s", "0.3 s", "0.6 s", "1.2 s", "AUTO"};
const char* const kOnOff[]        = {"OFF", "ON"};

const ParamInfo kParams[kCount] = {
    {"threshold", "Threshold", "dB", -15.0f, 15.0f, 0.0f},
    {"ratio", "Ratio", "", 0.0f, 2.0f, 1.0f, Taper::Choice, kRatioNames},
    {"attack", "Attack", "", 0.0f, 5.0f, 3.0f, Taper::Choice, kAttackNames},
    {"release", "Release", "", 0.0f, 4.0f, 2.0f, Taper::Choice, kReleaseNames},
    {"scfilter", "SC Filter", "Hz", 0.0f, 500.0f, 0.0f},
    {"mix", "Mix", "%", 0.0f, 100.0f, 100.0f},
    {"makeup", "Make-up", "dB", -5.0f, 15.0f, 0.0f},
    {"trim", "Trim", "dB", -18.0f, 18.0f, 0.0f},
    {"analog", "Analog", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"in", "In", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"fade", "Fade", "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"fadetime", "Rate-S", "s", 1.0f, 60.0f, 10.0f},
};
const MeterInfo kMeters[] = {{"IN", "dB", -54.0f, 0.0f}, {"OUT", "dB", -54.0f, 0.0f}};

constexpr float kLog10Mul = 8.685889638065035f;   // 20 / ln(10)
constexpr float kDbToNeper = 0.11512925464970229f;  // ln(10) / 20

class Compressor final : public Effect {
public:
    Compressor() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    bool paramActive(int i) const noexcept override { return i != Release || static_cast<int>(p(Release)) != 4; }

    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        scL_.clear(); scR_.clear();
        scHz_ = -1.0f;
        gr_ = 0.0f; grSlow_ = 0.0f;
        fade_ = 1.0f;
        makeup_.prepare(fs_, cfg::kSmoothMs, std::pow(10.0f, p(Makeup) * 0.05f));
        mix_.prepare(fs_, cfg::kSmoothMs, p(Mix) * 0.01f);
        trim_.prepare(fs_, cfg::kSmoothMs, std::pow(10.0f, p(Trim) * 0.05f));
        in_.prepare(fs_, cfg::kSwitchMs, p(In) >= 0.5f ? 1.0f : 0.0f);
        analog_.prepare(fs_, cfg::kSwitchMs, p(Analog) >= 0.5f ? 1.0f : 0.0f);
        publishReduction(0.0f);
        publishMeter(0, -120.0f); publishMeter(1, -120.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const int ratioIdx = std::clamp(static_cast<int>(p(Ratio)), 0, 2);
        const int attIdx = std::clamp(static_cast<int>(p(Attack)), 0, 5);
        const int relIdx = std::clamp(static_cast<int>(p(Release)), 0, 4);
        const float thr = cfg::kThrKnobToDbFS + p(Threshold);
        const float slope = 1.0f - 1.0f / cfg::kRatios[static_cast<std::size_t>(ratioIdx)];
        const float ca = onePoleCoef(fs_, cfg::kAttackMs[static_cast<std::size_t>(attIdx)]);
        const bool autoRel = relIdx == 4;
        const float crFixed = onePoleCoef(fs_, cfg::kReleaseS[static_cast<std::size_t>(relIdx)] * 1000.0);
        const float crFast = onePoleCoef(fs_, cfg::kAutoFastRelS * 1000.0), caSlow = onePoleCoef(fs_, cfg::kAutoSlowAttS * 1000.0), crSlow = onePoleCoef(fs_, cfg::kAutoSlowRelS * 1000.0);
        updateFilter();
        const bool hpf = scHz_ >= cfg::kScOffBelowHz;
        const float mkT = std::pow(10.0f, p(Makeup) * 0.05f), mxT = p(Mix) * 0.01f;
        const float trT = std::pow(10.0f, p(Trim) * 0.05f);
        const float inT = p(In) >= 0.5f ? 1.0f : 0.0f, anT = p(Analog) >= 0.5f ? 1.0f : 0.0f;
        const bool fadeOut = p(Fade) >= 0.5f;
        const float fstep = static_cast<float>(1.0 / (std::max(static_cast<double>(p(FadeTime)), 0.05) * fs_));
        const float kn = cfg::kKneeDb, hk = kn * 0.5f;
        const float drive = cfg::kAnalogDrive, bias = cfg::kAnalogBias, tb = std::tanh(drive * bias);

        float deepest = 0.0f, inPk = 0.0f, outPk = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float xl = L[i], xr = R[i];
            const float pin = std::max(std::fabs(xl), std::fabs(xr));
            inPk = std::max(inPk, pin);

            float dl = xl, dr = xr;
            if (hpf) { dl = scL_.tick(xl); dr = scR_.tick(xr); }
            const float det = std::max(std::fabs(dl), std::fabs(dr));
            const float lvl = kLog10Mul * std::log(det + 1e-9f);
            const float over = lvl - thr;
            float gt = 0.0f;
            if (over > hk) gt = slope * over;
            else if (over > -hk) { const float t = over + hk; gt = slope * t * t / (2.0f * kn); }
            float gr;
            if (autoRel) {   // two stages: the slow one only fills on longer peaks, so the release follows how long the peak lasted
                gr_ = gt > gr_ ? gt + ca * (gr_ - gt) : gt + crFast * (gr_ - gt);
                grSlow_ = gt > grSlow_ ? gt + caSlow * (grSlow_ - gt) : gt + crSlow * (grSlow_ - gt);
                gr = std::max(gr_, grSlow_);
            } else {
                gr_ = gt > gr_ ? gt + ca * (gr_ - gt) : gt + crFixed * (gr_ - gt);
                grSlow_ = 0.0f;
                gr = gr_;
            }
            const float g = std::exp(-gr * kDbToNeper);

            float wl = xl * g, wr = xr * g;
            const float an = analog_.next(anT);
            if (an > 1e-4f) {
                const float sl = (std::tanh(drive * (wl + bias)) - tb) / drive;
                const float sr = (std::tanh(drive * (wr + bias)) - tb) / drive;
                wl += (sl - wl) * an; wr += (sr - wr) * an;
            }
            const float mk = makeup_.next(mkT), mx = mix_.next(mxT);
            wl *= mk; wr *= mk;
            float yl = xl * (1.0f - mx) + wl * mx, yr = xr * (1.0f - mx) + wr * mx;
            const float ie = in_.next(inT);
            yl = xl + (yl - xl) * ie; yr = xr + (yr - xr) * ie;
            deepest = std::max(deepest, gr * ie);

            fade_ += fadeOut ? -fstep : fstep;
            fade_ = std::clamp(fade_, 0.0f, 1.0f);
            const float m = trim_.next(trT) * fade_;
            yl *= m; yr *= m;
            L[i] = yl; R[i] = yr;
            outPk = std::max(outPk, std::max(std::fabs(yl), std::fabs(yr)));
        }
        publishReduction(-deepest);
        publishMeter(0, gainToDb(inPk));
        publishMeter(1, gainToDb(outPk));
    }

private:
    void updateFilter() noexcept {
        const float hz = p(ScFilter);
        if (hz == scHz_) return;
        scHz_ = hz;
        if (hz >= cfg::kScOffBelowHz) {
            const auto c = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, hz, cfg::kScQ);
            scL_.c = c; scR_.c = c;
        }
    }
    double fs_ = 48000;
    Biquad scL_, scR_;
    float scHz_ = -1.0f;
    float gr_ = 0.0f, grSlow_ = 0.0f, fade_ = 1.0f;
    Smoothed makeup_, mix_, trim_, in_, analog_;
};

} // namespace

std::unique_ptr<hc::fx::Effect> make() { return std::make_unique<Compressor>(); }

} // namespace hem::compressor
