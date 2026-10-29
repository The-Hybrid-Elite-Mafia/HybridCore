#include "hybrid/hiss/HissMeters.hpp"

#include "hybrid/hiss/HissFft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace hc::hiss::meters {
namespace {

constexpr double kPi = std::numbers::pi;
using Cx = std::complex<double>;

double dbOf(double v) { return 10.0 * std::log10(std::max(v, 1e-30)); }

std::vector<double> hann(int n) {   // periodic Hann (same as scipy.signal.get_window("hann", n))
    std::vector<double> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    return w;
}

int nextPow2(std::size_t n) { int p = 1; while (static_cast<std::size_t>(p) < n) p <<= 1; return p; }

double percentile(std::vector<double> v, double pct) {   // linear interpolation, like numpy.percentile
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = (static_cast<double>(v.size()) - 1.0) * pct / 100.0;
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx)), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
}

// ---- 2nd-order sections (RBJ cookbook = bilinear Butterworth sections), 4th-order Butterworth = two of them ----
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void run(std::vector<double>& x) const {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (double& v : x) {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = v; y2 = y1; y1 = y; v = y;
        }
    }
};
Biquad makeBiquad(bool high, double f, double fs, double q) {
    const double w0 = 2.0 * kPi * f / fs, c = std::cos(w0), alpha = std::sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    Biquad b;
    if (high) { b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0; }
    else      { b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0;  b.b2 = (1 - c) / 2 / a0; }
    b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}
// forward then backward = zero phase (the response is squared). 4th-order Butterworth.
void filtfilt4(std::vector<double>& x, bool high, double f, double fs) {
    const Biquad s1 = makeBiquad(high, f, fs, 0.5411961001461969), s2 = makeBiquad(high, f, fs, 1.3065629648763766);
    s1.run(x); s2.run(x);
    std::reverse(x.begin(), x.end());
    s1.run(x); s2.run(x);
    std::reverse(x.begin(), x.end());
}

} // namespace

// ================================ STFT / ratio ================================
double Stft::band(int frame, double loHz, double hiHz) const noexcept {
    const double bh = binHz();
    double s = 0.0;
    const double* row = &p[static_cast<std::size_t>(frame) * static_cast<std::size_t>(bins)];
    for (int b = 0; b < bins; ++b) {
        const double f = b * bh;
        if (f >= loHz && f < hiHz) s += row[b];
    }
    return s;
}

Stft stftPower(const float* x, std::size_t n, double fs, int nfft, int hop) {
    Stft s;
    s.fs = fs; s.nfft = nfft; s.hop = hop; s.bins = nfft / 2 + 1;
    if (n < static_cast<std::size_t>(nfft)) return s;
    s.frames = 1 + static_cast<int>((n - static_cast<std::size_t>(nfft)) / static_cast<std::size_t>(hop));
    s.p.assign(static_cast<std::size_t>(s.frames) * static_cast<std::size_t>(s.bins), 0.0);
    const auto w = hann(nfft);
    double sw = 0; for (double v : w) sw += v;
    const double k = 1.0 / (sw * sw);
    FftD fft; fft.prepare(nfft);
    std::vector<Cx> buf(static_cast<std::size_t>(nfft));
    for (int f = 0; f < s.frames; ++f) {
        const float* src = x + static_cast<std::size_t>(f) * static_cast<std::size_t>(hop);
        for (int i = 0; i < nfft; ++i) buf[static_cast<std::size_t>(i)] = Cx(src[i] * w[static_cast<std::size_t>(i)], 0.0);
        fft.forward(buf.data());
        double* row = &s.p[static_cast<std::size_t>(f) * static_cast<std::size_t>(s.bins)];
        for (int b = 0; b < s.bins; ++b) row[b] = std::norm(buf[static_cast<std::size_t>(b)]) * k;
    }
    return s;
}

std::vector<char> loudMask(const std::vector<double>& e, double silenceDb, double enterPct, double leavePct) {
    std::vector<char> m(e.size(), 0), ns(e.size(), 0);
    std::vector<double> used;
    for (std::size_t i = 0; i < e.size(); ++i) if (dbOf(e[i]) > silenceDb) { ns[i] = 1; used.push_back(e[i]); }
    if (used.size() < 20) return m;
    const double hi = percentile(used, enterPct), lo = percentile(used, leavePct);
    bool on = false;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (!on && e[i] >= hi && ns[i]) on = true;
        else if (on && (e[i] < lo || !ns[i])) on = false;
        m[i] = on ? 1 : 0;
    }
    return m;
}

RatioResult ratioMetrics(const float* x, std::size_t n, double fs, const std::vector<char>* mask) {
    RatioResult r;
    const Stft s = stftPower(x, n, fs);
    if (s.frames < 1) return r;
    r.frames = s.frames;
    const double top = std::min(20000.0, fs / 2.0), top19 = std::min(19000.0, fs / 2.0);
    std::vector<double> A(static_cast<std::size_t>(s.frames)), B = A, C = A, B19 = A, E = A;
    for (int f = 0; f < s.frames; ++f) {
        const auto i = static_cast<std::size_t>(f);
        A[i] = s.band(f, 5000, 10000); B[i] = s.band(f, 16000, top); C[i] = s.band(f, 8000, top);
        B19[i] = s.band(f, 16000, top19); E[i] = s.band(f, 5000, 16000);
    }
    const std::vector<char> m = mask ? *mask : loudMask(E);
    double a = 0, b = 0, c = 0, b19 = 0; int cnt = 0;
    for (int f = 0; f < s.frames; ++f) {
        const auto i = static_cast<std::size_t>(f);
        if (i < m.size() && m[i]) { a += A[i]; b += B[i]; c += C[i]; b19 += B19[i]; ++cnt; }
    }
    r.loud = cnt;
    if (cnt < 20) return r;
    a /= cnt; b /= cnt; c /= cnt; b19 /= cnt;
    r.valid = true;
    r.a5to10Db = dbOf(a); r.b16to20Db = dbOf(b); r.c8to20Db = dbOf(c);
    r.ratio16_20Over5_10Db = dbOf(b) - dbOf(a);
    r.ratio16_19Over5_10Db = dbOf(b19) - dbOf(a);
    return r;
}

// ================================ bandwidth audit ================================
CutoffResult cutoffAudit(const float* x, std::size_t n, double fs) {
    CutoffResult r;
    constexpr int kN = 8192, kHop = 4096;
    if (n < static_cast<std::size_t>(kN)) return r;
    const int bins = kN / 2 + 1;
    const auto w = hann(kN);
    double sw = 0; for (double v : w) sw += v;
    FftD fft; fft.prepare(kN);
    std::vector<Cx> buf(static_cast<std::size_t>(kN));
    std::vector<double> p(static_cast<std::size_t>(bins), 0.0);
    int segs = 0;
    for (std::size_t st = 0; st + kN <= n; st += kHop) {
        for (int i = 0; i < kN; ++i) buf[static_cast<std::size_t>(i)] = Cx(x[st + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.0);
        fft.forward(buf.data());
        for (int b = 0; b < bins; ++b) p[static_cast<std::size_t>(b)] += std::norm(buf[static_cast<std::size_t>(b)]) * 2.0 / (sw * sw);
        ++segs;
    }
    for (double& v : p) v = v / segs + 1e-24;
    const double df = fs / kN, nyq = fs / 2.0;
    // 100 Hz boxcar on power with mirrored edges (so the ends do not fake a fall); numpy "same" alignment
    const int nk = std::max(1, static_cast<int>(std::lround(100.0 / df))), pad = nk / 2 + 1;
    std::vector<double> pp;
    for (int i = pad; i >= 1; --i) pp.push_back(p[static_cast<std::size_t>(i)]);
    pp.insert(pp.end(), p.begin(), p.end());
    for (int i = 1; i <= pad; ++i) pp.push_back(p[p.size() - 1 - static_cast<std::size_t>(i)]);
    std::vector<double> s(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        // full[m] = sum_j pp[m-j]/nk, same[i] = full[i + (nk-1)/2]; m = (pad + i) + (nk-1)/2
        const long m = static_cast<long>(pad + i) + (nk - 1) / 2;
        const long hiIdx = std::min<long>(m, static_cast<long>(pp.size()) - 1), loIdx = m - nk + 1;
        double sum = 0;
        for (long q = std::max<long>(loIdx, 0); q <= hiIdx; ++q) sum += pp[static_cast<std::size_t>(q)];
        s[i] = 10.0 * std::log10(sum / nk + 1e-30);
    }
    std::vector<double> ref;
    for (int b = 0; b < bins; ++b) { const double f = b * df; if (f >= 5000 && f < 10000) ref.push_back(s[static_cast<std::size_t>(b)]); }
    r.ref5to10Db = percentile(ref, 50.0);
    const int span = std::max(1, static_cast<int>(std::lround(500.0 / df)));
    double bestDrop = 0; int bestI = -1;
    for (int i = 0; i < bins; ++i) {
        const double f = i * df;
        if (f < 8000 || f > nyq * 0.995) continue;
        if (i + span >= bins) break;
        const double d = s[static_cast<std::size_t>(i)] - s[static_cast<std::size_t>(i + span)];
        if (d > bestDrop) { bestDrop = d; bestI = i; }
    }
    r.valid = true;
    if (bestDrop >= 20.0) {
        r.shape = EndShape::Wall; r.dropDbIn500Hz = bestDrop;
        const double lvl = s[static_cast<std::size_t>(std::max(0, bestI - static_cast<int>(std::lround(1000.0 / df))))];
        int k = bestI;
        while (k < bins - 1 && s[static_cast<std::size_t>(k)] > lvl - 6.0) ++k;
        r.cutoffHz = k * df;
    } else {
        int first = -1;
        for (int i = 0; i < bins; ++i) if (i * df >= 8000 && s[static_cast<std::size_t>(i)] < r.ref5to10Db - 30.0) { first = i; break; }
        if (first >= 0) { r.shape = EndShape::Gentle; r.cutoffHz = first * df; }
        else { r.shape = EndShape::None; r.cutoffHz = nyq; }
    }
    return r;
}

// ================================ comb ================================
CombResult combLines(const float* x, std::size_t n, double fs, double loHz, double hiHz) {
    CombResult r;
    const long P = std::lround(fs / 25.0);
    if (std::fabs(static_cast<double>(P) * 25.0 - fs) > 1e-6) return r;     // fs must be a multiple of 25
    const long Q = 2 * P, M = static_cast<long>(n) / Q;
    if (M < 2) return r;
    std::vector<double> y(static_cast<std::size_t>(Q), 0.0);
    for (long m = 0; m < M; ++m)
        for (long t = 0; t < Q; ++t) y[static_cast<std::size_t>(t)] += x[static_cast<std::size_t>(m * Q + t)];
    for (double& v : y) v /= static_cast<double>(M);
    std::vector<double> cs(static_cast<std::size_t>(Q)), sn(static_cast<std::size_t>(Q));
    for (long i = 0; i < Q; ++i) { cs[static_cast<std::size_t>(i)] = std::cos(2.0 * kPi * i / Q); sn[static_cast<std::size_t>(i)] = std::sin(2.0 * kPi * i / Q); }
    auto pw = [&](long j) {      // 2*|Y_j|^2, Y_j = (1/Q) sum y[t] e^{-2 pi i j t / Q}
        double re = 0, im = 0;
        for (long t = 0; t < Q; ++t) { const auto idx = static_cast<std::size_t>((j * t) % Q); re += y[static_cast<std::size_t>(t)] * cs[idx]; im -= y[static_cast<std::size_t>(t)] * sn[idx]; }
        re /= static_cast<double>(Q); im /= static_cast<double>(Q);
        return 2.0 * (re * re + im * im);
    };
    const long k0 = static_cast<long>(std::ceil(loHz / 25.0)), k1 = static_cast<long>(std::floor(hiHz / 25.0));
    std::vector<double> on, off, lv;
    long gt = 0;
    for (long k = k0; k <= k1; ++k) {
        const double o = pw(2 * k), f = 0.5 * (pw(2 * k - 1) + pw(2 * k + 1));
        on.push_back(dbOf(o)); off.push_back(dbOf(f));
        lv.push_back(dbOf(std::max(o - f, 1e-30)));
        if (o > f) ++gt;
    }
    r.valid = true; r.lines = static_cast<int>(lv.size()); r.periods = static_cast<int>(M);
    r.bandwidthHz = 1.0 / (static_cast<double>(M * Q) / fs);
    r.medianLineDb = percentile(lv, 50.0); r.medianOnDb = percentile(on, 50.0); r.medianOffDb = percentile(off, 50.0);
    r.onGreaterThanOffPct = 100.0 * static_cast<double>(gt) / static_cast<double>(lv.size());
    r.lineDb = std::move(lv);
    return r;
}

// ================================ flutter ================================
FlutterResult flutterDepth(const float* x, std::size_t n, double fs, double modHz) {
    FlutterResult r;
    if (static_cast<double>(n) < 19.5 * fs) return r;
    std::vector<double> b(x, x + n);
    filtfilt4(b, true, 5000.0, fs);
    filtfilt4(b, false, std::min(15000.0, fs / 2.0 * 0.9), fs);
    double ms = 0; for (double v : b) ms += v * v;
    if (std::sqrt(ms / static_cast<double>(n)) < std::pow(10.0, -90.0 / 20.0)) return r;
    // Hilbert envelope through a zero-padded power-of-2 FFT
    const int N = nextPow2(n);
    FftD fft; fft.prepare(N);
    std::vector<Cx> z(static_cast<std::size_t>(N), Cx{});
    for (std::size_t i = 0; i < n; ++i) z[i] = Cx(b[i], 0.0);
    fft.forward(z.data());
    for (int k = 1; k < N / 2; ++k) z[static_cast<std::size_t>(k)] *= 2.0;
    for (int k = N / 2 + 1; k < N; ++k) z[static_cast<std::size_t>(k)] = Cx{};
    fft.inverse(z.data());
    std::vector<double> e(n);
    for (std::size_t i = 0; i < n; ++i) e[i] = std::abs(z[i]);
    filtfilt4(e, false, 200.0, fs);
    double mean = 0; for (double v : e) mean += v;
    mean /= static_cast<double>(n);
    if (!(mean > 0)) return r;
    const auto w = hann(static_cast<int>(n));
    double sw = 0; for (double v : w) sw += v;
    const long k0 = std::lround(modHz * static_cast<double>(n) / fs);
    double best = 0;
    for (long k = k0 - 1; k <= k0 + 1; ++k) {
        double re = 0, im = 0;
        const double step = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double v = (e[i] - mean) * w[i], ang = step * static_cast<double>(i);
            re += v * std::cos(ang); im += v * std::sin(ang);
        }
        best = std::max(best, 2.0 * std::hypot(re, im) / sw);
    }
    r.valid = true; r.depth = best / mean;
    return r;
}

// ================================ levels ================================
double rmsDbfs(const float* x, std::size_t n) {
    if (n == 0) return -300.0;
    double s = 0; for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return dbOf(s / static_cast<double>(n));
}

double truePeakDbfs(const float* x, std::size_t n) {
    constexpr int kHalf = 16;   // taps each side
    double peak = 0;
    for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(static_cast<double>(x[i])));
    double h[3][2 * kHalf];     // windowed-sinc fractional-delay filters for phases 1/4, 2/4, 3/4
    for (int ph = 1; ph <= 3; ++ph)
        for (int t = 0; t < 2 * kHalf; ++t) {
            const double d = (t - (kHalf - 1)) - ph / 4.0;      // distance from the point being rebuilt
            const double sinc = std::fabs(d) < 1e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
            const double win = 0.42 + 0.5 * std::cos(kPi * d / kHalf) + 0.08 * std::cos(2.0 * kPi * d / kHalf);   // Blackman, centred on that point
            h[ph - 1][t] = sinc * win;
        }
    for (std::size_t i = kHalf; i + kHalf < n; ++i)
        for (int ph = 0; ph < 3; ++ph) {
            double acc = 0;
            for (int t = 0; t < 2 * kHalf; ++t) acc += h[ph][t] * x[i - (kHalf - 1) + static_cast<std::size_t>(t)];
            peak = std::max(peak, std::fabs(acc));
        }
    return 20.0 * std::log10(std::max(peak, 1e-15));
}

double integratedLufs(const float* x, std::size_t n, double fs) {
    // K-weighting: high shelf + high-pass (formulas as in BS.1770 reference code, valid for any sample rate)
    auto shelf = [&] {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad b; b.b0 = (Vh + Vb * K / Q + K * K) / a0; b.b1 = 2.0 * (K * K - Vh) / a0; b.b2 = (Vh - Vb * K / Q + K * K) / a0;
        b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0; return b;
    }();
    auto hp = [&] {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / fs), a0 = 1.0 + K / Q + K * K;
        Biquad b; b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0; b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0; return b;
    }();
    std::vector<double> y(x, x + n);
    shelf.run(y); hp.run(y);
    const std::size_t blk = static_cast<std::size_t>(std::lround(0.4 * fs)), hop = static_cast<std::size_t>(std::lround(0.1 * fs));
    if (n < blk) return -144.0;
    std::vector<double> sq(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) sq[i + 1] = sq[i] + y[i] * y[i];
    std::vector<double> ms;
    for (std::size_t st = 0; st + blk <= n; st += hop) ms.push_back((sq[st + blk] - sq[st]) / static_cast<double>(blk));
    auto lufsOf = [](double m) { return -0.691 + 10.0 * std::log10(std::max(m, 1e-30)); };
    double sum = 0; int cnt = 0;
    for (double m : ms) if (lufsOf(m) > -70.0) { sum += m; ++cnt; }
    if (cnt == 0) return -144.0;
    const double rel = lufsOf(sum / cnt) - 10.0;
    sum = 0; cnt = 0;
    for (double m : ms) if (lufsOf(m) > -70.0 && lufsOf(m) > rel) { sum += m; ++cnt; }
    return cnt == 0 ? -144.0 : lufsOf(sum / cnt);
}

double bandRmsDbfs(const float* x, std::size_t n, double fs, double loHz, double hiHz) {
    const int N = nextPow2(n);
    FftD fft; fft.prepare(N);
    std::vector<Cx> z(static_cast<std::size_t>(N), Cx{});
    for (std::size_t i = 0; i < n; ++i) z[i] = Cx(x[i], 0.0);
    fft.forward(z.data());
    double p = 0;
    for (int k = 0; k <= N / 2; ++k) {
        const double f = k * fs / N;
        if (f >= loHz && f < hiHz) p += std::norm(z[static_cast<std::size_t>(k)]);
    }
    return dbOf(2.0 * p / (static_cast<double>(N) * static_cast<double>(n)));
}

} // namespace hc::hiss::meters
