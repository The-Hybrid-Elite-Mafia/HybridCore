// HEM COMPRESSOR: what each control must actually do to the sound, measured with numbers.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hem/compressor/HemCompressor.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <numbers>

namespace hc::test {

using namespace fxt;

namespace {
constexpr double kFs = 48000.0;
constexpr float kThrM20 = -2.0f;   // knob value that puts the threshold at -20 dBFS (knob + (-18))

std::unique_ptr<fx::Effect> make() {
    auto e = hem::compressor::make();
    e->prepare(kFs, 512);
    return e;
}
void setAll(fx::Effect& e, float thr, int ratio, int att, int rel, float sc, float mix, float mk, float tr, bool analog, bool in) {
    e.set(param(e, "threshold"), thr); e.set(param(e, "ratio"), static_cast<float>(ratio));
    e.set(param(e, "attack"), static_cast<float>(att)); e.set(param(e, "release"), static_cast<float>(rel));
    e.set(param(e, "scfilter"), sc); e.set(param(e, "mix"), mix); e.set(param(e, "makeup"), mk); e.set(param(e, "trim"), tr);
    e.set(param(e, "analog"), analog ? 1.0f : 0.0f); e.set(param(e, "in"), in ? 1.0f : 0.0f);
    e.set(param(e, "fade"), 0.0f);
}
// Plain base: threshold -20 dBFS, 4:1, 0.1 ms attack, 0.3 s release, nothing else on.
void base(fx::Effect& e) { setAll(e, kThrM20, 1, 0, 1, 0.0f, 100.0f, 0.0f, 0.0f, false, true); e.reset(); }

// Runs `in` through the effect in blocks of `B`; returns the gain reduction (dB, positive) after every block.
std::vector<float> runTrace(fx::Effect& e, Stereo& s, std::uint32_t B) {
    std::vector<float> gr;
    for (std::size_t i = 0; i < s.size(); i += B) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(B, s.size() - i));
        e.process(s.l.data() + i, s.r.data() + i, n);
        gr.push_back(-e.reductionDb());
    }
    return gr;
}
double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
double linOf(double d) { return std::pow(10.0, d / 20.0); }
double rmsOf(const std::vector<float>& x, std::size_t a, std::size_t b) {
    double t = 0; for (std::size_t i = a; i < b; ++i) t += static_cast<double>(x[i]) * x[i];
    return std::sqrt(t / static_cast<double>(b - a));
}
double square(double amp, double f, std::size_t i) { return std::sin(2 * std::numbers::pi * f * static_cast<double>(i) / kFs) >= 0 ? amp : -amp; }
double harmonic(const std::vector<float>& x, double f, std::size_t a, std::size_t b) {
    double re = 0, im = 0;
    for (std::size_t i = a; i < b; ++i) { const double w = 2 * std::numbers::pi * f * static_cast<double>(i) / kFs; re += x[i] * std::cos(w); im += x[i] * std::sin(w); }
    return std::sqrt(re * re + im * im) / static_cast<double>(b - a) * 2.0;
}
// Samples until the trace first drops to `frac` of its value at index `at` (in blocks of B samples). Returns seconds.
double fallTime(const std::vector<float>& gr, std::size_t atBlock, std::size_t B, double frac) {
    const float start = gr[atBlock - 1];
    std::size_t k = atBlock;
    while (k + 1 < gr.size() && gr[k] > frac * start) ++k;
    return static_cast<double>((k - atBlock) * B) / kFs;
}
} // namespace

void testFxCompressor() {
    std::printf("[fx] HEM COMPRESSOR: every control measured\n");
    const float A = static_cast<float>(linOf(-6.0));   // 1 kHz at -6 dBFS peak

    {   // 0. Defaults (Steven's rule: zero / neutral unless the original's published default says otherwise).
        auto e = make();
        auto v = [&](const char* id) { return e->get(param(*e, id)); };
        auto txt = [&](const char* id) { const int i = param(*e, id); return fx::formatValue(e->params()[static_cast<std::size_t>(i)], e->get(i)); };
        std::printf("  defaults: thr %g, make-up %g, trim %g, sc %g, mix %g, analog %g, in %g, fade %s, rate-s %g, ratio %s, attack %s, release %s\n",
                    v("threshold"), v("makeup"), v("trim"), v("scfilter"), v("mix"), v("analog"), v("in"), txt("fade").c_str(), v("fadetime"),
                    txt("ratio").c_str(), txt("attack").c_str(), txt("release").c_str());
        CHECK(v("threshold") == 0.0f && v("makeup") == 0.0f && v("trim") == 0.0f && v("scfilter") == 0.0f);
        CHECK(v("mix") == 100.0f && v("analog") == 1.0f && v("in") == 1.0f && v("fadetime") == 10.0f);
        CHECK(v("fade") == 0.0f && txt("fade") == "OFF");
        CHECK(txt("ratio") == "4:1" && txt("attack") == "3 ms" && txt("release") == "0.6 s");
    }
    {   // 1. Static compression: 2:1, 4:1, 10:1 at -20 dBFS threshold, tone at -6 dBFS peak (14 dB over, minus half the knee effect is zero this far over).
        const int idx[] = {0, 1, 2};
        const double exp[] = {7.0, 10.5, 12.6};
        for (int k = 0; k < 3; ++k) {
            auto e = make(); base(*e); e->set(param(*e, "ratio"), static_cast<float>(idx[k]));
            Stereo s = sine(1000.0, 1.0, A); run(*e, s);
            const double o = dbOf(peak(s.l, 43200));
            std::printf("  ratio idx %d: output peak %.2f dBFS (expected %.2f)\n", idx[k], o, -6.0 - exp[k]);
            CHECK(std::fabs(o - (-6.0 - exp[k])) < 1.2);
        }
    }
    {   // 2. Threshold direction: lower number = more compression (published).
        double gr[2];
        for (int k = 0; k < 2; ++k) {
            auto e = make(); base(*e); e->set(param(*e, "threshold"), k == 0 ? -15.0f : 15.0f);
            Stereo s = sine(1000.0, 1.0, A); run(*e, s); gr[k] = -e->reductionDb();
        }
        std::printf("  threshold -15: %.1f dB reduction, +15: %.1f dB reduction\n", gr[0], gr[1]);
        CHECK(gr[0] > gr[1] + 8.0);
        CHECK(gr[0] < 20.5 && gr[0] > 15.0);   // -33 dBFS threshold, 10 dB... deep squeeze
    }
    {   // 3. Below threshold = unity. Make-up and trim.
        auto e = make(); base(*e); e->set(param(*e, "threshold"), 15.0f);
        Stereo in = sine(1000.0, 1.0, static_cast<float>(linOf(-30.0))), s = in; run(*e, s);
        CHECK(std::fabs(dbOf(rmsOf(s.l, 40000, 48000) / rmsOf(in.l, 40000, 48000))) < 0.05);
        e->set(param(*e, "makeup"), 6.0f); e->reset(); s = in; run(*e, s);
        const double mk = dbOf(rmsOf(s.l, 40000, 48000) / rmsOf(in.l, 40000, 48000));
        std::printf("  make-up +6: %.2f dB\n", mk);
        CHECK(std::fabs(mk - 6.02) < 0.1);
        e->set(param(*e, "makeup"), 0.0f); e->set(param(*e, "trim"), -9.0f); e->reset(); s = in; run(*e, s);
        const double tr = dbOf(rmsOf(s.l, 40000, 48000) / rmsOf(in.l, 40000, 48000));
        std::printf("  trim -9: %.2f dB\n", tr);
        CHECK(std::fabs(tr + 9.0) < 0.1);
    }
    {   // 4. IN off = bypass of compressor, make-up and mix.
        auto e = make(); base(*e); e->set(param(*e, "in"), 0.0f); e->set(param(*e, "makeup"), 12.0f); e->set(param(*e, "mix"), 30.0f); e->reset();
        Stereo in = sine(1000.0, 1.0, A), s = in; run(*e, s);
        const double g = dbOf(rmsOf(s.l, 40000, 48000) / rmsOf(in.l, 40000, 48000));
        std::printf("  IN off: gain %.3f dB, meter %.2f dB\n", g, e->reductionDb());
        CHECK(std::fabs(g) < 0.01);
        CHECK(e->reductionDb() > -0.01f);
    }
    {   // 5. Mix 50% sits between dry and fully compressed.
        auto f = make(); base(*f); Stereo full = sine(1000.0, 1.0, A); run(*f, full);
        auto h = make(); base(*h); h->set(param(*h, "mix"), 50.0f); Stereo half = sine(1000.0, 1.0, A); run(*h, half);
        const double expected = 0.5 * A + 0.5 * peak(full.l, 43200);
        const double got = peak(half.l, 43200);
        std::printf("  mix 50%%: peak %.2f dBFS (expected %.2f)\n", dbOf(got), dbOf(expected));
        CHECK(std::fabs(dbOf(got) - dbOf(expected)) < 0.6);
    }
    {   // 6. Attack: time to 63% of the final gain reduction on a steady-level (square) tone.
        const float att[] = {2, 4, 5};   // steps 1 ms, 10 ms, 30 ms
        const double ms[] = {1, 10, 30};
        for (int k = 0; k < 3; ++k) {
            auto e = make(); base(*e);
            e->set(param(*e, "attack"), att[k]); e->set(param(*e, "release"), 3.0f); e->set(param(*e, "ratio"), 2.0f); e->reset();
            const std::size_t at = 24000;
            Stereo s(at + 48000);
            for (std::size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = static_cast<float>(i < at ? square(linOf(-50), 100, i) : square(linOf(-6), 100, i));
            const auto gr = runTrace(*e, s, 8);
            const float fin = gr[(at + 47000) / 8];
            std::size_t idx = at / 8; while (idx < gr.size() && gr[idx] < 0.632f * fin) ++idx;
            const double t = static_cast<double>(idx * 8 - at) / kFs * 1000.0;
            std::printf("  attack %g ms: 63%% in %.2f ms\n", ms[k], t);
            CHECK(std::fabs(t - ms[k]) < std::max(0.6, ms[k] * 0.15));
        }
    }
    {   // 7. Release: loud then quiet, time for the gain reduction to fall to 37%.
        const int rel[] = {0, 2, 3};
        const double sec[] = {0.1, 0.6, 1.2};
        for (int k = 0; k < 3; ++k) {
            auto e = make(); base(*e);
            e->set(param(*e, "release"), static_cast<float>(rel[k])); e->set(param(*e, "ratio"), 2.0f); e->reset();
            const std::size_t at = 24000;
            Stereo s(at + static_cast<std::size_t>(kFs * 3));
            for (std::size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = static_cast<float>((i < at ? A : linOf(-60.0)) * std::sin(2 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kFs));
            const auto gr = runTrace(*e, s, 8);
            const double t = fallTime(gr, at / 8, 8, 0.368);
            std::printf("  release %.1f s: fell to 37%% in %.3f s\n", sec[k], t);
            CHECK(std::fabs(t - sec[k]) < sec[k] * 0.2 + 0.02);
        }
    }
    {   // 8. AUTO release follows how long the peak lasted: a short burst lets go faster than a long one.
        double t[2];
        const double burst[2] = {0.04, 2.0};
        for (int k = 0; k < 2; ++k) {
            auto e = make(); base(*e); e->set(param(*e, "release"), 4.0f); e->set(param(*e, "ratio"), 2.0f); e->reset();
            const std::size_t at = 24000, len = static_cast<std::size_t>(burst[k] * kFs);
            Stereo s(at + len + static_cast<std::size_t>(kFs * 4));
            for (std::size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = static_cast<float>(((i >= at && i < at + len) ? A : linOf(-70.0)) * std::sin(2 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kFs));
            const auto gr = runTrace(*e, s, 8);
            t[k] = fallTime(gr, (at + len) / 8, 8, 0.368);
        }
        std::printf("  AUTO release: after a 40 ms burst fell in %.2f s, after a 2 s peak %.2f s\n", t[0], t[1]);
        CHECK(t[1] > t[0] * 1.5);
    }
    {   // 9. SC FILTER: a 60 Hz tone squeezes hard with the filter off, much less at 300 Hz. Below 20 Hz the filter is off.
        double g[3]; const float sc[3] = {0.0f, 300.0f, 15.0f};
        for (int k = 0; k < 3; ++k) {
            auto e = make(); base(*e); e->set(param(*e, "scfilter"), sc[k]); e->reset();
            Stereo s = sine(60.0, 1.0, A); run(*e, s); g[k] = -e->reductionDb();
        }
        std::printf("  SC FILTER: 60 Hz GR %.1f dB off, %.1f dB at 300 Hz, %.1f dB at 15 Hz (off)\n", g[0], g[1], g[2]);
        CHECK(g[1] < g[0] * 0.35);
        CHECK(std::fabs(g[2] - g[0]) < 0.3);
    }
    {   // 10. FADE has three steps: OFF (skip to unity) -> FADE OUT -> FADE IN. Old sessions: saved 1 = FADE OUT.
        auto e = make();
        const int fi = param(*e, "fade");
        const auto& fp = e->params()[static_cast<std::size_t>(fi)];
        CHECK(fp.taper == fx::Taper::Choice && fp.max == 2.0f);
        CHECK(fx::formatValue(fp, 0.0f) == "OFF" && fx::formatValue(fp, 1.0f) == "FADE OUT" && fx::formatValue(fp, 2.0f) == "FADE IN");
        base(*e); e->set(param(*e, "in"), 0.0f); e->set(param(*e, "fadetime"), 2.0f);
        e->set(fi, 1.0f);   // what an old saved session (toggle ON) loads as
        e->reset();
        std::vector<float> a(64), b(64);
        // Script: 0-2 s FADE OUT, 2-3 s FADE IN, 3-3.5 s FADE OUT, then OFF (skip).
        float outHalf = 0, outEnd = 0, inHalf = 0, atFlip = 0, beforeFlip = 0, back = 0, mid = 0, afterSkip = 0;
        for (std::size_t s = 0; s < static_cast<std::size_t>(kFs * 4); s += 64) {
            std::fill(a.begin(), a.end(), 1.0f); std::fill(b.begin(), b.end(), 1.0f);
            if (s == 96000) e->set(fi, 2.0f);
            if (s == 144000) e->set(fi, 1.0f);
            if (s == 168000) e->set(fi, 0.0f);
            e->process(a.data(), b.data(), 64);
            if (s == 48000) outHalf = a[0];
            if (s == 96000 - 64) { outEnd = a[63]; }
            if (s == 120000) inHalf = a[0];
            if (s == 144000 - 64) beforeFlip = a[63];
            if (s == 144000) atFlip = a[0];
            if (s == 168000 - 64) mid = a[63];
            if (s == 168000 + 1024) afterSkip = a[0];
            if (s == 192000 - 64) back = a[63];
        }
        std::printf("  FADE OUT (old saved 1): %.3f at 1 s, %.4f at 2 s; FADE IN: %.3f after 0.5 s; OUT again from %.3f -> %.3f, %.3f at 0.5 s; OFF (skip): %.3f 21 ms later, %.3f at end\n",
                    outHalf, outEnd, inHalf, beforeFlip, atFlip, mid, afterSkip, back);
        CHECK(std::fabs(outHalf - 0.5f) < 0.02f); CHECK(outEnd < 0.002f);
        CHECK(std::fabs(inHalf - 0.25f) < 0.02f);
        CHECK(std::fabs(atFlip - beforeFlip) < 0.002f);   // FADE IN / OUT carry on from the current level (no jump)
        CHECK(std::fabs(mid - 0.25f) < 0.02f);
        CHECK(afterSkip > 0.999f && back > 0.999f);
    }
    {   // 10b. FADE IN on its own starts from the current level (already unity = stays at unity).
        auto e = make(); base(*e); e->set(param(*e, "in"), 0.0f); e->set(param(*e, "fade"), 2.0f); e->reset();
        Stereo s = sine(1000.0, 0.5, 0.5f), in = s; run(*e, s);
        CHECK(std::memcmp(s.l.data(), in.l.data(), s.size() * sizeof(float)) == 0);
    }
    {   // 11. ANALOG = subtle console colour, not a squash. Level change ANALOG on vs off (no compression), harmonics, noise.
        const float lv[3] = {0.0f, -6.0f, -30.0f};
        double dLev[3], h2[3], h3[3];
        for (int k = 0; k < 3; ++k) {
            double fund[2];
            for (int on = 0; on < 2; ++on) {
                // Mix 0% = the compressor's wet path is out, so nothing is compressed even at 0 dBFS.
                auto e = make(); base(*e); e->set(param(*e, "threshold"), 15.0f); e->set(param(*e, "mix"), 0.0f);
                e->set(param(*e, "analog"), on ? 1.0f : 0.0f); e->reset();
                Stereo s = sine(1000.0, 1.0, static_cast<float>(linOf(lv[k]))); run(*e, s);
                fund[on] = harmonic(s.l, 1000, 24000, 48000);
                if (on) { h2[k] = dbOf(harmonic(s.l, 2000, 24000, 48000) / fund[on]); h3[k] = dbOf(harmonic(s.l, 3000, 24000, 48000) / fund[on]); }
            }
            dLev[k] = dbOf(fund[1] / fund[0]);
        }
        std::printf("  ANALOG level change (on vs off): %+.3f dB at 0 dBFS, %+.3f dB at -6, %+.3f dB at -30\n", dLev[0], dLev[1], dLev[2]);
        std::printf("  ANALOG harmonics: 2nd %.1f / 3rd %.1f dB at 0 dBFS, %.1f / %.1f at -6, %.1f / %.1f at -30\n", h2[0], h3[0], h2[1], h3[1], h2[2], h3[2]);
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(dLev[k]) <= 0.1);
        const double sum6 = dbOf(std::sqrt(linOf(h2[1]) * linOf(h2[1]) + linOf(h3[1]) * linOf(h3[1])));
        const double sum0 = dbOf(std::sqrt(linOf(h2[0]) * linOf(h2[0]) + linOf(h3[0]) * linOf(h3[0])));
        const double sum30 = dbOf(std::sqrt(linOf(h2[2]) * linOf(h2[2]) + linOf(h3[2]) * linOf(h3[2])));
        std::printf("  ANALOG 2nd+3rd together: %.1f dB at 0 dBFS, %.1f at -6, %.1f at -30\n", sum0, sum6, sum30);
        CHECK(h2[1] > -66.0 && h2[1] < -50.0 && h3[1] > -66.0 && h3[1] < -50.0);
        CHECK(sum6 > -62.0 && sum6 < -50.0);
        CHECK(sum0 > sum6 && sum6 > sum30 && sum0 < -40.0);   // rises gently with level
        // Same compressor, wet path, ANALOG on: still no level loss when nothing is compressed.
        double fund[2];
        for (int on = 0; on < 2; ++on) {
            auto e = make(); base(*e); e->set(param(*e, "threshold"), 15.0f); e->set(param(*e, "analog"), on ? 1.0f : 0.0f); e->reset();
            Stereo s = sine(1000.0, 1.0, static_cast<float>(linOf(-6.0))); run(*e, s);
            fund[on] = harmonic(s.l, 1000, 24000, 48000);
        }
        std::printf("  ANALOG at -6 dBFS through the wet path (mix 100%%): %+.3f dB\n", dbOf(fund[1] / fund[0]));
        CHECK(std::fabs(dbOf(fund[1] / fund[0])) <= 0.1);
        // ANALOG off: no harmonics at all.
        {
            auto e = make(); base(*e); e->set(param(*e, "threshold"), 15.0f); e->set(param(*e, "analog"), 0.0f); e->reset();
            Stereo s = sine(1000.0, 1.0, 0.5f); run(*e, s);
            CHECK(harmonic(s.l, 2000, 24000, 48000) / harmonic(s.l, 1000, 24000, 48000) < 1e-5);
        }
    }
    {   // 11b. ANALOG noise floor: faint, steady, the same every run (seeded), and none when ANALOG is off or IN is off.
        double on = 0; bool silentOff = true, silentIn = true, same = true, lrDiffer = false;
        Stereo r1(48000), r2(48000);
        {   // the same instance after reset() makes the same noise
            auto e = make(); base(*e); e->set(param(*e, "analog"), 1.0f); e->reset();
            run(*e, r1); e->reset(); run(*e, r2);
        }
        on = rmsDb(r1.l, 4800);
        for (std::size_t i = 0; i < r1.size(); ++i) { if (r1.l[i] != r2.l[i] || r1.r[i] != r2.r[i]) same = false; if (r1.l[i] != r1.r[i]) lrDiffer = true; }
        {
            auto e = make(); base(*e); e->set(param(*e, "analog"), 0.0f); e->reset();
            Stereo s(48000); run(*e, s);
            for (std::size_t i = 0; i < s.size(); ++i) if (s.l[i] != 0.0f || s.r[i] != 0.0f) silentOff = false;
        }
        {
            auto e = make(); base(*e); e->set(param(*e, "analog"), 1.0f); e->set(param(*e, "in"), 0.0f); e->reset();
            Stereo s(48000); run(*e, s);
            for (std::size_t i = 0; i < s.size(); ++i) if (s.l[i] != 0.0f || s.r[i] != 0.0f) silentIn = false;
        }
        std::printf("  ANALOG noise floor: %.1f dBFS RMS on silence; ANALOG off silent: %s; IN off silent: %s; repeatable: %s; L/R differ: %s\n",
                    on, silentOff ? "yes" : "no", silentIn ? "yes" : "no", same ? "yes" : "no", lrDiffer ? "yes" : "no");
        CHECK(on > -100.0 && on < -92.0);
        CHECK(silentOff && silentIn && same && lrDiffer);
    }
    {   // 11c. IN off = bit-exact pass-through, even with ANALOG on, make-up, mix and SC filter set.
        auto e = make(); base(*e); e->set(param(*e, "in"), 0.0f); e->set(param(*e, "analog"), 1.0f); e->set(param(*e, "makeup"), 9.0f);
        e->set(param(*e, "mix"), 60.0f); e->set(param(*e, "scfilter"), 200.0f); e->reset();
        Stereo s = noise(1.0, 0.9f), in = s; run(*e, s, 177);
        const bool exact = std::memcmp(s.l.data(), in.l.data(), s.size() * sizeof(float)) == 0 && std::memcmp(s.r.data(), in.r.data(), s.size() * sizeof(float)) == 0;
        std::printf("  IN off with ANALOG on: bit-exact pass-through %s\n", exact ? "yes" : "NO");
        CHECK(exact);
    }
    {   // 12. Stereo link: loud left, quiet right, same gain on both.
        auto e = make(); base(*e);
        Stereo s(48000);
        for (std::size_t i = 0; i < s.size(); ++i) { const float v = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kFs)); s.l[i] = A * v; s.r[i] = 0.05f * v; }
        Stereo in = s; run(*e, s);
        const double gl = dbOf(rmsOf(s.l, 43200, 48000) / rmsOf(in.l, 43200, 48000)), gr = dbOf(rmsOf(s.r, 43200, 48000) / rmsOf(in.r, 43200, 48000));
        std::printf("  stereo link: left %.2f dB, right %.2f dB\n", gl, gr);
        CHECK(std::fabs(gl - gr) < 0.05);
    }
    {   // 13. Meters: input and output peak, gain reduction.
        auto e = make(); base(*e);
        Stereo s = sine(1000.0, 1.0, A); run(*e, s);
        std::printf("  meters: IN %.2f dBFS, OUT %.2f dBFS, GR %.2f dB\n", e->meter(0), e->meter(1), -e->reductionDb());
        CHECK(std::fabs(e->meter(0) + 6.0f) < 0.15f);
        CHECK(e->meter(1) < e->meter(0) - 8.0f);
        CHECK(std::fabs(-e->reductionDb() - 10.5f) < 1.2f);
        CHECK(e->meters().size() == 2);
    }
    {   // 14. Release knob is shown as unused while AUTO is on.
        auto e = make(); e->set(param(*e, "release"), 4.0f);
        CHECK(!e->paramActive(param(*e, "release")));
        e->set(param(*e, "release"), 2.0f);
        CHECK(e->paramActive(param(*e, "release")));
    }
    {   // 16. SC FILTER switching on starts from clean filter memory (no stale burst = no click / no false squeeze).
        auto e = make(); base(*e); e->set(param(*e, "release"), 0.0f);
        e->set(param(*e, "scfilter"), 100.0f);
        Stereo loud = sine(60.0, 0.2, 1.0f); run(*e, loud, 64);          // filter memory full of a loud low tone
        e->set(param(*e, "scfilter"), 0.0f);
        Stereo quiet = sine(1000.0, 1.0, static_cast<float>(linOf(-60.0))); run(*e, quiet, 64);   // filter off, GR back to 0
        e->set(param(*e, "scfilter"), 100.0f);
        Stereo q2 = sine(1000.0, 0.1, static_cast<float>(linOf(-60.0)));
        const auto gr = runTrace(*e, q2, 16);
        float mx = 0; for (float g : gr) mx = std::max(mx, g);
        std::printf("  SC FILTER switched on during quiet audio: worst gain reduction %.3f dB\n", mx);
        CHECK(mx < 0.01f);
    }
    {   // 17. Zero heap allocations in process(): ANALOG on, every release incl. AUTO, SC filter on, every FADE state.
        auto e = make(); base(*e); e->set(param(*e, "analog"), 1.0f); e->set(param(*e, "scfilter"), 120.0f); e->set(param(*e, "fadetime"), 1.0f); e->reset();
        Stereo s = noise(0.5, 0.8f);
        std::uint64_t allocs = 0; int runs = 0;
        {
            AllocScope scope;
            for (int rel = 0; rel <= 4; ++rel)
                for (int fade = 0; fade <= 2; ++fade) {
                    e->set(param(*e, "release"), static_cast<float>(rel)); e->set(param(*e, "fade"), static_cast<float>(fade));
                    e->set(param(*e, "scfilter"), (rel & 1) ? 0.0f : 120.0f + 10.0f * static_cast<float>(rel));
                    run(*e, s, 256); ++runs;
                }
            allocs = scope.count();
        }
        std::printf("  allocations in process(): %llu across %d runs (5 releases x 3 fade states, ANALOG on, SC filter on/off)\n", static_cast<unsigned long long>(allocs), runs);
        CHECK(allocs == 0);
    }
    {   // 15. Real-time cost, everything on.
        auto e = make(); setAll(*e, kThrM20, 1, 0, 4, 100.0f, 100.0f, 0.0f, 0.0f, true, true); e->reset();
        Stereo s = sine(440.0, 10.0, A);
        const auto t0 = std::chrono::steady_clock::now();
        run(*e, s, 128);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  speed: 10 s of stereo audio in %.0f ms, about %.0fx real time (this build, this machine)\n", ms, 10000.0 / ms);
        CHECK(ms < 10000.0);
    }
}

} // namespace hc::test
