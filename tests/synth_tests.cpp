// HEM WAVE (hc::synth): pitch, release, voices / stealing, unison width, filter, aliasing,
// presets, block-size independence, sample-accurate notes, zero allocation while playing.
#include "TestHarness.hpp"
#include "hybrid/synth/Wave.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

namespace hc::test {

namespace {
using synth::WaveSynth;
constexpr double kFs = 48000.0;

struct Out { std::vector<float> l, r; };

Out play(WaveSynth& s, std::size_t frames, std::uint32_t block = 512) {
    Out o{std::vector<float>(frames), std::vector<float>(frames)};
    for (std::size_t i = 0; i < frames; i += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, frames - i));
        s.render(o.l.data() + i, o.r.data() + i, n);
    }
    return o;
}

// Fundamental from rising zero crossings (linear interpolation) over [from, to).
double pitchHz(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? count * kFs / (last - first) : 0.0;
}

float peakOf(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    float p = 0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// Power at hz (Goertzel, Hann window) over [from, from + n).
double powerAt(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
    const double w = 2 * std::numbers::pi * hz / kFs, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

std::unique_ptr<WaveSynth> make(int preset = 0) {
    auto s = std::make_unique<WaveSynth>();
    s->applyPreset(preset);
    s->prepare(kFs, 512);
    return s;
}
} // namespace

void testSynth() {
    std::printf("[synth] HEM WAVE: pitch, release, voices, unison, filter, aliasing, presets, timing, no allocation\n");

    {   // A4 = 440 Hz (Init = BASIC table at position 0 = a sine)
        auto s = make();
        s->noteOn(69, 1.0f);
        const Out o = play(*s, 48000);
        const double hz = pitchHz(o.l, 24000, 48000);
        std::printf("  A4 pitch %.3f Hz (440 expected), level %.3f\n", hz, peakOf(o.l, 24000));
        CHECK(std::fabs(hz - 440.0) < 4.4);
        CHECK(peakOf(o.l, 24000) > 0.05f);
        s->pitchBend(1.0f);   // + BEND RANGE (2 semitones)
        const Out b = play(*s, 24000);
        const double hb = pitchHz(b.l, 4800, 24000);
        std::printf("  bend +2 st: %.2f Hz (%.2f expected)\n", hb, 440.0 * std::pow(2.0, 2.0 / 12.0));
        CHECK(std::fabs(hb / (440.0 * std::pow(2.0, 2.0 / 12.0)) - 1.0) < 0.01);
        s->pitchBend(0.0f);
        // Note off → release (200 ms to −60 dB) → the voice ends and the output is silent.
        s->noteOff(69);
        const Out r = play(*s, 48000);
        std::printf("  release: peak after 150 ms %.5f, after 0.6 s %.7f, voices %d\n", peakOf(r.l, 7200, 9600), peakOf(r.l, 28800), s->activeVoices());
        CHECK(s->activeVoices() == 0);
        CHECK(peakOf(r.l, 28800) < 1e-6f);
    }
    {   // Sample-accurate note starts, also when the offset is beyond the current block.
        auto s = make();
        s->set(synth::Position, 66.7f);   // a saw: non-zero right after the start
        s->noteOn(60, 1.0f, 100);
        s->noteOn(67, 1.0f, 600);   // 88 into the next block
        std::vector<float> L(512), R(512);
        s->render(L.data(), R.data(), 512);
        std::size_t first = 512;
        for (std::size_t i = 0; i < 512; ++i) if (L[i] != 0.0f) { first = i; break; }
        std::vector<float> L2(512), R2(512), ref(512), refR(512);
        auto t = make();
        t->set(synth::Position, 66.7f);
        t->noteOn(60, 1.0f, 100);
        t->render(ref.data(), refR.data(), 512);
        t->render(ref.data(), refR.data(), 512);   // reference without the second note
        s->render(L2.data(), R2.data(), 512);
        std::size_t second = 512;
        for (std::size_t i = 0; i < 512; ++i) if (std::fabs(L2[i] - ref[i]) > 1e-7f) { second = i; break; }
        std::printf("  note at offset 100 starts at sample %zu; offset 600 lands at %zu of the next block (88 expected)\n", first, second);
        CHECK(first >= 100 && first <= 101);
        CHECK(second >= 88 && second <= 89);
    }
    {   // 16 voices, stealing: no clicks (second difference stays small) and never more than 16.
        auto s = make();
        s->set(synth::AmpRelease, 2000.0f);
        std::vector<float> L, R;
        float worst = 0;
        int maxVoices = 0;
        for (int k = 0; k < 24; ++k) {
            s->noteOn(36 + k, 0.8f);
            const Out o = play(*s, 960);   // 20 ms per note
            L.insert(L.end(), o.l.begin(), o.l.end());
            maxVoices = std::max(maxVoices, s->activeVoices());
            if (k % 3 == 2) s->noteOff(36 + k - 1);
        }
        for (std::size_t i = 1; i + 1 < L.size(); ++i) worst = std::max(worst, std::fabs(L[i + 1] - 2 * L[i] + L[i - 1]));
        std::printf("  24 notes into 16 voices: max voices %d, worst 2nd difference %.4f (click limit 0.02)\n", maxVoices, worst);
        CHECK(maxVoices == WaveSynth::kVoices);
        CHECK(worst < 0.02f);
    }
    {   // MONO / LEGATO: one voice for any number of keys; back to the held key on release.
        auto s = make();
        s->set(synth::Mode, 2.0f);
        s->noteOn(60, 1.0f); s->noteOn(64, 1.0f); s->noteOn(67, 1.0f);
        (void)play(*s, 4800);
        CHECK(s->activeVoices() == 1);
        s->noteOff(67);
        const Out o = play(*s, 24000);
        const double hz = pitchHz(o.l, 4800, 24000);
        std::printf("  legato: 1 voice, after releasing G4 it plays E4 %.2f Hz (329.63 expected)\n", hz);
        CHECK(std::fabs(hz / 329.6276 - 1.0) < 0.01);
    }
    {   // Unison widens the stereo image; one voice is mono.
        auto mono = make(), wide = make();
        wide->set(synth::Unison, 6.0f); wide->set(synth::Detune, 30.0f); wide->set(synth::Spread, 100.0f);
        mono->set(synth::Position, 66.7f); wide->set(synth::Position, 66.7f);
        mono->noteOn(57, 1.0f); wide->noteOn(57, 1.0f);
        const Out a = play(*mono, 48000), b = play(*wide, 48000);
        auto corr = [](const Out& o) {
            double xy = 0, xx = 0, yy = 0;
            for (std::size_t i = 12000; i < o.l.size(); ++i) { xy += o.l[i] * o.r[i]; xx += o.l[i] * o.l[i]; yy += o.r[i] * o.r[i]; }
            return xy / std::sqrt(xx * yy + 1e-30);
        };
        std::printf("  L/R correlation: 1 voice %.3f, 7 unison voices %.3f\n", corr(a), corr(b));
        CHECK(corr(a) > 0.9999);
        CHECK(corr(b) < 0.9);
    }
    {   // Filter: 500 Hz low pass takes the top off a saw (C3).
        auto open = make(), shut = make();
        for (auto* s : {open.get(), shut.get()}) { s->set(synth::Position, 66.7f); s->set(synth::KeyTrack, 0.0f); }
        shut->set(synth::Cutoff, 500.0f);
        open->noteOn(48, 1.0f); shut->noteOn(48, 1.0f);
        const Out a = play(*open, 24000), b = play(*shut, 24000);
        const double f0 = 440.0 * std::pow(2.0, (48 - 69) / 12.0);
        double ha = 0, hb = 0;
        for (int k = 20; k <= 40; ++k) { ha += powerAt(a.l, 8192, 8192, f0 * k); hb += powerAt(b.l, 8192, 8192, f0 * k); }
        const double drop = 10 * std::log10(ha / std::max(hb, 1e-30));
        std::printf("  cutoff 500 Hz: harmonics 20-40 (2.6-5.2 kHz) down %.1f dB\n", drop);
        CHECK(drop > 30.0);
    }
    {   // Aliasing: C8 saw, everything that is not a harmonic stays far down.
        auto s = make();
        s->set(synth::Position, 66.7f);
        s->set(synth::KeyTrack, 0.0f);
        s->noteOn(108, 1.0f);
        const Out o = play(*s, 24000);
        const std::size_t from = 8192, n = 8192;
        const double f0 = 440.0 * std::pow(2.0, (108 - 69) / 12.0);
        double harm = 0, other = 0;
        for (double hz = 50.0; hz < 23950.0; hz += 25.0) {
            double nearest = std::round(hz / f0) * f0;
            const double p = powerAt(o.l, from, n, hz);
            if (std::fabs(hz - nearest) < 40.0 && nearest > 0) harm += p; else other += p;
        }
        const double ratio = 10 * std::log10(other / std::max(harm, 1e-30));
        std::printf("  C8 saw: non-harmonic energy %.1f dB below the harmonics\n", -ratio);
        CHECK(ratio < -50.0);
    }
    {   // Every preset: a 4-note chord at full velocity is finite, audible and below 0 dBFS.
        bool allOk = true;
        for (int p = 0; p < WaveSynth::presetCount(); ++p) {
            auto s = make(p);
            for (int note : {48, 55, 60, 64}) s->noteOn(note, 1.0f);
            const Out o = play(*s, 96000);
            bool finite = true;
            for (std::size_t i = 0; i < o.l.size(); ++i) finite = finite && std::isfinite(o.l[i]) && std::isfinite(o.r[i]);
            const float pk = std::max(peakOf(o.l), peakOf(o.r));
            std::printf("  preset %-14s peak %6.1f dBFS%s\n", WaveSynth::presetName(p), 20 * std::log10(std::max(pk, 1e-9f)), finite ? "" : "  NOT FINITE");
            if (!finite || pk >= 1.0f || pk < 1e-3f) allOk = false;
        }
        CHECK(allOk);
        CHECK(WaveSynth::presetCount() >= 8);
    }
    {   // Block size never changes the sound: 1-sample vs 512-sample blocks (unison, LFO, glide).
        for (int preset : {2, 4}) {
            auto a = make(preset), b = make(preset);
            for (auto* s : {a.get(), b.get()}) {
                s->noteOn(60, 0.9f, 0); s->noteOn(67, 0.7f, 300); s->noteOn(72, 1.0f, 7000);
                s->noteOff(67, 20000); s->noteOff(60, 30000); s->noteOff(72, 40000);
            }
            const Out x = play(*a, 48000, 512), y = play(*b, 48000, 1);
            double d = 0;
            for (std::size_t i = 0; i < x.l.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(x.l[i] - y.l[i])), static_cast<double>(std::fabs(x.r[i] - y.r[i]))});
            std::printf("  %s: 1 vs 512-sample blocks differ by at most %.2e\n", WaveSynth::presetName(preset), d);
            CHECK(d < 1e-4);
        }
    }
    {   // No allocation in render under heavy traffic (all features on).
        auto s = make(6);
        s->set(synth::Warp, 60.0f);
        s->set(synth::Sub, 50.0f);
        s->set(synth::Drive, 12.0f);
        s->set(synth::LfoShape, 4.0f);
        std::vector<float> L(333), R(333);
        std::uint64_t allocs = 0;
        bool finite = true;
        {
            AllocScope scope;
            for (int k = 0; k < 600; ++k) {
                s->noteOn(30 + (k * 7) % 70, 0.5f + 0.5f * static_cast<float>(k % 2), static_cast<std::uint32_t>(k % 333));
                if (k % 2) s->noteOff(30 + ((k - 5) * 7) % 70, static_cast<std::uint32_t>((k * 13) % 333));
                if (k % 50 == 0) { s->set(synth::Warp, (k / 50) % 2 ? -80.0f : 80.0f); s->pitchBend(static_cast<float>(k % 3) - 1.0f); s->modWheel(0.5f); }
                if (k == 300) s->set(synth::Mode, 1.0f);
                s->render(L.data(), R.data(), 333);
                for (float v : L) finite = finite && std::isfinite(v);
            }
            allocs = scope.count();
        }
        std::printf("  600 blocks of heavy note traffic: %llu allocations, finite %s\n", static_cast<unsigned long long>(allocs), finite ? "yes" : "NO");
        CHECK(allocs == 0);
        CHECK(finite);
    }
    {   // Parameters: text ↔ value round trip, defaults, presets touch only known ids.
        WaveSynth s;
        const auto ps = WaveSynth::params();
        CHECK(ps.size() == static_cast<std::size_t>(synth::kParamCount) && ps.size() <= 40);
        bool ok = true;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            for (float nrm : {0.0f, 0.42f, 1.0f}) {
                const float v = synth::toPlain(ps[i], nrm);
                float back = 0;
                if (!synth::parseValue(ps[i], synth::formatValue(ps[i], v), back)) { ok = false; continue; }
                const float tol = ps[i].taper == synth::Taper::Log ? v * 0.02f + 0.6f : (ps[i].max - ps[i].min) * 0.02f + 0.06f;
                if (std::fabs(back - v) > tol) { ok = false; std::printf("  FAIL detail: %s %f -> '%s' -> %f\n", ps[i].id, v, synth::formatValue(ps[i], v).c_str(), back); }
            }
            if (s.get(static_cast<int>(i)) != ps[i].def) ok = false;
            if (s.paramIndex(ps[i].id) != static_cast<int>(i)) ok = false;
        }
        CHECK(ok);
    }
}

} // namespace hc::test
