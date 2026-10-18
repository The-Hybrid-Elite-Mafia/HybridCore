// HEM CEILING — mastering limiter with a true-peak ceiling. Idea (not code) from modern
// mastering limiters: look ahead, find the peaks *between* samples too (4x oversampled
// detection), and turn the gain down smoothly just before they arrive so nothing passes
// the ceiling and nothing clicks.
//   detector : 4x oversampled |x| of both channels (linked) → gain each sample needs
//   look-ahead: minimum of the needed gain over the next W samples, then a W-sample
//               moving average (smooth fade that is already down when the peak arrives)
//   release  : set time, or AUTO (fast after short peaks, slow when it is working hard)
// Latency: kAlign + W - 1 samples (constant), reported to the host.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

#include <deque>

namespace hc::fx {
namespace {

enum P { Gain, Ceil, Release, Auto, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"gain",    "Gain",       "dB", 0.0f, 24.0f, 0.0f},
    {"ceiling", "Ceiling",    "dB", -12.0f, 0.0f, -1.0f},
    {"release", "Release",    "ms", 1.0f, 1000.0f, 80.0f, Taper::Log},
    {"auto",    "Auto Release", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
};

constexpr std::uint32_t kW = 96;       // look-ahead window: 2 ms at 48 kHz
constexpr std::uint32_t kAlign = 12;   // the 4x detector runs 11.5 samples late: audio waits for it
constexpr std::uint32_t kHoldExtra = 2;   // the hold also covers ±1 sample around the detector's time

// Sliding-window minimum over the last `len` values (monotonic queue in a fixed ring).
struct SlidingMin {
    std::array<float, 512> val{};
    std::array<std::uint32_t, 512> idx{};
    std::uint32_t head = 0, tail = 0, n = 0, len = 1;
    void clear(std::uint32_t l) noexcept { head = tail = n = 0; len = l; }
    float push(float v) noexcept {
        while (tail != head && val[(tail + 511) & 511] >= v) tail = (tail + 511) & 511;
        val[tail] = v; idx[tail] = n; tail = (tail + 1) & 511;
        while (idx[head] + len <= n) head = (head + 1) & 511;
        ++n;
        return val[head];
    }
};

// Peak of the parabola through (−1,a) (0,b) (1,c) when b is a local extreme, else 0.
inline float parabolaPeak(float a, float b, float c) noexcept {
    const float s = b >= 0.0f ? 1.0f : -1.0f;
    a *= s; b *= s; c *= s;
    if (b < a || b < c) return 0.0f;
    const float d = a - 2.0f * b + c;
    if (d > -1e-12f) return b;
    return b - (a - c) * (a - c) / (8.0f * d);
}

class Ceiling final : public Effect {
public:
    Ceiling() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    bool paramActive(int i) const noexcept override { return i != Release || p(Auto) < 0.5f; }
    std::uint32_t latency() const noexcept override { return kAlign + kW - 1; }

    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        for (auto& b : up_) b.assign(static_cast<std::size_t>(std::min(maxBlock, kMaxBlock)) * Oversampler4::kFactor, 0.0f);
        for (auto& d : delay_) d.prepare(latency());
        reset();
    }
    void reset() noexcept override {
        for (auto& o : os_) o.clear();
        for (auto& d : delay_) d.clear();
        hold_.clear(kW + kHoldExtra);
        avg_.fill(1.0f); avgSum_ = static_cast<double>(kW); avgPos_ = 0;
        env_ = 1.0f; slowGr_ = 0.0f;
        h1_[0] = h1_[1] = h2_[0] = h2_[1] = 0.0f;
        gain_.prepare(fs_, 20, dbToGain(p(Gain)));
        ceil_.prepare(fs_, 20, dbToGain(p(Ceil)));
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float gT = dbToGain(p(Gain)), cT = dbToGain(p(Ceil));
        const double relMs = p(Release);
        const bool autoRel = p(Auto) >= 0.5f;
        const float relSet = onePoleCoef(fs_, relMs), relFast = onePoleCoef(fs_, relMs * 0.25), relSlow = onePoleCoef(fs_, relMs * 2.5);
        const float slowCoef = onePoleCoef(fs_, 800.0);
        // Input gain, then the 4x detector on both channels.
        Smoothed g = gain_;
        for (std::uint32_t i = 0; i < n; ++i) { const float k = g.next(gT); L[i] *= k; R[i] *= k; }
        gain_ = g;
        os_[0].up(L, up_[0].data(), n);
        os_[1].up(R, up_[1].data(), n);
        float minGain = 1.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            // The 4x points include the plain sample positions (phase 0); between two 4x points
            // the peak is estimated with a parabola through three neighbours.
            float pk = 0.0f;
            for (std::uint32_t k = 0; k < Oversampler4::kFactor; ++k)
                for (int c = 0; c < 2; ++c) {
                    const float x = up_[c][i * 4 + k];
                    pk = std::max(pk, std::max(std::fabs(x), parabolaPeak(h2_[c], h1_[c], x)));
                    h2_[c] = h1_[c]; h1_[c] = x;
                }
            const float ce = ceil_.next(cT);
            const float need = pk > ce ? ce / pk : 1.0f;
            // Look-ahead: hold the smallest need of the window, then fade over W samples.
            const float h = hold_.push(need);
            avgSum_ += static_cast<double>(h) - static_cast<double>(avg_[avgPos_]);
            avg_[avgPos_] = h;
            avgPos_ = (avgPos_ + 1) % kW;
            const float a = static_cast<float>(avgSum_ / kW);
            // Release: never above the fade; recover with the set (or automatic) time.
            if (a < env_) env_ = a;
            else {
                const float c = !autoRel ? relSet : (slowGr_ > 0.12f ? relSlow : relFast);
                env_ = a + c * (env_ - a);
            }
            slowGr_ = (1.0f - env_) + slowCoef * (slowGr_ - (1.0f - env_));
            minGain = std::min(minGain, env_);
            L[i] = delay_[0].process(L[i], latency()) * env_;
            R[i] = delay_[1].process(R[i], latency()) * env_;
            // Safety: the plain sample never passes the ceiling (inter-sample overs are handled
            // by the look-ahead above; this only catches rounding at the very edge).
            L[i] = std::clamp(L[i], -ce, ce);
            R[i] = std::clamp(R[i], -ce, ce);
        }
        avgSum_ = 0.0;   // re-sum to stop floating-point drift
        for (float v : avg_) avgSum_ += static_cast<double>(v);
        publishReduction(gainToDb(minGain));
    }

private:
    double fs_ = 48000;
    Oversampler4 os_[2] = {Oversampler4(1.0), Oversampler4(1.0)};   // flat to 20 kHz: sees every real peak
    std::vector<float> up_[2];
    DelayLine delay_[2];
    SlidingMin hold_;
    std::array<float, kW> avg_{};
    double avgSum_ = kW;
    std::uint32_t avgPos_ = 0;
    float env_ = 1.0f, slowGr_ = 0.0f;
    float h1_[2]{}, h2_[2]{};   // the last two 4x points per channel
    Smoothed gain_, ceil_;
};

} // namespace

std::unique_ptr<Effect> makeCeiling() { return std::make_unique<Ceiling>(); }

} // namespace hc::fx
