#include "hybrid/hiss/HissMetrics.hpp"

#include "hybrid/hiss/HissFft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace hc::hiss::metrics {
namespace {

constexpr double kSilentRms = 1e-6;   // about -120 dBFS: below this a frame counts as digital silence

std::vector<double> hannWindow(int n) {
    std::vector<double> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n);
    return w;
}

// Four-pole high-pass (two RBJ sections) used only for looking at cymbal energy.
struct Hp4 {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    void design(double fs, double hz) {
        const double w = 2.0 * std::numbers::pi * std::min(hz, fs * 0.45) / fs, cw = std::cos(w), sw = std::sin(w);
        const double al = sw / (2.0 * 0.70710678);
        const double a0 = 1.0 + al;
        b0 = (1.0 + cw) / 2.0 / a0; b1 = -(1.0 + cw) / a0; b2 = b0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - al) / a0;
    }
    double tick(double x) {
        for (int s = 0; s < 2; ++s) {
            const double y = b0 * x + z1[s];
            z1[s] = b1 * x - a1 * y + z2[s];
            z2[s] = b2 * x - a2 * y;
            x = y;
        }
        return x;
    }
};

std::vector<double> highpassSegment(const float* x, std::size_t from, std::size_t to, double fs, double hz) {
    Hp4 f;
    f.design(fs, hz);
    std::vector<double> out(to - from);
    for (std::size_t i = from; i < to; ++i) out[i - from] = f.tick(static_cast<double>(x[i]));
    return out;
}

} // namespace

int BandScan::usedFrames() const noexcept {
    int c = 0;
    for (char u : used) c += u ? 1 : 0;
    return c;
}

BandScan scanBand(const float* x, std::size_t n, double fs, int nfft, double loHz, double hiHz) {
    BandScan s;
    s.fs = fs;
    s.nfft = nfft;
    const int hop = nfft / 2;
    s.bin0 = std::max(1, static_cast<int>(std::floor(loHz * nfft / fs)));
    const int bin1 = std::min(nfft / 2 - 1, static_cast<int>(std::ceil(hiHz * nfft / fs)));
    s.bins = std::max(0, bin1 - s.bin0 + 1);
    if (n < static_cast<std::size_t>(nfft) || s.bins == 0) return s;
    s.frames = static_cast<int>((n - static_cast<std::size_t>(nfft)) / static_cast<std::size_t>(hop)) + 1;
    s.power.assign(static_cast<std::size_t>(s.frames) * static_cast<std::size_t>(s.bins), 0.0f);
    s.frameRms.assign(static_cast<std::size_t>(s.frames), 0.0f);
    s.used.assign(static_cast<std::size_t>(s.frames), 0);

    FftD fft;
    fft.prepare(nfft);
    const auto w = hannWindow(nfft);
    double wsum2 = 0.0;
    for (double v : w) wsum2 += v * v;
    std::vector<std::complex<double>> buf(static_cast<std::size_t>(nfft));
    for (int f = 0; f < s.frames; ++f) {
        const std::size_t at = static_cast<std::size_t>(f) * static_cast<std::size_t>(hop);
        double e = 0.0;
        for (int i = 0; i < nfft; ++i) {
            const double v = static_cast<double>(x[at + static_cast<std::size_t>(i)]);
            e += v * v;
            buf[static_cast<std::size_t>(i)] = {v * w[static_cast<std::size_t>(i)], 0.0};
        }
        const double rms = std::sqrt(e / nfft);
        s.frameRms[static_cast<std::size_t>(f)] = static_cast<float>(rms);
        s.used[static_cast<std::size_t>(f)] = rms >= kSilentRms ? 1 : 0;
        fft.forward(buf.data());
        for (int b = 0; b < s.bins; ++b)
            s.power[static_cast<std::size_t>(f) * static_cast<std::size_t>(s.bins) + static_cast<std::size_t>(b)] =
                static_cast<float>(std::norm(buf[static_cast<std::size_t>(s.bin0 + b)]) / wsum2);
    }
    return s;
}

double quantileOf(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    const auto idx = static_cast<std::size_t>(std::floor(q * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}

std::vector<double> quantileSpectrum(const BandScan& s, double q) {
    std::vector<double> out(static_cast<std::size_t>(s.bins), 0.0);
    std::vector<double> col;
    col.reserve(static_cast<std::size_t>(s.frames));
    for (int b = 0; b < s.bins; ++b) {
        col.clear();
        for (int f = 0; f < s.frames; ++f)
            if (s.used[static_cast<std::size_t>(f)])
                col.push_back(static_cast<double>(s.power[static_cast<std::size_t>(f) * static_cast<std::size_t>(s.bins) + static_cast<std::size_t>(b)]));
        out[static_cast<std::size_t>(b)] = quantileOf(col, q);
    }
    return out;
}

FloorStat bandFloor(const float* x, std::size_t n, double fs, double loHz, double hiHz, double q, int nfft) {
    FloorStat r;
    const BandScan s = scanBand(x, n, fs, nfft, loHz, hiHz);
    std::vector<double> fb;
    for (int f = 0; f < s.frames; ++f) {
        if (!s.used[static_cast<std::size_t>(f)]) continue;
        double acc = 0.0;
        for (int b = 0; b < s.bins; ++b) acc += static_cast<double>(s.power[static_cast<std::size_t>(f) * static_cast<std::size_t>(s.bins) + static_cast<std::size_t>(b)]);
        fb.push_back(acc / std::max(1, s.bins));
    }
    r.frames = static_cast<int>(fb.size());
    if (fb.empty()) return r;
    double mean = 0.0;
    for (double v : fb) mean += v;
    mean /= static_cast<double>(fb.size());
    r.meanDb = 10.0 * std::log10(mean + 1e-30);
    r.floorDb = 10.0 * std::log10(quantileOf(fb, q) + 1e-30);
    return r;
}

// ---------------------------------------------------------------------------------------------
std::vector<std::size_t> findOnsets(const float* x, std::size_t n, double fs, int maxCount) {
    std::vector<std::size_t> out;
    const std::size_t blk = std::max<std::size_t>(32, static_cast<std::size_t>(fs / 375.0));   // about 2.7 ms
    if (n < blk * 16) return out;
    const auto hp = highpassSegment(x, 0, n, fs, 6000.0);
    const std::size_t nb = n / blk;
    std::vector<double> e(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        double acc = 0.0;
        for (std::size_t i = 0; i < blk; ++i) acc += hp[b * blk + i] * hp[b * blk + i];
        e[b] = 10.0 * std::log10(acc / static_cast<double>(blk) + 1e-20);
    }
    struct Cand { std::size_t at; double jump; };
    std::vector<Cand> cands;
    const std::size_t refractory = static_cast<std::size_t>(0.10 * fs) / blk + 1;
    std::size_t lastB = 0;
    bool haveLast = false;
    for (std::size_t b = 7; b + 1 < nb; ++b) {
        double mn = e[b - 1];
        for (std::size_t j = 2; j <= 6; ++j) mn = std::min(mn, e[b - j]);
        const double jump = e[b] - mn;
        if (jump > 15.0 && e[b] > -80.0 && e[b] >= e[b + 1] - 3.0) {
            if (haveLast && b - lastB < refractory) {
                if (jump > cands.back().jump) cands.back() = {b * blk, jump};
                lastB = b;
                continue;
            }
            cands.push_back({b * blk, jump});
            lastB = b;
            haveLast = true;
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.jump > b.jump; });
    if (static_cast<int>(cands.size()) > maxCount) cands.resize(static_cast<std::size_t>(maxCount));
    for (const auto& c : cands) out.push_back(c.at);
    std::sort(out.begin(), out.end());
    return out;
}

HitStats compareHits(const float* before, const float* after, std::size_t n, double fs, const std::vector<std::size_t>& onsets) {
    HitStats st;
    const std::size_t preRoll = static_cast<std::size_t>(0.033 * fs);
    const std::size_t lead    = static_cast<std::size_t>(0.008 * fs);
    const std::size_t span    = static_cast<std::size_t>(0.160 * fs);
    const std::size_t b1 = std::max<std::size_t>(8, static_cast<std::size_t>(fs * 0.001));     // 1 ms blocks
    double sumSq = 0.0;
    long long cnt = 0;
    for (const std::size_t s : onsets) {
        if (s < preRoll + lead || s + span + 16 >= n) continue;
        const std::size_t from = s - preRoll, to = s + span;
        const auto hb = highpassSegment(before, from, to, fs, 6000.0);
        const auto ha = highpassSegment(after, from, to, fs, 6000.0);
        const std::size_t w0 = preRoll - lead;   // index inside the segment where the analysis window starts
        double pkB = 0.0, pkA = 0.0;
        for (std::size_t i = w0; i < hb.size(); ++i) { pkB = std::max(pkB, std::fabs(hb[i])); pkA = std::max(pkA, std::fabs(ha[i])); }
        if (pkB < 1e-9) continue;
        ++st.hits;
        st.worstPeakLossDb = std::min(st.worstPeakLossDb, 20.0 * std::log10((pkA + 1e-12) / (pkB + 1e-12)));

        // loudness curve in 1 ms blocks
        const std::size_t nbk = (hb.size() - w0) / b1;
        std::vector<double> eb(nbk), ea(nbk);
        for (std::size_t k = 0; k < nbk; ++k) {
            double sb = 0.0, sa = 0.0;
            for (std::size_t i = 0; i < b1; ++i) { sb += hb[w0 + k * b1 + i] * hb[w0 + k * b1 + i]; sa += ha[w0 + k * b1 + i] * ha[w0 + k * b1 + i]; }
            eb[k] = std::sqrt(sb / static_cast<double>(b1)) + 1e-12;
            ea[k] = std::sqrt(sa / static_cast<double>(b1)) + 1e-12;
        }
        std::vector<double> pre(eb.begin(), eb.begin() + std::min<std::size_t>(6, nbk));
        const double preLevel = quantileOf(pre, 0.5);
        for (std::size_t k = 0; k < nbk; ++k) {
            if (eb[k] < preLevel * 3.1623) continue;   // only where the hit is 10 dB above what came before it
            const double d = 20.0 * std::log10(ea[k] / eb[k]);
            sumSq += d * d;
            ++cnt;
        }

        // attack timing: when has 10 %, 50 % and 90 % of the hit's first 48 ms of energy arrived?
        // (Counting energy that has arrived is steady; a threshold on a bumpy noise-like envelope is not.)
        auto arrival = [&](const std::vector<double>& h, double (&t)[3]) {
            const std::size_t len = std::min(h.size() - w0, static_cast<std::size_t>(0.048 * fs));
            std::vector<double> cum(len);
            double acc = 0.0;
            for (std::size_t i = 0; i < len; ++i) { acc += h[w0 + i] * h[w0 + i]; cum[i] = acc; }
            const double fr[3] = {0.1, 0.5, 0.9};
            for (int q = 0; q < 3; ++q) {
                std::size_t i = 0;
                while (i + 1 < len && cum[i] < fr[q] * acc) ++i;
                t[q] = static_cast<double>(i) * 1000.0 / fs;
            }
        };
        double tb[3], ta[3];
        arrival(hb, tb);
        arrival(ha, ta);
        for (int q = 0; q < 3; ++q) st.maxAttackShiftMs = std::max(st.maxAttackShiftMs, std::fabs(ta[q] - tb[q]));
    }
    st.envRmsErrDb = cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
    return st;
}

// ---------------------------------------------------------------------------------------------
OutOfBand outOfBand(const float* before, const float* after, std::size_t n, double fs, double centreHz, double halfSpanHz, double minLevelDb) {
    OutOfBand r;
    const int nfft = 4096, hop = nfft / 2;
    if (n < static_cast<std::size_t>(nfft)) return r;
    FftD fft;
    fft.prepare(nfft);
    const auto w = hannWindow(nfft);
    double wsum2 = 0.0;
    for (double v : w) wsum2 += v * v;
    const int bins = nfft / 2 + 1;
    std::vector<double> pb(static_cast<std::size_t>(bins), 0.0), pa(static_cast<std::size_t>(bins), 0.0);
    std::vector<std::complex<double>> bb(static_cast<std::size_t>(nfft)), ba(static_cast<std::size_t>(nfft));
    int frames = 0;
    for (std::size_t at = 0; at + static_cast<std::size_t>(nfft) <= n; at += static_cast<std::size_t>(hop), ++frames) {
        for (int i = 0; i < nfft; ++i) {
            bb[static_cast<std::size_t>(i)] = {static_cast<double>(before[at + static_cast<std::size_t>(i)]) * w[static_cast<std::size_t>(i)], 0.0};
            ba[static_cast<std::size_t>(i)] = {static_cast<double>(after[at + static_cast<std::size_t>(i)]) * w[static_cast<std::size_t>(i)], 0.0};
        }
        fft.forward(bb.data());
        fft.forward(ba.data());
        for (int k = 0; k < bins; ++k) {
            pb[static_cast<std::size_t>(k)] += std::norm(bb[static_cast<std::size_t>(k)]);
            pa[static_cast<std::size_t>(k)] += std::norm(ba[static_cast<std::size_t>(k)]);
        }
    }
    const double minLin = std::pow(10.0, minLevelDb / 10.0);
    double sumSq = 0.0;
    for (int k = 1; k < bins - 1; ++k) {
        const double f = static_cast<double>(k) * fs / nfft;
        if (std::fabs(f - centreHz) <= halfSpanHz) continue;
        const double lb = pb[static_cast<std::size_t>(k)] / (wsum2 * frames), la = pa[static_cast<std::size_t>(k)] / (wsum2 * frames);
        if (lb < minLin) continue;
        const double d = 10.0 * std::log10((la + 1e-30) / (lb + 1e-30));
        r.maxAbsDb = std::max(r.maxAbsDb, std::fabs(d));
        sumSq += d * d;
        ++r.bins;
    }
    r.rmsDb = r.bins > 0 ? std::sqrt(sumSq / r.bins) : 0.0;
    return r;
}

// ---------------------------------------------------------------------------------------------
double skewness(const std::vector<double>& v) {
    if (v.size() < 3) return 0.0;
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s2 = 0.0, s3 = 0.0;
    for (double x : v) { const double d = x - m; s2 += d * d; s3 += d * d * d; }
    s2 /= static_cast<double>(v.size());
    s3 /= static_cast<double>(v.size());
    return s2 > 0.0 ? s3 / std::pow(s2, 1.5) : 0.0;
}

double kurtosis(const std::vector<double>& v) {
    if (v.size() < 4) return 0.0;
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s2 = 0.0, s4 = 0.0;
    for (double x : v) { const double d = x - m; s2 += d * d; s4 += d * d * d * d; }
    s2 /= static_cast<double>(v.size());
    s4 /= static_cast<double>(v.size());
    return s2 > 0.0 ? s4 / (s2 * s2) : 0.0;
}

double musicalNoiseRatio(const float* before, const float* after, std::size_t n, double fs, double loHz, double hiHz, double quietShare) {
    const int nfft = 1024, hop = 256;
    if (n < static_cast<std::size_t>(nfft) * 4) return 1.0;
    FftD fft;
    fft.prepare(nfft);
    const auto w = hannWindow(nfft);
    const int k0 = std::max(1, static_cast<int>(std::floor(loHz * nfft / fs)));
    const int k1 = std::min(nfft / 2 - 1, static_cast<int>(std::ceil(hiHz * nfft / fs)));
    if (k1 <= k0) return 1.0;
    struct Fr { std::vector<double> mb, ma; double e; };
    std::vector<Fr> frames;
    std::vector<std::complex<double>> bb(static_cast<std::size_t>(nfft)), ba(static_cast<std::size_t>(nfft));
    for (std::size_t at = 0; at + static_cast<std::size_t>(nfft) <= n; at += static_cast<std::size_t>(hop)) {
        for (int i = 0; i < nfft; ++i) {
            bb[static_cast<std::size_t>(i)] = {static_cast<double>(before[at + static_cast<std::size_t>(i)]) * w[static_cast<std::size_t>(i)], 0.0};
            ba[static_cast<std::size_t>(i)] = {static_cast<double>(after[at + static_cast<std::size_t>(i)]) * w[static_cast<std::size_t>(i)], 0.0};
        }
        fft.forward(bb.data());
        fft.forward(ba.data());
        Fr f;
        f.e = 0.0;
        for (int k = k0; k <= k1; ++k) {
            f.mb.push_back(std::abs(bb[static_cast<std::size_t>(k)]));
            f.ma.push_back(std::abs(ba[static_cast<std::size_t>(k)]));
            f.e += std::norm(bb[static_cast<std::size_t>(k)]);
        }
        frames.push_back(std::move(f));
    }
    std::vector<double> es;
    for (const auto& f : frames) es.push_back(f.e);
    const double med = quantileOf(es, quietShare);
    std::vector<double> kb, ka;
    for (const auto& f : frames) {
        if (f.e > med || f.e <= 0.0) continue;   // only the quieter half: the artifact on its own
        // Each frame is divided by its own average, so a slow level change (a tail dying away) is not mistaken for blips.
        double sb = 0.0, sa = 0.0;
        for (double v : f.mb) sb += v;
        for (double v : f.ma) sa += v;
        sb /= static_cast<double>(f.mb.size());
        sa /= static_cast<double>(f.ma.size());
        if (sb <= 0.0 || sa <= 0.0) continue;
        for (double v : f.mb) kb.push_back(v / sb);
        for (double v : f.ma) ka.push_back(v / sa);
    }
    const double a = kurtosis(kb), b = kurtosis(ka);
    return a > 0.0 && b > 0.0 ? b / a : 1.0;
}

double rmsDb(const float* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return 10.0 * std::log10(s / static_cast<double>(std::max<std::size_t>(1, n)) + 1e-30);
}

double levelDifferenceDb(const float* before, const float* after, std::size_t n) { return rmsDb(after, n) - rmsDb(before, n); }

} // namespace hc::hiss::metrics
