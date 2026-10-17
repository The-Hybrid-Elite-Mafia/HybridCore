// ANALYZER tests: FFT against a plain DFT, waterfall levels and frequencies, BS.1770
// loudness (EBU reference tone, agreement with the export meter), peaks / clipping,
// stereo correlation, plain-words checks, and the audio-thread tap (wait-free, zero
// allocation, nothing lost or invented, fed by MixerProcessor's Main output).
#include "TestHarness.hpp"

#include "hybrid/analysis/Analyzer.hpp"
#include "hybrid/analysis/Fft.hpp"
#include "hybrid/analysis/ScopeTap.hpp"
#include "hybrid/core/Config.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/media/Loudness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace hc::test {

namespace {
using analysis::Analyzer;
using analysis::StereoFrame;
constexpr double kRate = 48000.0;

std::vector<StereoFrame> tone(double seconds, double hz, float ampL, float ampR, double phaseR = 0.0) {
    std::vector<StereoFrame> v(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double t = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate;
        v[i] = {ampL * static_cast<float>(std::sin(t)), ampR * static_cast<float>(std::sin(t + phaseR))};
    }
    return v;
}
int pointNear(double hz) {
    int best = 0;
    for (int i = 0; i < Analyzer::kPoints; ++i)
        if (std::fabs(std::log(Analyzer::pointHz(i) / hz)) < std::fabs(std::log(Analyzer::pointHz(best) / hz))) best = i;
    return best;
}
void feed(Analyzer& a, const std::vector<StereoFrame>& v, std::size_t block = 480) {
    for (std::size_t i = 0; i < v.size(); i += block) a.add(v.data() + i, std::min(block, v.size() - i), static_cast<double>(i) / kRate);
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
} // namespace

void testAnalyzer() {
    std::printf("[analyzer: fft, waterfall, loudness, peaks, stereo, checks]\n");

    {   // FFT against a direct DFT.
        analysis::Fft fft(256);
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        std::vector<std::complex<float>> x(256), y;
        for (auto& v : x) v = {u(rng), u(rng)};
        y = x;
        fft.transform(y.data());
        double worst = 0.0;
        for (std::size_t k = 0; k < 256; ++k) {
            std::complex<double> s{};
            for (std::size_t n = 0; n < 256; ++n) s += std::complex<double>(x[n]) * std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k * n) / 256.0);
            worst = std::max(worst, std::abs(s - std::complex<double>(y[k])));
        }
        std::printf("  FFT 256 vs DFT: largest error %.1e\n", worst);
        CHECK(worst < 1.0e-3);
        bool threw = false;
        try { analysis::Fft bad(1000); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    {   // Waterfall: a full-scale 1 kHz sine reads 0 dB at 1 kHz, far below elsewhere.
        Analyzer a(kRate);
        feed(a, tone(1.0, 1000.0, 1.0f, 1.0f));
        const float* r = a.row(0);
        const float at1k = r[pointNear(1000.0)], at100 = r[pointNear(100.0)], at10k = r[pointNear(10000.0)];
        std::printf("  1 kHz sine at 0 dBFS: %.2f dB at 1 kHz, %.1f dB at 100 Hz, %.1f dB at 10 kHz; %llu rows\n", at1k, at100, at10k,
                    static_cast<unsigned long long>(a.rowsMade()));
        CHECK(std::fabs(at1k) < 1.0f && at100 < -60.0f && at10k < -60.0f);
        CHECK(a.rowsMade() == ((48000 - Analyzer::kFftSize) / Analyzer::kHop + 1) / 2);   // one row per 2 hops once the FFT is full
        // A -20 dB sine at 100 Hz moves to its own point and level.
        a.reset();
        feed(a, tone(1.0, 100.0, 0.1f, 0.1f));
        std::printf("  100 Hz sine at -20 dBFS: %.2f dB at 100 Hz\n", a.row(0)[pointNear(100.0)]);
        CHECK(std::fabs(a.row(0)[pointNear(100.0)] + 20.0f) < 1.0f);
        CHECK(a.row(Analyzer::kRows - 1)[0] <= Analyzer::kFloorDb + 1.0e-3f || a.rowsMade() >= Analyzer::kRows);
        for (int i = 1; i < Analyzer::kPoints; ++i) CHECK(Analyzer::pointHz(i) > Analyzer::pointHz(i - 1));
        CHECK(std::fabs(Analyzer::pointHz(0) - 20.0f) < 0.01f && std::fabs(Analyzer::pointHz(Analyzer::kPoints - 1) - 20000.0f) < 1.0f);
    }

    {   // Loudness: EBU Tech 3341 tone (stereo 997 Hz at -23 dBFS) reads -23.0 LUFS.
        Analyzer a(kRate);
        feed(a, tone(10.0, 997.0, static_cast<float>(std::pow(10.0, -23.0 / 20.0)), static_cast<float>(std::pow(10.0, -23.0 / 20.0))));
        std::printf("  -23 dBFS tone: momentary %.2f, short-term %.2f, integrated %.2f LUFS\n", a.momentaryLufs(), a.shortTermLufs(), a.integratedLufs());
        CHECK(std::fabs(a.momentaryLufs() + 23.0) < 0.1 && std::fabs(a.shortTermLufs() + 23.0) < 0.1 && std::fabs(a.integratedLufs() + 23.0) < 0.1);
        // Same answer as the export's loudness meter on a song-like signal.
        std::mt19937 rng(11);
        std::normal_distribution<float> nd(0.0f, 0.1f);
        std::vector<StereoFrame> mix(static_cast<std::size_t>(12 * kRate));
        std::vector<float> L(mix.size()), R(mix.size());
        for (std::size_t i = 0; i < mix.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            const float env = std::fmod(t, 2.0) < 1.2 ? 1.0f : 0.2f;
            mix[i] = {env * (nd(rng) + 0.2f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 60.0 * t))), env * nd(rng)};
            L[i] = mix[i].l;
            R[i] = mix[i].r;
        }
        a.reset();
        feed(a, mix, 1024);
        const double ours = a.integratedLufs(), theirs = media::integratedLufs(L.data(), R.data(), L.size(), kRate);
        std::printf("  song-like signal: analyzer %.2f LUFS, export meter %.2f LUFS\n", ours, theirs);
        CHECK(std::fabs(ours - theirs) < 0.15);
        a.reset();
        CHECK(a.integratedLufs() <= Analyzer::kSilentLufs && a.shortTermLufs() <= Analyzer::kSilentLufs);
    }

    {   // Peaks and clipping (with the song time of the last clip).
        Analyzer a(kRate);
        auto v = tone(2.0, 440.0, 0.5f, 0.25f);
        feed(a, v);
        CHECK(std::fabs(a.maxPeakDb() + 6.02f) < 0.05f && a.clippedSamples() == 0);
        v[60000].l = 1.0f;
        v[90000].r = -1.2f;
        a.reset();
        feed(a, v);
        std::printf("  clipping: %llu samples, last at %.2f s\n", static_cast<unsigned long long>(a.clippedSamples()), a.lastClipSeconds());
        CHECK(a.clippedSamples() == 2 && a.lastClipSeconds() > 1.8 && a.lastClipSeconds() < 1.9);
        CHECK(a.maxPeakDb() > 1.5f);
    }

    {   // Correlation: same = +1, inverted = -1, unrelated ~ 0.
        Analyzer same(kRate), inv(kRate), unrel(kRate);
        feed(same, tone(4.0, 300.0, 0.5f, 0.5f));
        feed(inv, tone(4.0, 300.0, 0.5f, -0.5f));
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.0f, 0.2f);
        std::vector<StereoFrame> n(static_cast<std::size_t>(4 * kRate));
        for (auto& f : n) f = {nd(rng), nd(rng)};
        feed(unrel, n);
        std::printf("  correlation: same %.3f, inverted %.3f, unrelated %.3f\n", same.correlation(), inv.correlation(), unrel.correlation());
        CHECK(same.correlation() > 0.99f && inv.correlation() < -0.99f && std::fabs(unrel.correlation()) < 0.1f);
        CHECK(same.correlationAvg() > 0.6f && inv.correlationAvg() < -0.6f);
    }

    {   // Plain-words checks.
        Analyzer a(kRate);
        auto c = a.checks();
        CHECK(c.size() == 1 && startsWith(c[0].text, "Press PLAY"));
        feed(a, tone(12.0, 997.0, 0.03f, 0.03f));   // about -30 LUFS
        c = a.checks();
        std::printf("  quiet tone -> \"%s\" / \"%s\"\n", c[0].text.c_str(), c.size() > 1 ? c[1].text.c_str() : "");
        CHECK(c.size() == 5 && c[0].mark == Analyzer::Mark::Warn && startsWith(c[0].text, "Quiet"));
        CHECK(c[1].mark == Analyzer::Mark::Good && startsWith(c[1].text, "Peaks safe"));
        CHECK(c[4].mark == Analyzer::Mark::Note && startsWith(c[4].text, "Almost mono"));
        auto hot = tone(12.0, 997.0, 0.3f, 0.3f);
        hot[100000].l = 1.0f;
        a.reset();
        feed(a, hot);
        c = a.checks();
        std::printf("  hot tone with a clip -> \"%s\" / \"%s\"\n", c[0].text.c_str(), c[1].text.c_str());
        CHECK(c[0].mark == Analyzer::Mark::Good && c[1].mark == Analyzer::Mark::Bad && startsWith(c[1].text, "Clipping: 1 samples"));
        a.reset();
        feed(a, tone(12.0, 300.0, 0.3f, -0.3f));
        CHECK(a.checks().back().mark == Analyzer::Mark::Bad);
    }

    {   // Tap: off = nothing; on = every frame arrives in order or is counted as dropped;
        // push() never allocates; a producer thread and a consumer thread at once.
        auto tap = std::make_unique<analysis::ScopeTap>();
        std::vector<float> l(512, 0.25f), r(512, -0.25f);
        tap->push(l.data(), r.data(), 512);
        StereoFrame f[16];
        CHECK(tap->pop(f, 16) == 0);
        tap->setEnabled(true);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            tap->push(l.data(), r.data(), 512);
            allocs = scope.count();
        }
        CHECK(allocs == 0 && tap->pop(f, 16) == 16 && f[0].l == 0.25f && f[15].r == -0.25f);
        StereoFrame rest[512];
        CHECK(tap->pop(rest, 512) == 496);

        constexpr std::uint64_t kTotal = 8000 * 256;
        std::thread producer([&] {
            std::vector<float> a(256), b(256);
            for (std::uint64_t s = 0; s < kTotal; s += 256) {
                for (std::uint32_t i = 0; i < 256; ++i) { a[i] = static_cast<float>(s + i); b[i] = -a[i]; }
                tap->push(a.data(), b.data(), 256);
            }
        });
        std::uint64_t got = 0, bad = 0;
        float last = -1.0f;
        std::vector<StereoFrame> buf(4096);
        const auto t0 = std::chrono::steady_clock::now();
        while (got + tap->dropped() < kTotal && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            const std::size_t n = tap->pop(buf.data(), buf.size());
            for (std::size_t i = 0; i < n; ++i) {
                bad += !(buf[i].l > last && buf[i].r == -buf[i].l);
                last = buf[i].l;
            }
            got += n;
        }
        producer.join();
        got += tap->pop(buf.data(), buf.size());
        std::printf("  tap, two threads: %llu frames received + %llu dropped of %llu, %llu out of order\n", static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(tap->dropped()), static_cast<unsigned long long>(kTotal), static_cast<unsigned long long>(bad));
        CHECK(got + tap->dropped() == kTotal && bad == 0);
    }

    {   // MixerProcessor feeds the tap with exactly its Main output, without allocating.
        constexpr std::uint32_t F = 256;
        engine::MixerProcessor mp(engine::MixerProcessor::Source::TestTones);
        CHECK(mp.scopeTap() == nullptr);
        mp.prepare(io::StreamConfig{hc::kSampleRate, F, 0, 2, 0.0, 0.0});
        CHECK(mp.scopeTap() != nullptr);
        analysis::ScopeTap* tap = mp.scopeTap();
        mp.prepare(io::StreamConfig{hc::kSampleRate, F, 0, 2, 0.0, 0.0});
        CHECK(mp.scopeTap() == tap);   // never replaced
        tap->setEnabled(true);
        std::vector<float> o0(F), o1(F);
        float* outs[2] = {o0.data(), o1.data()};
        const engine::ProcessContext ctx{nullptr, 0, outs, 2, F, 0};
        std::uint64_t allocs = 0;
        bool same = true;
        StereoFrame got[F];
        for (int b = 0; b < 40; ++b) {
            {
                AllocScope scope;
                mp.process(ctx);
                allocs += scope.count();
            }
            same = same && tap->pop(got, F) == F;
            for (std::uint32_t i = 0; i < F; ++i) same = same && got[i].l == o0[i] && got[i].r == o1[i];
        }
        CHECK(allocs == 0 && same);
    }
}

} // namespace hc::test
