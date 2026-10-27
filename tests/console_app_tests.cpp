// HEM Digital Console — screen model (every faceplate control is bound and reachable) and the player
// (transport, stem hand-off, listen choice, offline export, session file). Measured, printed, checked.
#include "TestHarness.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/ConsolePlayer.hpp"
#include "hybrid/console/StepEq.hpp"
#include "hybrid/console/TwinBus.hpp"
#include "hybrid/console/ui/ConsoleUi.hpp"
#include "hybrid/media/AudioFile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hc::test {
namespace {

using namespace hc::console;
namespace U = hc::console::ui;

std::unique_ptr<engine::StemAudio> toneStem(double hz, double seconds, double dbfs) {
    auto s = std::make_unique<engine::StemAudio>();
    s->frames = static_cast<std::uint64_t>(seconds * 48000.0);
    s->left.allocate(s->frames);
    s->right.allocate(s->frames);
    const double a = std::pow(10.0, dbfs / 20.0);
    for (std::uint64_t i = 0; i < s->frames; ++i) {
        const float v = static_cast<float>(a * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0));
        s->left.data()[i] = v;
        s->right.data()[i] = v;
    }
    s->name = "tone";
    return s;
}
double rmsDbOf(const float* x, std::size_t n) {
    double a = 0;
    for (std::size_t i = 0; i < n; ++i) a += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(a / static_cast<double>(std::max<std::size_t>(n, 1)), 1e-30));
}

} // namespace

void testConsoleUi() {
    std::printf("[console ui] faceplates: every control bound, reachable and inside the screen\n");
    auto rig = std::make_unique<ConsoleRig>();
    U::AppState app;
    const auto pages = U::buildPages(*rig, app);
    CHECK(static_cast<int>(pages.size()) == U::kPageCount);
    // every desk / unit / tool parameter must appear on some page
    std::set<const fx::ParamInfo*> bound;
    int widgets = 0, controls = 0;
    bool inside = true, valid = true;
    for (const auto& p : pages) {
        for (const auto& w : p.widgets) {
            ++widgets;
            if (w.ctl.info) { ++controls; bound.insert(w.ctl.info); if (!w.ctl.get || !w.ctl.set) valid = false; }
            if (w.r.x < 0 || w.r.y < 0 || w.r.right() > U::kWidth || w.r.bottom() > U::kHeight) inside = false;
        }
    }
    auto covered = [&](std::span<const fx::ParamInfo> ps, std::vector<std::string>& missingOut) {
        int n = 0;
        for (const auto& pi : ps) { if (bound.count(&pi)) ++n; else missingOut.push_back(pi.id); }
        return n;
    };
    std::vector<std::string> missDesk, missMaster, missEq, missSe, missTb, missCol, missReso, missSh, missEcho;
    const int nDesk = covered(StudioDesk::channelParams(), missDesk), nMaster = covered(StudioDesk::masterParams(), missMaster);
    const int nEq = covered(rig->stepEq().params(), missEq), nSe = covered(rig->stepEqSe().params(), missSe), nTb = covered(rig->twinBus().params(), missTb), nCol = covered(rig->colour().params(), missCol);
    const int nReso = covered(rig->channelTool(0).tool(ToolSlot::Reso).params(), missReso);
    const int nSh = covered(rig->channelTool(0).tool(ToolSlot::Shuffle).params(), missSh);
    const int nEcho = covered(rig->channelTool(0).tool(ToolSlot::Echo).params(), missEcho);
    std::printf("  %d widgets on %d pages, %d bound to controls; all inside the 3840x2160 canvas %s\n", widgets, U::kPageCount, controls, inside ? "yes" : "NO");
    std::printf("  step EQ SE on screen: %d/%zu\n", nSe, rig->stepEqSe().params().size());
    std::printf("  on screen: desk channel %d/%zu, desk master %d/%zu, step EQ %d/%zu, twin bus %d/%zu, colour %d/%zu, reso %d/%zu, shuffle %d/%zu, echo %d/%zu\n",
                nDesk, StudioDesk::channelParams().size(), nMaster, StudioDesk::masterParams().size(), nEq, rig->stepEq().params().size(),
                nTb, rig->twinBus().params().size(), nCol, rig->colour().params().size(), nReso, rig->channelTool(0).tool(ToolSlot::Reso).params().size(),
                nSh, rig->channelTool(0).tool(ToolSlot::Shuffle).params().size(), nEcho, rig->channelTool(0).tool(ToolSlot::Echo).params().size());
    CHECK(inside && valid);
    CHECK(missDesk.empty() && missMaster.empty() && missEq.empty() && missSe.empty() && missTb.empty() && missCol.empty() && missReso.empty() && missSh.empty() && missEcho.empty());

    // every clickable control is hit-tested back to itself at its centre (nothing hides another)
    int unreachable = 0, clickable = 0;
    for (int model : {ConsoleRig::EqSpecial, ConsoleRig::EqStandard}) {   // both STEP EQ models' panels
    rig->setEqModel(model);
    for (const auto& p : pages) {
        if (model == ConsoleRig::EqStandard && p.id != U::PageId::StepEq) continue;
        U::Controller c([](const U::ActionEvent&) {});
        c.setPage(&p);
        for (std::size_t i = 0; i < p.widgets.size(); ++i) {
            const auto& w = p.widgets[i];
            const bool click = w.kind == U::Kind::Knob || w.kind == U::Kind::Stepped || w.kind == U::Kind::Switch || w.kind == U::Kind::Fader ||
                               w.kind == U::Kind::Selector || w.kind == U::Kind::Button || w.kind == U::Kind::Seek;
            if (!click || !U::widgetVisible(w)) continue;
            ++clickable;
            if (c.hit(w.r.cx(), w.r.cy()) != static_cast<int>(i)) ++unreachable;
        }
    }
    }
    rig->setEqModel(ConsoleRig::EqSpecial);
    std::printf("  %d clickable controls (visible now), unreachable at their centre: %d\n", clickable, unreachable);
    CHECK(unreachable == 0);

    // driving the controls like a mouse would
    const auto& desk = pages[static_cast<std::size_t>(U::PageId::Desk)];
    U::Controller c([&](const U::ActionEvent&) {});
    c.setPage(&desk);
    auto find = [&](const U::Page& p, const fx::ParamInfo* info, int nth = 0) -> const U::Widget* {
        for (const auto& w : p.widgets) if (w.ctl.info == info && nth-- == 0) return &w;
        return nullptr;
    };
    const auto* hf3 = find(desk, &StudioDesk::channelParams()[StudioDesk::HfGain], 2);   // channel 3's HF knob
    c.mouseDown(hf3->r.cx(), hf3->r.cy(), false, false);
    c.mouseMove(hf3->r.cx(), hf3->r.cy() - U::Controller::kDragUnitsForFullRange * 0.25f, false);
    c.mouseUp(hf3->r.cx(), hf3->r.cy());
    const float hfAfterDrag = rig->desk().channel(2, StudioDesk::HfGain);
    c.mouseDown(hf3->r.cx(), hf3->r.cy(), true, false);   // double-click = start position
    const float hfAfterDouble = rig->desk().channel(2, StudioDesk::HfGain);
    const auto* bell = find(desk, &StudioDesk::channelParams()[StudioDesk::HfBell], 2);
    c.mouseDown(bell->r.cx(), bell->r.cy(), false, false); c.mouseUp(bell->r.cx(), bell->r.cy());
    const float bellOn = rig->desk().channel(2, StudioDesk::HfBell);
    const auto* fader = find(desk, &StudioDesk::channelParams()[StudioDesk::Fader], 0);
    const float y0 = fader->r.y + fader->r.h * (1.0f - U::faderTravel(-10.0f));
    c.mouseDown(fader->r.cx(), y0, false, false); c.mouseUp(fader->r.cx(), y0);
    const float faderDb = rig->desk().channel(0, StudioDesk::Fader);
    std::printf("  mouse: HF knob dragged a quarter of the travel up -> %+.2f dB (expected +7.50), double-click -> %+.2f; BELL click -> %.0f; fader clicked at the -10 mark -> %.2f dB\n",
                static_cast<double>(hfAfterDrag), static_cast<double>(hfAfterDouble), static_cast<double>(bellOn), static_cast<double>(faderDb));
    CHECK(std::fabs(hfAfterDrag - 7.5f) < 0.01f && hfAfterDouble == 0.0f && bellOn == 1.0f && std::fabs(faderDb + 10.0f) < 0.05f);
    // stepped knob: one wheel notch = one detent (the standard model's page)
    rig->setEqModel(ConsoleRig::EqStandard);
    const auto& eqp = pages[static_cast<std::size_t>(U::PageId::StepEq)];
    U::Controller ce([](const U::ActionEvent&) {});
    ce.setPage(&eqp);
    const auto* g = find(eqp, &rig->stepEq().params()[static_cast<std::size_t>(stepeq_ids::id(0, stepeq_ids::HiGain))]);
    ce.wheel(g->r.cx(), g->r.cy(), 1, false); ce.wheel(g->r.cx(), g->r.cy(), 1, false);
    const float step2 = rig->stepEq().get(stepeq_ids::id(0, stepeq_ids::HiGain));
    std::printf("  step EQ HI gain: two wheel notches from 0 dB -> step %.0f (%s)\n", static_cast<double>(step2), U::widgetValueText(*g).c_str());
    CHECK(step2 == 7.0f && U::widgetValueText(*g) == "+4 dB");
    // fader scale round trip
    float worst = 0;
    for (float db = -80; db <= 10; db += 0.5f) worst = std::max(worst, std::fabs(U::faderDb(U::faderTravel(db)) - db));
    std::printf("  fader scale: 0 dB at %.0f %% of the travel, -10 at %.0f %%, round trip error %.4f dB\n",
                100.0 * U::faderTravel(0), 100.0 * U::faderTravel(-10), static_cast<double>(worst));
    CHECK(worst < 1e-3f && std::fabs(U::faderTravel(0) - 0.78f) < 1e-6f);
    // buttons fire on release, the song bar seeks, a stem slot asks for a file
    std::vector<U::ActionEvent> got;
    U::Controller ca([&](const U::ActionEvent& e) { got.push_back(e); });
    ca.setPage(&desk);
    const U::Widget* play = nullptr; const U::Widget* seek = nullptr; const U::Widget* slot = nullptr;
    for (const auto& w : desk.widgets) {
        if (w.kind == U::Kind::Button && w.act == U::Act::Play) play = &w;
        if (w.kind == U::Kind::Seek) seek = &w;
        if (w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName && w.customArg == 4) slot = &w;
    }
    ca.mouseDown(play->r.cx(), play->r.cy(), false, false);
    const bool notYet = got.empty();
    ca.mouseUp(play->r.cx(), play->r.cy());
    ca.mouseDown(seek->r.x + seek->r.w * 0.25f, seek->r.cy(), false, false); ca.mouseUp(seek->r.x, seek->r.cy());
    ca.mouseDown(slot->r.cx(), slot->r.cy(), false, false); ca.mouseUp(slot->r.cx(), slot->r.cy());
    ca.rightDown(slot->r.cx(), slot->r.cy());
    const bool ok = got.size() == 4 && got[0].act == U::Act::Play && got[1].act == U::Act::Seek && std::fabs(got[1].value - 0.25f) < 0.01f &&
                    got[2].act == U::Act::LoadSlot && got[2].arg == 4 && got[3].act == U::Act::ClearSlot && got[3].arg == 4;
    std::printf("  actions: PLAY fires on release %s; song bar at 25 %% -> seek %.2f; channel 5's name strip -> load slot %d, right-click -> clear slot %d\n",
                notYet ? "yes" : "NO", got.size() > 1 ? static_cast<double>(got[1].value) : -1.0, got.size() > 2 ? got[2].arg : -1, got.size() > 3 ? got[3].arg : -1);
    CHECK(ok);
    // the plate: one routed pocket per control (the TOOLS page's changing panels are drawn live)
    const auto L = U::platePockets(desk);
    std::printf("  desk plate: %zu CNC pockets, %zu neon channels\n", L.pockets.size(), L.grooves.size());
    CHECK(L.pockets.size() > 400);

    // BIG DESK: five strips, every channel control on each, bound to whichever channel the strip shows
    {
        const auto& big = pages[static_cast<std::size_t>(U::PageId::DeskBig)];
        std::set<const fx::ParamInfo*> onBig;
        for (const auto& w : big.widgets) if (w.ctl.info) onBig.insert(w.ctl.info);
        int perChannel = 0;
        for (const auto& pi : StudioDesk::channelParams()) if (onBig.count(&pi)) ++perChannel;
        int hfKnobs = 0;
        for (const auto& w : big.widgets) if (w.ctl.info == &StudioDesk::channelParams()[StudioDesk::HfGain]) ++hfKnobs;
        const bool nav = U::bigDeskFirst(0, -5) == 0 && U::bigDeskFirst(0, 5) == 5 && U::bigDeskFirst(5, 5) == 8 && U::bigDeskFirst(0, 0, 12) == 8 &&
                         U::bigDeskFirst(0, 0, 2) == 0 && U::bigDeskFirst(8, 0, 1) == 0 && U::bigDeskFirst(3, 1) == 4;
        std::vector<U::ActionEvent> ev;
        U::Controller cb([&](const U::ActionEvent& e) { ev.push_back(e); });
        cb.setPage(&big);
        app.bigFirst = U::bigDeskFirst(app.bigFirst, 0, 10);   // as if channel 11's number was clicked
        const auto* hf2 = find(big, &StudioDesk::channelParams()[StudioDesk::HfGain], 1);   // the second strip
        const float before = rig->desk().channel(9, StudioDesk::HfGain);
        cb.mouseDown(hf2->r.cx(), hf2->r.cy(), false, false);
        cb.mouseMove(hf2->r.cx(), hf2->r.cy() - U::Controller::kDragUnitsForFullRange * 0.25f, false);
        cb.mouseUp(hf2->r.cx(), hf2->r.cy());
        const float after = rig->desk().channel(app.bigFirst + 1, StudioDesk::HfGain);
        const U::Widget* name2 = nullptr;
        for (const auto& w : big.widgets) if (w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName && !name2 && w.r.x > 700) name2 = &w;
        cb.mouseDown(name2->r.cx(), name2->r.cy(), false, false);
        std::printf("  big desk: %d of %zu channel controls on each of %d strips; showing channels %d-%d, strip 2's HF knob moved channel %d: %+.2f -> %+.2f dB; its name strip loads slot %d; paging %s\n",
                    perChannel, StudioDesk::channelParams().size(), hfKnobs, app.bigFirst + 1, app.bigFirst + U::kBigChannels, app.bigFirst + 2,
                    static_cast<double>(before), static_cast<double>(after), ev.empty() ? -1 : ev.back().arg, nav ? "ok" : "WRONG");
        CHECK(perChannel == static_cast<int>(StudioDesk::channelParams().size()) && hfKnobs == U::kBigChannels && nav);
        CHECK(app.bigFirst == 8 && std::fabs(after - 7.5f) < 0.01f && !ev.empty() && ev.back().act == U::Act::LoadSlot && ev.back().arg == 9);
    }
}

void testConsolePlayer() {
    std::printf("[console player] transport, stem hand-off, LISTEN, export, session file\n");
    auto rig = std::make_unique<ConsoleRig>();
    auto proc = std::make_unique<ConsoleProcessor>(*rig);
    io::StreamConfig cfg; cfg.sampleRate = 48000; cfg.blockFrames = 480; cfg.numOutputs = 2;
    proc->prepare(cfg);
    std::vector<float> oL(480), oR(480);
    float* outs[2] = {oL.data(), oR.data()};
    engine::ProcessContext ctx; ctx.outputs = outs; ctx.numOutputs = 2; ctx.frames = 480;
    auto block = [&] { proc->process(ctx); };
    auto stem = toneStem(1000, 2.0, -20);
    const bool queued = proc->submit(0, stem);
    for (int i = 0; i < 10; ++i) block();
    const bool silentStopped = rmsDbOf(oL.data(), 480) < -200;
    proc->play();
    for (int i = 0; i < 20; ++i) block();
    const double playing = rmsDbOf(oL.data(), 480);
    const auto head = proc->playhead();
    proc->pause();
    block(); block();
    const double paused = rmsDbOf(oL.data(), 480);
    proc->seek(48000); proc->play(); block();
    const auto afterSeek = proc->playhead();
    std::printf("  stem queued %s; stopped: silent %s; playing: %.2f dBFS (expected -23.01), playhead %llu after 20 blocks; paused: %.0f dBFS; seek to 1 s -> playhead %llu\n",
                queued ? "yes" : "NO", silentStopped ? "yes" : "NO", playing, static_cast<unsigned long long>(head), paused, static_cast<unsigned long long>(afterSeek));
    CHECK(queued && !stem && silentStopped && std::fabs(playing + 23.01) < 0.05 && head == 20 * 480 && paused < -200 && afterSeek == 48000 + 480);
    // the end of the song: loop off stops and goes back to the start, loop on wraps
    proc->seek(96000 - 960); block(); block(); block();
    const bool stoppedAtEnd = !proc->wantsPlay() && proc->playhead() == 0;
    proc->setLoop(true); proc->play(); proc->seek(96000 - 960); block(); block(); block();
    const bool wrapped = proc->wantsPlay() && proc->playhead() == 480;
    std::printf("  end of song: loop off stops at the start %s; loop on wraps around %s; song length %llu frames\n", stoppedAtEnd ? "yes" : "NO",
                wrapped ? "yes" : "NO", static_cast<unsigned long long>(proc->songFrames()));
    CHECK(stoppedAtEnd && wrapped && proc->songFrames() == 96000);
    // LISTEN: cue 1 (send at 0 dB, ON) vs monitor
    rig->desk().setChannel(0, StudioDesk::Cue1On, 1); rig->desk().setChannel(0, StudioDesk::Cue1Level, -6); rig->desk().setMaster(StudioDesk::Cut, 1);
    for (int i = 0; i < 20; ++i) block();
    const double cutMon = rmsDbOf(oL.data(), 480);
    proc->setListen(ConsoleProcessor::ListenCue1);
    for (int i = 0; i < 20; ++i) block();
    const double cue1 = rmsDbOf(oL.data(), 480);
    proc->setListen(ConsoleProcessor::ListenCue2);
    for (int i = 0; i < 20; ++i) block();
    const double cue2 = rmsDbOf(oL.data(), 480);
    std::printf("  LISTEN: monitor with CUT %.0f dBFS; CUE 1 (send -6 dB, centre pan) %.2f dBFS; CUE 2 (nothing sent) %.0f dBFS\n", cutMon, cue1, cue2);
    CHECK(cutMon < -100 && cue1 > -40 && cue1 < -20 && cue2 < -100);
    // EXT 1 return into the monitor
    proc->setListen(ConsoleProcessor::ListenMonitor);
    rig->desk().setMaster(StudioDesk::Cut, 0); rig->desk().setMaster(StudioDesk::MonMain, 0); rig->desk().setMaster(StudioDesk::MonExt1, 1);
    auto ref = toneStem(500, 2.0, -12);
    (void)proc->submit(kExt1, ref);
    for (int i = 0; i < 20; ++i) block();
    const double ext = rmsDbOf(oL.data(), 480);
    std::printf("  EXT 1 slot (reference, -12 dBFS tone) heard on MONITOR SOURCE EXT 1: %.2f dBFS\n", ext);
    CHECK(std::fabs(ext - (-12 - 3.0103)) < 0.1);
    // clearing a slot returns the old stem to the message thread, which frees it
    const auto liveBefore = engine::StemAudio::liveCount().load();
    std::unique_ptr<engine::StemAudio> none;
    (void)proc->submit(0, none);
    block();
    proc->collectRetired();
    const auto liveAfter = engine::StemAudio::liveCount().load();
    std::printf("  clearing a slot: live stems %lld -> %lld (freed on the message thread)\n", static_cast<long long>(liveBefore), static_cast<long long>(liveAfter));
    CHECK(liveAfter == liveBefore - 1);
    // zero allocation on the audio path while playing with the whole rig on
    rig->desk().setMaster(StudioDesk::MainInsert, 1);
    for (int ch = 0; ch < 13; ++ch) { rig->desk().setChannel(ch, StudioDesk::Insert, 1); rig->channelTool(ch).setType(1 + ch % 3); }
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int i = 0; i < 200; ++i) block();
        allocs = scope.count();
    }
    std::printf("  player audio path, 200 blocks, playing, rack + 13 tools on: %llu heap allocations\n", static_cast<unsigned long long>(allocs));
    CHECK(allocs == 0);
    proc.reset();

    // ---- export (offline), read back ----
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hem_console_export_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto rig2 = std::make_unique<ConsoleRig>();
    rig2->desk().setMaster(StudioDesk::MainInsert, 1);   // the rack (46 samples) is in the path: the file must still start on time
    rig2->twinBus().set(twinbus_ids::id(0, twinbus_ids::In), 0); rig2->twinBus().set(twinbus_ids::id(1, twinbus_ids::In), 0);
    rig2->desk().setChannel(1, StudioDesk::Fader, -6);
    ExportJob job;
    job.rigState = rig2->saveText();
    job.out = dir / "mix.wav";
    std::vector<float> click(48000, 0.0f), tone(48000 * 3);
    click[1000] = 0.5f;
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
    std::array<RenderSource, kPlayerSlots> src{};
    src[0] = {click.data(), click.data(), click.size()};
    src[1] = {tone.data(), tone.data(), tone.size()};
    int ticks = 0;
    auto r = renderExport(job, src, [&](float) { ++ticks; return true; });
    CHECK(r.has_value());
    if (r) {
        auto back = media::decodeFile(r->files[0]);
        CHECK(back.has_value());
        if (back) {
            std::size_t pk = 0;
            for (std::size_t i = 0; i < 4000; ++i) if (std::fabs(back->left[i]) > std::fabs(back->left[pk])) pk = i;
            const double toneDb = rmsDbOf(back->left.data() + 48000, 48000);
            std::printf("  export MIX: %s, %llu frames (%.2f s, the longest stem), click at sample %zu (in: 1000; rack latency %u removed), 440 Hz stem at fader -6: %.2f dBFS (expected %.2f), %d progress ticks, %.2f s wall\n",
                        back->format.c_str(), static_cast<unsigned long long>(back->frames()), static_cast<double>(back->frames()) / 48000.0, pk, r->latencyTrimmed,
                        toneDb, 20 * std::log10(0.1) - 3.0103 - 6, ticks, r->seconds);
            std::printf("  export MIX loudness (HEM METER BRIDGE): %.2f LUFS integrated, %.2f dBTP\n", r->integratedLufs, r->truePeakDbtp);
            CHECK(back->format == "WAV 24-bit" && back->frames() == 144000 && pk == 1000 && r->latencyTrimmed == 46 && std::fabs(toneDb - (20 * std::log10(0.1) - 3.0103 - 6)) < 0.05);
        }
    }
    job.kind = ExportJob::Kind::Stems;
    job.out = dir / "stems";
    job.names[0] = "Click"; job.names[1] = "Tone: A/440";
    rig2->desk().setChannel(1, StudioDesk::DirectPostFader, 1);
    job.rigState = rig2->saveText();
    auto rs = renderExport(job, src);
    CHECK(rs.has_value());
    if (rs) {
        std::string names;
        for (const auto& f : rs->files) names += f.filename().string() + " ";
        auto t = media::decodeFile(rs->files[1]);
        const double stemDb = t ? rmsDbOf(t->left.data() + 48000, 48000) : -999;
        std::printf("  PRINT STEMS: %zu files (%s), channel 2 direct out POST fader (-6): %.2f dBFS\n", rs->files.size(), names.c_str(), stemDb);
        CHECK(rs->files.size() == 2 && rs->files[1].filename() == "02 Tone_ A_440.wav" && std::fabs(stemDb - (20 * std::log10(0.1) - 3.0103 - 6)) < 0.05);
    }
    // ---- the background exporter: real files (MP3 44.1 kHz, FLAC 96 kHz) decoded, converted to 48 kHz, rendered ----
    {
        const fs::path td = testDataDir();
        ExportJob ej;
        ej.files[0] = td / "tone-44k1-stereo.mp3";
        ej.files[1] = td / "tone-96k-24bit.flac";
        ej.rigState = ConsoleRig().saveText();
        ej.out = dir / "bg.wav";
        ConsoleExporter ex;
        const bool started = ex.start(ej);
        const bool refused = !ex.start(ej);   // one at a time
        float maxProgress = 0;
        std::expected<ExportResult, std::string> got = std::unexpected(std::string("timeout"));
        for (int i = 0; i < 6000; ++i) {
            maxProgress = std::max(maxProgress, ex.progress());
            if (ex.takeResult(got)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (got) {
            auto back = media::decodeFile(got->files[0]);
            std::printf("  background export of the real test files: %s, %.2f s long, %.1f LUFS, progress reached %.0f %%, a second start while busy refused %s%s%s\n",
                        back ? back->format.c_str() : "unreadable", static_cast<double>(got->frames) / 48000.0, got->integratedLufs, 100.0 * maxProgress,
                        refused ? "yes" : "NO", got->warning.empty() ? "" : "; warning: ", got->warning.c_str());
            CHECK(started && refused && back.has_value() && back->sampleRate == 48000 && got->frames > 0 && got->warning.empty());
        } else {
            std::printf("  background export FAILED: %s\n", got.error().c_str());
            CHECK(false);
        }
    }

    // ---- session file ----
    SessionData sd;
    sd.files[0] = job.out.parent_path() / "mix.wav";
    sd.files[kExt2] = dir / "stems" / "01 Click.wav";
    sd.files[3] = dir / "gone.wav";
    sd.listen = 2; sd.loop = true;
    sd.rigState = rig2->saveText();
    const std::string text = writeSession(sd, dir / "song.hemc");
    std::vector<int> missing;
    auto back = readSession(text, dir / "song.hemc", &missing);
    bool same = back.has_value() && back->files[0] == sd.files[0] && back->files[kExt2] == sd.files[kExt2] && back->listen == 2 && back->loop && back->rigState == sd.rigState;
    // the folder moved: the relative paths find the files
    const fs::path moved = fs::temp_directory_path() / "hem_console_export_test_moved";
    fs::remove_all(moved, ec);
    fs::rename(dir, moved, ec);
    auto back2 = readSession(text, moved / "song.hemc");
    const bool rel = back2 && back2->files[0] == moved / "mix.wav";
    const bool junk = !readSession("hello", dir / "x.hemc").has_value();
    std::printf("  session file: %zu bytes, reads back the same %s, missing file reported (slot %d), moved folder found by relative path %s, foreign file refused %s\n",
                text.size(), same ? "yes" : "NO", missing.empty() ? -1 : missing[0] + 1, rel ? "yes" : "NO", junk ? "yes" : "NO");
    CHECK(same && missing.size() == 1 && missing[0] == 3 && rel && junk);
    fs::remove_all(moved, ec);
}

} // namespace hc::test
