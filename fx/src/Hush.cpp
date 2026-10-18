// HEM HUSH — hiss and noise remover that learns the noise floor by itself.
//   STFT: 2048 points (4096 above 64 kHz), hop N/4, square-root Hann in and out; frames timed by
//   an internal counter (block-size independent); latency = N, the dry is never mixed in, so
//   nothing combs.
//   Noise floor per frequency: the median of 13 neighbouring bins (a held note is never mistaken
//   for the floor), then the lowest of that over the last ~4 s (8 slots of 0.5 s), × 1.6.
//   Gain per bin (Wiener-style): keep what stands above (1 + 3 × AMOUNT) × the floor, never
//   below -REDUCTION; smoothed over neighbouring bins (no "musical noise" twinkles) and in time
//   (falls at once, recovers over a few frames). Only above FROM. LISTEN plays what is removed.
//   Ported from the Rework Playground's Elite Hush and improved: TONE (keep more air or more
//   body), and the floor freezes while LEARN is off.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/fx/Fft.hpp"

#include <complex>

namespace hc::fx {
namespace {

enum P { Amount, Reduction, From, Tone, Learn, Listen, Output, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"amount",    "Amount",    "%", 0.0f, 100.0f, 50.0f},
    {"reduction", "Reduction", "dB", 3.0f, 30.0f, 12.0f},
    {"from",      "From",      "Hz", 20.0f, 8000.0f, 20.0f, Taper::Log},
    {"tone",      "Tone",      "%", -100.0f, 100.0f, 0.0f},
    {"learn",     "Learn",     "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"listen",    "Listen",    "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output",    "Output",    "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"NOISE FLOOR", "dBFS", -120.0f, 0.0f}, {"REMOVED", "dB", -24.0f, 0.0f}};
constexpr int kSlots = 8;
constexpr float kBias = 1.6f, kEps = 1e-14f;

class Hush final : public Effect {
public:
    Hush() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return static_cast<std::uint32_t>(n_); }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        n_ = fs > 64000.0 ? 4096 : 2048;
        hop_ = n_ / 4;
        fft_.prepare(n_);
        win_.resize(static_cast<std::size_t>(n_));
        for (int i = 0; i < n_; ++i) win_[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n_)));
        const auto N = static_cast<std::size_t>(n_), B = N / 2 + 1;
        for (int c = 0; c < 2; ++c) { in_[c].assign(N, 0.0f); out_[c].assign(N, 0.0f); buf_[c].assign(N, {}); }
        power_.assign(B, 0.0f); smooth_.assign(B, 0.0f); noise_.assign(B, 0.0f);
        gain_.assign(B, 1.0f); gainTmp_.assign(B, 1.0f);
        slotMin_.assign(B * kSlots, 0.0f); curMin_.assign(B, 0.0f);
        framesPerSlot_ = std::max(1, static_cast<int>(std::lround(0.5 * fs / hop_)));
        reset();
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { std::fill(in_[c].begin(), in_[c].end(), 0.0f); std::fill(out_[c].begin(), out_[c].end(), 0.0f); }
        for (auto* v : {&power_, &smooth_, &noise_, &slotMin_, &curMin_}) std::fill(v->begin(), v->end(), 0.0f);
        std::fill(gain_.begin(), gain_.end(), 1.0f);
        pos_ = hopCount_ = slotFrame_ = slot_ = slotsFilled_ = 0;
        removedAvg_ = 0.0f;
        outGain_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, -120.0f); publishMeter(1, 0.0f);
        publishReduction(0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const float oT = dbToGain(p(Output));
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto ps = static_cast<std::size_t>(pos_);
            in_[0][ps] = L[i]; in_[1][ps] = R[i];
            const float g = outGain_.next(oT);
            L[i] = out_[0][ps] * g; R[i] = out_[1][ps] * g;
            out_[0][ps] = out_[1][ps] = 0.0f;
            if (++pos_ >= n_) pos_ = 0;
            if (++hopCount_ >= hop_) { hopCount_ = 0; frame(); }
        }
    }

private:
    void frame() noexcept {
        const int N = n_;
        const auto B = static_cast<std::size_t>(N / 2 + 1);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                buf_[c][static_cast<std::size_t>(i)] = {in_[c][static_cast<std::size_t>((pos_ + i) % N)] * win_[static_cast<std::size_t>(i)], 0.0f};
        fft_.forward(buf_[0].data());
        fft_.forward(buf_[1].data());

        const float amount = p(Amount) * 0.01f, floorGain = dbToGain(-p(Reduction));
        const bool listen = p(Listen) >= 0.5f, learn = p(Learn) >= 0.5f;
        const int fromBin = static_cast<int>(std::floor(p(From) * N / fs_));
        const float over = 1.0f + 3.0f * amount;
        const float tone = p(Tone) * 0.01f;   // + keeps more air (eases the highs), - keeps more body
        for (std::size_t k = 0; k < B; ++k) {
            const float pw = 0.5f * (std::norm(buf_[0][k]) + std::norm(buf_[1][k]));
            power_[k] = 0.6f * power_[k] + 0.4f * pw;
            if (power_[k] < 1e-30f) power_[k] = 0.0f;
        }
        if (learn) {
            for (std::size_t k = 0; k < B; ++k) {
                std::array<float, 13> v{};
                for (int j = -6; j <= 6; ++j)
                    v[static_cast<std::size_t>(j + 6)] = power_[static_cast<std::size_t>(std::clamp(static_cast<int>(k) + j, 0, static_cast<int>(B) - 1))];
                std::nth_element(v.begin(), v.begin() + 6, v.end());
                smooth_[k] = v[6];
            }
            const bool start = slotFrame_ == 0;
            for (std::size_t k = 0; k < B; ++k) {
                curMin_[k] = start ? smooth_[k] : std::min(curMin_[k], smooth_[k]);
                float m = curMin_[k];
                for (int s = 0; s < std::min(slotsFilled_, kSlots); ++s) m = std::min(m, slotMin_[k * kSlots + static_cast<std::size_t>(s)]);
                noise_[k] = m * kBias;
            }
            if (++slotFrame_ >= framesPerSlot_) {
                for (std::size_t k = 0; k < B; ++k) slotMin_[k * kSlots + static_cast<std::size_t>(slot_)] = curMin_[k];
                slot_ = (slot_ + 1) % kSlots;
                slotsFilled_ = std::min(kSlots, slotsFilled_ + 1);
                slotFrame_ = 0;
            }
        }
        float noiseSum = 0.0f;
        for (std::size_t k = 0; k < B; ++k) {
            float g = 1.0f;
            if (static_cast<int>(k) >= fromBin && power_[k] > kEps) {
                const float pos = static_cast<float>(k) / static_cast<float>(B);   // 0 .. 1 across the spectrum
                const float bias = std::exp2(-tone * (pos - 0.3f) * 2.0f);        // tone: scale the threshold by up to ×4 / ÷4
                g = std::clamp(1.0f - over * bias * noise_[k] / power_[k], floorGain, 1.0f);
            }
            gainTmp_[k] = g;
            noiseSum += noise_[k] / kBias;
        }
        float removed = 0.0f, total = 0.0f;
        for (std::size_t k = 0; k < B; ++k) {
            const float a = gainTmp_[k > 0 ? k - 1 : k], b = gainTmp_[k], c = gainTmp_[k + 1 < B ? k + 1 : k];
            const float t = 0.25f * a + 0.5f * b + 0.25f * c;
            gain_[k] = t < gain_[k] ? t : gain_[k] + 0.35f * (t - gain_[k]);
            const float g = listen ? 1.0f - gain_[k] : gain_[k];
            removed += power_[k] * (1.0f - gain_[k] * gain_[k]);
            total += power_[k];
            buf_[0][k] *= g; buf_[1][k] *= g;
            if (k > 0 && k < static_cast<std::size_t>(N / 2)) {
                buf_[0][static_cast<std::size_t>(N) - k] = std::conj(buf_[0][k]);
                buf_[1][static_cast<std::size_t>(N) - k] = std::conj(buf_[1][k]);
            }
        }
        fft_.inverse(buf_[0].data());
        fft_.inverse(buf_[1].data());
        const float scale = 0.5f;   // inverse already divides by N; 4x sqrt-Hann overlap sums to 2
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                out_[c][static_cast<std::size_t>((pos_ + i) % N)] += buf_[c][static_cast<std::size_t>(i)].real() * win_[static_cast<std::size_t>(i)] * scale;
        const float rem = total > kEps ? 10.0f * std::log10(std::max(1e-6f, 1.0f - removed / total)) : 0.0f;
        removedAvg_ = 0.8f * removedAvg_ + 0.2f * rem;
        publishMeter(1, removedAvg_);
        publishReduction(removedAvg_);
        const float norm = 4.0f / static_cast<float>(N);
        publishMeter(0, noiseSum > kEps ? 10.0f * std::log10(noiseSum * norm * norm) : -120.0f);
    }

    double fs_ = 48000;
    int n_ = 2048, hop_ = 512, pos_ = 0, hopCount_ = 0;
    Fft fft_;
    std::vector<float> win_, in_[2], out_[2];
    std::vector<std::complex<float>> buf_[2];
    std::vector<float> power_, smooth_, noise_, gain_, gainTmp_, slotMin_, curMin_;
    int framesPerSlot_ = 47, slotFrame_ = 0, slot_ = 0, slotsFilled_ = 0;
    float removedAvg_ = 0;
    Smoothed outGain_;
};

} // namespace

std::unique_ptr<Effect> makeHush() { return std::make_unique<Hush>(); }

} // namespace hc::fx
