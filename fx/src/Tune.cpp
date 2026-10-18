// HEM TUNE — vocal pitch correction for one voice at a time (lead vocal, a solo instrument).
//   Listen: L+R averaged down to ~24 kHz; every ~5 ms YIN (de Cheveigné & Kawahara 2002) on
//   the last 20 ms finds the period between LOWEST and HIGHEST note (cumulative-mean
//   normalised difference, threshold 0.15, parabolic refinement). Quieter than about -60 dBFS,
//   or no clear period (breath, consonant, noise): nothing is corrected.
//   Target: nearest note of KEY / SCALE (chromatic, major, minor). STICKY notes: the target
//   only changes when the voice is clearly past the middle between two notes (15 cents of
//   hysteresis), so a singer sitting between two notes does not flip-flop.
//   Correct: AMOUNT of the distance, at most ±300 cents, glided over RETUNE SPEED (0 = hard snap).
//   Shifter: a 30 ms delay line read by two heads crossfaded with sin² (they always sum to 1);
//   at rest one head sits in the middle, so the plug-in's latency is a constant 15 ms and
//   with nothing to correct the output is the input, 15 ms late.
//   Ported from the Rework Playground's Elite Tune and improved: sticky notes.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Key, Scale, Speed, Amount, Low, High, Output, kCount };
const char* const kKeys[]   = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kScales[] = {"CHROMATIC", "MAJOR", "MINOR"};
const ParamInfo kParams[kCount] = {
    {"key",    "Key",          "", 0.0f, 11.0f, 0.0f, Taper::Choice, kKeys},
    {"scale",  "Scale",        "", 0.0f, 2.0f, 0.0f, Taper::Choice, kScales},
    {"speed",  "Retune Speed", "ms", 0.0f, 400.0f, 25.0f},
    {"amount", "Amount",       "%", 0.0f, 100.0f, 100.0f},
    {"low",    "Lowest Note",  "Hz", 50.0f, 400.0f, 80.0f, Taper::Log},
    {"high",   "Highest Note", "Hz", 200.0f, 1500.0f, 1000.0f, Taper::Log},
    {"output", "Output",       "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"PITCH", "Hz", 50.0f, 1500.0f}, {"NOTE", "note", 36.0f, 84.0f}, {"CORRECTION", "cents", -100.0f, 100.0f, true}};
constexpr float kYinThreshold = 0.15f, kMaxCents = 300.0f, kSticky = 0.15f;

} // namespace

// Nearest allowed MIDI note to `midi` for key (0 = C) and scale (0 chromatic, 1 major, 2 minor).
float tuneSnap(float midi, int key, int scale) noexcept {
    static constexpr bool kMajor[12] = {true, false, true, false, true, true, false, true, false, true, false, true};
    static constexpr bool kMinor[12] = {true, false, true, true, false, true, false, true, true, false, true, false};
    if (scale <= 0) return std::round(midi);
    const bool* ok = scale == 1 ? kMajor : kMinor;
    const int centre = static_cast<int>(std::lround(midi));
    float best = static_cast<float>(centre), bestD = 1e9f;
    for (int nn = centre - 2; nn <= centre + 2; ++nn) {
        if (!ok[((nn - key) % 12 + 12) % 12]) continue;
        const float d = std::fabs(static_cast<float>(nn) - midi);
        if (d < bestD) { bestD = d; best = static_cast<float>(nn); }
    }
    return best;
}

namespace {

class Tune final : public Effect {
public:
    Tune() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return static_cast<std::uint32_t>(win_ / 2); }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        decim_ = std::max(1, static_cast<int>(std::lround(fs / 24000.0)));
        anaRate_ = fs / decim_;
        anaWin_ = static_cast<int>(std::lround(0.02 * anaRate_));
        tauMax_ = static_cast<int>(std::ceil(anaRate_ / 50.0)) + 2;
        hop_ = std::max(32, static_cast<int>(std::lround(0.005 * anaRate_)));
        ana_.assign(static_cast<std::size_t>(anaWin_ + tauMax_), 0.0f);
        frame_.assign(ana_.size(), 0.0f);
        yin_.assign(static_cast<std::size_t>(tauMax_) + 1, 0.0f);
        win_ = static_cast<int>(std::lround(0.03 * fs));
        line_[0].assign(static_cast<std::size_t>(win_) + 4, 0.0f);
        line_[1].assign(static_cast<std::size_t>(win_) + 4, 0.0f);
        reset();
    }
    void reset() noexcept override {
        std::fill(ana_.begin(), ana_.end(), 0.0f);
        for (auto& l : line_) std::fill(l.begin(), l.end(), 0.0f);
        anaPos_ = hopCount_ = decimCount_ = writePos_ = 0;
        decimAcc_ = 0.0f;
        phase_ = 0.5;
        ratio_ = targetRatio_ = 1.0f;
        lastHz_ = lastCents_ = 0.0f;
        lastTarget_ = -1.0f;
        out_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, 0.0f); publishMeter(1, 0.0f); publishMeter(2, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const int key = std::clamp(static_cast<int>(p(Key)), 0, 11), scale = std::clamp(static_cast<int>(p(Scale)), 0, 2);
        const float speed = p(Speed), amount = p(Amount) * 0.01f, oT = dbToGain(p(Output));
        const float glide = speed < 0.5f ? 0.0f : onePoleCoef(fs_, speed);
        const double W = win_, maxNudge = 0.006 / W;
        const int lineSize = static_cast<int>(line_[0].size()), anaSize = static_cast<int>(ana_.size());
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            decimAcc_ += 0.5f * (l + r);
            if (++decimCount_ >= decim_) {
                ana_[static_cast<std::size_t>(anaPos_)] = decimAcc_ / static_cast<float>(decim_);
                if (++anaPos_ >= anaSize) anaPos_ = 0;
                decimAcc_ = 0.0f; decimCount_ = 0;
                if (++hopCount_ >= hop_) { hopCount_ = 0; listen(key, scale, amount); }
            }
            ratio_ = targetRatio_ + glide * (ratio_ - targetRatio_);
            line_[0][static_cast<std::size_t>(writePos_)] = l;
            line_[1][static_cast<std::size_t>(writePos_)] = r;
            phase_ += (1.0 - static_cast<double>(ratio_)) / W;
            if (std::fabs(ratio_ - 1.0f) < 1e-4f && std::fabs(targetRatio_ - 1.0f) < 1e-4f)
                phase_ += std::clamp(0.5 - phase_, -maxNudge, maxNudge);   // nothing to correct: settle on one clean head
            phase_ -= std::floor(phase_);
            const double phB = phase_ + (phase_ >= 0.5 ? -0.5 : 0.5);
            const float sA = static_cast<float>(std::sin(std::numbers::pi * phase_));
            const float gA = sA * sA, gB = 1.0f - gA;
            const float o = out_.next(oT);
            L[i] = (gA * read(line_[0], phase_ * W) + gB * read(line_[0], phB * W)) * o;
            R[i] = (gA * read(line_[1], phase_ * W) + gB * read(line_[1], phB * W)) * o;
            if (++writePos_ >= lineSize) writePos_ = 0;
        }
        publishMeter(0, lastHz_);
        publishMeter(1, lastTarget_ < 0 ? 0.0f : lastTarget_);
        publishMeter(2, lastCents_);
    }

private:
    [[nodiscard]] float read(const std::vector<float>& line, double delay) const noexcept {
        const int size = static_cast<int>(line.size());
        double rp = writePos_ - delay;
        while (rp < 0) rp += size;
        int i0 = static_cast<int>(rp);
        if (i0 >= size) i0 -= size;
        const float f = static_cast<float>(rp - std::floor(rp));
        const int i1 = i0 + 1 >= size ? 0 : i0 + 1;
        return line[static_cast<std::size_t>(i0)] + f * (line[static_cast<std::size_t>(i1)] - line[static_cast<std::size_t>(i0)]);
    }

    void listen(int key, int scale, float amount) noexcept {
        const float hz = detect();
        lastHz_ = hz;
        if (hz <= 0.0f) { targetRatio_ = 1.0f; lastCents_ = 0.0f; lastTarget_ = -1.0f; return; }
        const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
        float target = tuneSnap(midi, key, scale);
        // sticky: keep the previous note while the voice has not clearly crossed the midpoint
        if (lastTarget_ >= 0.0f && target != lastTarget_ && std::fabs(midi - lastTarget_) < std::fabs(target - lastTarget_) * 0.5f + kSticky)
            target = lastTarget_;
        const float cents = std::clamp((target - midi) * 100.0f * amount, -kMaxCents, kMaxCents);
        targetRatio_ = std::exp2(cents / 1200.0f);
        lastCents_ = cents;
        lastTarget_ = target;
    }

    float detect() noexcept {
        const int size = static_cast<int>(ana_.size());
        for (int i = 0; i < size; ++i) frame_[static_cast<std::size_t>(i)] = ana_[static_cast<std::size_t>((anaPos_ + i) % size)];
        const float lowHz = p(Low), highHz = std::max(p(High), lowHz * 1.5f);
        const int tauMin = std::max(2, static_cast<int>(std::floor(anaRate_ / highHz)));
        const int tauMax = std::min(tauMax_ - 1, static_cast<int>(std::ceil(anaRate_ / lowHz)));
        const int Wn = anaWin_;
        const float* x = frame_.data() + (size - Wn - tauMax);
        float energy = 0.0f;
        for (int j = 0; j < Wn; ++j) energy += x[j] * x[j];
        if (energy < static_cast<float>(Wn) * 1e-6f) return 0.0f;
        yin_[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= tauMax; ++tau) {
            float d = 0.0f;
            for (int j = 0; j < Wn; ++j) { const float df = x[j] - x[j + tau]; d += df * df; }
            running += d;
            yin_[static_cast<std::size_t>(tau)] = running > 0.0f ? d * static_cast<float>(tau) / running : 1.0f;
        }
        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau) {
            if (yin_[static_cast<std::size_t>(tau)] < kYinThreshold) {
                while (tau + 1 < tauMax && yin_[static_cast<std::size_t>(tau) + 1] < yin_[static_cast<std::size_t>(tau)]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0) return 0.0f;
        const float a = yin_[static_cast<std::size_t>(best) - 1], b = yin_[static_cast<std::size_t>(best)], c = yin_[static_cast<std::size_t>(best) + 1];
        const float den = a - 2.0f * b + c;
        const float shift = std::fabs(den) > 1e-9f ? std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
        return static_cast<float>(anaRate_ / (best + shift));
    }

    double fs_ = 48000, anaRate_ = 24000;
    int decim_ = 2, decimCount_ = 0, anaWin_ = 480, tauMax_ = 482, hop_ = 120, anaPos_ = 0, hopCount_ = 0;
    float decimAcc_ = 0;
    std::vector<float> ana_, frame_, yin_, line_[2];
    int win_ = 1440, writePos_ = 0;
    double phase_ = 0.5;
    float ratio_ = 1, targetRatio_ = 1, lastHz_ = 0, lastCents_ = 0, lastTarget_ = -1;
    Smoothed out_;
};

} // namespace

std::unique_ptr<Effect> makeTune() { return std::make_unique<Tune>(); }

} // namespace hc::fx
