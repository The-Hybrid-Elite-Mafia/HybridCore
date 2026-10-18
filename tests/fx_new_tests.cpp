// Behaviour tests of the twelve effects added in 0.14 (PUNCH .. SWIRL KILLER).
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <cstdio>

namespace hc::test {
using namespace fxt;

namespace {

double toneAmp(const std::vector<float>& x, double hz, std::size_t from, double fs = 48000.0) {
    double s = 0, c = 0;
    for (std::size_t i = from; i < x.size(); ++i) {
        const double w = 2 * std::numbers::pi * hz * static_cast<double>(i) / fs;
        s += x[i] * std::sin(w);
        c += x[i] * std::cos(w);
    }
    return 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(std::max<std::size_t>(1, x.size() - from));
}
double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }
double highEnergy(const std::vector<float>& x, std::size_t from, double hz) {
    fx::Biquad a, b;
    a.c = b.c = fx::BiquadCoefs::make(fx::BiquadCoefs::Type::HighPass, 48000.0, hz, 0.7071);
    double e = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float y = b.tick(a.tick(x[i]));
        if (i >= from) e += static_cast<double>(y) * y;
    }
    return e;
}
double zeroCrossHz(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? 48000.0 * count / (last - first) : 0.0;
}
void set(fx::Effect& e, const char* id, float v) { e.set(param(e, id), v); }
std::unique_ptr<fx::Effect> make(std::unique_ptr<fx::Effect> (*f)()) {
    auto e = f();
    e->prepare(48000.0, 512);
    return e;
}
// 80 Hz hits with a 60 ms decay, every 250 ms.
Stereo hits(double seconds) {
    Stereo s(static_cast<std::size_t>(seconds * 48000));
    const std::size_t period = 12000;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i % period);
        s.l[i] = s.r[i] = 0.5f * static_cast<float>(std::exp(-t / (0.06 * 48000)) * std::sin(2 * std::numbers::pi * 80 * t / 48000));
    }
    return s;
}
double segRms(const Stereo& s, std::size_t a, std::size_t b) {
    double acc = 0;
    std::size_t n = 0;
    for (std::size_t start = 24000; start + 12000 <= s.size(); start += 12000)
        for (std::size_t i = start + a; i < start + b; ++i) { acc += static_cast<double>(s.l[i]) * s.l[i]; ++n; }
    return std::sqrt(acc / static_cast<double>(std::max<std::size_t>(1, n)));
}
double maxDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t lat, std::size_t from = 0) {
    double d = 0;
    for (std::size_t i = std::max(lat, from); i < a.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i - lat])));
    return d;
}

} // namespace

void testFxNew() {
    // ---------------------------------------------------------------- PUNCH
    std::printf("[fx] HEM PUNCH: defaults transparent, ATTACK+ boosts the hit, SUSTAIN- shortens the tail\n");
    {
        const Stereo dry = hits(2.0);
        auto e = make(&fx::makePunch);
        Stereo x = dry; run(*e, x);
        CHECK(maxDiff(x.l, dry.l, 0) < 1e-5);
        auto a = make(&fx::makePunch);
        set(*a, "attack", 100.0f);
        Stereo y = dry; run(*a, y);
        const double hit = db(segRms(y, 0, 192) / segRms(dry, 0, 192));
        auto b = make(&fx::makePunch);
        set(*b, "sustain", -100.0f);
        Stereo z = dry; run(*b, z);
        const double tail = db(segRms(z, 2880, 9600) / segRms(dry, 2880, 9600));
        std::printf("  attack +100: hit %+.2f dB   sustain -100: tail %+.2f dB\n", hit, tail);
        CHECK(hit > 3.0);
        CHECK(tail < -3.0);
        CHECK(peak(y.l) <= 1.0f);   // clip guard
    }
    // ---------------------------------------------------------------- TRIBAND
    std::printf("[fx] HEM TRIBAND: ratio 1 is flat (crossovers add back up), band gain moves only its band, loud lows compress\n");
    {
        auto e = make(&fx::makeTriBand);
        for (const char* r : {"lowratio", "midratio", "highratio"}) set(*e, r, 1.0f);
        double worst = 0;
        for (double hz : {60.0, 180.0, 1000.0, 3500.0, 10000.0}) worst = std::max(worst, std::fabs(gainAtDb(*e, hz)));
        set(*e, "lowgain", 6.0f);
        const double g40 = gainAtDb(*e, 40.0), g8k = gainAtDb(*e, 8000.0);
        std::printf("  flat within %.2f dB; low gain +6: 40 Hz %+.2f dB, 8 kHz %+.2f dB\n", worst, g40, g8k);
        CHECK(worst < 0.2);
        CHECK(std::fabs(g40 - 6.0) < 0.5 && std::fabs(g8k) < 0.3);
        auto c = make(&fx::makeTriBand);
        set(*c, "lowthr", -30.0f); set(*c, "lowratio", 4.0f); set(*c, "highthr", -30.0f); set(*c, "highratio", 4.0f);
        Stereo s = sine(60.0, 1.0, 0.5f); run(*c, s);
        std::printf("  60 Hz at -6 dBFS: low GR %.1f dB, high GR %.1f dB\n", c->meter(0), c->meter(2));
        CHECK(c->meter(0) < -6.0f && c->meter(2) > -0.5f);
        set(*c, "listen", 3.0f);   // HIGH only: the 60 Hz tone disappears
        Stereo s2 = sine(60.0, 1.0, 0.5f); run(*c, s2);
        CHECK(rmsDb(s2.l, 24000) < -40.0);
    }
    // ---------------------------------------------------------------- WIDE
    std::printf("[fx] HEM WIDE: 0 %% = mono; MONO BELOW removes the side in the lows, keeps it up top; SAFE eases off out-of-phase width\n");
    {
        auto e = make(&fx::makeWide);
        set(*e, "width", 0.0f); set(*e, "monobelow", 20.0f);
        e->reset();
        Stereo x = noise(1.0, 0.3f); run(*e, x);
        double d = 0;
        for (std::size_t i = 4800; i < x.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(x.l[i] - x.r[i])));
        CHECK(d < 1e-5);
        CHECK(e->meter(0) > 0.99f);
        auto q = make(&fx::makeWide);
        set(*q, "monobelow", 150.0f);
        double side40 = 0, side3k = 0;
        for (double hz : {40.0, 3000.0}) {
            q->reset();
            Stereo s(48000);
            for (std::size_t i = 0; i < s.size(); ++i) { s.l[i] = 0.3f * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / 48000)); s.r[i] = -s.l[i]; }
            run(*q, s);
            (hz < 100 ? side40 : side3k) = db(toneAmp(s.l, hz, 24000) / 0.3);
        }
        std::printf("  pure side: 40 Hz %.1f dB, 3 kHz %+.2f dB\n", side40, side3k);
        CHECK(side40 < -20.0 && std::fabs(side3k) < 0.5);
        // SAFE: a pure out-of-phase signal widened to 200 % is held back to 100 %
        auto w = make(&fx::makeWide);
        set(*w, "width", 200.0f); set(*w, "monobelow", 20.0f); set(*w, "safe", 1.0f);
        w->reset();
        Stereo s(48000);
        for (std::size_t i = 0; i < s.size(); ++i) { s.l[i] = 0.2f * static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * static_cast<double>(i) / 48000)); s.r[i] = -s.l[i]; }
        run(*w, s);
        const double g = db(toneAmp(s.l, 1000.0, 24000) / 0.2);
        std::printf("  SAFE, width 200 %% on out-of-phase audio: %+.2f dB (would be +6.02 without)\n", g);
        CHECK(g < 1.0);
    }
    // ---------------------------------------------------------------- HUSH
    std::printf("[fx] HEM HUSH: hiss down 6 dB or more, a held 1 kHz tone within 1 dB\n");
    {
        auto e = make(&fx::makeHush);
        set(*e, "amount", 60.0f); set(*e, "reduction", 18.0f);
        e->reset();
        Stereo x = noise(6.0, 0.01f, 31);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const float t = 0.3f * static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * static_cast<double>(i) / 48000));
            x.l[i] += t; x.r[i] += t;
        }
        const Stereo in = x;
        run(*e, x);
        const std::size_t from = 4 * 48000;
        const double drop = 10 * std::log10(highEnergy(x.l, from, 4000) / highEnergy(in.l, from - e->latency(), 4000));
        const double tone = db(toneAmp(x.l, 1000.0, from) / 0.3);
        std::printf("  hiss above 4 kHz %+.2f dB, 1 kHz tone %+.2f dB, noise floor meter %.1f dBFS\n", drop, tone, e->meter(0));
        CHECK(drop < -6.0);
        CHECK(std::fabs(tone) < 1.0);
    }
    // ---------------------------------------------------------------- DE-ESS
    std::printf("[fx] HEM DE-ESS: a loud 8 kHz 'ess' goes down, 500 Hz at the same level stays\n");
    {
        double g8 = 0, g5 = 0, gw = 0;
        for (double hz : {8000.0, 500.0}) {
            auto e = make(&fx::makeDeEss);
            Stereo s = sine(hz, 1.0, 0.5f); run(*e, s);
            (hz > 1000 ? g8 : g5) = db(toneAmp(s.l, hz, 24000) / 0.5);
        }
        auto w = make(&fx::makeDeEss);
        set(*w, "mode", 1.0f);
        Stereo s = sine(8000.0, 1.0, 0.5f); run(*w, s);
        gw = db(toneAmp(s.l, 8000.0, 24000) / 0.5);
        auto q = make(&fx::makeDeEss);
        Stereo quiet = sine(8000.0, 1.0, 0.005f); run(*q, quiet);
        const double gq = db(toneAmp(quiet.l, 8000.0, 24000) / 0.005);
        std::printf("  split: 8 kHz %+.2f dB, 500 Hz %+.2f dB; wide: 8 kHz %+.2f dB; quiet 8 kHz %+.2f dB\n", g8, g5, gw, gq);
        CHECK(g8 < -6.0 && std::fabs(g5) < 0.3 && gw < -6.0 && std::fabs(gq) < 0.3);
    }
    // ---------------------------------------------------------------- DOUBLE
    std::printf("[fx] HEM DOUBLE: AMOUNT 0 untouched; a mono voice comes out wide (2 and 4 voices)\n");
    {
        auto e = make(&fx::makeDouble);
        set(*e, "amount", 0.0f);
        e->reset();
        Stereo in = noise(1.0, 0.3f);
        in.r = in.l;
        Stereo x = in; run(*e, x);
        CHECK(maxDiff(x.l, in.l, 0) < 1e-6);
        for (float voices : {0.0f, 1.0f}) {
            auto q = make(&fx::makeDouble);
            set(*q, "amount", 100.0f); set(*q, "voices", voices);
            q->reset();
            Stereo y = in; run(*q, y);
            double lr = 0, ll = 0, rr = 0;
            for (std::size_t i = 4800; i < y.size(); ++i) { lr += y.l[i] * y.r[i]; ll += y.l[i] * y.l[i]; rr += y.r[i] * y.r[i]; }
            const double corr = lr / std::sqrt(ll * rr);
            std::printf("  %d voices: L/R correlation %.3f, spread %.1f ms\n", voices > 0.5f ? 4 : 2, corr, q->meter(0));
            CHECK(corr < 0.9);
            CHECK(q->meter(0) > 3.0f);
        }
    }
    // ---------------------------------------------------------------- DESK
    std::printf("[fx] HEM DESK: quiet parts keep their level, loud parts get harmonics; even harmonics only with asymmetric desks\n");
    {
        auto e = make(&fx::makeDesk);
        set(*e, "character", 0.0f); set(*e, "crosstalk", 0.0f); set(*e, "drive", 12.0f);
        const double quiet = gainAtDb(*e, 1000.0, 0.003f);
        e->reset();
        Stereo loud = sine(200.0, 1.0, 0.8f); run(*e, loud);
        const double h3 = db(toneAmp(loud.l, 600.0, 24000) / toneAmp(loud.l, 200.0, 24000));
        const double h2 = db(toneAmp(loud.l, 400.0, 24000) / toneAmp(loud.l, 200.0, 24000));
        set(*e, "character", 3.0f);
        e->reset();
        Stereo tube = sine(200.0, 1.0, 0.8f); run(*e, tube);
        const double h2t = db(toneAmp(tube.l, 400.0, 24000) / toneAmp(tube.l, 200.0, 24000));
        std::printf("  quiet 1 kHz %+.2f dB; clean desk 3rd %.1f dB, 2nd %.1f dB; tube bus 2nd %.1f dB; saturation meter %.1f dB\n",
                    quiet, h3, h2, h2t, e->meter(0));
        CHECK(std::fabs(quiet) < 0.3);
        CHECK(h3 > -40.0 && h2 < -70.0 && h2t > -50.0);
    }
    // ---------------------------------------------------------------- DUST
    std::printf("[fx] HEM DUST: MIX 0 is dry; 4 bits crushes; wow bends the pitch; vinyl follows the music\n");
    {
        auto e = make(&fx::makeDust);
        set(*e, "mix", 0.0f);
        e->reset();
        Stereo in = noise(0.5, 0.3f);
        Stereo x = in; run(*e, x);
        CHECK(maxDiff(x.l, in.l, 0) < 1e-6);
        auto q = make(&fx::makeDust);
        set(*q, "wow", 0.0f); set(*q, "flutter", 0.0f); set(*q, "vinyl", 0.0f); set(*q, "tone", 20000.0f); set(*q, "bits", 4.0f);
        q->reset();
        Stereo s = sine(100.0, 0.1, 0.9f);
        const Stereo clean = s;
        run(*q, s);
        double dev = 0;
        for (std::size_t i = 1000; i < s.size(); ++i) dev = std::max(dev, static_cast<double>(std::fabs(s.l[i] - clean.l[i])));
        auto w = make(&fx::makeDust);
        set(*w, "wow", 100.0f); set(*w, "flutter", 0.0f); set(*w, "vinyl", 0.0f); set(*w, "tone", 20000.0f);
        w->reset();
        Stereo t = sine(1000.0, 2.0, 0.5f); run(*w, t);
        double lo = 1e9, hi = 0;
        for (std::size_t start = 24000; start + 4800 <= t.size(); start += 2400) {
            const double f = zeroCrossHz(t.l, start, start + 4800);
            lo = std::min(lo, f); hi = std::max(hi, f);
        }
        auto v = make(&fx::makeDust);
        set(*v, "vinyl", 100.0f);
        v->reset();
        Stereo music = sine(300.0, 1.0, 0.3f);
        Stereo tail(48000 * 3);
        run(*v, music);
        run(*v, tail);
        const double hissPlaying = rmsDb(music.l, 24000) , hissAfter = rmsDb(tail.l, 48000 * 2);
        std::printf("  4-bit max error %.3f; wow 100 %%: %.1f .. %.1f Hz; vinyl 2-3 s after the music stops %.1f dBFS\n", dev, lo, hi, hissAfter);
        CHECK(dev > 0.04);
        CHECK(hi - lo > 4.0);
        CHECK(hissAfter < -90.0);
        (void)hissPlaying;
    }
    // ---------------------------------------------------------------- TUNE
    std::printf("[fx] HEM TUNE: notes snap to the scale; 448 Hz comes out at 440 Hz; sticky notes; AMOUNT 0 = input, 15 ms late\n");
    {
        CHECK(fx::tuneSnap(69.3f, 0, 0) == 69.0f);
        CHECK(fx::tuneSnap(61.4f, 0, 1) == 62.0f);
        CHECK(fx::tuneSnap(66.2f, 0, 1) == 67.0f);
        CHECK(fx::tuneSnap(68.3f, 9, 2) == 69.0f);
        CHECK(fx::tuneSnap(63.2f, 9, 2) == 64.0f);
        CHECK(fx::tuneSnap(62.8f, 9, 2) == 62.0f);
        auto e = make(&fx::makeTune);
        set(*e, "speed", 0.0f);
        e->reset();
        Stereo x = sine(448.0, 2.0, 0.4f); run(*e, x);
        const double hz = zeroCrossHz(x.l, 48000, x.size());
        std::printf("  in 448 Hz -> out %.2f Hz; meters: pitch %.1f Hz, note %.0f, correction %.1f cents; latency %u\n",
                    hz, e->meter(0), e->meter(1), e->meter(2), e->latency());
        CHECK(std::fabs(hz - 440.0) < 2.0);
        CHECK(std::fabs(e->meter(0) - 448.0f) < 2.0f && static_cast<int>(e->meter(1)) == 69);
        // sticky: settled on A (440), the voice drifts to 55 cents sharp: still pulled to A, not flipped to A#
        {
            auto s2 = make(&fx::makeTune);
            set(*s2, "speed", 0.0f);
            s2->reset();
            Stereo a = sine(440.0, 1.0, 0.4f); run(*s2, a);
            const double hzUp = 440.0 * std::exp2(55.0 / 1200.0);
            Stereo b = sine(hzUp, 1.0, 0.4f); run(*s2, b);
            const int note = static_cast<int>(s2->meter(1));
            std::printf("  sticky: 55 cents sharp after A -> note %d (69 = A)\n", note);
            CHECK(note == 69);
        }
        auto q = make(&fx::makeTune);
        set(*q, "amount", 0.0f);
        q->reset();
        CHECK(q->latency() == 720);
        Stereo in = noise(1.0, 0.3f);
        Stereo y = in; run(*q, y);
        CHECK(maxDiff(y.l, in.l, q->latency()) < 1e-5);
    }
    // ---------------------------------------------------------------- BALANCE
    std::printf("[fx] HEM BALANCE: a mix with a +8 dB bump at 1 kHz gets the 1 kHz band turned down; FREEZE holds\n");
    {
        auto e = make(&fx::makeBalance);
        set(*e, "amount", 100.0f); set(*e, "range", 12.0f); set(*e, "speed", 2.0f); set(*e, "tilt", 3.0f);
        set(*e, "low", 0.0f);
        e->reset();
        Stereo x = noise(8.0, 0.1f, 17);
        fx::Biquad2 bump;
        bump.set(fx::BiquadCoefs::make(fx::BiquadCoefs::Type::Bell, 48000.0, 1000.0, 1.41, 8.0));
        bump.run(x.l.data(), x.r.data(), static_cast<std::uint32_t>(x.size()));
        run(*e, x);
        int mostCut = 0;
        std::printf("  band moves (31 Hz .. 16 kHz):");
        for (int b = 0; b < 10; ++b) {
            std::printf(" %+.1f", e->meter(b));
            if (e->meter(b) < e->meter(mostCut)) mostCut = b;
        }
        std::printf("\n");
        CHECK(mostCut == 5);
        CHECK(e->meter(5) < -3.0f);
        set(*e, "freeze", 1.0f);
        const float before = e->meter(5);
        Stereo silence(48000); run(*e, silence);
        CHECK(std::fabs(e->meter(5) - before) < 1e-4f);
    }
    // ---------------------------------------------------------------- ECHO
    std::printf("[fx] HEM ECHO: a click repeats at the synced time (1/4 at 120 BPM = 500 ms); free time works too\n");
    {
        for (int synced = 1; synced >= 0; --synced) {
            auto e = make(&fx::makeEcho);
            set(*e, "mix", 100.0f); set(*e, "feedback", 0.0f); set(*e, "lowcut", 20.0f); set(*e, "highcut", 20000.0f);
            set(*e, "sync", static_cast<float>(synced)); set(*e, "time", 250.0f);
            e->setTempo(120.0);
            e->reset();
            Stereo s(48000);
            s.l[100] = s.r[100] = 0.5f;
            run(*e, s);
            std::size_t at = 0; float best = 0;
            for (std::size_t i = 0; i < s.size(); ++i) if (std::fabs(s.l[i]) > best) { best = std::fabs(s.l[i]); at = i; }
            const double ms = static_cast<double>(at - 100) / 48.0;
            std::printf("  %s: echo after %.1f ms (meter %.0f ms)\n", synced ? "1/4 synced at 120 BPM" : "free 250 ms", ms, e->meter(0));
            CHECK(std::fabs(ms - (synced ? 500.0 : 250.0)) < 1.0);
        }
    }
    // ---------------------------------------------------------------- SWIRL KILLER
    std::printf("[fx] HEM SWIRL KILLER: AMOUNT 0 = input delayed; a mono centre is never touched; side swirl above FROM is removed\n");
    {
        for (float mode : {0.0f, 1.0f}) {
            auto e = make(&fx::makeSwirlKiller);
            set(*e, "amount", 0.0f); set(*e, "mode", mode);
            e->reset();
            Stereo in = noise(1.0, 0.3f, 11);
            Stereo x = in; run(*e, x);
            CHECK(maxDiff(x.l, in.l, e->latency(), e->latency() + 4096) < 1e-4);
            auto m = make(&fx::makeSwirlKiller);
            set(*m, "amount", 100.0f); set(*m, "protect", 0.0f); set(*m, "mode", mode);
            m->reset();
            Stereo c = noise(1.0, 0.3f, 5);
            c.r = c.l;
            Stereo y = c; run(*m, y);
            CHECK(maxDiff(y.l, c.l, m->latency(), m->latency() + 4096) < 1e-4);
        }
        auto e = make(&fx::makeSwirlKiller);
        set(*e, "amount", 100.0f); set(*e, "from", 4000.0f); set(*e, "protect", 30.0f); set(*e, "mode", 0.0f);
        e->reset();
        const std::size_t n = 48000 * 3;
        Stereo mid = noise(3.0, 0.2f, 21), side = noise(3.0, 0.12f, 99), in(n);
        for (std::size_t i = 0; i < n; ++i) { in.l[i] = mid.l[i] + side.l[i]; in.r[i] = mid.l[i] - side.l[i]; }
        Stereo x = in; run(*e, x);
        std::vector<float> sIn(n), sOut(n), mIn(n), mOut(n);
        for (std::size_t i = 0; i < n; ++i) {
            sIn[i] = 0.5f * (in.l[i] - in.r[i]); sOut[i] = 0.5f * (x.l[i] - x.r[i]);
            mIn[i] = 0.5f * (in.l[i] + in.r[i]); mOut[i] = 0.5f * (x.l[i] + x.r[i]);
        }
        const std::size_t from = 48000;
        const double sideCut = 10 * std::log10(highEnergy(sOut, from, 6000) / highEnergy(sIn, from - e->latency(), 6000));
        const double midKeep = 10 * std::log10(highEnergy(mOut, from, 6000) / highEnergy(mIn, from - e->latency(), 6000));
        std::printf("  side above 6 kHz %+.1f dB, centre above 6 kHz %+.2f dB; meters: swirl %.0f %%, side cut %.1f dB\n",
                    sideCut, midKeep, e->meter(1), e->meter(2));
        CHECK(sideCut < -10.0);
        CHECK(std::fabs(midKeep) < 0.5);
    }
}

} // namespace hc::test
