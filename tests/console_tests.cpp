// HEM Digital Console — Phase 1 engine tests with MEASURED results:
//   frequency responses (incl. our desk EQ vs the reference maker's own published curves), compression
//   ratios, attack / release timing (incl. dual-rate auto release), routing, zero heap allocation on the
//   audio path, and CPU time per block at 48 kHz and 96 kHz.
// Every number printed here is measured by running audio through the engine.
#include "TestHarness.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/ConsoleRig.hpp"
#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/FrontTools.hpp"
#include "hybrid/console/MeterBridge.hpp"
#include "hybrid/console/StepEq.hpp"
#include "hybrid/console/StudioDesk.hpp"
#include "hybrid/console/TwinBus.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace hc::test {
namespace {

using namespace hc::console;
constexpr double kFs = 48000.0;
using Buf = std::vector<float>;

double rmsDb(const Buf& x, std::size_t from, std::size_t to) {
    double a = 0;
    to = std::min(to, x.size());
    for (std::size_t i = from; i < to; ++i) a += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(a / static_cast<double>(std::max<std::size_t>(1, to - from)), 1e-30));
}
Buf sine(double hz, double seconds, double ampDbfs, double fs = kFs) {
    Buf b(static_cast<std::size_t>(seconds * fs));
    const double a = std::pow(10.0, ampDbfs / 20.0);
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<float>(a * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / fs));
    return b;
}
// Total harmonic distortion (%) of x at fundamental f (integer cycles over the window), harmonics 2..10.
double thdPercent(const Buf& x, double f, std::size_t from, std::size_t len, double fs = kFs) {
    auto mag = [&](double hz) {
        std::complex<double> s{0, 0};
        for (std::size_t i = 0; i < len; ++i) s += static_cast<double>(x[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / fs);
        return std::abs(s);
    };
    const double h1 = mag(f);
    double hs = 0;
    for (int k = 2; k <= 10; ++k) if (k * f < fs / 2) { const double m = mag(k * f); hs += m * m; }
    return 100.0 * std::sqrt(hs) / std::max(h1, 1e-30);
}

// ---- generic "gain at frequency" for a stereo in-place processor -------------------------------
using Proc = std::function<void(float*, float*, std::uint32_t)>;
double gainDb(const Proc& p, double hz, double fs = kFs, double ampDb = -20.0, int side = 0) {
    Buf l = sine(hz, 0.6, ampDb, fs), r = l;
    for (std::size_t i = 0; i < l.size(); i += 256) p(l.data() + i, r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(256, l.size() - i)));
    const Buf& o = side ? r : l;
    const std::size_t h = o.size() / 2;
    return rmsDb(o, h, o.size()) - (ampDb - 3.0103);
}

// Level-step timing: in dB-gain terms, how long until 63.2 % of the gain change after a step up (attack)
// and after a step down (release). The signal is a 1 kHz sine; gain is read per 1 ms window.
struct Timing { double grDb, attackMs, releaseMs; };
Timing stepTiming(const Proc& p, double lowDb, double highDb, double highSeconds, double fs = kFs, double tailSeconds = 2.0, std::size_t latency = 0) {
    const double pre = 1.0;
    const std::size_t n = static_cast<std::size_t>((pre + highSeconds + tailSeconds) * fs);
    Buf in(n), l(n), r(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        const double lv = (t >= pre && t < pre + highSeconds) ? highDb : lowDb;
        in[i] = static_cast<float>(std::pow(10.0, lv / 20.0) * std::sin(2 * std::numbers::pi * 1000.0 * t));
    }
    l = in; r = in;
    for (std::size_t i = 0; i < n; i += 64) p(l.data() + i, r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(64, n - i)));
    if (latency) { l.erase(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(latency)); l.resize(n, 0.0f); }   // line output up with input
    const std::size_t win = static_cast<std::size_t>(fs / 1000.0);
    auto gainAt = [&](std::size_t w) { return rmsDb(l, w * win, (w + 1) * win) - rmsDb(in, w * win, (w + 1) * win); };
    const std::size_t w0 = static_cast<std::size_t>(pre * 1000.0), w1 = static_cast<std::size_t>((pre + highSeconds) * 1000.0);
    const double base = gainAt(w0 - 5), settled = gainAt(w1 - 2);
    const double gr = base - settled;
    double att = -1, rel = -1;
    for (std::size_t w = w0; w < w1; ++w) if (base - gainAt(w) >= 0.632 * gr) { att = static_cast<double>(w - w0) + 0.5; break; }
    const double endGain = gainAt(n / win - 2);
    const double relSpan = endGain - settled;
    for (std::size_t w = w1; w < n / win; ++w) if (gainAt(w) - settled >= 0.632 * relSpan) { rel = static_cast<double>(w - w1) + 0.5; break; }
    return {gr, att, rel};
}

// Static ratio: output level change per input level change over [a, b] dB above threshold.
double measuredRatio(const Proc& p, const std::function<void()>& reset, double inA, double inB, double fs = kFs) {
    auto outLevel = [&](double in) {
        reset();
        Buf l = sine(1000.0, 1.0, in, fs), r = l;
        for (std::size_t i = 0; i < l.size(); i += 256) p(l.data() + i, r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(256, l.size() - i)));
        return rmsDb(l, l.size() * 3 / 4, l.size());
    };
    const double slope = (outLevel(inB) - outLevel(inA)) / (inB - inA);
    return 1.0 / slope;
}

// ---- the reference maker's published desk EQ curves (traced from DESK-GR, full boost) -----------
struct GraphPt { double hz, db; };
#include "console_graph_tables.inc"
#include "console_colour_tables.inc"

// ---- desk wiring helper: one stem in channel 0, everything else silent ------------------------
struct DeskRig {
    std::unique_ptr<StudioDesk> d = std::make_unique<StudioDesk>();
    Buf zero, mainL, mainR, busBL, busBR, c1L, c1R, c2L, c2R, monL, monR, altL, altR, dirL, dirR, e1, e2;
    std::uint32_t maxBlock;
    explicit DeskRig(double fs = kFs, std::uint32_t mb = 512) : maxBlock(mb) {
        d->prepare(fs, mb);
        for (Buf* b : {&zero, &mainL, &mainR, &busBL, &busBR, &c1L, &c1R, &c2L, &c2R, &monL, &monR, &altL, &altR, &dirL, &dirR, &e1, &e2})
            b->assign(mb, 0.0f);
    }
    // Processes n <= maxBlock frames: stem (l, r) on channel ch.
    void run(const float* l, const float* r, std::uint32_t n, int ch = 0, const float* ext1 = nullptr) {
        StudioDesk::Inputs in;
        for (auto& p : in.left) p = nullptr;
        in.left[static_cast<std::size_t>(ch)] = l;
        in.right[static_cast<std::size_t>(ch)] = r;
        in.ext1L = ext1; in.ext1R = ext1;
        StudioDesk::Outputs o;
        o.mainL = mainL.data(); o.mainR = mainR.data(); o.busBL = busBL.data(); o.busBR = busBR.data();
        o.cue1L = c1L.data(); o.cue1R = c1R.data(); o.cue2L = c2L.data(); o.cue2R = c2R.data();
        o.monL = monL.data(); o.monR = monR.data(); o.altL = altL.data(); o.altR = altR.data();
        o.directL[static_cast<std::size_t>(ch)] = dirL.data(); o.directR[static_cast<std::size_t>(ch)] = dirR.data();
        d->process(in, o, n);
    }
    // Runs a whole signal through, collecting one output.
    Buf through(const Buf& l, const Buf& r, Buf DeskRig::*out, int ch = 0, const Buf* ext = nullptr) {
        Buf o(l.size());
        for (std::size_t i = 0; i < l.size(); i += maxBlock) {
            const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(maxBlock, l.size() - i));
            run(l.data() + i, r.data() + i, n, ch, ext ? ext->data() + i : nullptr);
            std::copy_n((this->*out).data(), n, o.data() + i);
        }
        return o;
    }
    Proc mainProc() {   // channel 0 -> main L/R as an in-place processor
        return [this](float* l, float* r, std::uint32_t n) {
            run(l, r, n);
            std::copy_n(mainL.data(), n, l); std::copy_n(mainR.data(), n, r);
        };
    }
};
double levelOf(const Buf& b) { return rmsDb(b, b.size() / 2, b.size()); }
void runBlocks(fx::Effect& e, Buf& l, Buf& r, std::size_t block = 256) {
    for (std::size_t i = 0; i < l.size(); i += block)
        e.process(l.data() + i, r.data() + i, static_cast<std::uint32_t>(std::min(block, l.size() - i)));
}

} // namespace

// =================================================================================================
void testConsoleDesk() {
    std::printf("[console: HEM STUDIO DESK]\n");
    using D = StudioDesk;
    // ---- HPF ------------------------------------------------------------------------------------
    {
        DeskRig rig;
        rig.d->setChannel(0, D::Hpf, 1);
        auto p = rig.mainProc();
        const double g75 = gainDb(p, 75), g37 = gainDb(p, 37.5), g19 = gainDb(p, 18.75), g1k = gainDb(p, 1000);
        std::printf("  HPF: 75 Hz %.2f dB, 37.5 Hz %.2f dB, 18.75 Hz %.2f dB (octave slope %.1f dB), 1 kHz %.3f dB\n", g75, g37, g19, g37 - g19, g1k);
        CHECK(std::fabs(g75 + 3.01) < 0.3); CHECK(std::fabs(g1k) < 0.05); CHECK(g37 - g19 > 10.0);
    }
    // ---- EQ vs the published curves ------------------------------------------------------------
    {
        struct Case { const char* name; const GraphPt* pts; std::size_t n; int knob; int bell; };
        const Case cases[] = {
            {"MF   +15", kGraphMF, std::size(kGraphMF), D::MfGain, -1},
            {"HF shelf +15", kGraphHFSHELF, std::size(kGraphHFSHELF), D::HfGain, 0},
            {"LF shelf +15", kGraphLFSHELF, std::size(kGraphLFSHELF), D::LfGain, 0},
            {"HF bell +15", kGraphHFBELL, std::size(kGraphHFBELL), D::HfGain, 1},
            {"LF bell +15", kGraphLFBELL, std::size(kGraphLFBELL), D::LfGain, 1},
        };
        for (const auto& c : cases) {
            for (double fs : {48000.0, 96000.0}) {
                DeskRig rig(fs);
                rig.d->setChannel(0, D::EqIn, 1);
                rig.d->setChannel(0, c.knob, 15.0f);
                if (c.bell >= 0) rig.d->setChannel(0, c.knob == D::HfGain ? D::HfBell : D::LfBell, static_cast<float>(c.bell));
                auto p = rig.mainProc();
                double se = 0, mx = 0;
                for (std::size_t i = 0; i < c.n; ++i) {
                    const double e = gainDb(p, c.pts[i].hz, fs) - c.pts[i].db;
                    se += e * e; mx = std::max(mx, std::fabs(e));
                }
                const double rms = std::sqrt(se / static_cast<double>(c.n));
                std::printf("  EQ %-13s vs published curve @ %3.0f kHz: rms err %.2f dB, max err %.2f dB (%zu pts, 20 Hz-20 kHz)\n", c.name, fs / 1000, rms, mx, c.n);
                CHECK(rms < 1.0);
            }
        }
        // cut = exact mirror of boost; knob law
        DeskRig rig;
        rig.d->setChannel(0, D::EqIn, 1);
        auto p = rig.mainProc();
        rig.d->setChannel(0, D::MfGain, 15); const double up = gainDb(p, 700);
        rig.d->setChannel(0, D::MfGain, -15); const double dn = gainDb(p, 700);
        rig.d->setChannel(0, D::MfGain, 7.5f); const double half = gainDb(p, 700);
        std::printf("  EQ MF @700 Hz: +15 knob %+.2f dB, -15 knob %+.2f dB (mirror err %.3f dB), +7.5 knob %+.2f dB\n", up, dn, up + dn, half);
        CHECK(std::fabs(up + dn) < 0.05); CHECK(std::fabs(half - up / 2) < 0.1);
    }
    // ---- channel compressor -----------------------------------------------------------------------
    {
        DeskRig rig;
        rig.d->setChannel(0, D::CompIn, 1);
        rig.d->setChannel(0, D::CompThreshold, -20.0f);   // dBu -> -44 dBFS
        auto p = rig.mainProc();
        const double ratio = measuredRatio(p, [&] { rig.d->reset(); }, -30.0, -10.0);
        std::printf("  channel comp: measured ratio %.2f:1 (spec 2:1), 14-34 dB over threshold\n", ratio);
        CHECK(std::fabs(ratio - 2.0) < 0.1);
        // auto make-up: +10 dBu (-14 dBFS) keeps its level whatever the threshold
        for (float thr : {10.0f, 0.0f, -10.0f, -20.0f}) {
            rig.d->setChannel(0, D::CompThreshold, thr);
            const double g = gainDb(p, 1000, kFs, -14.0);
            std::printf("    threshold %+5.1f dBu: a +10 dBu tone comes out %+.2f dB\n", thr, g);
            CHECK(std::fabs(g) < 2.0);
        }
        rig.d->setChannel(0, D::CompThreshold, -10.0f);   // -34 dBFS
        rig.d->reset();
        const Timing small = stepTiming(p, -60.0, -28.0, 1.0);   // ~6 dB over
        rig.d->reset();
        const Timing big = stepTiming(p, -60.0, -4.0, 1.0);      // ~30 dB over
        std::printf("  channel comp timing: 6 dB over -> attack %.1f ms; 30 dB over -> attack %.1f ms (program dependent, spec ~8..30 ms); release %.0f ms (spec ~300)\n",
                    small.attackMs, big.attackMs, big.releaseMs);
        CHECK(big.attackMs < small.attackMs); CHECK(big.attackMs >= 3 && small.attackMs <= 45);
        CHECK(big.releaseMs > 150 && big.releaseMs < 600);
    }
    // ---- pan / balance / mono ---------------------------------------------------------------------
    {
        float l, r;
        StudioDesk::panGains(0, l, r);
        std::printf("  pan law: centre %.2f dB each side (spec -4.5), hard left L %.2f dB\n", 20 * std::log10(l), 20 * std::log10([] { float a, b; StudioDesk::panGains(-1, a, b); return a; }()));
        CHECK(std::fabs(20 * std::log10(l) + 4.5) < 0.01);
        StudioDesk::balanceGains(0, l, r); CHECK(std::fabs(l - 1) < 1e-6f && std::fabs(r - 1) < 1e-6f);
        StudioDesk::balanceGains(1, l, r); CHECK(l < 1e-6f && std::fabs(r - 1) < 1e-6f);
        DeskRig rig;
        Buf L = sine(1000, 0.5, -20), R(L.size(), 0.0f);
        rig.d->setChannel(0, D::Mono, 1);
        const Buf oL = rig.through(L, R, &DeskRig::mainL), oR = rig.through(L, R, &DeskRig::mainR);
        std::printf("  MONO + centre pan: L-only input -> main L %.2f dB, main R %.2f dB\n", levelOf(oL) - levelOf(L), levelOf(oR) - levelOf(L));
        CHECK(std::fabs(levelOf(oL) - levelOf(oR)) < 0.01);
    }
    // ---- routing: MUTE/BUS B, cue pre/post, direct out, PFL, monitor, ALT, EXT, cue1 -> bus comp ----
    {
        DeskRig rig;
        const Buf s = sine(1000, 0.4, -20);
        rig.d->setChannel(0, D::Fader, -10.0f);
        rig.d->setChannel(0, D::Cue1On, 1); rig.d->setChannel(0, D::Cue1Level, 0.0f);
        rig.d->setChannel(0, D::Cue2On, 1); rig.d->setChannel(0, D::Cue2Level, 0.0f); rig.d->setChannel(0, D::Cue2Post, 1);
        const double cuePre = levelOf(rig.through(s, s, &DeskRig::c1L)) - levelOf(s);
        const double cuePost = levelOf(rig.through(s, s, &DeskRig::c2L)) - levelOf(s);
        const double dirPre = levelOf(rig.through(s, s, &DeskRig::dirL)) - levelOf(s);
        rig.d->setChannel(0, D::DirectPostFader, 1);
        const double dirPost = levelOf(rig.through(s, s, &DeskRig::dirL)) - levelOf(s);
        std::printf("  fader -10 dB: cue PRE %.2f, cue POST %.2f, direct PRE %.2f, direct POST %.2f dB\n", cuePre, cuePost, dirPre, dirPost);
        CHECK(std::fabs(cuePre) < 0.05 && std::fabs(cuePost + 10) < 0.05 && std::fabs(dirPre) < 0.05 && std::fabs(dirPost + 10) < 0.05);
        rig.d->setChannel(0, D::MuteBusB, 1);
        const double mainM = levelOf(rig.through(s, s, &DeskRig::mainL)), busB = levelOf(rig.through(s, s, &DeskRig::busBL)) - levelOf(s);
        std::printf("  MUTE/BUS B: main %.1f dBFS (silent), Bus B %.2f dB\n", mainM, busB);
        CHECK(mainM < -100 && std::fabs(busB + 10) < 0.05);
        rig.d->setMaster(D::BusBMute, 1);
        CHECK(levelOf(rig.through(s, s, &DeskRig::busBL)) < -100);
        rig.d->setMaster(D::BusBMute, 0);
        // monitor: sources sum (MAIN + BUS B), PFL interrupts, DIM, MONO, CUT, ALT
        rig.d->setChannel(0, D::MuteBusB, 0);
        rig.d->setChannel(0, D::Fader, 0.0f);
        rig.d->setMaster(D::MonBusB, 1);
        const double monMain = levelOf(rig.through(s, s, &DeskRig::monL)) - levelOf(s);
        rig.d->setMaster(D::Dim, 1); rig.d->setMaster(D::DimLevel, -20.0f);
        const double monDim = levelOf(rig.through(s, s, &DeskRig::monL)) - levelOf(s);
        rig.d->setMaster(D::Dim, 0);
        Buf sl = sine(1000, 0.4, -20), sr(sl.size(), 0.0f);
        rig.d->setMaster(D::MonoMonitor, 1);
        const double monoL = levelOf(rig.through(sl, sr, &DeskRig::monL)) - levelOf(sl);
        rig.d->setMaster(D::MonoMonitor, 0);
        rig.d->setMaster(D::Alt, 1);
        const double altOn = levelOf(rig.through(s, s, &DeskRig::altL)) - levelOf(s), monOff = levelOf(rig.through(s, s, &DeskRig::monL));
        rig.d->setMaster(D::Alt, 0);
        rig.d->setMaster(D::Cut, 1);
        const double cut = levelOf(rig.through(s, s, &DeskRig::monL));
        rig.d->setMaster(D::Cut, 0);
        rig.d->setChannel(0, D::Pfl, 1); rig.d->setChannel(0, D::Fader, -30.0f);
        const double pfl = levelOf(rig.through(s, s, &DeskRig::monL)) - levelOf(s);
        std::printf("  monitor: MAIN %.2f dB, DIM -20 -> %.2f dB, MONO (L-only in) %.2f dB, ALT -> alt %.2f dB / main mon %.0f dBFS, CUT %.0f dBFS, PFL (fader -30) %.2f dB, PFL lit %d\n",
                    monMain, monDim, monoL, altOn, monOff, cut, pfl, rig.d->pflActive() ? 1 : 0);
        CHECK(std::fabs(monMain) < 0.05 && std::fabs(monDim + 20) < 0.05 && std::fabs(altOn) < 0.05 && monOff < -100 && cut < -100);
        CHECK(std::fabs(pfl) < 0.05 && rig.d->pflActive());
        CHECK(std::fabs(monoL - 20 * std::log10(spec::desk::kMonoMonitorGain)) < 0.05);
        rig.d->setChannel(0, D::Pfl, 0); rig.d->setChannel(0, D::Fader, 0.0f);
        // EXT 1 return to main (+6 dB level)
        Buf z(s.size(), 0.0f);
        rig.d->setMaster(D::Ext1ToMain, 1); rig.d->setMaster(D::Ext1Level, 6.0f);
        const double ext = levelOf(rig.through(z, z, &DeskRig::mainL, 0, &s)) - levelOf(s);
        std::printf("  EXT 1 -> main at +6 dB: %.2f dB\n", ext);
        CHECK(std::fabs(ext - 6) < 0.05);
        rig.d->setMaster(D::Ext1ToMain, 0);
        // monitor knob law
        std::printf("  monitor knob: 11 -> %.2f dB, 5.5 -> %.2f dB, 0 -> off\n", 20 * std::log10(StudioDesk::monitorKnobGain(11)), 20 * std::log10(StudioDesk::monitorKnobGain(5.5f)));
        CHECK(StudioDesk::monitorKnobGain(0) == 0.0f && std::fabs(StudioDesk::monitorKnobGain(11) - 1.0f) < 1e-6f);
    }
    // ---- bus compressor ---------------------------------------------------------------------------
    {
        DeskRig rig;
        rig.d->setMaster(D::BusCompIn, 1);
        rig.d->setMaster(D::BusCompThreshold, -10.0f);   // -34 dBFS
        auto p = rig.mainProc();
        const double ratio = measuredRatio(p, [&] { rig.d->reset(); }, -24.0, -4.0);
        rig.d->reset();
        const Timing t = stepTiming(p, -60.0, -10.0, 1.0);
        std::printf("  bus comp: measured ratio %.2f:1 (spec 4:1); attack %.1f ms (spec 30), release %.1f ms (spec 100), %.1f dB GR\n", ratio, t.attackMs, t.releaseMs, t.grDb);
        CHECK(std::fabs(ratio - 4.0) < 0.25); CHECK(t.attackMs > 20 && t.attackMs < 40); CHECK(t.releaseMs > 70 && t.releaseMs < 140);
        rig.d->setMaster(D::BusCompAutoRelease, 1);
        rig.d->reset();
        const Timing burst = stepTiming(p, -60.0, -10.0, 0.06, kFs, 3.0);
        rig.d->reset();
        const Timing held = stepTiming(p, -60.0, -10.0, 4.0, kFs, 30.0);
        std::printf("  bus comp AUTO release: after a 60 ms burst %.0f ms, after 4 s of compression %.0f ms (dual rate: 100 ms / 12 s)\n", burst.releaseMs, held.releaseMs);
        CHECK(held.releaseMs > 5 * burst.releaseMs);
        rig.d->setMaster(D::BusCompAutoRelease, 0);
        // 50 Hz side-chain HPF: a 30 Hz tone triggers less gain reduction than a 1 kHz tone at the same level
        auto grFor = [&](double hz) { rig.d->reset(); Buf s = sine(hz, 1.0, -10); Buf o = rig.through(s, s, &DeskRig::mainL); return levelOf(s) - levelOf(o); };
        const double gr30 = grFor(30), gr1k = grFor(1000);
        std::printf("  bus comp side-chain HPF: GR on 30 Hz %.2f dB vs 1 kHz %.2f dB\n", gr30, gr1k);
        CHECK(gr30 < gr1k - 1.0);
        std::printf("  bus comp LEDs at 7 dB GR: %d of 5 lit; channel meter LEDs at +7 dBu: %d of 8\n", StudioDesk::busCompLeds(7), StudioDesk::channelMeterLeds(7));
        CHECK(StudioDesk::busCompLeds(7) == 3 && StudioDesk::channelMeterLeds(7) == 5);
        // CUE 1 TO BUS COMP & SUM: main channel muted to Bus B, cue 1 (pre) feeds the comp and lands on main
        rig.d->setMaster(D::BusCompIn, 0);
        rig.d->setChannel(0, D::MuteBusB, 1);
        rig.d->setChannel(0, D::Cue1On, 1); rig.d->setChannel(0, D::Cue1Level, 0.0f);
        rig.d->setMaster(D::Cue1ToBusComp, 1);
        Buf s = sine(1000, 0.4, -20);
        const double viaCue = levelOf(rig.through(s, s, &DeskRig::mainL)) - levelOf(s);
        std::printf("  ST CUE 1 TO BUS COMP & SUM TO MAIN: main receives the cue 1 mix at %.2f dB\n", viaCue);
        CHECK(std::fabs(viaCue) < 0.1);
    }
    // ---- zero allocation, everything switched on ---------------------------------------------------
    {
        DeskRig rig(kFs, 256);
        for (int k = 0; k < static_cast<int>(StudioDesk::kChannels); ++k)
            for (int p : {D::Hpf, D::EqIn, D::HfBell, D::CompIn, D::Insert, D::Cue1On, D::Cue2On, D::Cue2Post, D::Pfl, D::Mono})
                rig.d->setChannel(k, p, 1);
        rig.d->setMaster(D::BusCompIn, 1); rig.d->setMaster(D::BusCompAutoRelease, 1); rig.d->setMaster(D::Cue1ToBusComp, 1);
        Buf s = sine(440, 0.1, -10);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (int it = 0; it < 50; ++it) {
                for (int k = 0; k < 13; ++k) rig.d->setChannel(k % 13, D::HfGain, static_cast<float>(it % 15));
                rig.run(s.data(), s.data(), 256);
            }
            allocs = scope.count();
        }
        std::printf("  desk audio path, 50 blocks, all switches on, knobs moving: %llu heap allocations\n", static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0);
    }
}

// =================================================================================================
void testConsoleStepEq() {
    std::printf("[console: HEM STEP EQ]\n");
    namespace I = stepeq_ids;
    namespace Sp = spec::stepeq;
    auto eq = makeStepEq();
    eq->prepare(kFs, 512);
    Proc p = [&](float* l, float* r, std::uint32_t n) { eq->process(l, r, n); };
    auto setBand = [&](int ch, int fp, int fi, int gp, int gi) { eq->setDefaults(); eq->set(I::id(ch, fp), static_cast<float>(fi)); eq->set(I::id(ch, gp), static_cast<float>(gi)); eq->reset(); };
    // every band, every frequency, +12 dB x1: gain measured AT the marked frequency
    struct B { const char* name; int fp, gp; const float* hz; };
    const B bands[] = {{"LO", I::LoFreq, I::LoGain, Sp::kLoHz.data()}, {"LO-MID", I::LoMidFreq, I::LoMidGain, Sp::kLoMidHz.data()},
                       {"HI-MID", I::HiMidFreq, I::HiMidGain, Sp::kHiMidHz.data()}, {"HI", I::HiFreq, I::HiGain, Sp::kHiHz.data()}};
    for (const auto& b : bands) {
        std::string line;
        for (int f = 0; f < 7; ++f) {
            setBand(0, b.fp, f, b.gp, 10);
            const double g = gainDb(p, b.hz[f]);
            char t[48]; std::snprintf(t, sizeof t, " %g:%+.2f", static_cast<double>(b.hz[f]), g); line += t;
            if (b.hz[f] <= 12500.0f) CHECK(std::fabs(g - 12.0) < 0.3);
        }
        std::printf("  %-6s bell +12 dB at each marked freq (Hz:dB):%s\n", b.name, line.c_str());
    }
    // every gain step at 3 kHz (HI-MID)
    {
        std::string line;
        for (int gi = 0; gi < 11; ++gi) {
            setBand(0, I::HiMidFreq, 2, I::HiMidGain, gi);
            const double g = gainDb(p, 3000);
            char t[24]; std::snprintf(t, sizeof t, " %+.2f", g); line += t;
            CHECK(std::fabs(g - Sp::kGainDb[static_cast<std::size_t>(gi)]) < 0.15);
        }
        std::printf("  gain steps at 3 kHz (spec -12 -9 -6 -4 -2 0 +2 +4 +6 +9 +12):%s\n", line.c_str());
    }
    // RANGE: +12 at x1/x.5/x.25, and 6 dB @x1 has the SAME curve as 12 dB @x.5
    {
        double r[3];
        for (int ri = 0; ri < 3; ++ri) { setBand(0, I::HiMidFreq, 2, I::HiMidGain, 10); eq->set(I::id(0, I::Range), static_cast<float>(ri)); r[ri] = gainDb(p, 3000); }
        double maxDiff = 0;
        for (double hz : {300.0, 800.0, 1500.0, 2200.0, 3000.0, 4500.0, 7000.0, 12000.0}) {
            setBand(0, I::HiMidFreq, 2, I::HiMidGain, 8); const double a = gainDb(p, hz);                 // +6 x1
            setBand(0, I::HiMidFreq, 2, I::HiMidGain, 10); eq->set(I::id(0, I::Range), 1); const double c = gainDb(p, hz);   // +12 x.5
            maxDiff = std::max(maxDiff, std::fabs(a - c));
        }
        std::printf("  RANGE: +12 knob -> x1 %+.2f, x.5 %+.2f, x.25 %+.2f dB; 6 dB@x1 vs 12 dB@x.5 curve max diff %.3f dB\n", r[0], r[1], r[2], maxDiff);
        CHECK(std::fabs(r[1] - 6) < 0.15 && std::fabs(r[2] - 3) < 0.1 && maxDiff < 0.01);
    }
    // proportional Q: half-gain bandwidth narrows as the gain rises
    {
        auto bw = [&](int gi) {
            setBand(0, I::HiMidFreq, 2, I::HiMidGain, gi);
            const double peak = gainDb(p, 3000);
            double lo = 3000, hi = 3000;
            while (gainDb(p, lo) > peak / 2 && lo > 50) lo /= 1.02;
            while (gainDb(p, hi) > peak / 2 && hi < 20000) hi *= 1.02;
            return std::log2(hi / lo);
        };
        const double b2 = bw(6), b12 = bw(10);
        std::printf("  proportional Q: half-gain width at +2 dB %.2f octaves, at +12 dB %.2f octaves (Q %.2f -> %.2f, ASSUMED law)\n", b2, b12, stepEqQ(2), stepEqQ(12));
        CHECK(b12 < b2);
    }
    // reciprocity: +9 then -9 = flat
    {
        auto eq2 = makeStepEq();
        eq2->prepare(kFs, 512);
        setBand(0, I::LoMidFreq, 5, I::LoMidGain, 9);
        eq2->setDefaults(); eq2->set(I::id(0, I::LoMidFreq), 5); eq2->set(I::id(0, I::LoMidGain), 1);
        Proc both = [&](float* l, float* r, std::uint32_t n) { eq->process(l, r, n); eq2->process(l, r, n); };
        double mx = 0;
        for (double hz : {50.0, 200.0, 500.0, 700.0, 1000.0, 3000.0, 10000.0}) mx = std::max(mx, std::fabs(gainDb(both, hz)));
        std::printf("  reciprocity: +9 then -9 at 700 Hz -> max deviation %.4f dB\n", mx);
        CHECK(mx < 0.02);
    }
    // shelves, and the engineer's "+0.5 dB shelf at 10 kHz" (HI 10 kHz SHELF, +2 dB, RANGE x.25)
    {
        setBand(0, I::LoFreq, 3, I::LoGain, 10); eq->set(I::id(0, I::LoShelf), 1);
        const double l30 = gainDb(p, 30), l100 = gainDb(p, 100), l1k = gainDb(p, 1000);
        std::printf("  LO SHELF 100 Hz +12: 30 Hz %+.2f, 100 Hz %+.2f, 1 kHz %+.2f dB\n", l30, l100, l1k);
        CHECK(std::fabs(l100 - 12) < 0.6 && std::fabs(l30 - 12) < 0.2);
        setBand(0, I::HiFreq, 3, I::HiGain, 6); eq->set(I::id(0, I::HiShelf), 1); eq->set(I::id(0, I::Range), 2);
        const double h1 = gainDb(p, 1000), h5 = gainDb(p, 5000), h10 = gainDb(p, 10000), h16 = gainDb(p, 16000);
        std::printf("  HI SHELF 10 kHz +2 dB x.25 (= +0.5 dB): 1 kHz %+.3f, 5 kHz %+.3f, 10 kHz %+.3f, 16 kHz %+.3f dB\n", h1, h5, h10, h16);
        CHECK(std::fabs(h10 - 0.5) < 0.05);
    }
    // BYPASS and IN are exact; channel 2 is independent
    {
        setBand(0, I::HiMidFreq, 2, I::HiMidGain, 10);
        Buf l = sine(3000, 0.05, -20), r = l, l0 = l;
        eq->set(I::Bypass, 1); eq->process(l.data(), r.data(), static_cast<std::uint32_t>(l.size()));
        const bool bypassExact = l == l0;
        eq->set(I::Bypass, 0); eq->set(I::id(0, I::In), 0); l = l0; eq->process(l.data(), r.data(), static_cast<std::uint32_t>(l.size()));
        const bool inExact = l == l0;
        eq->set(I::id(0, I::In), 1);
        const double right = gainDb(p, 3000, kFs, -20, 1);
        std::printf("  BYPASS bit-exact %s, IN off bit-exact %s, channel 2 untouched by channel 1 (+12): %+.3f dB\n", bypassExact ? "yes" : "NO", inExact ? "yes" : "NO", right);
        CHECK(bypassExact && inExact && std::fabs(right) < 0.01);
    }
    // zero allocation
    {
        Buf l = sine(1000, 0.01, -10), r = l;
        AllocScope scope;
        for (int it = 0; it < 200; ++it) { eq->set(I::id(it & 1, I::HiGain), static_cast<float>(it % 11)); eq->process(l.data(), r.data(), 256); }
        std::printf("  step EQ audio path, 200 blocks, knobs moving: %llu heap allocations\n", static_cast<unsigned long long>(scope.count()));
        CHECK(scope.count() == 0);
    }
}

// =================================================================================================
void testConsoleTwinBus() {
    std::printf("[console: HEM TWIN BUS]\n");
    namespace I = twinbus_ids;
    auto tb = makeTwinBus();
    tb->prepare(kFs, 512);
    const std::uint32_t lat = tb->latency();
    // Undo the fixed latency so level-vs-time measurements line up.
    Proc p = [&](float* l, float* r, std::uint32_t n) { tb->process(l, r, n); };
    auto fresh = [&] { tb->setDefaults(); tb->reset(); };
    auto set = [&](int side, int prm, float v) { tb->set(I::id(side, prm), v); };
    std::printf("  latency %u samples (%.2f ms at 48 kHz), constant with GRIT on or off\n", lat, 1000.0 * lat / kFs);

    // ratios: every position, measured 10..20 dB above the threshold
    {
        std::string line;
        for (int ri = 0; ri < 11; ++ri) {
            fresh();
            set(0, I::Threshold, 0);        // -20 dB -> -44 dBFS
            set(0, I::Ratio, static_cast<float>(ri));
            set(0, I::Attack, 3);           // 1 ms
            set(0, I::Release, 8);          // 1.2 s
            const double r = measuredRatio(p, [&] { tb->reset(); }, -34.0, -24.0);
            char t[24]; std::snprintf(t, sizeof t, " %.2f", r); line += t;
            const float spec = spec::twinbus::kRatio[static_cast<std::size_t>(ri)];
            CHECK(std::fabs(r - spec) < std::max(0.12, 0.12 * std::fabs(spec)));
        }
        std::printf("  ratio positions measured (spec 1.3 1.5 2 3 4 5 10 20 -2.5 -1.5 -0.5):%s\n", line.c_str());
    }
    // attack and release table, and AUTO / AUTO 2
    {
        std::string a, r;
        for (int ai : {4, 6, 8, 10}) {   // 3, 10, 20, 40 ms
            fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 6); set(0, I::Attack, static_cast<float>(ai)); set(0, I::Release, 0);
            const Timing t = stepTiming(p, -70, -14, 0.6, kFs, 2.0, lat);
            char s[40]; std::snprintf(s, sizeof s, " %gms->%.1f", static_cast<double>(spec::twinbus::kAttackMs[static_cast<std::size_t>(ai)]), t.attackMs); a += s;
        }
        for (int ri : {0, 2, 5, 8}) {    // 0.05, 0.15, 0.4, 1.2 s
            fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 6); set(0, I::Attack, 3); set(0, I::Release, static_cast<float>(ri));
            const Timing t = stepTiming(p, -70, -14, 0.6, kFs, 4.0, lat);
            char s[40]; std::snprintf(s, sizeof s, " %gs->%.0fms", static_cast<double>(spec::twinbus::kReleaseS[static_cast<std::size_t>(ri)]), t.releaseMs); r += s;
            CHECK(std::fabs(t.releaseMs - 1000.0 * spec::twinbus::kReleaseS[static_cast<std::size_t>(ri)]) < 0.25 * 1000.0 * spec::twinbus::kReleaseS[static_cast<std::size_t>(ri)] + 3);
        }
        std::printf("  attack (set->measured):%s\n  release (set->measured):%s\n", a.c_str(), r.c_str());
        for (int ri : {9, 10}) {
            fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 6); set(0, I::Attack, 3); set(0, I::Release, static_cast<float>(ri));
            const Timing b = stepTiming(p, -70, -14, 0.06, kFs, 3.0, lat);
            fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 6); set(0, I::Attack, 3); set(0, I::Release, static_cast<float>(ri));
            const Timing h = stepTiming(p, -70, -14, 4.0, kFs, 30.0, lat);
            std::printf("  %s: release after 60 ms burst %.0f ms, after 4 s of compression %.0f ms\n", ri == 9 ? "AUTO  " : "AUTO 2", b.releaseMs, h.releaseMs);
            CHECK(h.releaseMs > 5 * b.releaseMs);
        }
    }
    // MAKE-UP coarse / fine, MIX classic / parallel
    {
        fresh(); set(0, I::Threshold, 30); set(0, I::Makeup, 15);   // +5 dB, no compression (threshold +20 dB)
        const double mk = gainDb(p, 1000, kFs, -30);
        set(0, I::Fine, 1);                                          // fine: -5 + 7.5 = +2.5 dB
        const double mkF = gainDb(p, 1000, kFs, -30);
        std::printf("  MAKE-UP index 15: coarse %+.2f dB (spec +5), fine %+.2f dB (spec +2.5)\n", mk, mkF);
        CHECK(std::fabs(mk - 5) < 0.05 && std::fabs(mkF - 2.5) < 0.05);
        fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 7); set(0, I::Mix, 15);
        const double classic = gainDb(p, 1000, kFs, -10);
        tb->set(I::MixMode, 1);
        const double parallel = gainDb(p, 1000, kFs, -10);
        set(0, I::Mix, 30);
        const double audition = gainDb(p, 1000, kFs, -10);
        set(0, I::Mix, 0);
        const double dryOnly = gainDb(p, 1000, kFs, -10);
        std::printf("  MIX 50%%: classic %+.2f dB, parallel %+.2f dB; parallel WET (audition) %+.2f dB; DRY %+.2f dB\n", classic, parallel, audition, dryOnly);
        CHECK(parallel > classic && std::fabs(dryOnly) < 0.05 && audition < -10);
    }
    // MODES: Σ S/C ignores pure side content; DUAL MONO is independent; MID-SIDE solos
    {
        auto grFor = [&](int mode, bool sideOnly) {
            fresh(); tb->set(I::Mode, static_cast<float>(mode)); set(0, I::Threshold, 0); set(0, I::Ratio, 6);
            Buf l = sine(1000, 0.6, -10), r = l;
            if (sideOnly) for (auto& v : r) v = -v;
            const Buf l0 = l;
            runBlocks(*tb, l, r);
            return rmsDb(l0, l0.size() / 2, l0.size()) - rmsDb(l, l.size() / 2, l.size());
        };
        const double classicSide = grFor(0, true), sumSide = grFor(1, true), sumMono = grFor(1, false);
        std::printf("  CLASSIC STEREO on L=-R: %.2f dB GR; SUM S/C on L=-R: %.2f dB GR; SUM S/C on L=R: %.2f dB GR\n", classicSide, sumSide, sumMono);
        CHECK(classicSide > 10 && sumSide < 0.1 && sumMono > 10);
        fresh(); tb->set(I::Mode, 2); set(0, I::Threshold, 0); set(0, I::Ratio, 6); set(1, I::Threshold, 30);
        const double dL = gainDb(p, 1000, kFs, -10, 0), dR = gainDb(p, 1000, kFs, -10, 1);
        std::printf("  DUAL MONO: left (threshold -20) %+.2f dB, right (threshold +20) %+.2f dB\n", dL, dR);
        CHECK(dL < -10 && std::fabs(dR) < 0.05);
        fresh(); tb->set(I::Mode, 3); tb->set(I::MidSolo, 1);
        Buf l = sine(1000, 0.2, -20), r(l.size(), 0.0f);   // L only: M = S = L/2
        tb->process(l.data(), r.data(), static_cast<std::uint32_t>(l.size()));
        const double midL = rmsDb(l, l.size() / 2, l.size()), midR = rmsDb(r, r.size() / 2, r.size());
        std::printf("  MID-SIDE mid solo, L-only input: out L %.2f / R %.2f dBFS (both = mid, -6 dB)\n", midL, midR);
        CHECK(std::fabs(midL - midR) < 0.01 && std::fabs(midL - (-20 - 3.0103 - 6.0206)) < 0.1);
    }
    // LOW THD with the guide's own test settings (threshold 0 dB, 10:1, 0.1 ms, 0.1 s; +4 dBu = -20 dBFS)
    {
        auto thd = [&](bool lowThd, double hz) {
            fresh(); set(0, I::Threshold, 15); set(0, I::Ratio, 6); set(0, I::Attack, 0); set(0, I::Release, 1); set(0, I::LowThd, lowThd ? 1.0f : 0.0f);
            Buf l = sine(hz, 1.5, -20), r = l;
            runBlocks(*tb, l, r);
            return thdPercent(l, hz, static_cast<std::size_t>(kFs * 0.5), static_cast<std::size_t>(kFs));
        };
        const double off50 = thd(false, 50), on50 = thd(true, 50), off1k = thd(false, 1000), on1k = thd(true, 1000);
        std::printf("  LOW THD (guide's test settings): THD at 50 Hz %.2f%% -> %.2f%%, at 1 kHz %.3f%% -> %.3f%%\n", off50, on50, off1k, on1k);
        CHECK(on50 < off50 * 0.5);
    }
    // F/B: feedback detection gives a gentler effective ratio
    {
        auto rr = [&](bool fb) { fresh(); set(0, I::Threshold, 0); set(0, I::Ratio, 4); set(0, I::Attack, 3); set(0, I::Release, 8); set(0, I::Feedback, fb ? 1.0f : 0.0f);
                                 return measuredRatio(p, [&] { tb->reset(); }, -34.0, -24.0); };
        const double ff = rr(false), fb = rr(true);
        std::printf("  F/B: ratio knob 4:1 measures %.2f:1 feed-forward, %.2f:1 with F/B (more relaxed)\n", ff, fb);
        CHECK(fb < ff);
    }
    // GRIT levels: harmonic distortion rises with the SET DIST level; no change in level when off
    {
        std::string line;
        double prev = -1;
        for (int lv : {0, 4, 8}) {
            fresh(); set(0, I::Threshold, 30); set(0, I::Grit, 1); set(0, I::GritLevel, static_cast<float>(lv));
            Buf l = sine(1000, 0.6, -10), r = l;
            runBlocks(*tb, l, r);
            const double t = thdPercent(l, 1000, static_cast<std::size_t>(kFs * 0.2), static_cast<std::size_t>(kFs * 0.2));
            char s[32]; std::snprintf(s, sizeof s, " L%d:%.2f%%", lv + 1, t); line += s;
            CHECK(t > prev); prev = t;
        }
        fresh(); set(0, I::Threshold, 30);
        const double clean = gainDb(p, 1000, kFs, -10);
        std::printf("  GRIT THD at -10 dBFS:%s; GRIT off, no compression: %+.3f dB\n", line.c_str(), clean);
        CHECK(std::fabs(clean) < 0.02);
        // latency identical with GRIT off and on (impulse arrives at the same sample)
        auto impulseAt = [&](bool grit) {
            fresh(); set(0, I::Threshold, 30); set(0, I::Grit, grit ? 1.0f : 0.0f); set(0, I::GritLevel, 0);
            Buf l(256, 0.0f), r(256, 0.0f); l[10] = r[10] = 0.5f;
            tb->process(l.data(), r.data(), 256);
            return static_cast<int>(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - l.begin());
        };
        const int a = impulseAt(false), b = impulseAt(true);
        std::printf("  impulse at sample 10 comes out at %d (GRIT off) and %d (GRIT on); latency() = %u\n", a, b, lat);
        CHECK(a == b && a == 10 + static_cast<int>(lat));
    }
    // D-EQ: static LF/HF gain, dynamic comp / exp with RANGE, bell Q, RESO, transient expander, key input
    // (the compressor is parked at threshold +20 dB so only the D-EQ acts)
    {
        auto freshEq = [&] { fresh(); set(0, I::Threshold, 30); set(1, I::Threshold, 30); };
        freshEq(); set(0, I::LfOn, 1); set(0, I::LfGain, 30);   // +10 dB static, 60 Hz 1st-order shelf
        const double s20 = gainDb(p, 20), s60 = gainDb(p, 60), s1k = gainDb(p, 1000);
        std::printf("  D-EQ LF static +10 dB @60 Hz shelf: 20 Hz %+.2f, 60 Hz %+.2f, 1 kHz %+.2f dB\n", s20, s60, s1k);
        CHECK(s20 > 7 && std::fabs(s1k) < 0.6);
        std::printf("  LF/HF GAIN knob law (ASSUMED steps on printed marks): idx 19 %+.1f, 24 %+.1f, 30 %+.1f dB\n",
                    twinBusStaticGainDb(19), twinBusStaticGainDb(24), twinBusStaticGainDb(30));
        auto lfDyn = [&](int ce, int range, double lvl) { freshEq(); set(0, I::LfOn, 1); set(0, I::LfCompExp, static_cast<float>(ce)); set(0, I::LfRange, static_cast<float>(range)); return gainDb(p, 40, kFs, lvl); };
        const double compLoud = lfDyn(0, 22, -6), compQuiet = lfDyn(0, 22, -60), compRange3 = lfDyn(0, 5, -6), expLoud = lfDyn(30, 22, -6);
        std::printf("  D-EQ LF COMP full: 40 Hz loud %+.2f dB, quiet %+.2f dB; RANGE 3 dB loud %+.2f dB; EXP full loud %+.2f dB\n", compLoud, compQuiet, compRange3, expLoud);
        CHECK(compLoud < -3 && std::fabs(compQuiet) < 0.3 && compRange3 > -3.3 && expLoud > 3);
        freshEq(); tb->set(I::Mode, 0); set(0, I::HfOn, 1); set(0, I::HfBell, 1); set(0, I::HfFreq, 7); set(1, I::LfGain, 30);   // 4 kHz bell +10 static
        const double b4 = gainDb(p, 4000);
        double lo = 4000, hi = 4000;
        while (gainDb(p, lo) > b4 - 3.0103 && lo > 100) lo /= 1.01;
        while (gainDb(p, hi) > b4 - 3.0103 && hi < 20000) hi *= 1.01;
        std::printf("  D-EQ HF BELL 4 kHz, +10 dB (right LF/HF GAIN in stereo): peak %+.2f dB, -3 dB points %.0f / %.0f Hz (Q target 1.87 at 10 dB)\n", b4, lo, hi);
        CHECK(std::fabs(b4 - 10) < 0.3);
        freshEq(); set(0, I::LfOn, 1); set(0, I::LfGain, 30); set(0, I::Reso, 1);
        double peak = -100; for (double hz = 20; hz < 1000; hz *= 1.05) peak = std::max(peak, gainDb(p, hz));
        std::printf("  RESO (2nd-order LF, +10 dB): highest gain %+.2f dB (overshoot above +10)\n", peak);
        CHECK(peak > 10.2);
        // transient expander: a kick-like burst gets a louder front
        auto front = [&](bool on) {
            freshEq(); set(0, I::TransientExp, on ? 1.0f : 0.0f); set(0, I::HfCompExp, 30);
            Buf l(static_cast<std::size_t>(kFs), 0.0f);
            for (int k = 0; k < 4; ++k) for (std::size_t i = 0; i < 6000; ++i) l[static_cast<std::size_t>(k) * 12000 + i] = static_cast<float>(0.3 * std::exp(-static_cast<double>(i) / 1500.0) * std::sin(2 * std::numbers::pi * 120 * static_cast<double>(i) / kFs));
            Buf r = l;
            runBlocks(*tb, l, r);
            const std::size_t s0 = 3 * 12000 + lat;
            return rmsDb(l, s0, s0 + 480) - rmsDb(l, s0 + 3000, s0 + 4000);
        };
        const double off = front(false), on = front(true);
        std::printf("  transient expander: attack-to-tail level %.2f dB -> %.2f dB\n", off, on);
        CHECK(on > off + 1.0);
        // external key: a loud key compresses a quiet programme
        fresh(); tb->set(I::ExtSc, 1); set(0, I::Threshold, 0); set(0, I::Ratio, 6);
        Buf l = sine(1000, 0.6, -40), r = l, key = sine(200, 0.6, -6);
        for (std::size_t i = 0; i < l.size(); i += 256) { const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(256, l.size() - i)); twinBusSetKey(*tb, key.data() + i, key.data() + i); tb->process(l.data() + i, r.data() + i, n); }
        const double keyed = rmsDb(l, l.size() / 2, l.size()) - (-40 - 3.0103);
        std::printf("  EXT S/C key (-6 dBFS) on a -40 dBFS programme: %+.2f dB\n", keyed);
        CHECK(keyed < -10);
    }
    // 96 kHz: the 26 kHz input LPF becomes real
    {
        auto t96 = makeTwinBus();
        t96->prepare(96000.0, 512);
        t96->set(I::Lpf, 1); t96->set(I::id(0, I::Threshold), 30);
        Proc q = [&](float* l, float* r, std::uint32_t n) { t96->process(l, r, n); };
        const double g26 = gainDb(q, 26000, 96000.0), g10 = gainDb(q, 10000, 96000.0), g40 = gainDb(q, 40000, 96000.0);
        std::printf("  96 kHz input LPF: 10 kHz %+.2f, 26 kHz %+.2f, 40 kHz %+.2f dB (3rd order)\n", g10, g26, g40);
        CHECK(std::fabs(g26 + 3.0) < 0.5 && g40 < -10 && std::fabs(g10) < 0.1);
    }
    // zero allocation, worst case (both bands, bell, grit, key, mode changes)
    {
        fresh();
        for (int s = 0; s < 2; ++s) for (int prm : {I::LfOn, I::HfOn, I::HfBell, I::Grit, I::LowThd, I::Reso}) set(s, prm, 1);
        Buf l = sine(1000, 0.01, -10), r = l, key = l;
        AllocScope scope;
        for (int it = 0; it < 200; ++it) {
            tb->set(I::Mode, static_cast<float>(it % 4));
            set(0, I::HfCompExp, static_cast<float>(it % 31));
            twinBusSetKey(*tb, key.data(), key.data());
            tb->process(l.data(), r.data(), 256);
        }
        std::printf("  twin bus audio path, 200 blocks, every mode, knobs moving: %llu heap allocations\n", static_cast<unsigned long long>(scope.count()));
        CHECK(scope.count() == 0);
    }
}


// =================================================================================================
void testConsoleColour() {
    std::printf("[console: HEM COLOUR STAGE]\n");
    namespace I = colour_ids;
    namespace Sp = spec::colour;
    auto cs = makeColourStage();
    cs->prepare(kFs, 512);
    const std::uint32_t lat = cs->latency();
    Proc p = [&](float* l, float* r, std::uint32_t n) { cs->process(l, r, n); };
    auto fresh = [&] { cs->setDefaults(); cs->reset(); };
    std::printf("  latency %u samples (%.2f ms at 48 kHz), the same with every section in or out\n", lat, 1000.0 * lat / kFs);
    // all circuits off: flat (reference spec: +/-0.05 dB)
    {
        fresh();
        double mx = 0;
        for (double hz : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) mx = std::max(mx, std::fabs(gainDb(p, hz)));
        std::printf("  all circuits off: max deviation 20 Hz-20 kHz %.3f dB (reference spec +/-0.05 dB)\n", mx);
        CHECK(mx < 0.05);
    }
    // HPF OFF / 30 / 40 / 50 Hz, 18 dB/oct
    {
        std::string line;
        for (int h = 1; h <= 3; ++h) {
            fresh(); cs->set(I::Hpf, static_cast<float>(h));
            const double hz = Sp::kHpfHz[static_cast<std::size_t>(h - 1)];
            const double atF = gainDb(p, hz), oct = gainDb(p, hz / 2) - gainDb(p, hz / 4);
            char t[64]; std::snprintf(t, sizeof t, " %g Hz: %.2f dB at %g Hz, %.1f dB/oct below;", hz, atF, hz, oct); line += t;
            CHECK(std::fabs(atF + 3.01) < 0.2 && oct > 16.5);
        }
        std::printf("  HPF:%s\n", line.c_str());
    }
    // TONE EQ vs the maker's published curves (+9 dB)
    {
        struct C { const char* name; const GraphPt* pts; std::size_t n; int fp, gp, fi; };
        const C cases[] = {
            {"LOW 30 Hz", kToneLow30, std::size(kToneLow30), I::LowFreq, I::LowGain, 0}, {"LOW 50 Hz", kToneLow50, std::size(kToneLow50), I::LowFreq, I::LowGain, 1},
            {"LOW 70 Hz", kToneLow70, std::size(kToneLow70), I::LowFreq, I::LowGain, 2}, {"LOW 90 Hz", kToneLow90, std::size(kToneLow90), I::LowFreq, I::LowGain, 3},
            {"HIGH 8 kHz", kToneHigh8, std::size(kToneHigh8), I::HighFreq, I::HighGain, 0}, {"HIGH 12 kHz", kToneHigh12, std::size(kToneHigh12), I::HighFreq, I::HighGain, 1},
            {"HIGH 16 kHz", kToneHigh16, std::size(kToneHigh16), I::HighFreq, I::HighGain, 2}, {"HIGH 20 kHz", kToneHigh20, std::size(kToneHigh20), I::HighFreq, I::HighGain, 3},
        };
        for (const auto& c : cases) {
            fresh(); cs->set(I::EqIn, 1); cs->set(c.fp, static_cast<float>(c.fi)); cs->set(c.gp, 9.0f);
            std::vector<double> err;
            for (std::size_t i = 0; i < c.n; ++i) err.push_back(gainDb(p, c.pts[i].hz) - c.pts[i].db);
            double mean = 0; for (double e : err) mean += e; mean /= static_cast<double>(err.size());
            double se = 0, sh = 0, mx = 0, mxs = 0;
            for (double e : err) { se += e * e; sh += (e - mean) * (e - mean); mx = std::max(mx, std::fabs(e)); mxs = std::max(mxs, std::fabs(e - mean)); }
            const double rms = std::sqrt(se / static_cast<double>(c.n)), shape = std::sqrt(sh / static_cast<double>(c.n));
            std::printf("  TONE EQ %-11s +9 vs published curve: raw rms %.2f / max %.2f dB; shape (graph level offset %+.2f dB removed) rms %.2f / max %.2f dB (%zu pts)\n",
                        c.name, rms, mx, -mean, shape, mxs, c.n);
            CHECK(shape < 0.3 && rms < 0.8);
        }
        fresh(); cs->set(I::EqIn, 1); cs->set(I::LowGain, 9); const double up = gainDb(p, 30);
        cs->set(I::LowGain, -9); const double dn = gainDb(p, 30);
        std::printf("  TONE LOW 30 Hz at 30 Hz: +9 knob %+.2f dB, -9 knob %+.2f dB (mirror err %.3f dB)\n", up, dn, up + dn);
        CHECK(std::fabs(up + dn) < 0.05);
    }
    // DRIVE vs the maker's 1 kHz spectra (test conditions ASSUMED: drive 5, -10 dBFS)
    {
        auto harmonics = [&](float density, float drive, double inDb, double& h2, double& h3, float& thd) {
            fresh(); cs->set(I::DriveIn, 1); cs->set(I::Drive, drive); cs->set(I::Density, density);
            Buf l = sine(1000, 0.5, inDb), r = l;
            runBlocks(*cs, l, r);
            thd = cs->meter(I::MeterDriveThd);
            const std::size_t from = static_cast<std::size_t>(kFs * 0.2), len = static_cast<std::size_t>(kFs * 0.2);
            auto mag = [&](double hz) { std::complex<double> a{0, 0}; for (std::size_t i = 0; i < len; ++i) a += static_cast<double>(l[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kFs); return std::abs(a); };
            const double f1 = mag(1000);
            h2 = 20 * std::log10(mag(2000) / f1); h3 = 20 * std::log10(mag(3000) / f1);
        };
        double h2, h3; float thd;
        harmonics(2.0f, 5.0f, -10.0, h2, h3, thd);
        std::printf("  DRIVE density 2: H2 %.1f dBc (graph -27.5), H3 %.1f dBc (graph -38.0); LED %s (%.2f%%)\n", h2, h3,
                    std::array<const char*, 4>{"off", "green", "orange", "red"}[static_cast<std::size_t>(colourDriveLedColour(thd))], thd);
        CHECK(std::fabs(h2 + 27.5) < 1.5 && std::fabs(h3 + 38.0) < 1.5);
        harmonics(8.0f, 5.0f, -10.0, h2, h3, thd);
        std::printf("  DRIVE density 8: H2 %.1f dBc (graph -64.2), H3 %.1f dBc (graph -54.8); LED %s (%.2f%%)\n", h2, h3,
                    std::array<const char*, 4>{"off", "green", "orange", "red"}[static_cast<std::size_t>(colourDriveLedColour(thd))], thd);
        CHECK(std::fabs(h3 + 54.8) < 2.0 && h3 > h2);
        std::string line;
        for (float dr : {1.0f, 5.0f, 9.0f, 11.0f}) { harmonics(5.0f, dr, -14.0, h2, h3, thd); char t[48]; std::snprintf(t, sizeof t, " drive %.0f: %.2f%%;", dr, thd); line += t; }
        std::printf("  DRIVE distortion vs DRIVE (density 5, -14 dBFS):%s\n", line.c_str());
        // auto level compensation: small signal level barely moves with DRIVE
        fresh(); cs->set(I::DriveIn, 1); cs->set(I::Density, 5); cs->set(I::Drive, 1); const double g1 = gainDb(p, 1000, kFs, -40);
        cs->set(I::Drive, 11); const double g11 = gainDb(p, 1000, kFs, -40);
        std::printf("  DRIVE auto level compensation: quiet tone %+.2f dB at drive 1, %+.2f dB at drive 11\n", g1, g11);
        CHECK(std::fabs(g1 - g11) < 1.0);
        // hiss: silent input
        fresh(); cs->set(I::DriveIn, 1); cs->set(I::Drive, 6); cs->set(I::Density, 5);
        Buf z(static_cast<std::size_t>(kFs), 0.0f), z2 = z;
        runBlocks(*cs, z, z2);
        const double hissDbu = rmsDb(z, z.size() / 2, z.size()) + Sp::kDbuAtFullScale;
        std::printf("  DRIVE hiss, mid drive / mid density: %.1f dBu (reference spec -75 dBu typical)\n", hissDbu);
        CHECK(std::fabs(hissDbu - Sp::kHissDbu) < 2.0);
    }
    // HF COMPRESSOR: only the top above X-OVER moves; no make-up
    {
        fresh(); cs->set(I::HfcIn, 1); cs->set(I::HfcThreshold, -10.0f); cs->set(I::HfcXover, 8000.0f);
        const double g1k = gainDb(p, 1000, kFs, -6), g15k = gainDb(p, 15000, kFs, -6);
        cs->reset();
        const double quiet15k = gainDb(p, 15000, kFs, -60);
        std::printf("  HF COMPRESSOR (threshold -10, X-OVER 8 kHz): loud 1 kHz %+.2f dB, loud 15 kHz %+.2f dB, quiet 15 kHz %+.2f dB\n", g1k, g15k, quiet15k);
        CHECK(std::fabs(g1k) < 0.6 && g15k < -3.0 && std::fabs(quiet15k) < 0.1);
        // Crush: X-OVER becomes WET/DRY
        fresh(); cs->set(I::HfcIn, 1); cs->set(I::HfcCrush, 1); cs->set(I::HfcThreshold, -10.0f);
        cs->set(I::HfcXover, Sp::kHfcXoverMinHz); const double dry = gainDb(p, 1000, kFs, -6);
        cs->set(I::HfcXover, Sp::kHfcXoverMaxHz);
        const double wetLoud = gainDb(p, 1000, kFs, -6), wetQuiet = gainDb(p, 1000, kFs, -40);
        std::printf("  CRUSH: knob at DRY %+.2f dB; WET loud (-6 dBFS) %+.2f dB vs quiet (-40 dBFS) %+.2f dB (heavy squash)\n", dry, wetLoud, wetQuiet);
        CHECK(std::fabs(dry) < 0.05 && wetQuiet - wetLoud > 15);
    }
    // STEREO IMAGE: WIDTH moves only the side; SPACE is a broad bass shelf on the side
    {
        auto sideGain = [&](float width, float space, double hz, bool sideOnly) {
            fresh(); cs->set(I::ImageIn, 1); cs->set(I::Width, width); cs->set(I::Space, space);
            Buf l = sine(hz, 0.5, -20), r = l;
            if (sideOnly) for (auto& v : r) v = -v;
            runBlocks(*cs, l, r);
            return rmsDb(l, l.size() / 2, l.size()) - (-20 - 3.0103);
        };
        const double wSide = sideGain(6, 0, 1000, true), wMid = sideGain(6, 0, 1000, false), spLow = sideGain(0, 12, 60, true), spHigh = sideGain(0, 12, 8000, true);
        std::printf("  STEREO IMAGE: WIDTH +6 -> side %+.2f dB, mid %+.2f dB; SPACE +12 on side -> 60 Hz %+.2f dB, 8 kHz %+.2f dB\n", wSide, wMid, spLow, spHigh);
        CHECK(std::fabs(wSide - 6) < 0.05 && std::fabs(wMid) < 0.05 && spLow > 9 && std::fabs(spHigh) < 1.0);
    }
    // TRANSFORMER: low-end roll-off grows with level (+16 dBu = -8 dBFS)
    {
        // dBu is an RMS level: a sine at +16 dBu RMS peaks 3 dB higher
        auto tx = [&](double hz, double dbu) { fresh(); cs->set(I::Transformer, 1); return gainDb(p, hz, kFs, dbu + 3.0103 - Sp::kDbuAtFullScale); };
        const double a10 = tx(10, 16), a20 = tx(20, 16), a30 = tx(30, 16), a40 = tx(40, 16), a1k = tx(1000, 16), q20 = tx(20, -10);
        std::printf("  TRANSFORMER at +16 dBu: 10 Hz %+.2f (graph -10.1), 20 Hz %+.2f (graph -3.8), 30 Hz %+.2f (graph -0.9), 40 Hz %+.2f (graph -0.2), 1 kHz %+.2f dB; 20 Hz at -10 dBu %+.2f dB\n",
                    a10, a20, a30, a40, a1k, q20);
        CHECK(std::fabs(a20 + 3.8) < 0.8 && std::fabs(a30 + 0.9) < 0.5 && q20 > a20 && std::fabs(a1k) < 0.5);
    }
    // INSERT (stereo and M/S), BYPASS, O/L, meter
    {
        struct Probe final : InsertSlot {
            double l = 0, r = 0; int calls = 0;
            void process(float* a, float* b, std::uint32_t n) noexcept override { ++calls; for (std::uint32_t i = 0; i < n; ++i) { l += std::fabs(a[i]); r += std::fabs(b[i]); } }
        } probe;
        fresh(); colourStageSetInsert(*cs, &probe); cs->set(I::Insert, 1); cs->set(I::InsertMs, 1);
        Buf l = sine(1000, 0.1, -20), r = l;   // mono: Mid = signal, Side = 0
        runBlocks(*cs, l, r);
        std::printf("  INSERT M/S mode: slot called %d times, left (Mid) energy %.1f, right (Side) energy %.4f\n", probe.calls, probe.l, probe.r);
        CHECK(probe.calls > 0 && probe.l > 100 && probe.r < 1e-3);
        colourStageSetInsert(*cs, nullptr);
        fresh(); cs->set(I::InputTrim, 6); cs->set(I::DriveIn, 1); cs->set(I::Bypass, 1);
        Buf a(4800, 0.0f), b(4800, 0.0f); a[100] = b[100] = 0.5f;
        runBlocks(*cs, a, b);
        const bool exact = std::fabs(a[100 + lat] - 0.5f) < 1e-6f;
        cs->set(I::BypassPostTrim, 1); cs->reset();
        Buf c(4800, 0.0f), d(4800, 0.0f); c[100] = d[100] = 0.5f;
        runBlocks(*cs, c, d);
        std::printf("  BYPASS: impulse unchanged after %u samples %s; POST I/P TRIM (+6 dB) gives %.3f (expect %.3f)\n", lat, exact ? "yes" : "NO", c[100 + lat], 0.5 * std::pow(10.0, 6.0 / 20));
        CHECK(exact && std::fabs(c[100 + lat] - 0.5f * dbToLin(6.0f)) < 1e-3f);
        fresh();
        Buf hot = sine(1000, 0.1, 27.5 - Sp::kDbuAtFullScale), hot2 = hot;
        runBlocks(*cs, hot, hot2);
        const float ol = cs->meter(I::MeterOverL), m = cs->meter(I::MeterHoldL);
        std::printf("  O/L at +27.5 dBu: %s; meter hold reads %.1f dBu\n", ol > 0.5f ? "lit" : "NOT lit", m);
        CHECK(ol > 0.5f && std::fabs(m - 27.5f) < 0.2f);
    }
    // zero allocation
    {
        fresh();
        for (int prm : {I::DriveIn, I::EqIn, I::HfcIn, I::ImageIn, I::Transformer}) cs->set(prm, 1);
        Buf l = sine(1000, 0.01, -10), r = l;
        AllocScope scope;
        for (int it = 0; it < 200; ++it) { cs->set(I::HfcCrush, static_cast<float>(it & 1)); cs->set(I::LowGain, static_cast<float>(it % 9)); cs->process(l.data(), r.data(), 256); }
        std::printf("  colour stage audio path, 200 blocks, every section on, knobs moving: %llu heap allocations\n", static_cast<unsigned long long>(scope.count()));
        CHECK(scope.count() == 0);
    }
}

// =================================================================================================
// =============================================================================================
//  HEM METER BRIDGE — checked against the published test signals of EBU Tech 3341 / 3342 and BS.1770-4.
// =============================================================================================
void testConsoleMeter() {
    std::printf("[console meter] HEM METER BRIDGE (loudness / true peak / stereo / RTA)\n");
    // Feed a stereo signal made of (level dBFS, seconds) segments of a sine, in 512-frame blocks.
    struct Seg { double db, seconds; };
    auto feed = [](MeterBridge& m, const std::vector<Seg>& segs, double fs = kFs, double hz = 1000.0, double rGain = 1.0, double rHz = 0.0) {
        Buf l(512), r(512);
        std::size_t t = 0;
        for (const Seg& sg : segs) {
            const std::size_t n = static_cast<std::size_t>(sg.seconds * fs);
            const double a = sg.db <= -200 ? 0.0 : std::pow(10.0, sg.db / 20.0);
            for (std::size_t done = 0; done < n;) {
                const std::size_t k = std::min<std::size_t>(512, n - done);
                for (std::size_t i = 0; i < k; ++i, ++t) {
                    const double ph = 2 * std::numbers::pi * static_cast<double>(t) / fs;
                    l[i] = static_cast<float>(a * std::sin(hz * ph));
                    r[i] = static_cast<float>(rGain * a * std::sin((rHz > 0 ? rHz : hz) * ph));
                }
                m.process(l.data(), r.data(), static_cast<std::uint32_t>(k));
                done += k;
            }
        }
    };
    auto m = std::make_unique<MeterBridge>();
    // --- EBU Tech 3341 cases 1, 2 (stereo 1 kHz, 20 s): M, S and I must read the level within 0.1 LU ---
    for (double fs : {48000.0, 96000.0}) {
        for (double db : {-23.0, -33.0}) {
            m->prepare(fs, 512);
            feed(*m, {{db, 20.0}}, fs);
            std::printf("  1 kHz stereo %.0f dBFS @ %.0f kHz: momentary %.2f, short-term %.2f, integrated %.2f LUFS\n",
                        db, fs / 1000, m->momentaryLufs(), m->shortTermLufs(), m->integratedLufs());
            CHECK(std::fabs(m->momentaryLufs() - db) < 0.1 && std::fabs(m->shortTermLufs() - db) < 0.1 && std::fabs(m->integratedLufs() - db) < 0.1);
        }
    }
    // --- Tech 3341 case 3 and 4: gating must ignore the quiet parts ---
    {
        m->prepare(kFs, 512);
        feed(*m, {{-36, 10}, {-23, 60}, {-36, 10}});
        const double c3 = m->integratedLufs();
        m->prepare(kFs, 512);
        feed(*m, {{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}});
        const double c4 = m->integratedLufs();
        std::printf("  gating (Tech 3341 case 3 / case 4, expected -23.0 +/-0.1): %.2f / %.2f LUFS\n", c3, c4);
        CHECK(std::fabs(c3 + 23) < 0.1 && std::fabs(c4 + 23) < 0.1);
        CHECK(std::fabs(m->maxMomentaryLufs() + 23) < 0.1 && std::fabs(m->maxShortTermLufs() + 23) < 0.1);
    }
    // --- EBU Tech 3342 cases 1, 2: loudness range ---
    {
        m->prepare(kFs, 512);
        feed(*m, {{-20, 20}, {-30, 20}});
        const double c1 = m->loudnessRangeLu();
        m->prepare(kFs, 512);
        feed(*m, {{-20, 20}, {-15, 20}});
        const double c2 = m->loudnessRangeLu();
        std::printf("  loudness range (Tech 3342 case 1 expect 10 +/-1, case 2 expect 5 +/-1): %.2f / %.2f LU\n", c1, c2);
        CHECK(std::fabs(c1 - 10) < 1 && std::fabs(c2 - 5) < 1);
    }
    // --- true peak: fs/4 sine at 45 deg phase; samples sit 3.01 dB below the real waveform peak ---
    {
        m->prepare(kFs, 512);
        Buf l(512);
        for (int b = 0; b < 40; ++b) {
            for (std::size_t i = 0; i < l.size(); ++i) l[i] = static_cast<float>(0.5 * std::sin(std::numbers::pi / 2 * static_cast<double>(b * 512 + static_cast<int>(i)) + std::numbers::pi / 4));
            m->process(l.data(), l.data(), 512);
        }
        std::printf("  true peak, fs/4 sine 45 deg, real peak -6.02: sample peak %.2f dBFS, true peak %.2f dBTP\n", m->samplePeakDbfs(0), m->truePeakDbtp(0));
        CHECK(std::fabs(m->samplePeakDbfs(0) + 9.03) < 0.05 && std::fabs(m->truePeakDbtp(0) + 6.02) < 0.3);
        m->prepare(kFs, 512);
        feed(*m, {{-23, 10}});
        std::printf("  PLR, 1 kHz sine -23 dBFS (expect ~0 dB): %.2f dB\n", m->plrDb());
        CHECK(std::fabs(m->plrDb()) < 0.3);
    }
    // --- stereo: correlation, balance, mono deviation ---
    {
        m->prepare(kFs, 512); feed(*m, {{-20, 4}});
        const double same = m->correlation(), monoSame = m->monoDeviationLu();
        m->prepare(kFs, 512); feed(*m, {{-20, 4}}, kFs, 1000, -1.0);
        const double anti = m->correlation(), monoAnti = m->monoDeviationLu(), msAnti = m->balanceMsDb();
        m->prepare(kFs, 512); feed(*m, {{-20, 4}}, kFs, 1000, 1.0, 1370.0);
        const double unrel = m->correlation();
        m->prepare(kFs, 512); feed(*m, {{-20, 4}}, kFs, 1000, 0.0);
        const double monoLeftOnly = m->monoDeviationLu();
        m->prepare(kFs, 512); feed(*m, {{-20, 4}}, kFs, 1000, std::pow(10.0, -6.0 / 20.0));
        const double bal = m->balanceLrLu();
        std::printf("  correlation: L=R %.3f, L=-R %.3f, unrelated tones %.3f\n", same, anti, unrel);
        std::printf("  mono deviation: L=R %.2f LU, left only %.2f LU (expect -3.01), L=-R %.1f (mono cancels)\n", monoSame, monoLeftOnly, monoAnti);
        std::printf("  L/R balance, right 6 dB lower: %.2f LU | M/S balance L=-R: %.1f dB (all side)\n", bal, msAnti);
        CHECK(same > 0.999 && anti < -0.999 && std::fabs(unrel) < 0.05);
        CHECK(std::fabs(monoSame) < 0.01 && std::fabs(monoLeftOnly + 3.0103) < 0.05 && monoAnti < -100);
        CHECK(std::fabs(bal - 6) < 0.05 && msAnti > 100);
    }
    // --- RTA: a tone lands in its own 1/3-octave band, neighbours well down, peak hold holds ---
    {
        bool ok = true;
        for (int band : {2, 7, 17, 27}) {
            m->prepare(kFs, 512);
            feed(*m, {{-20, 2}}, kFs, MeterBridge::bandHz(band));
            const double at = m->bandDb(band), lo = m->bandDb(band - 2), hi = m->bandDb(band + 2);
            std::printf("  RTA %7.1f Hz tone -20 dBFS: own band %.2f dB, two bands down %.1f, two bands up %.1f\n", MeterBridge::bandHz(band), at, lo, hi);
            ok = ok && std::fabs(at + 20) < 0.3 && lo < at - 15 && hi < at - 15;
        }
        CHECK(ok);
        m->prepare(kFs, 512);
        feed(*m, {{-20, 2}, {-300, 1}});
        std::printf("  RTA peak hold 1 s after the tone stops: level %.1f dB, peak %.2f dB\n", m->bandDb(17), m->bandPeakDb(17));
        CHECK(m->bandDb(17) < -30 && std::fabs(m->bandPeakDb(17) + 20) < 0.3);
    }
    // --- radar history, vectorscope, reset ---
    {
        m->prepare(kFs, 512);
        feed(*m, {{-23, 10}});
        const auto hc = m->historyCount();
        const float last = m->historyLufs(hc - 1);
        float sl = 0, sr = 0; m->scopeFrame(m->scopeWritten() - 1, sl, sr);
        std::printf("  history after 10 s: %llu points (every 100 ms once 3 s is filled), last %.2f LUFS; scope frames written %llu\n",
                    static_cast<unsigned long long>(hc), last, static_cast<unsigned long long>(m->scopeWritten()));
        CHECK(hc == 71 && std::fabs(last + 23) < 0.1 && m->scopeWritten() == 480000 && sl == sr);
        m->reset();
        CHECK(m->integratedLufs() == MeterBridge::kSilent && m->historyCount() == 0 && m->truePeakDbtp(0) == MeterBridge::kSilent);
    }
    // --- zero allocation on the audio path ---
    {
        m->prepare(kFs, 512);
        Buf s = sine(997, 512.0 / kFs, -18);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (int it = 0; it < 400; ++it) m->process(s.data(), s.data(), 512);
            allocs = scope.count();
        }
        std::printf("  meter audio path, 400 blocks (4.3 s, loudness steps, histograms, history): %llu heap allocations\n", static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0);
    }
}

// =============================================================================================
//  FRONT-END TOOLS (our own designs): RESO TAMER, SHUFFLE, TAP ECHO, ToolSlot, TapTempo
// =============================================================================================
Buf noise(double seconds, double ampDb, std::uint32_t seed = 12345u) {
    Buf b(static_cast<std::size_t>(seconds * kFs));
    const double a = std::pow(10.0, ampDb / 20.0) * std::sqrt(3.0);   // uniform noise with this RMS
    std::uint32_t x = seed;
    for (float& v : b) { x = x * 1664525u + 1013904223u; v = static_cast<float>(a * (static_cast<double>(x >> 8) / 8388608.0 - 1.0)); }
    return b;
}
double toneLevel(const Buf& x, double hz, std::size_t from, std::size_t len) {   // dBFS RMS of one frequency (DFT bin)
    std::complex<double> acc{0, 0};
    for (std::size_t i = 0; i < len; ++i) acc += static_cast<double>(x[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kFs);
    return 20.0 * std::log10(std::max(std::abs(acc) * 2.0 / static_cast<double>(len) / std::sqrt(2.0), 1e-12));
}

void testConsoleTools() {
    std::printf("[console tools] HEM RESO TAMER / SHUFFLE / TAP ECHO (our own designs)\n");
    namespace RI = reso_ids;
    // ---------------- RESO TAMER ----------------
    {
        auto e = makeResoTamer(); e->prepare(kFs, 512);
        Buf in = noise(1.0, -20), l = in, r = in;
        runBlocks(*e, l, r);
        bool exactOff = l == in && r == in;
        e->reset(); e->set(RI::id(0, RI::On), 1); e->set(RI::id(0, RI::Freq), 1000);
        l = in; r = in; runBlocks(*e, l, r);
        const bool exactDepth0 = l == in && r == in;
        std::printf("  bands off: output identical %s; band on with DEPTH 0: identical %s\n", exactOff ? "yes" : "NO", exactDepth0 ? "yes" : "NO");
        CHECK(exactOff && exactDepth0);

        // a steady tone right on the band: cut = how far the narrow band pokes out (+9.03 dB for Q 4 vs Q 0.5)
        e->set(RI::id(0, RI::Width), 4); e->set(RI::id(0, RI::Threshold), 0); e->set(RI::id(0, RI::Depth), 12);
        e->reset();
        Buf t = sine(1000, 1.0, -20); l = t; r = t; runBlocks(*e, l, r);
        const double cutTone = levelOf(l) - (-20 - 3.0103);
        const double expect = -10 * std::log10(4.0 / 0.5);
        std::printf("  1 kHz tone, band at 1 kHz Q 4, THRESHOLD 0, DEPTH 12: output %+.2f dB (expected %+.2f), meter %+.2f dB\n", cutTone, expect, e->meter(RI::MeterGr1));
        CHECK(std::fabs(cutTone - expect) < 0.3 && std::fabs(e->meter(RI::MeterGr1) - expect) < 0.3);
        e->set(RI::id(0, RI::Depth), 3); e->reset();
        l = t; r = t; runBlocks(*e, l, r);
        const double cut3 = levelOf(l) - (-20 - 3.0103);
        e->set(RI::id(0, RI::Depth), 12); e->set(RI::id(0, RI::Threshold), 6); e->reset();
        l = t; r = t; runBlocks(*e, l, r);
        const double cutThr6 = levelOf(l) - (-20 - 3.0103);
        std::printf("  DEPTH 3 caps the cut: %+.2f dB; THRESHOLD 6 leaves %+.2f dB (expected %+.2f)\n", cut3, cutThr6, expect + 6);
        CHECK(std::fabs(cut3 + 3) < 0.2 && std::fabs(cutThr6 - (expect + 6)) < 0.3);

        // broadband noise is left alone (THRESHOLD 3): average cut over the second half
        e->set(RI::id(0, RI::Threshold), 3); e->reset();
        Buf nz = noise(2.0, -20); l = nz; r = nz;
        double grSum = 0; int grN = 0;
        for (std::size_t i = 0; i < l.size(); i += 256) {
            e->process(l.data() + i, r.data() + i, 256);
            if (i > l.size() / 2) { grSum += e->meter(RI::MeterGr1); ++grN; }
        }
        std::printf("  white noise, band at 1 kHz, THRESHOLD 3: average cut %.2f dB (should stay near 0)\n", grSum / grN);
        CHECK(grSum / grN > -1.0);

        // a band elsewhere doesn't touch the tone; LISTEN plays only the band
        e->set(RI::id(0, RI::Freq), 3000); e->set(RI::id(0, RI::Threshold), 0); e->reset();
        l = t; r = t; runBlocks(*e, l, r);
        const double other = levelOf(l) - (-20 - 3.0103);
        e->set(RI::id(0, RI::Freq), 1000); e->set(RI::id(0, RI::Listen), 1); e->reset();
        Buf lo = sine(100, 1.0, -20); l = lo; r = lo; runBlocks(*e, l, r);
        const double listen100 = levelOf(l) - (-20 - 3.0103);
        l = t; r = t; runBlocks(*e, l, r);
        const double listen1k = levelOf(l) - (-20 - 3.0103);
        std::printf("  band at 3 kHz on a 1 kHz tone: %+.2f dB | LISTEN on the 1 kHz band: 1 kHz %+.2f dB, 100 Hz %+.1f dB\n", other, listen1k, listen100);
        CHECK(std::fabs(other) < 0.5 && std::fabs(listen1k) < 0.3 && listen100 < -25);
        e->set(RI::id(0, RI::Listen), 0);

        // SPEED: time to reach 63 % of the cut
        auto attackMs = [&](int speed) {
            e->set(RI::id(0, RI::Speed), static_cast<float>(speed)); e->reset();
            Buf x(static_cast<std::size_t>(kFs * 0.5), 0.0f);
            const Buf tone = sine(1000, 0.3, -20);
            std::copy(tone.begin(), tone.end(), x.begin() + static_cast<std::ptrdiff_t>(kFs * 0.1));
            Buf a = x, b = x;
            for (std::size_t i = 0; i < a.size(); i += 48) {
                e->process(a.data() + i, b.data() + i, 48);
                if (i >= static_cast<std::size_t>(kFs * 0.1) && e->meter(RI::MeterGr1) <= 0.632 * expect) return (static_cast<double>(i) - kFs * 0.1) * 1000.0 / kFs;
            }
            return -1.0;
        };
        const double fast = attackMs(0), med = attackMs(1), slow = attackMs(2);
        std::printf("  SPEED (time to 63 %% of the cut): FAST %.1f ms, MEDIUM %.1f ms, SLOW %.1f ms\n", fast, med, slow);
        CHECK(fast > 0 && fast < med && med < slow);
    }
    // ---------------- SHUFFLE ----------------
    {
        namespace SI = shuffle_ids;
        auto e = makeShuffle(); e->prepare(kFs, 512);
        Buf a = noise(0.5, -20, 1), b = noise(0.5, -20, 2), l = a, r = b;
        runBlocks(*e, l, r);
        float maxDiff = 0;
        for (std::size_t i = 0; i < l.size(); ++i) maxDiff = std::max({maxDiff, std::fabs(l[i] - a[i]), std::fabs(r[i] - b[i])});
        std::printf("  SHUFFLE at defaults: max difference from input %.2g (float rounding only)\n", static_cast<double>(maxDiff));
        CHECK(maxDiff < 1e-6f);
        auto sideGain = [&](double hz) {   // a pure side signal (L = -R)
            e->reset();
            Buf x = sine(hz, 0.6, -20), y = x;
            for (float& v : y) v = -v;
            runBlocks(*e, x, y);
            return levelOf(x) - (-20 - 3.0103);
        };
        auto midGain = [&](double hz) { e->reset(); Buf x = sine(hz, 0.6, -20), y = x; runBlocks(*e, x, y); return levelOf(x) - (-20 - 3.0103); };
        e->set(SI::Shuffle, 6); e->set(SI::ShuffleFreq, 300);
        const double s40 = sideGain(40), s5k = sideGain(5000), m40 = midGain(40);
        e->set(SI::Shuffle, 0); e->set(SI::Mud, 6); e->set(SI::MudFreq, 250);
        const double mud250 = midGain(250), mud5k = midGain(5000), mudSide = sideGain(250);
        e->set(SI::Mud, 0); e->set(SI::MonoBass, 1);
        const double mb40 = sideGain(40), mb1k = sideGain(1000);
        e->set(SI::MonoBass, 0); e->set(SI::Width, 200);
        const double w200 = sideGain(1000);
        e->set(SI::Width, 0);
        const double w0 = sideGain(1000);
        e->set(SI::Width, 100); e->set(SI::Output, 6);
        const double out6 = midGain(1000);
        std::printf("  SHUFFLE +6 @ 300 Hz: side 40 Hz %+.2f, side 5 kHz %+.2f, mid 40 Hz %+.2f dB\n", s40, s5k, m40);
        std::printf("  MUD 6 @ 250 Hz: mid 250 Hz %+.2f, mid 5 kHz %+.2f, side 250 Hz %+.2f dB\n", mud250, mud5k, mudSide);
        std::printf("  MONO BASS: side 40 Hz %+.1f, side 1 kHz %+.2f dB | WIDTH 200 %%: side %+.2f dB, WIDTH 0: side %+.1f dB | OUTPUT +6: %+.2f dB\n", mb40, mb1k, w200, w0, out6);
        CHECK(s40 > 5.5 && std::fabs(s5k) < 0.2 && std::fabs(m40) < 0.05);
        CHECK(std::fabs(mud250 + 6) < 0.1 && std::fabs(mud5k) < 0.3 && std::fabs(mudSide) < 0.05);
        CHECK(mb40 < -12 && std::fabs(mb1k) < 0.2 && std::fabs(w200 - 6.0206) < 0.05 && w0 < -100 && std::fabs(out6 - 6) < 0.05);
        e->set(SI::Output, 0);
        Buf x = sine(500, 0.6, -20), y = x; runBlocks(*e, x, y);
        std::printf("  correlation meter on L = R: %+.3f\n", static_cast<double>(e->meter(SI::MeterCorrelation)));
        CHECK(e->meter(SI::MeterCorrelation) > 0.999f);
    }
    // ---------------- TAP ECHO ----------------
    {
        namespace EI = echo_ids;
        auto e = makeTapEcho(); e->prepare(kFs, 512);
        Buf a = noise(0.5, -20), l = a, r = a;
        runBlocks(*e, l, r);
        const bool exactDry = l == a && r == a;
        std::printf("  TAP ECHO with MIX 0: output identical %s\n", exactDry ? "yes" : "NO");
        CHECK(exactDry);
        auto impulseRun = [&](Buf& L, Buf& R, double seconds) {
            L.assign(static_cast<std::size_t>(seconds * kFs), 0.0f); R = L; L[0] = 1.0f; R[0] = 1.0f;
            runBlocks(*e, L, R);
        };
        auto peakAt = [](const Buf& x, std::size_t from, std::size_t to) {
            std::size_t best = from; for (std::size_t i = from; i < to && i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
            return best;
        };
        e->set(EI::Note, EI::NoteFree); e->set(EI::Time, 100); e->set(EI::Mix, 100); e->set(EI::Feedback, 50); e->reset();
        Buf L, R; impulseRun(L, R, 0.5);
        const std::size_t p1 = peakAt(L, 100, 7000), p2 = peakAt(L, 7000, 14000);
        double e1 = 0, e2 = 0;
        for (std::size_t i = p1 - 40; i < p1 + 40; ++i) e1 += L[i] * L[i];
        for (std::size_t i = p2 - 40; i < p2 + 40; ++i) e2 += L[i] * L[i];
        const double rep = 10 * std::log10(e2 / e1);
        std::printf("  TIME 100 ms (FREE): first echo at sample %zu (%.2f ms), second at %zu; FEEDBACK 50 %%: second echo %+.2f dB (expected -6.02)\n",
                    p1, static_cast<double>(p1) * 1000.0 / kFs, p2, rep);
        CHECK(p1 >= 4799 && p1 <= 4802 && p2 >= 9598 && p2 <= 9604 && std::fabs(rep + 6.02) < 0.3);
        e->set(EI::Mode, 1); e->reset(); impulseRun(L, R, 0.5);
        double l1 = 0, r1 = 0, l2 = 0, r2 = 0;
        for (std::size_t i = 4700; i < 4900; ++i) { l1 += L[i] * L[i]; r1 += R[i] * R[i]; }
        for (std::size_t i = 9500; i < 9700; ++i) { l2 += L[i] * L[i]; r2 += R[i] * R[i]; }
        std::printf("  PING-PONG: 1st echo L %.1f / R %.1f dB, 2nd echo L %.1f / R %.1f dB\n", 10 * std::log10(l1 + 1e-30), 10 * std::log10(r1 + 1e-30), 10 * std::log10(l2 + 1e-30), 10 * std::log10(r2 + 1e-30));
        CHECK(l1 > 100 * r1 && r2 > 100 * l2);
        const double n120 = echoNoteMs(5, 120, 0), n90 = echoNoteMs(5, 90, 0), d8 = echoNoteMs(9, 120, 0), t4 = echoNoteMs(7, 120, 0), fr = echoNoteMs(0, 120, 321);
        std::printf("  NOTE: 1/4 @120 = %.1f ms, 1/4 @90 = %.1f ms, 1/8 D @120 = %.1f ms, 1/4 T @120 = %.1f ms, FREE = %.0f ms\n", n120, n90, d8, t4, fr);
        CHECK(std::fabs(n120 - 500) < 1e-9 && std::fabs(n90 - 666.6667) < 1e-3 && std::fabs(d8 - 375) < 1e-9 && std::fabs(t4 - 333.3333) < 1e-3 && fr == 321);
        e->set(EI::Mode, 0); e->set(EI::Note, 5); e->set(EI::Tempo, 120); e->set(EI::Feedback, 0); e->reset(); impulseRun(L, R, 0.7);
        const std::size_t pq = peakAt(L, 100, 33000);
        std::printf("  NOTE 1/4 at TEMPO 120: echo at %.2f ms\n", static_cast<double>(pq) * 1000.0 / kFs);
        CHECK(pq >= 23999 && pq <= 24002);
        TapTempo tt;
        const bool first = tt.tap(10.0);
        tt.tap(10.5); tt.tap(11.0);
        const bool third = tt.tap(11.5);
        const double bpm = tt.bpm();
        const bool afterPause = tt.tap(15.0);
        std::printf("  TAP: presses every 0.5 s -> %.2f BPM; first press alone %s; after a 3.5 s pause the count restarts %s\n", bpm,
                    first ? "gave a tempo (WRONG)" : "gives no tempo", afterPause ? "NO" : "yes");
        CHECK(!first && third && std::fabs(bpm - 120) < 1e-6 && !afterPause);
    }
    // ---------------- ToolSlot ----------------
    {
        ToolSlot slot; slot.prepare(kFs, 512);
        Buf a = noise(0.2, -20), l = a, r = a;
        slot.process(l.data(), r.data(), static_cast<std::uint32_t>(l.size()));
        const bool offExact = l == a;
        slot.setType(ToolSlot::Shuffle); slot.tool(ToolSlot::Shuffle).set(shuffle_ids::Width, 0);
        Buf x = sine(1000, 0.5, -20), y(x.size(), 0.0f);
        slot.process(x.data(), y.data(), static_cast<std::uint32_t>(x.size()));
        float diff = 0; for (std::size_t i = x.size() * 3 / 4; i < x.size(); ++i) diff = std::max(diff, std::fabs(x[i] - y[i]));
        std::printf("  ToolSlot: OFF passes audio untouched %s; SHUFFLE WIDTH 0 makes L-only audio mono (max L-R %.2g)\n", offExact ? "yes" : "NO", static_cast<double>(diff));
        CHECK(offExact && diff < 1e-6f);
        std::uint64_t allocs = 0;
        {
            AllocScope scope;
            for (int t = 0; t < 4; ++t) {
                slot.setType(t);
                for (int k = 0; k < 20; ++k) slot.process(l.data(), r.data(), 256);
            }
            allocs = scope.count();
        }
        std::printf("  tools audio path, every tool, switching: %llu heap allocations\n", static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0);
    }
}

// =============================================================================================
//  HEM CONSOLE RIG: the whole path, saved state
// =============================================================================================
void testConsoleRig() {
    std::printf("[console rig] stems -> desk -> MAIN INSERT rack -> bus comp -> main -> meter; saved state\n");
    auto rig = std::make_unique<ConsoleRig>();
    rig->prepare(kFs, 512);
    const std::size_t N = static_cast<std::size_t>(kFs * 4);
    Buf t = sine(1000, 4.0, -20), mL(N), mR(N), monL(N), monR(N);
    auto run = [&](const Buf& in, int ch = 0) {
        for (Buf* b : {&mL, &mR, &monL, &monR}) b->assign(in.size(), 0.0f);
        ConsoleRig::Io io;
        io.stemL[static_cast<std::size_t>(ch)] = in.data(); io.stemR[static_cast<std::size_t>(ch)] = in.data();
        io.mainL = mL.data(); io.mainR = mR.data(); io.monL = monL.data(); io.monR = monR.data();
        rig->process(io, static_cast<std::uint32_t>(in.size()));
    };
    run(t);
    const double mainDb = levelOf(mL) - (-20 - 3.0103), monDb = levelOf(monL) - (-20 - 3.0103);
    std::printf("  defaults, 1 kHz -20 dBFS on channel 1: main %+.3f dB, monitor %+.3f dB, meter integrated %.2f LUFS\n", mainDb, monDb, rig->meter().integratedLufs());
    CHECK(std::fabs(mainDb) < 0.01 && std::fabs(monDb) < 0.01 && std::fabs(rig->meter().integratedLufs() + 20) < 0.15);

    // MAIN INSERT with every unit OUT, then every unit IN at neutral settings: same timing both ways
    auto impulseDelay = [&]() {
        rig->reset();
        constexpr std::size_t kAt = 9600;   // after the desk's gain glides have settled
        Buf imp(kAt + 4096, 0.0f); imp[kAt] = 1.0f;
        Buf oL(imp.size()), oR(imp.size());
        ConsoleRig::Io io; io.stemL[0] = imp.data(); io.stemR[0] = imp.data(); io.mainL = oL.data(); io.mainR = oR.data();
        rig->process(io, static_cast<std::uint32_t>(imp.size()));
        std::size_t best = 0; for (std::size_t i = 0; i < oL.size(); ++i) if (std::fabs(oL[i]) > std::fabs(oL[best])) best = i;
        return std::pair<std::size_t, float>{best - kAt, oL[best]};
    };
    rig->desk().setMaster(StudioDesk::MainInsert, 1);
    for (int u = 0; u < 3; ++u) rig->setUnitIn(u, false);
    const auto out = impulseDelay();
    rig->twinBus().set(twinbus_ids::id(0, twinbus_ids::In), 0); rig->twinBus().set(twinbus_ids::id(1, twinbus_ids::In), 0);
    for (int u = 0; u < 3; ++u) rig->setUnitIn(u, true);
    const auto in = impulseDelay();
    std::printf("  MAIN INSERT rack latency %u samples: all units OUT -> impulse at %zu (%.5f), all IN at neutral (twin bus sides OUT) -> at %zu (%.5f)\n",
                rig->rackLatency(), out.first, static_cast<double>(out.second), in.first, static_cast<double>(in.second));
    CHECK(out.first == rig->rackLatency() && in.first == rig->rackLatency() && std::fabs(out.second - 1.0f) < 1e-3f && std::fabs(in.second - 1.0f) < 1e-3f);
    std::printf("  rack orders: ");
    for (int o = 0; o < ConsoleRig::kOrderCount; ++o) std::printf("%s%s", o ? " | " : "", ConsoleRig::orderName(o).c_str());
    std::printf("\n");
    CHECK(ConsoleRig::orderName(0) == "EQ > BUS > COLOUR" && ConsoleRig::orderName(5) == "COLOUR > BUS > EQ");

    // each order really changes the order: EQ +12 dB at 1 kHz into the colour stage's output trim -6 vs the other way is the same
    // gain, so check order with the twin bus compressing: EQ before the bus is compressed more than EQ after it
    rig->setDefaults(); rig->reset();
    rig->desk().setMaster(StudioDesk::MainInsert, 1);
    namespace T = twinbus_ids; namespace E = stepeq_ids;
    for (int s = 0; s < 2; ++s) { rig->twinBus().set(T::id(s, T::Threshold), 0); rig->twinBus().set(T::id(s, T::Ratio), 6); }
    for (int c = 0; c < 2; ++c) { rig->stepEq().set(E::id(c, E::HiMidFreq), 0); rig->stepEq().set(E::id(c, E::HiMidGain), 10); }
    Buf tone = sine(800, 2.0, -10);
    rig->setRackOrder(0); rig->reset(); run(tone); const double eqFirst = levelOf(mL);
    rig->setRackOrder(2); rig->reset(); run(tone); const double busFirst = levelOf(mL);
    std::printf("  order matters: EQ > BUS (EQ +12 dB hits the 10:1 compressor) %.2f dBFS vs BUS > EQ %.2f dBFS\n", eqFirst, busFirst);
    CHECK(busFirst > eqFirst + 6);

    // TWIN BUS EXT S/C keyed by the EXT 2 return (not summed into main: only the key)
    {
        rig->setDefaults(); rig->reset();
        rig->desk().setMaster(StudioDesk::MainInsert, 1);
        rig->twinBus().set(T::ExtSc, 1);
        for (int s = 0; s < 2; ++s) { rig->twinBus().set(T::id(s, T::Threshold), 0); rig->twinBus().set(T::id(s, T::Ratio), 6); }
        Buf quiet = sine(1000, 2.0, -30), key = sine(60, 2.0, -6);
        auto runKey = [&](const Buf* k) {
            mL.assign(quiet.size(), 0.0f); mR.assign(quiet.size(), 0.0f);
            ConsoleRig::Io io; io.stemL[0] = quiet.data(); io.stemR[0] = quiet.data(); io.mainL = mL.data(); io.mainR = mR.data();
            if (k) { io.ext2L = k->data(); io.ext2R = k->data(); }
            rig->reset();
            rig->process(io, static_cast<std::uint32_t>(quiet.size()));
            return levelOf(mL);
        };
        const double noKey = runKey(nullptr), keyed = runKey(&key);
        std::printf("  TWIN BUS EXT S/C = COMP, key = EXT 2 (-6 dBFS, not sent to main): quiet stem %.2f dBFS without the key, %.2f dBFS with it\n", noKey, keyed);
        CHECK(keyed < noKey - 6);
    }

    // a channel tool behind the channel INSERT switch
    rig->setDefaults(); rig->reset();
    rig->channelTool(0).setType(ToolSlot::Shuffle); rig->channelTool(0).tool(ToolSlot::Shuffle).set(shuffle_ids::Width, 0);
    Buf left = sine(1000, 1.0, -20), silent(left.size(), 0.0f);
    auto runLR = [&](const Buf& l, const Buf& r) {
        mL.assign(l.size(), 0.0f); mR.assign(l.size(), 0.0f);
        ConsoleRig::Io io; io.stemL[0] = l.data(); io.stemR[0] = r.data(); io.mainL = mL.data(); io.mainR = mR.data();
        rig->process(io, static_cast<std::uint32_t>(l.size()));
    };
    runLR(left, silent);
    const double rOff = rmsDb(mR, 24000, 48000);
    rig->desk().setChannel(0, StudioDesk::Insert, 1); rig->reset();
    runLR(left, silent);
    const double rOn = rmsDb(mR, 24000, 48000);
    std::printf("  channel 1 TOOL = SHUFFLE (WIDTH 0), left-only stem: right output INSERT off %.1f dBFS, INSERT on %.2f dBFS\n", rOff, rOn);
    CHECK(rOff < -100 && std::fabs(rOn - (-20 - 3.0103 - 6.0206)) < 0.1);

    // saved state: every control round-trips through text
    rig->setDefaults();
    rig->desk().setChannel(4, StudioDesk::HfGain, 7.5f); rig->desk().setChannel(12, StudioDesk::Fader, -12.25f);
    rig->desk().setMaster(StudioDesk::BusCompThreshold, -8); rig->setRackOrder(4); rig->setUnitIn(1, false);
    rig->twinBus().set(T::id(1, T::Attack), 7); rig->colour().set(colour_ids::Drive, 8.5f);
    rig->channelTool(6).setType(ToolSlot::Echo); rig->channelTool(6).tool(ToolSlot::Echo).set(echo_ids::Feedback, 42);
    rig->colourTool().setType(ToolSlot::Reso);
    const std::string saved = rig->saveText();
    const auto lines = static_cast<int>(std::count(saved.begin(), saved.end(), '\n'));
    auto rig2 = std::make_unique<ConsoleRig>();
    const int applied = rig2->loadText(saved);
    const bool same = rig2->saveText() == saved;
    const int junk = rig2->loadText("nonsense\nfoo.bar=1\ndesk.ch99.hf=3\ndesk.ch01.nope=2\n# comment=1\n");
    float v = 0;
    const bool got = rig2->getByKey("desk.ch05.hf", v);
    std::printf("  saved state: %d lines, %d applied on load, identical after reload %s; junk lines applied: %d; desk.ch05.hf reads %.2f\n",
                lines, applied, same ? "yes" : "NO", junk, static_cast<double>(v));
    CHECK(applied == lines && same && junk == 0 && got && v == 7.5f);

    // zero allocation with everything switched on
    rig->setDefaults(); rig->prepare(kFs, 512);
    rig->desk().setMaster(StudioDesk::MainInsert, 1); rig->desk().setMaster(StudioDesk::BusCompIn, 1);
    for (int c = 0; c < ConsoleRig::kChannels; ++c) {
        for (int p : {StudioDesk::Hpf, StudioDesk::EqIn, StudioDesk::CompIn, StudioDesk::Insert}) rig->desk().setChannel(c, p, 1);
        rig->channelTool(c).setType(1 + c % 3);
    }
    rig->colour().set(colour_ids::DriveIn, 1); rig->colour().set(colour_ids::Insert, 1); rig->colourTool().setType(ToolSlot::Echo);
    std::vector<Buf> stems(13, sine(220, 512.0 / kFs, -12));
    for (Buf* b : {&mL, &mR, &monL, &monR}) b->assign(512, 0.0f);
    ConsoleRig::Io io;
    for (std::size_t c = 0; c < 13; ++c) { io.stemL[c] = stems[c].data(); io.stemR[c] = stems[c].data(); }
    io.mainL = mL.data(); io.mainR = mR.data(); io.monL = monL.data(); io.monR = monR.data();
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int it = 0; it < 100; ++it) { rig->setRackOrder(it % 6); rig->setUnitIn(it % 3, it % 2 == 0); rig->process(io, 512); }
        allocs = scope.count();
    }
    std::printf("  whole rig, 13 stems, every section and tool on, rack order and IN switches moving: %llu heap allocations\n", static_cast<unsigned long long>(allocs));
    CHECK(allocs == 0);
}

void benchmarkConsole() {
    std::printf("[console benchmark] (machine-dependent; the CI runner is NOT the rig)\n");
    using clock = std::chrono::steady_clock;
    for (double fs : {48000.0, 96000.0}) {
        for (std::uint32_t frames : {64u, 128u, 256u, 512u}) {
            // desk: 13 stereo stems, every processing section on
            auto d = std::make_unique<StudioDesk>();
            d->prepare(fs, frames);
            using D = StudioDesk;
            for (int k = 0; k < 13; ++k) {
                for (int p : {D::Hpf, D::EqIn, D::CompIn, D::Cue1On, D::Cue2On}) d->setChannel(k, p, 1);
                d->setChannel(k, D::HfGain, 3); d->setChannel(k, D::MfGain, -2); d->setChannel(k, D::LfGain, 2); d->setChannel(k, D::CompThreshold, -10);
            }
            d->setMaster(D::BusCompIn, 1); d->setMaster(D::BusCompAutoRelease, 1);
            std::vector<Buf> st(13, sine(220, static_cast<double>(frames) / fs, -12, fs));
            Buf o[12]; for (auto& b : o) b.assign(frames, 0.0f);
            StudioDesk::Inputs in; StudioDesk::Outputs out;
            for (int k = 0; k < 13; ++k) { in.left[static_cast<std::size_t>(k)] = st[static_cast<std::size_t>(k)].data(); in.right[static_cast<std::size_t>(k)] = st[static_cast<std::size_t>(k)].data(); }
            out.mainL = o[0].data(); out.mainR = o[1].data(); out.busBL = o[2].data(); out.busBR = o[3].data(); out.cue1L = o[4].data(); out.cue1R = o[5].data();
            out.cue2L = o[6].data(); out.cue2R = o[7].data(); out.monL = o[8].data(); out.monR = o[9].data(); out.altL = o[10].data(); out.altR = o[11].data();
            const int iters = static_cast<int>(2'000'000 / frames);
            auto t0 = clock::now();
            for (int i = 0; i < iters; ++i) d->process(in, out, frames);
            const double usDesk = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / iters;

            auto eq = makeStepEq(); eq->prepare(fs, frames);
            for (int c = 0; c < 2; ++c) for (int b : {stepeq_ids::LoGain, stepeq_ids::LoMidGain, stepeq_ids::HiMidGain, stepeq_ids::HiGain}) eq->set(stepeq_ids::id(c, b), 7);
            Buf l = sine(220, static_cast<double>(frames) / fs, -12, fs), r = l;
            t0 = clock::now();
            for (int i = 0; i < iters; ++i) eq->process(l.data(), r.data(), frames);
            const double usEq = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / iters;

            auto tb = makeTwinBus(); tb->prepare(fs, frames);
            namespace T = twinbus_ids;
            for (int s = 0; s < 2; ++s) for (int prm : {T::LfOn, T::HfOn, T::HfBell, T::Grit, T::LowThd}) tb->set(T::id(s, prm), 1);
            tb->set(T::id(0, T::LfCompExp), 5); tb->set(T::id(0, T::HfCompExp), 25); tb->set(T::id(0, T::Threshold), 5);
            t0 = clock::now();
            for (int i = 0; i < iters; ++i) tb->process(l.data(), r.data(), frames);
            const double usTb = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / iters;

            auto cs = makeColourStage(); cs->prepare(fs, frames);
            namespace C = colour_ids;
            for (int prm : {C::DriveIn, C::EqIn, C::HfcIn, C::ImageIn, C::Transformer}) cs->set(prm, 1);
            cs->set(C::Hpf, 1);
            t0 = clock::now();
            for (int i = 0; i < iters; ++i) cs->process(l.data(), r.data(), frames);
            const double usCs = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / iters;

            MeterBridge mb; mb.prepare(fs, frames);
            t0 = clock::now();
            for (int i = 0; i < iters; ++i) mb.process(l.data(), r.data(), frames);
            const double usMb = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / iters;

            // the whole rig: 13 stems, every section on, the rack in, a tool on every channel
            auto rig = std::make_unique<ConsoleRig>(); rig->prepare(fs, frames);
            rig->desk().setMaster(StudioDesk::MainInsert, 1); rig->desk().setMaster(StudioDesk::BusCompIn, 1);
            for (int k = 0; k < 13; ++k) {
                for (int p : {D::Hpf, D::EqIn, D::CompIn, D::Insert}) rig->desk().setChannel(k, p, 1);
                rig->channelTool(k).setType(1 + k % 3);
            }
            rig->colour().set(colour_ids::DriveIn, 1); rig->colour().set(colour_ids::EqIn, 1);
            ConsoleRig::Io rio;
            for (int k = 0; k < 13; ++k) { rio.stemL[static_cast<std::size_t>(k)] = st[static_cast<std::size_t>(k)].data(); rio.stemR[static_cast<std::size_t>(k)] = st[static_cast<std::size_t>(k)].data(); }
            rio.mainL = o[0].data(); rio.mainR = o[1].data(); rio.monL = o[8].data(); rio.monR = o[9].data();
            t0 = clock::now();
            const int rigIters = std::max(1, iters / 4);
            for (int i = 0; i < rigIters; ++i) rig->process(rio, frames);
            const double usRig = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / rigIters;

            const double budget = 1e6 * frames / fs;
            std::printf("  %3.0f kHz, %3u frames: WHOLE RIG (13 stems, all on, rack in, 13 tools) %.1f us (%.2f%%)\n", fs / 1000, frames, usRig, 100 * usRig / budget);
            CHECK(usRig < budget);
            std::printf("  %3.0f kHz, %3u frames (%.2f ms): desk 13ch all-on %.1f us (%.2f%%) | step EQ %.1f us (%.2f%%) | twin bus %.1f us (%.2f%%) | colour stage %.1f us (%.2f%%) | meter %.1f us (%.2f%%)\n",
                        fs / 1000, frames, budget / 1000, usDesk, 100 * usDesk / budget, usEq, 100 * usEq / budget, usTb, 100 * usTb / budget, usCs, 100 * usCs / budget, usMb, 100 * usMb / budget);
            CHECK(usDesk + usEq + usTb + usCs + usMb < budget);
        }
    }
}

} // namespace hc::test
