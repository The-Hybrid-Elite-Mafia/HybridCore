// HEM HISS AUTO — scan, diagnose, decide, fix, self-check, do no harm. See the header.
#include "hybrid/hiss/HissAuto.hpp"

#include "hybrid/hiss/HissFft.hpp"
#include "hybrid/hiss/HissMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <sstream>

namespace hc::hiss {
namespace M = metrics;
namespace {

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

std::vector<double> runningMedian(const std::vector<double>& v, int half) {
    const int n = static_cast<int>(v.size());
    std::vector<double> out(v.size());
    std::vector<double> win;
    win.reserve(static_cast<std::size_t>(2 * half + 1));
    for (int i = 0; i < n; ++i) {
        win.clear();
        for (int j = i - half; j <= i + half; ++j) win.push_back(v[static_cast<std::size_t>(std::clamp(j, 0, n - 1))]);
        const auto mid = win.begin() + static_cast<std::ptrdiff_t>(win.size() / 2);
        std::nth_element(win.begin(), mid, win.end());
        out[static_cast<std::size_t>(i)] = *mid;
    }
    return out;
}

std::vector<float> sumOf(const float* a, const float* b, std::size_t n, float wa, float wb) {
    std::vector<float> m(n);
    for (std::size_t i = 0; i < n; ++i) m[i] = wa * a[i] + wb * b[i];
    return m;
}

double dbOf(double p) { return 10.0 * std::log10(p + 1e-20); }

struct Excess {
    std::vector<double> raw, smooth;   // dB above the running-median baseline; smooth = 15-bin median
};

Excess excessOf(const std::vector<double>& qLinear, int baselineHalf) {
    std::vector<double> db(qLinear.size());
    for (std::size_t i = 0; i < db.size(); ++i) db[i] = dbOf(qLinear[i]);
    const auto base = runningMedian(db, baselineHalf);
    Excess e;
    e.raw.resize(db.size());
    for (std::size_t i = 0; i < db.size(); ++i) e.raw[i] = db[i] - base[i];
    e.smooth = runningMedian(e.raw, 7);
    return e;
}

// Looks for a repeating swing of the band's loudness (a flutter). Hits are blanked out first.
struct Pulse { double hz = 0.0, depthDb = 0.0, prominenceDb = 0.0, skew = 0.0, steadiness = 0.0; };

Pulse pulseAnalysis(const std::vector<float>& mid, double fs, double centreHz, double widthHz) {
    Pulse p;
    const int nfft = fs > cfg::kHighRateHz ? 2048 : 1024, hop = nfft / 4;
    const std::size_t n = mid.size();
    if (n < static_cast<std::size_t>(nfft) * 64) return p;
    FftD fft;
    fft.prepare(nfft);
    std::vector<double> w(static_cast<std::size_t>(nfft));
    double wsum2 = 0.0;
    for (int i = 0; i < nfft; ++i) { w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / nfft); wsum2 += w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)]; }
    const double half = std::max(widthHz, 300.0) * 0.5;
    const int k0 = std::max(1, static_cast<int>(std::floor((centreHz - half) * nfft / fs)));
    const int k1 = std::min(nfft / 2 - 1, static_cast<int>(std::ceil((centreHz + half) * nfft / fs)));
    if (k1 <= k0) return p;
    std::vector<double> env;
    std::vector<std::complex<double>> buf(static_cast<std::size_t>(nfft));
    for (std::size_t at = 0; at + static_cast<std::size_t>(nfft) <= n; at += static_cast<std::size_t>(hop)) {
        for (int i = 0; i < nfft; ++i) buf[static_cast<std::size_t>(i)] = {static_cast<double>(mid[at + static_cast<std::size_t>(i)]) * w[static_cast<std::size_t>(i)], 0.0};
        fft.forward(buf.data());
        double e = 0.0;
        for (int k = k0; k <= k1; ++k) e += std::norm(buf[static_cast<std::size_t>(k)]);
        env.push_back(e / wsum2 / static_cast<double>(k1 - k0 + 1));
    }
    const std::size_t T = env.size();
    std::vector<double> envDb(T);
    for (std::size_t t = 0; t < T; ++t) envDb[t] = dbOf(env[t]);
    {
        std::vector<double> used;
        for (double e : env) if (e > 1e-19) used.push_back(e);
        if (used.size() > 8) p.steadiness = M::quantileOf(used, 0.10) / std::max(1e-30, M::quantileOf(used, 0.50));
    }
    // slow trend out (1 s moving average), hits blanked
    const std::size_t win = std::max<std::size_t>(8, static_cast<std::size_t>(fs / hop));
    std::vector<double> pre(T + 1, 0.0);
    for (std::size_t t = 0; t < T; ++t) pre[t + 1] = pre[t] + envDb[t];
    const double med = M::quantileOf(envDb, 0.5);
    std::vector<double> det(T, 0.0), kept;
    for (std::size_t t = 0; t < T; ++t) {
        const std::size_t a = t > win / 2 ? t - win / 2 : 0, b = std::min(T, t + win / 2 + 1);
        const double mean = (pre[b] - pre[a]) / static_cast<double>(b - a);
        if (envDb[t] > med + 10.0) continue;   // a hit: blank
        det[t] = envDb[t] - mean;
        kept.push_back(det[t]);
    }
    if (kept.size() < T / 2) return p;
    p.skew = M::skewness(kept);
    p.depthDb = M::quantileOf(kept, 0.95) - M::quantileOf(kept, 0.05);
    std::size_t P = 1;
    while (P < 2 * T) P <<= 1;
    FftD f2;
    f2.prepare(static_cast<int>(P));
    std::vector<std::complex<double>> sp(P, {0.0, 0.0});
    for (std::size_t t = 0; t < T; ++t) sp[t] = {det[t] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(t) / static_cast<double>(T))), 0.0};
    f2.forward(sp.data());
    const double frameRate = fs / hop;
    auto hzOf = [&](std::size_t k) { return static_cast<double>(k) * frameRate / static_cast<double>(P); };
    double bestPow = 0.0, bestHz = 0.0;
    std::size_t bestK = 0;
    for (std::size_t k = 1; k < P / 2; ++k) {
        const double hz = hzOf(k);
        if (hz < cfg::kPulseMinHz || hz > cfg::kPulseMaxHz) continue;
        const double pw = std::norm(sp[k]);
        if (pw > bestPow) { bestPow = pw; bestHz = hz; bestK = k; }
    }
    std::vector<double> rest;
    for (std::size_t k = 1; k < P / 2; ++k) {
        const double hz = hzOf(k);
        if (hz < 1.0 || hz > 90.0 || std::fabs(hz - bestHz) < 1.5) continue;
        rest.push_back(std::norm(sp[k]));
    }
    (void)bestK;
    if (!rest.empty() && bestPow > 0.0) p.prominenceDb = 10.0 * std::log10(bestPow / std::max(1e-30, M::quantileOf(rest, 0.5)));
    p.hz = bestHz;
    return p;
}

} // namespace

const char* kindName(ArtifactKind k) noexcept {
    switch (k) {
    case ArtifactKind::Steady:  return "steady noise";
    case ArtifactKind::Tonal:   return "a tone";
    case ArtifactKind::Flutter: return "a flutter (noise that pulses)";
    case ArtifactKind::None:    break;
    }
    return "nothing clear";
}

const char* confidenceName(Confidence c) noexcept {
    switch (c) {
    case Confidence::High:   return "High";
    case Confidence::Medium: return "Medium";
    case Confidence::Low:    return "Low";
    case Confidence::Untouched: break;
    }
    return "Untouched";
}

// ---------------------------------------------------------------------------------------------
Diagnosis diagnose(const float* L, const float* R, std::size_t n, double fs) {
    Diagnosis d;
    const int nfft = fs > cfg::kHighRateHz ? cfg::kScanFft * 2 : cfg::kScanFft;
    if (n < static_cast<std::size_t>(nfft) * 8) { d.notes = "The file is too short to scan."; return d; }
    const auto mid = sumOf(L, R, n, 0.5f, 0.5f), side = sumOf(L, R, n, 0.5f, -0.5f);
    const double lo = std::max(1000.0, cfg::kSearchLowHz - cfg::kBaselineWindowHz * 0.5);
    const double hi = std::min(cfg::kSearchHighHz + cfg::kBaselineWindowHz * 0.5, 0.47 * fs);
    const auto scM = M::scanBand(mid.data(), n, fs, nfft, lo, hi);
    const auto scS = M::scanBand(side.data(), n, fs, nfft, lo, hi);
    if (scM.usedFrames() < 8) { d.notes = "The file is silent or too short to scan."; return d; }
    d.scanned = true;

    const auto qM = M::quantileSpectrum(scM, static_cast<double>(cfg::kScanFloorQuantile));
    const auto qS = M::quantileSpectrum(scS, static_cast<double>(cfg::kScanFloorQuantile));
    std::vector<double> qSum(qM.size());
    for (std::size_t i = 0; i < qM.size(); ++i) qSum[i] = qM[i] + qS[i];
    const double binHz = fs / nfft;
    const int baselineHalf = std::max(8, static_cast<int>(std::lround(cfg::kBaselineWindowHz * 0.5 / binHz)));
    const Excess exSum = excessOf(qSum, baselineHalf);

    const int nb = static_cast<int>(qSum.size());
    auto hzAt = [&](int i) { return scM.binHz(i); };
    int s0 = nb, s1 = -1;
    for (int i = 0; i < nb; ++i) {
        const double f = hzAt(i);
        if (f >= cfg::kSearchLowHz && f <= std::min(cfg::kSearchHighHz, 0.45 * fs)) { s0 = std::min(s0, i); s1 = i; }
    }
    if (s1 < s0) { d.notes = "The search range is outside this file's bandwidth."; return d; }

    int lft = -1, rgt = -1;
    bool tonal = false;
    // 1. a narrow line that stands out of its own neighbourhood?
    {
        int idx = s0;
        double best = -1e9;
        for (int i = s0; i <= s1; ++i) {
            const double fine = exSum.raw[static_cast<std::size_t>(i)] - exSum.smooth[static_cast<std::size_t>(i)];
            if (fine > best) { best = fine; idx = i; }
        }
        const double peak = exSum.raw[static_cast<std::size_t>(idx)];
        if (best >= cfg::kTonalMinOverDb && peak >= cfg::kMinBumpDb) {
            int l = idx, r = idx;
            while (l > 0 && exSum.raw[static_cast<std::size_t>(l - 1)] >= peak - 6.0) --l;
            while (r + 1 < nb && exSum.raw[static_cast<std::size_t>(r + 1)] >= peak - 6.0) ++r;
            const double width = static_cast<double>(r - l + 1) * binHz;
            if (width <= cfg::kTonalMaxWidthHz) {
                tonal = true;
                d.kind = ArtifactKind::Tonal;
                d.bumpDb = peak;
                d.centreHz = hzAt(idx);
                d.widthHz = width;
                lft = l; rgt = r;
            }
        }
    }
    // 2. otherwise a broad bump?
    if (!tonal) {
        int idx = s0;
        double best = -1e9;
        for (int i = s0; i <= s1; ++i)
            if (exSum.smooth[static_cast<std::size_t>(i)] > best) { best = exSum.smooth[static_cast<std::size_t>(i)]; idx = i; }
        if (best < cfg::kMinBumpDb) {
            d.notes = fmt("No clear artifact: the biggest bump in the always-there level between %.0f and %.0f Hz is only %.1f dB (needs %.1f).",
                          cfg::kSearchLowHz, cfg::kSearchHighHz, best, cfg::kMinBumpDb);
            d.bumpDb = best;
            return d;
        }
        int l = idx, r = idx;
        while (l > 0 && exSum.smooth[static_cast<std::size_t>(l - 1)] >= best - 3.0) --l;
        while (r + 1 < nb && exSum.smooth[static_cast<std::size_t>(r + 1)] >= best - 3.0) ++r;
        d.kind = ArtifactKind::Steady;
        d.bumpDb = best;
        d.centreHz = 0.5 * (hzAt(l) + hzAt(r));
        d.widthHz = std::min(cfg::kWidthMaxHz, static_cast<double>(r - l + 1) * binHz);
        lft = l; rgt = r;
    }

    // 3. is it in mid, side, or both?
    {
        const Excess eM = excessOf(qM, baselineHalf), eS = excessOf(qS, baselineHalf);
        double bm = -1e9, bs = -1e9;
        for (int i = lft; i <= rgt; ++i) {
            const auto& a = tonal ? eM.raw : eM.smooth;
            const auto& b = tonal ? eS.raw : eS.smooth;
            bm = std::max(bm, a[static_cast<std::size_t>(i)]);
            bs = std::max(bs, b[static_cast<std::size_t>(i)]);
        }
        d.bumpMidDb = bm;
        d.bumpSideDb = bs;
        d.mode = std::fabs(bm - bs) >= cfg::kMidSideSplitDb ? Mode::MidSide : Mode::Stereo;
    }

    // 4. steady, or does the loudness swing?
    const Pulse pu = pulseAnalysis(mid, fs, d.centreHz, tonal ? 300.0 : d.widthHz);
    d.steadiness = pu.steadiness;
    d.pulseHz = pu.hz;
    d.pulseDepthDb = pu.depthDb;
    d.pulseProminenceDb = pu.prominenceDb;
    d.pulseSkew = pu.skew;
    if (!tonal && pu.prominenceDb >= cfg::kPulseMinProminenceDb && pu.depthDb >= cfg::kPulseMinDepthDb && std::fabs(pu.skew) < cfg::kPulseMaxSkew)
        d.kind = ArtifactKind::Flutter;

    d.notes = fmt("Found %s around %.0f Hz (about %.0f Hz wide), %.1f dB above the normal level around it.", 0, d.centreHz, d.widthHz, d.bumpDb);
    {
        std::string k = kindName(d.kind);
        d.notes = "Found " + k + " around " + fmt("%.0f", d.centreHz) + " Hz (about " + fmt("%.0f", d.widthHz) + " Hz wide), " +
                  fmt("%.1f", d.bumpDb) + " dB above the normal level around it.";
    }
    return d;
}

// ---------------------------------------------------------------------------------------------
void renderWithEngine(HissTamerEngine& eng, const float* L, const float* R, std::size_t n, std::size_t primeFrames,
                      std::vector<float>& oL, std::vector<float>& oR) {
    const std::size_t lat = eng.latency();
    if (primeFrames > 0) eng.prime(L, R, static_cast<std::uint32_t>(std::min(primeFrames, n)));
    else eng.reset();
    oL.assign(n + lat, 0.0f);
    oR.assign(n + lat, 0.0f);
    std::copy(L, L + n, oL.begin());
    std::copy(R, R + n, oR.begin());
    constexpr std::size_t kBlock = 512;
    for (std::size_t i = 0; i < oL.size(); i += kBlock)
        eng.process(oL.data() + i, oR.data() + i, static_cast<std::uint32_t>(std::min(kBlock, oL.size() - i)));
    oL.erase(oL.begin(), oL.begin() + static_cast<std::ptrdiff_t>(lat));
    oR.erase(oR.begin(), oR.begin() + static_cast<std::ptrdiff_t>(lat));
}

namespace {

struct Segment { std::size_t start, len; };

CheckResult measure(const std::vector<float>& beforeMid, const std::vector<float>& afterMid, const std::vector<float>& beforeSide,
                    const std::vector<float>& afterSide, double fs, const Params& p, ArtifactKind kind, std::size_t skipFrames) {
    CheckResult r;
    const std::size_t n = beforeMid.size();
    r.measured = true;
    const double halfW = 0.5 * std::max(static_cast<double>(p.widthHz), 300.0);
    const double lo = static_cast<double>(p.focusHz) - halfW, hi = static_cast<double>(p.focusHz) + halfW;
    {   // the drop is read in the channel where the artifact is louder (mid, or side for a side-only problem)
        const auto bm = M::bandFloor(beforeMid.data(), n, fs, lo, hi), am = M::bandFloor(afterMid.data(), n, fs, lo, hi);
        const auto bs = M::bandFloor(beforeSide.data(), n, fs, lo, hi), as = M::bandFloor(afterSide.data(), n, fs, lo, hi);
        r.dropDb = bs.floorDb > bm.floorDb ? bs.floorDb - as.floorDb : bm.floorDb - am.floorDb;
    }
    const auto ons = M::findOnsets(beforeMid.data(), n, fs);
    const auto hs = M::compareHits(beforeMid.data(), afterMid.data(), n, fs, ons);
    r.hitsChecked = hs.hits;
    r.peakLossDb = hs.worstPeakLossDb;
    r.envelopeErrDb = hs.envRmsErrDb;
    r.attackShiftMs = hs.maxAttackShiftMs;
    r.outOfBandDb = M::outOfBand(beforeMid.data(), afterMid.data(), n, fs, static_cast<double>(p.focusHz), halfW + cfg::kSkirtHz + 300.0).maxAbsDb;
    if (kind != ArtifactKind::Tonal && n > skipFrames + 8192)
        r.musicalNoise = M::musicalNoiseRatio(beforeMid.data() + skipFrames, afterMid.data() + skipFrames, n - skipFrames, fs, lo - 250.0, hi + 250.0, 0.25);
    r.levelDiffDb = M::levelDifferenceDb(beforeMid.data(), afterMid.data(), n);

    r.passed = true;
    auto fail = [&](const std::string& why) { if (r.passed) r.failReason = why; r.passed = false; };
    if (r.peakLossDb < -cfg::kMaxPeakLossDb) fail("a cymbal hit lost " + fmt("%.2f", -r.peakLossDb) + " dB at its peak (limit " + fmt("%.2f", cfg::kMaxPeakLossDb) + ")");
    if (r.envelopeErrDb > cfg::kMaxEnvelopeErrDb) fail("a hit's loudness curve changed by " + fmt("%.2f", r.envelopeErrDb) + " dB (limit " + fmt("%.2f", cfg::kMaxEnvelopeErrDb) + ")");
    if (r.attackShiftMs > cfg::kMaxAttackShiftMs) fail("a hit's attack moved by " + fmt("%.2f", r.attackShiftMs) + " ms (limit " + fmt("%.2f", cfg::kMaxAttackShiftMs) + ")");
    if (r.outOfBandDb > cfg::kMaxOutOfBandDb) fail("the sound outside the focus changed by " + fmt("%.3f", r.outOfBandDb) + " dB (limit " + fmt("%.3f", cfg::kMaxOutOfBandDb) + ")");
    if (r.musicalNoise > cfg::kMaxMusicalNoise) fail("the result sounds watery (score " + fmt("%.2f", r.musicalNoise) + ", limit " + fmt("%.2f", cfg::kMaxMusicalNoise) + ")");
    return r;
}

std::vector<Segment> pickSegments(const std::vector<float>& mid, std::size_t n, double fs, const AutoOptions& opt) {
    const auto chunk = static_cast<std::size_t>(opt.chunkSeconds * fs);
    if (n <= chunk * static_cast<std::size_t>(opt.chunkCount) + chunk / 2 || opt.chunkCount <= 1) return {{0, n}};
    const std::size_t segs = n / chunk;
    std::vector<double> rms(segs), onsets(segs, 0.0);
    for (std::size_t s = 0; s < segs; ++s) rms[s] = M::rmsDb(mid.data() + s * chunk, chunk);
    for (std::size_t o : M::findOnsets(mid.data(), n, fs, 512)) {
        const std::size_t s = o / chunk;
        if (s < segs) onsets[s] += 1.0;
    }
    std::vector<std::size_t> pick;
    auto add = [&](std::size_t s) { if (std::find(pick.begin(), pick.end(), s) == pick.end()) pick.push_back(s); };
    std::size_t quiet = 0, busy = 0;
    for (std::size_t s = 0; s < segs; ++s) {
        if (rms[s] > -110.0 && (rms[quiet] <= -110.0 || rms[s] < rms[quiet])) quiet = s;
        if (onsets[s] > onsets[busy]) busy = s;
    }
    add(quiet);
    add(busy);
    std::vector<std::size_t> order(segs);
    for (std::size_t s = 0; s < segs; ++s) order[s] = s;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return rms[a] < rms[b]; });
    add(order[segs / 2]);
    for (std::size_t s = 0; s < segs && static_cast<int>(pick.size()) < opt.chunkCount; ++s) add(s);
    std::sort(pick.begin(), pick.end());
    std::vector<Segment> out;
    for (std::size_t s : pick) out.push_back({s * chunk, chunk});
    return out;
}

CheckResult mergeWorst(const std::vector<CheckResult>& v) {
    CheckResult m;
    if (v.empty()) return m;
    m = v[0];
    double dropSum = 0.0;
    m.passed = true;
    m.failReason.clear();
    m.peakLossDb = 0.0; m.envelopeErrDb = 0.0; m.attackShiftMs = 0.0; m.outOfBandDb = 0.0; m.musicalNoise = 1.0; m.hitsChecked = 0;
    for (const auto& c : v) {
        dropSum += c.dropDb;
        m.hitsChecked += c.hitsChecked;
        m.peakLossDb = std::min(m.peakLossDb, c.peakLossDb);
        m.envelopeErrDb = std::max(m.envelopeErrDb, c.envelopeErrDb);
        m.attackShiftMs = std::max(m.attackShiftMs, c.attackShiftMs);
        m.outOfBandDb = std::max(m.outOfBandDb, c.outOfBandDb);
        m.musicalNoise = std::max(m.musicalNoise, c.musicalNoise);
        m.levelDiffDb = c.levelDiffDb;
        if (!c.passed && m.passed) { m.passed = false; m.failReason = c.failReason; }
    }
    m.dropDb = dropSum / static_cast<double>(v.size());
    return m;
}

Params settingsFor(const Diagnosis& d, Method method, double reductionDb, double sensitivityShift) {
    Params p;
    p.method = method;
    p.mode = d.mode;
    p.focusHz = static_cast<float>(d.centreHz);
    p.widthHz = static_cast<float>(d.kind == ArtifactKind::Tonal ? 400.0 : std::clamp(d.widthHz, 400.0, cfg::kWidthMaxHz));
    p.reductionDb = static_cast<float>(reductionDb);
    p.mix = 1.0f;
    float sens = d.kind == ArtifactKind::Flutter ? 0.85f : 0.5f;
    sens = std::clamp(sens + static_cast<float>(sensitivityShift), 0.0f, 1.0f);
    p.sensitivity = sens;
    if (d.kind == ArtifactKind::Flutter && d.pulseHz > 0.0) {
        p.smoothMs = static_cast<float>(std::clamp(1200.0 / d.pulseHz, 100.0, 500.0));
        p.steadySeconds = static_cast<float>(std::clamp(3.0 / d.pulseHz, 0.5, 2.0));
    }
    return p;
}

Confidence rate(const Diagnosis& d, const CheckResult& c, double target, int backOffs) {
    if (!c.passed) return Confidence::Untouched;
    const bool tonal = d.kind == ArtifactKind::Tonal;
    if (backOffs > 0 || c.dropDb < cfg::kLowConfidenceDropShare * target || d.bumpDb < cfg::kMinBumpDb + 1.0) return Confidence::Low;
    if (c.dropDb >= cfg::kHighConfidenceDropShare * target && d.bumpDb >= cfg::kHighConfidenceBumpDb && c.peakLossDb > -0.25 &&
        (tonal || c.musicalNoise <= 1.25))
        return Confidence::High;
    return Confidence::Medium;
}

std::string reportFor(const AutoResult& r, double fs) {
    std::ostringstream o;
    const auto& d = r.diagnosis;
    o << "HEM HISS TAMER - AUTO FIX" << (r.name.empty() ? "" : " - " + r.name) << "\n";
    o << "Confidence: " << confidenceName(r.confidence) << "\n\n";
    o << "What I found\n  " << d.notes << "\n";
    if (d.scanned && d.kind != ArtifactKind::None) {
        o << "  Mid " << fmt("%.1f", d.bumpMidDb) << " dB, side " << fmt("%.1f", d.bumpSideDb) << " dB above normal, so I used " << modeName(d.mode) << " mode.\n";
        if (d.kind == ArtifactKind::Flutter)
            o << "  The band's loudness swings about " << fmt("%.1f", d.pulseHz) << " times a second (" << fmt("%.1f", d.pulseDepthDb) << " dB deep).\n";
    }
    o << "\n";
    if (!r.touched) {
        o << "What I did\n  Nothing. The file is returned exactly as it came in.\n";
        o << "  Why: " << (!r.finalCheck.failReason.empty() ? "a safety check said no: " + r.finalCheck.failReason + "."
                                                           : (d.kind == ArtifactKind::None ? "there was no clear artifact to remove, and I never touch a file just to look busy."
                                                                                           : "I could not reach a result that passed every safety check.")) << "\n";
    } else {
        const auto& p = r.chosen;
        o << "What I did\n  Cut up to " << fmt("%.1f", p.reductionDb) << " dB around " << fmt("%.0f", p.focusHz) << " Hz (flat part " << fmt("%.0f", p.widthHz)
          << " Hz wide), method " << methodName(p.method) << ", " << modeName(p.mode) << " mode.\n";
        if (r.backOffSteps > 0) o << "  I had to back the cut off " << r.backOffSteps << " time(s) to stay safe (first target was " << fmt("%.1f", r.targetReductionDb) << " dB).\n";
        o << "  " << (r.candidates.size() > 1 ? "I tried " + std::to_string(r.candidates.size()) + " ways and kept the one that measured best.\n" : "");
        o << "\nWhat I measured on this file (after the fix)\n";
        const auto& c = r.finalCheck;
        o << "  The always-there level in the focus band fell by " << fmt("%.1f", c.dropDb) << " dB.\n";
        o << "  Cymbal and other hits checked: " << c.hitsChecked << ". Worst peak change " << fmt("%.2f", c.peakLossDb) << " dB, loudness curve off by "
          << fmt("%.2f", c.envelopeErrDb) << " dB, attack moved " << fmt("%.2f", c.attackShiftMs) << " ms.\n";
        o << "  Outside the focus the sound changed by at most " << fmt("%.3f", c.outOfBandDb) << " dB.\n";
        if (d.kind != ArtifactKind::Tonal) o << "  Watery score " << fmt("%.2f", c.musicalNoise) << " (1.00 = same texture as the input).\n";
        o << "  Overall level changed by " << fmt("%.4f", c.levelDiffDb) << " dB. (Gain to level-match for A/B: " << fmt("%.4f", r.levelMatchGainDb) << " dB.)\n";
        o << "  The first fraction of a second of the file is left uncut while the guard settles.\n";
    }
    o << "\nWhat I cannot know\n"
      << "  I cannot tell hiss from a steady cymbal wash that is part of the music: both look the same to a detector that learns what is always there.\n"
      << "  I measured with numbers. Only your ears (ideally blind and level-matched) say if it sounds better.\n";
    (void)fs;
    return o.str();
}

} // namespace

// ---------------------------------------------------------------------------------------------
AutoResult autoFix(const float* L, const float* R, std::size_t n, double fs, const AutoOptions& opt, const std::string& name) {
    AutoResult res;
    res.name = name;
    res.left.assign(L, L + n);
    res.right.assign(R, R + n);
    res.removedLeft.assign(n, 0.0f);
    res.removedRight.assign(n, 0.0f);
    auto say = [&](const std::string& s) { if (opt.progress) opt.progress(s); };

    say("scan + diagnose");
    res.diagnosis = diagnose(L, R, n, fs);
    const Diagnosis& d = res.diagnosis;
    if (!d.scanned || d.kind == ArtifactKind::None) { res.report = reportFor(res, fs); return res; }

    const auto mid = sumOf(L, R, n, 0.5f, 0.5f);
    const double target = std::clamp(d.bumpDb - cfg::kLeaveDb, static_cast<double>(cfg::kAutoMinReductionDb), static_cast<double>(cfg::kAutoMaxReductionDb));
    res.targetReductionDb = target;

    std::vector<Method> methods;
    if (d.kind == ArtifactKind::Tonal) { if (opt.tryTonal) methods.push_back(Method::Tonal); }
    else {
        if (opt.tryFloor) methods.push_back(Method::Floor);
        if (opt.tryPercentile) methods.push_back(Method::Percentile);
    }
    if (methods.empty()) { res.report = reportFor(res, fs); return res; }

    const auto segs = pickSegments(mid, n, fs, opt);
    const bool whole = segs.size() == 1 && segs[0].len == n;
    const auto primeFrames = static_cast<std::size_t>(opt.primeSeconds * fs);

    struct Held { Params p; CheckResult c; std::vector<float> oL, oR; bool feasible = false; double score = 0.0; };
    std::vector<Held> held;
    double reduction = target;
    double sensShift = 0.0;
    int step = 0;
    int best = -1;
    for (; step <= cfg::kBackOffSteps; ++step) {
        held.clear();
        best = -1;
        for (Method m : methods) {
            Held h;
            h.p = settingsFor(d, m, reduction, sensShift);
            std::vector<CheckResult> cs;
            for (const auto& sg : segs) {
                HissTamerEngine eng;
                eng.prepare(fs);
                eng.setParams(h.p);
                std::vector<float> oL, oR;
                renderWithEngine(eng, L + sg.start, R + sg.start, sg.len, std::min(primeFrames, sg.len), oL, oR);
                const auto beforeMid = std::vector<float>(mid.begin() + static_cast<std::ptrdiff_t>(sg.start), mid.begin() + static_cast<std::ptrdiff_t>(sg.start + sg.len));
                const auto afterMid = sumOf(oL.data(), oR.data(), sg.len, 0.5f, 0.5f);
                const auto beforeSide = sumOf(L + sg.start, R + sg.start, sg.len, 0.5f, -0.5f);
                const auto afterSide = sumOf(oL.data(), oR.data(), sg.len, 0.5f, -0.5f);
                cs.push_back(measure(beforeMid, afterMid, beforeSide, afterSide, fs, h.p, d.kind, static_cast<std::size_t>(2.0 * fs)));
                if (whole) { h.oL = std::move(oL); h.oR = std::move(oR); }
            }
            h.c = mergeWorst(cs);
            h.feasible = h.c.passed;
            h.score = h.c.dropDb - 2.0 * std::max(0.0, h.c.musicalNoise - 1.0);
            say(std::string("tried ") + methodName(m) + ": drop " + fmt("%.1f", h.c.dropDb) + " dB, " + (h.feasible ? "ok" : "failed: " + h.c.failReason));
            held.push_back(std::move(h));
        }
        for (std::size_t i = 0; i < held.size(); ++i)
            if (held[i].feasible && (best < 0 || held[i].score > held[static_cast<std::size_t>(best)].score)) best = static_cast<int>(i);
        if (best >= 0) break;
        reduction *= cfg::kBackOffFactor;
        sensShift -= 0.2;
    }
    res.backOffSteps = std::min(step, cfg::kBackOffSteps + 1);
    for (const auto& h : held) res.candidates.push_back({h.p, h.c, h.score, h.feasible});
    if (best < 0) {
        res.backOffSteps = cfg::kBackOffSteps;
        res.finalCheck = held.empty() ? CheckResult{} : held[0].c;
        res.report = reportFor(res, fs);
        return res;   // untouched
    }

    // The winner on the whole file (re-used when the trial already was the whole file).
    Held& win = held[static_cast<std::size_t>(best)];
    say("fix + check on the whole file");
    std::vector<float> oL, oR;
    Params p = win.p;
    CheckResult fc;
    for (;;) {
        if (whole && !win.oL.empty() && p.reductionDb == win.p.reductionDb) { oL = win.oL; oR = win.oR; }
        else {
            HissTamerEngine eng;
            eng.prepare(fs);
            eng.setParams(p);
            renderWithEngine(eng, L, R, n, std::min(primeFrames, n), oL, oR);
        }
        const auto afterMid = sumOf(oL.data(), oR.data(), n, 0.5f, 0.5f);
        const auto side = sumOf(L, R, n, 0.5f, -0.5f), afterSide = sumOf(oL.data(), oR.data(), n, 0.5f, -0.5f);
        fc = measure(mid, afterMid, side, afterSide, fs, p, d.kind, static_cast<std::size_t>(2.0 * fs));
        if (fc.passed || res.backOffSteps >= cfg::kBackOffSteps) break;
        ++res.backOffSteps;
        p.reductionDb = static_cast<float>(p.reductionDb * cfg::kBackOffFactor);
        p.sensitivity = std::max(0.0f, p.sensitivity - 0.2f);
        win.p = p;
        win.oL.clear();
    }
    res.finalCheck = fc;
    if (fc.passed && fc.dropDb < cfg::kMinUsefulDropDb) {
        fc.passed = false;
        fc.failReason = "the fix only lowered the always-there level by " + fmt("%.1f", fc.dropDb) + " dB (not worth changing the file for, minimum " + fmt("%.1f", cfg::kMinUsefulDropDb) + ")";
        res.finalCheck = fc;
    }
    if (!fc.passed) { res.report = reportFor(res, fs); return res; }   // untouched

    res.touched = true;
    res.chosen = p;
    res.left = std::move(oL);
    res.right = std::move(oR);
    for (std::size_t i = 0; i < n; ++i) {
        res.removedLeft[i] = L[i] - res.left[i];
        res.removedRight[i] = R[i] - res.right[i];
    }
    res.levelMatchGainDb = -fc.levelDiffDb;
    res.confidence = rate(d, fc, res.targetReductionDb, res.backOffSteps);
    res.report = reportFor(res, fs);
    return res;
}

std::vector<AutoResult> autoFixStems(const std::vector<StemRef>& stems, double fs, const AutoOptions& opt) {
    std::vector<AutoResult> out;
    for (const auto& s : stems) {
        if (opt.progress) opt.progress("stem " + s.name);
        out.push_back(autoFix(s.left->data(), s.right->data(), s.left->size(), fs, opt, s.name));
    }
    return out;
}

std::string stemsReport(const std::vector<AutoResult>& results) {
    std::ostringstream o;
    int touched = 0;
    for (const auto& r : results) touched += r.touched ? 1 : 0;
    o << "HEM HISS TAMER - AUTO FIX, " << results.size() << " stem(s), " << touched << " fixed, " << (results.size() - static_cast<std::size_t>(touched)) << " left exactly as they were\n\n";
    for (const auto& r : results) {
        o << "* " << (r.name.empty() ? "(unnamed)" : r.name) << ": " << confidenceName(r.confidence);
        if (r.touched) o << " - cut " << fmt("%.1f", r.finalCheck.dropDb) << " dB near " << fmt("%.0f", r.chosen.focusHz) << " Hz (" << kindName(r.diagnosis.kind) << ")";
        else o << " - untouched";
        o << "\n";
    }
    o << "\n";
    for (const auto& r : results) o << r.report << "\n----------------------------------------\n\n";
    return o.str();
}

} // namespace hc::hiss
