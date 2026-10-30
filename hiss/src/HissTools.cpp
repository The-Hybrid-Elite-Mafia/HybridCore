#include "hybrid/hiss/HissTools.hpp"

#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissMeters.hpp"

#include <cmath>
#include <cstdio>

namespace hc::hiss::tools {
namespace {

std::vector<float> mono(const float* l, const float* r, std::size_t n) {
    std::vector<float> m(n);
    for (std::size_t i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
    return m;
}
const char* shapeName(meters::EndShape s) { return s == meters::EndShape::Wall ? "WALL" : s == meters::EndShape::Gentle ? "GENTLE" : "NONE"; }
const std::string kSyn = "TESTED";    // checked on made-up signals only

// Tolerances = the limits the tests in tests/hiss_tests.cpp and tests/lab_tests.cpp used, or the measured gap to an outside tool.
// They are ranges seen on made-up signals and on 14 real files for the old meters. They are not promises about other files.

} // namespace

ToolOutput hissScanner(const float* left, const float* right, std::size_t n, double fs, const std::string& name) {
    ToolOutput o;
    const auto x = mono(left, right, n);
    auto& f = o.findings;
    namespace M = meters;
    if (n < static_cast<std::size_t>(fs)) f.push_back(report::refused("whole scan", "clip shorter than 1 s"));
    else {
        f.push_back(report::withAbsRange("Loudness (integrated)", M::integratedLufs(x.data(), n, fs), 0.01, "LUFS", kSyn, "checked against libebur128 on 14 files, gap 0.0000"));
        f.push_back(report::withAbsRange("True peak", M::truePeakDbfs(x.data(), n), 0.01, "dBFS", kSyn, "libebur128 gap at most 0.008 dB on 14 files"));
        f.push_back(report::withAbsRange("RMS", M::rmsDbfs(x.data(), n), 0.01, "dBFS", kSyn));
        f.push_back(report::withAbsRange("Band 8-12 kHz", M::bandRmsDbfs(x.data(), n, fs, 8000, 12000), 0.11, "dBFS", kSyn, "ffmpeg within 0.109 dB on 14 files"));
        f.push_back(report::withAbsRange("Band 12-16 kHz", M::bandRmsDbfs(x.data(), n, fs, 12000, 16000), 0.11, "dBFS", kSyn));
        f.push_back(report::withAbsRange("Band 16-19 kHz", M::bandRmsDbfs(x.data(), n, fs, 16000, 19000), 0.11, "dBFS", kSyn));
        const auto c = M::cutoffAudit(x.data(), n, fs);
        if (!c.valid) f.push_back(report::refused("Where the sound ends", "clip too short for the bandwidth audit"));
        else {
            char b[160]; std::snprintf(b, sizeof b, "shape %s; reaches 19 kHz: %s", shapeName(c.shape), c.reaches19k() ? "yes" : "NO (a file that stops short of 19 kHz cannot be judged on 16-20 kHz air)");
            f.push_back(report::withAbsRange("Where the sound ends", c.cutoffHz, 300.0, "Hz", kSyn, b));
        }
        const auto r = M::ratioMetrics(x.data(), n, fs);
        if (!r.valid) f.push_back(report::refused("Band levels A, B, C and the 16-20 over 5-10 kHz ratio", "fewer than 20 loud frames, nothing is reported"));
        else {
            f.push_back(report::withAbsRange("Loud frames used", r.loud, 0.0, "frames", kSyn));
            f.push_back(report::withAbsRange("A: 5-10 kHz, loud parts", r.a5to10Db, 0.01, "dB", kSyn));
            f.push_back(report::withAbsRange("B: 16-20 kHz, loud parts", r.b16to20Db, 0.01, "dB", kSyn));
            f.push_back(report::withAbsRange("C: 8-20 kHz, loud parts", r.c8to20Db, 0.01, "dB", kSyn));
            f.push_back(report::withAbsRange("Ratio 16-20 over 5-10 kHz", r.ratio16_20Over5_10Db, 0.01, "dB", kSyn, "can rise because A fell; read it next to A and B"));
        }
    }
    o.text = report::makeReport("Hiss Scanner: " + name, f, {
        "The loud-part rule can flip on peaky clips if the file is changed afterwards (FAILURES.md F3). For before/after, hold the loud frames fixed.",
        "Every number was checked on made-up signals and on 14 real files for the old meters. Other real files are UNKNOWN.",
        "Nothing here says anything sounds better or worse. It measures."});
    return o;
}

ToolOutput combAndClickFinder(const float* left, const float* right, std::size_t n, double fs, const std::string& name, double clickPeriodMs) {
    ToolOutput o;
    const auto x = mono(left, right, n);
    auto& f = o.findings;
    namespace M = meters;
    const auto cb = M::combLines(x.data(), n, fs);
    if (!cb.valid) f.push_back(report::refused("25 Hz comb", "clip too short, or the sample rate is not a multiple of 25 Hz"));
    else {
        char b[200]; std::snprintf(b, sizeof b, "%.1f %% of lines beat their neighbours (about 45-50 %% with no comb, clearly higher with one)", cb.onGreaterThanOffPct);
        f.push_back(report::withAbsRange("Comb line level (median, 3-20 kHz)", cb.medianLineDb, 1.0, "dBFS", kSyn, b));
        f.push_back(report::withAbsRange("Lines beating neighbours", cb.onGreaterThanOffPct, 5.0, "%", "ASSUMED", "about 45 for no comb; the +/-5 is a guess"));
    }
    const auto sc = lab::seamClicks(x.data(), n, fs, clickPeriodMs);
    if (!sc.valid) f.push_back(report::refused("Seam clicks", "clip shorter than 0.5 s"));
    else {
        char b[260];
        std::snprintf(b, sizeof b, "%.4g ms fold test: chance a clean file looks this periodic = %.4f (%d other frame lengths used as the null). Call: %s", clickPeriodMs, sc.foldP, sc.nullFolds, sc.pattern ? "YES, a repeating pattern at this period" : "no pattern shown");
        f.push_back(report::noRange("Repeating click pattern at the chosen period", sc.pattern ? 1.0 : 0.0, "(1 = yes)", kSyn, b));
        if (sc.nullFolds > 0) {
            std::snprintf(b, sizeof b, "worked out from THIS file's own high-frequency noise: a single-sample click smaller than this is not shown as a pattern");
            f.push_back(report::noRange("Smallest click this file can show (all same direction)", sc.detectLimitDb, "dBFS", kSyn, b));
            f.push_back(report::noRange("Smallest click this file can show (either direction)", sc.detectLimitAnySignDb, "dBFS", kSyn, b));
            if (sc.pattern) {
                f.push_back(report::withAbsRange("Click size (single-sample peak)", sc.foldLevelDb, 3.0, "dBFS", kSyn, "read from the fold; range from tests on made-up clips"));
                f.push_back(report::withAbsRange("Click position inside the frame", sc.foldPhaseMs, 0.5, "ms", kSyn));
            }
        }
        std::snprintf(b, sizeof b, "single-click rule for this clip: %.1f times the local typical size (from clip length at 1 in 100; assumes bell-shaped local noise)", sc.clickThresholdRatio);
        f.push_back(report::withAbsRange("Single clicks found", static_cast<double>(sc.clicks.size()), 0.05 * static_cast<double>(sc.clicks.size()), "clicks", kSyn, b));
        if (sc.clicks.size() >= 2) {
            f.push_back(report::withAbsRange("Median gap between clicks", sc.medianIntervalMs, 0.1, "ms", kSyn));
            f.push_back(report::withAbsRange("Share of gaps at the chosen period (or multiple)", 100.0 * sc.periodicShare, 5.0, "%", kSyn, "information only; the pattern call uses the fold test above"));
        }
    }
    o.text = report::makeReport("Comb and Click Finder: " + name, f, {
        "The click finder was tested on clicks we put into noise. Music with sharp drum attacks above 3 kHz can look like clicks. The smallest click it can show is worked out per file (line above). Music with a steady beat that divides evenly by the chosen period can also fold strongly. Real files: UNKNOWN.",
        "The 25 Hz comb meter confuses grid music with comb unless lines are compared to their neighbours; it does that (FAILURES.md F6)."});
    return o;
}

ToolOutput airAndSmearLab(const float* left, const float* right, std::size_t n, double fs, const std::string& name, double aLo, double aHi, double aDb) {
    ToolOutput o;
    const auto x = mono(left, right, n);
    auto& f = o.findings;
    namespace M = meters;
    const auto fl = M::flutterDepth(x.data(), n, fs);
    if (!fl.valid) f.push_back(report::refused("Flutter at 25 Hz", "clip under 19.5 s, band empty, or the calibration readings disagree"));
    else if (fl.depth < cfg::kFlutterTrustedDepth) f.push_back(report::noRange("Flutter depth at 25 Hz", fl.depth, "", "UNKNOWN", "under 0.15 the meter's accuracy is UNKNOWN (human clips with nothing added read up to 0.16)"));
    else f.push_back(report::withRelRange("Flutter depth at 25 Hz", fl.depth, 0.20, "", kSyn, "27 of 27 trials inside +/-20 % on never-used clips"));
    const auto nt = lab::trackNotch(x.data(), n, fs);
    if (!nt.valid) f.push_back(report::refused("Moving notch", "clip too short"));
    else if (!nt.found) f.push_back(report::nothingFound("Moving notch", "no notch: only " + std::to_string(nt.significantGroups) + " of " + std::to_string(nt.groups) + " readings had a dip rarer than chance (chance of that many in a clean file " + std::to_string(nt.binomP).substr(0, 6) + ")", kSyn));
    else {
        f.push_back(report::withAbsRange("Notch depth", nt.medianDepthDb, 4.0, "dB", kSyn, "a relative figure, it reads about 3 dB under the true depth on made-up dips"));
        f.push_back(report::withAbsRange("Notch frequency (median)", nt.medianFreqHz, 150.0, "Hz", kSyn));
        f.push_back(report::withRelRange("Notch drift", nt.driftHzPerSec, 0.15, "Hz/s", kSyn, "positive = rising"));
    }
    const auto ht = lab::hitsAndTails(x.data(), n, fs);
    if (!ht.valid) f.push_back(report::refused("Hits and tails", "clip shorter than 1 s"));
    else if (ht.hits.empty()) f.push_back(report::nothingFound("Hits and tails", "no hits found in 6-16 kHz, so no tail, no hit average", kSyn));
    else {
        f.push_back(report::withAbsRange("Hits found (6-16 kHz)", static_cast<double>(ht.hits.size()), 0.1 * static_cast<double>(ht.hits.size()), "hits", kSyn));
        if (ht.counted > 0) f.push_back(report::withRelRange("Tail time (median, to fall 20 dB)", ht.medianTailMs, 0.15, "ms", kSyn, "reads 5 to 12 % short on made-up tails"));
        else f.push_back(report::refused("Tail time", "no hit had a countable tail"));
        const auto av = lab::averageHits(x.data(), n, fs);
        if (!av.valid) f.push_back(report::refused("Averaged hit tail", "fewer than 3 hits with a free window around them"));
        else {
            char b2[100]; std::snprintf(b2, sizeof b2, "%d hits averaged", av.hits);
            if (av.tail20Ms >= 0) f.push_back(report::withRelRange("Averaged tail, 20 dB down", av.tail20Ms, 0.15, "ms", kSyn, b2));
            else f.push_back(report::refused("Averaged tail, 20 dB down", "the average never fell 20 dB inside the window"));
        }
    }
    const auto ec = lab::echoCheck(x.data(), n, fs);
    if (!ec.valid) f.push_back(report::refused("Echo / repeat", "clip too short"));
    else if (!ec.found) f.push_back(report::nothingFound("Echo / repeat", "no repeat found: " + std::to_string(ec.agreeing) + " of " + std::to_string(ec.segments) + " parts of the clip agree on one delay. A steady beat also repeats, so a repeat alone is not proof of an artifact", kSyn));
    else {
        f.push_back(report::withAbsRange("Repeat delay", ec.lagMs, 4.0, "ms", kSyn));
        f.push_back(report::withAbsRange("Repeat strength", ec.strength, 0.08, "", kSyn, "about the loudness of the repeat against the original; reads about 0.07 low at -10 dB"));
    }
    const auto wb = lab::highBandWobble(x.data(), n, fs);
    if (wb.valid && wb.detectLimitDepth > 0) {
        char lb[240]; std::snprintf(lb, sizeof lb, "worked out from this clip's own loudness wander and beat at 1 in 100. At the worst rate (%.1f Hz) it is %.3f: a smaller wobble can hide under the beat there", wb.detectLimitWorstHz, wb.detectLimitWorstDepth);
        f.push_back(report::noRange("Smallest wobble depth this clip could show (typical rate)", wb.detectLimitDepth, "", kSyn, lb));
    }
    if (!wb.valid) f.push_back(report::refused("10-16 kHz wobble", "clip under 10 s, band empty, or the calibration did not move"));
    else if (!wb.found) f.push_back(report::nothingFound("10-16 kHz wobble", "no line beats the shuffled copies of this clip (rank test at 1 in 100)", kSyn));
    else {
        int k = 0;
        for (const auto& ln : wb.lines) {
            if (!ln.significant) continue;
            ++k;
            char nm[96], nt3[200];
            std::snprintf(nm, sizeof nm, "Wobble line %d: rate", k);
            std::snprintf(nt3, sizeof nt3, "%s (chance a clean clip shows a line this strong: %.4f)", ln.rhythmLike ? "RHYTHM-LIKE: more at 2x or 3x the rate than a plain wobble gives, so a beat or pattern, not a steady wobble" : "plain wobble (no extra at 2x or 3x)", ln.p);
            f.push_back(report::withAbsRange(nm, ln.freqHz, 0.3, "Hz", kSyn, nt3));
            std::snprintf(nm, sizeof nm, "Wobble line %d: depth", k);
            std::snprintf(nt3, sizeof nt3, "range %.3f to %.3f at 1 in 100, worked out from the leakage in this clip", ln.depthLo, ln.depthHi);
            f.push_back(report::withAbsRange(nm, ln.depth, std::max(ln.depth - ln.depthLo, ln.depthHi - ln.depth), "", kSyn, nt3));
        }
    }
    if (std::equal(left, left + n, right)) f.push_back(report::refused("Left/right phase", "the file is mono (both channels identical), nothing to compare"));
    else {
        const auto ph = lab::phaseConsistency(left, right, n, fs);
        if (!ph.valid) f.push_back(report::refused("Left/right phase", "clip too short"));
        else for (const auto& b : ph.bands) {
            char nm[96]; std::snprintf(nm, sizeof nm, "Phase L-R, %.0f-%.0f Hz", b.loHz, b.hiHz);
            char nt2[160];
            if (!b.significant) {
                std::snprintf(nt2, sizeof nt2, "agreement %.3f does not beat time-shifted copies (their median %.3f): left and right are different sounds here, a phase angle would mean nothing", b.coherence, b.nullMedian);
                f.push_back(report::nothingFound(nm, nt2, kSyn));
            } else {
                std::snprintf(nt2, sizeof nt2, "agreement %.3f (1 = same signal), beats every time-shifted copy (median %.3f)", b.coherence, b.nullMedian);
                f.push_back(report::withAbsRange(nm, b.phaseDeg, 3.0, "deg", kSyn, nt2));
            }
        }
    }
    if (aDb < -150.0) f.push_back(report::refused("Masking", "not run: no artifact level was given"));
    else {
        const auto mk = lab::maskingShare(x.data(), n, fs, aLo, aHi, aDb);
        if (!mk.valid) f.push_back(report::refused("Masking", "too few loud frames"));
        else {
            char b[160]; std::snprintf(b, sizeof b, "ASSUMED model, not a hearing test. Median margin %.1f dB (negative = covered)", mk.medianMarginDb);
            f.push_back(report::noRange("Share of loud frames where the artifact is covered", 100.0 * mk.coveredShare, "%", "ASSUMED", b));
        }
    }
    o.text = report::makeReport("Air and Smear Lab: " + name, f, {
        "Every engine here was checked on signals we made, with a known answer. None has been run against a real Suno file with a known cause. Real files: UNKNOWN.",
        "Music has its own beat. A line marked RHYTHM-LIKE in the wobble section is probably the groove, not a fault. Its depth range is wider because the groove leaks into it.",
        "Masking is an assumed textbook model. Only a blind listening test says what a person hears.",
        "Every yes/no call is a rank test against copies of THIS file at a 1 in 100 false-alarm rate. The masking line uses published textbook constants, not measured ones."});
    return o;
}

} // namespace hc::hiss::tools
