// The HEM effects: rules every effect must keep (all of them, from the registry), then
// HEM CLIP, HEM CEILING and HEM HEAT in detail. The other effects: fx_dyn_tests.cpp,
// fx_tone_tests.cpp.
#include "FxTestUtil.hpp"
#include "hybrid/fx/Dsp.hpp"
#include "TestHarness.hpp"

#include <cstdio>
#include <set>

namespace hc::test {

using namespace fxt;

void testFxCommon() {
    std::printf("[fx] every HEM effect: parameters, silence, finite output, no allocation, constant latency\n");
    std::set<std::string> names;
    std::set<std::uint32_t> uids;
    for (const fx::EffectInfo& info : fx::effects()) {
        CHECK(names.insert(info.name).second);   // names and UIDs unique
        CHECK(uids.insert(info.processorUid[0]).second && uids.insert(info.controllerUid[0]).second);
        auto e = info.create();
        CHECK(e != nullptr);
        if (!e) continue;
        const auto ps = e->params();
        CHECK(!ps.empty() && ps.size() <= static_cast<std::size_t>(fx::Effect::kMaxParams));
        std::set<std::string> ids;
        bool rangesOk = true, roundTrip = true;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            const fx::ParamInfo& p = ps[i];
            ids.insert(p.id);
            if (!(p.min < p.max) || p.def < p.min || p.def > p.max || (p.taper == fx::Taper::Log && p.min <= 0)) rangesOk = false;
            if (p.taper == fx::Taper::Choice && !p.choices) rangesOk = false;
            for (float nrm : {0.0f, 0.37f, 1.0f}) {   // normalized ↔ plain ↔ text
                const float v = fx::toPlain(p, nrm);
                float back = 0;
                if (!fx::parseValue(p, fx::formatValue(p, v), back)) { roundTrip = false; continue; }
                const float tol = p.taper == fx::Taper::Log ? v * 0.02f + 0.6f : (p.max - p.min) * 0.02f + 0.06f;
                if (std::fabs(back - v) > tol) roundTrip = false;
            }
            if (std::fabs(e->get(static_cast<int>(i)) - p.def) > 1e-6f) rangesOk = false;   // starts at the defaults
        }
        CHECK(ids.size() == ps.size() && rangesOk && roundTrip);
        if (!rangesOk || !roundTrip) std::printf("  FAIL detail: %s parameters\n", info.name);

        e->prepare(48000.0, 512);
        const std::uint32_t lat = e->latency();
        CHECK(lat < 48000);
        // Silence stays silence (no noise, no denormal blow-up) — the reverb's tail too.
        Stereo z(48000);
        run(*e, z);
        CHECK(peak(z.l) < 1e-4f && peak(z.r) < 1e-4f);
        // Loud noise and every parameter swept to both ends while it plays: finite, no allocation.
        Stereo s = noise(2.0, 0.9f);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (std::size_t i = 0, blk = 0; i < s.size(); i += 333, ++blk) {
                const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(333, s.size() - i));
                const std::size_t pi = blk % ps.size();
                e->set(static_cast<int>(pi), (blk / ps.size()) % 2 ? ps[pi].max : ps[pi].min);
                e->process(s.l.data() + i, s.r.data() + i, n);
            }
            allocs = scope.count();
        }
        CHECK(allocs == 0);
        CHECK(finite(s));
        CHECK(e->latency() == lat);
        if (allocs || !finite(s)) std::printf("  FAIL detail: %s allocs %llu finite %d\n", info.name, static_cast<unsigned long long>(allocs), finite(s) ? 1 : 0);
        // Back to defaults + reset: silence again.
        e->setDefaults();
        e->reset();
        Stereo z2(24000);
        run(*e, z2);
        CHECK(peak(z2.l) < 1e-4f);
        // Block size never changes the result (1 vs 512 samples).
        auto a = info.create(), b = info.create();
        a->prepare(48000.0, 512); b->prepare(48000.0, 512);
        Stereo x = noise(0.25, 0.5f, 3), y = x;
        run(*a, x, 512);
        run(*b, y, 1);
        double diff = 0;
        for (std::size_t i = 0; i < x.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(x.l[i] - y.l[i])));
        CHECK(diff < 1e-3);
        if (diff >= 1e-3) std::printf("  FAIL detail: %s block-size difference %.5f\n", info.name, diff);
        std::printf("  %-14s %2zu params  latency %u  ok\n", info.name, ps.size(), lat);
    }
    CHECK(fx::effects().size() == 22);
}

void testFxClip() {
    std::printf("[fx] HEM CLIP: ceiling holds, drive adds loudness, latency lines up the dry\n");
    auto e = fx::makeClip();
    e->prepare(48000.0, 512);
    e->set(param(*e, "ceiling"), -3.0f);
    e->set(param(*e, "drive"), 12.0f);
    e->set(param(*e, "knee"), 0.0f);
    Stereo s = sine(100.0, 1.0, 0.5f);
    const double inRms = rmsDb(s.l, 24000);
    run(*e, s);
    const float pk = peak(s.l, 4800);
    CHECK(pk < fx::dbToGain(-3.0f) * 1.06f);   // the FIR ringing of a hard clip stays within ~0.5 dB
    CHECK(rmsDb(s.l, 24000) > inRms + 4.5);   // louder: 0.5 driven +12 dB, clipped at -3 dB ≈ +6 dB RMS
    CHECK(e->reductionDb() < -6.0f);   // 2.0 peak into a 0.708 ceiling = 9 dB shaved
    // Below the ceiling it changes nothing (apart from the delay).
    auto c = fx::makeClip();
    c->prepare(48000.0, 512);
    Stereo q = sine(1000.0, 0.5, 0.25f), ref = q;
    run(*c, q);
    const std::uint32_t lat = c->latency();
    double err = 0;
    for (std::size_t i = 1000; i < q.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(q.l[i] - ref.l[i - lat])));
    CHECK(lat > 0 && err < 2e-3);
}

// True peak of a signal: 8x band-limited interpolation (independent of the effect's own 4x).
static float truePeak(const std::vector<float>& x, std::size_t from) {
    float pk = 0;
    constexpr int kUp = 8, kHalf = 32;
    for (std::size_t n = from + kHalf; n + kHalf < x.size(); ++n)
        for (int ph = 0; ph < kUp; ++ph) {
            const double t = static_cast<double>(ph) / kUp;
            double acc = 0;
            for (int k = -kHalf + 1; k <= kHalf; ++k) {
                const double d = t - k;
                const double sinc = d == 0 ? 1.0 : std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
                const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * d / (kHalf + 1));
                acc += x[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(n) + k)] * sinc * w;
            }
            pk = std::max(pk, static_cast<float>(std::fabs(acc)));
        }
    return pk;
}

void testFxCeiling() {
    std::printf("[fx] HEM CEILING: true-peak ceiling holds, quiet audio untouched, release\n");
    // A sine at fs/4 + phase has inter-sample peaks ~3 dB above its samples: the classic test.
    auto e = fx::makeCeiling();
    e->prepare(48000.0, 512);
    e->set(param(*e, "ceiling"), -1.0f);
    e->set(param(*e, "gain"), 12.0f);
    Stereo s(48000);
    for (std::size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = 0.5f * static_cast<float>(std::sin(std::numbers::pi / 2 * static_cast<double>(i) + std::numbers::pi / 4));
    Stereo m = noise(1.0, 0.7f, 11);   // and loud noise with sharp transients (music-like band: below 18 kHz)
    for (int pass = 0; pass < 2; ++pass) {
        fx::Biquad2 lp;
        lp.set(fx::BiquadCoefs::make(fx::BiquadCoefs::Type::LowPass, 48000.0, 18000.0, 0.707));
        lp.run(m.l.data(), m.r.data(), static_cast<std::uint32_t>(m.size()));
    }
    for (std::size_t i = 0; i < s.size(); ++i) { s.l[i] += m.l[i] * (i % 9000 < 300 ? 1.0f : 0.1f); s.r[i] += m.r[i] * 0.1f; }
    run(*e, s);
    const float ceil = fx::dbToGain(-1.0f);
    const float tpL = truePeak(s.l, 2000), tpR = truePeak(s.r, 2000);
    std::printf("  true peak out %.2f / %.2f dBTP (ceiling -1.00), sample peak %.2f dBFS, reduction meter %.1f dB\n",
                fx::gainToDb(tpL), fx::gainToDb(tpR), fx::gainToDb(peak(s.l, 2000)), e->reductionDb());
    CHECK(peak(s.l, 2000) <= ceil + 1e-6f);
    CHECK(tpL < ceil * fx::dbToGain(0.1f) && tpR < ceil * fx::dbToGain(0.1f));
    CHECK(e->reductionDb() < -6.0f);
    // Below the ceiling: exact delayed copy.
    auto q = fx::makeCeiling();
    q->prepare(48000.0, 512);
    Stereo a = sine(440.0, 0.5, 0.3f), ref = a;
    run(*q, a);
    const std::uint32_t lat = q->latency();
    double err = 0;
    for (std::size_t i = lat; i < a.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(a.l[i] - ref.l[i - lat])));
    CHECK(err < 1e-6);
    // Release: after a burst, gain comes back (80 ms auto) — level 0.5 s later is unity again.
    auto r = fx::makeCeiling();
    r->prepare(48000.0, 512);
    Stereo b = sine(200.0, 1.5, 0.2f);
    for (std::size_t i = 4800; i < 6000; ++i) { b.l[i] *= 5.0f; b.r[i] *= 5.0f; }
    run(*r, b);
    const double late = rmsDb(b.l, 48000, 72000), want = 20.0 * std::log10(0.2 / std::sqrt(2.0));
    CHECK(std::fabs(late - want) < 0.2);
}

void testFxHeat() {
    std::printf("[fx] HEM HEAT: harmonics by character, auto gain, mix 0 = dry, tone tilt\n");
    auto harmonics = [](int mode, float driveDb, double& h2, double& h3) {
        auto e = fx::makeHeat();
        e->prepare(48000.0, 512);
        e->set(param(*e, "mode"), static_cast<float>(mode));
        e->set(param(*e, "drive"), driveDb);
        e->reset();
        Stereo s = sine(500.0, 1.0, 0.3f);
        run(*e, s);
        auto bin = [&](double hz) {   // DFT magnitude over the settled half
            double re = 0, im = 0;
            for (std::size_t i = 24000; i < 48000; ++i) {
                const double w = 2 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
                re += s.l[i] * std::cos(w); im += s.l[i] * std::sin(w);
            }
            return std::sqrt(re * re + im * im);
        };
        const double f = bin(500);
        h2 = 20 * std::log10(bin(1000) / f + 1e-12);
        h3 = 20 * std::log10(bin(1500) / f + 1e-12);
        return rmsDb(s.l, 24000) - (20.0 * std::log10(0.3 / std::sqrt(2.0)));
    };
    double t2, t3, u2, u3, x2, x3;
    const double tapeLvl = harmonics(0, 18.0f, t2, t3), tubeLvl = harmonics(1, 18.0f, u2, u3), trLvl = harmonics(2, 18.0f, x2, x3);
    std::printf("  drive 18 dB, 500 Hz: TAPE h2 %.1f h3 %.1f dB (level %+.1f)  TUBE h2 %.1f h3 %.1f (%+.1f)  TRANSISTOR h2 %.1f h3 %.1f (%+.1f)\n",
                t2, t3, tapeLvl, u2, u3, tubeLvl, x2, x3, trLvl);
    CHECK(t3 > -40.0 && t2 < -60.0);    // tape: odd harmonics, no even
    CHECK(u2 > -40.0);                  // tube: even harmonics
    CHECK(x3 > -40.0);
    CHECK(std::fabs(tapeLvl) < 4.0 && std::fabs(tubeLvl) < 4.0 && std::fabs(trLvl) < 4.0);   // auto gain keeps the level near
    // Mix 0 % = dry (delayed).
    auto e = fx::makeHeat();
    e->prepare(48000.0, 512);
    e->set(param(*e, "mix"), 0.0f);
    e->set(param(*e, "drive"), 30.0f);
    e->reset();
    Stereo a = noise(0.3, 0.5f), ref = a;
    run(*e, a);
    double err = 0;
    for (std::size_t i = e->latency(); i < a.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(a.l[i] - ref.l[i - e->latency()])));
    CHECK(err < 1e-5);
    // Tone +100: 8 kHz louder than 100 Hz relative to tone 0.
    auto t = fx::makeHeat();
    t->prepare(48000.0, 512);
    t->set(param(*t, "drive"), 0.0f);
    t->set(param(*t, "tone"), 100.0f);
    const double hiUp = gainAtDb(*t, 8000.0, 0.01f) - gainAtDb(*t, 100.0, 0.01f);
    CHECK(hiUp > 9.0);
}

} // namespace hc::test
