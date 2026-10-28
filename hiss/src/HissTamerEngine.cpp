// HEM HISS TAMER ENGINE — see the header for the plain-words description.
//
//  Frame loop (every hop = N/4 samples):
//    1. window the last N samples of L and R, one packed complex FFT (L + iR), split into XL, XR
//    2. detector(s): Stereo = one linked detector on (|XL|^2 + |XR|^2) / 2; Mid/Side = one each on M and S
//         S  = a * S + (1 - a) * P, a = exp(-hop / 40 ms)   (smoothed power per bin)
//         Fm = median of 13 neighbouring S            (kills lone peaks)
//         A  = estimated power of the steady artifact (method: Floor / Percentile / Tonal)
//         Floor / Percentile: rho = S / A. rho below the ramp start = hiss only -> full cut; rho above the
//           ramp end = clearly something else (a hit, music) -> no cut; smooth in between (log domain).
//           cut_dB = -Reduction * confidence * (1 - ramp(rho)).  A hit makes S jump, so rho jumps and the cut vanishes.
//         Tonal: g = sqrt(max(gmin^2, 1 - A / max(S, P))): brings a steady peak down to its neighbours, no further.
//         then: 3-bin smoothing, focus weight, transient guard, slow-down / fast-up smoothing
//    3. removed spectrum = (1 - g) * X; inverse FFT; overlap-add into the "removed" signal
//    Output = original (delayed N samples) - mix * removed.   g = 1 everywhere  =>  output == input, bit for bit.
#include "hybrid/hiss/HissTamerEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace hc::hiss {
namespace {

constexpr float kEps = 1e-20f;

inline float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// Index of the q-quantile among n sorted values (nearest rank). Same rule as the Python reference.
inline int qIndex(double q, int n) noexcept { return static_cast<int>(std::floor(q * static_cast<double>(n - 1) + 0.5)); }

inline float pick(float* v, int n, int idx) noexcept {
    std::nth_element(v, v + idx, v + n);
    return v[idx];
}

} // namespace

const char* methodName(Method m) noexcept {
    switch (m) {
    case Method::Floor:      return "Floor";
    case Method::Percentile: return "Percentile";
    case Method::Tonal:      return "Tonal";
    }
    return "?";
}
const char* modeName(Mode m) noexcept { return m == Mode::MidSide ? "Mid/Side" : "Stereo"; }

std::vector<float> HissTamerEngine::makeWindow(int n) {
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        w[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n))));
    return w;
}

void HissTamerEngine::Detector::clear() noexcept {
    for (auto* v : {&S, &Fm, &curMin, &slotMin, &ringS, &ringF, &medS, &base, &minS, &aTmp, &cTmp}) std::fill(v->begin(), v->end(), 0.0f);
    std::fill(gain.begin(), gain.end(), 1.0f);
    std::fill(gTmp.begin(), gTmp.end(), 1.0f);
    guard = 0.0f;
    energyAvg = 0.0f;
}

HissTamerEngine::HissTamerEngine() = default;

void HissTamerEngine::prepare(double sampleRate) {
    prepared_ = false;
    fs_    = sampleRate;
    n_     = fs_ > cfg::kHighRateHz ? cfg::kFftHigh : cfg::kFftNormal;
    hop_   = n_ / cfg::kHopDivisor;
    bins_  = n_ / 2 + 1;
    fft_.prepare(n_);
    win_ = makeWindow(n_);
    double wsum = 0.0;
    for (int j = 0; j < cfg::kHopDivisor; ++j) {
        const double w = static_cast<double>(win_[static_cast<std::size_t>(j * hop_)]);
        wsum += w * w;
    }
    olaScale_ = static_cast<float>(1.0 / wsum);

    k0_ = std::max(1, static_cast<int>(std::floor(cfg::kTrackFromHz * n_ / fs_)));
    k1_ = bins_ - 2;
    nT_ = std::max(1, k1_ - k0_ + 1);
    tMax_ = std::max(8, static_cast<int>(std::ceil(static_cast<double>(cfg::kMaxSteadySeconds) * fs_ / hop_)));
    framesPerSlot_ = std::max(1, static_cast<int>(std::lround(static_cast<double>(cfg::kFloorSlotSeconds) * fs_ / hop_)));
    warmFrames_    = std::max(1, static_cast<int>(std::lround(static_cast<double>(cfg::kWarmupSeconds) * fs_ / hop_)));

    const double hopSec = static_cast<double>(hop_) / fs_;
    gainFallK_  = static_cast<float>(1.0 - std::exp(-hopSec / (static_cast<double>(cfg::kGainFallMs) * 0.001)));
    guardRiseK_ = static_cast<float>(1.0 - std::exp(-hopSec / (static_cast<double>(cfg::kGuardRiseMs) * 0.001)));
    guardFallK_ = static_cast<float>(1.0 - std::exp(-hopSec / (static_cast<double>(cfg::kGuardFallMs) * 0.001)));
    guardDecay_ = static_cast<float>(std::exp(-hopSec / (static_cast<double>(cfg::kGuardHoldMs) * 0.001)));

    const auto N = static_cast<std::size_t>(n_), B = static_cast<std::size_t>(bins_), T = static_cast<std::size_t>(nT_);
    for (int c = 0; c < 2; ++c) { in_[static_cast<std::size_t>(c)].assign(N, 0.0f); out_[static_cast<std::size_t>(c)].assign(N, 0.0f); }
    z_.assign(N, Cx{});
    xl_.assign(B, Cx{});
    xr_.assign(B, Cx{});
    w_.assign(T, 0.0f);
    pw_[0].assign(T, 0.0f);
    pw_[1].assign(T, 0.0f);
    scratch_.assign(static_cast<std::size_t>(std::max(tMax_, 64)) + 8, 0.0f);
    for (auto& d : det_) {
        d.S.assign(T, 0.0f); d.Fm.assign(T, 0.0f); d.curMin.assign(T, 0.0f);
        d.slotMin.assign(T * static_cast<std::size_t>(cfg::kFloorSlots), 0.0f);
        d.ringS.assign(T * static_cast<std::size_t>(tMax_), 0.0f);
        d.ringF.assign(T * static_cast<std::size_t>(tMax_), 0.0f);
        d.gain.assign(T, 1.0f); d.medS.assign(T, 0.0f); d.base.assign(T, 0.0f); d.minS.assign(T, 0.0f);
        d.aTmp.assign(T, 0.0f); d.cTmp.assign(T, 0.0f); d.gTmp.assign(T, 1.0f);
    }
    const float step = static_cast<float>(1.0 / std::max(1.0, static_cast<double>(cfg::kBypassMs) * 0.001 * fs_));
    mixRamp_.step = bypassRamp_.step = listenRamp_.step = step;
    prepared_ = true;
    reset();
}

HissTamerEngine::Snap HissTamerEngine::snapshot() const noexcept {
    Snap s{};
    s.sens   = clampf(sens_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    s.red    = clampf(red_.load(std::memory_order_relaxed), 0.0f, cfg::kMaxReductionDb);
    const auto fmax = static_cast<float>(std::min(cfg::kFocusMaxHz, 0.45 * fs_));
    s.focus  = clampf(focus_.load(std::memory_order_relaxed), static_cast<float>(cfg::kFocusMinHz), fmax);
    s.width  = clampf(width_.load(std::memory_order_relaxed), static_cast<float>(cfg::kWidthMinHz), static_cast<float>(cfg::kWidthMaxHz));
    s.mix    = clampf(mix_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    s.steady = clampf(steady_.load(std::memory_order_relaxed), cfg::kMinSteadySeconds, cfg::kMaxSteadySeconds);
    {
        const float ms = clampf(smooth_.load(std::memory_order_relaxed), cfg::kMinSmoothMs, cfg::kMaxSmoothMs);
        s.smoothA = static_cast<float>(std::exp(-(static_cast<double>(hop_) / fs_) / (static_cast<double>(ms) * 0.001)));
    }
    s.bypass = bypass_.load(std::memory_order_relaxed) != 0;
    s.listen = listen_.load(std::memory_order_relaxed) != 0;
    s.mode   = mode_.load(std::memory_order_relaxed) != 0 ? Mode::MidSide : Mode::Stereo;
    const int m = method_.load(std::memory_order_relaxed);
    s.method = m == 1 ? Method::Percentile : (m == 2 ? Method::Tonal : Method::Floor);
    return s;
}

void HissTamerEngine::setParams(const Params& p) noexcept {
    sens_.store(p.sensitivity, std::memory_order_relaxed);
    red_.store(p.reductionDb, std::memory_order_relaxed);
    focus_.store(p.focusHz, std::memory_order_relaxed);
    width_.store(p.widthHz, std::memory_order_relaxed);
    mix_.store(p.mix, std::memory_order_relaxed);
    steady_.store(p.steadySeconds, std::memory_order_relaxed);
    smooth_.store(p.smoothMs, std::memory_order_relaxed);
    bypass_.store(p.bypass ? 1 : 0, std::memory_order_relaxed);
    listen_.store(p.listenRemoved ? 1 : 0, std::memory_order_relaxed);
    mode_.store(static_cast<int>(p.mode), std::memory_order_relaxed);
    method_.store(static_cast<int>(p.method), std::memory_order_relaxed);
}

Params HissTamerEngine::params() const noexcept {
    const Snap s = snapshot();
    Params p;
    p.sensitivity = s.sens; p.reductionDb = s.red; p.focusHz = s.focus; p.widthHz = s.width; p.mix = s.mix;
    p.steadySeconds = s.steady; p.smoothMs = smooth_.load(std::memory_order_relaxed); p.bypass = s.bypass; p.listenRemoved = s.listen; p.mode = s.mode; p.method = s.method;
    return p;
}

void HissTamerEngine::clearDetectors() noexcept {
    for (auto& d : det_) d.clear();
    ringPos_ = ringFill_ = slotFrame_ = slot_ = slotsFilled_ = warm_ = 0;
    reductionAvg_ = 0.0f;
}

void HissTamerEngine::reset() noexcept {
    if (!prepared_) return;
    for (auto& v : in_) std::fill(v.begin(), v.end(), 0.0f);
    for (auto& v : out_) std::fill(v.begin(), v.end(), 0.0f);
    pos_ = hopCount_ = 0;
    clearDetectors();
    snap_ = snapshot();
    lastMode_ = snap_.mode;
    focusCacheHz_ = widthCacheHz_ = -1.0f;
    updateFocus();
    mixRamp_.cur = mixRamp_.target = snap_.mix;
    bypassRamp_.cur = bypassRamp_.target = snap_.bypass ? 1.0f : 0.0f;
    listenRamp_.cur = listenRamp_.target = snap_.listen ? 1.0f : 0.0f;
    reductionDb_.store(0.0f, std::memory_order_relaxed);
    artifactDb_.store(-120.0f, std::memory_order_relaxed);
    guardMeter_.store(0.0f, std::memory_order_relaxed);
}

void HissTamerEngine::prime(const float* left, const float* right, std::uint32_t frames) {
    if (!prepared_ || frames == 0) return;
    primeL_.assign(left, left + frames);
    primeR_.assign(right, right + frames);
    process(primeL_.data(), primeR_.data(), frames);
    // Keep what the detectors learned; start the audio path, the gains and the guard from scratch.
    for (auto& v : in_) std::fill(v.begin(), v.end(), 0.0f);
    for (auto& v : out_) std::fill(v.begin(), v.end(), 0.0f);
    pos_ = hopCount_ = 0;
    for (auto& d : det_) {
        std::fill(d.gain.begin(), d.gain.end(), 1.0f);
        std::fill(d.gTmp.begin(), d.gTmp.end(), 1.0f);
        d.guard = 0.0f;
        d.energyAvg = 0.0f;
    }
    warm_ = warmFrames_;
    mixRamp_.cur = mixRamp_.target;
    bypassRamp_.cur = bypassRamp_.target;
    listenRamp_.cur = listenRamp_.target;
}

void HissTamerEngine::updateFocus() noexcept {
    if (snap_.focus == focusCacheHz_ && snap_.width == widthCacheHz_) return;
    focusCacheHz_ = snap_.focus;
    widthCacheHz_ = snap_.width;
    const double c = static_cast<double>(snap_.focus), half = 0.5 * static_cast<double>(snap_.width);
    aLo_ = nT_;
    aHi_ = -1;
    for (int t = 0; t < nT_; ++t) {
        const double f = static_cast<double>(k0_ + t) * fs_ / static_cast<double>(n_);
        const double dist = std::max(0.0, std::fabs(f - c) - half);
        double w = 0.0;
        if (dist < cfg::kSkirtHz) w = 0.5 * (1.0 + std::cos(std::numbers::pi * dist / cfg::kSkirtHz));
        w_[static_cast<std::size_t>(t)] = static_cast<float>(w);
        if (w > 0.0) { aLo_ = std::min(aLo_, t); aHi_ = t; }
    }
    if (aHi_ < 0) aLo_ = 0;
}

void HissTamerEngine::process(float* L, float* R, std::uint32_t frames) noexcept {
    if (!prepared_) return;
    snap_ = snapshot();
    if (snap_.mode != lastMode_) {
        clearDetectors();
        lastMode_ = snap_.mode;
    }
    updateFocus();
    mixRamp_.set(snap_.mix);
    bypassRamp_.set(snap_.bypass ? 1.0f : 0.0f);
    listenRamp_.set(snap_.listen ? 1.0f : 0.0f);

    for (std::uint32_t i = 0; i < frames; ++i) {
        float x0 = L[i], x1 = R[i];
        if (!(std::fabs(x0) < 1e18f)) x0 = 0.0f;   // NaN and infinity become silence
        if (!(std::fabs(x1) < 1e18f)) x1 = 0.0f;
        const auto ps = static_cast<std::size_t>(pos_);
        const float d0 = in_[0][ps], d1 = in_[1][ps];     // the sample from N samples ago: the delayed original
        const float r0 = out_[0][ps], r1 = out_[1][ps];   // the removed part for that same moment
        in_[0][ps] = x0; in_[1][ps] = x1;
        out_[0][ps] = 0.0f; out_[1][ps] = 0.0f;

        const float m = mixRamp_.next(), b = bypassRamp_.next(), ls = listenRamp_.next();
        const float a = m * (1.0f - b);
        float y0 = d0 - a * r0, y1 = d1 - a * r1;
        const float lis = ls * (1.0f - b);
        if (lis > 0.0f) {
            y0 = (1.0f - lis) * y0 + lis * (m * r0);
            y1 = (1.0f - lis) * y1 + lis * (m * r1);
        }
        L[i] = y0;
        R[i] = y1;

        if (++pos_ >= n_) pos_ = 0;
        if (++hopCount_ >= hop_) { hopCount_ = 0; frame(); }
    }
}

// ---------------------------------------------------------------------------------------------
//  One detector: power in, per-bin gain out (det.gain). P holds the power of the tracked bins.
// ---------------------------------------------------------------------------------------------
void HissTamerEngine::runDetector(Detector& d, const float* P, float gmin) noexcept {
    const int nT = nT_;
    const float sens = snap_.sens;
    const auto tMax = static_cast<std::size_t>(tMax_);

    // 1. smoothed power, 13-bin median
    for (int t = 0; t < nT; ++t) {
        // The very first frame starts from the measured power itself: starting from zero would record
        // a fake "floor" of nearly nothing while the smoothing climbs, and that would poison the next 4 seconds.
        float s = ringFill_ == 0 ? P[t] : snap_.smoothA * d.S[static_cast<std::size_t>(t)] + (1.0f - snap_.smoothA) * P[t];
        if (s < 1e-30f) s = 0.0f;
        d.S[static_cast<std::size_t>(t)] = s;
    }
    constexpr int kHalf = cfg::kNeighbourMedian / 2;
    float v[cfg::kNeighbourMedian];
    for (int t = 0; t < nT; ++t) {
        for (int j = -kHalf; j <= kHalf; ++j)
            v[j + kHalf] = d.S[static_cast<std::size_t>(std::clamp(t + j, 0, nT - 1))];
        d.Fm[static_cast<std::size_t>(t)] = pick(v, cfg::kNeighbourMedian, kHalf);
    }

    // 2. history (ring) and floor slots
    const bool slotStart = slotFrame_ == 0;
    for (int t = 0; t < nT; ++t) {
        const auto ts = static_cast<std::size_t>(t);
        d.ringS[ts * tMax + static_cast<std::size_t>(ringPos_)] = d.S[ts];
        d.ringF[ts * tMax + static_cast<std::size_t>(ringPos_)] = d.Fm[ts];
        d.curMin[ts] = slotStart ? d.Fm[ts] : std::min(d.curMin[ts], d.Fm[ts]);
    }
    const int T   = std::clamp(static_cast<int>(std::lround(static_cast<double>(snap_.steady) * fs_ / hop_)), 8, tMax_);
    const int cnt = std::min(ringFill_ + 1, T);
    const float r0 = cfg::kSteadyLowBase - cfg::kSteadyLowSlope * sens;
    const float rho1 = cfg::kRampLowBase + cfg::kRampLowSlope * sens;
    const float lnRho1 = std::log(rho1), lnSpan = std::log(cfg::kRampSpan);
    const float gmin2 = gmin * gmin;
    const float warmK = std::min(1.0f, static_cast<float>(warm_) / static_cast<float>(warmFrames_));
    float* sc = scratch_.data();

    auto lastIdx = [&](int j) { return static_cast<std::size_t>(((ringPos_ - j) % tMax_ + tMax_) % tMax_); };
    auto loadRing = [&](const std::vector<float>& ring, int t) {
        for (int j = 0; j < cnt; ++j) sc[j] = ring[static_cast<std::size_t>(t) * tMax + lastIdx(j)];
    };

    // 3. artifact power A (and how sure we are) per active bin, then the gain
    if (aHi_ >= aLo_) {
        if (snap_.method == Method::Tonal) {
            constexpr int kTh = cfg::kTonalBaseBins / 2;
            const int t0 = std::max(0, aLo_ - kTh), t1 = std::min(nT - 1, aHi_ + kTh);
            for (int t = t0; t <= t1; ++t) {
                loadRing(d.ringS, t);
                float mn = sc[0];
                for (int j = 1; j < cnt; ++j) mn = std::min(mn, sc[j]);
                d.minS[static_cast<std::size_t>(t)] = mn;
                d.medS[static_cast<std::size_t>(t)] = pick(sc, cnt, cnt / 2);
            }
            float vb[cfg::kTonalBaseBins];
            const float thr = cfg::kTonalPromBaseDb - cfg::kTonalPromSlopeDb * sens;
            for (int t = aLo_; t <= aHi_; ++t) {
                const auto ts = static_cast<std::size_t>(t);
                for (int j = -kTh; j <= kTh; ++j)
                    vb[j + kTh] = d.medS[static_cast<std::size_t>(std::clamp(t + j, t0, t1))];
                const float base = pick(vb, cfg::kTonalBaseBins, kTh);
                const float ms = d.medS[ts];
                const float promDb = 10.0f * std::log10((ms + kEps) / (base + kEps));
                const float cr = clampf((promDb - thr) / thr, 0.0f, 1.0f);
                const float pm = d.minS[ts] / (ms + kEps);
                const float cp = clampf((pm - cfg::kTonalHoldLow) / cfg::kTonalHoldSpan, 0.0f, 1.0f);
                const float A = cr * cp * std::max(0.0f, ms - base);
                d.aTmp[ts] = A;
                const float D = std::max(d.S[ts], P[t]);
                const float gsq = 1.0f - A / (D + kEps);
                float g = gsq <= gmin2 ? gmin : std::sqrt(gsq);
                d.gTmp[ts] = 1.0f - warmK * (1.0f - g);
            }
        } else {
            const int ns = std::min(slotsFilled_, cfg::kFloorSlots);
            for (int t = aLo_; t <= aHi_; ++t) {
                const auto ts = static_cast<std::size_t>(t);
                float A, c;
                if (snap_.method == Method::Floor) {
                    float F = d.curMin[ts];
                    for (int s = 0; s < ns; ++s) F = std::min(F, d.slotMin[ts * static_cast<std::size_t>(cfg::kFloorSlots) + static_cast<std::size_t>(s)]);
                    F *= cfg::kFloorBias;
                    loadRing(d.ringF, t);
                    const float med = pick(sc, cnt, cnt / 2);
                    c = clampf((F / (med + kEps) - r0) / cfg::kSteadySpan, 0.0f, 1.0f);
                    A = F;
                } else {
                    loadRing(d.ringF, t);
                    const float q20 = pick(sc, cnt, qIndex(static_cast<double>(cfg::kQuantileLow), cnt));
                    const float q80 = pick(sc, cnt, qIndex(static_cast<double>(cfg::kQuantileHigh), cnt));
                    c = clampf((q20 / (q80 + kEps) - r0) / cfg::kSteadySpan, 0.0f, 1.0f);
                    A = q20 * cfg::kQuantileBias;
                }
                d.aTmp[ts] = A;
                d.cTmp[ts] = c;
                // ramp: 0 = "just the artifact", 1 = "clearly something else"
                const float x = (std::log((d.S[ts] + kEps) / (A + kEps)) - lnRho1) / lnSpan;
                const float tt = clampf(x, 0.0f, 1.0f);
                const float ramp = tt * tt * (3.0f - 2.0f * tt);
                const float g = std::pow(gmin, c * (1.0f - ramp));
                d.gTmp[ts] = 1.0f - warmK * (1.0f - g);
            }
        }
    }

    // 5. transient guard (from the band's own energy)
    float energy = 0.0f, wsum = 0.0f, artAcc = 0.0f;
    for (int t = aLo_; t <= aHi_; ++t) {
        const float w = w_[static_cast<std::size_t>(t)];
        energy += w * P[t];
        wsum += w;
        artAcc += w * d.aTmp[static_cast<std::size_t>(t)];
    }
    const float ratio = energy / (d.energyAvg + 1e-30f);
    const float thr = cfg::kGuardThresholdBase + cfg::kGuardThresholdSlope * (1.0f - sens);
    float target = clampf((ratio - thr) / (0.5f * thr), 0.0f, 1.0f);
    // A jump only counts as "a hit" when the band is also clearly above the learned artifact level.
    // Hiss that wobbles (flutter) swings a lot but stays near its own floor, so it must not switch the cut off.
    const float aboveFloor = energy / (artAcc + 1e-30f);
    if (artAcc > 0.0f) target *= clampf((aboveFloor - rho1) / rho1, 0.0f, 1.0f);
    d.guard = std::max(target, d.guard * guardDecay_);
    d.energyAvg += (energy > d.energyAvg ? guardRiseK_ : guardFallK_) * (energy - d.energyAvg);
    if (wsum <= 0.0f) d.guard = 0.0f;

    // 6. smooth across bins, apply focus weight and guard, then smooth in time
    for (int t = 0; t < nT; ++t) {
        const auto ts = static_cast<std::size_t>(t);
        float tg = 1.0f;
        if (t >= aLo_ && t <= aHi_) {
            const float a = d.gTmp[static_cast<std::size_t>(std::max(t - 1, aLo_))];
            const float b = d.gTmp[ts];
            const float c = d.gTmp[static_cast<std::size_t>(std::min(t + 1, aHi_))];
            const float gs = 0.25f * a + 0.5f * b + 0.25f * c;
            const float gf = 1.0f - w_[ts] * (1.0f - gs);
            tg = 1.0f - (1.0f - gf) * (1.0f - d.guard);
        }
        float G = d.gain[ts];
        G += (tg > G ? cfg::kGainRise : gainFallK_) * (tg - G);
        if (G > 1.0f - 1e-6f) G = 1.0f;
        if (G < gmin) G = gmin;
        d.gain[ts] = G;
    }
    (void)artAcc;
}

void HissTamerEngine::frame() noexcept {
    const int N = n_;
    for (int i = 0; i < N; ++i) {
        int idx = pos_ + i;
        if (idx >= N) idx -= N;
        const float w = win_[static_cast<std::size_t>(i)];
        z_[static_cast<std::size_t>(i)] = Cx(in_[0][static_cast<std::size_t>(idx)] * w, in_[1][static_cast<std::size_t>(idx)] * w);
    }
    fft_.forward(z_.data());
    for (int k = 0; k < bins_; ++k) {
        const Cx zk = z_[static_cast<std::size_t>(k)];
        const Cx zn = z_[static_cast<std::size_t>((N - k) & (N - 1))];
        const float ar = 0.5f * (zk.real() + zn.real()), ai = 0.5f * (zk.imag() - zn.imag());          // XL
        const float dr = zk.real() - zn.real(), di = zk.imag() + zn.imag();                           // Z[k] - conj(Z[N-k])
        xl_[static_cast<std::size_t>(k)] = Cx(ar, ai);
        xr_[static_cast<std::size_t>(k)] = Cx(0.5f * di, -0.5f * dr);                                   // / (2i)
    }

    const float gmin = std::pow(10.0f, -snap_.red * 0.05f);
    const bool ms = snap_.mode == Mode::MidSide;
    float* p0 = pw_[0].data();
    float* p1 = pw_[1].data();
    for (int t = 0; t < nT_; ++t) {
        const Cx a = xl_[static_cast<std::size_t>(k0_ + t)], b = xr_[static_cast<std::size_t>(k0_ + t)];
        if (!ms) {
            p0[t] = 0.5f * (a.real() * a.real() + a.imag() * a.imag() + b.real() * b.real() + b.imag() * b.imag());
        } else {
            const float mr = 0.5f * (a.real() + b.real()), mi = 0.5f * (a.imag() + b.imag());
            const float sr = 0.5f * (a.real() - b.real()), si = 0.5f * (a.imag() - b.imag());
            p0[t] = mr * mr + mi * mi;
            p1[t] = sr * sr + si * si;
        }
    }
    runDetector(det_[0], p0, gmin);
    if (ms) runDetector(det_[1], p1, gmin);

    // removed spectrum -> z_
    std::fill(z_.begin(), z_.end(), Cx{});
    bool any = false;
    float redAcc = 0.0f, redW = 0.0f, artAcc = 0.0f, artW = 0.0f;
    for (int t = 0; t < nT_; ++t) {
        const auto ts = static_cast<std::size_t>(t);
        const int k = k0_ + t;
        const Cx a = xl_[static_cast<std::size_t>(k)], b = xr_[static_cast<std::size_t>(k)];
        float lr, li, rr, ri;
        const float g0 = det_[0].gain[ts];
        if (!ms) {
            const float rem = 1.0f - g0;
            if (rem <= 0.0f) continue;
            lr = a.real() * rem; li = a.imag() * rem; rr = b.real() * rem; ri = b.imag() * rem;
        } else {
            const float g1 = det_[1].gain[ts];
            const float remM = 1.0f - g0, remS = 1.0f - g1;
            if (remM <= 0.0f && remS <= 0.0f) continue;
            const float mr = 0.5f * (a.real() + b.real()) * remM, mi = 0.5f * (a.imag() + b.imag()) * remM;
            const float sr = 0.5f * (a.real() - b.real()) * remS, si = 0.5f * (a.imag() - b.imag()) * remS;
            lr = mr + sr; li = mi + si; rr = mr - sr; ri = mi - si;
        }
        any = true;
        z_[static_cast<std::size_t>(k)]     = Cx(lr - ri, li + rr);
        z_[static_cast<std::size_t>(N - k)] = Cx(lr + ri, -li + rr);
    }
    for (int t = aLo_; t <= aHi_; ++t) {
        const auto ts = static_cast<std::size_t>(t);
        const float w = w_[ts];
        float g = det_[0].gain[ts];
        if (ms) g = 0.5f * (g + det_[1].gain[ts]);
        redAcc += w * 20.0f * std::log10(std::max(g, 1e-6f));
        redW += w;
        artAcc += w * det_[0].aTmp[ts];
        artW += w;
    }
    if (any) {
        fft_.inverse(z_.data());
        for (int i = 0; i < N; ++i) {
            int idx = pos_ + i;
            if (idx >= N) idx -= N;
            const float w = win_[static_cast<std::size_t>(i)] * olaScale_;
            out_[0][static_cast<std::size_t>(idx)] += z_[static_cast<std::size_t>(i)].real() * w;
            out_[1][static_cast<std::size_t>(idx)] += z_[static_cast<std::size_t>(i)].imag() * w;
        }
    }

    // meters
    const float red = redW > 0.0f ? redAcc / redW : 0.0f;
    reductionAvg_ = 0.8f * reductionAvg_ + 0.2f * red;
    reductionDb_.store(reductionAvg_, std::memory_order_relaxed);
    const float artMean = artW > 0.0f ? artAcc / artW : 0.0f;
    artifactDb_.store(artMean > 1e-20f ? 10.0f * std::log10(artMean / (0.5f * static_cast<float>(N))) : -120.0f, std::memory_order_relaxed);
    guardMeter_.store(det_[0].guard, std::memory_order_relaxed);

    // counters shared by all detectors
    ringPos_ = (ringPos_ + 1) % tMax_;
    ringFill_ = std::min(ringFill_ + 1, tMax_);
    if (++slotFrame_ >= framesPerSlot_) {
        for (auto& d : det_)
            for (int t = 0; t < nT_; ++t)
                d.slotMin[static_cast<std::size_t>(t) * static_cast<std::size_t>(cfg::kFloorSlots) + static_cast<std::size_t>(slot_)] = d.curMin[static_cast<std::size_t>(t)];
        slot_ = (slot_ + 1) % cfg::kFloorSlots;
        slotsFilled_ = std::min(cfg::kFloorSlots, slotsFilled_ + 1);
        slotFrame_ = 0;
    }
    if (warm_ < warmFrames_) ++warm_;
}

} // namespace hc::hiss
