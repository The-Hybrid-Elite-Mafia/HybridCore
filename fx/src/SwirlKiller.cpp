// HEM SWIRL KILLER — removes AI / codec "swirl" (watery shimmer in the stereo sides) and keeps
// the centre untouched.
//   STFT: 2048 points (4096 above 64 kHz), hop N/4, square-root Hann; latency = N.
//   Per bin, Mid / Side coherence over ~130 ms: real panning and width move WITH the centre
//   (coherence near 1); swirl is Side with nothing steady to do with the Mid (near 0). Side
//   flicker against its 5-frame median adds to the score. Bins that are mostly Side on purpose
//   (wider than PROTECT WIDTH allows) are left alone; only above FROM.
//   REMOVE mode turns the erratic Side down; REPAIR keeps the part of the Side that follows the
//   Mid (stable panning) and cuts only the erratic rest. Gains are smoothed over 3 bins and in
//   time (fast down, slower up), never below -26 dB.
//   WHISTLE HUNTER: a Side bin >= 9 dB above its neighbours for a long time (a steady whine,
//   e.g. 12.2 kHz) gets a Side-only notch. TRANSIENT SHIELD: on a sudden jump of the highs
//   (snare, crash) the cleaning backs off for the frames containing the attack.
//   LISTEN plays only what is removed. Ported from the Rework Playground's Elite Swirl Killer.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/fx/Fft.hpp"

#include <complex>

namespace hc::fx {
namespace {

enum P { Amount, From, Protect, Mode, Whistle, Shield, Listen, Output, kCount };
const char* const kModes[] = {"REMOVE", "REPAIR"};
const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kParams[kCount] = {
    {"amount",  "Amount",           "%", 0.0f, 100.0f, 50.0f},
    {"from",    "From",             "Hz", 1000.0f, 16000.0f, 4000.0f, Taper::Log},
    {"protect", "Protect Width",    "%", 0.0f, 100.0f, 40.0f},
    {"mode",    "Mode",             "", 0.0f, 1.0f, 1.0f, Taper::Choice, kModes},
    {"whistle", "Whistle Hunter",   "%", 0.0f, 100.0f, 0.0f},
    {"shield",  "Transient Shield", "%", 0.0f, 100.0f, 60.0f},
    {"listen",  "Listen",           "", 0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"output",  "Output",           "dB", -24.0f, 12.0f, 0.0f},
};
const MeterInfo kMeters[] = {{"COHERENCE", "corr", -1.0f, 1.0f, true}, {"SWIRL", "%", 0.0f, 100.0f},
                             {"SIDE CUT", "dB", -24.0f, 0.0f}, {"WHISTLE", "Hz", 0.0f, 20000.0f}};
constexpr float kEps = 1e-12f, kFloorGain = 0.05f;

inline float median5(const std::array<float, 5>& v) noexcept {
    auto a = v;
    std::sort(a.begin(), a.end());
    return a[2];
}
inline float smoothstep(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class SwirlKiller final : public Effect {
public:
    SwirlKiller() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return static_cast<std::uint32_t>(n_); }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        n_ = fs > 64000.0 ? 4096 : 2048;
        hop_ = n_ / 4;
        fft_.prepare(n_);
        const auto N = static_cast<std::size_t>(n_), B = N / 2 + 1;
        win_.resize(N);
        for (int i = 0; i < n_; ++i) win_[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n_)));
        for (int c = 0; c < 2; ++c) { in_[c].assign(N, 0.0f); out_[c].assign(N, 0.0f); buf_[c].assign(N, {}); }
        sPow_.assign(B, 0.0f); mPow_.assign(B, 0.0f); cross_.assign(B, {});
        gain_.assign(B, 1.0f); gainTmp_.assign(B, 1.0f);
        sideSlow_.assign(B, 0.0f); whistleGain_.assign(B, 1.0f); whistleTmp_.assign(B, 1.0f);
        hist_.assign(B, {});
        reset();
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { std::fill(in_[c].begin(), in_[c].end(), 0.0f); std::fill(out_[c].begin(), out_[c].end(), 0.0f); }
        std::fill(sPow_.begin(), sPow_.end(), 0.0f);
        std::fill(mPow_.begin(), mPow_.end(), 0.0f);
        std::fill(cross_.begin(), cross_.end(), std::complex<float>());
        std::fill(gain_.begin(), gain_.end(), 1.0f);
        std::fill(sideSlow_.begin(), sideSlow_.end(), 0.0f);
        std::fill(whistleGain_.begin(), whistleGain_.end(), 1.0f);
        for (auto& h : hist_) h.fill(0.0f);
        pos_ = hopCount_ = histPos_ = shieldHold_ = 0;
        hfAvg_ = 0.0f; corrAvg_ = 1.0f;
        outGain_.prepare(fs_, 30, dbToGain(p(Output)));
        publishMeter(0, 1.0f); publishMeter(1, 0.0f); publishMeter(2, 0.0f); publishMeter(3, 0.0f);
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
        const int N = n_, half = N / 2;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                buf_[c][static_cast<std::size_t>(i)] = {in_[c][static_cast<std::size_t>((pos_ + i) % N)] * win_[static_cast<std::size_t>(i)], 0.0f};
        fft_.forward(buf_[0].data());
        fft_.forward(buf_[1].data());

        const float amount = p(Amount) * 0.01f, fromHz = p(From), protect = p(Protect) * 0.01f;
        const bool repair = p(Mode) >= 0.5f, listen = p(Listen) >= 0.5f;
        const float whistleAmt = p(Whistle) * 0.01f, shield = p(Shield) * 0.01f;
        const float binHz = static_cast<float>(fs_) / static_cast<float>(N);
        const float maxSide = 1.0f - 0.7f * protect;
        constexpr float panRate = 0.08f;
        const float frameMs = 1000.0f * static_cast<float>(hop_) / static_cast<float>(fs_);

        // transient shield: high-frequency energy of the newest hop (first differences)
        {
            double hf = 0.0;
            for (int i = N - hop_; i < N; ++i) {
                const int a = (pos_ + i) % N, b = (pos_ + i - 1 + N) % N;
                const float dl = in_[0][static_cast<std::size_t>(a)] - in_[0][static_cast<std::size_t>(b)];
                const float dr = in_[1][static_cast<std::size_t>(a)] - in_[1][static_cast<std::size_t>(b)];
                hf += static_cast<double>(dl) * dl + static_cast<double>(dr) * dr;
            }
            const auto hfF = static_cast<float>(hf);
            if (hfF > 4.0f * hfAvg_ + 1e-7f) shieldHold_ = N / hop_ + std::max(1, static_cast<int>(std::ceil(35.0f / frameMs)));
            else if (shieldHold_ > 0) --shieldHold_;
            hfAvg_ += 0.1f * (hfF - hfAvg_);
            if (hfAvg_ < 1e-30f) hfAvg_ = 0.0f;
        }
        const float amountNow = shieldHold_ > 0 ? amount * (1.0f - shield) : amount;
        double cNum = 0, cL = 0, cR = 0, sideIn = 0, sideOut = 0, sideHi = 0, swirlHi = 0;
        for (int k = 1; k < half; ++k) {
            const auto ks = static_cast<std::size_t>(k);
            const auto Lc = buf_[0][ks], Rc = buf_[1][ks];
            const auto M = 0.5f * (Lc + Rc), S = 0.5f * (Lc - Rc);
            const float pM = std::norm(M), pS = std::norm(S), f = static_cast<float>(k) * binHz;
            auto& sc = cross_[ks];
            sc += panRate * (S * std::conj(M) - sc);
            mPow_[ks] += panRate * (pM - mPow_[ks]);
            sPow_[ks] += panRate * (pS - sPow_[ks]);
            if (std::norm(sc) < 1e-30f) sc = {};
            if (mPow_[ks] < 1e-30f) mPow_[ks] = 0.0f;
            if (sPow_[ks] < 1e-30f) sPow_[ks] = 0.0f;
            const float coh = std::clamp(std::norm(sc) / (mPow_[ks] * sPow_[ks] + 1e-20f), 0.0f, 1.0f);
            auto& h = hist_[ks];
            const float sMag = std::sqrt(pS);
            h[static_cast<std::size_t>(histPos_)] = sMag;
            const float flicker = std::clamp(sMag / (median5(h) + 1e-9f) - 1.0f, 0.0f, 1.0f);
            const float w = smoothstep(fromHz * 0.7f, fromHz, f);
            const bool audible = pM + pS > 1e-10f;
            const float sideRatio = pS / (pM + pS + kEps);
            const float sideW = 1.0f - smoothstep(maxSide, std::min(1.0f, maxSide + 0.15f), sideRatio);
            float score = audible ? (1.0f - coh) * (0.75f + 0.25f * flicker) * w * sideW : 0.0f;
            score = std::clamp((score - 0.1f) / 0.9f, 0.0f, 1.0f);
            gainTmp_[ks] = std::max(kFloorGain, 1.0f - amountNow * score);
            sideSlow_[ks] += 0.03f * (sMag - sideSlow_[ks]);
            if (sideSlow_[ks] < 1e-20f) sideSlow_[ks] = 0.0f;
            whistleTmp_[ks] = 1.0f;
            if (f >= fromHz) {
                const auto cr = Lc * std::conj(Rc);
                cNum += cr.real(); cL += std::norm(Lc); cR += std::norm(Rc);
            }
            if (audible) { sideHi += static_cast<double>(pS * w); swirlHi += static_cast<double>(pS * w * score); }
        }
        histPos_ = (histPos_ + 1) % 5;
        if (cL > 1e-12 && cR > 1e-12) corrAvg_ += 0.15f * (static_cast<float>(cNum / std::sqrt(cL * cR)) - corrAvg_);

        float loudest = 0.0f, whistleHz = 0.0f;
        if (whistleAmt > 0.0f) {
            const int k0 = std::max(8, static_cast<int>(std::min(fromHz, 6000.0f) / binHz));
            const float notch = 1.0f - whistleAmt * (1.0f - 0.063f);
            for (int k = k0; k < half - 8; ++k) {
                const float v = sideSlow_[static_cast<std::size_t>(k)];
                if (v < 1e-5f || v < sideSlow_[static_cast<std::size_t>(k - 1)] || v < sideSlow_[static_cast<std::size_t>(k + 1)]) continue;
                float nb = 0.0f;
                for (int j = 3; j <= 7; ++j) nb += sideSlow_[static_cast<std::size_t>(k - j)] + sideSlow_[static_cast<std::size_t>(k + j)];
                nb *= 0.1f;
                if (v > 2.8f * (nb + 1e-9f)) {
                    for (int j = -1; j <= 1; ++j) whistleTmp_[static_cast<std::size_t>(k + j)] = std::min(whistleTmp_[static_cast<std::size_t>(k + j)], notch);
                    if (v > loudest) { loudest = v; whistleHz = static_cast<float>(k) * binHz; }
                }
            }
        }
        for (int k = 1; k < half; ++k) {
            const auto ks = static_cast<std::size_t>(k);
            const float t = (gainTmp_[ks - 1] + 2.0f * gainTmp_[ks] + gainTmp_[static_cast<std::size_t>(std::min(k + 1, half - 1))]) * 0.25f;
            float& g = gain_[ks];
            g += (shieldHold_ > 0 ? 0.75f : (t < g ? 0.6f : 0.15f)) * (t - g);
            const auto Lc = buf_[0][ks], Rc = buf_[1][ks];
            const auto M = 0.5f * (Lc + Rc), S = 0.5f * (Lc - Rc);
            std::complex<float> S2;
            if (repair) {
                const auto coef = cross_[ks] / (mPow_[ks] + 1e-9f);
                auto steady = coef * M;
                if (std::norm(steady) > std::norm(S)) steady *= std::sqrt(std::norm(S) / (std::norm(steady) + kEps));
                S2 = steady + g * (S - steady);
            } else {
                S2 = g * S;
            }
            float& wg = whistleGain_[ks];
            wg += (whistleTmp_[ks] < wg ? 0.5f : 0.1f) * (whistleTmp_[ks] - wg);
            S2 *= wg;
            if (static_cast<float>(k) * binHz >= fromHz) { sideIn += std::norm(S); sideOut += std::norm(S2); }
            auto nl = M + S2, nr = M - S2;
            if (listen) { nl = Lc - nl; nr = Rc - nr; }
            buf_[0][ks] = nl; buf_[1][ks] = nr;
            buf_[0][static_cast<std::size_t>(N - k)] = std::conj(nl);
            buf_[1][static_cast<std::size_t>(N - k)] = std::conj(nr);
        }
        if (listen) { buf_[0][0] = buf_[1][0] = {}; buf_[0][static_cast<std::size_t>(half)] = buf_[1][static_cast<std::size_t>(half)] = {}; }
        fft_.inverse(buf_[0].data());
        fft_.inverse(buf_[1].data());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                out_[c][static_cast<std::size_t>((pos_ + i) % N)] += buf_[c][static_cast<std::size_t>(i)].real() * win_[static_cast<std::size_t>(i)] * 0.5f;
        const float cut = sideIn > 1e-12 ? static_cast<float>(10.0 * std::log10(std::max(1e-12, sideOut) / sideIn)) : 0.0f;
        publishMeter(0, corrAvg_);
        publishMeter(1, sideHi > 1e-9 ? static_cast<float>(100.0 * swirlHi / sideHi) : 0.0f);
        publishMeter(2, cut);
        publishMeter(3, whistleHz);
        publishReduction(cut);
    }

    double fs_ = 48000;
    int n_ = 2048, hop_ = 512, pos_ = 0, hopCount_ = 0, histPos_ = 0, shieldHold_ = 0;
    Fft fft_;
    std::vector<float> win_, in_[2], out_[2];
    std::vector<std::complex<float>> buf_[2], cross_;
    std::vector<float> sPow_, mPow_, gain_, gainTmp_, sideSlow_, whistleGain_, whistleTmp_;
    std::vector<std::array<float, 5>> hist_;
    float hfAvg_ = 0, corrAvg_ = 1;
    Smoothed outGain_;
};

} // namespace

std::unique_ptr<Effect> makeSwirlKiller() { return std::make_unique<SwirlKiller>(); }

} // namespace hc::fx
