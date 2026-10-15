// Phase 3 tests: AVX2 kernels vs scalar reference, 13-stem routing matrix behaviour,
// MixerProcessor sources/outputs, zero-allocation audio path, measured CPU cost.
#include "TestHarness.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/dsp/Kernels.hpp"
#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/engine/Engine.hpp"
#include "hybrid/engine/MixerProcessor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace hc::test {

namespace {

bool near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

constexpr std::uint32_t kN      = 256;
constexpr std::uint32_t kStems  = dsp::StemMatrix::kStems;
constexpr std::uint32_t kBuses  = dsp::StemMatrix::kBuses;

// Holds per-stem constant-value sources.
struct StemBank {
    std::vector<std::vector<float>>       l, r;
    std::array<const float*, kStems>      pl{}, pr{};
    StemBank() : l(kStems, std::vector<float>(kN, 0.f)), r(kStems, std::vector<float>(kN, 0.f)) {
        for (std::uint32_t s = 0; s < kStems; ++s) { pl[s] = nullptr; pr[s] = nullptr; }
    }
    void set(std::uint32_t s, float vl, float vr) {
        std::fill(l[s].begin(), l[s].end(), vl);
        std::fill(r[s].begin(), r[s].end(), vr);
        pl[s] = l[s].data();
        pr[s] = r[s].data();
    }
};

// Runs two blocks: first ramps to the new settings, second is steady state.
void settle(dsp::StemMatrix& m, const StemBank& b) {
    m.process(b.pl.data(), b.pr.data(), kN);
    m.process(b.pl.data(), b.pr.data(), kN);
}

float at(const dsp::StemMatrix& m, std::uint32_t bus, std::uint32_t ch, std::uint32_t i = kN - 1) {
    return m.busChannel(bus, ch)[i];
}

} // namespace

void testKernels() {
    std::printf("[kernels]\n");
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    bool allOk = true;

    for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 31u, 128u, 1001u}) {
        std::vector<float> src(n), dst(n), ref(n);
        for (auto& v : src) v = dist(rng);
        for (auto& v : dst) v = dist(rng);

        auto cmp = [&](const char* name) {
            for (std::size_t i = 0; i < n; ++i)
                if (!near(dst[i], ref[i], 1e-6f)) {
                    std::printf("  mismatch %s n=%zu i=%zu got %.9g want %.9g\n", name, n, i, dst[i], ref[i]);
                    allOk = false;
                    return;
                }
        };
        const std::vector<float> base = dst;

        dst = base; ref = base;
        dsp::scale(dst.data(), 0.37f, n);
        for (std::size_t i = 0; i < n; ++i) ref[i] *= 0.37f;
        cmp("scale");

        dst = base; ref = base;
        dsp::scaleRamp(dst.data(), 0.2f, 0.9f, n);
        for (std::size_t i = 0; i < n; ++i) ref[i] *= 0.2f + (0.9f - 0.2f) / static_cast<float>(n) * static_cast<float>(i);
        cmp("scaleRamp");

        dst = base; ref = base;
        dsp::mixAdd(dst.data(), src.data(), -0.61f, n);
        for (std::size_t i = 0; i < n; ++i) ref[i] += src[i] * -0.61f;
        cmp("mixAdd");

        dst = base; ref = base;
        dsp::mixAddRamp(dst.data(), src.data(), 1.0f, 0.0f, n);
        for (std::size_t i = 0; i < n; ++i) ref[i] += src[i] * (1.0f - 1.0f / static_cast<float>(n) * static_cast<float>(i));
        cmp("mixAddRamp");

        dst = base;
        dsp::clear(dst.data(), n);
        CHECK(std::all_of(dst.begin(), dst.end(), [](float v) { return v == 0.f; }));

        float  pk = 0.f;
        double ss = 0.0;
        for (float v : src) { pk = std::max(pk, std::fabs(v)); ss += static_cast<double>(v) * v; }
        CHECK(dsp::peakAbs(src.data(), n) == pk);
        CHECK(std::fabs(dsp::sumSquares(src.data(), n) - ss) <= 1e-9 * std::max(1.0, ss));
    }
    CHECK(allOk);
    std::printf("  scale / ramp / mixAdd / mixAddRamp / peak / sumSquares match scalar reference: %s\n",
                allOk ? "yes" : "NO");
}

void testMatrix() {
    std::printf("[matrix] %u stems x %u buses\n", kStems, kBuses);
    CHECK(kStems == 13);
    CHECK(dsp::StemMatrix::dbToGain(dsp::StemMatrix::kOffDb) == 0.f);
    CHECK(dsp::StemMatrix::dbToGain(NAN) == 0.f);
    CHECK(near(dsp::StemMatrix::dbToGain(-6.f), 0.5011872f));

    float gl = 0, gr = 0;
    dsp::StemMatrix::balanceGains(0.f, gl, gr);  CHECK(near(gl, 1.f) && near(gr, 1.f));
    dsp::StemMatrix::balanceGains(-1.f, gl, gr); CHECK(gl == 1.f && gr == 0.f);
    dsp::StemMatrix::balanceGains(1.f, gl, gr);  CHECK(gl == 0.f && gr == 1.f);
    dsp::StemMatrix::balanceGains(0.5f, gl, gr); CHECK(near(gl, 0.5411961f, 1e-4f) && gr == 1.f);

    auto m = std::make_unique<dsp::StemMatrix>();
    m->prepare(kN);
    StemBank b;
    b.set(0, 1.0f, 0.5f);

    // First block ramps 0 -> 1 (no click), second is steady at unity on Main only.
    m->process(b.pl.data(), b.pr.data(), kN);
    CHECK(at(*m, 0, 0, 0) == 0.f);
    CHECK(near(at(*m, 0, 0, kN / 2), 0.5f));
    m->process(b.pl.data(), b.pr.data(), kN);
    CHECK(near(at(*m, 0, 0), 1.f) && near(at(*m, 0, 1), 0.5f));
    for (std::uint32_t bus = 1; bus < kBuses; ++bus) CHECK(at(*m, bus, 0) == 0.f && at(*m, bus, 1) == 0.f);

    // Meters: stem post-trim, bus output.
    CHECK(near(m->stemPeak(0), 1.f));
    CHECK(near(m->stemRms(0), std::sqrt((1.f + 0.25f) / 2.f)));
    CHECK(near(m->busPeak(0, 0), 1.f) && near(m->busPeak(0, 1), 0.5f));
    CHECK(m->stemPeak(5) == 0.f);

    m->setStemTrimDb(0, 6.f);       settle(*m, b); CHECK(near(at(*m, 0, 0), 1.9952623f));
    m->setStemTrimDb(0, 0.f);
    m->setStemBalance(0, -1.f);     settle(*m, b); CHECK(near(at(*m, 0, 0), 1.f) && at(*m, 0, 1) == 0.f);
    m->setStemBalance(0, 0.f);
    m->setStemInvert(0, true);      settle(*m, b); CHECK(near(at(*m, 0, 0), -1.f));
    m->setStemInvert(0, false);
    m->setStemMute(0, true);        settle(*m, b); CHECK(at(*m, 0, 0) == 0.f);
    m->setStemMute(0, false);

    // Solo-in-place: soloing stem 1 silences stem 0.
    b.set(1, 0.25f, 0.25f);
    settle(*m, b);                  CHECK(near(at(*m, 0, 0), 1.25f));
    m->setStemSolo(1, true);        settle(*m, b); CHECK(near(at(*m, 0, 0), 0.25f));
    m->setStemSolo(1, false);       settle(*m, b); CHECK(near(at(*m, 0, 0), 1.25f));

    // Sends: stem 1 to bus 2 at -6 dB, Main unaffected.
    m->setSendDb(1, 2, -6.f);       settle(*m, b);
    CHECK(near(at(*m, 2, 0), 0.25f * 0.5011872f));
    CHECK(near(at(*m, 0, 0), 1.25f));
    m->setSendDb(1, 0, dsp::StemMatrix::kOffDb);
    settle(*m, b);                  CHECK(near(at(*m, 0, 0), 1.0f));

    // Bus gain and bus mute.
    m->setBusGainDb(0, -6.f);       settle(*m, b); CHECK(near(at(*m, 0, 0), 0.5011872f));
    m->setBusMute(0, true);         settle(*m, b); CHECK(at(*m, 0, 0) == 0.f);
    m->setBusMute(0, false);
    m->setBusGainDb(0, 0.f);

    // Mono stem (R == nullptr) duplicates L; all 13 stems summing.
    auto m2 = std::make_unique<dsp::StemMatrix>();
    m2->prepare(kN);
    StemBank all;
    for (std::uint32_t s = 0; s < kStems; ++s) { all.set(s, 0.01f, 0.f); all.pr[s] = nullptr; }
    settle(*m2, all);
    CHECK(near(at(*m2, 0, 0), 0.13f) && near(at(*m2, 0, 1), 0.13f));

    // Gain change ramps: block after a change is monotonic (no step).
    m2->setBusGainDb(0, -20.f);
    m2->process(all.pl.data(), all.pr.data(), kN);
    bool monotonic = true;
    const float* ch = m2->busChannel(0, 0);
    for (std::uint32_t i = 1; i < kN; ++i) if (ch[i] > ch[i - 1] + 1e-7f) monotonic = false;
    CHECK(monotonic);
    CHECK(near(ch[0], 0.13f));
}

void testMixer() {
    std::printf("[mixer processor]\n");
    constexpr std::uint32_t F = 256;
    std::vector<std::vector<float>> in(4, std::vector<float>(F)), out(8, std::vector<float>(F));
    std::array<const float*, 4> ip{};
    std::array<float*, 8>       op{};
    for (std::uint32_t i = 0; i < 4; ++i) { std::fill(in[i].begin(), in[i].end(), 0.1f * (i + 1)); ip[i] = in[i].data(); }
    for (std::uint32_t i = 0; i < 8; ++i) op[i] = out[i].data();

    engine::MixerProcessor mix(engine::MixerProcessor::Source::Hardware);
    mix.prepare(io::StreamConfig{hc::kSampleRate, F, 4, 8, 0.0, 0.0});
    const engine::ProcessContext ctx{ip.data(), 4, op.data(), 8, F, 0};
    mix.process(ctx);
    mix.process(ctx);
    // stem0 = in0/in1 (0.1/0.2), stem1 = in2/in3 (0.3/0.4): Main = 0.4 / 0.6
    CHECK(near(out[0][F - 1], 0.4f) && near(out[1][F - 1], 0.6f));
    CHECK(out[2][F - 1] == 0.f);   // bus 1 has no sends by default

    mix.matrix().setSendDb(0, 1, 0.f);
    mix.process(ctx);
    mix.process(ctx);
    CHECK(near(out[2][F - 1], 0.1f) && near(out[3][F - 1], 0.2f));

    engine::MixerProcessor tones(engine::MixerProcessor::Source::TestTones);
    tones.prepare(io::StreamConfig{hc::kSampleRate, F, 0, 2, 0.0, 0.0});
    const engine::ProcessContext tctx{nullptr, 0, op.data(), 2, F, 0};
    for (int k = 0; k < 20; ++k) tones.process(tctx);
    float pk = 0.f;
    for (float v : out[0]) pk = std::max(pk, std::fabs(v));
    CHECK(pk > 0.01f && pk < 13.f * 0.0316228f + 1e-4f);
    CHECK(near(tones.matrix().stemPeak(12), 0.0316228f, 2e-2f));
    CHECK(std::fabs(engine::MixerProcessor::testToneHz(4) - 220.0) < 1e-9);
    std::printf("  13 test tones summed on Main, peak %.4f (limit %.4f)\n", pk, 13.f * 0.0316228f);
}

void testZeroAllocation() {
    std::printf("[zero allocation on the audio path]\n");
    constexpr std::uint32_t F = 128;
    engine::Engine e;
    e.prepare(io::StreamConfig{hc::kSampleRate, F, 26, 8, 0.0, 0.0});

    auto owned = std::make_unique<engine::MixerProcessor>(engine::MixerProcessor::Source::TestTones);
    engine::MixerProcessor* mix = owned.get();
    std::unique_ptr<engine::AudioProcessor> p = std::move(owned);
    CHECK(e.install(p));

    std::vector<std::vector<float>> in(26, std::vector<float>(F, 0.05f)), out(8, std::vector<float>(F));
    std::vector<const float*> ip;
    std::vector<float*>       op;
    for (auto& v : in) ip.push_back(v.data());
    for (auto& v : out) op.push_back(v.data());

    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int k = 0; k < 4000; ++k) {
            if (k % 50 == 0) {   // parameter traffic during processing
                mix->matrix().setStemBalance(static_cast<std::uint32_t>(k / 50) % kStems, (k % 100) ? -0.5f : 0.5f);
                mix->matrix().setSendDb(3, static_cast<std::uint32_t>(k / 50) % kBuses, -12.f);
                mix->matrix().setStemSolo(5, (k / 50) % 2 == 0);
                (void)e.setMasterGain((k / 50) % 2 ? 0.8f : 1.0f);
            }
            if (k == 2000) mix->setSource(engine::MixerProcessor::Source::Hardware);
            e.process(ip.data(), 26, op.data(), 8, F);
        }
        allocs = scope.count();
    }
    CHECK(allocs == 0);
    std::printf("  4,000 blocks (engine + 13-stem mixer, live parameter changes): %llu heap allocations\n",
                static_cast<unsigned long long>(allocs));

    // Sanity: the tracker really counts (guards against a silently broken test).
    std::uint64_t probe = 0;
    {
        AllocScope scope;
        auto v = std::make_unique<int>(7);
        probe = scope.count();
    }
    CHECK(probe >= 1);
}

void benchmark() {
    std::printf("[benchmark] (machine-dependent; CI runner is NOT the rig)\n");
    using clock = std::chrono::steady_clock;

    for (std::uint32_t frames : {64u, 128u, 256u}) {
        auto m = std::make_unique<dsp::StemMatrix>();
        m->prepare(frames);
        // Worst case: every stem stereo, sending to every bus, gains changing every block (all ramps).
        std::vector<std::vector<float>> l(kStems, std::vector<float>(frames)), r(kStems, std::vector<float>(frames));
        std::array<const float*, kStems> pl{}, pr{};
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> d(-0.1f, 0.1f);
        for (std::uint32_t s = 0; s < kStems; ++s) {
            for (auto& v : l[s]) v = d(rng);
            for (auto& v : r[s]) v = d(rng);
            pl[s] = l[s].data();
            pr[s] = r[s].data();
            for (std::uint32_t b = 0; b < kBuses; ++b) m->setSendDb(s, b, -6.f);
        }

        constexpr int kIters = 20000;
        const auto t0 = clock::now();
        for (int k = 0; k < kIters; ++k) {
            m->setStemTrimDb(static_cast<std::uint32_t>(k) % kStems, (k & 1) ? -1.f : 0.f);
            m->process(pl.data(), pr.data(), frames);
        }
        const double us     = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kIters;
        const double budget = 1e6 * frames / hc::kSampleRate;
        std::printf("  13x4 matrix, %3u frames (%.3f ms block): %.3f us per block = %.3f%% of one core's budget\n",
                    frames, budget / 1000.0, us, 100.0 * us / budget);
        CHECK(us < budget);
    }
}

} // namespace hc::test
