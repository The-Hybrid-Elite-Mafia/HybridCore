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
//  8: seam clicks
// =============================================================================================================
SeamResult seamClicks(const float* x, std::size_t n, double fs, double periodMs) {
    SeamResult r;
    if (n < static_cast<std::size_t>(0.5 * fs)) return r;
    r.valid = true;
    Bq h1 = makeBq(true, cfg::kClickHpHz, fs), h2 = h1;
    std::vector<double> a(n);
    for (std::size_t i = 0; i < n; ++i) a[i] = std::fabs(h2.run(h1.run(static_cast<double>(x[i]))));
    // Typical size: median of |sample| inside a window around each point. Computed on a coarse grid (every 0.5 ms), so it stays cheap.
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
        const double t = std::max(typical[i / grid], 1e-9);
        if (a[i] >= cfg::kClickRiseRatio * t && a[i] > 1e-5) {
            // peak inside the next 1 ms
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
    r.pattern = static_cast<int>(r.clicks.size()) >= cfg::kClickMinCount && r.periodicShare >= cfg::kClickMinPeriodic;
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
    int have = 0;
    auto flush = [&](double tSec) {
        // acc holds summed power over `have` frames
        std::vector<double> db(static_cast<std::size_t>(bins), -200.0);
        for (int b = b0; b <= b1; ++b) db[static_cast<std::size_t>(b)] = 10.0 * std::log10(acc[static_cast<std::size_t>(b)] / have + 1e-30);
        // moving averages by running sums
        std::vector<double> cs(static_cast<std::size_t>(bins) + 1, 0.0);
        for (int b = 0; b < bins; ++b) cs[static_cast<std::size_t>(b) + 1] = cs[static_cast<std::size_t>(b)] + db[static_cast<std::size_t>(b)];
        auto avg = [&](int c, int hw) {
            const int lo = std::max(b0, c - hw), hi = std::min(b1, c + hw);
            return (cs[static_cast<std::size_t>(hi) + 1] - cs[static_cast<std::size_t>(lo)]) / (hi - lo + 1);
        };
        std::vector<double> win;
        auto med = [&](int c, int hw) {       // normal level = MEDIAN of the wide window, so the dip itself does not pull it down
            win.assign(db.begin() + (std::max(b0, c - hw)), db.begin() + (std::min(b1, c + hw) + 1));
            std::nth_element(win.begin(), win.begin() + static_cast<std::ptrdiff_t>(win.size() / 2), win.end());
            return win[win.size() / 2];
        };
        double bestDepth = -1e9; int bestBin = b0;
        for (int b = b0 + wide; b <= b1 - wide; ++b) {
            const double depth = med(b, wide) - avg(b, narrow);
            if (depth > bestDepth) { bestDepth = depth; bestBin = b; }
        }
        r.track.push_back({tSec, bestBin * df, bestDepth});
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
    std::vector<double> d, fr;
    for (auto& p : r.track) { d.push_back(p.depthDb); fr.push_back(p.freqHz); }
    r.medianDepthDb = percentile(d, 50.0);
    r.medianFreqHz = percentile(fr, 50.0);
    r.found = r.medianDepthDb >= cfg::kNotchMinDepthDb;
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
    std::vector<int> pk;
    int last = -1000000;
    for (int i = back; i < m; ++i) {
        if (i - last < refr) continue;
        if (db[static_cast<std::size_t>(i)] - db[static_cast<std::size_t>(i - back)] >= cfg::kHitRiseDb && db[static_cast<std::size_t>(i)] >= r.floorDb + cfg::kHitAboveFloorDb) {
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
    const int N = nextPow2(2 * m);
    FftD fft; fft.prepare(N);
    std::vector<Cx> v(static_cast<std::size_t>(N), Cx{});
    for (std::size_t i = 0; i < m; ++i) v[i] = Cx(nov[i], 0.0);
    fft.forward(v.data());
    for (auto& s : v) s = Cx(std::norm(s), 0.0);
    fft.inverse(v.data());
    const double r0 = v[0].real();
    if (r0 <= 1e-30) return r;
    const int lo = static_cast<int>(cfg::kEchoMinMs / c.dtMs), hi = std::min(static_cast<int>(cfg::kEchoMaxMs / c.dtMs), static_cast<int>(m) / 2);
    if (hi <= lo + 4) return r;
    r.valid = true;
    std::vector<double> mags;
    int best = lo; double bv = -1;
    for (int l = lo; l <= hi; ++l) {
        const double q = v[static_cast<std::size_t>(l)].real() / r0;
        mags.push_back(std::fabs(q));
        if (q > bv) { bv = q; best = l; }
    }
    const double typical = std::max(percentile(mags, 50.0), 1e-9);
    r.lagMs = best * c.dtMs;
    r.contrast = bv / typical;
    // ratio = g / (1 + g^2)  ->  g = (1 - sqrt(1 - 4 ratio^2)) / (2 ratio)
    const double q = std::clamp(bv, 0.0, 0.499);
    r.strength = q > 1e-6 ? (1.0 - std::sqrt(1.0 - 4.0 * q * q)) / (2.0 * q) : 0.0;
    r.found = r.strength >= cfg::kEchoMinStrength && r.contrast >= cfg::kEchoMinContrast;
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
    const int seg = 1024, hop = 512;
    if (m < static_cast<std::size_t>(seg + hop)) return r;
    const auto w = hann(seg);
    FftD fft; fft.prepare(seg);
    std::vector<double> ps(static_cast<std::size_t>(seg / 2 + 1), 0.0);
    std::vector<Cx> buf(static_cast<std::size_t>(seg));
    int cnt = 0;
    for (std::size_t s0 = 0; s0 + static_cast<std::size_t>(seg) <= m; s0 += static_cast<std::size_t>(hop)) {
        for (int i = 0; i < seg; ++i) buf[static_cast<std::size_t>(i)] = Cx(z[s0 + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.0);
        fft.forward(buf.data());
        for (int b = 0; b <= seg / 2; ++b) ps[static_cast<std::size_t>(b)] += std::norm(buf[static_cast<std::size_t>(b)]);
        ++cnt;
    }
    const double df = rate / seg;
    const int lo = std::max(1, static_cast<int>(std::ceil(cfg::kWobbleMinHz / df))), hi = std::min(seg / 2 - 1, static_cast<int>(std::floor(cfg::kWobbleMaxHz / df)));
    std::vector<double> inband;
    for (int b = lo; b <= hi; ++b) inband.push_back(ps[static_cast<std::size_t>(b)]);
    const double typical = std::max(percentile(inband, 50.0), 1e-30);
    // peaks: local maxima that clear the contrast rule, strongest first, at least 1.5 Hz apart, at most 6
    std::vector<int> peaks;
    for (int b = lo + 1; b < hi; ++b) {
        const double v = ps[static_cast<std::size_t>(b)];
        if (v >= ps[static_cast<std::size_t>(b - 1)] && v > ps[static_cast<std::size_t>(b + 1)]) peaks.push_back(b);
    }
    std::sort(peaks.begin(), peaks.end(), [&](int p, int q) { return ps[static_cast<std::size_t>(p)] > ps[static_cast<std::size_t>(q)]; });
    std::vector<int> chosen;
    const int sep = std::max(1, static_cast<int>(std::lround(1.5 / df)));
    for (int p : peaks) {
        if (10.0 * std::log10(ps[static_cast<std::size_t>(p)] / typical) < cfg::kWobbleContrastDb) break;
        bool near = false;
        for (int q : chosen) if (std::abs(p - q) < sep) near = true;
        if (!near) chosen.push_back(p);
        if (chosen.size() == 6) break;
    }
    if (chosen.empty()) {     // nothing clears the rule: report the strongest line so the page can show how far under it was
        int pb = lo;
        for (int b = lo; b <= hi; ++b) if (ps[static_cast<std::size_t>(b)] > ps[static_cast<std::size_t>(pb)]) pb = b;
        chosen.push_back(pb);
    }
    auto lockIn = [&](const std::vector<double>& zz, double f) {
        Cx acc{};
        for (std::size_t i = 0; i < zz.size(); ++i) acc += zz[i] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(i) / rate);
        return 2.0 * acc / static_cast<double>(zz.size());
    };
    for (int pb : chosen) {
        WobbleLine ln;
        ln.contrastDb = 10.0 * std::log10(ps[static_cast<std::size_t>(pb)] / typical);
        double f = pb * df;
        if (pb > lo && pb < hi) {
            const double l = std::log(ps[static_cast<std::size_t>(pb - 1)] + 1e-30), cc = std::log(ps[static_cast<std::size_t>(pb)] + 1e-30), rr = std::log(ps[static_cast<std::size_t>(pb + 1)] + 1e-30);
            const double den = l - 2.0 * cc + rr;
            if (std::fabs(den) > 1e-12) f += 0.5 * (l - rr) / den * df;
        }
        ln.freqHz = f;
        auto nearPow = [&](double mult) {
            const int c = static_cast<int>(std::lround(mult * f / df));
            if (c + 1 > seg / 2) return 0.0;
            return std::max({ps[static_cast<std::size_t>(c - 1)], ps[static_cast<std::size_t>(c)], ps[static_cast<std::size_t>(c + 1)]});
        };
        const double pf = ps[static_cast<std::size_t>(pb)];
        ln.rhythmLike = nearPow(2.0) >= cfg::kWobbleHarmonicShare * pf || nearPow(3.0) >= cfg::kWobbleHarmonicShare * pf;
        const Cx a0 = lockIn(z, f);
        // Standard addition (same idea as the flutter meter): add a known wobble at this rate, read how much the reading moved,
        // and scale. This removes the meter's own gain error. If the reading does not move, no number is given.
        std::vector<float> copy(x, x + n);
        maker::injectBandWobble(copy, fs, f, cfg::kWobbleCalibDepth, bandLoHz, std::min(bandHiHz, 0.45 * fs));
        const Curve c2 = bandCurve(copy.data(), n, fs, bandLoHz, bandHiHz, cfg::kWobbleEnvHz, 80.0);
        if (c2.p.size() != m) continue;
        std::vector<double> z2(m);
        double mean2 = 0;
        for (std::size_t i = 0; i < m; ++i) { z2[i] = std::sqrt(c2.p[i]); mean2 += z2[i]; }
        mean2 /= static_cast<double>(m);
        for (std::size_t i = 0; i < m; ++i) z2[i] = z2[i] / mean2 - 1.0;
        const double moved = std::abs(lockIn(z2, f) - a0);
        if (moved < 0.2 * cfg::kWobbleCalibDepth) continue;
        ln.depth = std::abs(a0) / moved * cfg::kWobbleCalibDepth;
        r.lines.push_back(ln);
    }
    if (r.lines.empty()) return r;
    r.valid = true;
    for (auto& ln : r.lines) {
        ln.depthTrusted = !ln.rhythmLike;
        for (const auto& o : r.lines) if (o.rhythmLike && o.contrastDb > ln.contrastDb) ln.depthTrusted = false;
    }
    const WobbleLine* best = &r.lines[0];
    for (const auto& ln : r.lines) if (!ln.rhythmLike) { best = &ln; break; }
    r.freqHz = best->freqHz; r.depth = best->depth; r.contrastDb = best->contrastDb;
    for (const auto& ln : r.lines) { if (ln.contrastDb >= cfg::kWobbleContrastDb) { r.found = true; if (!ln.rhythmLike) r.foundSteady = true; } }
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
    const double df = fs / N;
    const auto w = hann(N);
    FftD fft; fft.prepare(N);
    std::vector<Cx> L(static_cast<std::size_t>(N)), R(static_cast<std::size_t>(N));
    constexpr int kBands = 8;
    Cx cross[kBands] = {};
    double pl[kBands] = {}, pr[kBands] = {};
    for (int f = 0; f < frames; ++f) {
        const std::size_t o = static_cast<std::size_t>(f) * static_cast<std::size_t>(hop);
        for (int i = 0; i < N; ++i) { L[static_cast<std::size_t>(i)] = Cx(left[o + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0); R[static_cast<std::size_t>(i)] = Cx(right[o + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0); }
        fft.forward(L.data()); fft.forward(R.data());
        for (int b = 0; b < kBands; ++b) {
            const int k0 = static_cast<int>(std::ceil(edges[b] / df)), k1 = std::min(N / 2, static_cast<int>(std::floor(edges[b + 1] / df)));
            for (int k = k0; k < k1; ++k) {
                cross[b] += L[static_cast<std::size_t>(k)] * std::conj(R[static_cast<std::size_t>(k)]);
                pl[b] += std::norm(L[static_cast<std::size_t>(k)]); pr[b] += std::norm(R[static_cast<std::size_t>(k)]);
            }
        }
    }
    double sum = 0; int used = 0;
    for (int b = 0; b < kBands; ++b) {
        if (edges[b] >= 0.45 * fs) break;
        PhaseBand pb; pb.loHz = edges[b]; pb.hiHz = std::min(edges[b + 1], 0.5 * fs);
        const double den = std::sqrt(pl[b] * pr[b]);
        pb.coherence = den > 1e-30 ? std::abs(cross[b]) / den : 0.0;
        pb.phaseDeg = std::arg(cross[b]) * 180.0 / kPi;
        r.bands.push_back(pb);
        sum += pb.coherence; ++used;
    }
    r.valid = used > 0;
    r.meanCoherence = used ? sum / used : 0.0;
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
