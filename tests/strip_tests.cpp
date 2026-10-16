// Phase 8 tests: per-stem EQ + compressor (dsp::ChannelStrip), the main-bus look-ahead
// limiter (dsp::BusLimiter), their place in the matrix / mixer processor, the new
// parameters, zero allocation on the audio path, and the measured cost of the full chain.
#include "TestHarness.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/dsp/BusLimiter.hpp"
#include "hybrid/dsp/ChannelStrip.hpp"
#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/params/MixerParams.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

namespace hc::test {

namespace {
constexpr double kFs = 48000.0;
constexpr std::uint32_t kBlock = 480;

double db(double lin) { return 20.0 * std::log10(std::max(lin, 1e-30)); }

// Runs a sine through a strip long enough for every glide to finish, returns gain in dB
// (output RMS / input RMS over the last half second).
double stripGainDb(dsp::ChannelStrip& cs, double hz, double amp = 0.25) {
    std::vector<float> l(kBlock), r(kBlock);
    double ph = 0.0, inSq = 0.0, outSq = 0.0;
    const double w = 2.0 * std::numbers::pi * hz / kFs;
    const int blocks = 200;   // 2 s
    for (int b = 0; b < blocks; ++b) {
        for (std::uint32_t i = 0; i < kBlock; ++i) { l[i] = r[i] = static_cast<float>(amp * std::sin(ph)); ph += w; }
        double in = 0.0;
        if (b >= blocks / 2) for (float v : l) in += static_cast<double>(v) * v;
        cs.process(l.data(), r.data(), kBlock);
        if (b >= blocks / 2) { inSq += in; for (float v : l) outSq += static_cast<double>(v) * v; }
    }
    return 10.0 * std::log10(outSq / inSq);
}
} // namespace

void testChannelStrip() {
    std::printf("[channel strip: EQ + compressor]\n");
    {   // Neutral strip: not engaged, and processing leaves the signal bit-exact.
        dsp::ChannelStrip cs;
        cs.prepare(kFs);
        CHECK(!cs.engaged());
        std::vector<float> l(kBlock), r(kBlock), l0(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = l0[i] = std::sin(0.01f * static_cast<float>(i)) * 0.5f;
        cs.process(l.data(), r.data(), kBlock);
        CHECK(std::equal(l.begin(), l.end(), l0.begin()) && cs.gainReductionDb() == 0.0f);
    }
    {   // EQ responses against the designed curves.
        auto measure = [](auto setup, double hz) {
            dsp::ChannelStrip cs;
            cs.prepare(kFs);
            setup(cs);
            return stripGainDb(cs, hz);
        };
        const double low30  = measure([](auto& c) { c.setLowDb(6.0f); }, 30.0);
        const double low5k  = measure([](auto& c) { c.setLowDb(6.0f); }, 5000.0);
        const double hi18k  = measure([](auto& c) { c.setHighDb(-6.0f); }, 18000.0);
        const double hi200  = measure([](auto& c) { c.setHighDb(-6.0f); }, 200.0);
        const double mid1k  = measure([](auto& c) { c.setMidDb(9.0f); c.setMidHz(1000.0f); }, 1000.0);
        const double mid8k  = measure([](auto& c) { c.setMidDb(9.0f); c.setMidHz(1000.0f); }, 8000.0);
        const double hpf100 = measure([](auto& c) { c.setHpfHz(100.0f); }, 100.0);
        const double hpf25  = measure([](auto& c) { c.setHpfHz(100.0f); }, 25.0);
        const double hpf1k  = measure([](auto& c) { c.setHpfHz(100.0f); }, 1000.0);
        std::printf("  low shelf +6 dB: 30 Hz %+.2f dB, 5 kHz %+.2f dB | high shelf -6 dB: 18 kHz %+.2f dB, 200 Hz %+.2f dB\n",
                    low30, low5k, hi18k, hi200);
        std::printf("  mid +9 dB @ 1 kHz: 1 kHz %+.2f dB, 8 kHz %+.2f dB | low cut 100 Hz: 100 Hz %+.2f dB, 25 Hz %+.2f dB, 1 kHz %+.2f dB\n",
                    mid1k, mid8k, hpf100, hpf25, hpf1k);
        CHECK(std::fabs(low30 - 6.0) < 0.6 && std::fabs(low5k) < 0.1);
        CHECK(std::fabs(hi18k + 6.0) < 0.8 && std::fabs(hi200) < 0.1);
        CHECK(std::fabs(mid1k - 9.0) < 0.05 && mid8k < 1.5 && mid8k > 0.0);
        CHECK(std::fabs(hpf100 + 3.01) < 0.1 && hpf25 < -22.0 && std::fabs(hpf1k) < 0.1);
    }
    {   // Static gain curve (6 dB soft knee).
        using C = dsp::ChannelStrip;
        CHECK(C::gainComputerDb(-30.0f, -20.0f, 4.0f) == 0.0f);
        CHECK(std::fabs(C::gainComputerDb(0.0f, -20.0f, 4.0f) + 15.0f) < 1e-4f);
        CHECK(std::fabs(C::gainComputerDb(-20.0f, -20.0f, 4.0f) + 0.5625f) < 1e-4f);   // knee centre: (1/4-1)*3^2/12
        CHECK(C::gainComputerDb(-5.0f, -20.0f, 1.0f) == 0.0f);
    }
    {   // Compressor on a 1 kHz sine at 0 dBFS peak: threshold -20, 4:1 -> 15 dB less; quiet input untouched.
        dsp::ChannelStrip cs;
        cs.prepare(kFs);
        cs.setThresholdDb(-20.0f);
        cs.setRatio(4.0f);
        cs.setAttackMs(5.0f);
        cs.setReleaseMs(200.0f);
        const double g = stripGainDb(cs, 1000.0, 1.0);
        std::printf("  compressor -20 dB 4:1, 0 dBFS sine: %.2f dB (expected -15), meter %.2f dB\n", g, cs.gainReductionDb());
        CHECK(std::fabs(g + 15.0) < 0.6 && std::fabs(cs.gainReductionDb() - 15.0f) < 0.8f);
        dsp::ChannelStrip q;   // -40 dBFS through the same settings: below the knee -> unity
        q.prepare(kFs);
        q.setThresholdDb(-20.0f);
        q.setRatio(4.0f);
        CHECK(std::fabs(stripGainDb(q, 1000.0, 0.01)) < 1e-4);
    }
    {   // Attack: a step from silence to 0 dBFS reaches ~63 % of the final reduction after one attack time.
        dsp::ChannelStrip cs;
        cs.prepare(kFs);
        cs.setThresholdDb(-20.0f);
        cs.setRatio(20.0f);
        cs.setAttackMs(10.0f);
        std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
        for (int b = 0; b < 10; ++b) cs.process(l.data(), r.data(), kBlock);   // glide to the settings
        std::vector<float> step(480, 1.0f), stepR(480, 1.0f);                  // 10 ms of DC at 0 dBFS
        cs.process(step.data(), stepR.data(), 480);
        const double finalGr = -dsp::ChannelStrip::gainComputerDb(0.0f, -20.0f, 20.0f);
        const double grAt = -db(step[479]);
        std::printf("  attack 10 ms: %.1f dB of %.1f dB reached after 10 ms (%.0f %%)\n", grAt, finalGr, 100.0 * grAt / finalGr);
        CHECK(grAt / finalGr > 0.55 && grAt / finalGr < 0.70);
    }
    {   // Audio path: no allocation with every setting moving.
        dsp::ChannelStrip cs;
        cs.prepare(kFs);
        std::vector<float> l(kBlock), r(kBlock);
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> d(-0.8f, 0.8f);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (int b = 0; b < 2000; ++b) {
                for (std::uint32_t i = 0; i < kBlock; ++i) { l[i] = d(rng); r[i] = d(rng); }
                if (b % 20 == 0) {
                    cs.setLowDb(static_cast<float>(b % 30) - 15.0f);
                    cs.setMidHz(150.0f + static_cast<float>(b % 7000));
                    cs.setHpfHz(20.0f + static_cast<float>(b % 400));
                    cs.setRatio(1.0f + static_cast<float>(b % 19));
                    cs.setThresholdDb(-static_cast<float>(b % 48));
                }
                cs.process(l.data(), r.data(), kBlock);
            }
            allocs = scope.count();
        }
        bool finite = true;
        for (float v : l) finite &= std::isfinite(v);
        CHECK(allocs == 0 && finite);
        std::printf("  2,000 blocks with settings changing: %llu heap allocations, output finite: %s\n",
                    static_cast<unsigned long long>(allocs), finite ? "yes" : "NO");
    }
}

void testLimiter() {
    std::printf("[main limiter]\n");
    constexpr std::uint32_t F = 256;
    dsp::BusLimiter lim;
    lim.prepare(kFs, F);
    const std::uint32_t L = lim.latency();
    CHECK(L == 48);

    // Off: every channel is the input delayed by exactly L samples.
    std::vector<std::vector<float>> in(8, std::vector<float>(F));
    std::array<const float*, 8> ip{};
    for (std::uint32_t c = 0; c < 8; ++c) ip[c] = in[c].data();
    std::vector<std::vector<float>> got(8);
    std::uint64_t n = 0;
    for (int b = 0; b < 8; ++b) {
        for (std::uint32_t i = 0; i < F; ++i, ++n)
            for (std::uint32_t c = 0; c < 8; ++c) in[c][i] = 1.5f * std::sin(0.013f * static_cast<float>(n) * static_cast<float>(c + 1));
        lim.process(ip.data(), F);
        for (std::uint32_t c = 0; c < 8; ++c) got[c].insert(got[c].end(), lim.channel(c / 2, c % 2), lim.channel(c / 2, c % 2) + F);
    }
    int bad = 0;
    for (std::uint32_t c = 0; c < 8; ++c)
        for (std::size_t i = L; i < got[c].size(); ++i)
            if (got[c][i] != 1.5f * std::sin(0.013f * static_cast<float>(i - L) * static_cast<float>(c + 1))) ++bad;
    CHECK(bad == 0 && lim.gainReductionDb() == 0.0f);

    // On, ceiling -1 dB: loud sines, random bursts and single-sample spikes never pass the ceiling.
    lim.setEnabled(true);
    lim.setCeilingDb(-1.0f);
    const float ceil = std::pow(10.0f, -1.0f / 20.0f);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    float peak = 0.0f, grMax = 0.0f;
    double maxStep = 0.0;
    float prevGain = 1.0f;
    std::vector<float> hist;   // input history, to read the gain the limiter applied
    for (int b = 0; b < 400; ++b) {
        for (std::uint32_t i = 0; i < F; ++i) {
            float x = 2.0f * std::sin(0.02f * static_cast<float>(b * F + i));
            if (b % 37 == 5) x += 3.0f * d(rng);
            if (b % 53 == 9 && i == 100) x = 6.0f;
            in[0][i] = x;
            in[1][i] = -x * 0.7f;
            hist.push_back(x);
        }
        lim.process(ip.data(), F);
        grMax = std::max(grMax, lim.gainReductionDb());
        const float* oL = lim.channel(0, 0);
        const float* oR = lim.channel(0, 1);
        for (std::uint32_t i = 0; i < F; ++i) {
            peak = std::max({peak, std::fabs(oL[i]), std::fabs(oR[i])});
            const std::size_t at = static_cast<std::size_t>(b) * F + i;
            if (at >= L && std::fabs(hist[at - L]) > 0.05f) {
                const float g = oL[i] / hist[at - L];
                maxStep = std::max(maxStep, static_cast<double>(std::fabs(g - prevGain)));
                prevGain = g;
            }
        }
    }
    std::printf("  +15 dBFS bursts and spikes, ceiling -1.0 dB: output peak %.4f (ceiling %.4f), deepest reduction %.1f dB,"
                " largest gain step per sample %.4f\n", peak, ceil, grMax, maxStep);
    CHECK(peak <= ceil * 1.000001f && grMax > 10.0f && maxStep < 0.2);

    // Below the ceiling after release: gain back at exactly 1.
    for (int b = 0; b < 400; ++b) {
        for (std::uint32_t i = 0; i < F; ++i) in[0][i] = in[1][i] = 0.5f;
        lim.process(ip.data(), F);
    }
    CHECK(lim.channel(0, 0)[F - 1] == 0.5f && lim.gainReductionDb() == 0.0f);

    // Zero allocation while switching on/off and moving the ceiling.
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int b = 0; b < 3000; ++b) {
            if (b % 100 == 0) { lim.setEnabled((b / 100) % 2 == 0); lim.setCeilingDb(-static_cast<float>(b % 7)); }
            for (std::uint32_t i = 0; i < F; ++i) in[0][i] = in[1][i] = 3.0f * d(rng);
            lim.process(ip.data(), F);
        }
        allocs = scope.count();
    }
    CHECK(allocs == 0);
    std::printf("  latency %u samples (%.2f ms) on every bus; 3,000 blocks switching on/off: %llu heap allocations\n", L,
                1000.0 * L / kFs, static_cast<unsigned long long>(allocs));
}

void testStripInMatrix() {
    std::printf("[strips in the matrix + mixer]\n");
    constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;
    constexpr std::uint32_t F = 256;
    auto m = std::make_unique<dsp::StemMatrix>();
    m->prepare(F, kFs);
    std::vector<float> dc(F, 0.1f);
    std::array<const float*, kStems> pl{}, pr{};
    pl[0] = pr[0] = dc.data();
    m->setStemTrimDb(0, 6.0f);
    const float want = 0.1f * dsp::StemMatrix::dbToGain(6.0f);
    // A strip that is engaged but changes nothing at this level (compressor far above the signal):
    // switching it in and out mid-stream must not move the output.
    float worst = 0.0f;
    for (int b = 0; b < 60; ++b) {
        if (b == 20) { m->strip(0).setThresholdDb(0.0f); m->strip(0).setRatio(2.0f); }
        if (b == 40) m->strip(0).setRatio(1.0f);
        m->process(pl.data(), pr.data(), F);
        if (b >= 2)
            for (std::uint32_t i = 0; i < F; ++i) worst = std::max(worst, std::fabs(m->busChannel(0, 0)[i] - want));
    }
    std::printf("  strip switched in and out on a +6 dB-trimmed stem: worst deviation %.2e\n", static_cast<double>(worst));
    CHECK(worst < 2e-5f);

    // Compressor gain reduction reaches the per-stem meter.
    m->strip(0).setThresholdDb(-40.0f);
    m->strip(0).setRatio(10.0f);
    for (int b = 0; b < 100; ++b) m->process(pl.data(), pr.data(), F);
    CHECK(m->strip(0).gainReductionDb() > 10.0f && m->stemPeak(0) < want);

    // Full chain through MixerProcessor: every strip with every band and the compressor on,
    // limiter on: no allocation, output under the ceiling.
    engine::MixerProcessor mp(engine::MixerProcessor::Source::TestTones);
    io::StreamConfig cfg;
    cfg.sampleRate = 48000; cfg.blockFrames = kBlock; cfg.numOutputs = 8;
    mp.prepare(cfg);
    for (std::uint32_t s = 0; s < kStems; ++s) {
        auto& cs = mp.matrix().strip(s);
        cs.setHpfHz(60.0f); cs.setLowDb(3.0f); cs.setMidDb(-2.0f); cs.setMidHz(800.0f); cs.setHighDb(2.0f);
        cs.setThresholdDb(-40.0f); cs.setRatio(3.0f); cs.setMakeupDb(24.0f);
        mp.matrix().setStemTrimDb(s, 12.0f);
    }
    mp.limiter().setEnabled(true);
    mp.limiter().setCeilingDb(-0.3f);
    std::vector<float> out(8 * kBlock);
    float* outs[8];
    for (int i = 0; i < 8; ++i) outs[i] = out.data() + i * kBlock;
    engine::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 8; ctx.frames = kBlock;
    float pk = 0.0f;
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int b = 0; b < 1000; ++b) {
            mp.process(ctx);
            for (std::uint32_t i = 0; i < kBlock; ++i) pk = std::max({pk, std::fabs(outs[0][i]), std::fabs(outs[1][i])});
        }
        allocs = scope.count();
    }
    std::printf("  13 strips (all bands + compressor, +24 dB makeup) into the limiter: main peak %.4f (ceiling %.4f), limiter %.1f dB,"
                " %llu heap allocations\n", pk, std::pow(10.0f, -0.3f / 20.0f), mp.limiter().gainReductionDb(),
                static_cast<unsigned long long>(allocs));
    CHECK(allocs == 0 && pk <= std::pow(10.0f, -0.3f / 20.0f) * 1.000001f && mp.latencyFrames() == 48);

    // Cost of the full chain at the rig's 480-frame block (CI machine; the rig will differ).
    using clock = std::chrono::steady_clock;
    constexpr int kIters = 3000;
    const auto t0 = clock::now();
    for (int k = 0; k < kIters; ++k) mp.process(ctx);
    const double us = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kIters;
    std::printf("  full chain, 13 stems with EQ + compressor + limiter, 480 frames: %.1f us per block = %.2f %% of one core\n", us,
                100.0 * us / (1e6 * kBlock / kFs));
    CHECK(us < 1e6 * kBlock / kFs);
}

void testStripParams() {
    std::printf("[strip + limiter parameters]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    // 13 x 10 strip parameters + limiter on/off + ceiling, appended after the 0.7 set (VST3 ids unchanged).
    const std::size_t before = 13 * 9 + 4 * 2;
    CHECK(tree.size() == before + 13 * 10 + 2);
    CHECK(tree.at(before - 1).spec().id == "aux3.mute" && tree.at(before).spec().id == "stem01.hpf");
    params::Parameter& hpf = *tree.find("stem03.hpf");
    params::Parameter& midf = *tree.find("stem03.midf");
    params::Parameter& ratio = *tree.find("stem03.ratio");
    CHECK(hpf.format() == "OFF" && ratio.format() == "OFF");
    midf.setPlain(2500.0f);
    CHECK(midf.format() == "2.50 kHz" && std::fabs(midf.toPlain(midf.toNormalized(2500.0f)) - 2500.0f) < 0.05f);
    midf.setNormalized(0.5f);   // log taper: the middle of travel is the geometric mean
    CHECK(std::fabs(midf.plain() - std::sqrt(150.0f * 8000.0f)) < 0.5f);
    ratio.setPlain(4.0f);
    CHECK(ratio.format() == "4.0:1");
    CHECK(tree.find("stem03.att")->format() == "10 ms" && tree.find("stem03.lo")->format() == "+0.0 dB");
    CHECK(tree.find("main.limit")->isOn() && tree.find("main.ceil")->format() == "-0.3 dB");

    // Bindings reach the strip and the limiter.
    const params::MixerBindings bind(tree);
    auto m = std::make_unique<dsp::StemMatrix>();
    m->prepare(kBlock, kFs);
    dsp::BusLimiter lim;
    lim.prepare(kFs, kBlock);
    bind.applyAll(tree, *m, &lim);
    CHECK(lim.enabled());
    tree.find("main.limit")->setPlain(0.0f);
    bind.apply(*tree.find("main.limit"), *m, &lim);
    CHECK(!lim.enabled());
    tree.find("stem05.mid")->setPlain(9.0f);
    tree.find("stem05.midf")->setPlain(1000.0f);
    bind.apply(*tree.find("stem05.mid"), *m, &lim);
    bind.apply(*tree.find("stem05.midf"), *m, &lim);
    CHECK(std::fabs(stripGainDb(m->strip(4), 1000.0) - 9.0) < 0.05);
    int mapped = 0;
    for (std::size_t i = before; i < tree.size(); ++i) mapped += bind.entry(i).target != params::MixerBindings::Target::None;
    CHECK(mapped == static_cast<int>(13 * 10 + 2));
}

} // namespace hc::test
