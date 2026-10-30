#include "hybrid/hiss/HissLab.hpp"

#include "hybrid/hiss/HissFft.hpp"
#include "hybrid/hiss/HissMeters.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace hc::hiss::lab {
namespace {

constexpr double kPi = std::numbers::pi;
using Cx = std::complex<double>;

// ---- small filters (offline) --------------------------------------------------------------------------
struct Bq {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
Bq makeBq(bool high, double f, double fs, double q = 0.70710678118) {
    Bq b;
    const double w = 2.0 * kPi * std::min(f, 0.49 * fs) / fs, c = std::cos(w), s = std::sin(w), al = s / (2.0 * q), a0 = 1.0 + al;
    if (high) { b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0; }
    else      { b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0;  b.b2 = (1 - c) / 2 / a0; }
    b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0;
    return b;
}
// One-pass band-pass: two high-pass and two low-pass sections (4th order each side).
std::vector<double> bandForward(const float* x, std::size_t n, double fs, double lo, double hi) {
    Bq h1 = makeBq(true, lo, fs), h2 = h1, l1 = makeBq(false, hi, fs), l2 = l1;
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) y[i] = l2.run(l1.run(h2.run(h1.run(static_cast<double>(x[i])))));
    return y;
}
std::vector<double> lowpass2(const std::vector<double>& x, double fs, double f) {
    Bq l = makeBq(false, f, fs);
    std::vector<double> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) y[i] = l.run(x[i]);
    return y;
}
// Band power (linear, mean square) as a loudness curve at about outHz. step = samples per curve point.
struct Curve { std::vector<double> p; int step = 1; double dtMs = 0; };
Curve bandCurve(const float* x, std::size_t n, double fs, double lo, double hi, double outHz, double smoothHz) {
    Curve c;
    c.step = std::max(1, static_cast<int>(std::lround(fs / outHz)));
    c.dtMs = 1000.0 * c.step / fs;
    auto y = bandForward(x, n, fs, lo, std::min(hi, 0.45 * fs));
    for (auto& v : y) v *= v;
    y = lowpass2(y, fs, smoothHz);
    for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(c.step)) c.p.push_back(std::max(y[i], 0.0));
    return c;
}
double toDb(double p) { return 10.0 * std::log10(p + 1e-14); }
double percentile(std::vector<double> v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = (static_cast<double>(v.size()) - 1.0) * pct / 100.0;
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx)), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
}
int nextPow2(std::size_t n) { int p = 1; while (static_cast<std::size_t>(p) < n) p <<= 1; return p; }
std::vector<double> hann(int n) {
    std::vector<double> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    return w;
}

} // namespace


// =============================================================================================================
//  statistics (the only place a threshold is worked out)
// =============================================================================================================
namespace stats {
double zUpper(double p) noexcept {
    if (!(p > 0.0)) return 40.0;
    if (p >= 0.5) return 0.0;
    double lo = 0.0, hi = 40.0;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (0.5 * std::erfc(mid / std::numbers::sqrt2) > p) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}
double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}
double robustSigma(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double med = median(v);
    std::vector<double> d(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) d[i] = std::fabs(v[i] - med);
    const double mad = median(d);
    if (mad > 0.0) return 1.4826 * mad;
    double m = 0, q = 0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    for (double x : v) q += (x - m) * (x - m);
    return std::sqrt(q / static_cast<double>(v.size() - 1));
}
double tailBar(const std::vector<double>& nullSample, double neff, double alpha) {
    // Exponential tail from two points inside the sample: its 80th and 90th percentiles (chance 0.2 and 0.1 of being beaten).
    // For an exponential tail, P(X > x) = 0.1 exp(-(x - q90)/lambda) beyond q90, with lambda = (q90 - q80)/ln 2.
    // Using only the 80-90 % range keeps a real signal that fills the top 10 % or less from bending the fit.
    // The result is the level a clean value beats with chance alpha/neff.
    if (nullSample.size() < 100 || !(neff > 0)) return 1e300;
    std::vector<double> v = nullSample;
    std::sort(v.begin(), v.end());
    auto q = [&](double f) { return v[static_cast<std::size_t>(f * static_cast<double>(v.size() - 1))]; };
    const double q80 = q(0.80), q90 = q(0.90), lam = (q90 - q80) / std::log(2.0);
    if (!(lam > 0)) return 1e300;
    return q90 + lam * std::log(0.1 * neff / alpha);
}
double binomTail(int k, int n, double p) noexcept {
    if (k <= 0) return 1.0;
    if (k > n) return 0.0;
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    double sum = 0.0;
    for (int i = k; i <= n; ++i) {
        const double lg = std::lgamma(n + 1.0) - std::lgamma(i + 1.0) - std::lgamma(n - i + 1.0) + i * std::log(p) + (n - i) * std::log1p(-p);
        sum += std::exp(lg);
    }
    return std::min(1.0, sum);
}
} // namespace stats

// =============================================================================================================
//  8: seam clicks
// =============================================================================================================
SeamResult seamClicks(const float* x, std::size_t n, double fs, double periodMs) {
    SeamResult r;
    if (n < static_cast<std::size_t>(0.5 * fs)) return r;
    r.valid = true;
    Bq h1 = makeBq(true, cfg::kClickHpHz, fs), h2 = h1;
    std::vector<double> h(n), a(n);
    for (std::size_t i = 0; i < n; ++i) { h[i] = h2.run(h1.run(static_cast<double>(x[i]))); a[i] = std::fabs(h[i]); }

    // ---- the fold test (decides "pattern") ----
    // Two folds are made. (1) ENERGY fold: add up the click-band energy at every position inside a 40 ms frame, over the whole clip.
    // Works for clicks of any sign. (2) SIGNED fold: the same, but with a filter matched to what a single-sample click looks like after the
    // 3 kHz high-pass, and without squaring. If the clicks all point the same way their sizes add up in step, which is much stronger.
    // Each fold is compared with the same fold at other frame lengths (the null). Two tests are run, so each gets half of kAlpha.
    std::vector<double> g;
    double Eh = 0;
    {
        Bq i1 = makeBq(true, cfg::kClickHpHz, fs), i2 = i1;
        std::vector<double> full(8192);
        for (int k = 0; k < 8192; ++k) { full[static_cast<std::size_t>(k)] = i2.run(i1.run(k == 0 ? 1.0 : 0.0)); Eh += full[static_cast<std::size_t>(k)] * full[static_cast<std::size_t>(k)]; }
        double acc = 0; std::size_t L = 0;
        while (L < full.size() && acc < 0.999 * Eh) { acc += full[L] * full[L]; ++L; }
        g.assign(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(std::max<std::size_t>(L, 8)));
    }
    double Eg = 0; for (double v : g) Eg += v * v;
    const std::size_t Lg = g.size();
    const std::size_t W = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(cfg::kClickWindowMs * 0.001 * fs)));
    const std::size_t nv = n > W + Lg ? n - W - Lg : 0;                 // positions that fit inside the clip
    std::vector<double> e(nv, 0.0), mf(nv, 0.0);
    if (nv > 0) {
        double run = 0;
        for (std::size_t i = 0; i < W; ++i) run += h[i] * h[i];
        for (std::size_t i = 0; i < nv; ++i) { e[i] = run; run += h[i + W] * h[i + W] - h[i] * h[i]; }
        for (std::size_t i = 0; i < nv; ++i) { double acc = 0; for (std::size_t j = 0; j < Lg; ++j) acc += g[j] * h[i + j]; mf[i] = acc / Eg; }
    }
    // statistic of one fold length: energy fold -> (largest - median) of the mean energy per position; signed fold -> largest |mean| per position
    auto foldStat = [&](const std::vector<double>& arr, std::size_t P, bool isSigned, std::vector<double>* keep) {
        std::vector<double> S(P, 0.0), cnt(P, 0.0);
        for (std::size_t i = 0; i < nv; ++i) { const std::size_t q = i % P; S[q] += arr[i]; cnt[q] += 1.0; }
        for (std::size_t q = 0; q < P; ++q) S[q] = cnt[q] > 0 ? S[q] / cnt[q] : 0.0;
        if (keep) *keep = S;
        if (isSigned) { double mx = 0; for (double v : S) mx = std::max(mx, std::fabs(v)); return mx; }
        return *std::max_element(S.begin(), S.end()) - stats::median(S);
    };
    const std::size_t P0 = static_cast<std::size_t>(std::lround(periodMs * 0.001 * fs));
    if (P0 >= 8 && nv >= 5 * P0) {
        std::vector<double> SE, SS;
        const double ME = foldStat(e, P0, false, &SE), MS = foldStat(mf, P0, true, &SS);
        // null: other frame lengths (75 % to 125 % of 40 ms, but not within 3 % of it). How many is set by kAlpha.
        const int J = static_cast<int>(std::ceil(3.0 / cfg::kAlpha)) - 1;
        std::vector<double> nE, nS;
        const int cand = static_cast<int>(std::ceil(J / 0.94)) + 2;
        for (int c = 0; c < cand && static_cast<int>(nE.size()) < J; ++c) {
            const double ratio = 0.75 + 0.5 * (c + 0.5) / cand;
            if (std::fabs(ratio - 1.0) < 0.03) continue;
            const std::size_t Pn = static_cast<std::size_t>(std::lround(ratio * static_cast<double>(P0)));
            if (Pn < 8) continue;
            nE.push_back(foldStat(e, Pn, false, nullptr));
            nS.push_back(foldStat(mf, Pn, true, nullptr));
        }
        r.nullFolds = static_cast<int>(nE.size());
        int exE = 0, exS = 0;
        for (double v : nE) if (v >= ME) ++exE;
        for (double v : nS) if (v >= MS) ++exS;
        const double pE = (1.0 + exE) / (1.0 + static_cast<double>(nE.size())), pS = (1.0 + exS) / (1.0 + static_cast<double>(nS.size()));
        r.foldP = std::min(1.0, 2.0 * std::min(pE, pS));
        r.pattern = r.foldP <= cfg::kAlpha;
        r.foldSigned = pS <= pE;
        const std::vector<double>& SB = r.foldSigned ? SS : SE;
        std::size_t pk = 0;
        if (r.foldSigned) { for (std::size_t q = 0; q < SB.size(); ++q) if (std::fabs(SB[q]) > std::fabs(SB[pk])) pk = q; }
        else pk = static_cast<std::size_t>(std::max_element(SB.begin(), SB.end()) - SB.begin());
        r.foldPhaseMs = 1000.0 * static_cast<double>(pk) / fs;
        {   // picture of the winning fold for the screen: how far each position stands above the middle one, max-pooled to at most 1000 bins
            std::vector<double> mag(SB.size());
            for (std::size_t q = 0; q < SB.size(); ++q) mag[q] = r.foldSigned ? std::fabs(SB[q]) : SB[q];
            const double base = r.foldSigned ? 0.0 : stats::median(mag);   // same units as the null: signed = largest |mean|, energy = largest minus median
            const std::size_t bins = std::min<std::size_t>(1000, mag.size());
            r.foldProfile.assign(bins, 0.0f);
            for (std::size_t q = 0; q < mag.size(); ++q) { const std::size_t b = q * bins / mag.size(); r.foldProfile[b] = std::max(r.foldProfile[b], static_cast<float>(std::max(mag[q] - base, 0.0))); }
            r.foldProfileMs = 1000.0 * static_cast<double>(P0) / fs;
        }
        r.foldLevelDb = r.foldSigned ? 20.0 * std::log10(std::max(std::fabs(SB[pk]), 1e-12)) : (ME > 0 ? 10.0 * std::log10(ME / Eh) : -200.0);
        std::sort(nE.begin(), nE.end()); std::sort(nS.begin(), nS.end());
        const std::size_t qi = std::min(nE.size() - 1, static_cast<std::size_t>(std::ceil((1.0 - cfg::kAlpha / 2.0) * static_cast<double>(nE.size() + 1))) - 1);
        r.detectLimitAnySignDb = nE[qi] > 0 ? 10.0 * std::log10(nE[qi] / Eh) : -200.0;
        r.detectLimitDb = nS[qi] > 0 ? 20.0 * std::log10(nS[qi]) : -200.0;
        r.foldBar = static_cast<float>(r.foldSigned ? nS[qi] : nE[qi]);
    }

    // ---- single clicks (information: where they sit). The size rule comes from the clip length, assuming the local noise is bell-shaped. ----
    const double zc = stats::zUpper(cfg::kAlpha / (2.0 * static_cast<double>(n)));
    r.clickThresholdRatio = zc / 0.67449;           // median of |bell-shaped noise| is 0.67449 sigma
    const std::size_t win = std::max<std::size_t>(8, static_cast<std::size_t>(cfg::kClickLocalMs * 0.001 * fs));
    const std::size_t grid = std::max<std::size_t>(1, static_cast<std::size_t>(0.0005 * fs));
    std::vector<double> typical((n + grid - 1) / grid, 0.0), tmp;
    for (std::size_t g = 0; g < typical.size(); ++g) {
        const std::size_t c = g * grid, lo = c > win / 2 ? c - win / 2 : 0, hi = std::min(n, c + win / 2);
        tmp.assign(a.begin() + static_cast<std::ptrdiff_t>(lo), a.begin() + static_cast<std::ptrdiff_t>(hi));
        const std::size_t mid = tmp.size() / 2;
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(mid), tmp.end());
        typical[g] = tmp[mid];
    }
    const std::size_t refractory = static_cast<std::size_t>(0.004 * fs);
    std::size_t i = 0;
    while (i < n) {
        const double t = std::max(typical[i / grid], cfg::kClickAbsFloor);
        if (a[i] >= r.clickThresholdRatio * t && a[i] > cfg::kClickAbsFloor) {
            std::size_t pk = i, end = std::min(n, i + static_cast<std::size_t>(0.001 * fs) + 1);
            for (std::size_t j = i; j < end; ++j) if (a[j] > a[pk]) pk = j;
            r.clicks.push_back({1000.0 * static_cast<double>(pk) / fs, 20.0 * std::log10(a[pk] / t)});
            i = pk + refractory;
        } else ++i;
    }
    if (r.clicks.size() >= 2) {
        std::vector<double> gaps;
        for (std::size_t k = 1; k < r.clicks.size(); ++k) gaps.push_back(r.clicks[k].timeMs - r.clicks[k - 1].timeMs);
        r.medianIntervalMs = percentile(gaps, 50.0);
        int ok = 0;
        for (double g : gaps) {
            const double m = std::round(g / periodMs);
            if (m >= 1.0 && m <= 8.0 && std::fabs(g - m * periodMs) <= cfg::kClickPeriodTolMs) ++ok;
        }
        r.periodicShare = static_cast<double>(ok) / static_cast<double>(gaps.size());
    }
    return r;
}

// =============================================================================================================
//  9: moving notch
// =============================================================================================================
NotchResult trackNotch(const float* x, std::size_t n, double fs) {
    NotchResult r;
    const int N = cfg::kNotchFft, hop = N / 2;
    if (n < static_cast<std::size_t>(N + (cfg::kNotchAvgFrames) * hop)) return r;
    const int frames = 1 + static_cast<int>((n - static_cast<std::size_t>(N)) / static_cast<std::size_t>(hop));
    const int bins = N / 2 + 1;
    const double df = fs / N;
    const int b0 = static_cast<int>(std::ceil(cfg::kNotchBandLoHz / df)), b1 = std::min(bins - 1, static_cast<int>(std::floor(std::min(cfg::kNotchBandHiHz, 0.45 * fs) / df)));
    if (b1 - b0 < 40) return r;
    const auto w = hann(N);
    FftD fft; fft.prepare(N);
    std::vector<Cx> buf(static_cast<std::size_t>(N));
    const int wide = std::max(2, static_cast<int>(std::lround(cfg::kNotchWideHz / df))), narrow = std::max(1, static_cast<int>(std::lround(cfg::kNotchNarrowHz / df)));
    std::vector<double> acc(static_cast<std::size_t>(bins), 0.0);
    std::vector<std::vector<double>> allDepths;
    int have = 0;
    auto flush = [&](double tSec) {
        // acc holds summed power over `have` frames
        std::vector<double> db(static_cast<std::size_t>(bins), -200.0);
        for (int b = b0; b <= b1; ++b) db[static_cast<std::size_t>(b)] = 10.0 * std::log10(acc[static_cast<std::size_t>(b)] / have + 1e-30);
        // moving average of POWER (then to dB) by running sums. Averaging in power keeps the "nothing there" spread nearly symmetric
        // (averaging dB values would make dips rarer-and-deeper than bumps, which breaks the bumps-as-null rule below).
        std::vector<double> cs(static_cast<std::size_t>(bins) + 1, 0.0);
        for (int b = 0; b < bins; ++b) cs[static_cast<std::size_t>(b) + 1] = cs[static_cast<std::size_t>(b)] + (b >= b0 && b <= b1 ? acc[static_cast<std::size_t>(b)] / have : 0.0);
        auto avg = [&](int c, int hw) {
            const int lo = std::max(b0, c - hw), hi = std::min(b1, c + hw);
            return 10.0 * std::log10((cs[static_cast<std::size_t>(hi) + 1] - cs[static_cast<std::size_t>(lo)]) / (hi - lo + 1) + 1e-30);
        };
        std::vector<double> win;
        auto med = [&](int c, int hw) {       // normal level = MEDIAN of the wide window, so the dip itself does not pull it down
            win.assign(db.begin() + (std::max(b0, c - hw)), db.begin() + (std::min(b1, c + hw) + 1));
            std::nth_element(win.begin(), win.begin() + static_cast<std::ptrdiff_t>(win.size() / 2), win.end());
            return win[win.size() / 2];
        };
        double bestDepth = -1e9; int bestBin = b0;
        std::vector<double> depths;
        for (int b = b0 + wide; b <= b1 - wide; ++b) {
            const double depth = med(b, wide) - avg(b, narrow);
            depths.push_back(depth);
            if (depth > bestDepth) { bestDepth = depth; bestBin = b; }
        }
        allDepths.push_back(std::move(depths));
        const bool sig = false;     // decided after all groups are in (see below)
        r.track.push_back({tSec, bestBin * df, bestDepth, sig});
        std::fill(acc.begin(), acc.end(), 0.0);
        have = 0;
    };
    const double sw = [&] { double s = 0; for (double v : w) s += v; return s; }();
    for (int f = 0; f < frames; ++f) {
        const float* src = x + static_cast<std::size_t>(f) * static_cast<std::size_t>(hop);
        for (int i = 0; i < N; ++i) buf[static_cast<std::size_t>(i)] = Cx(src[i] * w[static_cast<std::size_t>(i)], 0.0);
        fft.forward(buf.data());
        for (int b = b0; b <= b1; ++b) acc[static_cast<std::size_t>(b)] += std::norm(buf[static_cast<std::size_t>(b)]) * 2.0 / (sw * sw);
        if (++have == cfg::kNotchAvgFrames) flush((static_cast<double>(f) - cfg::kNotchAvgFrames / 2.0 + 0.5) * hop / fs + N / (2.0 * fs));
    }
    if (r.track.empty()) return r;
    r.valid = true;
    // Decide which readings hold a real dip. The depth of a dip is (normal level - level over 30 Hz). Across all bins and readings of
    // the clip, the top 10 % tail of that depth is fitted with an exponential tail (the standard peaks-over-threshold method), and the bar
    // is the depth a clean reading beats only 1 time in 100 over the independent places in the band. A notch covers a small share of
    // the bins, so it barely moves a fit made from the top 10 %.
    {
        std::vector<double> pooled;
        for (auto& g : allDepths) pooled.insert(pooled.end(), g.begin(), g.end());
        const double med = stats::median(pooled);
        const double neff = std::max(1.0, static_cast<double>(allDepths.front().size()) / (2.0 * narrow + 1.0));
        const double bar = stats::tailBar(pooled, neff, cfg::kAlpha) - med;
        r.depthBarDb = bar;
        for (std::size_t g = 0; g < r.track.size() && g < allDepths.size(); ++g) r.track[g].significant = (r.track[g].depthDb - med) >= bar;
    }
    std::vector<double> d, fr;
    for (auto& p : r.track) { d.push_back(p.depthDb); fr.push_back(p.freqHz); }
    r.medianDepthDb = percentile(d, 50.0);
    r.medianFreqHz = percentile(fr, 50.0);
    r.groups = static_cast<int>(r.track.size());
    for (auto& p : r.track) if (p.significant) ++r.significantGroups;
    r.binomP = stats::binomTail(r.significantGroups, r.groups, cfg::kAlpha);
    r.found = r.binomP <= cfg::kAlpha;
    if (r.found && r.track.size() >= 3) {
        double st = 0, sf = 0, stt = 0, stf = 0;
        const double m = static_cast<double>(r.track.size());
        for (auto& p : r.track) { st += p.timeSec; sf += p.freqHz; stt += p.timeSec * p.timeSec; stf += p.timeSec * p.freqHz; }
        const double den = m * stt - st * st;
        r.driftHzPerSec = std::fabs(den) > 1e-12 ? (m * stf - st * sf) / den : 0.0;
    }
    return r;
}

// =============================================================================================================
//  11 + 13: hits, tails, averaging
// =============================================================================================================
HitResult hitsAndTails(const float* x, std::size_t n, double fs, double bandLoHz, double bandHiHz) {
    HitResult r;
    if (n < static_cast<std::size_t>(fs)) return r;
    const Curve c = bandCurve(x, n, fs, bandLoHz, bandHiHz, cfg::kEnvHz, 150.0);
    const int m = static_cast<int>(c.p.size());
    std::vector<double> db(static_cast<std::size_t>(m));
    for (int i = 0; i < m; ++i) db[static_cast<std::size_t>(i)] = toDb(c.p[static_cast<std::size_t>(i)]);
    r.floorDb = percentile(db, 10.0);
    r.valid = true;
    const int refr = static_cast<int>(cfg::kHitRefractoryMs / c.dtMs), peakWin = static_cast<int>(20.0 / c.dtMs), back = static_cast<int>(10.0 / c.dtMs);
    // A hit is a rise (over 10 ms) that is a rare outlier among all the rises in the clip. The spread of normal rises comes from the
    // clip itself (robust, so the hits do not inflate it); the bar is the chance level for that many independent looks at kAlpha.
    std::vector<double> rises;
    const int settle = std::max(back, static_cast<int>(100.0 / c.dtMs));      // the band filters start from rest: skip their first 100 ms
    for (int i = settle; i < m; ++i) rises.push_back(db[static_cast<std::size_t>(i)] - db[static_cast<std::size_t>(i - back)]);
    // Falls (rises mirrored around the median) hold no hits, so they are the null sample. Hits only make rises.
    const double riseMed = stats::median(rises);
    std::vector<double> falls;
    for (double q : rises) if (q < riseMed) falls.push_back(riseMed - q);
    const double riseSd = falls.size() >= 200 ? 1.0 : 0.0;     // 0 = not enough clip to tell
    r.riseBarDb = riseMed + stats::tailBar(falls, std::max(1.0, static_cast<double>(m) / back), cfg::kAlpha);
    std::vector<int> pk;
    int last = -1000000;
    for (int i = settle; i < m && riseSd > 0; ++i) {
        if (i - last < refr) continue;
        if (db[static_cast<std::size_t>(i)] - db[static_cast<std::size_t>(i - back)] >= r.riseBarDb) {
            int p = i;
            for (int j = i; j < std::min(m, i + peakWin + 1); ++j) if (db[static_cast<std::size_t>(j)] > db[static_cast<std::size_t>(p)]) p = j;
            pk.push_back(p);
            last = p;
            i = p;
        }
    }
    const int tailMax = static_cast<int>(cfg::kHitTailMaxMs / c.dtMs);
    std::vector<double> tails;
    for (std::size_t h = 0; h < pk.size(); ++h) {
        Hit hit;
        hit.timeMs = pk[h] * c.dtMs;
        hit.peakDb = db[static_cast<std::size_t>(pk[h])];
        const int stop = std::min(h + 1 < pk.size() ? pk[h + 1] : m, pk[h] + tailMax);
        for (int j = pk[h]; j < stop; ++j)
            if (db[static_cast<std::size_t>(j)] <= hit.peakDb - cfg::kHitTailDb) { hit.tailMs = (j - pk[h]) * c.dtMs; tails.push_back(hit.tailMs); break; }
        r.hits.push_back(hit);
    }
    r.counted = static_cast<int>(tails.size());
    r.medianTailMs = percentile(tails, 50.0);
    return r;
}

AverageResult averageHits(const float* x, std::size_t n, double fs, double bandLoHz, double bandHiHz) {
    AverageResult r;
    const HitResult hr = hitsAndTails(x, n, fs, bandLoHz, bandHiHz);
    if (!hr.valid) return r;
    const Curve c = bandCurve(x, n, fs, bandLoHz, bandHiHz, cfg::kEnvHz, 150.0);
    const int m = static_cast<int>(c.p.size()), pre = static_cast<int>(cfg::kAvgPreMs / c.dtMs), post = static_cast<int>(cfg::kAvgPostMs / c.dtMs);
    r.stepMs = c.dtMs; r.startMs = -pre * c.dtMs;
    std::vector<double> sum(static_cast<std::size_t>(pre + post + 1), 0.0);
    int used = 0;
    for (std::size_t h = 0; h < hr.hits.size(); ++h) {
        const int p = static_cast<int>(std::lround(hr.hits[h].timeMs / c.dtMs));
        const int next = h + 1 < hr.hits.size() ? static_cast<int>(std::lround(hr.hits[h + 1].timeMs / c.dtMs)) : m + post + 1;
        if (p - pre < 0 || p + post >= m || next - p <= post) continue;    // window must be inside the clip and free of other hits
        const double pp = std::max(c.p[static_cast<std::size_t>(p)], 1e-14);
        for (int k = -pre; k <= post; ++k) sum[static_cast<std::size_t>(k + pre)] += c.p[static_cast<std::size_t>(p + k)] / pp;
        ++used;
    }
    r.hits = used;
    if (used < 3) return r;
    r.valid = true;
    r.profileDb.resize(sum.size());
    for (std::size_t i = 0; i < sum.size(); ++i) r.profileDb[i] = toDb(sum[i] / used);
    for (std::size_t i = static_cast<std::size_t>(pre); i < sum.size(); ++i) {
        const double ms = (static_cast<double>(i) - pre) * c.dtMs;
        if (r.tail20Ms < 0 && r.profileDb[i] <= -20.0) r.tail20Ms = ms;
        if (r.tail30Ms < 0 && r.profileDb[i] <= -30.0) r.tail30Ms = ms;
    }
    return r;
}

// =============================================================================================================
//  14: echo / repeat
// =============================================================================================================
EchoResult echoCheck(const float* x, std::size_t n, double fs, double bandLoHz, double bandHiHz) {
    EchoResult r;
    if (n < static_cast<std::size_t>(2.0 * fs)) return r;
    const Curve c = bandCurve(x, n, fs, bandLoHz, bandHiHz, cfg::kEnvHz, 150.0);
    const std::size_t m = c.p.size();
    std::vector<double> nov(m, 0.0);
    double prev = std::sqrt(c.p[0]);
    for (std::size_t i = 1; i < m; ++i) { const double a = std::sqrt(c.p[i]); nov[i] = std::max(0.0, a - prev); prev = a; }
    // autocorrelation of a piece of the novelty curve, normalised; returns q[lag] for lag in [lo, hi]
    auto auto_corr = [&](std::size_t from, std::size_t len, int lo, int hi, std::vector<double>& q) {
        const int N = nextPow2(2 * len);
        FftD fft; fft.prepare(N);
        std::vector<Cx> v(static_cast<std::size_t>(N), Cx{});
        for (std::size_t i = 0; i < len; ++i) v[i] = Cx(nov[from + i], 0.0);
        fft.forward(v.data());
        for (auto& z : v) z = Cx(std::norm(z), 0.0);
        fft.inverse(v.data());
        const double r0 = v[0].real();
        q.clear();
        if (r0 <= 1e-30) return false;
        // Every hit correlates with its own shape (rise and tail), which makes a smooth hill at short lags in ANY clip. Take that hill away
        // (median of the neighbouring lags, 50 ms each side, taken from lags a window wider than the search range so the edges are not bent).
        const int hw = std::max(2, static_cast<int>(50.0 / c.dtMs));
        const int ra = std::max(1, lo - hw), rb = std::min(static_cast<int>(len) - 1, hi + hw);
        std::vector<double> raw, win;
        for (int l = ra; l <= rb; ++l) raw.push_back(v[static_cast<std::size_t>(l)].real() / r0);
        for (int l = lo; l <= hi; ++l) {
            const std::size_t ci = static_cast<std::size_t>(l - ra);
            const std::size_t w0 = ci > static_cast<std::size_t>(hw) ? ci - static_cast<std::size_t>(hw) : 0, w1 = std::min(raw.size(), ci + static_cast<std::size_t>(hw) + 1);
            win.assign(raw.begin() + static_cast<std::ptrdiff_t>(w0), raw.begin() + static_cast<std::ptrdiff_t>(w1));
            q.push_back(raw[ci] - stats::median(win));
        }
        return true;
    };
    int lo = static_cast<int>(cfg::kEchoMinMs / c.dtMs), hi = std::min(static_cast<int>(cfg::kEchoMaxMs / c.dtMs), static_cast<int>(m) / 2);
    // How many separate parts of the clip must agree on the lag: the number where agreement by luck is rarer than kAlpha.
    // One part picks a lag; another part lands within +-1 step of it by luck with chance about 3/L (L = number of lags tried).
    const double L0 = std::max(4.0, static_cast<double>(hi - lo + 1));
    int K = static_cast<int>(std::ceil(1.0 + std::log(cfg::kAlpha) / std::log(std::min(0.99, 3.0 / L0))));
    K = std::max(K, 2);
    const std::size_t seg = m / static_cast<std::size_t>(K);
    hi = std::min(hi, static_cast<int>(seg) / 2 - 1);      // a part must be long enough to hold the longest lag twice
    if (hi <= lo + 4) return r;                             // clip too short for the replicated test: refuse
    r.valid = true;
    const double L = static_cast<double>(hi - lo + 1);
    K = std::max(2, static_cast<int>(std::ceil(1.0 + std::log(cfg::kAlpha) / std::log(std::min(0.99, 3.0 / L)))));
    std::vector<double> qAll;
    if (!auto_corr(0, m, lo, hi, qAll)) return r;
    int best = 0; double bv = -1;
    std::vector<double> mags;
    for (std::size_t i = 0; i < qAll.size(); ++i) { mags.push_back(std::fabs(qAll[i])); if (qAll[i] > bv) { bv = qAll[i]; best = static_cast<int>(i); } }
    const double typical = std::max(percentile(mags, 50.0), 1e-9);
    r.lagMs = (lo + best) * c.dtMs;
    r.contrast = bv / typical;
    const double qc = std::clamp(bv, 0.0, 0.499);      // ratio = g / (1 + g^2)  ->  g = (1 - sqrt(1 - 4 ratio^2)) / (2 ratio)
    r.strength = qc > 1e-6 ? (1.0 - std::sqrt(1.0 - 4.0 * qc * qc)) / (2.0 * qc) : 0.0;
    // Size test: after the smooth hill is taken away, dips and bumps of the correlation look alike when nothing repeats. The dips are the
    // "nothing there" sample; the bar is the height a clean clip beats 1 time in 100 over the independent lags (lag-to-lag correlation sets how many).
    {
        std::vector<double> neg;
        double s11 = 0, s01 = 0;
        for (std::size_t i = 0; i < qAll.size(); ++i) { if (qAll[i] < 0) neg.push_back(-qAll[i]); s11 += qAll[i] * qAll[i]; if (i + 1 < qAll.size()) s01 += qAll[i] * qAll[i + 1]; }
        const double rho = s11 > 0 ? std::clamp(s01 / s11, 0.0, 0.95) : 0.0;
        const double neffLags = std::max(1.0, L * (1.0 - rho) / (1.0 + rho));
        r.peakBar = stats::tailBar(neg, neffLags, cfg::kAlpha);
    }
    // replication
    const std::size_t sl = m / static_cast<std::size_t>(K);
    r.segments = K;
    for (int k = 0; k < K; ++k) {
        std::vector<double> q;
        if (!auto_corr(static_cast<std::size_t>(k) * sl, sl, lo, hi, q)) continue;
        int bi = 0; double bq = -1;
        for (std::size_t i = 0; i < q.size(); ++i) if (q[i] > bq) { bq = q[i]; bi = static_cast<int>(i); }
        if (std::abs(bi - best) <= 1) ++r.agreeing;
    }
    r.chanceAllAgree = std::pow(std::min(1.0, 3.0 / L), static_cast<double>(r.segments - 1));
    r.found = r.agreeing == r.segments && r.chanceAllAgree <= cfg::kAlpha && bv >= r.peakBar;
    return r;
}

// =============================================================================================================
//  15: high band wobble
// =============================================================================================================
WobbleResult highBandWobble(const float* x, std::size_t n, double fs, double bandLoHz, double bandHiHz) {
    WobbleResult r;
    if (n < static_cast<std::size_t>(cfg::kWobbleMinSeconds * fs)) return r;
    const Curve c = bandCurve(x, n, fs, bandLoHz, bandHiHz, cfg::kWobbleEnvHz, 80.0);
    const std::size_t m = c.p.size();
    std::vector<double> a(m), z(m);
    double mean = 0;
    for (std::size_t i = 0; i < m; ++i) { a[i] = std::sqrt(c.p[i]); mean += a[i]; }
    mean /= static_cast<double>(m);
    if (mean <= 1e-9) return r;     // band empty
    for (std::size_t i = 0; i < m; ++i) z[i] = a[i] / mean - 1.0;
    const double rate = cfg::kWobbleEnvHz;

    // amplitude spectrum of the loudness curve (Hann window, amplitude of a sine = its depth)
    const int Nf = nextPow2(m);
    FftD fft; fft.prepare(Nf);
    const auto wv = hann(static_cast<int>(m));
    double wsum = 0; for (double v : wv) wsum += v;
    const double df = rate / Nf;
    const int lo = std::max(1, static_cast<int>(std::ceil(cfg::kWobbleMinHz / df))), hi = std::min(Nf / 2 - 1, static_cast<int>(std::floor(cfg::kWobbleMaxHz / df)));
    std::vector<Cx> buf(static_cast<std::size_t>(Nf));
    auto spectrum = [&](const std::vector<double>& sig, std::vector<double>& amp) {
        double mu = 0; for (double v : sig) mu += v; mu /= static_cast<double>(sig.size());
        std::fill(buf.begin(), buf.end(), Cx{});
        for (std::size_t i = 0; i < m; ++i) buf[i] = Cx((sig[i] - mu) * wv[i], 0.0);
        fft.forward(buf.data());
        amp.assign(static_cast<std::size_t>(hi) + 2, 0.0);
        for (int b = 0; b <= hi + 1; ++b) amp[static_cast<std::size_t>(b)] = 2.0 * std::abs(buf[static_cast<std::size_t>(b)]) / wsum;
    };
    std::vector<double> amp;
    spectrum(z, amp);

    // Is a line real? Compare it with the loudness-curve spectrum at nearby rates (1-10 Hz away, not at its multiples): that is what
    // "nothing there" looks like in THIS clip, beat and wander included. With bell-shaped noise the strength of one rate follows a
    // Rayleigh law, so the chance of a value this big at one rate is exp(-A^2 / s2). We tried (range x seconds) independent rates,
    // so the bar is raised by that count (Bonferroni). A beat that is loud in this clip raises s2 and hides a small wobble; the limit
    // below says how small.
    const double Tsec = static_cast<double>(m) / rate;
    const double nRates = std::max(1.0, (cfg::kWobbleMaxHz - cfg::kWobbleMinHz) * Tsec);
    auto ampNoise = [&](double centre, double f) {
        std::vector<double> v;
        const double excl = 0.5 * cfg::kWobbleNearMinHz;
        for (double d = cfg::kWobbleNearMinHz; d <= cfg::kWobbleNearMaxHz + 1e-9; d += cfg::kWobbleNearStepHz)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const double g = centre + sgn * d;
                if (g < cfg::kWobbleMinHz || g > cfg::kWobbleMaxHz) continue;
                bool bad = false;
                for (int k = 1; k <= 4; ++k) { if (std::fabs(g - k * f) < excl || (k > 1 && std::fabs(g - f / k) < excl)) bad = true; }
                if (bad) continue;
                const int bi = static_cast<int>(std::lround(g / df));
                if (bi >= 1 && bi <= hi) v.push_back(amp[static_cast<std::size_t>(bi)] * amp[static_cast<std::size_t>(bi)]);
            }
        if (v.size() < 20) return -1.0;
        // median of a Rayleigh-shaped power is ln2 x its mean. The median of N independent values has relative spread 1.44/sqrt(N)
        // (N = span x seconds / 2 for a Hann window), so the estimate is raised by its own one-sided kAlpha uncertainty.
        const double nInd = std::max(1.0, static_cast<double>(v.size()) * cfg::kWobbleNearStepHz * Tsec / 2.0);
        return stats::median(v) / std::log(2.0) * (1.0 + stats::zUpper(cfg::kAlpha) * 1.4427 / std::sqrt(nInd));
    };
    // adjusted chance (<= kAlpha means: a clean clip shows a rate this strong less than 1 time in 100)
    auto pAdj = [&](double A, double centre, double f) {
        const double s2 = ampNoise(centre, f);
        if (s2 <= 0) return 1.0;
        return std::min(1.0, nRates * std::exp(-A * A / s2));
    };

    // candidate lines: local maxima that pass, strongest first, at least 1.5 Hz apart, at most 6
    std::vector<int> peaks;
    for (int b = lo + 1; b < hi; ++b) if (amp[static_cast<std::size_t>(b)] >= amp[static_cast<std::size_t>(b - 1)] && amp[static_cast<std::size_t>(b)] > amp[static_cast<std::size_t>(b + 1)]) peaks.push_back(b);
    std::sort(peaks.begin(), peaks.end(), [&](int p, int q) { return amp[static_cast<std::size_t>(p)] > amp[static_cast<std::size_t>(q)]; });
    std::vector<int> chosen;
    const int sep = std::max(1, static_cast<int>(std::lround(1.5 / df)));
    for (int p : peaks) {
        if (chosen.size() == 6) break;
        const double fp = p * df;
        if (pAdj(amp[static_cast<std::size_t>(p)], fp, fp) > cfg::kAlpha) continue;
        bool near = false;
        for (int q : chosen) if (std::abs(p - q) < sep) near = true;
        if (!near) chosen.push_back(p);
    }
    std::vector<double> typicalAmp;
    for (int b = lo; b <= hi; ++b) typicalAmp.push_back(amp[static_cast<std::size_t>(b)] * amp[static_cast<std::size_t>(b)]);
    const double typicalPow = std::max(stats::median(typicalAmp), 1e-30);
    r.valid = true;
    // the smallest plain wobble this clip could show: amplitude whose adjusted chance equals kAlpha at the clip-wide typical s2
    // The limit depends on the rate (a beat is loud at low rates and quiet at high ones), so it is worked out at every rate from 1 Hz up:
    // the middle one and the worst one are reported.
    std::vector<double> limitsAmp; double worstAmp = 0, worstAt = 0;
    for (double fr = 1.0; fr <= cfg::kWobbleMaxHz; fr += 0.5) {
        const double s2 = ampNoise(fr, fr);
        if (s2 <= 0) continue;
        const double la = std::sqrt(s2 * std::log(nRates / cfg::kAlpha));
        limitsAmp.push_back(la);
        if (la > worstAmp) { worstAmp = la; worstAt = fr; }
    }
    const double limitAmp = limitsAmp.empty() ? 0.0 : stats::median(limitsAmp);

    auto lockIn = [&](const std::vector<double>& zz, double f) {
        Cx acc{};
        for (std::size_t i = 0; i < zz.size(); ++i) acc += zz[i] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(i) / rate);
        return 2.0 * acc / static_cast<double>(zz.size());
    };
    // leakage power at a rate: median over nearby rates (not near f or its multiples) of |lock-in|^2, divided by ln 2 (the median/mean ratio of that spread)
    auto leakPower = [&](double centre, double f) {
        std::vector<double> v;
        const double excl = 0.5 * cfg::kWobbleNearMinHz;
        for (double d = cfg::kWobbleNearMinHz; d <= cfg::kWobbleNearMaxHz + 1e-9; d += cfg::kWobbleNearStepHz)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const double g = centre + sgn * d;
                if (g < cfg::kWobbleMinHz || g > cfg::kWobbleMaxHz) continue;
                bool bad = false;
                for (int k = 1; k <= 4; ++k) { if (std::fabs(g - k * f) < excl || (k > 1 && std::fabs(g - f / k) < excl)) bad = true; }
                if (bad) continue;
                v.push_back(std::norm(lockIn(z, g)));
            }
        return v.size() >= 20 ? stats::median(v) / std::log(2.0) : -1.0;
    };
    const double tFac = std::sqrt(-std::log(cfg::kAlpha));      // 1 - kAlpha bound of a Rayleigh-shaped spread, in units of its sigma
    // add a known wobble of `depth` at f, return how far the reading at f moved per unit depth (0 if it cannot be read)
    auto probeGain = [&](double f, double depth, std::vector<double>* zOut, const Cx& a0) {
        std::vector<float> copy(x, x + n);
        maker::injectBandWobble(copy, fs, f, depth, bandLoHz, std::min(bandHiHz, 0.45 * fs));
        const Curve c2 = bandCurve(copy.data(), n, fs, bandLoHz, bandHiHz, cfg::kWobbleEnvHz, 80.0);
        if (c2.p.size() != m) return 0.0;
        std::vector<double> zz(m);
        double mean2 = 0;
        for (std::size_t i = 0; i < m; ++i) { zz[i] = std::sqrt(c2.p[i]); mean2 += zz[i]; }
        mean2 /= static_cast<double>(m);
        for (std::size_t i = 0; i < m; ++i) zz[i] = zz[i] / mean2 - 1.0;
        const double mv = std::abs(lockIn(zz, f) - a0);
        if (zOut) *zOut = std::move(zz);
        return mv / depth;
    };

    for (int pb : chosen) {
        WobbleLine ln;
        double f = pb * df;
        {
            const double l = std::log(amp[static_cast<std::size_t>(pb - 1)] + 1e-30), cc = std::log(amp[static_cast<std::size_t>(pb)] + 1e-30), rr = std::log(amp[static_cast<std::size_t>(pb + 1)] + 1e-30);
            const double den = l - 2.0 * cc + rr;
            if (std::fabs(den) > 1e-12) f += 0.5 * (l - rr) / den * df;
        }
        ln.freqHz = f;
        ln.contrastDb = 10.0 * std::log10(amp[static_cast<std::size_t>(pb)] * amp[static_cast<std::size_t>(pb)] / typicalPow);
        ln.p = pAdj(amp[static_cast<std::size_t>(pb)], f, f);
        ln.significant = ln.p <= cfg::kAlpha;
        const Cx a0 = lockIn(z, f);
        // standard addition: add a known wobble at this rate, see how far the reading moves, scale (removes the meter's own gain error).
        // Two sizes of added wobble: the reading is slightly sub-linear when a wobble is already there, so the two gains differ a little.
        // Their mean is used and their half-difference widens the range.
        std::vector<double> z2;
        const double gSmall = probeGain(f, cfg::kWobbleCalibSmall, nullptr, a0);
        const double gBig = probeGain(f, cfg::kWobbleCalibBig, &z2, a0);
        if (gSmall <= 0 || gBig <= 0) continue;
        const double gain = 0.5 * (gSmall + gBig), gRel = 0.5 * std::fabs(gSmall - gBig) / gain;
        const double sig2 = std::max(leakPower(f, f), 0.0);
        const double amp0 = std::abs(a0), bound = tFac * std::sqrt(sig2);
        if (gain * cfg::kWobbleCalibSmall < bound) continue;     // the added wobble did not rise above this clip's leakage: no number
        ln.depth = std::sqrt(std::max(amp0 * amp0 - sig2, 0.0)) / gain;
        ln.depthLo = std::max(amp0 - bound, 0.0) / (gain * (1.0 + gRel));
        ln.depthHi = (amp0 + bound) / (gain * (1.0 - gRel));
        ln.depthTrusted = ln.significant && ln.depthLo > 0.0;
        // rhythm: a plain wobble puts a little at 2x and 3x (the meter's own response, measured by the same added wobble). A beat puts clearly more.
        for (int h = 2; h <= 3 && !ln.rhythmLike; ++h) {
            const double fh = h * f;
            if (fh > cfg::kWobbleMaxHz) break;
            const Cx ah = lockIn(z, fh);
            const double predicted = std::abs(lockIn(z2, fh) - ah) * std::pow(std::max(ln.depth, 0.0) / cfg::kWobbleCalibBig, h);
            const double sh = std::max(leakPower(fh, f), 0.0);
            // is the amplitude at fh a line (beats the null) AND more than the pure-wobble prediction plus its own spread?
            if (std::abs(ah) > predicted + tFac * std::sqrt(sh) && pAdj(std::abs(ah), fh, f) <= cfg::kAlpha) ln.rhythmLike = true;
        }
        r.lines.push_back(ln);
    }
    // gain of the meter for the limit: one small added wobble at 10 Hz
    {
        const double g = probeGain(10.0, cfg::kWobbleCalibSmall, nullptr, lockIn(z, 10.0));
        if (g > 0) { r.detectLimitDepth = limitAmp / g; r.detectLimitWorstDepth = worstAmp / g; r.detectLimitWorstHz = worstAt; }
    }
    if (r.lines.empty()) return r;
    const WobbleLine* best = &r.lines[0];
    for (const auto& ln : r.lines) if (ln.significant && !ln.rhythmLike) { best = &ln; break; }
    r.freqHz = best->freqHz; r.depth = best->depth; r.contrastDb = best->contrastDb;
    for (const auto& ln : r.lines) { if (ln.significant) { r.found = true; if (!ln.rhythmLike) r.foundSteady = true; } }
    return r;
}

// =============================================================================================================
//  16: phase consistency
// =============================================================================================================
PhaseResult phaseConsistency(const float* left, const float* right, std::size_t n, double fs) {
    PhaseResult r;
    const int N = cfg::kPhaseFft, hop = N / 2;
    if (n < static_cast<std::size_t>(2 * N)) return r;
    const int frames = 1 + static_cast<int>((n - static_cast<std::size_t>(N)) / static_cast<std::size_t>(hop));
    const double edges[] = {100, 200, 400, 800, 1600, 3200, 6400, 12800, 20000};
    constexpr int kBands = 8;
    const double df = fs / N;
    const auto w = hann(N);
    FftD fft; fft.prepare(N);
    // frames used: all of them, or kPhaseMaxFrames spread evenly across the clip
    const int Fu = std::min(frames, cfg::kPhaseMaxFrames);
    int kLo[kBands], kHi[kBands], nb = 0;
    for (int b = 0; b < kBands; ++b) {
        if (edges[b] >= 0.45 * fs) break;
        kLo[b] = static_cast<int>(std::ceil(edges[b] / df)); kHi[b] = std::min(N / 2, static_cast<int>(std::floor(edges[b + 1] / df)));
        ++nb;
    }
    if (nb == 0) return r;
    const int k0 = kLo[0], k1 = kHi[nb - 1], nk = k1 - k0;
    if (nk <= 0) return r;
    using Cf = std::complex<float>;
    std::vector<Cf> LS(static_cast<std::size_t>(Fu) * static_cast<std::size_t>(nk)), RS(LS.size());
    std::vector<Cx> L(static_cast<std::size_t>(N)), R(static_cast<std::size_t>(N));
    for (int j = 0; j < Fu; ++j) {
        const int f = Fu == frames ? j : static_cast<int>(std::lround(static_cast<double>(j) * (frames - 1) / std::max(1, Fu - 1)));
        const std::size_t o = static_cast<std::size_t>(f) * static_cast<std::size_t>(hop);
        for (int i = 0; i < N; ++i) { L[static_cast<std::size_t>(i)] = Cx(left[o + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0); R[static_cast<std::size_t>(i)] = Cx(right[o + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0); }
        fft.forward(L.data()); fft.forward(R.data());
        for (int k = 0; k < nk; ++k) {
            LS[static_cast<std::size_t>(j) * static_cast<std::size_t>(nk) + static_cast<std::size_t>(k)] = Cf(static_cast<float>(L[static_cast<std::size_t>(k0 + k)].real()), static_cast<float>(L[static_cast<std::size_t>(k0 + k)].imag()));
            RS[static_cast<std::size_t>(j) * static_cast<std::size_t>(nk) + static_cast<std::size_t>(k)] = Cf(static_cast<float>(R[static_cast<std::size_t>(k0 + k)].real()), static_cast<float>(R[static_cast<std::size_t>(k0 + k)].imag()));
        }
    }
    // agreement of L with R paired at a frame shift (0 = the real pairing)
    auto pairing = [&](int shift, double* coh, double* phaseDeg) {
        std::complex<double> cross[kBands] = {};
        double pl[kBands] = {}, pr[kBands] = {};
        for (int j = 0; j < Fu; ++j) {
            const int jr = (j + shift) % Fu;
            for (int b = 0; b < nb; ++b)
                for (int k = kLo[b]; k < kHi[b]; ++k) {
                    const Cf l = LS[static_cast<std::size_t>(j) * static_cast<std::size_t>(nk) + static_cast<std::size_t>(k - k0)];
                    const Cf rr = RS[static_cast<std::size_t>(jr) * static_cast<std::size_t>(nk) + static_cast<std::size_t>(k - k0)];
                    const Cf cp = l * std::conj(rr);
                    cross[b] += std::complex<double>(cp.real(), cp.imag());
                    pl[b] += std::norm(l); pr[b] += std::norm(rr);
                }
        }
        for (int b = 0; b < nb; ++b) {
            const double den = std::sqrt(pl[b] * pr[b]);
            coh[b] = den > 1e-30 ? std::abs(cross[b]) / den : 0.0;
            if (phaseDeg) phaseDeg[b] = std::arg(cross[b]) * 180.0 / kPi;
        }
    };
    double coh0[kBands] = {}, ph0[kBands] = {};
    pairing(0, coh0, ph0);
    // NULL: the same agreement with R slid in time against L (the two channels no longer belong together). Neighbouring frames overlap, so
    // slides closer than 4 frames to the real pairing are not used. The number of slides is set by kAlpha.
    const int S = static_cast<int>(std::ceil(2.0 / cfg::kAlpha)) - 1;
    std::vector<int> shifts;
    const int avail = Fu - 7;
    if (avail > 0) {
        const int take = std::min(S, avail);
        for (int i = 0; i < take; ++i) shifts.push_back(4 + static_cast<int>(std::lround(static_cast<double>(i) * (avail - 1) / std::max(1, take - 1))));
    }
    std::vector<std::vector<double>> nullC(static_cast<std::size_t>(nb));
    for (int sh : shifts) { double cb[kBands] = {}; pairing(sh, cb, nullptr); for (int b = 0; b < nb; ++b) nullC[static_cast<std::size_t>(b)].push_back(cb[b]); }
    const bool enough = static_cast<int>(shifts.size()) >= static_cast<int>(std::ceil(1.0 / cfg::kAlpha)) - 1;
    double sum = 0;
    for (int b = 0; b < nb; ++b) {
        PhaseBand pb; pb.loHz = edges[b]; pb.hiHz = std::min(edges[b + 1], 0.5 * fs);
        pb.coherence = coh0[b]; pb.phaseDeg = ph0[b];
        pb.nullMedian = stats::median(nullC[static_cast<std::size_t>(b)]);
        int ex = 0; for (double v : nullC[static_cast<std::size_t>(b)]) if (v >= coh0[b]) ++ex;
        const double pv = (1.0 + ex) / (1.0 + static_cast<double>(shifts.size()));
        pb.significant = enough && pv <= cfg::kAlpha;
        r.bands.push_back(pb);
        sum += pb.coherence;
    }
    r.valid = nb > 0;
    r.meanCoherence = nb ? sum / nb : 0.0;
    return r;
}

// =============================================================================================================
//  12: masking (ASSUMED model)
// =============================================================================================================
double hzToBark(double hz) noexcept { return 13.0 * std::atan(0.00076 * hz) + 3.5 * std::atan((hz / 7500.0) * (hz / 7500.0)); }

double maskingMarginDb(double artifactDb, double maskerDb, double distanceBark) noexcept {
    // distanceBark > 0: the masker sits BELOW the artifact in frequency (spreads upward: gentle slope). < 0: masker above (steep slope).
    const double slope = distanceBark >= 0 ? cfg::kMaskSlopeHighDb : cfg::kMaskSlopeLowDb;
    const double covered = maskerDb - cfg::kMaskOffsetDb - slope * std::fabs(distanceBark);
    return artifactDb - covered;
}

MaskResult maskingShare(const float* x, std::size_t n, double fs, double artifactLoHz, double artifactHiHz, double artifactDb) {
    MaskResult r;
    const meters::Stft s = meters::stftPower(x, n, fs);
    if (s.frames < cfg::kMinLoudFrames) return r;
    std::vector<double> loudBand(static_cast<std::size_t>(s.frames));
    for (int f = 0; f < s.frames; ++f) loudBand[static_cast<std::size_t>(f)] = s.band(f, cfg::kLoudBandLoHz, cfg::kLoudBandHiHz);
    const auto mask = meters::loudMask(loudBand);
    const double artBark = hzToBark(0.5 * (artifactLoHz + artifactHiHz));
    // 1 Bark-wide bands up to 24 Bark
    std::vector<double> margins;
    int covered = 0;
    for (int f = 0; f < s.frames; ++f) {
        if (!mask[static_cast<std::size_t>(f)]) continue;
        double thr = 0.0;   // summed covered power (linear)
        for (int z = 0; z < 24; ++z) {
            // band edges in Hz for Bark z..z+1 (inverse by scan of the bin grid)
            double lo = 1e9, hi = 0.0;
            for (int b = 1; b < s.bins; ++b) { const double zb = hzToBark(b * s.binHz()); if (zb >= z && zb < z + 1) { lo = std::min(lo, b * s.binHz()); hi = std::max(hi, (b + 1) * s.binHz()); } }
            if (hi <= lo) continue;
            if (lo >= artifactLoHz && hi <= artifactHiHz) continue;     // the artifact's own band is not allowed to hide itself
            const double p = s.band(f, lo, hi);
            if (p <= 1e-14) continue;
            const double dist = artBark - (z + 0.5);
            thr += std::pow(10.0, (10.0 * std::log10(p) - cfg::kMaskOffsetDb - (dist >= 0 ? cfg::kMaskSlopeHighDb : cfg::kMaskSlopeLowDb) * std::fabs(dist)) / 10.0);
        }
        const double margin = artifactDb - 10.0 * std::log10(thr + 1e-30);
        margins.push_back(margin);
        if (margin < 0) ++covered;
    }
    r.frames = static_cast<int>(margins.size());
    if (r.frames < cfg::kMinLoudFrames) return r;
    r.valid = true;
    r.coveredShare = static_cast<double>(covered) / r.frames;
    r.medianMarginDb = percentile(margins, 50.0);
    return r;
}

// =============================================================================================================
//  19: SHA-256 and blind names
// =============================================================================================================
std::string sha256Hex(const void* data, std::size_t bytes) {
    static const std::uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto rotr = [](std::uint32_t v, int k) { return (v >> k) | (v << (32 - k)); };
    std::vector<unsigned char> msg(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + bytes);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes) * 8ull;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (static_cast<std::uint32_t>(msg[off + 4 * static_cast<std::size_t>(i)]) << 24) | (static_cast<std::uint32_t>(msg[off + 4 * static_cast<std::size_t>(i) + 1]) << 16) | (static_cast<std::uint32_t>(msg[off + 4 * static_cast<std::size_t>(i) + 2]) << 8) | msg[off + 4 * static_cast<std::size_t>(i) + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
}

std::string blindName(const std::string& hex) { return "HEM-" + hex.substr(0, 8); }

// =============================================================================================================
//  17: signal maker
// =============================================================================================================
namespace maker {

double Rng::gauss() noexcept {
    const double u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

std::vector<float> whiteNoise(std::size_t n, double rmsDb, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<float> v(n);
    double s = 0;
    for (auto& e : v) { const double g = rng.gauss(); e = static_cast<float>(g); s += g * g; }
    const double k = std::pow(10.0, rmsDb / 20.0) / std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, n)));
    for (auto& e : v) e = static_cast<float>(e * k);
    return v;
}

std::vector<float> bandpass(const float* x, std::size_t n, double fs, double lo, double hi) {
    // Zero-phase FFT band-pass, flat inside [lo, hi], 100 Hz raised-cosine skirts. Flat so that "depth" means the same for any band that sits inside it.
    const int N = nextPow2(n + 8192);
    FftD fft; fft.prepare(N);
    std::vector<Cx> v(static_cast<std::size_t>(N), Cx{});
    for (std::size_t i = 0; i < n; ++i) v[i] = Cx(x[i], 0.0);
    fft.forward(v.data());
    const double skirt = 100.0, top = std::min(hi, 0.45 * fs);
    for (int k = 0; k <= N / 2; ++k) {
        const double f = k * fs / N;
        double g = 1.0;
        if (f < lo - skirt || f > top + skirt) g = 0.0;
        else if (f < lo) g = 0.5 - 0.5 * std::cos(kPi * (f - (lo - skirt)) / skirt);
        else if (f > top) g = 0.5 + 0.5 * std::cos(kPi * (f - top) / skirt);
        v[static_cast<std::size_t>(k)] *= g;
        if (k > 0 && k < N / 2) v[static_cast<std::size_t>(N - k)] = std::conj(v[static_cast<std::size_t>(k)]);
    }
    fft.inverse(v.data());
    std::vector<float> t(n);
    for (std::size_t i = 0; i < n; ++i) t[i] = static_cast<float>(v[i].real());
    return t;
}

void injectBandWobble(std::vector<float>& x, double fs, double modHz, double depth, double lo, double hi) {
    const auto y = bandpass(x.data(), x.size(), fs, lo, hi);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] += static_cast<float>(depth * std::sin(2.0 * kPi * modHz * static_cast<double>(i) / fs) * y[i]);
}

void injectComb(std::vector<float>& x, double fs, double lineDb, double lo, double hi, std::uint64_t seed) {
    const long P = std::lround(fs / cfg::kCombFundamentalHz);
    if (std::fabs(static_cast<double>(P) * cfg::kCombFundamentalHz - fs) > 1e-6 || x.empty()) return;
    Rng rng(seed);
    std::vector<double> cycle(static_cast<std::size_t>(P), 0.0);
    const double amp = std::sqrt(2.0) * std::pow(10.0, lineDb / 20.0);
    const long m0 = static_cast<long>(std::ceil(lo / cfg::kCombFundamentalHz)), m1 = static_cast<long>(std::floor(hi / cfg::kCombFundamentalHz));
    for (long m = m0; m <= m1; ++m) {
        const double ph = 2.0 * kPi * rng.uniform();
        for (long j = 0; j < P; ++j) cycle[static_cast<std::size_t>(j)] += amp * std::cos(2.0 * kPi * static_cast<double>(m * j) / static_cast<double>(P) + ph);
    }
    for (std::size_t i = 0; i < x.size(); ++i) x[i] += static_cast<float>(cycle[i % static_cast<std::size_t>(P)]);
}

std::vector<std::size_t> injectSeamClicks(std::vector<float>& x, double fs, double periodMs, double peakDb, double startMs) {
    std::vector<std::size_t> at;
    const float a = static_cast<float>(std::pow(10.0, peakDb / 20.0));
    for (int k = 0;; ++k) {
        const long idx = std::lround((startMs + k * periodMs) * fs / 1000.0);
        if (idx < 0) continue;
        if (static_cast<std::size_t>(idx) >= x.size()) break;
        x[static_cast<std::size_t>(idx)] += a;
        at.push_back(static_cast<std::size_t>(idx));
    }
    return at;
}

void injectEcho(std::vector<float>& x, double fs, double delayMs, double gainDb) {
    const std::vector<float> orig = x;
    const std::size_t d = static_cast<std::size_t>(std::lround(delayMs * fs / 1000.0));
    const double g = std::pow(10.0, gainDb / 20.0);
    for (std::size_t i = d; i < x.size(); ++i) x[i] += static_cast<float>(g * orig[i - d]);
}

void injectNotchSweep(std::vector<float>& x, double fs, double f0, double f1, double depthDb, double sigmaHz) {
    const int N = 4096, hop = N / 4;
    const std::size_t n = x.size();
    if (n < static_cast<std::size_t>(N)) return;
    const auto w = hann(N);
    FftD fft; fft.prepare(N);
    std::vector<double> out(n + static_cast<std::size_t>(N), 0.0);
    std::vector<Cx> buf(static_cast<std::size_t>(N));
    const double floorGain = std::pow(10.0, -depthDb / 20.0), total = static_cast<double>(n);
    for (std::size_t s0 = 0; s0 + static_cast<std::size_t>(N) <= n; s0 += static_cast<std::size_t>(hop)) {
        for (int i = 0; i < N; ++i) buf[static_cast<std::size_t>(i)] = Cx(x[s0 + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.0);
        fft.forward(buf.data());
        const double fc = f0 + (f1 - f0) * (static_cast<double>(s0) + N / 2.0) / total;
        for (int k = 0; k <= N / 2; ++k) {
            const double f = k * fs / N, g = 1.0 - (1.0 - floorGain) * std::exp(-0.5 * (f - fc) * (f - fc) / (sigmaHz * sigmaHz));
            buf[static_cast<std::size_t>(k)] *= g;
            if (k > 0 && k < N / 2) buf[static_cast<std::size_t>(N - k)] = std::conj(buf[static_cast<std::size_t>(k)]);
        }
        fft.inverse(buf.data());
        for (int i = 0; i < N; ++i) out[s0 + static_cast<std::size_t>(i)] += buf[static_cast<std::size_t>(i)].real();
    }
    // Hann at 75 % overlap sums to 2 in the middle. Edges (first and last N samples) are left as they were.
    for (std::size_t i = static_cast<std::size_t>(N); i + static_cast<std::size_t>(N) < n; ++i) x[i] = static_cast<float>(out[i] / 2.0);
}

HitTrain hitTrain(std::size_t n, double fs, double meanSpacingMs, double tauMs, double peakDb, double lo, double hi, std::uint64_t seed) {
    HitTrain t;
    Rng rng(seed);
    auto noise = whiteNoise(n, 0.0, seed + 7);
    auto band = bandpass(noise.data(), n, fs, lo, hi);
    double s = 0;
    for (float v : band) s += static_cast<double>(v) * v;
    const double k = 1.0 / std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, n)));
    std::vector<double> env(n, 0.0);
    const double amp = std::pow(10.0, peakDb / 20.0);
    double tms = meanSpacingMs * 0.5;
    const std::size_t len = static_cast<std::size_t>(8.0 * tauMs * fs / 1000.0);
    while (true) {
        const std::size_t i0 = static_cast<std::size_t>(tms * fs / 1000.0);
        if (i0 >= n) break;
        t.hitMs.push_back(tms);
        for (std::size_t j = 0; j < len && i0 + j < n; ++j) env[i0 + j] += std::exp(-static_cast<double>(j) / (tauMs * fs / 1000.0));
        tms += meanSpacingMs * (0.75 + 0.5 * rng.uniform());
    }
    t.audio.resize(n);
    for (std::size_t i = 0; i < n; ++i) t.audio[i] = static_cast<float>(band[i] * k * amp * env[i]);
    return t;
}

} // namespace maker
} // namespace hc::hiss::lab
