// Tests for the HEM Hiss Tamer engine and the HEM Hiss Auto brain.
// Everything is generated in code with fixed seeds, so every run sees the same audio.
// "Known answer" = we added the artifact ourselves, so we know exactly what is in the file.
#include "HissHarness.hpp"
#include "HissTestSignals.hpp"
#include "hybrid/hiss/HissAuto.hpp"
#include "hybrid/hiss/HissCombAuto.hpp"
#include "hybrid/hiss/HissCombEngine.hpp"
#include "hybrid/hiss/HissFft.hpp"
#include "hybrid/hiss/HissMetrics.hpp"
#include "hybrid/hiss/HissMeters.hpp"
#include <random>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using namespace hc::hiss;
namespace M = hc::hiss::metrics;
namespace T = hc::hiss::test;

namespace {

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

std::vector<float> mono(const std::vector<float>& l, const std::vector<float>& r) {
    std::vector<float> m(l.size());
    for (std::size_t i = 0; i < l.size(); ++i) m[i] = 0.5f * (l[i] + r[i]);
    return m;
}

struct Rendered { std::vector<float> L, R; };
Rendered render(const Params& p, const T::Scene& sc, double fs, std::size_t primeSec = 4) {
    HissTamerEngine e;
    e.prepare(fs);
    e.setParams(p);
    Rendered r;
    renderWithEngine(e, sc.L.data(), sc.R.data(), sc.L.size(), primeSec * static_cast<std::size_t>(fs), r.L, r.R);
    return r;
}

Params steadyParams(double reductionDb = 12.0) {
    Params p;
    p.reductionDb = static_cast<float>(reductionDb);
    p.focusHz = 11750.0f;
    p.widthHz = 1000.0f;
    return p;
}

// ---------------------------------------------------------------------------------------------
void testBuildingBlocks() {
    std::printf("[building blocks]\n");
    for (int n : {2048, 4096}) {
        const auto w = HissTamerEngine::makeWindow(n);
        const int hop = n / cfg::kHopDivisor;
        double worst = 0.0;
        for (int i = 0; i < hop; ++i) {
            double s = 0.0;
            for (int k = 0; k < cfg::kHopDivisor; ++k) { const double v = w[static_cast<std::size_t>(i + k * hop)]; s += v * v; }
            worst = std::max(worst, std::fabs(s - 2.0));
        }
        HCHECK(worst < 1e-5, "window overlap-add sum is 2 everywhere for FFT %d (worst error %.2e) = perfect rebuild", n, worst);
    }
    {
        Fft f;
        f.prepare(1024);
        T::Rng rng(5);
        std::vector<std::complex<float>> x(1024), y;
        for (auto& v : x) v = {static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())};
        y = x;
        f.forward(y.data());
        f.inverse(y.data());
        double worst = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(x[i] - y[i])));
        HCHECK(worst < 1e-4, "FFT forward then inverse gives the input back (worst error %.2e)", worst);
        // a pure tone lands in the right bin
        std::vector<std::complex<float>> t(1024);
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = {static_cast<float>(std::cos(2.0 * std::numbers::pi * 100.0 * static_cast<double>(i) / 1024.0)), 0.0f};
        f.forward(t.data());
        std::size_t best = 0;
        for (std::size_t i = 0; i < 512; ++i) if (std::abs(t[i]) > std::abs(t[best])) best = i;
        HCHECK(best == 100, "FFT puts a 100-cycle tone in bin %zu", best);
    }
}

// ---------------------------------------------------------------------------------------------
void testPassThrough() {
    std::printf("[pass-through: bypass, mix 0, nothing to cut]\n");
    T::SceneSpec s;
    s.seconds = 8.0;
    const auto sc = T::makeScene(s);
    const std::size_t n = sc.L.size();
    for (int kind = 0; kind < 3; ++kind) {
        Params p = steadyParams();
        const char* what = kind == 0 ? "bypass on" : (kind == 1 ? "mix 0" : "reduction 0");
        if (kind == 0) p.bypass = true;
        if (kind == 1) p.mix = 0.0f;
        if (kind == 2) p.reductionDb = 0.0f;
        HissTamerEngine e;
        e.prepare(sc.fs);
        e.setParams(p);
        const auto lat = e.latency();
        std::vector<float> L = sc.L, R = sc.R;
        L.resize(n + lat, 0.0f);
        R.resize(n + lat, 0.0f);
        for (std::size_t i = 0; i < L.size(); i += 333) e.process(L.data() + i, R.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(333, L.size() - i)));
        // after the first 0.2 s the output is the input, delayed by the latency, bit for bit
        std::size_t bad = 0;
        for (std::size_t i = static_cast<std::size_t>(0.2 * sc.fs); i < n; ++i)
            if (L[i + lat] != sc.L[i] || R[i + lat] != sc.R[i]) ++bad;
        HCHECK(bad == 0, "%s: output equals the delayed input exactly (%zu different samples)", what, bad);
    }
    // unity: an engine with the cut turned on, fed a file with nothing in the band, changes nothing
    {
        T::SceneSpec c;
        c.seconds = 8.0;
        c.hiss = T::Hiss::None;
        const auto cs = T::makeScene(c);
        const auto r = render(steadyParams(), cs, cs.fs, 0);
        const double diff = M::levelDifferenceDb(cs.L.data(), r.L.data(), cs.L.size());
        const auto oob = M::outOfBand(cs.L.data(), r.L.data(), cs.L.size(), cs.fs, 11750.0, 800.0);
        HMEASURED("level change, clean file", diff, "dB");
        HCHECK(std::fabs(diff) < 0.01, "clean file: overall level moves by only %.4f dB", diff);
        HMEASURED("spectrum change anywhere, clean file", oob.maxAbsDb, "dB");
    }
}

// ---------------------------------------------------------------------------------------------
void testEngineBehaviour() {
    std::printf("[engine on known-answer files]\n");
    // 1. steady hiss alone: how far does the floor drop?
    {
        T::SceneSpec s; s.hits = false; s.bed = false; s.seconds = 20.0;
        const auto sc = T::makeScene(s);
        for (Method m : {Method::Floor, Method::Percentile}) {
            Params p = steadyParams(12.0);
            p.method = m;
            const auto r = render(p, sc, sc.fs);
            const double drop = M::bandFloor(sc.L.data(), sc.L.size(), sc.fs, 11250, 12250).floorDb - M::bandFloor(r.L.data(), r.L.size(), sc.fs, 11250, 12250).floorDb;
            const double mnr = M::musicalNoiseRatio(sc.L.data() + 96000, r.L.data() + 96000, sc.L.size() - 96000, sc.fs, 11000, 12500);
            std::printf("  (%s)\n", methodName(m));
            HMEASURED("steady hiss floor drop", drop, "dB");
            HMEASURED("watery score", mnr, "(1.00 = same texture)");
            HCHECK(drop > 9.0 && drop < 12.6, "%s: steady hiss falls by %.1f dB with a 12 dB limit (needs 9 to 12.6)", methodName(m), drop);
            HCHECK(mnr < 1.5, "%s: watery score %.2f (limit 1.5)", methodName(m), mnr);
        }
    }
    // 2. reduction control scales the cut
    {
        T::SceneSpec s; s.hits = false; s.bed = false; s.seconds = 16.0;
        const auto sc = T::makeScene(s);
        double prev = -1.0;
        bool rising = true;
        for (double red : {3.0, 6.0, 12.0}) {
            const auto r = render(steadyParams(red), sc, sc.fs);
            const double drop = M::bandFloor(sc.L.data(), sc.L.size(), sc.fs, 11250, 12250).floorDb - M::bandFloor(r.L.data(), r.L.size(), sc.fs, 11250, 12250).floorDb;
            std::printf("  reduction %4.1f dB -> floor drop %.2f dB\n", red, drop);
            HCHECK(drop > red * 0.7 && drop < red + 0.8, "reduction %.0f dB gives a drop of %.2f dB", red, drop);
            if (drop <= prev) rising = false;
            prev = drop;
        }
        HCHECK(rising, "a bigger Reduction setting always cuts more");
    }
    // 3. hits survive, nothing outside the focus moves
    {
        T::SceneSpec s; s.seconds = 24.0;
        const auto sc = T::makeScene(s);
        const auto r = render(steadyParams(12.0), sc, sc.fs);
        const auto m0 = mono(sc.L, sc.R), m1 = mono(r.L, r.R);
        const auto ons = M::findOnsets(m0.data(), m0.size(), sc.fs);
        const auto hs = M::compareHits(m0.data(), m1.data(), m0.size(), sc.fs, ons);
        const auto oob = M::outOfBand(m0.data(), m1.data(), m0.size(), sc.fs, 11750.0, 500.0 + cfg::kSkirtHz + 300.0);
        HMEASURED("hits checked", hs.hits, "");
        HMEASURED("worst hit peak change", hs.worstPeakLossDb, "dB");
        HMEASURED("hit loudness-curve error", hs.envRmsErrDb, "dB");
        HMEASURED("hit attack shift", hs.maxAttackShiftMs, "ms");
        HMEASURED("spectrum change outside focus", oob.maxAbsDb, "dB");
        HCHECK(hs.hits >= 8, "found %d hits to check", hs.hits);
        HCHECK(hs.worstPeakLossDb > -0.5, "no hit loses more than 0.5 dB at its peak (worst %.2f)", hs.worstPeakLossDb);
        HCHECK(hs.envRmsErrDb < 1.0, "hit loudness curves stay within 1 dB (%.2f)", hs.envRmsErrDb);
        HCHECK(hs.maxAttackShiftMs < 0.5, "hit attacks move less than 0.5 ms (%.2f)", hs.maxAttackShiftMs);
        HCHECK(oob.maxAbsDb < 0.05, "outside the focus nothing changes by more than 0.05 dB (%.3f)", oob.maxAbsDb);
    }
    // 4. a tone is removed without a watery sound; the tone method leaves the neighbourhood alone
    {
        T::SceneSpec s; s.hiss = T::Hiss::Tone; s.seconds = 16.0;
        const auto sc = T::makeScene(s);
        Params p = steadyParams(12.0);
        p.method = Method::Tonal;
        p.widthHz = 400.0f;
        const auto r = render(p, sc, sc.fs);
        const auto m0 = mono(sc.L, sc.R), m1 = mono(r.L, r.R);
        const auto a = M::bandFloor(m0.data(), m0.size(), sc.fs, 11700, 11900), b = M::bandFloor(m1.data(), m1.size(), sc.fs, 11700, 11900);
        HMEASURED("tone floor drop", a.floorDb - b.floorDb, "dB");
        HCHECK(a.floorDb - b.floorDb > 9.0, "tone falls by %.1f dB", a.floorDb - b.floorDb);
    }
    // 5. listen-to-removed + processed = input
    {
        T::SceneSpec s; s.seconds = 10.0;
        const auto sc = T::makeScene(s);
        Params p = steadyParams(12.0);
        const auto fixedR = render(p, sc, sc.fs, 2);
        p.listenRemoved = true;
        const auto remR = render(p, sc, sc.fs, 2);
        double worst = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.3 * sc.fs); i < sc.L.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(fixedR.L[i]) + remR.L[i] - sc.L[i]));
        HMEASURED("fixed + removed vs input, worst gap", 20.0 * std::log10(worst + 1e-12), "dBFS");
        HCHECK(worst < 1e-4, "'Listen to Removed' plus the fixed sound adds back to the input (worst gap %.2e)", worst);
        const double remRms = M::rmsDb(remR.L.data(), remR.L.size());
        HCHECK(remRms < M::rmsDb(sc.L.data(), sc.L.size()) - 10.0, "what is removed is far quieter than the music (%.1f dBFS)", remRms);
    }
    // 6. mix halfway = halfway
    {
        T::SceneSpec s; s.hits = false; s.bed = false; s.seconds = 14.0;
        const auto sc = T::makeScene(s);
        Params p = steadyParams(12.0);
        p.mix = 0.5f;
        const auto r = render(p, sc, sc.fs);
        const double drop = M::bandFloor(sc.L.data(), sc.L.size(), sc.fs, 11250, 12250).floorDb - M::bandFloor(r.L.data(), r.L.size(), sc.fs, 11250, 12250).floorDb;
        HMEASURED("drop at Mix 50%", drop, "dB");
        HCHECK(drop > 3.0 && drop < 8.0, "Mix 50%% gives a drop of %.1f dB (about 6 dB expected for a 12 dB cut halved in amplitude)", drop);
    }
    // 7. repeatable
    {
        T::SceneSpec s; s.seconds = 8.0;
        const auto sc = T::makeScene(s);
        const auto a = render(steadyParams(), sc, sc.fs, 2), b = render(steadyParams(), sc, sc.fs, 2);
        HCHECK(sameBits(a.L, b.L) && sameBits(a.R, b.R), "same input and settings give the identical output twice");
    }
}

// ---------------------------------------------------------------------------------------------
void testRatesAndSpeed() {
    std::printf("[sample rates, latency, speed]\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        T::SceneSpec s; s.fs = fs; s.seconds = 12.0;
        const auto sc = T::makeScene(s);
        HissTamerEngine e;
        e.prepare(fs);
        e.setParams(steadyParams());
        const auto lat = e.latency();
        const auto r = render(steadyParams(), sc, fs, 3);
        bool finite = true;
        for (float v : r.L) if (!std::isfinite(v)) finite = false;
        const double drop = M::bandFloor(sc.L.data(), sc.L.size(), fs, 11250, 12250).floorDb - M::bandFloor(r.L.data(), r.L.size(), fs, 11250, 12250).floorDb;
        std::printf("  %.0f Hz: latency %u samples = %.2f ms, FFT %d, drop %.1f dB\n", fs, lat, 1000.0 * lat / fs, e.fftSize(), drop);
        HCHECK(finite, "%.0f Hz: output is all finite numbers", fs);
        HCHECK(drop > 9.0, "%.0f Hz: steady hiss falls by %.1f dB", fs, drop);
    }
    {
        T::SceneSpec s; s.seconds = 20.0;
        const auto sc = T::makeScene(s);
        HissTamerEngine e;
        e.prepare(sc.fs);
        Params p = steadyParams();
        p.mode = Mode::MidSide;
        e.setParams(p);
        std::vector<float> L = sc.L, R = sc.R;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < L.size(); i += 256) e.process(L.data() + i, R.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(256, L.size() - i)));
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double load = sec / s.seconds * 100.0;
        HMEASURED("engine CPU load, Mid/Side, 1 core, this machine", load, "% of real time");
        HCHECK(load < 25.0, "engine uses %.2f%% of one core's real time (limit 25%%; UNKNOWN on the i7-14700HX until measured there)", load);
    }
}

// ---------------------------------------------------------------------------------------------
void testNoAllocationAndStress() {
    std::printf("[audio thread: no allocation, odd input, random settings]\n");
    T::SceneSpec s; s.seconds = 6.0;
    const auto sc = T::makeScene(s);
    HissTamerEngine e;
    e.prepare(sc.fs);
    e.setParams(steadyParams());
    std::vector<float> L = sc.L, R = sc.R;
    T::Rng rng(77);
    std::uint64_t allocs = 0;
    bool finite = true;
    std::size_t at = 0;
    int calls = 0;
    {
        T::AllocScope scope;
        while (at + 700 < L.size()) {
            Params p;
            p.sensitivity = static_cast<float>(rng.uniform());
            p.reductionDb = static_cast<float>(rng.uniform() * 30.0 - 3.0);   // includes out-of-range values on purpose
            p.focusHz = static_cast<float>(rng.uniform() * 24000.0);
            p.widthHz = static_cast<float>(rng.uniform() * 9000.0);
            p.mix = static_cast<float>(rng.uniform() * 1.4 - 0.2);
            p.steadySeconds = static_cast<float>(rng.uniform() * 3.0);
            p.smoothMs = static_cast<float>(rng.uniform() * 800.0);
            p.bypass = rng.uniform() < 0.15;
            p.listenRemoved = rng.uniform() < 0.15;
            p.mode = rng.uniform() < 0.5 ? Mode::Stereo : Mode::MidSide;
            p.method = static_cast<Method>(static_cast<int>(rng.uniform() * 3.0) % 3);
            e.setParams(p);
            const auto blk = static_cast<std::uint32_t>(1 + static_cast<std::size_t>(rng.uniform() * 699.0));
            if ((calls % 17) == 5) { L[at] = std::numeric_limits<float>::quiet_NaN(); R[at + 1] = std::numeric_limits<float>::infinity(); }
            e.process(L.data() + at, R.data() + at, blk);
            for (std::uint32_t i = 0; i < blk; ++i)
                if (!std::isfinite(L[at + i]) || !std::isfinite(R[at + i]) || std::fabs(L[at + i]) > 100.0f) finite = false;
            at += blk;
            ++calls;
        }
        allocs = scope.count();
    }
    std::printf("  %d process calls with random settings, changed every call\n", calls);
    HCHECK(allocs == 0, "no memory allocated on the audio thread (%llu allocations)", static_cast<unsigned long long>(allocs));
    HCHECK(finite, "no NaN, no infinity, nothing wild in the output, even with bad numbers fed in and settings out of range");
}

// ---------------------------------------------------------------------------------------------
void testAutoKnownAnswers() {
    std::printf("[AUTO FIX on known-answer files]\n");
    struct Case { const char* name; T::SceneSpec s; ArtifactKind want; };
    std::vector<Case> cases;
    { T::SceneSpec s; cases.push_back({"steady hiss", s, ArtifactKind::Steady}); }
    { T::SceneSpec s; s.hiss = T::Hiss::Tone; cases.push_back({"a single tone", s, ArtifactKind::Tonal}); }
    { T::SceneSpec s; s.hiss = T::Hiss::Flutter; cases.push_back({"flutter", s, ArtifactKind::Flutter}); }
    { T::SceneSpec s; s.fs = 44100.0; cases.push_back({"steady hiss at 44.1 kHz", s, ArtifactKind::Steady}); }
    { T::SceneSpec s; s.fs = 96000.0; s.seconds = 12.0; cases.push_back({"steady hiss at 96 kHz", s, ArtifactKind::Steady}); }
    { T::SceneSpec s; s.hissLo = 9250.0; s.hissHi = 10250.0; cases.push_back({"steady hiss, different place (9.75 kHz)", s, ArtifactKind::Steady}); }
    for (const auto& c : cases) {
        const auto sc = T::makeScene(c.s);
        const auto r = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs, {}, c.name);
        std::printf("  %s -> found %s near %.0f Hz, confidence %s, drop %.1f dB, worst hit %.2f dB, watery %.2f\n", c.name, kindName(r.diagnosis.kind), r.diagnosis.centreHz,
                    confidenceName(r.confidence), r.finalCheck.dropDb, r.finalCheck.peakLossDb, r.finalCheck.musicalNoise);
        const double expectHz = c.s.hiss == T::Hiss::Tone ? c.s.toneHz : 0.5 * (c.s.hissLo + c.s.hissHi);
        HCHECK(r.diagnosis.kind == c.want, "%s: it names the problem right (%s)", c.name, kindName(r.diagnosis.kind));
        HCHECK(std::fabs(r.diagnosis.centreHz - expectHz) < (c.want == ArtifactKind::Tonal ? 40.0 : 300.0), "%s: it finds the place (%.0f Hz, truth %.0f Hz)", c.name, r.diagnosis.centreHz, expectHz);
        HCHECK(r.touched, "%s: it fixes the file", c.name);
        HCHECK(r.finalCheck.dropDb > 8.0, "%s: the always-there level falls by %.1f dB", c.name, r.finalCheck.dropDb);
        HCHECK(r.finalCheck.peakLossDb > -0.5 && r.finalCheck.attackShiftMs < 0.5 && r.finalCheck.outOfBandDb < 0.05, "%s: hits and the rest of the sound are safe", c.name);
        HCHECK(r.confidence >= Confidence::Medium, "%s: confidence %s", c.name, confidenceName(r.confidence));
        // the fixed + removed = original, sample for sample
        double worst = 0.0;
        for (std::size_t i = 0; i < sc.L.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(r.left[i]) + r.removedLeft[i] - sc.L[i]));
        HCHECK(worst < 1e-6, "%s: fixed + removed gives back the original (worst gap %.1e)", c.name, worst);
        // how much of the TRUE artifact was taken out, compared with how much of the music was?
        double artIn = 0, artGone = 0, musicGone = 0;
        const std::size_t a = static_cast<std::size_t>(2.0 * sc.fs), b = sc.L.size();
        const auto lo = c.s.hiss == T::Hiss::Tone ? c.s.toneHz - 50.0 : c.s.hissLo, hi = c.s.hiss == T::Hiss::Tone ? c.s.toneHz + 50.0 : c.s.hissHi;
        (void)lo; (void)hi;
        for (std::size_t i = a; i < b; ++i) {
            artIn += static_cast<double>(sc.hissL[i]) * sc.hissL[i];
            const double gone = sc.L[i] - r.left[i];
            artGone += gone * gone;
            const double wrong = gone - sc.hissL[i];
            musicGone += wrong * wrong;
        }
        (void)artGone;
        HMEASURED((std::string(c.name) + ": gap between what was cut and the true artifact, all the time incl. during hits (0 dB = nothing really removed, -12 dB = nearly perfect)").c_str(), 10.0 * std::log10(musicGone / artIn + 1e-20), "dB");
    }
}

void testAutoDoNoHarm() {
    std::printf("[AUTO FIX: do no harm]\n");
    // clean file: untouched, bit for bit
    {
        T::SceneSpec s; s.hiss = T::Hiss::None;
        const auto sc = T::makeScene(s);
        const auto r = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs, {}, "clean");
        HCHECK(!r.touched && r.confidence == Confidence::Untouched, "clean file: left alone (%s)", r.diagnosis.notes.c_str());
        HCHECK(sameBits(r.left, sc.L) && sameBits(r.right, sc.R), "clean file: comes back bit for bit the same");
    }
    // hard case: dense hi-hats, no hiss at all. It may touch it or not, but hits must not be hurt.
    {
        T::SceneSpec s; s.denseHats = true; s.hiss = T::Hiss::None; s.seconds = 20.0;
        const auto sc = T::makeScene(s);
        const auto r = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs, {}, "dense hats");
        const auto m0 = mono(sc.L, sc.R), m1 = mono(r.left, r.right);
        const auto ons = M::findOnsets(m0.data(), m0.size(), sc.fs);
        const auto hs = M::compareHits(m0.data(), m1.data(), m0.size(), sc.fs, ons);
        std::printf("  dense hats, no hiss: touched=%d, worst hit change %.2f dB\n", r.touched ? 1 : 0, hs.worstPeakLossDb);
        HCHECK(hs.worstPeakLossDb > -0.5 && hs.maxAttackShiftMs < 0.5, "dense hats: hits unharmed (worst %.2f dB, attack shift %.2f ms)", hs.worstPeakLossDb, hs.maxAttackShiftMs);
        HCHECK(r.touched || (sameBits(r.left, sc.L) && sameBits(r.right, sc.R)), "dense hats: either fixed safely or returned untouched bit for bit");
    }
    // hard case: dense hats WITH hiss
    {
        T::SceneSpec s; s.denseHats = true; s.seconds = 20.0;
        const auto sc = T::makeScene(s);
        const auto r = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs, {}, "dense hats + hiss");
        const auto m0 = mono(sc.L, sc.R), m1 = mono(r.left, r.right);
        const auto ons = M::findOnsets(m0.data(), m0.size(), sc.fs);
        const auto hs = M::compareHits(m0.data(), m1.data(), m0.size(), sc.fs, ons);
        std::printf("  dense hats + hiss: touched=%d, found %s, drop %.1f dB, worst hit change %.2f dB\n", r.touched ? 1 : 0, kindName(r.diagnosis.kind), r.finalCheck.dropDb, hs.worstPeakLossDb);
        HCHECK(hs.worstPeakLossDb > -0.5, "dense hats + hiss: hits unharmed (worst %.2f dB)", hs.worstPeakLossDb);
        HCHECK(r.touched || (sameBits(r.left, sc.L) && sameBits(r.right, sc.R)), "dense hats + hiss: either fixed safely or returned untouched bit for bit");
    }
    // silence, tiny file
    {
        std::vector<float> z(48000 * 5, 0.0f);
        const auto r = autoFix(z.data(), z.data(), z.size(), 48000.0, {}, "silence");
        HCHECK(!r.touched && sameBits(r.left, z), "silence: untouched");
        std::vector<float> tiny(100, 0.25f);
        const auto r2 = autoFix(tiny.data(), tiny.data(), tiny.size(), 48000.0, {}, "tiny");
        HCHECK(!r2.touched && sameBits(r2.left, tiny), "a file of 100 samples: untouched");
    }
    // hiss only in the side channel: should be found as Mid/Side and mid left alone
    {
        T::SceneSpec s; s.hiss = T::Hiss::None; s.seconds = 20.0;
        auto sc = T::makeScene(s);
        const auto h = T::bandNoise(sc.L.size(), sc.fs, 11250, 12250, -64.0, 4242);
        for (std::size_t i = 0; i < sc.L.size(); ++i) { sc.L[i] += h[i]; sc.R[i] -= h[i]; }
        const auto r = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs, {}, "side-only hiss");
        std::printf("  side-only hiss: mode %s, mid bump %.1f dB, side bump %.1f dB, drop %.1f dB\n", modeName(r.diagnosis.mode), r.diagnosis.bumpMidDb, r.diagnosis.bumpSideDb, r.finalCheck.dropDb);
        HCHECK(r.diagnosis.mode == Mode::MidSide, "side-only hiss: it chooses Mid/Side mode");
        HCHECK(r.touched && r.finalCheck.dropDb > 8.0, "side-only hiss: removed (%.1f dB)", r.finalCheck.dropDb);
    }
    // repeatable
    {
        T::SceneSpec s; s.seconds = 12.0;
        const auto sc = T::makeScene(s);
        const auto a = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs), b = autoFix(sc.L.data(), sc.R.data(), sc.L.size(), sc.fs);
        HCHECK(sameBits(a.left, b.left) && a.report == b.report, "AUTO FIX gives the identical result and report on a second run");
    }
}

void testStems() {
    std::printf("[13 stems]\n");
    const double fs = 48000.0;
    const char* names[13] = {"kick", "snare", "hats", "oh", "bass", "gtr1", "gtr2", "keys", "pad", "vox1", "vox2", "fx", "bus"};
    std::vector<std::vector<float>> L(13), R(13);
    std::vector<bool> hasHiss(13, false);
    for (int i = 0; i < 13; ++i) {
        T::SceneSpec s; s.seconds = 14.0; s.seed = static_cast<std::uint64_t>(i + 1); s.hits = (i == 2 || i == 3 || i == 1);
        s.hiss = (i == 3 || i == 8 || i == 12) ? T::Hiss::Steady : T::Hiss::None;
        s.bed = i != 2 && i != 3;
        hasHiss[static_cast<std::size_t>(i)] = s.hiss == T::Hiss::Steady;
        const auto sc = T::makeScene(s);
        L[static_cast<std::size_t>(i)] = sc.L;
        R[static_cast<std::size_t>(i)] = sc.R;
    }
    L[11].assign(L[11].size(), 0.0f);   // one silent stem
    R[11].assign(R[11].size(), 0.0f);
    std::vector<StemRef> refs;
    for (int i = 0; i < 13; ++i) refs.push_back({names[i], &L[static_cast<std::size_t>(i)], &R[static_cast<std::size_t>(i)]});
    const auto res = autoFixStems(refs, fs);
    int wrongTouch = 0, missed = 0, notExact = 0;
    for (int i = 0; i < 13; ++i) {
        const auto& r = res[static_cast<std::size_t>(i)];
        if (hasHiss[static_cast<std::size_t>(i)] && !r.touched) ++missed;
        if (!hasHiss[static_cast<std::size_t>(i)] && r.touched) ++wrongTouch;
        if (!r.touched && !(sameBits(r.left, L[static_cast<std::size_t>(i)]) && sameBits(r.right, R[static_cast<std::size_t>(i)]))) ++notExact;
        std::printf("  %-6s hiss=%d -> %s (%s)\n", names[i], hasHiss[static_cast<std::size_t>(i)] ? 1 : 0, r.touched ? "fixed" : "untouched", confidenceName(r.confidence));
    }
    HCHECK(res.size() == 13, "all 13 stems come back");
    HCHECK(missed == 0, "every stem that has hiss was fixed (%d missed)", missed);
    HCHECK(wrongTouch == 0, "no clean stem was touched (%d touched)", wrongTouch);
    HCHECK(notExact == 0, "every untouched stem is bit for bit the same (%d differ)", notExact);
    const auto text = stemsReport(res);
    HCHECK(text.find("13 stem(s)") != std::string::npos, "the stem report is written");
}


// ---------------------------------------------------------------------------------------------
//  The 25 Hz comb remover (Phase 1B)
// ---------------------------------------------------------------------------------------------
struct CombScene { std::vector<float> L, R, cL, cR, bedL, bedR; };

// quiet floor + a 25 Hz line comb (lines 3-20 kHz, random phase and size) + loud bursts (fake music) + a stretch of digital silence
CombScene makeCombScene(double fs, double seconds, double combRmsDb, bool withComb, bool withSilence, std::uint64_t seed) {
    const auto n = static_cast<std::size_t>(seconds * fs);
    CombScene s;
    auto mask = [](double f) { return f < 20000.0 ? 1.0 : 0.0; };
    s.bedL = T::shapedNoise(n, fs, mask, -75.0, seed);
    s.bedR = T::shapedNoise(n, fs, mask, -75.0, seed + 1);
    // loud bursts every 2 s (broadband, -20 dBFS, 0.4 s): music-like, non-repeating
    const auto burst = T::shapedNoise(static_cast<std::size_t>(0.4 * fs), fs, [](double) { return 1.0; }, -20.0, seed + 2);
    for (double t = 1.0; t + 0.4 < seconds; t += 2.0) {
        const auto at = static_cast<std::size_t>(t * fs);
        for (std::size_t i = 0; i < burst.size(); ++i) { s.bedL[at + i] += burst[i]; s.bedR[at + i] += 0.8f * burst[i]; }
    }
    if (withSilence) {
        const auto a = static_cast<std::size_t>(seconds * 0.8 * fs), b = a + static_cast<std::size_t>(1.5 * fs);
        for (std::size_t i = a; i < b && i < n; ++i) s.bedL[i] = s.bedR[i] = 0.0f;
    }
    s.cL.assign(n, 0.0f);
    s.cR.assign(n, 0.0f);
    if (withComb) {
        T::Rng rng(seed + 9);
        const int jHi = static_cast<int>(std::min(20000.0, 0.5 * fs - 500.0) / 25.0);
        const int lines = jHi - 120 + 1;
        const double a = T::dbToLin(combRmsDb) * std::sqrt(2.0 / lines);
        for (int j = 120; j <= jHi; ++j) {
            const double amp = a * std::pow(10.0, (-4.0 + 8.0 * rng.uniform()) / 20.0), p1 = 6.2831853 * rng.uniform(), p2 = 6.2831853 * rng.uniform();
            for (std::size_t i = 0; i < n; ++i) {
                const double th = 2.0 * 3.14159265358979323846 * j * 25.0 * static_cast<double>(i) / fs;
                s.cL[i] += static_cast<float>(amp * std::sin(th + p1));
                s.cR[i] += static_cast<float>(amp * std::sin(th + p2));
            }
        }
    }
    s.L = s.bedL;
    s.R = s.bedR;
    for (std::size_t i = 0; i < n; ++i) { s.L[i] += s.cL[i]; s.R[i] += s.cR[i]; }
    if (withSilence) {
        const auto a = static_cast<std::size_t>(seconds * 0.8 * fs), b = a + static_cast<std::size_t>(1.5 * fs);
        for (std::size_t i = a; i < b && i < n; ++i) s.L[i] = s.R[i] = 0.0f;
    }
    return s;
}

double combRemovedDb(const CombScene& sc, const std::vector<float>& outL, std::size_t skip) {
    double e0 = 0.0, e1 = 0.0;
    for (std::size_t i = skip; i < sc.L.size(); ++i) {
        const double d0 = static_cast<double>(sc.L[i]) - sc.bedL[i], d1 = static_cast<double>(outL[i]) - sc.bedL[i];
        e0 += d0 * d0;
        e1 += d1 * d1;
    }
    return 10.0 * std::log10(e0 / std::max(e1, 1e-30));
}

std::vector<float> runCombEngine(HissCombEngine& e, const CombScene& sc, std::vector<float>* outR = nullptr) {
    auto oL = sc.L;
    auto oR = sc.R;
    e.prime(sc.L.data(), sc.R.data(), static_cast<std::uint32_t>(sc.L.size()));
    for (std::size_t i = 0; i < oL.size(); i += 512)
        e.process(oL.data() + i, oR.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(512, oL.size() - i)));
    if (outR) *outR = oR;
    return oL;
}

void testComb() {
    std::printf("\n== 25 Hz comb remover ==\n");
    const double fs = 48000.0;
    const auto sc = makeCombScene(fs, 30.0, -68.0, true, true, 5);
    {
        HissCombEngine e;
        e.prepare(fs);
        HCHECK(e.supported() && e.periodSamples() == 1920 && e.latency() == 0, "48 kHz: whole-sample period of 1920 samples, no latency");
        CombParams p;
        p.reductionDb = 18.0f;
        e.setParams(p);
        const auto out = runCombEngine(e, sc);
        const double rem = combRemovedDb(sc, out, static_cast<std::size_t>(2 * fs));
        HMEASURED("comb removed (known answer, 18 dB cut)", rem, "dB");
        HCHECK(rem >= 10.0, "the added comb is mostly gone (>= 10 dB)");
        HCHECK(e.linesKept() > 400, "the engine found most of the %d lines (kept %d)", 681, e.linesKept());
        // below the band nothing changes: the difference has no content under 2 kHz
        double lowIn = 0.0, lowDiff = 0.0;
        const auto lpIn = M::bandFloor(sc.L.data(), sc.L.size(), fs, 100.0, 2000.0, 0.5);
        std::vector<float> diff(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) diff[i] = sc.L[i] - out[i];
        const auto lpDiff = M::bandFloor(diff.data(), diff.size(), fs, 100.0, 2000.0, 0.5);
        lowIn = lpIn.meanDb;
        lowDiff = lpDiff.meanDb;
        HMEASURED("level below 2 kHz: input", lowIn, "dB");
        HMEASURED("level below 2 kHz: what was taken out", lowDiff, "dB");
        HCHECK(lowDiff < lowIn - 60.0, "what was taken out has nothing below 2 kHz (60 dB under the music there)");
        // digital silence stays exactly silent after the Auto brain's silence restore, and the engine itself leaves it near silent
        const auto a = static_cast<std::size_t>(30.0 * 0.8 * fs) + 4800, b = a + static_cast<std::size_t>(1.0 * fs);
        double peakInSilence = 0.0;
        for (std::size_t i = a; i < b; ++i) peakInSilence = std::max(peakInSilence, static_cast<double>(std::abs(out[i])));
        HMEASURED("peak inside the digital silence (engine alone)", T::linToDb(peakInSilence), "dBFS");
        HCHECK(peakInSilence < 1e-5, "engine alone: nothing audible is written into silence (< -100 dBFS)");
    }
    {   // pass-through settings are bit exact
        const auto s2 = makeCombScene(fs, 8.0, -68.0, true, false, 6);
        for (int mode = 0; mode < 3; ++mode) {
            HissCombEngine e;
            e.prepare(fs);
            CombParams p;
            if (mode == 0) p.bypass = true;
            if (mode == 1) p.mix = 0.0f;
            if (mode == 2) p.reductionDb = 0.0f;
            e.setParams(p);
            std::vector<float> oR;
            const auto oL = runCombEngine(e, s2, &oR);
            HCHECK(sameBits(oL, s2.L) && sameBits(oR, s2.R), "%s: output is bit-exact with the input", mode == 0 ? "bypass" : (mode == 1 ? "mix 0" : "cut 0 dB"));
        }
    }
    {   // no allocation on the audio thread, live learning included
        const auto s3 = makeCombScene(fs, 6.0, -68.0, true, false, 7);
        HissCombEngine e;
        e.prepare(fs);
        auto oL = s3.L;
        auto oR = s3.R;
        T::AllocScope scope;
        for (std::size_t i = 0; i < oL.size(); i += 480)
            e.process(oL.data() + i, oR.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(480, oL.size() - i)));
        HCHECK(scope.count() == 0, "no memory allocation while processing (6 s live, with refreshes): %llu", static_cast<unsigned long long>(scope.count()));
        HCHECK(e.linesKept() > 0, "live mode learned something from 6 s");
    }
    {   // stress: NaN, Inf, huge values, random parameters, odd block sizes
        HissCombEngine e;
        e.prepare(fs);
        T::Rng rng(77);
        std::vector<float> l(4096), r(4096);
        bool finite = true;
        for (int round = 0; round < 200; ++round) {
            for (auto& v : l) v = static_cast<float>(rng.uniform() * 2.0 - 1.0) * (round % 17 == 0 ? 1e6f : 0.1f);
            for (auto& v : r) v = static_cast<float>(rng.uniform() * 2.0 - 1.0) * 0.1f;
            if (round % 11 == 0) { l[5] = std::numeric_limits<float>::quiet_NaN(); r[9] = std::numeric_limits<float>::infinity(); }
            CombParams p;
            p.reductionDb = static_cast<float>(rng.uniform() * 40.0 - 5.0);
            p.loHz = static_cast<float>(rng.uniform() * 30000.0);
            p.hiHz = static_cast<float>(rng.uniform() * 50000.0);
            p.mix = static_cast<float>(rng.uniform() * 1.4 - 0.2);
            p.adaptSeconds = static_cast<float>(rng.uniform() * 40.0 - 5.0);
            p.sensitivity = static_cast<float>(rng.uniform() * 2.0 - 0.5);
            p.bypass = (round % 7 == 0);
            p.listenRemoved = (round % 5 == 0);
            e.setParams(p);
            const std::uint32_t blk = 1 + static_cast<std::uint32_t>(rng.uniform() * 4095.0);
            e.process(l.data(), r.data(), blk);
            for (std::uint32_t i = 0; i < blk; ++i) if (!std::isfinite(l[i]) || !std::isfinite(r[i])) finite = false;
        }
        HCHECK(finite, "200 rounds of random settings, NaN and Inf input, odd block sizes: every output sample is finite");
    }
    for (double rate : {44100.0, 96000.0}) {
        const auto s4 = makeCombScene(rate, 24.0, -68.0, true, false, 8);
        HissCombEngine e;
        e.prepare(rate);
        CombParams p;
        p.reductionDb = 18.0f;
        e.setParams(p);
        const auto out = runCombEngine(e, s4);
        const double rem = combRemovedDb(s4, out, static_cast<std::size_t>(2 * rate));
        HMEASURED(rate < 50000.0 ? "comb removed at 44.1 kHz" : "comb removed at 96 kHz", rem, "dB");
        HCHECK(e.supported() && e.periodSamples() == static_cast<int>(rate / 25.0) && rem >= 8.0, "%.0f Hz: period %d samples, comb mostly gone", rate, e.periodSamples());
    }
    {   // the Auto brain
        const auto good = makeCombScene(fs, 30.0, -74.0, true, true, 5);
        const auto r = autoFixComb(good.L.data(), good.R.data(), good.L.size(), fs, {}, "synthetic comb");
        std::printf("%s", r.report.c_str());
        HCHECK(r.before.found && r.touched, "Auto finds the comb and fixes it");
        HCHECK(r.touched && r.check.onGridAfter < r.check.onGridBefore * 0.5, "Auto: on-grid lines %d -> %d", r.check.onGridBefore, r.check.onGridAfter);
        const auto a = static_cast<std::size_t>(30.0 * 0.8 * fs) + 100, b = a + static_cast<std::size_t>(1.0 * fs);
        bool exact = true;
        for (std::size_t i = a; i < b; ++i) if (r.left[i] != 0.0f || r.right[i] != 0.0f) exact = false;
        HCHECK(exact, "Auto: digital silence stays exactly zero");

        const auto clean = makeCombScene(fs, 30.0, -68.0, false, true, 5);
        const auto rc = autoFixComb(clean.L.data(), clean.R.data(), clean.L.size(), fs, {}, "no comb");
        HCHECK(!rc.before.found && !rc.touched && sameBits(rc.left, clean.L) && sameBits(rc.right, clean.R), "Auto: a file without a comb comes back bit for bit (lines %d, on grid %d)", rc.before.lines, rc.before.onGrid);

        // a single steady tone is not a comb
        auto tone = clean;
        const auto t = T::sine(tone.L.size(), fs, 11775.0, -70.0);
        for (std::size_t i = 0; i < tone.L.size(); ++i) { tone.L[i] += t[i]; tone.R[i] += t[i]; }
        const auto rt = autoFixComb(tone.L.data(), tone.R.data(), tone.L.size(), fs, {}, "one tone");
        HCHECK(!rt.touched && sameBits(rt.left, tone.L), "Auto: one steady tone is not taken for the comb");
    }
}


// ---------------------------------------------------------------------------------------------
// Meters (Phase 1C). Known answers: every signal is built from parts whose level we chose.
std::vector<float> bandNoise(int log2n, double fs, double lo, double hi, unsigned seed, double rms = 1.0) {
    const int N = 1 << log2n;
    FftD fft; fft.prepare(N);
    std::mt19937 g(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<std::complex<double>> X(static_cast<std::size_t>(N));
    for (int k = 1; k < N / 2; ++k) {
        const double f = k * fs / N;
        if (f < lo || f >= hi) continue;
        const std::complex<double> v(nd(g), nd(g));
        X[static_cast<std::size_t>(k)] = v;
        X[static_cast<std::size_t>(N - k)] = std::conj(v);
    }
    fft.inverse(X.data());
    double ms = 0;
    for (auto& v : X) ms += v.real() * v.real();
    const double sc = rms / std::sqrt(ms / N);
    std::vector<float> out(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) out[static_cast<std::size_t>(i)] = static_cast<float>(X[static_cast<std::size_t>(i)].real() * sc);
    return out;
}

void testMeters() {
    std::printf("[meters]\n");
    namespace Mt = hc::hiss::meters;
    const double fs = 48000.0;
    {   // levels and loudness
        std::vector<float> s(480000);
        for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(i) / fs));
        const double l = Mt::integratedLufs(s.data(), s.size(), fs), r = Mt::rmsDbfs(s.data(), s.size()), b = Mt::bandRmsDbfs(s.data(), s.size(), fs, 500.0, 2000.0);
        HMEASURED("loudness of a 997 Hz sine, peak -20 dBFS", l, "LUFS");
        HCHECK(std::fabs(r - (-23.0103)) < 0.01, "rms of a -20 dBFS-peak sine is -23.01 dBFS (read %.3f)", r);
        HCHECK(std::fabs(b - (-23.0103)) < 0.1, "band rms 0.5-2 kHz of that sine is -23.01 dBFS (read %.3f)", b);
        HCHECK(std::fabs(l - (-23.0103)) < 0.1, "loudness of that sine is -23.01 LUFS (read %.3f; K-weighting is about +0.69 dB at 997 Hz and the BS.1770 offset is -0.691)", l);
        std::vector<float> q(48000);
        for (std::size_t i = 0; i < q.size(); ++i) q[i] = static_cast<float>(std::sin(3.14159265358979323846 * static_cast<double>(i) / 2.0 + 3.14159265358979323846 / 4.0));
        const double tp = Mt::truePeakDbfs(q.data(), q.size());
        HCHECK(tp > -0.3 && tp < 0.3, "true peak of a fs/4 sine whose samples all sit at 0.707 reads about 0 dBFS (read %.3f, sample peak is -3.01)", tp);
    }
    {   // ratio can rise because the bottom band fell
        const int L = 20;   // 2^20 samples = 21.8 s
        const auto nA = bandNoise(L, fs, 5000, 10000, 1), nB = bandNoise(L, fs, 16000, 20000, 2), nL = bandNoise(L, fs, 200, 4000, 3);
        auto mix = [&](double gAdb, double gBdb) {
            std::vector<float> x(nA.size());
            const double gA = 0.05 * std::pow(10.0, gAdb / 20.0), gB = 0.005 * std::pow(10.0, gBdb / 20.0);
            for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(gA * nA[i] + gB * nB[i] + 0.05 * nL[i]);
            return x;
        };
        const auto base = mix(0, 0), cut = mix(-10, 0), air = mix(0, 6);
        const auto m0 = Mt::ratioMetrics(base.data(), base.size(), fs), m1 = Mt::ratioMetrics(cut.data(), cut.size(), fs), m2 = Mt::ratioMetrics(air.data(), air.size(), fs);
        HCHECK(m0.valid && m1.valid && m2.valid, "enough loud frames to measure (%d, %d, %d)", m0.loud, m1.loud, m2.loud);
        HMEASURED("5-10 kHz cut 10 dB: ratio rise", m1.ratio16_20Over5_10Db - m0.ratio16_20Over5_10Db, "dB");
        HCHECK(std::fabs((m1.ratio16_20Over5_10Db - m0.ratio16_20Over5_10Db) - 10.0) < 0.5, "cutting 5-10 kHz by 10 dB raises the ratio by 10 dB (+/-0.5)");
        HCHECK(std::fabs(m1.b16to20Db - m0.b16to20Db) < 0.3, "...while the absolute 16-20 kHz level moves less than 0.3 dB (%.3f)", m1.b16to20Db - m0.b16to20Db);
        HCHECK(std::fabs((m2.b16to20Db - m0.b16to20Db) - 6.0) < 0.5, "adding 6 dB of real air raises the absolute 16-20 kHz level by 6 dB (+/-0.5) (%.3f)", m2.b16to20Db - m0.b16to20Db);
    }
    {   // bandwidth audit
        const auto wall = bandNoise(20, fs, 20, 16000, 4, 0.1), full = bandNoise(20, fs, 20, 23990, 5, 0.1);
        const auto cw = Mt::cutoffAudit(wall.data(), wall.size(), fs), cf = Mt::cutoffAudit(full.data(), full.size(), fs);
        HMEASURED("brick wall at 16000 Hz found at", cw.cutoffHz, "Hz");
        HCHECK(cw.shape == Mt::EndShape::Wall && std::fabs(cw.cutoffHz - 16000.0) < 300.0, "a brick wall at 16 kHz is found as a WALL within 300 Hz");
        HCHECK(!cw.reaches19k(), "a file ending at 16 kHz is disqualified from the 8-20 kHz measurement");
        HCHECK(cf.shape == Mt::EndShape::None || cf.cutoffHz > 23000.0, "a full-band file is not called a wall below 23 kHz");
    }
    {   // the 25 Hz comb
        const std::size_t n = 960000;
        auto x = bandNoise(20, fs, 20, 20000, 6, std::pow(10.0, -60.0 / 20.0));
        x.resize(n);
        const auto none = Mt::combLines(x.data(), n, fs);
        std::mt19937 g(9); std::uniform_real_distribution<double> ph(0.0, 6.283185307179586);
        const double amp = std::sqrt(2.0) * std::pow(10.0, -100.0 / 20.0);
        std::vector<double> phase; for (int k = 120; k <= 800; ++k) phase.push_back(ph(g));
        for (std::size_t i = 0; i < n; ++i) {
            double add = 0;
            for (int k = 120; k <= 800; k += 1) add += std::sin(2.0 * 3.14159265358979323846 * k * 25.0 * static_cast<double>(i % 1920) / fs + phase[static_cast<std::size_t>(k - 120)]);
            x[i] += static_cast<float>(amp * add);
        }
        const auto with = Mt::combLines(x.data(), n, fs);
        HMEASURED("comb injected at -100 dBFS per line, read", with.medianLineDb, "dBFS");
        HMEASURED("measurement bandwidth", with.bandwidthHz, "Hz");
        HCHECK(with.valid && std::fabs(with.medianLineDb - (-100.0)) < 1.0, "an injected comb of -100 dBFS per line is read within 1 dB (%.2f)", with.medianLineDb);
        HCHECK(with.onGreaterThanOffPct > 95.0, "with a comb, nearly every line beats its neighbours (%.1f %%)", with.onGreaterThanOffPct);
        HCHECK(none.onGreaterThanOffPct < 65.0 && none.onGreaterThanOffPct > 30.0, "with no comb, about half do (%.1f %%)", none.onGreaterThanOffPct);
        HCHECK(!Mt::combLines(x.data(), n, 44100.5).valid, "a sample rate that is not a multiple of 25 Hz is refused");
    }
    {   // flutter (quantified, self-calibrating)
        const auto b = bandNoise(20, fs, 5000, 15000, 7, 0.05);
        const std::size_t n = 960000;
        std::vector<float> plain(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)), am = plain;
        for (std::size_t i = 0; i < n; ++i) am[i] = static_cast<float>(plain[i] * (1.0 + 0.30 * std::sin(2.0 * 3.14159265358979323846 * 25.0 * static_cast<double>(i) / fs + 1.0)));
        const auto f0 = Mt::flutterDepth(plain.data(), n, fs), f1 = Mt::flutterDepth(am.data(), n, fs);
        HMEASURED("flutter reading, nothing injected", f0.depth, "");
        HMEASURED("flutter reading, 0.30 injected", f1.depth, "");
        HCHECK(f0.valid && f1.valid, "both clips are long enough and have a band to measure");
        // The registered rule (SPLIT.json check 4) is +/-20 %%. I first wrote +/-10 %% as my own stricter guess; steady noise reads 0.341 (14 %% high), so that guess failed and is not used.
        HCHECK(f1.depth > 0.24 && f1.depth < 0.36, "a true 0.30 reads 0.24 to 0.36 (the registered +/-20 %%) (%.3f)", f1.depth);
        HCHECK(f0.depth < 0.03, "no flutter reads under 0.03 (%.4f)", f0.depth);
        // the exact slip that failed the first real test: 19.5 s at 44.1 kHz puts 25 Hz half way between two FFT bins
        const double fs2 = 44100.0; const std::size_t n2 = static_cast<std::size_t>(19.5 * fs2);
        const auto b2 = bandNoise(20, fs2, 5000, 15000, 8, 0.05);
        std::vector<float> am2(b2.begin(), b2.begin() + static_cast<std::ptrdiff_t>(n2));
        for (std::size_t i = 0; i < n2; ++i) am2[i] = static_cast<float>(am2[i] * (1.0 + 0.30 * std::sin(2.0 * 3.14159265358979323846 * 25.0 * static_cast<double>(i) / fs2 + 2.0)));
        const auto f2 = Mt::flutterDepth(am2.data(), n2, fs2);
        HMEASURED("flutter reading, 0.30 injected, 19.5 s at 44.1 kHz (half-bin case)", f2.depth, "");
        HCHECK(f2.valid && f2.depth > 0.24 && f2.depth < 0.36, "the half-bin case also reads 0.24 to 0.36 (%.3f)", f2.depth);
        HCHECK(!Mt::flutterDepth(plain.data(), 480000, fs).valid, "a 10 s clip is refused");
        std::vector<float> silent(n, 0.0f);
        HCHECK(!Mt::flutterDepth(silent.data(), n, fs).valid, "a clip with nothing in 5-15 kHz is refused");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::printf("HEM HISS TAMER + HISS AUTO — tests\n\n");
    if (argc > 1 && std::string(argv[1]) == "meters") {
        testMeters();
        std::printf("\n%d checks, %d failed\n", T::gChecks, T::gFails);
        return T::gFails == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "comb") {     // quick run of the comb tests only
        testComb();
        std::printf("\n%d checks, %d failed\n", T::gChecks, T::gFails);
        return T::gFails == 0 ? 0 : 1;
    }
    testBuildingBlocks();
    testPassThrough();
    testEngineBehaviour();
    testRatesAndSpeed();
    testNoAllocationAndStress();
    testAutoKnownAnswers();
    testAutoDoNoHarm();
    testStems();
    testComb();
    testMeters();
    std::printf("\n%d checks, %d failed\n", T::gChecks, T::gFails);
    return T::gFails == 0 ? 0 : 1;
}
