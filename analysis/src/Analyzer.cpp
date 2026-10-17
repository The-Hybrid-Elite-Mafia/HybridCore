#include "hybrid/analysis/Analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace hc::analysis {

namespace {
constexpr double kHearDb      = -70.0;   // below this a sample counts as silence
constexpr double kCorrTau     = 0.3;     // s
constexpr double kCorrAvgTau  = 3.0;
constexpr double kBandTau     = 3.0;
constexpr double kHoldSeconds = 2.0;
constexpr double kPeakFallDbS = 20.0;

double toLufs(double e) { return e > 0.0 ? -0.691 + 10.0 * std::log10(e) : Analyzer::kSilentLufs; }

std::string mmss(double s) {
    if (s < 0.0) s = 0.0;
    const int t = static_cast<int>(s);
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
} // namespace

float Analyzer::toDb(float lin) noexcept { return lin > 1.0e-6f ? 20.0f * std::log10(lin) : -120.0f; }
float Analyzer::toDbPow(double p) noexcept { return p > 1.0e-14 ? static_cast<float>(10.0 * std::log10(p)) : -140.0f; }

float Analyzer::pointHz(int i) noexcept {
    return 20.0f * std::pow(1000.0f, static_cast<float>(i) / static_cast<float>(kPoints - 1));
}

Analyzer::Analyzer(double sampleRate) : sr_(sampleRate), fft_(kFftSize) {
    window_.resize(kFftSize);
    double sum = 0.0;
    for (int i = 0; i < kFftSize; ++i) {
        window_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize));
        sum += window_[static_cast<std::size_t>(i)];
    }
    windowNorm_ = static_cast<float>(2.0 / sum);
    hist_.resize(kFftSize);
    buf_.resize(kFftSize);
    rows_.assign(static_cast<std::size_t>(kRows) * kPoints, kFloorDb);
    pending_.fill(kFloorDb);
    const double binHz = sr_ / kFftSize, half = std::pow(1000.0, 0.5 / (kPoints - 1));   // half a point step
    for (int i = 0; i < kPoints; ++i) {
        const double f = pointHz(i);
        int lo = static_cast<int>(std::floor(f / half / binHz)), hi = static_cast<int>(std::ceil(f * half / binHz));
        lo = std::clamp(lo, 1, kFftSize / 2);
        hi = std::clamp(hi, lo, kFftSize / 2);
        binLo_[static_cast<std::size_t>(i)] = lo;
        binHi_[static_cast<std::size_t>(i)] = hi;
    }

    // K-weighting (BS.1770): high shelf + RLB high-pass, designed for this rate.
    for (auto& ch : kw_) {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(std::numbers::pi * f0 / sr_), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            ch[0].b0 = (Vh + Vb * K / Q + K * K) / a0;
            ch[0].b1 = 2.0 * (K * K - Vh) / a0;
            ch[0].b2 = (Vh - Vb * K / Q + K * K) / a0;
            ch[0].a1 = 2.0 * (K * K - 1.0) / a0;
            ch[0].a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(std::numbers::pi * f0 / sr_), a0 = 1.0 + K / Q + K * K;
            ch[1].b0 = 1.0; ch[1].b1 = -2.0; ch[1].b2 = 1.0;
            ch[1].a1 = 2.0 * (K * K - 1.0) / a0;
            ch[1].a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    quarterLen_ = static_cast<std::size_t>(std::llround(0.1 * sr_));
    corrK_ = std::exp(-1.0 / (kCorrTau * sr_));
    blocks_.reserve(36000);   // one hour
}

void Analyzer::reset() {
    std::fill(hist_.begin(), hist_.end(), StereoFrame{});
    histPos_ = sinceHop_ = filled_ = 0;
    std::fill(rows_.begin(), rows_.end(), kFloorDb);
    pending_.fill(kFloorDb);
    pendingCount_ = 0;
    newest_ = 0;
    rowsMade_ = 0;
    bandPow_.fill(0.0);
    bandSum_.fill(0.0);
    bandHops_ = 0;
    for (auto& ch : kw_)
        for (auto& b : ch) b.z1 = b.z2 = 0.0;
    quarterSum_ = 0.0;
    quarterFill_ = quarterCount_ = 0;
    quarters_.fill(0.0);
    momentary_ = shortTerm_ = integrated_ = kSilentLufs;
    blocks_.clear();
    integratedAt_ = 0;
    peakNow_ = peakHold_ = peakMax_ = 0.0f;
    holdAge_ = 0.0;
    clipped_ = 0;
    lastClipAt_ = -1.0;
    sLR_ = sLL_ = sRR_ = 0.0;
    corrNow_ = corrAvg_ = 0.0f;
    heard_ = 0.0;
    quietFor_ = 1.0e12;
}

void Analyzer::add(const StereoFrame* frames, std::size_t n, double songSeconds) {
    const float hear = static_cast<float>(std::pow(10.0, kHearDb / 20.0));
    for (std::size_t i = 0; i < n; ++i) {
        const float l = frames[i].l, r = frames[i].r;
        const float a = std::max(std::fabs(l), std::fabs(r));
        peakNow_ = std::max(peakNow_, a);
        if (a > peakHold_) { peakHold_ = a; holdAge_ = 0.0; }
        peakMax_ = std::max(peakMax_, a);
        if (std::fabs(l) >= 1.0f) { ++clipped_; lastClipAt_ = songSeconds; }
        if (std::fabs(r) >= 1.0f) { ++clipped_; lastClipAt_ = songSeconds; }
        if (a > hear) { heard_ += 1.0; quietFor_ = 0.0; } else { quietFor_ += 1.0; }

        sLR_ = sLR_ * corrK_ + static_cast<double>(l) * r;
        sLL_ = sLL_ * corrK_ + static_cast<double>(l) * l;
        sRR_ = sRR_ * corrK_ + static_cast<double>(r) * r;

        const double yl = kw_[0][1].run(kw_[0][0].run(l)), yr = kw_[1][1].run(kw_[1][0].run(r));
        quarterSum_ += yl * yl + yr * yr;
        if (++quarterFill_ == quarterLen_) {
            addQuarter(quarterSum_ / static_cast<double>(quarterLen_));
            quarterSum_ = 0.0;
            quarterFill_ = 0;
        }

        hist_[histPos_] = frames[i];
        histPos_ = (histPos_ + 1) % hist_.size();
        filled_ = std::min(filled_ + 1, hist_.size());
        if (++sinceHop_ >= static_cast<std::size_t>(kHop) && filled_ == hist_.size()) {
            sinceHop_ = 0;
            analyseHop();
        }
    }
    const double dt = static_cast<double>(n) / sr_;
    peakNow_ *= static_cast<float>(std::pow(10.0, -kPeakFallDbS * dt / 20.0));
    holdAge_ += dt;
    if (holdAge_ > kHoldSeconds) peakHold_ = peakNow_;
    const double den = std::sqrt(sLL_ * sRR_);
    corrNow_ = den > 1.0e-12 ? static_cast<float>(std::clamp(sLR_ / den, -1.0, 1.0)) : 0.0f;
    if (hearingNow()) corrAvg_ += (corrNow_ - corrAvg_) * static_cast<float>(1.0 - std::exp(-dt / kCorrAvgTau));
}

void Analyzer::addQuarter(double e) {
    quarters_[quarterCount_ % quarters_.size()] = e;
    ++quarterCount_;
    auto meanOfLast = [&](std::size_t k) {
        k = std::min(k, quarterCount_);
        double s = 0.0;
        for (std::size_t j = 0; j < k; ++j) s += quarters_[(quarterCount_ - 1 - j) % quarters_.size()];
        return s / static_cast<double>(k);
    };
    momentary_ = toLufs(meanOfLast(4));
    shortTerm_ = toLufs(meanOfLast(30));
    if (quarterCount_ >= 4) blocks_.push_back(meanOfLast(4));
}

double Analyzer::integratedLufs() const {
    if (integratedAt_ == blocks_.size()) return integrated_;
    integratedAt_ = blocks_.size();
    auto gated = [&](double gate, double& mean) {
        double s = 0.0;
        std::size_t n = 0;
        for (double b : blocks_)
            if (b > 0.0 && toLufs(b) > gate) { s += b; ++n; }
        if (n == 0) return false;
        mean = s / static_cast<double>(n);
        return true;
    };
    double abs = 0.0, rel = 0.0;
    integrated_ = (gated(-70.0, abs) && gated(toLufs(abs) - 10.0, rel)) ? toLufs(rel) : kSilentLufs;
    return integrated_;
}

void Analyzer::analyseHop() {
    const std::size_t N = hist_.size();
    for (std::size_t i = 0; i < N; ++i) {
        const StereoFrame& f = hist_[(histPos_ + i) % N];   // oldest first
        buf_[i] = {0.5f * (f.l + f.r) * window_[i], 0.0f};
    }
    fft_.transform(buf_.data());

    for (int p = 0; p < kPoints; ++p) {
        // Loudest bin of the point, then the true peak between bins (Gaussian interpolation on the
        // log magnitudes of the bin and its neighbours): a sine reads its level wherever it falls.
        int   mb = binLo_[static_cast<std::size_t>(p)];
        float m  = 0.0f;
        for (int b = binLo_[static_cast<std::size_t>(p)]; b <= binHi_[static_cast<std::size_t>(p)]; ++b) {
            const float v = std::abs(buf_[static_cast<std::size_t>(b)]);
            if (v > m) { m = v; mb = b; }
        }
        float db = toDb(m * windowNorm_);
        if (mb > 1 && mb < kFftSize / 2 - 1 && m > 0.0f) {
            const float a = std::abs(buf_[static_cast<std::size_t>(mb - 1)]), c = std::abs(buf_[static_cast<std::size_t>(mb + 1)]);
            if (a > 0.0f && c > 0.0f && a <= m && c <= m) {
                const float la = std::log(a), lb = std::log(m), lc = std::log(c), den = la - 2.0f * lb + lc;
                if (den < -1.0e-6f) {
                    const float off = 0.5f * (la - lc) / den;   // -0.5 .. 0.5 bins
                    db = toDb(std::exp(lb - 0.25f * (la - lc) * off) * windowNorm_);
                }
            }
        }
        db = std::max(kFloorDb, db);
        pending_[static_cast<std::size_t>(p)] = std::max(pending_[static_cast<std::size_t>(p)], db);
    }
    if (++pendingCount_ == 2) {
        newest_ = (newest_ + 1) % kRows;
        std::copy(pending_.begin(), pending_.end(), rows_.begin() + static_cast<std::ptrdiff_t>(newest_) * kPoints);
        pending_.fill(kFloorDb);
        pendingCount_ = 0;
        ++rowsMade_;
    }

    if (hearingNow()) {   // tone balance: power per band (full-scale sine = 0 dB), averaged over ~3 s
        const double binHz = sr_ / static_cast<double>(N), norm = static_cast<double>(windowNorm_) * windowNorm_ * 0.5;
        std::array<double, 3> p{};
        for (std::size_t b = 1; b < N / 2; ++b) {
            const double f = static_cast<double>(b) * binHz;
            if (f < 20.0 || f > 20000.0) continue;
            p[f < 150.0 ? 0 : f < 5000.0 ? 1 : 2] += static_cast<double>(std::norm(buf_[b])) * norm;
        }
        const double k = 1.0 - std::exp(-static_cast<double>(kHop) / sr_ / kBandTau);
        for (int i = 0; i < 3; ++i) {
            bandPow_[static_cast<std::size_t>(i)] += (p[static_cast<std::size_t>(i)] - bandPow_[static_cast<std::size_t>(i)]) * k;
            bandSum_[static_cast<std::size_t>(i)] += p[static_cast<std::size_t>(i)];
        }
        ++bandHops_;
    }
}

std::vector<Analyzer::Check> Analyzer::checks() const {
    std::vector<Check> out;
    char b[160];
    if (heardSeconds() < 3.0) {
        out.push_back({Mark::Note, hearingNow() ? "Listening..." : "Press PLAY: the analyzer listens to what comes out of MAIN."});
        return out;
    }

    // Loudness: whole play so far once there is enough, else the last 3 s.
    const double whole = integratedLufs();
    const bool   enough = heardSeconds() >= 10.0 && whole > kSilentLufs;
    const double L = enough ? whole : shortTerm_;
    const char*  over = enough ? "" : " (last 3 s)";
    if (L <= kSilentLufs) {
        out.push_back({Mark::Note, "Loudness: still measuring"});
    } else if (L < -20.0) {
        std::snprintf(b, sizeof b, "Quiet: %.1f LUFS%s. Finished songs are about -14.", L, over);
        out.push_back({Mark::Warn, b});
    } else if (L < -16.0) {
        std::snprintf(b, sizeof b, "A bit quiet: %.1f LUFS%s (finished songs are about -14).", L, over);
        out.push_back({Mark::Note, b});
    } else if (L <= -12.0) {
        std::snprintf(b, sizeof b, "Song level: %.1f LUFS%s.", L, over);
        out.push_back({Mark::Good, b});
    } else if (L <= -9.0) {
        std::snprintf(b, sizeof b, "Loud: %.1f LUFS%s. Streaming turns it down a little.", L, over);
        out.push_back({Mark::Good, b});
    } else {
        std::snprintf(b, sizeof b, "Very loud: %.1f LUFS%s. Streaming turns it down; listen for crunch.", L, over);
        out.push_back({Mark::Warn, b});
    }

    // Peaks.
    const float pk = maxPeakDb();
    if (clipped_ > 0) {
        std::snprintf(b, sizeof b, "Clipping: %llu samples hit the top (last at %s).", static_cast<unsigned long long>(clipped_), mmss(lastClipAt_).c_str());
        out.push_back({Mark::Bad, b});
    } else if (pk > -1.0f) {
        std::snprintf(b, sizeof b, "Peaks reach %.1f dB: can crunch on phones and MP3. Keep them under -1 dB.", pk);
        out.push_back({Mark::Warn, b});
    } else {
        std::snprintf(b, sizeof b, "Peaks safe: highest %.1f dB.", pk);
        out.push_back({Mark::Good, b});
    }

    // Tone balance (rough guide; levels are relative to the mids).
    const float bass = bandAvgDb(0) - bandAvgDb(1), highs = bandAvgDb(2) - bandAvgDb(1);
    if (bass > 14.0f) out.push_back({Mark::Warn, "Bass very heavy: it may boom on big speakers."});
    else if (bass < 0.0f) out.push_back({Mark::Note, "Light on bass."});
    else out.push_back({Mark::Good, "Bass: solid."});
    if (highs < -18.0f) out.push_back({Mark::Note, "Dull: not much top end (hats, air)."});
    else if (highs > 2.0f) out.push_back({Mark::Warn, "Very bright: listen for harsh S and hats."});
    else out.push_back({Mark::Good, "Highs: balanced."});

    // Stereo.
    if (corrAvg_ < 0.0f) out.push_back({Mark::Bad, "Out of phase: parts can vanish on a phone speaker."});
    else if (corrAvg_ < 0.3f) out.push_back({Mark::Warn, "Very wide: check how it sounds in mono."});
    else if (corrAvg_ > 0.97f) out.push_back({Mark::Note, "Almost mono: sounds narrow."});
    else out.push_back({Mark::Good, "Stereo: OK, safe in mono."});
    return out;
}

} // namespace hc::analysis
