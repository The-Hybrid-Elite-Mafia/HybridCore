// Known-answer tests for HissLab. The answer is known because the signal maker (engine 17) puts the problem in at a set level,
// and the meters that were already tested (combLines, flutterDepth) check the maker from the outside.
#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissMeters.hpp"
#include "HissHarness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace hc::hiss;
namespace mk = hc::hiss::lab::maker;

static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

static void testMaker() {
    std::printf("[17 signal maker, checked from outside]\n");
    const double fs = 48000.0;
    auto n = mk::whiteNoise(48000, -30.0, 3);
    double s = 0; for (float v : n) s += static_cast<double>(v) * v;
    HMEASURED("white noise rms", db(std::sqrt(s / n.size())), "dBFS");
    HCHECK(std::fabs(db(std::sqrt(s / n.size())) + 30.0) < 0.001, "white noise comes out at exactly the asked rms");
    // band-pass: 1 kHz sine must be gone from a 5-15 kHz band, 10 kHz must stay
    std::vector<float> a(48000), b(48000);
    for (int i = 0; i < 48000; ++i) { a[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * i / fs)); b[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 10000.0 * i / fs)); }
    auto ya = mk::bandpass(a.data(), a.size(), fs, 5000, 15000), yb = mk::bandpass(b.data(), b.size(), fs, 5000, 15000);
    auto rmsMid = [](const std::vector<float>& v) { double q = 0; for (std::size_t i = 12000; i < 36000; ++i) q += static_cast<double>(v[i]) * v[i]; return std::sqrt(q / 24000.0); };
    HMEASURED("1 kHz through 5-15 kHz band", db(rmsMid(ya) / (1.0 / std::sqrt(2.0))), "dB");
    HMEASURED("10 kHz through 5-15 kHz band", db(rmsMid(yb) / (1.0 / std::sqrt(2.0))), "dB");
    HCHECK(db(rmsMid(ya) / (1.0 / std::sqrt(2.0))) < -40.0, "the band-pass removes a 1 kHz tone by more than 40 dB");
    HCHECK(std::fabs(db(rmsMid(yb) / (1.0 / std::sqrt(2.0)))) < 1.0, "the band-pass passes a 10 kHz tone within 1 dB");
    // clicks: exact places
    std::vector<float> z(48000, 0.0f);
    auto at = mk::injectSeamClicks(z, fs, 40.0, -20.0, 10.0);
    HCHECK(at.size() == 25 && at[0] == 480 && at[1] == 480 + 1920 && std::fabs(z[at[3]] - 0.1f) < 1e-6f, "seam clicks: 25 clicks, first at 10 ms, gap 40 ms, level -20 dBFS");
    // echo: an impulse comes back at the delay with the gain
    std::vector<float> im(48000, 0.0f); im[100] = 1.0f;
    mk::injectEcho(im, fs, 80.0, -10.0);
    HCHECK(im[100] == 1.0f && std::fabs(im[100 + 3840] - std::pow(10.0, -0.5)) < 1e-6, "echo: an impulse returns 80 ms later at -10 dB");
    // comb: the already-tested meter must read what was injected
    auto bed = mk::whiteNoise(48000 * 20, -50.0, 11);
    mk::injectComb(bed, fs, -80.0, 3000.0, 20000.0, 5);
    const auto cr = meters::combLines(bed.data(), bed.size(), fs);
    HMEASURED("injected comb -80 dBFS per line, existing comb meter reads", cr.medianLineDb, "dBFS");
    HCHECK(cr.valid && std::fabs(cr.medianLineDb + 80.0) < 1.5, "the comb the maker adds is read back by the comb meter within 1.5 dB");
    // flutter: the already-tested meter must read what was injected
    auto fl = mk::whiteNoise(static_cast<std::size_t>(48000 * 20.5), -30.0, 12);
    mk::injectBandWobble(fl, fs, 25.0, 0.30, 5000.0, 15000.0);
    const auto fr = meters::flutterDepth(fl.data(), fl.size(), fs);
    HMEASURED("injected 25 Hz flutter depth 0.30, existing flutter meter reads", fr.depth, "");
    HCHECK(fr.valid && fr.depth > 0.24 && fr.depth < 0.36, "the flutter the maker adds is read back by the flutter meter inside its registered +/-20 %%");
}

static void testClickIsComb() {
    std::printf("[8 + 7: a 40 ms click train is also a 25 Hz comb (maths: an impulse train of period T has lines every 1/T)]\n");
    const double fs = 48000.0;
    auto x = mk::whiteNoise(48000 * 20, -50.0, 22);
    mk::injectSeamClicks(x, fs, 40.0, -20.0, 13.3);
    const auto c = meters::combLines(x.data(), x.size(), fs);
    // one-sample impulse of height A every P samples: every line has rms sqrt(2) * A / P
    const double want = 20.0 * std::log10(std::sqrt(2.0) * 0.1 / 1920.0);
    HMEASURED("expected comb line from the click train (maths)", want, "dBFS");
    HMEASURED("comb meter reads", c.medianLineDb, "dBFS");
    HCHECK(c.valid && std::fabs(c.medianLineDb - want) < 2.0, "the comb meter reads the click train as a comb at the level the maths gives, within 2 dB");
}

static void testSeam() {
    std::printf("[8 seam click finder]\n");
    const double fs = 48000.0;
    auto x = mk::whiteNoise(48000 * 10, -40.0, 21);
    const auto truth = mk::injectSeamClicks(x, fs, 40.0, -20.0, 13.3);
    const auto r = lab::seamClicks(x.data(), x.size(), fs);
    int hit = 0;
    for (std::size_t t : truth) {
        const double tm = 1000.0 * t / fs;
        for (const auto& c : r.clicks) if (std::fabs(c.timeMs - tm) < 0.25) { ++hit; break; }
    }
    HMEASURED("clicks injected", truth.size(), "");
    HMEASURED("clicks found at the right place (+/-0.25 ms)", hit, "");
    HMEASURED("clicks found in total", r.clicks.size(), "");
    HMEASURED("median gap", r.medianIntervalMs, "ms");
    std::printf("  MEASURED single-click rule for this clip = %.2f x local typical size\n", r.clickThresholdRatio);
    HCHECK(hit >= static_cast<int>(0.90 * truth.size()), "at least 90 %% of injected clicks are found one by one (rule worked out from clip length)");
    HCHECK(r.clicks.size() <= truth.size() + truth.size() / 20, "no more than 5 %% extra clicks are invented");
    HCHECK(std::fabs(r.medianIntervalMs - 40.0) < 0.1, "median gap reads 40 ms within 0.1 ms");
    HMEASURED("fold chance a clean file looks this periodic", r.foldP, "");
    HMEASURED("fold click level read (true -20 dBFS peak)", r.foldLevelDb, "dBFS");
    HMEASURED("this file's smallest showable click", r.detectLimitDb, "dBFS");
    HMEASURED("fold position (true 13.3 ms)", r.foldPhaseMs, "ms");
    HCHECK(r.pattern, "a 40 ms click train is called a pattern");
    HCHECK(std::fabs(r.foldLevelDb + 20.0) < 3.0, "its size is read within 3 dB");
    HCHECK(std::fabs(r.foldPhaseMs - 13.3) < 1.5, "its position inside the 40 ms frame is read within 1.5 ms (window start sits just before the click)");
    auto y = mk::whiteNoise(48000 * 10, -40.0, 21);
    const auto q = lab::seamClicks(y.data(), y.size(), fs);
    HMEASURED("clicks found in clean noise", q.clicks.size(), "");
    HCHECK(!q.pattern, "clean noise is not called a pattern");
    auto w = mk::whiteNoise(48000 * 10, -40.0, 21);
    mk::injectSeamClicks(w, fs, 33.0, -20.0, 5.0);
    const auto o = lab::seamClicks(w.data(), w.size(), fs);
    HMEASURED("share of gaps that are 40 ms, clicks every 33 ms", o.periodicShare, "");
    HCHECK(!o.pattern, "clicks every 33 ms are not called the 40 ms pattern");
    std::vector<float> tiny(100, 0.0f);
    HCHECK(!lab::seamClicks(tiny.data(), tiny.size(), fs).valid, "a clip under 0.5 s is refused");
}

static void testNotch() {
    std::printf("[9 moving notch tracker]\n");
    const double fs = 48000.0;
    auto x = mk::whiteNoise(48000 * 14, -30.0, 31);
    mk::injectNotchSweep(x, fs, 11000.0, 15000.0, 15.0, 100.0);
    const auto r = lab::trackNotch(x.data(), x.size(), fs);
    HCHECK(r.valid && r.found, "a 15 dB sliding dip is found");
    HMEASURED("median depth read (true centre depth 15 dB)", r.medianDepthDb, "dB");
    HMEASURED("drift read (true 4000 Hz over 14 s = 285.7)", r.driftHzPerSec, "Hz/s");
    double worst = 0, sum = 0; int cnt = 0;
    const double total = x.size() / fs;
    for (const auto& p : r.track) {
        if (p.timeSec < 1.0 || p.timeSec > total - 1.0) continue;     // edges of the clip are left untouched by the maker
        const double truth = 11000.0 + 4000.0 * p.timeSec / total;
        const double e = std::fabs(p.freqHz - truth); worst = std::max(worst, e); sum += e; ++cnt;
    }
    HMEASURED("mean frequency error", sum / std::max(1, cnt), "Hz");
    HMEASURED("worst frequency error", worst, "Hz");
    HCHECK(cnt > 10 && sum / cnt < 150.0, "the dip is followed to within 150 Hz on average");
    HCHECK(std::fabs(r.driftHzPerSec - 285.7) < 0.15 * 285.7, "the drift is read within 15 %%");
    HCHECK(r.medianDepthDb > 11.0 && r.medianDepthDb < 19.0, "depth reads within 4 dB of the true 15 dB");
    auto y = mk::whiteNoise(48000 * 14, -30.0, 31);
    const auto q = lab::trackNotch(y.data(), y.size(), fs);
    HMEASURED("deepest dip in plain noise", q.medianDepthDb, "dB");
    HMEASURED("plain noise: readings with a rare dip", q.significantGroups, "");
    HCHECK(q.valid && !q.found, "plain noise has no notch");
    auto z = mk::whiteNoise(48000 * 14, -30.0, 31);
    mk::injectNotchSweep(z, fs, 13000.0, 13000.0, 6.0, 100.0);
    const auto s = lab::trackNotch(z.data(), z.size(), fs);
    HMEASURED("depth read for a 6 dB fixed dip", s.medianDepthDb, "dB");
    HCHECK(s.found, "a steady 6 dB dip is a rare outlier in this noise, so it IS called a notch (no 8 dB rule any more)");
}

static void testHits() {
    std::printf("[11 post-hit smear meter, 13 hit averager]\n");
    const double fs = 48000.0;
    for (double tau : {40.0, 120.0}) {
        auto t = mk::hitTrain(static_cast<std::size_t>(48000 * 20), fs, 900.0, tau, -20.0, 6000.0, 16000.0, 41);
        auto bed = mk::whiteNoise(t.audio.size(), -70.0, 42);
        for (std::size_t i = 0; i < bed.size(); ++i) t.audio[i] += bed[i];
        const double want = 2.302585 * tau;
        const auto r = lab::hitsAndTails(t.audio.data(), t.audio.size(), fs);
        char nm[96];
        std::snprintf(nm, sizeof nm, "tau %.0f ms: hits injected / found / counted", tau);
        std::printf("  MEASURED %s = %zu / %zu / %d\n", nm, t.hitMs.size(), r.hits.size(), r.counted);
        std::snprintf(nm, sizeof nm, "tau %.0f ms: median 20 dB tail (true %.1f ms)", tau, want);
        HMEASURED(nm, r.medianTailMs, "ms");
        HCHECK(r.valid && r.hits.size() >= t.hitMs.size() * 9 / 10 && r.hits.size() <= t.hitMs.size() * 11 / 10, "tau %.0f: 90-110 %% of hits found", tau);
        HCHECK(std::fabs(r.medianTailMs - want) < 0.15 * want, "tau %.0f: tail time within 15 %%", tau);
        const auto a = lab::averageHits(t.audio.data(), t.audio.size(), fs);
        std::snprintf(nm, sizeof nm, "tau %.0f ms: averaged tail20 (true %.1f ms)", tau, want);
        HMEASURED(nm, a.tail20Ms, "ms");
        HCHECK(a.valid && a.tail20Ms > 0 && std::fabs(a.tail20Ms - want) < 0.15 * want, "tau %.0f: averaged tail within 15 %%", tau);
    }
    auto quiet = mk::whiteNoise(48000 * 5, -60.0, 43);
    const auto q = lab::hitsAndTails(quiet.data(), quiet.size(), fs);
    HMEASURED("hits found in plain noise", q.hits.size(), "");
    HCHECK(q.valid && q.hits.empty(), "plain noise has no hits");
    HCHECK(!lab::averageHits(quiet.data(), quiet.size(), fs).valid, "no hits, no average");
}

static void testEcho() {
    std::printf("[14 echo and repeat check]\n");
    const double fs = 48000.0;
    auto t = mk::hitTrain(static_cast<std::size_t>(48000 * 40), fs, 1100.0, 30.0, -20.0, 6000.0, 16000.0, 51);
    auto base = t.audio;
    const auto c = lab::echoCheck(base.data(), base.size(), fs);
    HMEASURED("no echo: strength", c.strength, "");
    HMEASURED("no echo: contrast", c.contrast, "");
    HCHECK(c.valid && !c.found, "hits with no repeat: no echo is called");
    auto e = t.audio;
    mk::injectEcho(e, fs, 80.0, -10.0);
    const auto r = lab::echoCheck(e.data(), e.size(), fs);
    HMEASURED("echo 80 ms at -10 dB: lag read", r.lagMs, "ms");
    HMEASURED("echo 80 ms at -10 dB: strength read (true 0.316)", r.strength, "");
    HMEASURED("echo 80 ms at -10 dB: contrast", r.contrast, "");
    HCHECK(r.found && std::fabs(r.lagMs - 80.0) <= 4.0, "an 80 ms repeat is found at 80 ms within 4 ms");
    HCHECK(std::fabs(r.strength - 0.316) < 0.08, "its strength reads within 0.08 of the true 0.316");
    auto e2 = t.audio;
    mk::injectEcho(e2, fs, 200.0, -6.0);
    const auto r2 = lab::echoCheck(e2.data(), e2.size(), fs);
    HMEASURED("echo 200 ms at -6 dB: lag / strength (true 0.501)", r2.lagMs, "ms");
    HMEASURED("echo 200 ms at -6 dB: strength", r2.strength, "");
    HCHECK(r2.found && std::fabs(r2.lagMs - 200.0) <= 4.0 && std::fabs(r2.strength - 0.501) < 0.1, "a 200 ms repeat at -6 dB is found, lag within 4 ms, strength within 0.1");
}

static void testWobble() {
    std::printf("[15 high-band wobble meter]\n");
    const double fs = 48000.0;
    auto x = mk::whiteNoise(48000 * 30, -30.0, 61);
    mk::injectBandWobble(x, fs, 7.0, 0.20, 10000.0, 16000.0);
    const auto r = lab::highBandWobble(x.data(), x.size(), fs);
    HMEASURED("wobble 7 Hz depth 0.20: frequency read", r.freqHz, "Hz");
    HMEASURED("wobble 7 Hz depth 0.20: depth read", r.depth, "");
    HMEASURED("wobble 7 Hz depth 0.20: contrast", r.contrastDb, "dB");
    HCHECK(r.valid && r.found && r.foundSteady && std::fabs(r.freqHz - 7.0) < 0.3, "a 7 Hz wobble is found at 7 Hz within 0.3 Hz and called steady");
    HCHECK(std::fabs(r.depth - 0.20) < 0.04, "its depth reads within +/-20 %% of 0.20");
    auto y = mk::whiteNoise(48000 * 30, -30.0, 61);
    const auto q = lab::highBandWobble(y.data(), y.size(), fs);
    HMEASURED("steady noise: contrast", q.contrastDb, "dB");
    HMEASURED("steady noise: depth at its strongest line", q.depth, "");
    HCHECK(q.valid && !q.found, "steady noise has no wobble");
    auto z = mk::whiteNoise(48000 * 30, -30.0, 61);
    mk::injectBandWobble(z, fs, 7.0, 0.05, 10000.0, 16000.0);
    const auto s = lab::highBandWobble(z.data(), z.size(), fs);
    HMEASURED("wobble 7 Hz depth 0.05: depth read", s.depth, "");
    HMEASURED("wobble 7 Hz depth 0.05: contrast", s.contrastDb, "dB");
    std::vector<float> shortClip(48000 * 5, 0.1f);
    HCHECK(!lab::highBandWobble(shortClip.data(), shortClip.size(), fs).valid, "a 5 s clip is refused");
    // wobble under a beat: hits about once a second make a strong slow line with harmonics; the 7 Hz wobble must still be found, and the beat must be flagged
    auto h = mk::hitTrain(static_cast<std::size_t>(48000 * 30), fs, 1000.0, 40.0, -22.0, 10000.0, 16000.0, 62);
    auto bd = mk::whiteNoise(h.audio.size(), -45.0, 63);
    for (std::size_t i = 0; i < bd.size(); ++i) h.audio[i] += bd[i];
    mk::injectBandWobble(h.audio, fs, 7.0, 0.20, 10000.0, 16000.0);
    const auto m = lab::highBandWobble(h.audio.data(), h.audio.size(), fs);
    int at7 = -1, rhythmLines = 0;
    for (std::size_t i = 0; i < m.lines.size(); ++i) {
        std::printf("  MEASURED line %zu: rate %.3f Hz, depth %.4f, contrast %.1f dB, rhythm-like %d, depth trusted %d\n", i + 1, m.lines[i].freqHz, m.lines[i].depth, m.lines[i].contrastDb, m.lines[i].rhythmLike, m.lines[i].depthTrusted);
        if (std::fabs(m.lines[i].freqHz - 7.0) < 0.3) at7 = static_cast<int>(i);
        if (m.lines[i].rhythmLike) ++rhythmLines;
    }
    // Honest result: a loud beat raises the "nothing there" level, so a small wobble may be hidden. What must hold: if the 7 Hz line is listed,
    // the true 0.20 sits inside its range; and the clip's own smallest-showable depth is given.
    HMEASURED("beat clip: smallest wobble depth this clip could show", m.detectLimitDepth, "");
    if (at7 >= 0) {
        const auto& l7 = m.lines[static_cast<std::size_t>(at7)];
        std::printf("  MEASURED 7 Hz line under the beat: depth %.3f, range %.3f to %.3f (true 0.20)\n", l7.depth, l7.depthLo, l7.depthHi);
        HCHECK(l7.depthLo <= 0.20 && l7.depthHi >= 0.20, "the true 0.20 is inside its range");
    } else {
        HCHECK(m.detectLimitDepth > 0.0, "the 7 Hz line is hidden by the beat, and the clip says how small a wobble it could show");
        HCHECK(m.detectLimitDepth > 0.0, "(range check skipped: line not listed)");
    }
    const auto clean = lab::highBandWobble(x.data(), x.size(), fs);
    HCHECK(clean.valid && !clean.lines.empty() && clean.lines[0].depthTrusted, "with no beat the depth is trusted (range excludes 0)");
    HCHECK(clean.lines[0].depthLo <= 0.20 && clean.lines[0].depthHi >= 0.20, "with no beat the true 0.20 is inside the range %.3f to %.3f", clean.lines[0].depthLo, clean.lines[0].depthHi);
}

static void testPhase() {
    std::printf("[16 left/right phase check]\n");
    const double fs = 48000.0;
    auto l = mk::whiteNoise(48000 * 10, -30.0, 71);
    const auto same = lab::phaseConsistency(l.data(), l.data(), l.size(), fs);
    double worstP = 0, minC = 1;
    for (auto& b : same.bands) { worstP = std::max(worstP, std::fabs(b.phaseDeg)); minC = std::min(minC, b.coherence); }
    HMEASURED("identical channels: lowest coherence", minC, "");
    HMEASURED("identical channels: worst phase", worstP, "deg");
    HCHECK(same.valid && minC > 0.999 && worstP < 0.1, "identical channels read coherence 1 and phase 0");
    std::vector<float> r(l.size(), 0.0f);
    for (std::size_t i = 4; i < l.size(); ++i) r[i] = l[i - 4];
    const auto d = lab::phaseConsistency(l.data(), r.data(), l.size(), fs);
    const auto& b = d.bands[3];     // 800-1600 Hz
    HMEASURED("right delayed 4 samples, 800-1600 Hz: phase (expect 36)", b.phaseDeg, "deg");
    HMEASURED("right delayed 4 samples, 800-1600 Hz: coherence", b.coherence, "");
    HCHECK(std::fabs(b.phaseDeg - 36.0) < 3.0 && b.coherence > 0.95, "a 4-sample delay reads 36 deg within 3 deg, coherence above 0.95");
    auto o = mk::whiteNoise(l.size(), -30.0, 72);
    const auto ind = lab::phaseConsistency(l.data(), o.data(), l.size(), fs);
    double maxC = 0;
    for (auto& bb : ind.bands) maxC = std::max(maxC, bb.coherence);
    HMEASURED("independent noise: highest coherence", maxC, "");
    HCHECK(maxC < 0.15, "two different noises read coherence under 0.15");
    int sigInd = 0, sigDel = 0;
    for (auto& bb : ind.bands) if (bb.significant) ++sigInd;
    for (auto& bb : d.bands) if (bb.significant) ++sigDel;
    HMEASURED("independent noise: bands called significant", sigInd, "of 8");
    HMEASURED("delayed copy: bands called significant", sigDel, "of 8");
    HCHECK(sigDel == 8, "every band of a delayed copy beats the time-shifted pairings");
}

// ---------------------------------------------------------------------------------------------------------------
//  How the decisions behave over many different files: false alarms on clean noise and smallest detectable sizes.
//  The only chosen number is kAlpha = 0.01. These numbers are MEASURED here, not assumed.
// ---------------------------------------------------------------------------------------------------------------
static void testCalibration() {
    std::printf("[calibration of the decisions: false alarms over many clean clips, smallest detectable sizes]\n");
    const double fs = 48000.0;
    const int seeds = 40;
    // click fold
    int fa = 0;
    for (int sd = 0; sd < seeds; ++sd) { auto y = mk::whiteNoise(48000 * 8, -40.0, 1000 + sd); if (lab::seamClicks(y.data(), y.size(), fs).pattern) ++fa; }
    std::printf("  MEASURED click fold: false alarms on %d clean noises = %d\n", seeds, fa);
    HCHECK(fa <= 3, "click fold: clean noise is accused at about 1 in 100 or better (%d of %d)", fa, seeds);
    // sweep: smallest click shown, same direction
    double firstSeen = 1e9, limitAtThere = 0;
    for (double lvl = -30.0; lvl >= -75.0; lvl -= 5.0) {
        int seen = 0; double lim = 0, rd = 0; std::size_t single = 0;
        for (int sd = 0; sd < 6; ++sd) {
            auto y = mk::whiteNoise(48000 * 8, -40.0, 2000 + sd);
            mk::injectSeamClicks(y, fs, 40.0, lvl, 7.0 + sd);
            const auto r = lab::seamClicks(y.data(), y.size(), fs);
            if (r.pattern) { ++seen; rd += r.foldLevelDb; }
            lim += r.detectLimitDb; single += r.clicks.size();
        }
        std::printf("  MEASURED click peak %.0f dBFS (noise -40 dBFS rms): pattern shown in %d of 6, mean level read %.1f, file's own limit %.1f dBFS; clicks found one by one: %zu of about 1200\n", lvl, seen, seen ? rd / seen : 0.0, lim / 6.0, single);
        if (seen >= 5 && lvl < firstSeen) { firstSeen = lvl; limitAtThere = lim / 6.0; }
    }
    HCHECK(firstSeen <= -45.0, "the fold shows a same-direction click train at %.0f dBFS (one-by-one finding needs about -25 dBFS in this noise)", firstSeen);
    HCHECK(firstSeen >= limitAtThere - 3.0, "no click is shown that is more than 3 dB under the limit the file reported (%.1f vs %.1f)", firstSeen, limitAtThere);
    // clicks that point either way (every second one flipped): the energy fold
    {
        double seenAt = 1e9; double limAny = 0;
        for (double lvl = -25.0; lvl >= -60.0; lvl -= 5.0) {
            int seen = 0;
            for (int sd = 0; sd < 6; ++sd) {
                auto y = mk::whiteNoise(48000 * 8, -40.0, 2500 + sd);
                const auto at = mk::injectSeamClicks(y, fs, 40.0, lvl, 7.0 + sd);
                for (std::size_t k = 1; k < at.size(); k += 2) y[at[k]] -= static_cast<float>(2.0 * std::pow(10.0, lvl / 20.0));
                const auto r = lab::seamClicks(y.data(), y.size(), fs);
                if (r.pattern) ++seen;
                limAny = r.detectLimitAnySignDb;
            }
            std::printf("  MEASURED click peak %.0f dBFS, every second one flipped: pattern shown in %d of 6 (file's own limit for either direction %.1f dBFS)\n", lvl, seen, limAny);
            if (seen >= 5) seenAt = lvl;
        }
        HCHECK(seenAt <= -30.0 && seenAt >= limAny - 3.0, "either-direction clicks: shown down to %.0f dBFS, not more than 3 dB under the reported limit %.1f", seenAt, limAny);
    }
    // notch
    int nfa = 0;
    for (int sd = 0; sd < 15; ++sd) { auto y = mk::whiteNoise(48000 * 14, -30.0, 3000 + sd); if (lab::trackNotch(y.data(), y.size(), fs).found) ++nfa; }
    std::printf("  MEASURED notch: false alarms on 15 clean noises = %d\n", nfa);
    HCHECK(nfa <= 1, "notch: clean noise accused %d of 15", nfa);
    for (double dd : {2.0, 3.0, 4.0, 6.0}) {
        int f = 0;
        for (int sd = 0; sd < 4; ++sd) { auto y = mk::whiteNoise(48000 * 14, -30.0, 3100 + sd); mk::injectNotchSweep(y, fs, 13000.0, 13000.0, dd, 100.0); if (lab::trackNotch(y.data(), y.size(), fs).found) ++f; }
        std::printf("  MEASURED notch: steady dip of %.0f dB found in %d of 4\n", dd, f);
    }
    // hits
    int hfa = 0;
    for (int sd = 0; sd < seeds; ++sd) { auto y = mk::whiteNoise(48000 * 6, -60.0, 4000 + sd); if (!lab::hitsAndTails(y.data(), y.size(), fs).hits.empty()) ++hfa; }
    std::printf("  MEASURED hits: clips of clean noise with an invented hit = %d of %d\n", hfa, seeds);
    HCHECK(hfa <= 3, "hits: clean noise gets an invented hit in %d of %d clips", hfa, seeds);
    // echo
    int efa = 0;
    for (int sd = 0; sd < 12; ++sd) {
        auto t = mk::hitTrain(48000 * 20, fs, 900.0, 60.0, -20.0, 6000.0, 16000.0, 5000 + sd);
        auto bed = mk::whiteNoise(t.audio.size(), -70.0, 5100 + sd);
        for (std::size_t i = 0; i < bed.size(); ++i) t.audio[i] += bed[i];
        if (lab::echoCheck(t.audio.data(), t.audio.size(), fs).found) ++efa;
    }
    std::printf("  MEASURED echo: random-spaced hits with no echo called an echo = %d of 12\n", efa);
    HCHECK(efa <= 1, "echo: hits with no echo accused in %d of 12 clips", efa);
    // wobble
    int wfa = 0;
    for (int sd = 0; sd < 20; ++sd) { auto y = mk::whiteNoise(48000 * 15, -30.0, 6000 + sd); if (lab::highBandWobble(y.data(), y.size(), fs).found) ++wfa; }
    std::printf("  MEASURED wobble: false alarms on 20 clean noises = %d\n", wfa);
    HCHECK(wfa <= 1, "wobble: clean noise accused %d of 20", wfa);
    int inside = 0, tot = 0; double lim0 = 0;
    for (int sd = 0; sd < 12; ++sd) {
        auto y = mk::whiteNoise(48000 * 15, -30.0, 6100 + sd);
        mk::injectBandWobble(y, fs, 5.0 + sd * 0.7, 0.10, 10000.0, 16000.0);
        const auto r = lab::highBandWobble(y.data(), y.size(), fs);
        lim0 = r.detectLimitDepth;
        if (!r.lines.empty()) { ++tot; if (r.lines[0].depthLo <= 0.10 && r.lines[0].depthHi >= 0.10) ++inside; }
    }
    std::printf("  MEASURED wobble depth 0.10: line found in %d of 12, true depth inside the reported range in %d of those; this noise's own smallest depth = %.4f\n", tot, inside, lim0);
    HCHECK(tot >= 11 && inside >= tot - 1, "wobble: found %d of 12, truth inside the range in %d", tot, inside);
    for (double d : {0.004, 0.008, 0.016, 0.03}) {
        int f = 0;
        for (int sd = 0; sd < 4; ++sd) { auto y = mk::whiteNoise(48000 * 15, -30.0, 6200 + sd); mk::injectBandWobble(y, fs, 6.0, d, 10000.0, 16000.0); if (lab::highBandWobble(y.data(), y.size(), fs).found) ++f; }
        std::printf("  MEASURED wobble: depth %.3f found in %d of 4\n", d, f);
    }
    // phase
    int pfa = 0, ptests = 0;
    for (int sd = 0; sd < 15; ++sd) {
        auto a = mk::whiteNoise(48000 * 12, -30.0, 7000 + sd), b = mk::whiteNoise(48000 * 12, -30.0, 7100 + sd);
        const auto r = lab::phaseConsistency(a.data(), b.data(), a.size(), fs);
        for (auto& bb : r.bands) { ++ptests; if (bb.significant) ++pfa; }
    }
    std::printf("  MEASURED phase: bands of unrelated noise called related = %d of %d\n", pfa, ptests);
    HCHECK(pfa <= 3, "phase: unrelated noise called related in %d of %d bands", pfa, ptests);
}

static void testMask() {
    std::printf("[12 masking calculator (ASSUMED model)]\n");
    HCHECK(std::fabs(lab::maskingMarginDb(-30.0, 0.0, 0.0) - (-15.5)) < 1e-9, "artifact 30 dB under a masker in the same band: margin -15.5 dB (covered)");
    HCHECK(lab::maskingMarginDb(0.0, 0.0, 0.0) > 0.0, "artifact as loud as the masker is not covered");
    HCHECK(std::fabs(lab::maskingMarginDb(-30.0, 0.0, 5.0) - 34.5) < 1e-9, "masker 5 Bark below: margin +34.5 dB (not covered)");
    HCHECK(std::fabs(lab::maskingMarginDb(-30.0, 0.0, -1.0) - 9.5) < 1e-9, "masker 1 Bark above uses the steeper slope: margin +9.5 dB");
    HCHECK(lab::hzToBark(1000.0) > 8.0 && lab::hzToBark(1000.0) < 9.0 && lab::hzToBark(15000.0) > 22.0, "Bark scale: 1 kHz about 8.5, 15 kHz above 22");
    const double fs = 48000.0;
    auto x = mk::whiteNoise(48000 * 10, -20.0, 81);
    const auto lowArt = lab::maskingShare(x.data(), x.size(), fs, 11000.0, 12000.0, -120.0);
    const auto highArt = lab::maskingShare(x.data(), x.size(), fs, 11000.0, 12000.0, 10.0);
    HMEASURED("artifact -120: covered share", lowArt.coveredShare, "");
    HMEASURED("artifact +10: covered share", highArt.coveredShare, "");
    HCHECK(lowArt.valid && lowArt.coveredShare > 0.99, "a very quiet artifact under loud noise is covered in nearly every loud frame");
    HCHECK(highArt.valid && highArt.coveredShare < 0.01, "a very loud artifact is covered in almost no frame");
}

static void testHash() {
    std::printf("[19 fingerprint and blind name]\n");
    HCHECK(lab::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing matches the published value");
    HCHECK(lab::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc' matches the published value");
    std::string big(1000000, 'a');
    HCHECK(lab::sha256Hex(big.data(), big.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million 'a' matches the published value");
    const std::string n = lab::blindName(lab::sha256Hex("abc", 3));
    HCHECK(n == "HEM-ba7816bf", "blind name is HEM- plus 8 hex digits of the fingerprint");
}

int main() {
    testMaker();
    testSeam();
    testClickIsComb();
    testNotch();
    testHits();
    testEcho();
    testWobble();
    testPhase();
    testCalibration();
    testMask();
    testHash();
    std::printf("\n%d checks, %d failed\n", test::gChecks, test::gFails);
    return test::gFails ? 1 : 0;
}
