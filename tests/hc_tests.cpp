// hc_tests — hardware-free unit tests for the RT core. Runs in CI and on the rig.
// This file: wait-free SPSC ring (2 threads), AVX2 sample conversion, engine command /
// retire / telemetry path including concurrent processor hot-swaps, and main().
#include "TestHarness.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/engine/Engine.hpp"
#include "hybrid/engine/ToneProcessor.hpp"
#include "SampleConvert.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace hc::io::convert;

namespace hc::test {

void testRing() {
    std::printf("[ring]\n");
    static hc::core::SpscRing<std::uint64_t, 1024> ring;
    constexpr std::uint64_t kItems = 10'000'000;
    std::thread producer([] {
        for (std::uint64_t i = 0; i < kItems;)
            if (ring.tryPush(i)) ++i;
    });
    std::uint64_t expect = 0, v = 0;
    bool inOrder = true;
    while (expect < kItems)
        if (ring.tryPop(v)) { if (v != expect) inOrder = false; ++expect; }
    producer.join();
    CHECK(inOrder);
    CHECK(!ring.tryPop(v));

    hc::core::SpscRing<int, 4> small;
    int x = 0;
    CHECK(small.tryPush(1) && small.tryPush(2) && small.tryPush(3) && small.tryPush(4));
    CHECK(!small.tryPush(5));
    CHECK(small.tryPop(x) && x == 1);
    CHECK(small.tryPush(5));
    std::printf("  10,000,000 items across 2 threads, in order: %s\n", inOrder ? "yes" : "NO");
}

void testConvert() {
    std::printf("[convert]\n");
    const std::vector<float> src = {0.f, 1.f, -1.f, 2.f, -2.f, 0.5f, -0.5f, NAN, 1e-30f,
                                    0.99999994f, 0.25f, -0.25f, 0.75f, -0.75f, 0.1f, -0.1f, 0.3f};
    {
        std::vector<std::int32_t> d(src.size());
        floatToS32(src.data(), d.data(), src.size());
        CHECK(d[1] == 2147483520);
        CHECK(d[2] == INT32_MIN);
        CHECK(d[3] == 2147483520 && d[4] == INT32_MIN);
        CHECK(d[7] == INT32_MIN);
        CHECK(d[5] == 1073741824);
        CHECK(d[16] == static_cast<std::int32_t>(std::lrintf(0.3f * 2147483648.f)));
        std::vector<float> back(src.size());
        s32ToFloat(d.data(), back.data(), d.size());
        CHECK(std::fabs(back[5] - 0.5f) < 1e-7f);
    }
    {
        std::vector<std::int32_t> d(src.size());
        floatToS24In32(src.data(), d.data(), src.size());
        CHECK(d[1] == (8388607 << 8));
        CHECK(d[2] == INT32_MIN);
        CHECK((d[5] & 0xFF) == 0 && (d[5] >> 8) == 4194304);
    }
    {
        std::vector<float> s(37);
        for (std::size_t i = 0; i < s.size(); ++i) s[i] = std::sin(0.37f * static_cast<float>(i)) * 1.2f;
        std::vector<std::int16_t> d(s.size());
        floatToS16(s.data(), d.data(), s.size());
        bool ok = true;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const float c = s[i] > 32767.f / 32768.f ? 32767.f / 32768.f : (s[i] >= -1.f ? s[i] : -1.f);
            if (d[i] != static_cast<std::int16_t>(std::lrintf(c * 32768.f))) ok = false;
        }
        CHECK(ok);
        std::vector<float> back(d.size());
        s16ToFloat(d.data(), back.data(), d.size());
        CHECK(std::fabs(back[3] - d[3] / 32768.f) < 1e-9f);
    }
    {
        std::vector<std::uint8_t> p(src.size() * 3);
        floatToS24Packed(src.data(), p.data(), src.size());
        std::vector<float> back(src.size());
        s24PackedToFloat(p.data(), back.data(), src.size());
        CHECK(back[2] == -1.0f);
        CHECK(std::fabs(back[13] + 0.75f) < 2e-7f);
    }
    {
        std::vector<double> d(src.size());
        floatToF64(src.data(), d.data(), src.size());
        std::vector<float> back(src.size());
        f64ToFloat(d.data(), back.data(), src.size());
        CHECK(back[5] == 0.5f && back[16] == 0.3f);
    }
    {
        const std::uint32_t F = 21;
        std::vector<float> l(F), r(F), c(F, 7.f), out(2 * F), out3(3 * F);
        for (std::uint32_t i = 0; i < F; ++i) { l[i] = static_cast<float>(i); r[i] = -static_cast<float>(i); }
        const float* p2[2] = {l.data(), r.data()};
        interleave(p2, 2, F, out.data());
        bool ok = true;
        for (std::uint32_t i = 0; i < F; ++i)
            if (out[2 * i] != l[i] || out[2 * i + 1] != r[i]) ok = false;
        CHECK(ok);
        const float* p3[3] = {l.data(), r.data(), c.data()};
        interleave(p3, 3, F, out3.data());
        CHECK(out3[15] == 5.f && out3[16] == -5.f && out3[17] == 7.f);
    }
}

struct EngineCounter final : hc::engine::AudioProcessor {
    static inline std::atomic<int> alive{0};
    EngineCounter() { ++alive; }
    ~EngineCounter() override { --alive; }
    void prepare(const hc::io::StreamConfig&) override {}
    void process(const hc::engine::ProcessContext& c) noexcept override {
        for (std::uint32_t i = 0; i < c.frames; ++i) c.outputs[0][i] = 1.f;
    }
};

void testEngine() {
    std::printf("[engine]\n");
    constexpr std::uint32_t F = 100;
    std::vector<float> a(F), b(F);
    float* outs[2] = {a.data(), b.data()};
    {
        hc::engine::Engine e;
        std::unique_ptr<hc::engine::AudioProcessor> p = std::make_unique<hc::engine::ToneProcessor>();
        CHECK(!e.install(p));   // not prepared yet
        e.prepare(hc::io::StreamConfig{hc::kSampleRate, F, 0, 2, 0.0, 0.0});
        CHECK(e.install(p) && !p);
        e.process(nullptr, 0, outs, 2, F);
        float peak = 0.f;
        for (float v : a) peak = std::fmax(peak, std::fabs(v));
        CHECK(std::fabs(peak - 0.1258925f) < 2e-3f);   // -18 dBFS
        CHECK(a[37] == b[37]);
        CHECK(e.setMasterGain(0.5f));
        for (int k = 0; k < 30; ++k) e.process(nullptr, 0, outs, 2, F);
        hc::engine::Telemetry t;
        int reports = 0;
        while (e.popTelemetry(t)) ++reports;
        CHECK(reports > 0);
    }
    {
        hc::engine::Engine e;
        e.prepare(hc::io::StreamConfig{hc::kSampleRate, F, 0, 1, 0.0, 0.0});
        float* o1[1] = {a.data()};
        std::unique_ptr<hc::engine::AudioProcessor> c = std::make_unique<EngineCounter>();
        CHECK(e.install(c));
        CHECK(e.setMasterGain(0.5f));
        e.process(nullptr, 0, o1, 1, F);
        CHECK(std::fabs(a[0] - 1.0f) < 1e-6f);
        CHECK(std::fabs(a[50] - 0.75f) < 1e-5f);
        CHECK(std::fabs(a[99] - 0.505f) < 1e-5f);
        e.process(nullptr, 0, o1, 1, F);
        CHECK(a[10] == 0.5f);
        CHECK(e.setMuted(true));
        e.process(nullptr, 0, o1, 1, F);
        e.process(nullptr, 0, o1, 1, F);
        CHECK(a[10] == 0.f);

        std::atomic<bool> stop{false};
        std::thread rt([&] {
            std::vector<float> buf(F);
            float* o[1] = {buf.data()};
            while (!stop.load()) e.process(nullptr, 0, o, 1, F);
        });
        int installed = 0;
        while (installed < 5000) {
            std::unique_ptr<hc::engine::AudioProcessor> q = std::make_unique<EngineCounter>();
            if (e.install(q)) ++installed;
            e.collectGarbage();
        }
        stop.store(true);
        rt.join();
        // Drain swaps still queued when the RT thread stopped (this thread is now the only consumer).
        std::vector<float> tail(F);
        float* ot[1] = {tail.data()};
        for (int k = 0; k < 8; ++k) e.process(nullptr, 0, ot, 1, F);
        e.collectGarbage();
        CHECK(EngineCounter::alive.load() == 1);
        std::printf("  5,000 hot-swaps under a live RT thread, live processors left: %d (expect 1)\n",
                    EngineCounter::alive.load());
    }
    CHECK(EngineCounter::alive.load() == 0);
}

} // namespace hc::test

int main(int argc, char** argv) {
    using namespace hc::test;
    hc::test::setExePath(argc > 0 ? argv[0] : "");
    testRing();
    testConvert();
    testEngine();
    testKernels();
    testMatrix();
    testMixer();
    testZeroAllocation();
    testParams();
    testSettings();
    testMixerBinding();
    testChannelStrip();
    testLimiter();
    testStripInMatrix();
    testStripParams();
    testMediaFiles();
    testResampler();
    testStemLoader();
    testStemPlayback();
    testTransport();
    testSession();
    testExport();
    testLoudness();
    testAnalyzer();
    testForgeComposer();
    testForgeEngine();
    testForgeInMixer();
    testRemake();
    testUiImage();
    testUiLayout();
    testUiFaceplate();
    testUiParts();
    testUiFont();
    benchmark();
    std::printf("\n%d checks, %d failed -> %s\n", gChecks, gFails, gFails ? "FAIL" : "PASS");
    return gFails ? 1 : 0;
}
