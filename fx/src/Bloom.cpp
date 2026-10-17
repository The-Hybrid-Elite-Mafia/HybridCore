// HEM BLOOM — reverb that steps out of the way. Idea (not code) from the newest "smart"
// reverbs: the tail ducks only in the frequency bands where the dry sound is loud, so a
// vocal or a snare stays clear while the space still fills the gaps.
// Our design:
//   pre-delay (0–250 ms) → 8-line feedback delay network (Householder matrix, delay times
//   in milliseconds scaled by Size, per-line damping low-pass set by Tone, per-line gain
//   from Decay = the RT60 in seconds) → stereo taps → Width (mid/side)
//   DUCK: the dry (L+R) and the wet are each split into 4 bands at 200 Hz, 1 kHz, 4 kHz
//   (the wet split is complementary: the bands add back to the wet exactly); per band a
//   fast-attack envelope with Release; the wet band is turned down by
//   1 / (1 + 4 · Duck · dry/wet), floored at −24 dB. Duck 0 % = a plain reverb.
//   Mix blends dry and wet. Delay gains / damping follow the knobs every 32 samples
//   (internal counter: block-size independent). Tiny values are flushed to zero.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Size, Decay, PreDelay, Tone, Width, Duck, Release, Mix, kCount };
const ParamInfo kParams[kCount] = {
    {"size",     "Size",      "%", 0.0f, 100.0f, 60.0f},
    {"decay",    "Decay",     "s", 0.2f, 12.0f, 2.0f, Taper::Log},
    {"predelay", "Pre-delay", "ms", 0.0f, 250.0f, 20.0f},
    {"tone",     "Tone",      "%", 0.0f, 100.0f, 50.0f},
    {"width",    "Width",     "%", 0.0f, 150.0f, 100.0f},
    {"duck",     "Duck",      "%", 0.0f, 100.0f, 50.0f},
    {"release",  "Duck Release", "ms", 20.0f, 2000.0f, 250.0f, Taper::Log},
    {"mix",      "Mix",       "%", 0.0f, 100.0f, 25.0f},
};
constexpr int kLines = 8, kBands = 4;
constexpr double kBaseMs[kLines] = {29.7, 37.1, 41.1, 43.7, 53.0, 59.9, 67.1, 79.3};
constexpr double kSplitHz[kBands - 1] = {200.0, 1000.0, 4000.0};
constexpr double kMaxScale = 1.75;
constexpr std::uint32_t kSub = 32;

inline float flush(float x) noexcept { return std::fabs(x) < 1e-15f ? 0.0f : x; }

// Complementary 4-band split: band0 = LP200(x), band1 = LP1k(x − b0), … last = remainder.
struct Split4 {
    Biquad lp[kBands - 1];
    void set(const BiquadCoefs* c) noexcept { for (int i = 0; i < kBands - 1; ++i) lp[i].c = c[i]; }
    void clear() noexcept { for (auto& f : lp) f.clear(); }
    void run(float x, float* band) noexcept {
        float rest = x;
        for (int i = 0; i < kBands - 1; ++i) { band[i] = lp[i].tick(rest); rest -= band[i]; }
        band[kBands - 1] = rest;
    }
};

class Bloom final : public Effect {
public:
    Bloom() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        for (int i = 0; i < kLines; ++i) lines_[i].prepare(static_cast<std::uint32_t>(kBaseMs[i] * 0.001 * kMaxScale * fs) + 4);
        pre_.prepare(static_cast<std::uint32_t>(0.25 * fs) + 4);
        for (int b = 0; b < kBands - 1; ++b) split_[b] = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs, kSplitHz[b], 0.70710678);
        reset();
    }
    void reset() noexcept override {
        for (auto& l : lines_) l.clear();
        pre_.clear();
        for (auto& d : damp_) d = 0.0f;
        for (auto& s : wetSplit_) { s.set(split_); s.clear(); }
        drySplit_.set(split_); drySplit_.clear();
        for (int b = 0; b < kBands; ++b) { envDry_[b].env = 0.0f; envWet_[b].env = 0.0f; }
        size_.prepare(fs_, 50.0, scaleFor(p(Size)));
        pred_.prepare(fs_, 50.0, p(PreDelay) * 0.001f * static_cast<float>(fs_));
        width_.prepare(fs_, 20.0, p(Width) * 0.01f);
        duck_.prepare(fs_, 20.0, p(Duck) * 0.01f);
        mix_.prepare(fs_, 20.0, p(Mix) * 0.01f);
        counter_ = 0;
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        if (pre_.buf.empty()) return;
        const float sT = scaleFor(p(Size)), pT = p(PreDelay) * 0.001f * static_cast<float>(fs_), wT = p(Width) * 0.01f,
                    dT = p(Duck) * 0.01f, mT = p(Mix) * 0.01f;
        float most = 1.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float scale = size_.next(sT), pd = pred_.next(pT), width = width_.next(wT), duck = duck_.next(dT), mix = mix_.next(mT);
            if (counter_ % kSub == 0) update(scale);
            ++counter_;
            const float dl = L[i], dr = R[i], dm = 0.5f * (dl + dr);
            // Pre-delay, then the network.
            pre_.write(dm);
            const float in = pre_.read(pd);
            float s[kLines];
            float sum = 0.0f;
            for (int k = 0; k < kLines; ++k) {
                float x = lines_[k].read(delay_[k] - 1.0f);
                damp_[k] = flush(x + dampC_ * (damp_[k] - x));
                s[k] = damp_[k] * gain_[k];
                sum += s[k];
            }
            const float h = sum * (2.0f / kLines);
            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < kLines; ++k) {
                const float fb = s[k] - h;
                lines_[k].write(flush(fb + in * ((k & 1) ? -0.35f : 0.35f)));
                wl += (k < 4 ? 1.0f : -1.0f) * ((k & 1) ? -s[k] : s[k]);
                wr += ((k & 2) ? -1.0f : 1.0f) * s[k];
            }
            wl *= 0.35f; wr *= 0.35f;
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            wl = mid + side; wr = mid - side;
            // Duck the wet per band where the dry is loud.
            float bd[kBands], bl[kBands], br[kBands];
            drySplit_.run(dm, bd);
            wetSplit_[0].run(wl, bl);
            wetSplit_[1].run(wr, br);
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < kBands; ++b) {
                const float ed = envDry_[b].tick(std::fabs(bd[b]));
                const float ew = envWet_[b].tick(0.5f * (std::fabs(bl[b]) + std::fabs(br[b])));
                if (envDry_[b].env < 1e-12f) envDry_[b].env = 0.0f;
                if (envWet_[b].env < 1e-12f) envWet_[b].env = 0.0f;
                const float ratio = std::min(ed / (ew + 1e-9f), 100.0f);
                const float g = std::max(1.0f / (1.0f + 4.0f * duck * ratio), 0.063f);
                most = std::min(most, g);
                ol += bl[b] * g;
                orr += br[b] * g;
            }
            L[i] = dl + mix * (flush(ol) - dl);
            R[i] = dr + mix * (flush(orr) - dr);
        }
        publishReduction(n > 0 ? gainToDb(most) : 0.0f);
    }

private:
    [[nodiscard]] static float scaleFor(float sizePct) noexcept { return 0.25f + 1.5f * sizePct * 0.01f; }
    void update(float scale) noexcept {
        const double t60 = std::max(0.05, static_cast<double>(p(Decay)));
        for (int k = 0; k < kLines; ++k) {
            delay_[k] = static_cast<float>(kBaseMs[k] * 0.001 * scale * fs_);
            gain_[k] = static_cast<float>(std::pow(10.0, -3.0 * delay_[k] / (t60 * fs_)));
        }
        const double fc = 16000.0 + (1500.0 - 16000.0) * (1.0 - p(Tone) * 0.01);   // Tone 100 % = bright
        dampC_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * std::min(fc, fs_ * 0.45) / fs_));
        const double relMs = p(Release);
        for (int b = 0; b < kBands; ++b) { envDry_[b].set(fs_, 2.0, relMs); envWet_[b].set(fs_, 2.0, relMs); }
    }

    double fs_ = 48000;
    DelayLine lines_[kLines], pre_;
    float delay_[kLines]{}, gain_[kLines]{}, damp_[kLines]{};
    float dampC_ = 0.0f;
    BiquadCoefs split_[kBands - 1];
    Split4 wetSplit_[2], drySplit_;
    Envelope envDry_[kBands], envWet_[kBands];
    Smoothed size_, pred_, width_, duck_, mix_;
    std::uint32_t counter_ = 0;
};

} // namespace

std::unique_ptr<Effect> makeBloom() { return std::make_unique<Bloom>(); }

} // namespace hc::fx
