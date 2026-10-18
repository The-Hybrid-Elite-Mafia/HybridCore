// HEM CALM — resonance tamer. Idea (not code) from the newest "dynamic resonance
// suppressor" plug-ins: instead of a fixed EQ cut, find the frequencies that ring out
// above their neighbours right now, and turn only those down, only while they ring.
// Our design:
//   STFT: 2048-point frames, hop 512, square-root Hann in and out (overlap-add is exact),
//         frames timed by an internal sample counter (block-size independent);
//         latency = 2048 samples (42.7 ms at 48 kHz), the dry path is delayed to match.
//   Per frame: magnitude of L+R (the same gains go to both sides: the stereo image holds);
//   a spectral envelope = the average magnitude over a band around each bin, as wide as a
//   fraction of its frequency (Sharpness: wide → only broad bumps…narrow → sharp rings);
//   how far a bin pokes above it, less 3 dB, up to Depth, is the cut. Cuts glide per bin
//   (Speed sets attack; release is 4× slower), are smoothed over 3 neighbouring bins, and
//   only act between Low Hz and High Hz.
//   Mix blends with the delayed dry; Delta plays only what was removed.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

#include <complex>

namespace hc::fx {
namespace {

enum P { Depth, Sharpness, Speed, LowHz, HighHz, Mix, Delta, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"depth",     "Depth",     "dBr", 0.0f, 12.0f, 5.0f},
    {"sharpness", "Sharpness", "%", 0.0f, 100.0f, 50.0f},
    {"speed",     "Speed",     "%", 0.0f, 100.0f, 50.0f},
    {"lowhz",     "Low Hz",    "Hz", 20.0f, 20000.0f, 200.0f, Taper::Log},
    {"highhz",    "High Hz",   "Hz", 20.0f, 20000.0f, 16000.0f, Taper::Log},
    {"mix",       "Mix",       "%", 0.0f, 100.0f, 100.0f},
    {"delta",     "Delta",     "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
};

constexpr int kN = 2048, kHop = 512, kBins = kN / 2 + 1;

// In-place radix-2 complex FFT (inverse = conjugate trick), twiddles precomputed.
class Fft {
public:
    Fft() {
        for (int i = 0; i < kN / 2; ++i) tw_[static_cast<std::size_t>(i)] = std::polar(1.0f, static_cast<float>(-2.0 * std::numbers::pi * i / kN));
        int bits = 0;
        while ((1 << bits) < kN) ++bits;
        for (int i = 0; i < kN; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[static_cast<std::size_t>(i)] = r;
        }
    }
    void forward(std::complex<float>* x) const noexcept {
        for (int i = 0; i < kN; ++i) {
            const int r = rev_[static_cast<std::size_t>(i)];
            if (r > i) std::swap(x[i], x[r]);
        }
        for (int len = 2; len <= kN; len <<= 1) {
            const int half = len / 2, step = kN / len;
            for (int i = 0; i < kN; i += len)
                for (int k = 0; k < half; ++k) {
                    const std::complex<float> t = x[i + k + half] * tw_[static_cast<std::size_t>(k * step)];
                    x[i + k + half] = x[i + k] - t;
                    x[i + k] += t;
                }
        }
    }
    void inverse(std::complex<float>* x) const noexcept {
        for (int i = 0; i < kN; ++i) x[i] = std::conj(x[i]);
        forward(x);
        const float s = 1.0f / kN;
        for (int i = 0; i < kN; ++i) x[i] = std::conj(x[i]) * s;
    }

private:
    std::array<std::complex<float>, kN / 2> tw_{};
    std::array<int, kN> rev_{};
};

class Calm final : public Effect {
public:
    Calm() {
        setDefaults();
        for (int i = 0; i < kN; ++i)   // periodic sqrt-Hann: (sqrt-Hann)² at hop N/4 sums to 2
            win_[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kN)));
    }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        for (int c = 0; c < 2; ++c) {
            in_[c].assign(kN, 0.0f);
            out_[c].assign(2 * kN, 0.0f);
            dry_[c].assign(2 * kN, 0.0f);
            spec_[c].assign(kN, {});
        }
        gainDb_.assign(kBins, 0.0f);
        target_.assign(kBins, 0.0f);
        mag_.assign(kBins, 0.0f);
        prefix_.assign(kBins + 1, 0.0);
        reset();
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) {
            std::fill(in_[c].begin(), in_[c].end(), 0.0f);
            std::fill(out_[c].begin(), out_[c].end(), 0.0f);
            std::fill(dry_[c].begin(), dry_[c].end(), 0.0f);
        }
        std::fill(gainDb_.begin(), gainDb_.end(), 0.0f);
        t_ = 0;
        mix_.prepare(fs_, 20.0, p(Mix) * 0.01f);
        delta_.prepare(fs_, 20.0, p(Delta));
        publishReduction(0.0f);
    }
    std::uint32_t latency() const noexcept override { return kN; }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        if (in_[0].empty()) return;
        const float mT = p(Mix) * 0.01f, dT = p(Delta);
        float* ch[2] = {L, R};
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint64_t t = t_++;
            const auto ip = static_cast<std::size_t>(t % kN), op = static_cast<std::size_t>(t % (2 * kN));
            float wet[2], dry[2];
            for (int c = 0; c < 2; ++c) {
                in_[c][ip] = ch[c][i];
                dry_[c][op] = ch[c][i];
                // Output for time t: frames up to time t-1 have been added; sample t − kN of the input.
                wet[c] = out_[c][op];
                out_[c][op] = 0.0f;
                dry[c] = dry_[c][static_cast<std::size_t>((t + kN) % (2 * kN))];   // input at t − kN
            }
            if ((t + 1) % kHop == 0) frame(t);
            const float m = mix_.next(mT), d = delta_.next(dT);
            for (int c = 0; c < 2; ++c) {
                const float y = dry[c] + m * (wet[c] - dry[c]);
                ch[c][i] = y + d * ((dry[c] - y) - y);   // Delta: dry − processed (what was removed)
            }
        }
    }

private:
    // Frame of inputs t−N+1 … t; its output lands at times t+1 … t+N (latency N).
    void frame(std::uint64_t t) noexcept {
        for (int c = 0; c < 2; ++c) {
            auto& s = spec_[c];
            for (int k = 0; k < kN; ++k)
                s[static_cast<std::size_t>(k)] = {in_[c][static_cast<std::size_t>((t + 1 + static_cast<std::uint64_t>(k)) % kN)] * win_[static_cast<std::size_t>(k)], 0.0f};
            fft_.forward(s.data());
        }
        // Detection on the stereo sum.
        for (int k = 0; k < kBins; ++k)
            mag_[static_cast<std::size_t>(k)] = 0.5f * (std::abs(spec_[0][static_cast<std::size_t>(k)]) + std::abs(spec_[1][static_cast<std::size_t>(k)]));
        prefix_[0] = 0.0;
        for (int k = 0; k < kBins; ++k) prefix_[static_cast<std::size_t>(k + 1)] = prefix_[static_cast<std::size_t>(k)] + mag_[static_cast<std::size_t>(k)];
        const float depth = p(Depth), sharp = p(Sharpness) * 0.01f, speed = p(Speed) * 0.01f;
        const double ratio = 0.5 + (0.12 - 0.5) * sharp;          // envelope width as a fraction of frequency
        const double binHz = fs_ / kN;
        const double lo = std::min(p(LowHz), p(HighHz)), hi = std::max(p(LowHz), p(HighHz));
        const double hopSec = kHop / fs_;
        const double tauAtt = 0.060 + (0.005 - 0.060) * speed;     // 60 ms … 5 ms
        const float att = static_cast<float>(std::exp(-hopSec / tauAtt)), rel = static_cast<float>(std::exp(-hopSec / (4.0 * tauAtt)));
        float most = 0.0f;
        for (int k = 0; k < kBins; ++k) {
            float cut = 0.0f;
            const double hz = k * binHz;
            if (k > 0 && hz >= lo && hz <= hi && depth > 0.0f) {
                const int w = std::max(3, static_cast<int>(k * ratio));
                const int a = std::max(0, k - w), b = std::min(kBins - 1, k + w);
                const double env = (prefix_[static_cast<std::size_t>(b + 1)] - prefix_[static_cast<std::size_t>(a)]) / (b - a + 1);
                const double m = mag_[static_cast<std::size_t>(k)];
                if (m > 1e-9 && env > 1e-12) {
                    const double over = 20.0 * std::log10(m / env) - 3.0;
                    cut = static_cast<float>(std::clamp(over, 0.0, static_cast<double>(depth)));
                }
            }
            target_[static_cast<std::size_t>(k)] = -cut;
        }
        for (int k = 0; k < kBins; ++k) {
            const float a = target_[static_cast<std::size_t>(std::max(0, k - 1))], b = target_[static_cast<std::size_t>(k)],
                        c = target_[static_cast<std::size_t>(std::min(kBins - 1, k + 1))];
            const float tgt = std::min(b, 0.25f * a + 0.5f * b + 0.25f * c);
            float& g = gainDb_[static_cast<std::size_t>(k)];
            const float coef = tgt < g ? att : rel;
            g = tgt + coef * (g - tgt);
            if (g > -1e-6f) g = 0.0f;
            most = std::min(most, g);
        }
        publishReduction(most);
        for (int c = 0; c < 2; ++c) {
            auto& s = spec_[c];
            for (int k = 0; k < kBins; ++k) {
                const float g = dbToGain(gainDb_[static_cast<std::size_t>(k)]);
                s[static_cast<std::size_t>(k)] *= g;
                if (k > 0 && k < kN / 2) s[static_cast<std::size_t>(kN - k)] = std::conj(s[static_cast<std::size_t>(k)]);
            }
            fft_.inverse(s.data());
            for (int k = 0; k < kN; ++k) {
                const auto o = static_cast<std::size_t>((t + 1 + static_cast<std::uint64_t>(k)) % (2 * kN));
                out_[c][o] += 0.5f * s[static_cast<std::size_t>(k)].real() * win_[static_cast<std::size_t>(k)];
            }
        }
    }

    double fs_ = 48000;
    Fft fft_;
    std::array<float, kN> win_{};
    std::vector<float> in_[2], out_[2], dry_[2];
    std::vector<std::complex<float>> spec_[2];
    std::vector<float> gainDb_, target_, mag_;
    std::vector<double> prefix_;
    std::uint64_t t_ = 0;
    Smoothed mix_, delta_;
};

} // namespace

std::unique_ptr<Effect> makeCalm() { return std::make_unique<Calm>(); }

} // namespace hc::fx
