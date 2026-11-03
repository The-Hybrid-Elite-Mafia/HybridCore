// HEM COMPRESSOR: what each control must actually do to the sound, measured with numbers.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hem/compressor/HemCompressor.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <chrono>
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

    {   // 0. Defaults: ANALOG starts ON (published default), IN starts ON.
        auto e = make();
        CHECK(e->get(param(*e, "analog")) == 1.0f && e->get(param(*e, "in")) == 1.0f);
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
    {   // 10. Fade: 2 s fade, check gain at 1 s and 2 s, then fade back in.
        auto e = make(); base(*e); e->set(param(*e, "in"), 0.0f); e->set(param(*e, "fadetime"), 2.0f); e->set(param(*e, "fade"), 1.0f); e->reset();
        std::vector<float> a(128, 1.0f), b(128, 1.0f);
        float one = 0, two = 0, back = 0;
        for (std::size_t s = 0; s < static_cast<std::size_t>(kFs * 5); s += 128) {
            std::fill(a.begin(), a.end(), 1.0f); std::fill(b.begin(), b.end(), 1.0f);
            if (s == static_cast<std::size_t>(kFs) * 2) e->set(param(*e, "fade"), 0.0f);
            e->process(a.data(), b.data(), 128);
            if (s == 48000) one = a[0];
            if (s == 48000 * 2 + 128) two = a[0];
            if (s == 48000 * 3) back = a[0];
        }
        std::printf("  fade: %.3f at 1 s, %.3f at 2 s, %.3f one second after fading back in\n", one, two, back);
        CHECK(std::fabs(one - 0.5f) < 0.02f); CHECK(two < 0.01f); CHECK(std::fabs(back - 0.5f) < 0.03f);
    }
    {   // 11. ANALOG adds harmonics (no compression here: tone stays under the knee).
        double h3[2], h2on = 0;
        for (int k = 0; k < 2; ++k) {
            auto e = make(); base(*e); e->set(param(*e, "threshold"), 15.0f); e->set(param(*e, "analog"), k ? 1.0f : 0.0f); e->reset();
            Stereo s = sine(1000.0, 1.0, 0.45f); run(*e, s);
            h3[k] = harmonic(s.l, 3000, 24000, 48000) / harmonic(s.l, 1000, 24000, 48000);
            if (k) h2on = harmonic(s.l, 2000, 24000, 48000) / harmonic(s.l, 1000, 24000, 48000);
        }
        std::printf("  ANALOG: 3rd harmonic %.0f dB off, %.1f dB on; 2nd harmonic %.1f dB on\n", dbOf(std::max(h3[0], 1e-9)), dbOf(h3[1]), dbOf(h2on));
        CHECK(h3[1] > 0.005 && h3[0] < 1e-4 && h2on > 0.0005);
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
