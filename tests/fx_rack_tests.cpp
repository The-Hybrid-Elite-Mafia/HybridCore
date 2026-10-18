// FX RACK (engine::FxRack in the stem matrix): inserts change the sound, bypass is clean,
// delay compensation keeps every stem in time, text round-trips, no allocation on the audio path.
#include "FxTestUtil.hpp"
#include "TestHarness.hpp"
#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/engine/FxRack.hpp"
#include "hybrid/fx/Effects.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace hc::test {
namespace {

int typeOf(const char* name) {
    const auto list = fx::effects();
    for (std::size_t i = 0; i < list.size(); ++i) if (std::string(list[i].name) == name) return static_cast<int>(i);
    return -1;
}

// Runs `seconds` of the given stems (L only = mono) through matrix + rack; returns Main L.
std::vector<float> render(dsp::StemMatrix& m, engine::FxRack* rack, const std::vector<std::vector<float>>& stems, std::uint32_t block = 480) {
    m.setInsertHook(rack);
    const std::size_t n = stems[0].size();
    std::vector<float> out(n, 0.0f);
    std::vector<const float*> L(dsp::StemMatrix::kStems, nullptr), R(dsp::StemMatrix::kStems, nullptr);
    for (std::size_t i = 0; i < n; i += block) {
        const auto k = static_cast<std::uint32_t>(std::min<std::size_t>(block, n - i));
        for (std::size_t s = 0; s < stems.size(); ++s) L[s] = R[s] = stems[s].data() + i;
        m.process(L.data(), R.data(), k);
        std::copy_n(m.busChannel(0, 0), k, out.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return out;
}

} // namespace

void testFxRack() {
    std::printf("[fx] FX RACK: inserts on the stem strips (sound, bypass, delay compensation, save text, no allocation)\n");
    constexpr std::uint32_t kBlock = 480;
    const std::size_t n = 48000;

    // 1) No effects: the rack changes nothing.
    {
        std::vector<std::vector<float>> stems(2, std::vector<float>(n));
        for (std::size_t i = 0; i < n; ++i) { stems[0][i] = 0.3f * std::sin(0.05f * static_cast<float>(i)); stems[1][i] = 0.2f * std::sin(0.013f * static_cast<float>(i)); }
        dsp::StemMatrix a, b;
        a.prepare(kBlock); b.prepare(kBlock);
        a.snapToTargets(); b.snapToTargets();
        engine::FxRack rack;
        rack.prepare(48000.0, kBlock);
        const auto plain = render(a, nullptr, stems), racked = render(b, &rack, stems);
        double d = 0;
        for (std::size_t i = 0; i < n; ++i) d = std::max(d, static_cast<double>(std::fabs(plain[i] - racked[i])));
        CHECK(d < 1e-7);
    }
    // 2) HEM DESK driven hard on stem 1 changes it; bypass brings back the input (after its 10 ms fade).
    {
        std::vector<std::vector<float>> stems(1, std::vector<float>(n));
        for (std::size_t i = 0; i < n; ++i) stems[0][i] = 0.8f * std::sin(2.0f * 3.14159265f * 200.0f * static_cast<float>(i) / 48000.0f);
        dsp::StemMatrix plainM, m;
        plainM.prepare(kBlock); m.prepare(kBlock);
        plainM.snapToTargets(); m.snapToTargets();
        engine::FxRack rack;
        rack.prepare(48000.0, kBlock);
        CHECK(rack.setEffect(0, 0, typeOf("HEM DESK")));
        rack.effect(0, 0)->set(fxt::param(*rack.effect(0, 0), "drive"), 18.0f);
        const auto plain = render(plainM, nullptr, stems);
        const auto wet = render(m, &rack, stems);
        const std::uint32_t lat = rack.latency();
        double diff = 0;
        for (std::size_t i = 24000; i < n; ++i) diff = std::max(diff, static_cast<double>(std::fabs(wet[i] - plain[i - lat])));
        rack.setBypass(0, 0, true);
        const auto byp = render(m, &rack, stems);
        double bd = 0;
        for (std::size_t i = 24000; i < n; ++i) bd = std::max(bd, static_cast<double>(std::fabs(byp[i] - plain[i - lat])));
        std::printf("  HEM DESK on strip 1: max change %.3f, bypassed %.2e (latency %u samples)\n", diff, bd, lat);
        CHECK(diff > 0.05);
        CHECK(bd < 1e-5);
        CHECK(lat == fx::effects()[static_cast<std::size_t>(typeOf("HEM DESK"))].create()->latency());
    }
    // 3) Delay compensation: HEM HUSH (2048 samples) on stem 1 only; a click on stems 1 and 2 at
    //    the same moment comes out of Main at the same moment.
    {
        std::vector<std::vector<float>> stems(2, std::vector<float>(n, 0.0f));
        stems[0][10000] = 0.5f;
        dsp::StemMatrix m1, m2;
        m1.prepare(kBlock); m2.prepare(kBlock);
        m1.snapToTargets(); m2.snapToTargets();
        engine::FxRack rack;
        rack.prepare(48000.0, kBlock);
        CHECK(rack.setEffect(0, 0, typeOf("HEM HUSH")));
        rack.effect(0, 0)->set(fxt::param(*rack.effect(0, 0), "amount"), 0.0f);
        rack.effect(0, 0)->set(fxt::param(*rack.effect(0, 0), "reduction"), 3.0f);
        auto peakAt = [](const std::vector<float>& x) {
            std::size_t at = 0; float best = 0;
            for (std::size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > best) { best = std::fabs(x[i]); at = i; }
            return at;
        };
        const auto a = render(m1, &rack, stems);
        std::vector<std::vector<float>> other(2, std::vector<float>(n, 0.0f));
        other[1][10000] = 0.5f;
        const auto b = render(m2, &rack, other);   // same rack: stem 2 has no effect but is delayed to match
        std::printf("  click via HEM HUSH (strip 1) at %zu, via no effect (strip 2) at %zu; rack latency %u\n", peakAt(a), peakAt(b), rack.latency());
        CHECK(peakAt(a) == peakAt(b));
        CHECK(peakAt(a) == 10000 + rack.latency());
        CHECK(rack.latency() == 2048);
    }
    // 4) Text round trip; unknown effects are reported, not crashed on.
    {
        engine::FxRack a, b;
        a.prepare(48000.0, kBlock); b.prepare(48000.0, kBlock);
        CHECK(a.setEffect(4, 2, typeOf("HEM ECHO")));
        a.effect(4, 2)->set(fxt::param(*a.effect(4, 2), "feedback"), 63.0f);
        a.setBypass(4, 2, true);
        CHECK(a.setEffect(12, 0, typeOf("HEM TUNE")));
        const std::string t = a.toText();
        const std::string note = b.fromText(t + "stem02.fx1=HEM FUTURE|bypass=0\n");
        CHECK(b.type(4, 2) == typeOf("HEM ECHO") && b.bypass(4, 2) && b.type(12, 0) == typeOf("HEM TUNE"));
        CHECK(b.effect(4, 2) && std::fabs(b.effect(4, 2)->get(fxt::param(*b.effect(4, 2), "feedback")) - 63.0f) < 1e-4f);
        CHECK(b.toText() == t);
        CHECK(note.find("HEM FUTURE") != std::string::npos);
        CHECK(b.count(4) == 1 && b.count(1) == 0 && b.anyEffect());
        b.clear();
        CHECK(!b.anyEffect());
    }
    // 5) Audio path: no allocation with effects in every slot of a strip.
    {
        dsp::StemMatrix m;
        m.prepare(kBlock);
        engine::FxRack rack;
        rack.prepare(48000.0, kBlock);
        CHECK(rack.setEffect(0, 0, typeOf("HEM PUNCH")) && rack.setEffect(0, 1, typeOf("HEM SWIRL KILLER")) && rack.setEffect(0, 2, typeOf("HEM ECHO")));
        m.setInsertHook(&rack);
        std::vector<float> x(kBlock, 0.1f);
        std::vector<const float*> L(dsp::StemMatrix::kStems, nullptr);
        L[0] = x.data();
        m.process(L.data(), L.data(), kBlock);   // first block takes the effects in
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (int i = 0; i < 200; ++i) m.process(L.data(), L.data(), kBlock);
            rack.setBypass(0, 1, true);
            for (int i = 0; i < 50; ++i) m.process(L.data(), L.data(), kBlock);
            allocs = scope.count();
        }
        CHECK(allocs == 0);
        CHECK(rack.latency() == 2048);
        // replacing an effect: the old one is handed back and freed on this (message) thread
        CHECK(rack.setEffect(0, 1, -1));
        m.process(L.data(), L.data(), kBlock);
        rack.collectRetired();
        CHECK(rack.latency() == 0 && rack.count(0) == 2);
    }
}

} // namespace hc::test
