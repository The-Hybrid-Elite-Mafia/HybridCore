// HEM VOX ONE, HEM BUS PRESS, HEM STRIP: what each one must actually do to the sound.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <cstdio>

namespace hc::test {

using namespace fxt;

namespace {
// Level (dB RMS) of x over [from, to) seconds.
double secDb(const std::vector<float>& x, double from, double to) {
    return rmsDb(x, static_cast<std::size_t>(from * 48000.0), static_cast<std::size_t>(to * 48000.0));
}
// 1 kHz sine alternating loud / quiet every 2 s (8 s): long enough for an opto release.
Stereo loudQuiet(float loudAmp, float quietAmp, double hz = 1000.0) {
    Stereo s = sine(hz, 8.0, 1.0f);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const float g = (i / 96000) % 2 == 0 ? loudAmp : quietAmp;
        s.l[i] *= g; s.r[i] *= g;
    }
    return s;
}
} // namespace

void testFxDynamics() {
    std::printf("[fx] HEM BUS PRESS, HEM STRIP, HEM VOX ONE: what they do to the sound\n");

    // ---- BUS PRESS -------------------------------------------------------------------
    {
        auto e = fx::makeBusPress();
        e->prepare(48000.0, 512);
        e->set(param(*e, "threshold"), -20.0f);
        e->set(param(*e, "ratio"), 2.0f);   // 4:1
        e->set(param(*e, "attack"), 1.0f);
        e->set(param(*e, "release"), 300.0f);
        const double g = gainAtDb(*e, 1000.0, 0.5f);   // peak -6 dBFS: 14 dB over → 10.5 dB down
        std::printf("  BUS PRESS 4:1, -20 dB, sine peak -6 dBFS: %.2f dB (expected -10.5), meter %.1f dB\n", g, e->reductionDb());
        CHECK(g < -9.0 && g > -12.0);
        CHECK(e->reductionDb() < -9.0f);
        e->set(param(*e, "ratio"), 3.0f);    // 10:1 → 12.6 dB down
        const double g10 = gainAtDb(*e, 1000.0, 0.5f);
        CHECK(g10 < g - 1.0 && g10 > -14.5);
        const double below = gainAtDb(*e, 1000.0, 0.02f);   // -34 dBFS: under the threshold
        CHECK(std::fabs(below) < 0.2);
        e->set(param(*e, "makeup"), 6.0f);
        CHECK(std::fabs(gainAtDb(*e, 1000.0, 0.02f) - 6.0) < 0.3);
        // Mix 0 % = the dry signal exactly (no latency).
        e->set(param(*e, "mix"), 0.0f);
        e->set(param(*e, "makeup"), 0.0f);
        e->reset();
        Stereo s = noise(0.5, 0.8f), ref = s;
        run(*e, s);
        float err = 0;
        for (std::size_t i = 0; i < s.size(); ++i) err = std::max(err, std::fabs(s.l[i] - ref.l[i]));
        CHECK(err < 1e-6f);
        // Detector low cut: a 40 Hz sine is compressed much less with SC LOW CUT at 300 Hz.
        e->set(param(*e, "mix"), 100.0f);
        e->set(param(*e, "ratio"), 2.0f);
        const double low0 = gainAtDb(*e, 40.0, 0.5f);
        e->set(param(*e, "sclowcut"), 300.0f);
        const double low1 = gainAtDb(*e, 40.0, 0.5f);
        std::printf("  BUS PRESS 40 Hz: %.1f dB, with SC low cut 300 Hz: %.1f dB\n", low0, low1);
        CHECK(low1 > low0 + 6.0);
        // Auto release: after a long loud passage it recovers slower than after a short hit.
        e->set(param(*e, "sclowcut"), 20.0f);
        e->set(param(*e, "auto"), 1.0f);
        auto recoverDb = [&](double loudSec) {
            e->reset();
            Stereo t = sine(1000.0, loudSec + 0.3, 0.02f);
            for (std::size_t i = 0; i < static_cast<std::size_t>(loudSec * 48000); ++i) { t.l[i] *= 25.0f; t.r[i] *= 25.0f; }
            run(*e, t);
            const double q0 = loudSec + 0.15;   // 150 ms after the loud part ends
            return secDb(t.l, q0, q0 + 0.05) - (20.0 * std::log10(0.02 / std::sqrt(2.0)));
        };
        const double afterShort = recoverDb(0.03), afterLong = recoverDb(2.0);
        std::printf("  BUS PRESS auto release, 150 ms after: short hit %.2f dB, 2 s passage %.2f dB\n", afterShort, afterLong);
        CHECK(afterShort > -1.0 && afterLong < afterShort - 1.5);
    }

    // ---- STRIP ----------------------------------------------------------------------
    {
        auto e = fx::makeStrip();
        e->prepare(48000.0, 512);
        CHECK(std::fabs(gainAtDb(*e, 1000.0)) < 0.1);   // defaults: transparent
        const struct { const char* id; double hz; } bands[] = {{"low", 50.0}, {"mid", 1000.0}, {"high", 16000.0}};
        for (const auto& b : bands)
            for (float db : {9.0f, -9.0f}) {
                e->setDefaults();
                e->set(param(*e, b.id), db);
                const double got = gainAtDb(*e, b.hz);
                if (std::fabs(got - db) > 1.0) std::printf("  STRIP %s %+.0f dB at %.0f Hz: got %.2f\n", b.id, db, b.hz, got);
                CHECK(std::fabs(got - db) < 1.0);
            }
        e->setDefaults();
        e->set(param(*e, "mid"), 9.0f);
        e->set(param(*e, "midfreq"), 3000.0f);
        CHECK(gainAtDb(*e, 3000.0) > 8.0 && gainAtDb(*e, 300.0) < 1.5);
        e->setDefaults();
        e->set(param(*e, "lowcut"), 200.0f);
        CHECK(gainAtDb(*e, 50.0) < -20.0 && std::fabs(gainAtDb(*e, 2000.0)) < 0.2);
        // Compressor: 4:1 at -30 dB, sine peak -10 dBFS → 15 dB down.
        e->setDefaults();
        e->set(param(*e, "compthr"), -30.0f);
        e->set(param(*e, "ratio"), 4.0f);
        e->set(param(*e, "attack"), 1.0f);
        e->set(param(*e, "release"), 300.0f);
        const double c = gainAtDb(*e, 1000.0, 0.316f);
        std::printf("  STRIP comp 4:1, -30 dB, peak -10 dBFS: %.2f dB (expected -15)\n", c);
        CHECK(c < -13.5 && c > -16.5);
        // Gate: -40 dB threshold, 80 dB range. Quiet (-60 dBFS) is shut, loud (-10) passes.
        e->setDefaults();
        e->set(param(*e, "gatethr"), -40.0f);
        e->set(param(*e, "gaterange"), 80.0f);
        const double quiet = gainAtDb(*e, 1000.0, 0.001f);
        const float grShut = e->reductionDb();   // the meter shows the closed gate too
        const double loud = gainAtDb(*e, 1000.0, 0.316f);
        std::printf("  STRIP gate -40 dB: quiet signal %.1f dB (meter %.1f dB), loud signal %.2f dB\n", quiet, grShut, loud);
        CHECK(quiet < -40.0 && std::fabs(loud) < 0.2 && grShut < -40.0f);
        // Reduction amounts read as reductions: "-60.0 dB", and typing either sign means the same.
        const fx::ParamInfo& range = e->params()[static_cast<std::size_t>(param(*e, "gaterange"))];
        float back = 0;
        CHECK(fx::formatValue(range, 60.0f) == "-60.0 dB" && fx::parseValue(range, "-60 dB", back) && back == 60.0f &&
              fx::parseValue(range, "30", back) && back == 30.0f);
    }

    // ---- VOX ONE --------------------------------------------------------------------
    {
        auto e = fx::makeVoxOne();
        e->prepare(48000.0, 512);
        e->set(param(*e, "amount"), 0.0f);
        const double t1 = gainAtDb(*e, 1000.0, 0.3f), t2 = gainAtDb(*e, 3000.0, 0.3f), lc = gainAtDb(*e, 30.0, 0.3f);
        std::printf("  VOX ONE amount 0: 1 kHz %.2f dB, 3 kHz %.2f dB, 30 Hz %.1f dB (low cut)\n", t1, t2, lc);
        CHECK(std::fabs(t1) < 0.2 && std::fabs(t2) < 0.3 && lc < -12.0);
        // Evening out: the loud-quiet difference (20 dB in) shrinks a lot at full amount.
        auto spread = [&](float amount) {
            e->setDefaults();
            e->set(param(*e, "amount"), amount);
            e->reset();
            Stereo s = loudQuiet(0.5f, 0.05f);
            run(*e, s);
            return secDb(s.l, 5.5, 6.0) - secDb(s.l, 7.5, 8.0);   // settled loud vs settled quiet
        };
        const double sp0 = spread(0.0f), sp100 = spread(100.0f);
        std::printf("  VOX ONE loud vs quiet: amount 0 %.1f dB, amount 100 %.1f dB\n", sp0, sp100);
        CHECK(std::fabs(sp0 - 20.0) < 0.5 && sp100 < sp0 - 6.0);
        // De-esser: a 7 kHz burst is turned down more than a 1 kHz one (beyond the presence lift).
        e->setDefaults();
        e->set(param(*e, "amount"), 80.0f);
        const double s7 = gainAtDb(*e, 7000.0, 0.3f), s1 = gainAtDb(*e, 1000.0, 0.3f);
        // The leveler treats both the same; take out the presence / air lift (4 kHz +3.2 dB,
        // 12 kHz +2.4 dB at amount 80) to see the de-esser alone.
        using T = fx::BiquadCoefs::Type;
        const auto pres = fx::BiquadCoefs::make(T::HighShelf, 48000.0, 4000.0, 0.7071, 3.2);
        const auto air  = fx::BiquadCoefs::make(T::HighShelf, 48000.0, 12000.0, 0.7071, 2.4);
        const double eq = (pres.magnitudeDb(48000.0, 7000.0) + air.magnitudeDb(48000.0, 7000.0)) -
                          (pres.magnitudeDb(48000.0, 1000.0) + air.magnitudeDb(48000.0, 1000.0));
        const double deEss = (s7 - s1) - eq;
        std::printf("  VOX ONE amount 80: 7 kHz %.2f dB, 1 kHz %.2f dB, de-esser on 7 kHz %.2f dB\n", s7, s1, deEss);
        CHECK(deEss < -3.0);
        // Mix 0 % = dry.
        e->setDefaults();
        e->set(param(*e, "amount"), 100.0f);
        e->set(param(*e, "mix"), 0.0f);
        CHECK(std::fabs(gainAtDb(*e, 1000.0, 0.3f)) < 0.05);
    }
}

} // namespace hc::test
