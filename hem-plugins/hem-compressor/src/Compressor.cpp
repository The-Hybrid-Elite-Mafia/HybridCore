// HEM COMPRESSOR: stereo-linked peak compressor with parallel mix, a subtle console colour (ANALOG),
// detector high-pass, trim and a three-step fade. Our own design. Every guess it makes is in
// include/hem/compressor/CompressorConfig.hpp.
// Signal path: detector (optional 12 dB/oct high-pass, loudest side) -> level in dB -> 6 dB soft-knee
// gain computer -> attack / release smoothing in dB (AUTO release follows how long the peak lasted)
// -> gain -> make-up -> parallel mix -> ANALOG colour (tiny 2nd + 3rd harmonic, faint seeded noise)
// -> IN cross-fade (IN off = the input, bit for bit) -> trim * fade.
// Audio thread: no allocation, no locks, no waiting. Parameters are read once per block.
#include "hem/compressor/CompressorConfig.hpp"
#include "hem/compressor/HemCompressor.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <atomic>
#include <cmath>

namespace hem::compressor {
namespace {
using namespace hc::fx;

enum P { Threshold, Ratio, Attack, Release, ScFilter, Mix, Makeup, Trim, Analog, In, Fade, FadeTime, kCount };

const char* const kRatioNames[]   = {"2:1", "4:1", "10:1"};
const char* const kAttackNames[]  = {"0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"};
const char* const kReleaseNames[] = {"0.1 s", "0.3 s", "0.6 s", "1.2 s", "AUTO"};
const char* const kOnOff[]        = {"OFF", "ON"};
const char* const kFadeNames[]    = {"OFF", "FADE OUT", "FADE IN"};   // saved value 1 = FADE OUT, same as the old ON

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
    {"fade", "Fade", "", 0.0f, 2.0f, 0.0f, Taper::Choice, kFadeNames},
    {"fadetime", "Rate-S", "s", 1.0f, 60.0f, 10.0f},
};
const MeterInfo kMeters[] = {{"IN", "dB", -54.0f, 0.0f}, {"OUT", "dB", -54.0f, 0.0f}};

constexpr float kLog10Mul = 8.685889638065035f;   // 20 / ln(10)
constexpr float kDbToNeper = 0.11512925464970229f;  // ln(10) / 20

// Each instance gets its own noise seed (so two stems never hiss in lock-step); reset() restarts it,
// so the same instance always makes the same noise. Set on the message thread only.
std::atomic<std::uint32_t> gSeedCounter{0};

class Compressor final : public Effect {
public:
    Compressor() : seed_(cfg::kNoiseSeed + 0x9E3779B9u * gSeedCounter.fetch_add(1, std::memory_order_relaxed)) {
        if (seed_ == 0) seed_ = cfg::kNoiseSeed;
        setDefaults();
    }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    bool paramActive(int i) const noexcept override { return i != Release || static_cast<int>(p(Release)) != 4; }

    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        scL_.clear(); scR_.clear();
        scHz_ = -1.0f; scOn_ = false;
        dcInL_ = dcInR_ = dcOutL_ = dcOutR_ = 0.0f;
        rng_ = seed_;
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
        const int fadeMode = std::clamp(static_cast<int>(p(Fade) + 0.5f), 0, 2);   // 0 OFF (skip), 1 FADE OUT, 2 FADE IN
        const float fstep = static_cast<float>(1.0 / (std::max(static_cast<double>(p(FadeTime)), 0.05) * fs_));
        const float skipStep = static_cast<float>(1.0 / (std::max(static_cast<double>(cfg::kFadeSkipMs), 0.1) * 0.001 * fs_));
        const float kn = cfg::kKneeDb, hk = kn * 0.5f;
        const float a2 = cfg::kAnalogEven, a3 = cfg::kAnalogOdd, lim = cfg::kAnalogInputLimit;
        const float dcR = static_cast<float>(1.0 - 2.0 * 3.14159265358979323846 * cfg::kAnalogDcHz / fs_);
        // Uniform noise in [-amp, amp] has an RMS of amp / sqrt(3).
        const float noiseAmp = std::pow(10.0f, cfg::kAnalogNoiseDbFS * 0.05f) * 1.7320508f;
        const float noiseScale = noiseAmp / 2147483648.0f;

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
            const float mk = makeup_.next(mkT), mx = mix_.next(mxT);
            wl *= mk; wr *= mk;
            float yl = xl * (1.0f - mx) + wl * mx, yr = xr * (1.0f - mx) + wr * mx;

            // ANALOG: even part (x squared, its DC removed by a 5 Hz high-pass) + a little odd part (x cubed)
            // + faint white noise. The filters always run so switching ANALOG on never clicks.
            const float an = analog_.next(anT);
            const float cl = std::clamp(yl, -lim, lim), cr = std::clamp(yr, -lim, lim);
            const float el = a2 * cl * cl, er = a2 * cr * cr;
            dcOutL_ = el - dcInL_ + dcR * dcOutL_; dcInL_ = el;
            dcOutR_ = er - dcInR_ + dcR * dcOutR_; dcInR_ = er;
            if (an > 0.0f) {
                const float nl = static_cast<float>(static_cast<std::int32_t>(nextRand())) * noiseScale;
                const float nr = static_cast<float>(static_cast<std::int32_t>(nextRand())) * noiseScale;
                yl += (dcOutL_ - a3 * cl * cl * cl + nl) * an;
                yr += (dcOutR_ - a3 * cr * cr * cr + nr) * an;
            }
            const float ie = in_.next(inT);
            yl = xl + (yl - xl) * ie; yr = xr + (yr - xr) * ie;
            deepest = std::max(deepest, gr * ie);

            if (fadeMode == 1) fade_ = std::max(fade_ - fstep, 0.0f);
            else fade_ = std::min(fade_ + (fadeMode == 2 ? fstep : skipStep), 1.0f);
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
        const bool on = hz >= cfg::kScOffBelowHz;
        if (on) {
            const auto c = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, hz, cfg::kScQ);
            scL_.c = c; scR_.c = c;
            if (!scOn_) { scL_.clear(); scR_.clear(); }   // switching on: start from clean memory, so no stale burst / click
        }
        scOn_ = on;
    }
    // xorshift32: per-instance state, no shared libc state, no allocation.
    std::uint32_t nextRand() noexcept { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
    double fs_ = 48000;
    Biquad scL_, scR_;
    float scHz_ = -1.0f;
    bool scOn_ = false;
    std::uint32_t seed_ = 1, rng_ = 1;
    float dcInL_ = 0.0f, dcInR_ = 0.0f, dcOutL_ = 0.0f, dcOutR_ = 0.0f;
    float gr_ = 0.0f, grSlow_ = 0.0f, fade_ = 1.0f;
    Smoothed makeup_, mix_, trim_, in_, analog_;
};

} // namespace

std::unique_ptr<hc::fx::Effect> make() { return std::make_unique<Compressor>(); }

} // namespace hem::compressor
