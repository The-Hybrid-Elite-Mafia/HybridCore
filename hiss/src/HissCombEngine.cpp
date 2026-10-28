#include "hybrid/hiss/HissCombEngine.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace hc::hiss {
namespace {

constexpr int kSynthChunk = 16;

float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// RBJ high-pass biquad, normalised so a0 = 1. out = {b0, b1, b2, a1, a2}
void highPass(double fs, double fc, double q, double* out) noexcept {
    const double w0 = 2.0 * std::numbers::pi * fc / fs, c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
    out[0] = (1.0 + c) * 0.5 / a0;
    out[1] = -(1.0 + c) / a0;
    out[2] = (1.0 + c) * 0.5 / a0;
    out[3] = -2.0 * c / a0;
    out[4] = (1.0 - al) / a0;
}

std::complex<double> biquadResponse(const double* b, double w) noexcept {
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    return (b[0] + b[1] * z1 + b[2] * z2) / (1.0 + b[3] * z1 + b[4] * z2);
}

} // namespace

HissCombEngine::HissCombEngine() = default;

void HissCombEngine::prepare(double sampleRate) {
    fs_ = sampleRate;
    const double pd = sampleRate / cfg::kCombSpacingHz;
    period_ = static_cast<int>(std::lround(pd));
    supported_ = period_ >= 40 && std::fabs(pd - static_cast<double>(period_)) < 1e-6;
    if (!supported_) { period_ = 1920; }
    period2_ = 2 * period_;
    cosT_.resize(static_cast<std::size_t>(period2_));
    sinT_.resize(static_cast<std::size_t>(period2_));
    for (int i = 0; i < period2_; ++i) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(period2_);
        cosT_[static_cast<std::size_t>(i)] = std::cos(a);
        sinT_[static_cast<std::size_t>(i)] = std::sin(a);
    }
    jMin_ = std::max(1, static_cast<int>(std::ceil(2000.0 / cfg::kCombSpacingHz)));
    jMax_ = std::max(jMin_, static_cast<int>(std::floor(0.45 * sampleRate / cfg::kCombSpacingHz)));
    highPass(sampleRate, cfg::kCombHighPassHz, 0.5411961, hp_[0]);
    highPass(sampleRate, cfg::kCombHighPassHz, 1.3065630, hp_[1]);
    hRe_.assign(static_cast<std::size_t>(jMax_ + 2), 1.0);
    hIm_.assign(static_cast<std::size_t>(jMax_ + 2), 0.0);
    for (int j = 1; j <= jMax_; ++j) {
        const double w = 2.0 * std::numbers::pi * static_cast<double>(j) * cfg::kCombSpacingHz / sampleRate;
        const auto h = biquadResponse(hp_[0], w) * biquadResponse(hp_[1], w);
        hRe_[static_cast<std::size_t>(j)] = h.real();
        hIm_[static_cast<std::size_t>(j)] = h.imag();
    }
    pAlpha_ = 1.0 - std::exp(-1.0 / (cfg::kCombPowerSmoothMs * 0.001 * sampleRate));
    warmN_ = static_cast<int>(0.05 * sampleRate);
    refreshSamples_ = static_cast<int>(cfg::kCombRefreshSeconds * sampleRate);
    for (auto& c : ch_) {
        c.S.assign(static_cast<std::size_t>(period2_), 0.0);
        c.Z.assign(static_cast<std::size_t>(period2_), 0.0);
        c.T2.assign(static_cast<std::size_t>(period2_), 0.0);
        c.SK.assign(static_cast<std::size_t>(period2_) * kChunks, 0.0);
        c.ZK.assign(static_cast<std::size_t>(period2_) * kChunks, 0.0);
        c.TK.assign(static_cast<std::size_t>(period2_) * kChunks, 0.0);
        c.validK.fill(false);
        c.w[0].assign(static_cast<std::size_t>(period_), 0.0f);
        c.w[1].assign(static_cast<std::size_t>(period_), 0.0f);
        c.cRe.assign(static_cast<std::size_t>(jMax_ + 2), 0.0);
        c.cIm.assign(static_cast<std::size_t>(jMax_ + 2), 0.0);
    }
    prepared_ = true;
    clearState();
}

void HissCombEngine::clearState() noexcept {
    if (!prepared_) return;
    for (auto& c : ch_) {
        std::fill(c.S.begin(), c.S.end(), 0.0);
        std::fill(c.Z.begin(), c.Z.end(), 0.0);
        std::fill(c.SK.begin(), c.SK.end(), 0.0);
        std::fill(c.ZK.begin(), c.ZK.end(), 0.0);
        std::fill(c.w[0].begin(), c.w[0].end(), 0.0f);
        std::fill(c.w[1].begin(), c.w[1].end(), 0.0f);
        c.active = 0;
        for (int i = 0; i < 2; ++i) { c.hpS1[i] = 0.0; c.hpS2[i] = 0.0; }
        c.p = 0.0;
        c.blockMin = 1e300;
        c.minRing.fill(1e300);
        c.ringFill = 0; c.ringPos = 0; c.blockCount = 0;
        c.permMin = 1e300;
        c.pref = 1e300;
        c.warm = 0;
    }
    m1_ = 0; m2_ = 0;
    stage_ = 0; lineIdx_ = 0; synthIdx_ = 0; chIdx_ = 0;
    workCredit_ = 0.0; itemsPerSample_ = 0.0; sinceRefresh_ = 0;
    snap_ = snapshot();
    mixR_.cur = mixR_.target = snap_.mix;
    byR_.cur = byR_.target = snap_.bypass ? 1.0f : 0.0f;
    listenR_.cur = listenR_.target = snap_.listen ? 1.0f : 0.0f;
    const auto step = static_cast<float>(1.0 / (cfg::kBypassMs * 0.001 * fs_));
    mixR_.step = byR_.step = listenR_.step = step;
    gainSub_ = 1.0f - std::pow(10.0f, -snap_.red / 20.0f);
    levelDb_.store(-200.0f);
    linesKept_.store(0);
    suspect_.store(false);
    infiniteMemory_ = false;
}

void HissCombEngine::reset() noexcept { clearState(); }

HissCombEngine::Snap HissCombEngine::snapshot() const noexcept {
    Snap s{};
    s.red = clampf(red_.load(std::memory_order_relaxed), 0.0f, cfg::kMaxReductionDb);
    s.lo = clampf(lo_p_.load(std::memory_order_relaxed), 2000.0f, 40000.0f);
    s.hi = clampf(hi_p_.load(std::memory_order_relaxed), 2000.0f, 40000.0f);
    s.mix = clampf(mix_p_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    s.adapt = clampf(adapt_p_.load(std::memory_order_relaxed), cfg::kCombAdaptMinSeconds, cfg::kCombAdaptMaxSeconds);
    s.sens = clampf(sens_p_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    s.bypass = bypass_.load(std::memory_order_relaxed);
    s.listen = listen_.load(std::memory_order_relaxed);
    return s;
}

void HissCombEngine::setParams(const CombParams& p) noexcept {
    red_.store(p.reductionDb, std::memory_order_relaxed);
    lo_p_.store(p.loHz, std::memory_order_relaxed);
    hi_p_.store(p.hiHz, std::memory_order_relaxed);
    mix_p_.store(p.mix, std::memory_order_relaxed);
    adapt_p_.store(p.adaptSeconds, std::memory_order_relaxed);
    sens_p_.store(p.sensitivity, std::memory_order_relaxed);
    bypass_.store(p.bypass, std::memory_order_relaxed);
    listen_.store(p.listenRemoved, std::memory_order_relaxed);
}

CombParams HissCombEngine::params() const noexcept {
    const Snap s = snapshot();
    CombParams p;
    p.reductionDb = s.red; p.loHz = s.lo; p.hiHz = s.hi; p.mix = s.mix; p.adaptSeconds = s.adapt; p.sensitivity = s.sens;
    p.bypass = s.bypass; p.listenRemoved = s.listen;
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
//  Refresh: snapshot -> one item per line -> synthesis chunks -> swap. Spread over the audio blocks.
// ---------------------------------------------------------------------------------------------------------------------
void HissCombEngine::beginRefresh() noexcept {
    lo_ = snap_.lo;
    hi_ = snap_.hi;
    kappa_ = cfg::kCombWienerBase - cfg::kCombWienerSlope * static_cast<double>(snap_.sens);
    const int jLo = std::clamp(static_cast<int>(std::ceil(static_cast<double>(lo_) / cfg::kCombSpacingHz)), jMin_, jMax_);
    const int jHi = std::clamp(static_cast<int>(std::floor(static_cast<double>(hi_) / cfg::kCombSpacingHz)), jMin_, jMax_);
    const int lines = std::max(0, jHi - jLo + 1);
    const double perChan = 1.0 + static_cast<double>(lines) + static_cast<double>((period_ + kSynthChunk - 1) / kSynthChunk) + 1.0;
    itemsPerSample_ = 2.0 * perChan / static_cast<double>(std::max(1, refreshSamples_));
    stage_ = 1;
    chIdx_ = 0;
    lineIdx_ = jLo;
    synthIdx_ = 0;
}

bool HissCombEngine::step(int chn) noexcept {
    Chan& c = ch_[static_cast<std::size_t>(chn)];
    const int jLo = std::clamp(static_cast<int>(std::ceil(static_cast<double>(lo_) / cfg::kCombSpacingHz)), jMin_, jMax_);
    const int jHi = std::clamp(static_cast<int>(std::floor(static_cast<double>(hi_) / cfg::kCombSpacingHz)), jMin_, jMax_);
    if (stage_ == 1) {
        for (int m = 0; m < period2_; ++m) {
            const auto i = static_cast<std::size_t>(m);
            c.T2[i] = c.S[i] / (c.Z[i] + cfg::kCombPriorWeight);
        }
        if (agree_) {
            // offline: the estimate is the plain average over all quiet material, no forgetting
            for (int m = 0; m < period2_; ++m) {
                double sa = 0.0, za = 0.0;
                for (int k = 0; k < kChunks; ++k) {
                    const auto u = static_cast<std::size_t>(k) * static_cast<std::size_t>(period2_) + static_cast<std::size_t>(m);
                    sa += c.SK[u]; za += c.ZK[u];
                }
                c.T2[static_cast<std::size_t>(m)] = sa / (za + cfg::kCombPriorWeight);
            }
            for (int k = 0; k < kChunks; ++k) {
                double zs = 0.0;
                const std::size_t o = static_cast<std::size_t>(k) * static_cast<std::size_t>(period2_);
                for (int m = 0; m < period2_; ++m) zs += c.ZK[o + static_cast<std::size_t>(m)];
                c.validK[static_cast<std::size_t>(k)] = zs >= cfg::kCombAgreeMinSeconds * fs_;
                for (int m = 0; m < period2_; ++m) {
                    const auto u = o + static_cast<std::size_t>(m);
                    c.TK[u] = c.SK[u] / (c.ZK[u] + cfg::kCombPriorWeight);
                }
            }
        }
        std::fill(c.cRe.begin(), c.cRe.end(), 0.0);
        std::fill(c.cIm.begin(), c.cIm.end(), 0.0);
        lineIdx_ = jLo;
        stage_ = 2;
        return false;
    }
    if (stage_ == 2) {
        if (jHi < jLo || lineIdx_ > jHi) { stage_ = 3; synthIdx_ = 0; return false; }
        const int j = lineIdx_++;
        double re[3] = {0, 0, 0}, im[3] = {0, 0, 0};
        const int ks[3] = {2 * j, 2 * j - 1, 2 * j + 1};
        for (int b = 0; b < 3; ++b) {
            const int k = ks[b];
            int idx = 0;
            double sr = 0.0, si = 0.0;
            for (int m = 0; m < period2_; ++m) {
                const double t = c.T2[static_cast<std::size_t>(m)];
                sr += t * cosT_[static_cast<std::size_t>(idx)];
                si += t * sinT_[static_cast<std::size_t>(idx)];
                idx += k;
                if (idx >= period2_) idx -= period2_;
            }
            re[b] = sr / static_cast<double>(period2_);
            im[b] = si / static_cast<double>(period2_);
        }
        bool agrees = true;
        if (agree_) {
            double cr[kChunks], ci[kChunks], nz[kChunks];
            int nv = 0;
            for (int k = 0; k < kChunks; ++k) {
                if (!c.validK[static_cast<std::size_t>(k)]) continue;
                const std::size_t o = static_cast<std::size_t>(k) * static_cast<std::size_t>(period2_);
                double rr[3] = {0, 0, 0}, ii[3] = {0, 0, 0};
                const int kk[3] = {2 * j, 2 * j - 1, 2 * j + 1};
                for (int b3 = 0; b3 < 3; ++b3) {
                    int idx = 0;
                    for (int m = 0; m < period2_; ++m) {
                        const auto u = static_cast<std::size_t>(idx);
                        const double t = c.TK[o + static_cast<std::size_t>(m)];
                        rr[b3] += t * cosT_[u]; ii[b3] += t * sinT_[u];
                        idx += kk[b3];
                        if (idx >= period2_) idx -= period2_;
                    }
                    rr[b3] /= period2_; ii[b3] /= period2_;
                }
                cr[nv] = rr[0]; ci[nv] = ii[0];
                nz[nv] = 0.5 * (rr[1] * rr[1] + ii[1] * ii[1] + rr[2] * rr[2] + ii[2] * ii[2]);
                ++nv;
            }
            if (nv >= 2) {
                double mr = 0, mi = 0;
                for (int q = 0; q < nv; ++q) { mr += cr[q]; mi += ci[q]; }
                mr /= nv; mi /= nv;
                const double m2 = mr * mr + mi * mi;
                const double mabs = std::sqrt(m2);
                for (int q = 0; q < nv && mabs > 0.0; ++q) {
                    // split each chunk's estimate into the part along the average (size may vary a lot: the real artifact breathes) and the part across it (phase: must not)
                    const double along = (cr[q] * mr + ci[q] * mi) / mabs;
                    const double across = (ci[q] * mr - cr[q] * mi) / mabs;
                    if (across * across > cfg::kCombAgreeMax * cfg::kCombAgreeMax * m2 + 4.0 * nz[q]) agrees = false;
                    if (along < 0.0 && along * along > 4.0 * nz[q]) agrees = false;
                }
            }
        }
        const double pw = re[0] * re[0] + im[0] * im[0];
        const double noise = 0.5 * (re[1] * re[1] + im[1] * im[1] + re[2] * re[2] + im[2] * im[2]);
        double g = pw > 0.0 ? 1.0 - kappa_ * noise / pw : 0.0;
        g = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
        if (!agrees) g = 0.0;
        const double f = static_cast<double>(j) * cfg::kCombSpacingHz;
        const double e = cfg::kCombEdgeHz;
        double edge = 1.0;
        if (f < static_cast<double>(lo_) + e) edge = std::clamp((f - static_cast<double>(lo_)) / e, 0.0, 1.0);
        if (f > static_cast<double>(hi_) - e) edge = std::min(edge, std::clamp((static_cast<double>(hi_) - f) / e, 0.0, 1.0));
        edge = 0.5 - 0.5 * std::cos(edge * std::numbers::pi);
        g *= edge;
        // undo the high-pass: the learner saw H(f) x c
        const std::complex<double> h(hRe_[static_cast<std::size_t>(j)], hIm_[static_cast<std::size_t>(j)]);
        const std::complex<double> cx = std::complex<double>(re[0], im[0]) / h;
        c.cRe[static_cast<std::size_t>(j)] = g * cx.real();
        c.cIm[static_cast<std::size_t>(j)] = g * cx.imag();
        return false;
    }
    // stage 3: build the pattern (one period) from the kept lines
    {
        auto& dst = c.w[c.active ^ 1];
        const int m0 = synthIdx_, m1 = std::min(period_, synthIdx_ + kSynthChunk);
        for (int m = m0; m < m1; ++m) {
            double acc = 0.0;
            // angle = +2 pi j m / P = 2 pi (2 j m) / P2; the tables hold the negative angle, so use cos as is and negate sin
            const int inc = 2 * m;                                   // < period2_
            int idx = static_cast<int>((static_cast<long long>(inc) * jLo) % period2_);
            for (int j = jLo; j <= jHi; ++j) {
                const auto u = static_cast<std::size_t>(idx);
                acc += 2.0 * (c.cRe[static_cast<std::size_t>(j)] * cosT_[u] + c.cIm[static_cast<std::size_t>(j)] * sinT_[u]);
                idx += inc;
                if (idx >= period2_) idx -= period2_;
            }
            dst[static_cast<std::size_t>(m)] = static_cast<float>(acc);
        }
        synthIdx_ = m1;
        if (synthIdx_ < period_) return false;
        // done: sanity check, swap, publish meters
        {
            double e0 = 0.0;
            for (float v : dst) e0 += static_cast<double>(v) * v;
            const double rmsDb = 10.0 * std::log10(e0 / static_cast<double>(period_) + 1e-30);
            if (rmsDb > cfg::kCombMaxPatternDb) {
                std::fill(dst.begin(), dst.end(), 0.0f);
                suspectNow_ = true;
            }
        }
        c.active ^= 1;
        int kept = 0;
        for (int j = jLo; j <= jHi; ++j)
            if (c.cRe[static_cast<std::size_t>(j)] != 0.0 || c.cIm[static_cast<std::size_t>(j)] != 0.0) ++kept;
        if (chn == 1) {
            suspect_.store(suspectNow_, std::memory_order_relaxed);
            suspectNow_ = false;
            everRefreshed_ = true;
            double ep = 0.0;
            for (const auto& cc : ch_)
                for (float v : cc.w[cc.active]) ep += static_cast<double>(v) * v;
            ep /= static_cast<double>(2 * period_);
            levelDb_.store(static_cast<float>(10.0 * std::log10(ep + 1e-20)), std::memory_order_relaxed);
            linesKept_.store(suspect_.load() ? 0 : kept, std::memory_order_relaxed);
        }
        return true;
    }
}

void HissCombEngine::refreshWork(int items) noexcept {
    for (int i = 0; i < items; ++i) {
        if (stage_ == 0) { beginRefresh(); continue; }
        if (step(chIdx_)) {
            if (chIdx_ == 0) {
                chIdx_ = 1;
                stage_ = 1;
            } else {
                stage_ = 0;   // both channels done; the next call starts a new refresh
            }
        }
    }
}

void HissCombEngine::finishRefreshNow() noexcept {
    // complete a refresh already under way, then run one more complete one
    int guard = 0;
    while (stage_ != 0 && guard++ < 10'000'000) refreshWork(1);
    snap_ = snapshot();
    beginRefresh();
    guard = 0;
    while (stage_ != 0 && guard++ < 10'000'000) refreshWork(1);
}

// ---------------------------------------------------------------------------------------------------------------------
//  Audio
// ---------------------------------------------------------------------------------------------------------------------
void HissCombEngine::processOne(float& l, float& r) noexcept {
    float* io[2] = {&l, &r};
    const float mixV = mixR_.next(), byV = byR_.next(), lisV = listenR_.next();
    const float engage = mixV * (1.0f - byV) * gainSub_;
    const double forget = forget_;
    const bool learn = learning_.load(std::memory_order_relaxed);
    for (int chn = 0; chn < 2; ++chn) {
        Chan& c = ch_[static_cast<std::size_t>(chn)];
        float x = *io[chn];
        if (!std::isfinite(x)) x = 0.0f;
        // high-pass for the learner
        double y = static_cast<double>(x);
        for (int s = 0; s < 2; ++s) {
            const double* b = hp_[s];
            const double o = b[0] * y + c.hpS1[s];
            c.hpS1[s] = b[1] * y - b[3] * o + c.hpS2[s];
            c.hpS2[s] = b[2] * y - b[4] * o;
            y = o;
        }
        c.p += pAlpha_ * (y * y - c.p);
        if (supported_) {
            // lowest power seen lately (blocks of 0.25 s, last 4 s); digital silence is ignored
            if (c.warm < warmN_) ++c.warm;                  // the power meter starts from zero: ignore its first 50 ms
            else if (c.p < c.blockMin && c.p > cfg::kCombSilencePower) c.blockMin = c.p;
            if (++c.blockCount >= static_cast<int>(0.25 * fs_)) {
                c.blockCount = 0;
                if (c.blockMin < 1e299) {
                    c.minRing[static_cast<std::size_t>(c.ringPos)] = c.blockMin;
                    c.ringPos = (c.ringPos + 1) % 256;
                    c.ringFill = std::min(256, c.ringFill + 1);
                    if (!freezeFloor_) {
                        c.permMin = std::min(c.permMin, c.blockMin);
                        auto& lg = floorLog_[static_cast<std::size_t>(chn)];
                        if (lg.size() < lg.capacity()) lg.push_back(c.blockMin);      // offline pass only: capacity is reserved in prime(), nothing is allocated here
                    }
                }
                c.blockMin = 1e300;
                double mn = 1e300;
                if (infiniteMemory_) mn = c.permMin;
                else for (double v : c.minRing) mn = std::min(mn, v);
                c.pref = mn < 1e299 ? cfg::kCombQuietFactor * mn : 1e300;
            }
            double pm = c.blockMin < 1e299 ? cfg::kCombQuietFactor * c.blockMin : 1e300;
            pm = std::min(pm, c.pref);
            if (infiniteMemory_) pm = c.pref;     // offline pass: the quiet floor of the whole file, not of the last moments
            const double a = (pm >= 1e299 || c.p <= pm) ? 1.0 : (hardGate_ ? 0.0 : pm / c.p);
            if (hardGate_ && a > 0.0) ++c.gated;
            if (learn) {
                const auto i = static_cast<std::size_t>(m2_);
                const double f = 1.0 - a * forget;
                c.S[i] = f * c.S[i] + a * y;
                c.Z[i] = f * c.Z[i] + a;
                if (agree_) {
                    const int ck = c.gatedTotal > 0 ? static_cast<int>(std::min<std::uint64_t>(kChunks - 1, c.gated * kChunks / c.gatedTotal)) : 0;
                    const auto u = static_cast<std::size_t>(ck) * static_cast<std::size_t>(period2_) + i;
                    c.SK[u] += a * y;       // no forgetting: the offline pass weighs the whole file evenly
                    c.ZK[u] += a;
                }
            }
        }
        // nothing is subtracted into silence or fade-outs: there the artifact is not there to remove, and a subtracted pattern would be added as new noise
        const float sub = c.p > cfg::kCombSilencePower ? c.w[c.active][static_cast<std::size_t>(m1_)] * engage : 0.0f;
        *io[chn] = (1.0f - lisV) * (x - sub) + lisV * sub;
    }
    if (++m1_ >= period_) m1_ = 0;
    if (++m2_ >= period2_) m2_ = 0;
}

void HissCombEngine::process(float* left, float* right, std::uint32_t frames) noexcept {
    if (!prepared_ || frames == 0) return;
    const Snap s = snapshot();
    snap_ = s;
    mixR_.target = s.mix;
    byR_.target = s.bypass ? 1.0f : 0.0f;
    listenR_.target = s.listen ? 1.0f : 0.0f;
    gainSub_ = 1.0f - std::pow(10.0f, -s.red / 20.0f);
    forget_ = 1.0 / std::max(1.0, static_cast<double>(s.adapt) * fs_ / static_cast<double>(period2_));
    for (std::uint32_t n = 0; n < frames; ++n) processOne(left[n], right[n]);
    if (supported_ && (learning_.load(std::memory_order_relaxed) || stage_ != 0 || !everRefreshed_)) {
        workCredit_ += itemsPerSample_ * static_cast<double>(frames);
        if (stage_ == 0) workCredit_ += 1.0;   // starting a refresh costs one item
        int items = static_cast<int>(workCredit_);
        items = std::min(items, 64);
        workCredit_ -= static_cast<double>(items);
        if (workCredit_ > 256.0) workCredit_ = 256.0;
        refreshWork(items);
    }
}

void HissCombEngine::prime(const float* left, const float* right, std::uint32_t frames) {
    if (!prepared_ || !supported_ || frames == 0) return;
    clearState();
    // the learning pass runs fully treated and unlistened, so nothing odd gets learned from the settings
    std::vector<float> l(left, left + frames), r(right, right + frames);
    const int saved = refreshSamples_;
    refreshSamples_ = static_cast<int>(5.0 * fs_);   // offline: rebuild every 5 s instead of every second
    constexpr std::uint32_t kBlock = 512;
    // pass A: find the quiet floor of the whole file (nothing learned yet)
    infiniteMemory_ = true; freezeFloor_ = false; hardGate_ = false;
    for (auto& lg : floorLog_) { lg.clear(); lg.reserve(static_cast<std::size_t>(frames / (0.25 * fs_)) + 16); }
    const bool wasLearning = learning_.load();
    learning_.store(false);
    for (std::uint32_t i = 0; i < frames; i += kBlock) process(l.data() + i, r.data() + i, std::min(kBlock, frames - i));
    for (std::size_t q = 0; q < 2; ++q) {
        auto& lg = floorLog_[q];
        if (lg.empty()) continue;
        std::sort(lg.begin(), lg.end());
        ch_[q].permMin = lg[static_cast<std::size_t>(cfg::kCombFloorPercentile * static_cast<double>(lg.size() - 1))];   // a low percentile, not the single lowest block: one odd dip must not set the floor
    }
    for (auto& lg : floorLog_) { lg.clear(); lg.shrink_to_fit(); }
    // pass A2: count how much quiet material there is, so it can be cut into equal chunks for the agreement test
    for (auto& c : ch_) { for (int i = 0; i < 2; ++i) { c.hpS1[i] = 0.0; c.hpS2[i] = 0.0; } c.p = 0.0; c.warm = 0; c.blockCount = 0; c.blockMin = 1e300; c.gated = 0; }
    m1_ = 0; m2_ = 0;
    freezeFloor_ = true; hardGate_ = true;
    for (std::uint32_t i = 0; i < frames; i += kBlock) process(l.data() + i, r.data() + i, std::min(kBlock, frames - i));
    for (auto& c : ch_) { c.gatedTotal = c.gated; c.gated = 0; }
    // pass B: learn only from moments close to that floor; louder moments are ignored completely
    for (auto& c : ch_) { for (int i = 0; i < 2; ++i) { c.hpS1[i] = 0.0; c.hpS2[i] = 0.0; } c.p = 0.0; c.warm = 0; c.blockCount = 0; c.blockMin = 1e300; }
    m1_ = 0; m2_ = 0;
    std::copy(left, left + frames, l.begin()); std::copy(right, right + frames, r.begin());
    freezeFloor_ = true; hardGate_ = true;
    learning_.store(true);
    agree_ = true;
    for (std::uint32_t i = 0; i < frames; i += kBlock) process(l.data() + i, r.data() + i, std::min(kBlock, frames - i));
    freezeFloor_ = false; hardGate_ = false; infiniteMemory_ = false;
    (void)wasLearning;
    refreshSamples_ = saved;
    finishRefreshNow();
    agree_ = false;
    learning_.store(false);          // offline use: the pattern stays exactly as learned from the quiet material (setLearning(true) re-enables)
    // rewind the clock and the audio path, keep what was learned
    m1_ = 0; m2_ = 0;
    for (auto& c : ch_) { for (int i = 0; i < 2; ++i) { c.hpS1[i] = 0.0; c.hpS2[i] = 0.0; } c.p = 0.0; c.warm = 0; c.blockCount = 0; c.blockMin = 1e300; }
    stage_ = 0; workCredit_ = 0.0; sinceRefresh_ = 0;
    snap_ = snapshot();
    mixR_.cur = mixR_.target = snap_.mix;
    byR_.cur = byR_.target = snap_.bypass ? 1.0f : 0.0f;
    listenR_.cur = listenR_.target = snap_.listen ? 1.0f : 0.0f;
}

} // namespace hc::hiss
