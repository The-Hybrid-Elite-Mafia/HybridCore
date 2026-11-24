// HEM QA pass for the 22 HEM Suite effects (hc::fx): stability, parameter sweep, meter liveness and the
// HEM-board faceplate (layoutFxPanelFramed). Report only — writes qa_report_hybridcore.csv and never fails
// the test run. Run it with: hc_tests --qa
#include "QaCommon.hpp"
#include "TestHarness.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/ui/FxPanel.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hc::test {
namespace {

using hemqa::Stereo;

// Runs `in` through the effect in host-sized blocks (the effect works in place on a copy).
Stereo run(fx::Effect& e, const Stereo& in) {
    Stereo out = in;
    for (std::size_t pos = 0; pos < out.size(); pos += hemqa::kBlock) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(hemqa::kBlock, out.size() - pos));
        e.process(out.l.data() + pos, out.r.data() + pos, n);
    }
    return out;
}

hemqa::Rect qr(const ui::Rect& r) { return {r.x, r.y, r.w, r.h}; }

bool stepped(const fx::ParamInfo& p) { return p.taper == fx::Taper::Choice || p.taper == fx::Taper::Toggle; }

void auditDsp(const fx::EffectInfo& info, hemqa::Report& rep) {
    const std::string name = info.name;
    auto e = info.create();
    const std::span<const fx::ParamInfo> ps = e->params();
    const Stereo full = hemqa::testAudio(1.0), sweep = hemqa::testAudio(1.0), voice = hemqa::voiceAudio(1.0),
                 silence(static_cast<std::size_t>(0.5 * hemqa::kRate));

    // ---- stability: defaults, 1 s of the test signal
    e->prepare(hemqa::kRate, hemqa::kBlock);
    e->setDefaults();
    e->reset();
    if (!hemqa::finite(run(*e, full))) rep.add(name, "DSP", "NAN_OR_INF", "defaults", "non-finite output on 1 s of test audio at default settings");

    // ---- parameter sweep
    // A control can be silent at the default settings for a good reason (a band's frequency while its gain is 0 dB,
    // a ratio while the threshold is above the signal). So a control is only DEAD_WIRE when the output stays
    // bit-identical in every one of these settings of the other controls: defaults, all at min, all at max, all at
    // the middle; each other control moved on its own; 8 seeded random settings; the control pressed while the
    // music plays; each of them with the test mix and with a single gliding voice (pitch tools need one).
    // Switches that silence or pass audio straight through stay at their defaults.
    auto keepAtDefault = [&](const fx::ParamInfo& p) {
        const std::string id = p.id;
        for (const char* k : {"bypass", "solo", "listen", "mute", "freeze", "delta", "audition"})
            if (id.find(k) != std::string::npos) return true;
        return false;
    };
    constexpr int kRandomSettings = 8;
    int pairK = 0;
    float pairV = 0;
    bool useVoice = false;
    // 0 defaults, 1 all min, 2 all max, 3 all middle, 5 one other control (pairK) at pairV, 6 pressed while playing,
    // 7.. a seeded random setting of all the other controls
    auto setContext = [&](int skip, int mode) {
        e->setDefaults();
        if (mode == 0 || mode == 6) return;
        if (mode == 5) { if (pairK != skip) e->set(pairK, pairV); return; }
        std::uint32_t r = 0x9E3779B9u * static_cast<std::uint32_t>(mode);
        for (std::size_t k = 0; k < ps.size(); ++k) {
            r = r * 1664525u + 1013904223u;
            if (static_cast<int>(k) == skip || keepAtDefault(ps[k])) continue;
            const float u = static_cast<float>(r >> 8) / 16777216.0f;
            float v = mode == 1 ? ps[k].min : mode == 2 ? ps[k].max : fx::toPlain(ps[k], 0.5f);
            if (mode >= 7) v = stepped(ps[k]) ? std::floor(ps[k].min + u * (ps[k].max - ps[k].min + 1.0f)) : fx::toPlain(ps[k], u);
            e->set(static_cast<int>(k), v);
        }
    };
    bool fresh = false;
    auto stateHash = [&](int mode, int param, float value, bool& blewUp, bool& getOk) {
        if (fresh) { e = info.create(); e->prepare(hemqa::kRate, hemqa::kBlock); }
        setContext(param, mode);
        const Stereo& in = useVoice ? voice : sweep;
        Stereo o;
        if (mode == 6 && param >= 0) {   // pressed while playing: half at its default, then the new value
            e->reset();
            Stereo a(in.size() / 2), b(in.size() - in.size() / 2);
            std::copy_n(in.l.data(), a.size(), a.l.data()); std::copy_n(in.r.data(), a.size(), a.r.data());
            std::copy_n(in.l.data() + a.size(), b.size(), b.l.data()); std::copy_n(in.r.data() + a.size(), b.size(), b.r.data());
            const Stereo oa = run(*e, a);
            e->set(param, value);
            const Stereo ob = run(*e, b);
            o = Stereo(in.size());
            std::copy_n(oa.l.data(), oa.size(), o.l.data()); std::copy_n(oa.r.data(), oa.size(), o.r.data());
            std::copy_n(ob.l.data(), ob.size(), o.l.data() + oa.size()); std::copy_n(ob.r.data(), ob.size(), o.r.data() + oa.size());
        } else {
            if (param >= 0) {
                e->set(param, value);
                getOk = hemqa::closeTo(e->get(param), fx::clampPlain(ps[static_cast<std::size_t>(param)], value));
            }
            e->reset();
            o = run(*e, in);
        }
        blewUp = !hemqa::finite(o);
        hemqa::Hash h;
        h.add(o);
        h.add(e->reductionDb());
        for (int m = 0; m < static_cast<int>(e->meters().size()); ++m) h.add(e->meter(m));
        return h.h;
    };
    bool dummyBlew = false, dummyGet = true;
    const std::uint64_t base1 = stateHash(0, -1, 0, dummyBlew, dummyGet), base2 = stateHash(0, -1, 0, dummyBlew, dummyGet);
    bool deterministic = base1 == base2;
    if (!deterministic) {
        // reset() does not bring the effect back to the same start: run every state on a fresh instance instead
        fresh = true;
        if (stateHash(0, -1, 0, dummyBlew, dummyGet) == stateHash(0, -1, 0, dummyBlew, dummyGet)) {
            deterministic = true;
            rep.add(name, "DSP", "RESET_NOT_REPEATABLE", "reset()", "two runs after reset() differ (a fresh instance repeats): offline renders may not match; sweep ran on fresh instances");
        } else {
            rep.note(name, "output differs even between two fresh instances (random source): DEAD_WIRE check skipped");
        }
    }
    for (int mode = 1; mode <= 3; ++mode) {
        bool b = false, g = true;
        (void)stateHash(mode, -1, 0, b, g);
        if (b) rep.add(name, "DSP", "NAN_OR_INF_SWEEP", mode == 1 ? "all controls at min" : mode == 2 ? "all controls at max" : "all controls at middle",
                       "non-finite output");
    }

    for (std::size_t i = 0; i < ps.size(); ++i) {
        e->setDefaults();
        if (!e->paramActive(static_cast<int>(i))) continue;   // does nothing at the default settings by design
        const fx::ParamInfo& p = ps[i];
        bool sampled = false;
        const std::vector<float> values = hemqa::sweepValues(p.min, p.def, p.max, stepped(p), &sampled);
        if (sampled) rep.note(name, std::string(p.id) + ": stepped control with more than 64 steps, 64 sampled");
        bool blew = false, mism = false, live = !deterministic;
        std::string blewAt, mismAt;
        auto liveIn = [&](int mode) {
            std::set<std::uint64_t> hashes;
            for (float v : values) {
                bool b = false, g = true;
                hashes.insert(stateHash(mode, static_cast<int>(i), v, b, g));
                if (b && !blew) { blew = true; blewAt = fx::formatValue(p, fx::clampPlain(p, v)); }
                if (!g && !mism) { mism = true; mismAt = hemqa::fmt("%g", v); }
            }
            return hashes.size() > 1;
        };
        for (int pass = 0; pass < 2 && !live; ++pass) {   // the test mix, then a single voice
            useVoice = pass == 1;
            for (int mode = 0; mode <= 3 && !live; ++mode) live = liveIn(mode);
            if (!live) live = liveIn(6);
            for (int seed = 7; seed < 7 + kRandomSettings && !live; ++seed) live = liveIn(seed);
            for (std::size_t k = 0; k < ps.size() && !live; ++k) {
                if (k == i || keepAtDefault(ps[k])) continue;
                for (float o : hemqa::sweepValues(ps[k].min, ps[k].def, ps[k].max, stepped(ps[k]))) {
                    if (o == ps[k].def) continue;
                    pairK = static_cast<int>(k); pairV = o;
                    if ((live = liveIn(5))) break;
                }
            }
        }
        useVoice = false;
        if (blew) rep.add(name, "DSP", "NAN_OR_INF_SWEEP", p.id, "non-finite output at " + blewAt);
        if (mism) rep.add(name, "DSP", "GET_SET_MISMATCH", p.id, "get() != set() at " + mismAt);
        if (!live)
            rep.add(name, "DSP", "DEAD_WIRE", p.id, "audio + meters bit-identical at all " + std::to_string(values.size()) +
                    " values, in every setting of the other controls tried (default / min / max / middle, each one moved,"
                    " 8 random, pressed while playing), with the test mix and with a single voice");
    }

    // ---- meters: silence, then the signal, read after every block (as the faceplate reads them). A meter must be
    // finite and move with the sound in at least one setting of the controls. One that only follows a control
    // (a delay-time readout) is reported as a note, not as stuck.
    const ui::PanelSpec spec = ui::panelSpecFor(info, *e);
    ui::EffectPanelHost host(*e);
    std::vector<int> moved(spec.meters.size(), 0), bad(spec.meters.size(), 0);
    std::vector<float> lastHot(spec.meters.size(), 0.0f);
    std::vector<std::set<float>> seen(spec.meters.size());
    auto meterPass = [&](int mode) {
        setContext(-1, mode);
        e->reset();
        (void)run(*e, silence);
        std::vector<float> quiet;
        for (int m = 0; m < static_cast<int>(spec.meters.size()); ++m) quiet.push_back(host.meter(m));
        const Stereo& in = useVoice ? voice : full;
        Stereo blk(hemqa::kBlock);
        for (std::size_t pos = 0; pos < in.size(); pos += hemqa::kBlock) {
            const std::size_t nb = std::min<std::size_t>(hemqa::kBlock, in.size() - pos);
            std::copy_n(in.l.data() + pos, nb, blk.l.data());
            std::copy_n(in.r.data() + pos, nb, blk.r.data());
            e->process(blk.l.data(), blk.r.data(), static_cast<std::uint32_t>(nb));
            for (std::size_t m = 0; m < spec.meters.size(); ++m) {
                const float hot = host.meter(static_cast<int>(m));
                if (!std::isfinite(hot) || !std::isfinite(quiet[m])) bad[m] = 1;
                else if (quiet[m] != hot) moved[m] = 1;
                lastHot[m] = hot;
            }
        }
        for (std::size_t m = 0; m < spec.meters.size(); ++m) seen[m].insert(lastHot[m]);
    };
    auto allMoved = [&] { return std::all_of(moved.begin(), moved.end(), [](int x) { return x != 0; }); };
    e->prepare(hemqa::kRate, hemqa::kBlock);
    for (int pass = 0; pass < 2 && !allMoved(); ++pass) {
        useVoice = pass == 1;
        for (int mode = 0; mode <= 3; ++mode) meterPass(mode);
        for (int seed = 7; seed < 7 + kRandomSettings && !allMoved(); ++seed) meterPass(seed);
        for (std::size_t k = 0; k < ps.size() && !allMoved(); ++k) {
            if (keepAtDefault(ps[k])) continue;
            for (float o : hemqa::sweepValues(ps[k].min, ps[k].def, ps[k].max, stepped(ps[k]))) {
                if (o == ps[k].def) continue;
                pairK = static_cast<int>(k); pairV = o;
                meterPass(5);
                if (allMoved()) break;
            }
        }
    }
    useVoice = false;
    for (std::size_t m = 0; m < spec.meters.size(); ++m) {
        const std::string mn = spec.meters[m].name;
        if (bad[m]) rep.add(name, "METER", "METER_NAN_OR_INF", mn, "non-finite meter value");
        if (moved[m]) continue;
        if (seen[m].size() > 1) { rep.note(name, mn + ": a readout of a setting (follows the controls, not the sound): " + std::to_string(seen[m].size()) + " values seen"); continue; }
        rep.add(name, "METER", "METER_STUCK", mn, "reads " + hemqa::fmt("%g", lastHot[m]) +
                " for both silence and the signal (read after every block), in every setting of the controls tried, with"
                " the test mix and with a single voice");
    }
}

void auditFace(const fx::EffectInfo& info, hemqa::Report& rep) {
    const std::string name = info.name;
    auto e = info.create();
    e->setDefaults();
    const int count = static_cast<int>(e->params().size());
    const ui::PanelSpec spec = ui::panelSpecFor(info, *e);
    const ui::PanelLayout F = ui::layoutFxPanelFramed(spec);

    struct Hot { hemqa::Rect r; std::string what; };
    std::vector<Hot> hot;
    std::vector<int> seen(static_cast<std::size_t>(count), 0);
    auto bind = [&](int param, const char* kind, int choice = -1) {
        if (param < 0 || param >= count) { rep.add(name, "UI", "BAD_PARAM_INDEX", kind, "parameter index " + std::to_string(param) + " (has " + std::to_string(count) + ")"); return false; }
        const fx::ParamInfo& p = e->params()[static_cast<std::size_t>(param)];
        if (choice >= 0 && choice > static_cast<int>(std::lround(p.max)))
            rep.add(name, "UI", "BAD_PARAM_INDEX", p.id, std::string(kind) + " selects choice " + std::to_string(choice) + " past the last one");
        ++seen[static_cast<std::size_t>(param)];
        return true;
    };
    for (const auto& k : F.knobs) if (bind(k.param, "knob")) hot.push_back({qr(k.well), std::string("knob ") + e->params()[static_cast<std::size_t>(k.param)].id});
    for (const auto& b : F.buttons) if (bind(b.param, "button", b.choice)) hot.push_back({qr(b.rect), "button " + b.caption});
    for (const auto& s : F.selectors)
        if (bind(s.param, "selector")) {
            const std::string id = e->params()[static_cast<std::size_t>(s.param)].id;
            hot.push_back({qr(s.prev), "selector < " + id});
            hot.push_back({qr(s.next), "selector > " + id});
            hot.push_back({qr(s.box), "selector " + id});
        }
    if (spec.hasBypass) hot.push_back({qr(F.bypass), "POWER"});

    for (int p = 0; p < count; ++p)
        if (!seen[static_cast<std::size_t>(p)] && e->paramActive(p)) rep.add(name, "UI", "ORPHAN_PARAM", e->params()[static_cast<std::size_t>(p)].id, "no knob, button or selector");

    const hemqa::Rect bed = qr(ui::fxframe::kBed), emblem = qr(ui::fxframe::kEmblem);
    for (std::size_t i = 0; i < hot.size(); ++i) {
        if (!hemqa::within(hot[i].r, bed) || hemqa::overlaps(hot[i].r, emblem))
            rep.add(name, "UI", "OUT_OF_BOUNDS", hot[i].what, hemqa::fmt("x %.0f", hot[i].r.x) + hemqa::fmt(" y %.0f", hot[i].r.y) +
                    hemqa::fmt(" w %.0f", hot[i].r.w) + hemqa::fmt(" h %.0f", hot[i].r.h) + " (glass bed 230,230 3380x1690; emblem 1720,0 400x545)");
        for (std::size_t j = i + 1; j < hot.size(); ++j)
            if (hemqa::overlaps(hot[i].r, hot[j].r)) rep.add(name, "UI", "HITBOX_OVERLAP", hot[i].what + " / " + hot[j].what, "hit boxes collide");
    }
}

} // namespace

void runQaReport() {
    std::printf("[QA] HEM Suite: %zu effects — stability, sweep, meters, faceplate\n", fx::effects().size());
    hemqa::Report rep("HybridCore (HEM Suite)");
    for (const fx::EffectInfo& info : fx::effects()) {
        rep.plugin(info.name);
        auditDsp(info, rep);
        auditFace(info, rep);
        std::printf("[QA]   %s done\n", info.name);
        std::fflush(stdout);
    }
    const auto file = hemqa::outDir() / "qa_report_hybridcore.csv";
    std::printf("[QA] report: %s (%s)\n", file.string().c_str(), rep.write(file) ? "written" : "COULD NOT WRITE");
    rep.printSummary();
}

} // namespace hc::test
