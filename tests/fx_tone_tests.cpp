// HEM EQ-6, HEM SUBSTACK, HEM CALM, HEM BLOOM: each does what its knobs say.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"

#include <cstdio>

namespace hc::test {

using namespace fxt;

namespace {
// Level (dB re full-scale sine amplitude 1) of the component at hz in x[from..), Goertzel.
double toneDb(const std::vector<float>& x, double hz, std::size_t from = 0, double fs = 48000.0) {
    const std::size_t n = x.size() - from;
    const double w = 2.0 * std::numbers::pi * hz / fs, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (std::size_t i = from; i < x.size(); ++i) {
        // Hann window for low leakage
        const double win = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i - from) / static_cast<double>(n));
        const double s = x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    const double amp = 2.0 * std::sqrt(re * re + im * im) / (0.5 * static_cast<double>(n));
    return 20.0 * std::log10(std::max(amp, 1e-12));
}
void setP(fx::Effect& e, const char* id, float v) {
    const int i = param(e, id);
    CHECK(i >= 0);
    e.set(i, v);
}
} // namespace

void testFxEq6() {
    std::printf("[fx] HEM EQ-6: bells and shelves hit their gain, cuts cut, flat when untouched\n");
    auto e = fx::makeEq6();
    e->prepare(48000.0, 512);
    const double flat1k = gainAtDb(*e, 1000.0), flat50 = gainAtDb(*e, 50.0), flat15k = gainAtDb(*e, 15000.0);
    std::printf("  flat: 50 Hz %+.2f  1 kHz %+.2f  15 kHz %+.2f dB\n", flat50, flat1k, flat15k);
    CHECK(std::fabs(flat1k) < 0.1 && std::fabs(flat50) < 0.1 && std::fabs(flat15k) < 0.1);

    setP(*e, "b1freq", 1000.0f); setP(*e, "b1gain", 6.0f); setP(*e, "b1q", 1.0f);
    const double b1 = gainAtDb(*e, 1000.0), b1far = gainAtDb(*e, 60.0);
    setP(*e, "b1gain", 0.0f);
    setP(*e, "b2freq", 5000.0f); setP(*e, "b2gain", -9.0f); setP(*e, "b2q", 2.0f);
    const double b2 = gainAtDb(*e, 5000.0), b2far = gainAtDb(*e, 500.0);
    setP(*e, "b2gain", 0.0f);
    setP(*e, "lsfreq", 200.0f); setP(*e, "lsgain", 6.0f);
    const double ls = gainAtDb(*e, 30.0), lsfar = gainAtDb(*e, 5000.0);
    setP(*e, "lsgain", 0.0f);
    setP(*e, "hsfreq", 4000.0f); setP(*e, "hsgain", -6.0f);
    const double hs = gainAtDb(*e, 18000.0), hsfar = gainAtDb(*e, 200.0);
    setP(*e, "hsgain", 0.0f);
    std::printf("  bell1 +6 @1k: %+.2f (60 Hz %+.2f)  bell2 -9 @5k: %+.2f (500 Hz %+.2f)\n", b1, b1far, b2, b2far);
    std::printf("  low shelf +6 @200: 30 Hz %+.2f (5k %+.2f)  high shelf -6 @4k: 18k %+.2f (200 Hz %+.2f)\n", ls, lsfar, hs, hsfar);
    CHECK(std::fabs(b1 - 6.0) < 0.5 && std::fabs(b1far) < 0.5);
    CHECK(std::fabs(b2 + 9.0) < 0.5 && std::fabs(b2far) < 0.5);
    CHECK(std::fabs(ls - 6.0) < 0.5 && std::fabs(lsfar) < 0.5);
    CHECK(std::fabs(hs + 6.0) < 0.5 && std::fabs(hsfar) < 0.5);

    setP(*e, "lowcut", 200.0f); setP(*e, "lowslope", 1.0f);
    const double lc24 = gainAtDb(*e, 100.0), lcPass = gainAtDb(*e, 2000.0);
    setP(*e, "lowslope", 0.0f);
    const double lc12 = gainAtDb(*e, 100.0);
    setP(*e, "lowcut", 20.0f);
    setP(*e, "highcut", 2000.0f); setP(*e, "highslope", 1.0f);
    const double hc24 = gainAtDb(*e, 4000.0), hcPass = gainAtDb(*e, 200.0);
    setP(*e, "highslope", 0.0f);
    const double hc12 = gainAtDb(*e, 4000.0);
    setP(*e, "highcut", 20000.0f);
    std::printf("  low cut 200: 100 Hz %.1f (24) %.1f (12), 2 kHz %+.2f  high cut 2k: 4 kHz %.1f (24) %.1f (12), 200 Hz %+.2f dB\n",
                lc24, lc12, lcPass, hc24, hc12, hcPass);
    CHECK(lc24 < -20.0 && lc12 < -10.0 && std::fabs(lcPass) < 0.3);
    CHECK(hc24 < -20.0 && hc12 < -10.0 && std::fabs(hcPass) < 0.3);
    setP(*e, "output", -6.0f);
    const double out = gainAtDb(*e, 1000.0);
    CHECK(std::fabs(out + 6.0) < 0.1);
    // Back at the end stops: transparent again.
    setP(*e, "output", 0.0f);
    CHECK(std::fabs(gainAtDb(*e, 30.0)) < 0.1 && std::fabs(gainAtDb(*e, 19000.0)) < 0.1);
}

void testFxSubStack() {
    std::printf("[fx] HEM SUBSTACK: transparent at rest, Sub moves the low end, Harmonics add 2nd/3rd\n");
    auto e = fx::makeSubStack();
    e->prepare(48000.0, 512);
    {   // Untouched: output = input exactly.
        Stereo s = noise(0.5, 0.5f), ref = s;
        run(*e, s);
        double err = 0;
        for (std::size_t i = 0; i < s.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(s.l[i] - ref.l[i])));
        CHECK(err < 1e-5);
    }
    setP(*e, "freq", 150.0f);
    setP(*e, "sub", 6.0f);
    const double sub40 = gainAtDb(*e, 40.0), sub2k = gainAtDb(*e, 2000.0);
    setP(*e, "sub", -6.0f);
    const double cut40 = gainAtDb(*e, 40.0);
    setP(*e, "sub", 0.0f);
    std::printf("  Sub +6: 40 Hz %+.2f dB (2 kHz %+.2f)  Sub -6: 40 Hz %+.2f dB\n", sub40, sub2k, cut40);
    CHECK(std::fabs(sub40 - 6.0) < 1.0 && std::fabs(sub2k) < 0.2 && std::fabs(cut40 + 6.0) < 1.0);

    setP(*e, "freq", 90.0f);
    auto harmonicsAt = [&](float pct, double& h2, double& h3, double& f1) {
        setP(*e, "harmonics", pct);
        e->reset();
        Stereo s = sine(50.0, 2.0, 0.5f);
        run(*e, s);
        h2 = toneDb(s.l, 100.0, 48000); h3 = toneDb(s.l, 150.0, 48000); f1 = toneDb(s.l, 50.0, 48000);
    };
    double h2a, h3a, f1a, h2b, h3b, f1b;
    harmonicsAt(0.0f, h2a, h3a, f1a);
    harmonicsAt(100.0f, h2b, h3b, f1b);
    std::printf("  50 Hz sine: 100 Hz %.1f -> %.1f dB, 150 Hz %.1f -> %.1f dB, 50 Hz %.1f -> %.1f dB\n", h2a, h2b, h3a, h3b, f1a, f1b);
    CHECK(h2b > h2a + 30.0 && h2b > -30.0);
    CHECK(h3b > h3a + 30.0 && h3b > -30.0);
    CHECK(std::fabs(f1b - f1a) < 1.5);
    setP(*e, "harmonics", 0.0f);

    // Mono Below: a low tone only on the left comes out on both sides.
    setP(*e, "monobelow", 1.0f);
    e->reset();
    Stereo s = sine(40.0, 1.0, 0.5f);
    std::fill(s.r.begin(), s.r.end(), 0.0f);
    run(*e, s);
    const double l = rmsDb(s.l, 24000), r = rmsDb(s.r, 24000);
    std::printf("  Mono Below, 40 Hz left only: L %.1f  R %.1f dB\n", l, r);
    CHECK(std::fabs(l - r) < 1.5);
}

void testFxCalm() {
    std::printf("[fx] HEM CALM: the ringing tone goes down, the rest hardly; Depth 0 transparent; Delta = what's removed\n");
    auto make = [](float depth, bool delta) {
        auto e = fx::makeCalm();
        e->prepare(48000.0, 512);
        setP(*e, "depth", depth);
        setP(*e, "delta", delta ? 1.0f : 0.0f);
        e->reset();   // start at these settings (no 20 ms glide from the defaults)
        return e;
    };
    auto input = [] {
        Stereo s = noise(3.0, 0.05f, 11);
        for (std::size_t i = 0; i < s.size(); ++i) {
            const float v = 0.3f * static_cast<float>(std::sin(2 * std::numbers::pi * 3000.0 * static_cast<double>(i) / 48000.0));
            s.l[i] += v; s.r[i] += v;
        }
        return s;
    };
    const Stereo in = input();
    auto e = make(12.0f, false);
    const std::uint32_t lat = e->latency();
    Stereo out = in;
    run(*e, out);
    const std::size_t from = 48000;   // settled, and past the latency
    std::vector<float> inAligned(in.l.begin() + static_cast<std::ptrdiff_t>(from - lat), in.l.end() - static_cast<std::ptrdiff_t>(lat));
    std::vector<float> outPart(out.l.begin() + static_cast<std::ptrdiff_t>(from), out.l.end());
    const double toneCut = toneDb(outPart, 3000.0) - toneDb(inAligned, 3000.0);
    double noiseCut = 0;
    const double probes[] = {700.0, 1300.0, 6100.0, 9100.0};
    for (double hz : probes) noiseCut += (toneDb(outPart, hz) - toneDb(inAligned, hz)) / 4.0;
    std::printf("  3 kHz ring %+.1f dB, noise around it %+.1f dB, reduction meter %.1f dB, latency %u\n", toneCut, noiseCut, e->reductionDb(), lat);
    CHECK(toneCut < -6.0);
    CHECK(noiseCut > -2.0);
    CHECK(toneCut < noiseCut - 4.0);

    // Depth 0: the input, delayed by latency(), nothing else.
    auto z = make(0.0f, false);
    Stereo t = in;
    run(*z, t);
    double err = 0;
    for (std::size_t i = lat; i < t.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(t.l[i] - in.l[i - lat])));
    std::printf("  Depth 0: max difference to the delayed input %.2e\n", err);
    CHECK(err < 1e-4);

    // Delta: processed + removed = the delayed input; the removed part is mostly the ring.
    auto d = make(12.0f, true);
    Stereo rm = in;
    run(*d, rm);
    double sumErr = 0;
    for (std::size_t i = lat; i < rm.size(); ++i) sumErr = std::max(sumErr, static_cast<double>(std::fabs(rm.l[i] + out.l[i] - in.l[i - lat])));
    std::vector<float> rmPart(rm.l.begin() + static_cast<std::ptrdiff_t>(from), rm.l.end());
    const double rmTone = toneDb(rmPart, 3000.0), rmNoise = toneDb(rmPart, 700.0);
    std::printf("  Delta: removed 3 kHz %.1f dB vs 700 Hz %.1f dB, processed+removed vs input %.2e\n", rmTone, rmNoise, sumErr);
    CHECK(sumErr < 1e-4);
    CHECK(rmTone > rmNoise + 20.0);
}

void testFxBloom() {
    std::printf("[fx] HEM BLOOM: a click leaves a tail; Duck pulls the wet down under a loud dry\n");
    auto e = fx::makeBloom();
    e->prepare(48000.0, 512);
    setP(*e, "mix", 100.0f);
    setP(*e, "duck", 0.0f);
    setP(*e, "decay", 2.0f);
    e->reset();
    Stereo c(48000 * 3);
    c.l[0] = c.r[0] = 1.0f;
    run(*e, c);
    const double early = rmsDb(c.l, 4800, 24000), late = rmsDb(c.l, 96000, 120000);
    std::printf("  click: tail 0.1-0.5 s %.1f dB (R %.1f), 2.0-2.5 s %.1f dB\n", early, rmsDb(c.r, 4800, 24000), late);
    CHECK(early > -70.0);
    CHECK(late < early - 20.0);   // RT60 2 s: about −40 dB by 2 s

    auto wetUnderDry = [&](float duck) {
        setP(*e, "duck", duck);
        e->reset();
        Stereo s = noise(1.5, 0.5f, 5);
        run(*e, s);
        return rmsDb(s.l, 36000);
    };
    const double w0 = wetUnderDry(0.0f), w100 = wetUnderDry(100.0f);
    std::printf("  wet under a loud dry: Duck 0 %% %.1f dB, Duck 100 %% %.1f dB, meter %.1f dB\n", w0, w100, e->reductionDb());
    CHECK(w100 < w0 - 6.0);
    // When the dry stops, the ducked tail comes back up.
    setP(*e, "duck", 100.0f);
    e->reset();
    Stereo s = noise(1.0, 0.5f, 5);
    s.l.resize(48000 * 2, 0.0f); s.r.resize(48000 * 2, 0.0f);
    run(*e, s);
    const double under = rmsDb(s.l, 36000, 48000), after = rmsDb(s.l, 60000, 72000);
    std::printf("  Duck 100 %%: wet during the dry %.1f dB, 0.25-0.5 s after it stops %.1f dB\n", under, after);
    CHECK(after > under - 15.0);
}

void testFxTone() {
    testFxEq6();
    testFxSubStack();
    testFxCalm();
    testFxBloom();
}

} // namespace hc::test
