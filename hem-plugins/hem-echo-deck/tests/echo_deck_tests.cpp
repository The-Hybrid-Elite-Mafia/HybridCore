// HEM ECHO DECK: what each control must actually do to the sound, measured with numbers.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hem/echodeck/EchoDeckConfig.hpp"
#include "hem/echodeck/HemEchoDeck.hpp"

#include <chrono>
#include <cstdio>
#include <numbers>
#include <string>

namespace hc::test {

using namespace fxt;

namespace {
constexpr double kFs = 48000.0;

std::unique_ptr<fx::Effect> make(double fs = kFs) {
    auto e = hem::echodeck::make();
    e->prepare(fs, 512);
    return e;
}
void setp(fx::Effect& e, const char* id, float v) { e.set(param(e, id), v); }
// Echo only (mix 100 = no dry), clean, MS mode at `ms`.
void wetOnly(fx::Effect& e, float ms) {
    setp(e, "mix", 100.0f); setp(e, "timemode", 0.0f); setp(e, "time", ms); setp(e, "style", 0.0f);
    e.reset();
}
Stereo impulse(double seconds, float amp = 0.5f) { Stereo s(static_cast<std::size_t>(seconds * kFs)); s.l[0] = s.r[0] = amp; return s; }
// Where the echoes land: indices of local peaks above `thr`, in order.
std::vector<std::size_t> hits(const std::vector<float>& x, float thr) {
    std::vector<std::size_t> h;
    for (std::size_t i = 1; i + 1 < x.size(); ++i)
        if (std::fabs(x[i]) > thr && std::fabs(x[i]) >= std::fabs(x[i - 1]) && std::fabs(x[i]) > std::fabs(x[i + 1])) h.push_back(i);
    return h;
}
double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
double tone(const std::vector<float>& x, double f, std::size_t a, std::size_t b) {
    double re = 0, im = 0;
    for (std::size_t i = a; i < b; ++i) { const double w = 2 * std::numbers::pi * f * static_cast<double>(i) / kFs; re += x[i] * std::cos(w); im += x[i] * std::sin(w); }
    return std::sqrt(re * re + im * im) / static_cast<double>(b - a) * 2.0;
}
// Level (dB) of the FIRST echo of a steady tone at hz, relative to the input tone. 100 ms echo, no feedback.
double echoGainDb(fx::Effect& e, double hz, float amp = 0.1f) {
    setp(e, "mix", 100.0f); setp(e, "timemode", 0.0f); setp(e, "time", 100.0f); setp(e, "feedback", 0.0f);
    e.reset();
    Stereo s = sine(hz, 0.6, amp); run(e, s);
    // RMS over 0.3 s .. 0.6 s (wow / flutter bend the pitch a little, so a single-frequency probe would under-read).
    return rmsDb(s.l, 14400, 28800) - 20.0 * std::log10(amp / std::sqrt(2.0));
}
} // namespace

void testFxEchoDeck() {
    std::printf("[fx] HEM ECHO DECK: every control measured\n");

    {   // 0. Defaults (zero / neutral unless a delay needs something else to be heard).
        auto e = make();
        auto v = [&](const char* id) { return e->get(param(*e, id)); };
        auto txt = [&](const char* id) { const int i = param(*e, id); return fx::formatValue(e->params()[static_cast<std::size_t>(i)], e->get(i)); };
        std::printf("  defaults: input %g, output %g, mix %g, feedback %g, groove %g, feel %g, saturation %g, low cut %g, high cut %g, time mode %s, note %s, tempo %s, style %s, mode %s\n",
                    v("input"), v("output"), v("mix"), v("feedback"), v("groove"), v("feel"), v("saturation"), v("lowcut"), v("highcut"),
                    txt("timemode").c_str(), txt("note").c_str(), txt("temposrc").c_str(), txt("style").c_str(), txt("mode").c_str());
        CHECK(v("input") == 0.0f && v("output") == 0.0f && v("feedback") == 0.0f && v("groove") == 0.0f && v("feel") == 0.0f && v("saturation") == 0.0f);
        CHECK(v("mix") == 50.0f && v("lowcut") == hem::echodeck::cfg::kLowCutOffHz && v("highcut") == hem::echodeck::cfg::kHighCutOffHz);
        CHECK(txt("timemode") == "NOTE" && txt("note") == "1/4" && txt("temposrc") == "HOST" && txt("style") == "CLEAN" && txt("mode") == "SINGLE");
        CHECK(e->params().size() == 17);
    }
    {   // 1. ECHO TIME in MS mode lands on the exact sample.
        const float ms[] = {1.0f, 10.0f, 250.0f, 1000.0f, 2500.0f};
        for (float t : ms) {
            auto e = make(); wetOnly(*e, t);
            Stereo s = impulse(t * 0.001 + 0.1); run(*e, s);
            const auto h = hits(s.l, 0.25f);
            const double got = h.empty() ? -1.0 : static_cast<double>(h[0]) / kFs * 1000.0;
            std::printf("  TIME %g ms: first echo at %.3f ms (%zu samples), level %.3f\n", t, got, h.empty() ? 0 : h[0], h.empty() ? 0.0f : s.l[h[0]]);
            CHECK(!h.empty() && std::fabs(got - t) < 0.03 && std::fabs(s.l[h[0]] - 0.5f) < 1e-3f);
        }
    }
    {   // 2. NOTE / DOT / TRIP at the host tempo, and the own (TAP) tempo.
        struct Case { int mode; int note; float bpm; int src; double expMs; const char* what; };
        const Case cs[] = {{1, 4, 120.0f, 0, 500.0, "1/4 @ host 120"}, {2, 3, 120.0f, 0, 375.0, "1/8 DOT @ host 120"},
                           {3, 4, 120.0f, 0, 1000.0 / 3.0, "1/4 TRIP @ host 120"}, {1, 2, 90.0f, 0, 1000.0 / 6.0, "1/16 @ host 90"},
                           {1, 4, 100.0f, 1, 600.0, "1/4 @ own 100 BPM"}};
        for (const auto& c : cs) {
            auto e = make(); wetOnly(*e, 100.0f);
            setp(*e, "timemode", static_cast<float>(c.mode)); setp(*e, "note", static_cast<float>(c.note));
            setp(*e, "temposrc", static_cast<float>(c.src));
            if (c.src) setp(*e, "bpm", c.bpm); else e->setTempo(c.bpm);
            e->reset();
            Stereo s = impulse(c.expMs * 0.001 + 0.1); run(*e, s);
            const auto h = hits(s.l, 0.25f);
            const double got = h.empty() ? -1.0 : static_cast<double>(h[0]) / kFs * 1000.0;
            std::printf("  %s: first echo at %.3f ms (expected %.3f)\n", c.what, got, c.expMs);
            CHECK(!h.empty() && std::fabs(got - c.expMs) < 0.05);
        }
    }
    {   // 3. FEEDBACK: each repeat is FEEDBACK x the one before.
        const float fbs[] = {25.0f, 50.0f, 90.0f};
        for (float fb : fbs) {
            auto e = make(); setp(*e, "feedback", fb); wetOnly(*e, 100.0f);
            Stereo s = impulse(0.75); run(*e, s);
            const auto h = hits(s.l, 0.001f);
            CHECK(h.size() >= 5);
            if (h.size() < 5) continue;
            const double d1 = dbOf(s.l[h[1]] / s.l[h[0]]), d4 = dbOf(s.l[h[4]] / s.l[h[3]]);
            std::printf("  FEEDBACK %g%%: repeat 2 vs 1 %.2f dB, repeat 5 vs 4 %.2f dB (expected %.2f), spacing %zu / %zu samples\n",
                        fb, d1, d4, dbOf(fb * 0.01), h[1] - h[0], h[4] - h[3]);
            CHECK(std::fabs(d1 - dbOf(fb * 0.01)) < 0.05 && std::fabs(d4 - dbOf(fb * 0.01)) < 0.05);
            CHECK(h[1] - h[0] == 4800 && h[4] - h[3] == 4800);
        }
        auto e = make(); wetOnly(*e, 100.0f);   // FEEDBACK 0 = one echo only
        Stereo s = impulse(0.5); run(*e, s);
        const auto h = hits(s.l, 1e-4f);
        std::printf("  FEEDBACK 0: %zu echo(es)\n", h.size());
        CHECK(h.size() == 1);
    }
    {   // 4. GROOVE: every other repeat moves; the pairs stay on the grid.
        const float gv[] = {-100.0f, -50.0f, 50.0f, 100.0f};
        for (float g : gv) {
            auto e = make(); setp(*e, "feedback", 80.0f); setp(*e, "groove", g); wetOnly(*e, 300.0f);
            Stereo s = impulse(1.5); run(*e, s);
            const auto h = hits(s.l, 0.01f);
            CHECK(h.size() >= 4);
            if (h.size() < 4) continue;
            const double r1 = static_cast<double>(h[0]) / kFs * 1000, r2 = static_cast<double>(h[1]) / kFs * 1000;
            const double r3 = static_cast<double>(h[2]) / kFs * 1000, r4 = static_cast<double>(h[3]) / kFs * 1000;
            const double exp1 = 300.0 * (1.0 - g * 0.01 / 3.0);
            std::printf("  GROOVE %+g%%: repeats at %.1f, %.1f, %.1f, %.1f ms (repeat 1 expected %.1f, pairs on 600 / 1200)\n", g, r1, r2, r3, r4, exp1);
            CHECK(std::fabs(r1 - exp1) < 0.1 && std::fabs(r2 - 600.0) < 0.1 && std::fabs(r4 - 1200.0) < 0.1 && std::fabs(r3 - (600.0 + exp1)) < 0.1);
        }
    }
    {   // 5. FEEL: every repeat moves by the same amount (DRAG = late, RUSH = early).
        const float fv[] = {-100.0f, 100.0f};
        for (float f : fv) {
            auto e = make(); setp(*e, "feedback", 50.0f); setp(*e, "feel", f); wetOnly(*e, 200.0f);
            Stereo s = impulse(0.8); run(*e, s);
            const auto h = hits(s.l, 0.01f);
            CHECK(h.size() >= 3);
            if (h.size() < 3) continue;
            const double sh = -f * 0.01 * 30.0;
            std::printf("  FEEL %+g%% (%s): repeats at %.2f, %.2f, %.2f ms (expected shift %+.0f ms on all)\n", f, f < 0 ? "DRAG" : "RUSH",
                        static_cast<double>(h[0]) / kFs * 1000, static_cast<double>(h[1]) / kFs * 1000, static_cast<double>(h[2]) / kFs * 1000, sh);
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(static_cast<double>(h[static_cast<std::size_t>(k)]) / kFs * 1000 - (200.0 * (k + 1) + sh)) < 0.05);
        }
    }
    {   // 6. MIX law: 0 = dry only, 50 = dry and echo both full, 100 = echo only. 25 = echo at half.
        const float mv[] = {0.0f, 25.0f, 50.0f, 75.0f, 100.0f};
        for (float m : mv) {
            auto e = make(); wetOnly(*e, 100.0f); setp(*e, "mix", m); e->reset();
            Stereo s = impulse(0.2); run(*e, s);
            std::printf("  MIX %g%%: dry %.3f, echo %.3f (of 0.5)\n", m, s.l[0], s.l[4800]);
            const float dry = std::min(1.0f, 2.0f * (1.0f - m * 0.01f)) * 0.5f, wet = std::min(1.0f, 2.0f * m * 0.01f) * 0.5f;
            CHECK(std::fabs(s.l[0] - dry) < 1e-4f && std::fabs(s.l[4800] - wet) < 1e-3f);
        }
    }
    {   // 7. INPUT and OUTPUT change the echo only, never the dry sound.
        auto e = make(); wetOnly(*e, 100.0f); setp(*e, "mix", 50.0f); setp(*e, "input", 6.0f); setp(*e, "output", -12.0f); e->reset();
        Stereo s = impulse(0.2, 0.05f); run(*e, s);
        const double dry = dbOf(s.l[0] / 0.05), wet = dbOf(s.l[4800] / 0.05);
        std::printf("  INPUT +6 / OUTPUT -12: dry %+.3f dB, echo %+.3f dB (expected 0 / -6.02)\n", dry, wet);
        CHECK(std::fabs(dry) < 1e-3 && std::fabs(wet + 6.02) < 0.02);
    }
    {   // 8. LOW CUT / HIGH CUT on the echo (12 dB/oct), and off at their ends.
        auto e = make();
        const double flat100 = echoGainDb(*e, 100.0), flat10k = echoGainDb(*e, 10000.0);
        setp(*e, "lowcut", 200.0f);
        const double lc200 = echoGainDb(*e, 200.0), lc50 = echoGainDb(*e, 50.0), lc2k = echoGainDb(*e, 2000.0);
        setp(*e, "lowcut", 20.0f); setp(*e, "highcut", 5000.0f);
        const double hc5k = echoGainDb(*e, 5000.0), hc10k = echoGainDb(*e, 10000.0), hc500 = echoGainDb(*e, 500.0);
        std::printf("  filters off: 100 Hz %+.2f dB, 10 kHz %+.2f dB\n", flat100, flat10k);
        std::printf("  LOW CUT 200 Hz: 50 Hz %+.2f, 200 Hz %+.2f, 2 kHz %+.2f dB\n", lc50, lc200, lc2k);
        std::printf("  HIGH CUT 5 kHz: 500 Hz %+.2f, 5 kHz %+.2f, 10 kHz %+.2f dB\n", hc500, hc5k, hc10k);
        CHECK(std::fabs(flat100) < 0.05 && std::fabs(flat10k) < 0.05);
        CHECK(std::fabs(lc200 + 3.01) < 0.2 && lc50 < -22.0 && std::fabs(lc2k) < 0.1);
        CHECK(std::fabs(hc5k + 3.01) < 0.2 && hc10k < -11.0 && std::fabs(hc500) < 0.1);
    }
    {   // 9. Filters are in the loop: each repeat gets darker again.
        auto e = make(); setp(*e, "feedback", 100.0f); setp(*e, "highcut", 3000.0f); wetOnly(*e, 100.0f);
        Stereo s = sine(6000.0, 0.05, 0.3f); s.l.resize(28800, 0.0f); s.r.resize(28800, 0.0f); run(*e, s);
        const double e1 = dbOf(tone(s.l, 6000.0, 4900, 7100) / 0.3), e2 = dbOf(tone(s.l, 6000.0, 9700, 11900) / 0.3), e3 = dbOf(tone(s.l, 6000.0, 14500, 16700) / 0.3);
        std::printf("  HIGH CUT 3 kHz in the loop, 6 kHz tone, feedback 100%%: repeat 1 %.1f, 2 %.1f, 3 %.1f dB\n", e1, e2, e3);
        CHECK(e2 < e1 - 8.0 && e3 < e2 - 8.0);
    }
    {   // 10. SATURATION: more knob = more harmonics on the echo; at 0 a quiet echo stays clean.
        double h3[3]; const float sv[] = {0.0f, 50.0f, 100.0f};
        for (int k = 0; k < 3; ++k) {
            auto e = make(); setp(*e, "style", 2.0f); setp(*e, "saturation", sv[k]);
            setp(*e, "mix", 100.0f); setp(*e, "timemode", 0.0f); setp(*e, "time", 100.0f); e->reset();
            Stereo s = sine(500.0, 0.6, 0.5f); run(*e, s);
            const double f1 = tone(s.l, 500.0, 14400, 28800), t3 = tone(s.l, 1500.0, 14400, 28800);
            h3[k] = dbOf(t3 / f1);
        }
        std::printf("  SATURATION (TAPE, -6 dBFS 500 Hz): 3rd harmonic %.1f dB at 0%%, %.1f at 50%%, %.1f at 100%%\n", h3[0], h3[1], h3[2]);
        CHECK(h3[1] > h3[0] + 6.0 && h3[2] > h3[1] + 3.0);
        auto e = make(); setp(*e, "mix", 100.0f); setp(*e, "timemode", 0.0f); setp(*e, "time", 100.0f); e->reset();
        Stereo s = sine(500.0, 0.6, 0.5f); run(*e, s);
        const double clean3 = dbOf(tone(s.l, 1500.0, 14400, 28800) / tone(s.l, 500.0, 14400, 28800));
        std::printf("  CLEAN style, SATURATION 0, -6 dBFS: 3rd harmonic %.1f dB\n", clean3);
        CHECK(clean3 < -100.0);
    }
    {   // 11. STYLES: each one's measured band (first echo, -20 dBFS tones) and its level on a quiet tone.
        for (int st = 0; st < static_cast<int>(hem::echodeck::cfg::kStyles.size()); ++st) {
            auto e = make(); setp(*e, "style", static_cast<float>(st));
            const double g100 = echoGainDb(*e, 100.0), g1k = echoGainDb(*e, 1000.0), g10k = echoGainDb(*e, 10000.0);
            std::printf("  STYLE %-9s: 100 Hz %+6.1f, 1 kHz %+5.1f, 10 kHz %+6.1f dB\n", hem::echodeck::cfg::kStyles[static_cast<std::size_t>(st)].name, g100, g1k, g10k);
            CHECK(std::isfinite(g100) && std::isfinite(g1k) && std::isfinite(g10k));
            CHECK(g1k > -6.0 && g1k < 1.0);   // every style passes the middle
            if (st != 0) CHECK(g10k < -0.5 || g100 < -0.5);   // every coloured style bands the echo somewhere
        }
    }
    {   // 12. PING-PONG: mono in, repeat 1 left only, repeat 2 right only, then left again.
        auto e = make(); setp(*e, "mode", 1.0f); setp(*e, "feedback", 50.0f); wetOnly(*e, 100.0f);
        Stereo s = impulse(0.4); s.r[0] = 0.0f; run(*e, s);   // left-only hit: summed to mono
        const float l1 = s.l[4800], r1 = s.r[4800], l2 = s.l[9600], r2 = s.r[9600], l3 = s.l[14400], r3 = s.r[14400];
        std::printf("  PING-PONG: repeat 1 L %.3f R %.3f, repeat 2 L %.3f R %.3f, repeat 3 L %.3f R %.3f\n", l1, r1, l2, r2, l3, r3);
        CHECK(std::fabs(l1 - 0.25f) < 1e-3f && std::fabs(r1) < 1e-6f && std::fabs(r2 - 0.125f) < 1e-3f && std::fabs(l2) < 1e-6f);
        CHECK(std::fabs(l3 - 0.0625f) < 1e-3f && std::fabs(r3) < 1e-6f);
    }
    {   // 13. TAP TEMPO: taps every 0.5 s (blocks of 64) -> about 120 BPM, switches the tempo to TAP; a 3 s gap starts again.
        auto e = make(); wetOnly(*e, 100.0f); setp(*e, "timemode", 1.0f); e->reset();
        Stereo blk(64);
        auto runFor = [&](double sec) { for (int k = 0; k < static_cast<int>(sec * kFs / 64.0); ++k) e->process(blk.l.data(), blk.r.data(), 64); };
        float tv = 0.0f;
        runFor(0.1);
        for (int t = 0; t < 5; ++t) { tv = 1.0f - tv; setp(*e, "tap", tv); runFor(0.5); }
        const double bpm1 = e->get(param(*e, "bpm"));
        std::printf("  TAP every 0.5 s: %.2f BPM (expected 120), tempo now %s, BPM meter %.2f\n", bpm1,
                    e->get(param(*e, "temposrc")) >= 0.5f ? "TAP" : "HOST", e->meter(1));
        CHECK(std::fabs(bpm1 - 120.0) < 0.5 && e->get(param(*e, "temposrc")) >= 0.5f);
        runFor(3.0);
        for (int t = 0; t < 3; ++t) { tv = 1.0f - tv; setp(*e, "tap", tv); runFor(0.75); }
        const double bpm2 = e->get(param(*e, "bpm"));
        std::printf("  3 s pause, then taps every 0.75 s: %.2f BPM (expected 80)\n", bpm2);
        CHECK(std::fabs(bpm2 - 80.0) < 0.5);
        // the echo follows the tapped tempo
        Stereo s = impulse(1.0); run(*e, s);
        const auto h = hits(s.l, 0.25f);
        std::printf("  echo after tapping 80 BPM, 1/4 note: %.2f ms (expected 750)\n", h.empty() ? -1.0 : static_cast<double>(h[0]) / kFs * 1000);
        CHECK(!h.empty() && std::fabs(static_cast<double>(h[0]) / kFs * 1000 - 750.0) < 1.0);
    }
    {   // 14. Changing the note value cross-fades (no jump): the biggest sample step stays small.
        auto e = make(); setp(*e, "mix", 100.0f); setp(*e, "feedback", 70.0f); e->setTempo(120.0); e->reset();
        Stereo s = sine(220.0, 2.0, 0.3f);
        float worst = 0.0f, steady = 0.0f, prev = 0.0f;
        for (std::size_t i = 0; i < s.size(); i += 256) {
            if (i == 48000) setp(*e, "note", 3.0f);   // 1/4 -> 1/8
            const auto nn = static_cast<std::uint32_t>(std::min<std::size_t>(256, s.size() - i));
            e->process(s.l.data() + i, s.r.data() + i, nn);
            for (std::size_t k = i; k < i + nn; ++k) {
                const float st = std::fabs(s.l[k] - prev); prev = s.l[k];
                if (k > 30000 && k < 48000) steady = std::max(steady, st);
                if (k >= 48000 && k < 60000) worst = std::max(worst, st);
            }
        }
        std::printf("  note change 1/4 -> 1/8 while echoing: biggest step %.4f during the change, %.4f before\n", worst, steady);
        CHECK(worst < 2.0f * steady + 0.01f);
    }
    {   // 15. Can't blow up: FEEDBACK 100%, SATURATION 100%, INPUT +12, loud noise for 10 s, every style.
        float pk = 0.0f; bool ok = true;
        for (int sat = 0; sat <= 100; sat += 100)
        for (int st = 0; st < static_cast<int>(hem::echodeck::cfg::kStyles.size()); ++st) {
            auto e = make(); setp(*e, "style", static_cast<float>(st)); setp(*e, "feedback", 100.0f); setp(*e, "saturation", static_cast<float>(sat));
            setp(*e, "input", 12.0f); setp(*e, "mix", 100.0f); setp(*e, "timemode", 0.0f); setp(*e, "time", 30.0f); e->reset();
            Stereo s = noise(10.0, 0.9f, 11u + static_cast<unsigned>(st)); run(*e, s);
            ok = ok && finite(s); pk = std::max(pk, std::max(peak(s.l), peak(s.r)));
        }
        std::printf("  worst case (fb 100%%, sat 0 and 100%%, input +12, 10 s noise, all styles): peak %.2f (%.1f dBFS), finite %s\n", pk, dbOf(pk), ok ? "yes" : "NO");
        CHECK(ok && pk < 4.0f);
    }
    {   // 16. Which controls count right now (faceplate dims the rest).
        auto e = make();
        const int t = param(*e, "time"), n = param(*e, "note"), b = param(*e, "bpm");
        const bool a1 = e->paramActive(t), a2 = e->paramActive(n), a3 = e->paramActive(b);
        setp(*e, "temposrc", 1.0f);
        const bool a4 = e->paramActive(b);
        setp(*e, "timemode", 0.0f);
        const bool a5 = e->paramActive(t), a6 = e->paramActive(n);
        std::printf("  active: NOTE mode -> time %d note %d bpm(host) %d bpm(tap) %d; MS mode -> time %d note %d\n", a1, a2, a3, a4, a5, a6);
        CHECK(!a1 && a2 && !a3 && a4 && a5 && !a6);
    }
    {   // 17. Meters: echo time, BPM in use, IN and OUT levels.
        auto e = make(); wetOnly(*e, 250.0f); setp(*e, "input", -6.0f);
        Stereo s = sine(1000.0, 0.5, 0.5f); run(*e, s);
        std::printf("  meters: ECHO %.1f ms, BPM %.1f, IN %.2f dBFS (expected -12.0), OUT %.2f dBFS\n", e->meter(0), e->meter(1), e->meter(2), e->meter(3));
        CHECK(std::fabs(e->meter(0) - 250.0f) < 0.1f && std::fabs(e->meter(2) + 12.04f) < 0.1f && e->meter(3) > -13.0f && e->meter(3) < -11.0f);
    }
    {   // 18. 96 kHz: times still exact.
        auto e = make(96000.0); setp(*e, "mix", 100.0f); setp(*e, "timemode", 0.0f); setp(*e, "time", 250.0f); e->reset();
        Stereo s(48000); s.l[0] = s.r[0] = 0.5f; run(*e, s);
        const auto h = hits(s.l, 0.25f);
        std::printf("  96 kHz, TIME 250 ms: first echo at sample %zu (expected 24000)\n", h.empty() ? 0 : h[0]);
        CHECK(!h.empty() && h[0] == 24000);
    }
    {   // 19. Zero heap allocations in process(): every style, both modes, all time modes, taps, filter moves.
        auto e = make(); setp(*e, "feedback", 60.0f); e->reset();
        Stereo s = noise(0.25, 0.5f);
        std::uint64_t allocs = 0; int runs = 0;
        {
            AllocScope scope;
            for (int st = 0; st < static_cast<int>(hem::echodeck::cfg::kStyles.size()); ++st)
                for (int md = 0; md < 2; ++md)
                    for (int tm = 0; tm < 4; ++tm) {
                        setp(*e, "style", static_cast<float>(st)); setp(*e, "mode", static_cast<float>(md)); setp(*e, "timemode", static_cast<float>(tm));
                        setp(*e, "lowcut", 20.0f + 30.0f * static_cast<float>(tm)); setp(*e, "highcut", 20000.0f - 2000.0f * static_cast<float>(tm));
                        setp(*e, "tap", static_cast<float>((runs) & 1)); setp(*e, "groove", static_cast<float>(tm * 30 - 50)); setp(*e, "saturation", 40.0f);
                        run(*e, s, 256); ++runs;
                    }
            allocs = scope.count();
        }
        std::printf("  allocations in process(): %llu across %d runs (8 styles x 2 modes x 4 time modes, filters, taps, groove)\n", static_cast<unsigned long long>(allocs), runs);
        CHECK(allocs == 0);
    }
    {   // 20. Real-time cost per 512-sample block, worst style (diffusion + wow), feedback on, at 48 and 96 kHz.
        const double rates[] = {48000.0, 96000.0};
        for (double fs : rates) {
            auto e = make(fs); setp(*e, "style", 4.0f); setp(*e, "feedback", 70.0f); setp(*e, "saturation", 50.0f);
            setp(*e, "lowcut", 100.0f); setp(*e, "highcut", 8000.0f); setp(*e, "groove", 30.0f); setp(*e, "feel", -20.0f); e->reset();
            Stereo s = noise(10.0, 0.5f, 3, fs);
            const auto t0 = std::chrono::steady_clock::now();
            run(*e, s, 512);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const double blocks = static_cast<double>(s.size()) / 512.0, usPerBlock = ms * 1000.0 / blocks, budgetUs = 512.0 / fs * 1e6;
            std::printf("  speed @ %.0f kHz: %.1f us per 512-sample block = %.2f%% of the block's time (this build, this machine)\n", fs / 1000, usPerBlock, 100.0 * usPerBlock / budgetUs);
            CHECK(usPerBlock < budgetUs);
        }
    }
}

} // namespace hc::test
