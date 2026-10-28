#include "hybrid/hiss/HissCombAuto.hpp"

#include "hybrid/hiss/HissMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hc::hiss {

namespace {

constexpr int    kNfft = 65536;
constexpr double kGridTolHz = 1.5;
constexpr double kScanLoHz = 3000.0, kScanHiHz = 20000.0;

struct ChanScan { int lines = 0, onGrid = 0; double depth = 0.0; bool ok = false; };

ChanScan scanChannel(const std::vector<float>& x, double fs) {
    ChanScan out;
    const auto s = metrics::scanBand(x.data(), x.size(), fs, kNfft, kScanLoHz, std::min(kScanHiHz, 0.5 * fs - 500.0));
    if (s.frames < 4 || s.usedFrames() < 4) return out;
    out.ok = true;
    const auto q = metrics::quantileSpectrum(s, 0.10);
    const int n = static_cast<int>(q.size());
    std::vector<double> db(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) db[static_cast<std::size_t>(i)] = 10.0 * std::log10(q[static_cast<std::size_t>(i)] + 1e-30);
    // "their surroundings": running median over about 590 Hz
    const double binHz = fs / kNfft;
    int half = static_cast<int>(std::round(295.0 / binHz));
    half = std::max(10, half);
    std::vector<double> ex(static_cast<std::size_t>(n)), win;
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - half), b = std::min(n - 1, i + half);
        win.assign(db.begin() + a, db.begin() + b + 1);
        std::nth_element(win.begin(), win.begin() + static_cast<long>(win.size() / 2), win.end());
        ex[static_cast<std::size_t>(i)] = db[static_cast<std::size_t>(i)] - win[win.size() / 2];
    }
    // lines: local maxima at least kCombLineMinDb high, at least ~15 Hz apart (strongest wins)
    const int minDist = std::max(2, static_cast<int>(std::round(14.6 / binHz)));
    std::vector<int> cand;
    for (int i = 1; i + 1 < n; ++i) {
        const double v = ex[static_cast<std::size_t>(i)];
        if (v >= cfg::kCombLineMinDb && v >= ex[static_cast<std::size_t>(i - 1)] && v > ex[static_cast<std::size_t>(i + 1)]) cand.push_back(i);
    }
    std::vector<int> order(cand.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ex[static_cast<std::size_t>(cand[static_cast<std::size_t>(a)])] > ex[static_cast<std::size_t>(cand[static_cast<std::size_t>(b)])]; });
    std::vector<char> taken(cand.size(), 0);
    double depthSum = 0.0;
    for (int oi : order) {
        const int idx = cand[static_cast<std::size_t>(oi)];
        bool clash = false;
        for (std::size_t k = 0; k < cand.size() && !clash; ++k)
            if (taken[k] && std::abs(cand[k] - idx) < minDist) clash = true;
        if (clash) continue;
        taken[static_cast<std::size_t>(oi)] = 1;
        ++out.lines;
        const double hz = s.binHz(idx);
        const double r = hz / cfg::kCombSpacingHz;
        if (std::abs(hz - std::round(r) * cfg::kCombSpacingHz) < kGridTolHz) { ++out.onGrid; depthSum += std::pow(10.0, ex[static_cast<std::size_t>(idx)] / 10.0) - 1.0; }
    }
    out.depth = out.onGrid && depthSum > 0.0 ? 10.0 * std::log10(depthSum) : 0.0;     // comb strength: all on-grid line excess added up, in dB
    return out;
}

std::vector<float> mixOf(const float* a, const float* b, std::size_t n, float sb) {
    std::vector<float> o(n);
    for (std::size_t i = 0; i < n; ++i) o[i] = 0.5f * (a[i] + sb * b[i]);
    return o;
}

double rmsDb(const std::vector<float>& a, const std::vector<float>& b) {
    double e = 0.0;
    const std::size_t n = a.size();
    for (std::size_t i = 0; i < n; ++i) e += static_cast<double>(a[i]) * a[i] + static_cast<double>(b[i]) * b[i];
    return 10.0 * std::log10(e / (2.0 * static_cast<double>(std::max<std::size_t>(1, n))) + 1e-30);
}

// samples inside runs of exact digital silence (>= 32 zeros in a row) are put back to the input value (0)
void restoreSilence(const float* inL, const float* inR, std::vector<float>& oL, std::vector<float>& oR) {
    const std::size_t n = oL.size();
    constexpr std::size_t kRun = 32;
    for (int ch = 0; ch < 2; ++ch) {
        const float* in = ch == 0 ? inL : inR;
        auto& o = ch == 0 ? oL : oR;
        std::size_t i = 0;
        while (i < n) {
            if (in[i] != 0.0f) { ++i; continue; }
            std::size_t j = i;
            while (j < n && in[j] == 0.0f) ++j;
            if (j - i >= kRun) for (std::size_t k = i; k < j; ++k) o[k] = 0.0f;
            i = j;
        }
    }
}

} // namespace

CombDiagnosis diagnoseComb(const float* left, const float* right, std::size_t frames, double sampleRate) {
    CombDiagnosis d;
    if (frames < static_cast<std::size_t>(kNfft) * 4 || sampleRate < 32000.0) { d.notes = "file too short to scan"; return d; }
    const auto mid = mixOf(left, right, frames, 1.0f), side = mixOf(left, right, frames, -1.0f);
    const ChanScan m = scanChannel(mid, sampleRate), s = scanChannel(side, sampleRate);
    if (!m.ok && !s.ok) { d.notes = "file is silent"; return d; }
    d.scanned = true;
    const auto score = [](const ChanScan& c) { return c.ok ? c.onGrid : -1; };
    const ChanScan& w = score(s) > score(m) ? s : m;
    d.sideLouder = score(s) > score(m);
    d.lines = w.lines;
    d.onGrid = w.onGrid;
    d.chanceOnGrid = 2.0 * kGridTolHz / cfg::kCombSpacingHz * w.lines;
    d.gridShare = w.lines ? static_cast<double>(w.onGrid) / w.lines : 0.0;
    d.depthDb = w.depth;
    d.found = w.onGrid >= cfg::kCombMinLines && d.gridShare >= cfg::kCombMinGridShare;
    char b[240];
    std::snprintf(b, sizeof b, "%d narrow lines in the quiet floor, %d on the 25 Hz grid (luck would give about %.0f)", d.lines, d.onGrid, d.chanceOnGrid);
    d.notes = b;
    return d;
}

CombResult autoFixComb(const float* left, const float* right, std::size_t frames, double sampleRate, const CombOptions& opt, const std::string& name) {
    CombResult r;
    r.name = name;
    r.left.assign(left, left + frames);
    r.right.assign(right, right + frames);
    r.removedLeft.assign(frames, 0.0f);
    r.removedRight.assign(frames, 0.0f);
    const auto say = [&](const std::string& s) { if (opt.progress) opt.progress(s); };
    say("scanning for the 25 Hz comb");
    r.before = diagnoseComb(left, right, frames, sampleRate);
    char b[600];
    if (!r.before.scanned) {
        r.report = name + ": left alone (" + r.before.notes + ").\n";
        return r;
    }
    if (!r.before.found && !opt.force) {
        std::snprintf(b, sizeof b, "%s: no comb found (%s). Left alone, bit for bit.\n", name.c_str(), r.before.notes.c_str());
        r.report = b;
        return r;
    }
    HissCombEngine probe;
    probe.prepare(sampleRate);
    if (!probe.supported()) {
        r.report = name + ": comb found, but this sample rate has no whole-sample 25 Hz period. Left alone.\n";
        return r;
    }
    double red = opt.reductionDb;
    std::string lastFail;
    for (int attempt = 0; attempt <= 2; ++attempt, red *= 0.5) {
        say("removing the comb, cut " + std::to_string(static_cast<int>(red + 0.5)) + " dB");
        HissCombEngine e;
        e.prepare(sampleRate);
        CombParams p;
        p.reductionDb = static_cast<float>(red);
        e.setParams(p);
        e.prime(left, right, static_cast<std::uint32_t>(frames));
        std::vector<float> oL(left, left + frames), oR(right, right + frames);
        for (std::size_t i = 0; i < frames; i += 512) e.process(oL.data() + i, oR.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(512, frames - i)));
        restoreSilence(left, right, oL, oR);

        CombCheck c;
        c.measured = true;
        std::vector<float> dL(frames), dR(frames);
        double pk = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            dL[i] = left[i] - oL[i];
            dR[i] = right[i] - oR[i];
            pk = std::max({pk, static_cast<double>(std::abs(dL[i])), static_cast<double>(std::abs(dR[i]))});
        }
        c.removedRmsDb = rmsDb(dL, dR);
        c.peakChangeDb = 20.0 * std::log10(pk + 1e-30);
        {   // how much of what was taken out lives below 2.5 kHz (it should be nothing)
            const auto all = metrics::scanBand(dL.data(), frames, sampleRate, 8192, 20.0, 0.5 * sampleRate - 100.0);
            const auto low = metrics::scanBand(dL.data(), frames, sampleRate, 8192, 20.0, 2500.0);
            double ea = 0.0, el = 0.0;
            for (float v : all.power) ea += v;
            for (float v : low.power) el += v;
            c.lowBandShare = ea > 0.0 ? el / ea : 0.0;
        }
        const auto after = diagnoseComb(oL.data(), oR.data(), frames, sampleRate);
        c.linesBefore = r.before.lines; c.onGridBefore = r.before.onGrid; c.depthBeforeDb = r.before.depthDb;
        c.linesAfter = after.lines; c.onGridAfter = after.onGrid; c.depthAfterDb = after.depthDb;
        const double fell = r.before.onGrid > 0 ? 1.0 - static_cast<double>(after.onGrid) / r.before.onGrid : 0.0;
        if (opt.force) c.failReason.clear();
        else if (fell < cfg::kCombMinFall) c.failReason = "the comb did not get clearly shallower";
        else if (c.removedRmsDb > cfg::kCombMaxPatternDb) c.failReason = "the part taken out is too loud to be the comb";
        else if (c.lowBandShare > cfg::kCombMaxLowShare) c.failReason = "something below 2.5 kHz was changed";
        c.passed = c.failReason.empty();
        if (c.passed) {
            r.touched = true;
            r.reductionDb = red;
            r.backOffSteps = attempt;
            r.check = c;
            r.left = std::move(oL);
            r.right = std::move(oR);
            r.removedLeft = std::move(dL);
            r.removedRight = std::move(dR);
            r.confidence = fell >= 0.70 ? Confidence::High : (fell >= 0.40 ? Confidence::Medium : Confidence::Low);
            std::snprintf(b, sizeof b,
                "%s: comb found (%s). Removed it with a %.1f dB cut.\n"
                "   comb lines on the grid: %d -> %d   (comb strength: %.1f dB -> %.1f dB)\n"
                "   what was taken out: %.1f dBFS RMS, %.4f of it below 2.5 kHz, biggest single change %.1f dBFS\n"
                "   silence stays exactly silent. Confidence: %s.\n",
                name.c_str(), r.before.notes.c_str(), red, c.onGridBefore, c.onGridAfter, c.depthBeforeDb, c.depthAfterDb,
                c.removedRmsDb, c.lowBandShare, c.peakChangeDb, confidenceName(r.confidence));
            r.report = b;
            return r;
        }
        lastFail = c.failReason;
        r.check = c;
    }
    std::snprintf(b, sizeof b, "%s: comb found (%s), but the fix did not pass its own checks (%s). Left alone, bit for bit.\n", name.c_str(), r.before.notes.c_str(), lastFail.c_str());
    r.report = b;
    return r;
}

} // namespace hc::hiss
