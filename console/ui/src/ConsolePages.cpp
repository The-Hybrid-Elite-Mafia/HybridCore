// HEM DIGITAL CONSOLE — the six faceplates. Every widget is bound to a real control of the rig.
#include "hybrid/console/ui/ConsoleUi.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/StepEq.hpp"
#include "hybrid/console/StepEqSe.hpp"
#include "hybrid/console/TwinBus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace hc::console::ui {
namespace {

namespace C = colours;
using fx::ParamInfo;
using fx::Taper;

const char* const kOnOff[] = {"OFF", "ON"};
const ParamInfo kUnitInInfo{"unit_in", "Rack In", "", 0, 1, 1, Taper::Toggle, kOnOff};
const char* const kToolTypeN[] = {"OFF", "RESO", "SHUFFLE", "ECHO"};
const ParamInfo kToolTypeInfo{"tool", "Tool", "", 0, 3, 0, Taper::Choice, kToolTypeN};

struct OrderNames {
    std::array<std::string, ConsoleRig::kOrderCount> s;
    std::array<const char*, ConsoleRig::kOrderCount> p{};
    OrderNames() { for (int i = 0; i < ConsoleRig::kOrderCount; ++i) { s[static_cast<std::size_t>(i)] = ConsoleRig::orderName(i); p[static_cast<std::size_t>(i)] = s[static_cast<std::size_t>(i)].c_str(); } }
};
const OrderNames kOrderN;
const ParamInfo kRackOrderInfo{"rack_order", "Rack Order", "", 0, ConsoleRig::kOrderCount - 1, 0, Taper::Choice, kOrderN.p.data()};
const char* const kEqModelN[] = {"STANDARD", "SPECIAL EDITION"};
const ParamInfo kEqModelInfo{"eq_model", "Step EQ Model", "", 0, 1, 1, Taper::Choice, kEqModelN};
constexpr float kZoomSteps[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
const char* const kZoomN[] = {"FIT", "125 %", "150 %", "175 %", "200 %", "250 %", "300 %"};
const ParamInfo kZoomInfo{"zoom", "Zoom", "", 0, 6, 0, Taper::Choice, kZoomN};

// ---- bindings ------------------------------------------------------------------------------------
Ctl deskCh(ConsoleRig& rig, int ch, int p) {
    return {&StudioDesk::channelParams()[static_cast<std::size_t>(p)], [&rig, ch, p] { return rig.desk().channel(ch, p); },
            [&rig, ch, p](float v) { rig.desk().setChannel(ch, p, v); }};
}
Ctl deskM(ConsoleRig& rig, int p) {
    return {&StudioDesk::masterParams()[static_cast<std::size_t>(p)], [&rig, p] { return rig.desk().master(p); },
            [&rig, p](float v) { rig.desk().setMaster(p, v); }};
}
Ctl fxp(fx::Effect& e, int i) {
    return {&e.params()[static_cast<std::size_t>(i)], [&e, i] { return e.get(i); }, [&e, i](float v) { e.set(i, v); }};
}
Ctl unitIn(ConsoleRig& rig, int u) {
    return {&kUnitInInfo, [&rig, u] { return rig.unitIn(u) ? 1.0f : 0.0f; }, [&rig, u](float v) { rig.setUnitIn(u, v > 0.5f); }};
}
Ctl rackOrder(ConsoleRig& rig) {
    return {&kRackOrderInfo, [&rig] { return static_cast<float>(rig.rackOrder()); }, [&rig](float v) { rig.setRackOrder(static_cast<int>(std::lround(v))); }};
}
// BIG DESK: a control of whichever channel sits in strip `slot` right now.
Ctl deskBig(ConsoleRig& rig, const AppState& app, int slot, int p) {
    return {&StudioDesk::channelParams()[static_cast<std::size_t>(p)],
            [&rig, &app, slot, p] { return rig.desk().channel(app.bigFirst + slot, p); },
            [&rig, &app, slot, p](float v) { rig.desk().setChannel(app.bigFirst + slot, p, v); }};
}
Ctl zoomCtl(AppState& app) {
    return {&kZoomInfo,
            [&app] {
                int best = 0;
                for (int i = 1; i < 7; ++i) if (std::fabs(kZoomSteps[i] - app.zoom) < std::fabs(kZoomSteps[best] - app.zoom)) best = i;
                return static_cast<float>(best);
            },
            [&app](float v) { app.zoom = kZoomSteps[std::clamp(static_cast<int>(std::lround(v)), 0, 6)]; }};
}
Ctl toolType(ToolSlot& slot) {
    return {&kToolTypeInfo, [&slot] { return static_cast<float>(slot.type()); }, [&slot](float v) { slot.setType(static_cast<int>(std::lround(v))); }};
}
ToolSlot& targetSlot(ConsoleRig& rig, const AppState& app) {
    return app.toolTarget >= kColourToolTarget ? rig.colourTool() : rig.channelTool(std::clamp(app.toolTarget, 0, ConsoleRig::kChannels - 1));
}
// A control of the tool shown on the TOOLS page (follows the page's target).
Ctl toolP(ConsoleRig& rig, const AppState& app, int type, int i) {
    return {&rig.channelTool(0).tool(type).params()[static_cast<std::size_t>(i)],
            [&rig, &app, type, i] { return targetSlot(rig, app).tool(type).get(i); },
            [&rig, &app, type, i](float v) { targetSlot(rig, app).tool(type).set(i, v); }};
}

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string timeText(double s) {
    if (!(s >= 0)) s = 0;
    const int m = static_cast<int>(s / 60.0);
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%04.1f", m, s - 60.0 * m);
    return b;
}

// ---- builder ---------------------------------------------------------------------------------------
struct B {
    Page& p;
    Widget& add(Widget w) { p.widgets.push_back(std::move(w)); return p.widgets.back(); }
    Widget& knob(Rect r, std::string label, Ctl c, bool bipolar = false, Rgba col = C::lime) {
        Widget w; w.kind = Kind::Knob; w.r = r; w.label = std::move(label); w.ctl = std::move(c); w.bipolar = bipolar; w.colour = col;
        return add(std::move(w));
    }
    Widget& stepped(Rect r, std::string label, Ctl c, Rgba col = C::lime) {
        Widget w; w.kind = Kind::Stepped; w.r = r; w.label = std::move(label); w.ctl = std::move(c); w.colour = col;
        return add(std::move(w));
    }
    Widget& sw(Rect r, std::string label, Ctl c, Rgba lens = C::violet) {
        Widget w; w.kind = Kind::Switch; w.r = r; w.label = std::move(label); w.ctl = std::move(c); w.colour = lens;
        return add(std::move(w));
    }
    Widget& fader(Rect track, std::string label, Ctl c) {
        Widget w; w.kind = Kind::Fader; w.r = track; w.label = std::move(label); w.ctl = std::move(c);
        return add(std::move(w));
    }
    Widget& sel(Rect r, std::string label, Ctl c) {
        Widget w; w.kind = Kind::Selector; w.r = r; w.label = std::move(label); w.ctl = std::move(c);
        return add(std::move(w));
    }
    Widget& button(Rect r, std::string label, Act a, int arg = 0, Rgba col = C::violet) {
        Widget w; w.kind = Kind::Button; w.r = r; w.label = std::move(label); w.act = a; w.arg = arg; w.colour = col;
        return add(std::move(w));
    }
    Widget& leds(Rect r, int count, bool vertical, std::function<float()> v, std::vector<Rgba> cols) {
        Widget w; w.kind = Kind::Leds; w.r = r; w.count = count; w.vertical = vertical; w.value = std::move(v); w.ledColours = std::move(cols);
        return add(std::move(w));
    }
    Widget& bar(Rect r, float mn, float mx, bool vertical, std::function<float()> v, std::string label = {}) {
        Widget w; w.kind = Kind::LedBar; w.r = r; w.min = mn; w.max = mx; w.vertical = vertical; w.value = std::move(v); w.label = std::move(label);
        return add(std::move(w));
    }
    Widget& lamp(Rect r, std::string label, std::function<float()> v, Rgba col = C::red) {
        Widget w; w.kind = Kind::Lamp; w.r = r; w.label = std::move(label); w.value = std::move(v); w.colour = col;
        return add(std::move(w));
    }
    Widget& readout(Rect r, std::function<std::string()> t, Rgba col = C::lime) {
        Widget w; w.kind = Kind::Readout; w.r = r; w.text = std::move(t); w.colour = col;
        return add(std::move(w));
    }
    Widget& custom(Rect r, CustomId id, int arg = 0) {
        Widget w; w.kind = Kind::Custom; w.r = r; w.custom = id; w.customArg = arg;
        return add(std::move(w));
    }
    void section(Rect r, std::string title, Rgba col = C::violet) { p.sections.push_back({r, std::move(title), col}); }
    void engrave(std::string t, float x, float y, int align = 1, bool small = true, Rgba col = C::dim) {
        p.engravings.push_back({std::move(t), x, y, align, small, col});
    }
};

Rect sq(float cx, float cy, float d) { return {cx - d * 0.5f, cy - d * 0.5f, d, d}; }
Rect box(float cx, float cy, float w, float h) { return {cx - w * 0.5f, cy - h * 0.5f, w, h}; }

const std::vector<Rgba> kChannelLedCols{C::green, C::green, C::green, C::green, C::green, C::amber, C::amber, C::red};
const std::vector<Rgba> kMainLedCols{C::green, C::green, C::green, C::green, C::green, C::green, C::green, C::green, C::amber, C::amber, C::red, C::red};
const std::vector<Rgba> kGrLedCols{C::amber, C::amber, C::amber, C::amber, C::red};

// ---- common bars on every page --------------------------------------------------------------------
void topBar(B& b, ConsoleRig& rig, AppState& app, PageId id) {
    b.p.neon.push_back({30, 152, kWidth - 30, 152});
    b.engrave("HEM DIGITAL CONSOLE", 200, 106, 0, false, C::text);
    float x = 900;
    for (PageId t : kTabOrder) {
        Widget& w = b.button({x, 38, 220, 84}, pageName(t), Act::Page, static_cast<int>(t), C::violet);
        w.tip = t == PageId::DeskBig ? std::string("The desk, five channels at a time, big") : std::string("Show the ") + pageName(t) + " faceplate";
        x += 228;
    }
    (void)id;
    Widget& ord = b.sel({x + 8, 38, 460, 84}, "", rackOrder(rig));
    ord.tip = "Order of the three rack units behind the desk's MAIN INSERT";
    Widget& zm = b.sel({x + 484, 38, 280, 84}, "", zoomCtl(app));
    zm.text = [&app] { return app.zoom <= 1.001f ? std::string("ZOOM  FIT") : fmt("ZOOM  %.0f %%", 100.0 * app.zoom); };
    zm.tip = "ZOOM: everything bigger (also Ctrl + mouse wheel; Ctrl + 0 = fit). Drag the plate or use the wheel to move around";
    b.readout({x + 780, 38, kWidth - 30 - (x + 780), 84}, [&app] {
        std::string s = fmt("DSP %.1f %%", 100.0 * app.dspLoad);
        if (!app.status.empty()) s += "  " + app.status;
        return s;
    }, C::text);
}

void bottomBar(B& b, const AppState& app) {
    b.p.neon.push_back({30, 2004, kWidth - 30, 2004});
    const float y = 2026, h = 108;
    float x = 30;
    auto next = [&](float w) { const Rect r{x, y, w, h}; x += w + 10; return r; };
    b.button(next(250), "LOAD STEMS", Act::LoadStems).tip = "Load up to 13 stems (WAV, MP3, FLAC) in name order";
    b.button(next(110), "|<", Act::Rewind).tip = "Back to the start";
    Widget& play = b.button(next(170), "PLAY", Act::Play, 0, C::green);
    play.text = [&app] { return std::string(app.playing ? "PAUSE" : "PLAY"); };
    play.value = [&app] { return app.playing ? 1.0f : 0.0f; };
    b.button(next(140), "STOP", Act::Stop);
    Widget& loop = b.button(next(140), "LOOP", Act::Loop, 0, C::amber);
    loop.value = [&app] { return app.loop ? 1.0f : 0.0f; };
    b.readout(next(330), [&app] { return timeText(app.position) + " / " + timeText(app.length); });
    Widget& seek = b.add([&] { Widget w; w.kind = Kind::Seek; w.r = next(690); w.act = Act::Seek; return w; }());
    seek.value = [&app] { return app.length > 0 ? static_cast<float>(app.position / app.length) : 0.0f; };
    seek.tip = "Song position: click or drag to jump";
    Widget& listen = b.button(next(330), "LISTEN", Act::Listen, 0, C::cyan);
    listen.text = [&app] { static const char* n[] = {"HEAR: MONITOR", "HEAR: CUE 1", "HEAR: CUE 2"}; return std::string(n[std::clamp(app.listen, 0, 2)]); };
    listen.tip = "What your speakers / headset play: the MONITOR section, or a cue mix (the PHONES source)";
    Widget& ex = b.button(next(220), "EXPORT", Act::Export, 0, C::lime);
    ex.text = [&app] { return app.exportProgress >= 0 ? fmt("%.0f %%", 100.0 * app.exportProgress) : std::string("EXPORT"); };
    ex.tip = "Render the MAIN OUT to a WAV file (offline, the same sound as playback)";
    b.button(next(240), "PRINT STEMS", Act::PrintStems).tip = "Write each channel's DIRECT OUT to its own WAV file";
    b.button(next(120), "NEW", Act::NewSession);
    b.button(next(130), "OPEN", Act::OpenSession);
    Widget& save = b.button(next(130), "SAVE", Act::SaveSession);
    save.value = [&app] { return app.dirty ? 1.0f : 0.0f; };
    Widget& reset = b.button(next(170), "RESET", Act::ResetAll, 0, C::red);
    reset.tip = "Every control back to its starting position (press twice; UNDO for 15 s after)";
    reset.text = [&app] { return std::string(app.resetStage == 1 ? "SURE?" : app.resetStage == 2 ? "UNDO" : "RESET"); };
    reset.value = [&app] { return app.resetStage != 0 ? 1.0f : 0.0f; };
    Widget& dev = b.button({x, y, kWidth - 30 - x, h}, "DEVICE", Act::Device, 0, C::cyan);
    dev.text = [&app] { return app.device; };
    dev.value = [&app] { return app.alert ? 1.0f : 0.0f; };
    dev.tip = "Output device (click to choose)";
}

// ---- DESK ------------------------------------------------------------------------------------------
void deskPage(B& b, ConsoleRig& rig, const AppState& app) {
    using D = StudioDesk;
    constexpr float kTop = 165, kPitch = 202, kW = 194;
    for (int ch = 0; ch < ConsoleRig::kChannels; ++ch) {
        const float x = 30 + kPitch * static_cast<float>(ch);
        const float c0 = x + 50, c1 = x + 143;
        b.section({x, kTop, kW, 1830}, "");
        b.engrave(std::to_string(ch + 1), x + kW * 0.5f, 205, 1, false, C::violet);
        // TOOL slot (where the reference desk has its microphone preamp)
        Widget& tool = b.sel({x + 6, 216, 134, 48}, "", toolType(rig.channelTool(ch)));
        tool.tip = "TOOL behind this channel's INSERT switch";
        b.button({x + 144, 216, 44, 48}, "ED", Act::EditTool, ch).tip = "Open this channel's tool on the TOOLS page";
        // input
        b.knob(sq(c0, 344, 76), "TRIM", deskCh(rig, ch, D::Trim), false);
        b.sw(box(c1, 308, 94, 36), "MONO", deskCh(rig, ch, D::Mono), C::amber);
        b.sw(box(c1, 352, 94, 36), "\xC3\x98", deskCh(rig, ch, D::Polarity), C::amber);   // Ø
        b.sw(box(c1, 396, 94, 36), "HPF", deskCh(rig, ch, D::Hpf), C::amber);
        // cue sends
        b.knob(sq(c0, 489, 66), "CUE 1", deskCh(rig, ch, D::Cue1Level), false, C::cyan);
        b.knob(sq(c1, 489, 66), "PAN", deskCh(rig, ch, D::Cue1Pan), true, C::cyan);
        b.sw(box(c0, 546, 84, 32), "ON", deskCh(rig, ch, D::Cue1On), C::cyan);
        b.sw(box(c1, 546, 94, 32), "POST", deskCh(rig, ch, D::Cue1Post), C::cyan);
        b.knob(sq(c0, 633, 66), "CUE 2", deskCh(rig, ch, D::Cue2Level), false, C::cyan);
        b.knob(sq(c1, 633, 66), "PAN", deskCh(rig, ch, D::Cue2Pan), true, C::cyan);
        b.sw(box(c0, 690, 84, 32), "ON", deskCh(rig, ch, D::Cue2On), C::cyan);
        b.sw(box(c1, 690, 94, 32), "POST", deskCh(rig, ch, D::Cue2Post), C::cyan);
        // EQ
        Widget& hf = b.knob(sq(c0, 786, 76), "HF", deskCh(rig, ch, D::HfGain), true);
        hf.enabled = [&rig, ch] { return rig.desk().channel(ch, D::EqIn) > 0.5f; };
        b.sw(box(c1, 786, 94, 36), "BELL", deskCh(rig, ch, D::HfBell), C::lime);
        Widget& mf = b.knob(sq(c0, 894, 76), "MF", deskCh(rig, ch, D::MfGain), true);
        mf.enabled = hf.enabled;
        b.sw(box(c1, 894, 94, 36), "EQ IN", deskCh(rig, ch, D::EqIn), C::lime);
        Widget& lf = b.knob(sq(c0, 1002, 76), "LF", deskCh(rig, ch, D::LfGain), true);
        lf.enabled = hf.enabled;
        b.sw(box(c1, 1002, 94, 36), "BELL", deskCh(rig, ch, D::LfBell), C::lime);
        // compressor
        Widget& thr = b.knob(sq(c0, 1110, 76), "COMP", deskCh(rig, ch, D::CompThreshold), false, C::amber);
        thr.enabled = [&rig, ch] { return rig.desk().channel(ch, D::CompIn) > 0.5f; };
        b.sw(box(c1, 1092, 94, 36), "IN", deskCh(rig, ch, D::CompIn), C::amber);
        b.leds(box(c1, 1136, 76, 18), 3, false, [&rig, ch] { return static_cast<float>(D::channelCompLeds(rig.desk().channelCompGrDb(ch))); },
               {C::amber, C::amber, C::red}).tip = "Compressor gain reduction: 3, 6, 10 dB";
        // output
        b.knob(sq(c0, 1218, 76), "PAN", deskCh(rig, ch, D::Pan), true);
        b.sw(box(c1, 1198, 94, 36), "INS", deskCh(rig, ch, D::Insert), C::violet).tip = "INSERT: run this channel's TOOL";
        b.sw(box(c1, 1240, 94, 36), "D.POST", deskCh(rig, ch, D::DirectPostFader), C::violet).tip = "Direct out (PRINT STEMS) after the fader";
        b.sw({x + 8, 1272, 86, 38}, "BUS B", deskCh(rig, ch, D::MuteBusB), C::red).tip = "MUTE / BUS B: off the main mix, onto Bus B";
        b.sw({x + 100, 1272, 86, 38}, "PFL", deskCh(rig, ch, D::Pfl), C::amber);
        // fader + meter
        const float fx = x + 62;
        constexpr float kFy = 1352, kFh = 524;
        b.fader({fx - 12, kFy, 24, kFh}, "", deskCh(rig, ch, D::Fader));
        for (float db : {10.0f, 0.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f}) {
            const float y = kFy + kFh * (1.0f - faderTravel(db));
            b.engrave(db > 0 ? "+10" : std::to_string(static_cast<int>(db)), x + 112, y + 8, 1, true);
        }
        for (int side = 0; side < 2; ++side)
            b.leds({x + 140 + 26.0f * static_cast<float>(side), kFy + 4, 20, kFh - 8}, 8, true,
                   [&rig, ch, side] { return static_cast<float>(D::channelMeterLeds(rig.desk().channelPeakDbu(ch, side))); }, kChannelLedCols)
                .tip = "Channel meter (dBu): -21, -15, -6, 0, +6, +12, +18, +24";
        b.custom({x + 6, 1906, kW - 12, 80}, CustomId::StemName, ch).tip = "Click: load a file into this channel. Right-click: empty it";
        (void)app;
    }

    // ---- master section ----
    const float mx = 2672;
    // bus compressor
    b.section({mx, kTop, 374, 560}, "BUS COMPRESSOR");
    Widget& bthr = b.knob(sq(mx + 100, 340, 110), "THRESH", deskM(rig, D::BusCompThreshold), false, C::amber);
    bthr.enabled = [&rig] { return rig.desk().master(D::BusCompIn) > 0.5f; };
    Widget& bmk = b.knob(sq(mx + 274, 340, 110), "MAKE UP", deskM(rig, D::BusCompMakeup), false, C::amber);
    bmk.enabled = bthr.enabled;
    b.sw({mx + 22, 418, 156, 48}, "AUTO REL", deskM(rig, D::BusCompAutoRelease), C::amber);
    b.sw({mx + 196, 418, 156, 48}, "IN", deskM(rig, D::BusCompIn), C::amber);
    b.leds({mx + 40, 492, 294, 24}, 5, false, [&rig] { return static_cast<float>(D::busCompLeds(rig.desk().busCompGrDb())); }, kGrLedCols)
        .tip = "Bus compressor gain reduction: 1, 3, 6, 9, 15 dB";
    {
        const char* marks[] = {"1", "3", "6", "9", "15"};
        for (int i = 0; i < 5; ++i) b.engrave(marks[i], mx + 40 + 294.0f * (static_cast<float>(i) + 0.5f) / 5.0f, 546, 1, true);
    }
    b.sw({mx + 22, 566, 330, 48}, "CUE 1 > COMP", deskM(rig, D::Cue1ToBusComp), C::cyan).tip = "ST CUE 1 to the bus compressor & sum to main";
    b.sw({mx + 22, 636, 330, 60}, "MAIN INSERT", deskM(rig, D::MainInsert), C::violet).tip = "Send the main bus through the RACK (STEP EQ, TWIN BUS, COLOUR STAGE)";
    // EXT returns
    b.section({mx, 740, 374, 420}, "EXT RETURNS");
    b.knob(sq(mx + 100, 900, 96), "EXT 1", deskM(rig, D::Ext1Level), false, C::cyan);
    b.knob(sq(mx + 274, 900, 96), "EXT 2", deskM(rig, D::Ext2Level), false, C::cyan);
    b.sw({mx + 22, 966, 156, 44}, "TO MAIN", deskM(rig, D::Ext1ToMain), C::cyan);
    b.sw({mx + 196, 966, 156, 44}, "TO MAIN", deskM(rig, D::Ext2ToMain), C::cyan);
    b.custom({mx + 14, 1028, 172, 118}, CustomId::StemName, kExt1Slot).tip = "Click: load a file into EXT 1 (a reference, a cue track ...)";
    b.custom({mx + 188, 1028, 172, 118}, CustomId::StemName, kExt2Slot).tip = "Click: load a file into EXT 2";
    // Bus B
    b.section({mx, 1180, 374, 236}, "BUS B");
    b.knob(sq(mx + 100, 1338, 96), "LEVEL", deskM(rig, D::BusBLevel), false, C::red);
    b.sw({mx + 196, 1316, 156, 46}, "MUTE", deskM(rig, D::BusBMute), C::red);
    // rack units (behind MAIN INSERT)
    b.section({mx, 1436, 374, 210}, "RACK IN");
    b.sw({mx + 14, 1490, 168, 52}, "EQ", unitIn(rig, ConsoleRig::UnitStepEq), C::violet).tip = "STEP EQ in the rack";
    b.sw({mx + 192, 1490, 168, 52}, "BUS", unitIn(rig, ConsoleRig::UnitTwinBus), C::violet).tip = "TWIN BUS in the rack";
    b.sw({mx + 14, 1556, 168, 52}, "COLOUR", unitIn(rig, ConsoleRig::UnitColour), C::violet).tip = "COLOUR STAGE in the rack";
    b.lamp({mx + 206, 1568, 28, 28}, "LIVE", [&rig] { return rig.desk().master(D::MainInsert) > 0.5f ? 1.0f : 0.0f; }, C::violet);

    // cues + monitor
    const float cx = 3060;
    b.section({cx, kTop, 370, 320}, "ST CUE 1");
    b.knob(sq(cx + 185, 330, 104), "MASTER", deskM(rig, D::Cue1Master), false, C::cyan);
    b.sw({cx + 20, 410, 160, 44}, "+ EXT 1", deskM(rig, D::Cue1Ext1), C::cyan);
    b.sw({cx + 190, 410, 160, 44}, "+ EXT 2", deskM(rig, D::Cue1Ext2), C::cyan);
    b.section({cx, 500, 370, 320}, "ST CUE 2");
    b.knob(sq(cx + 185, 665, 104), "MASTER", deskM(rig, D::Cue2Master), false, C::cyan);
    b.sw({cx + 20, 745, 160, 44}, "+ EXT 1", deskM(rig, D::Cue2Ext1), C::cyan);
    b.sw({cx + 190, 745, 160, 44}, "+ EXT 2", deskM(rig, D::Cue2Ext2), C::cyan);
    b.section({cx, 835, 370, 290}, "MONITOR SOURCE");
    b.sw({cx + 20, 900, 160, 50}, "MAIN", deskM(rig, D::MonMain), C::lime);
    b.sw({cx + 190, 900, 160, 50}, "BUS B", deskM(rig, D::MonBusB), C::lime);
    b.sw({cx + 20, 962, 160, 50}, "EXT 1", deskM(rig, D::MonExt1), C::lime);
    b.sw({cx + 190, 962, 160, 50}, "EXT 2", deskM(rig, D::MonExt2), C::lime);
    b.lamp({cx + 30, 1052, 30, 30}, "PFL", [&rig] { return rig.desk().pflActive() ? 1.0f : 0.0f; }, C::amber);
    b.section({cx, 1140, 370, 506}, "MONITOR");
    b.knob(sq(cx + 92, 1290, 96), "DIM LEVEL", deskM(rig, D::DimLevel), false, C::amber);
    b.knob(sq(cx + 262, 1300, 140), "LEVEL", deskM(rig, D::MonitorLevel), false, C::lime);
    b.sw({cx + 20, 1404, 104, 48}, "DIM", deskM(rig, D::Dim), C::amber);
    b.sw({cx + 133, 1404, 104, 48}, "CUT", deskM(rig, D::Cut), C::red);
    b.sw({cx + 246, 1404, 104, 48}, "MONO", deskM(rig, D::MonoMonitor), C::amber);
    b.sw({cx + 20, 1470, 330, 48}, "ALT", deskM(rig, D::Alt), C::cyan).tip = "ALT: the monitor feed goes to the ALT output (with one output device, you hear the same)";

    // main meter + main fader
    const float gx = 3444;
    b.section({gx, kTop, 366, 1481}, "MAIN");
    for (int side = 0; side < 2; ++side)
        b.leds({gx + 26 + 34.0f * static_cast<float>(side), 240, 26, 840}, 12, true,
               [&rig, side] { return static_cast<float>(D::mainMeterLeds(rig.desk().mainMeterDbu(side))); }, kMainLedCols)
            .tip = "Main meter (dBu, follows the monitor source)";
    {
        const char* marks[] = {"-21", "-15", "-9", "-4", "0", "+6", "+9", "+12", "+15", "+18", "+21", "+24"};
        for (int i = 0; i < 12; ++i) b.engrave(marks[i], gx + 132, 240 + 840.0f * (1.0f - (static_cast<float>(i) + 0.5f) / 12.0f) + 8, 1, true);
    }
    b.engrave("L", gx + 39, 1110, 1, true); b.engrave("R", gx + 73, 1110, 1, true);
    const float mfx = gx + 270;
    b.fader({mfx - 14, 240, 28, 1060}, "MAIN", deskM(rig, D::MainFader));
    for (float db : {10.0f, 0.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f}) {
        const float y = 240 + 1060 * (1.0f - faderTravel(db));
        b.engrave(db > 0 ? "+10" : std::to_string(static_cast<int>(db)), gx + 196, y + 8, 1, true);
    }
    b.engrave("MAIN FADER", mfx, 1350, 1, true, C::text);
    // loudness strip under the master section
    b.custom({mx, 1664, kWidth - 30 - mx, 330}, CustomId::DeskLoud).tip = "HEM METER BRIDGE on the main out (full screen: METER BRIDGE page)";
}

// ---- STEP EQ ----------------------------------------------------------------------------------------
void stepEqPage(B& b, ConsoleRig& rig) {
    namespace E = stepeq_ids;
    namespace X = stepeqse_ids;
    fx::Effect& eq = rig.stepEq();
    fx::Effect& se = rig.stepEqSe();
    auto isStd = [&rig] { return rig.eqModel() == ConsoleRig::EqStandard; };
    auto isSe = [&rig] { return rig.eqModel() == ConsoleRig::EqSpecial; };
    b.engrave("HEM STEP EQ", 60, 250, 0, false, C::lime);
    b.engrave("2-CHANNEL 4-BAND STEPPED EQ", 60, 300, 0, true, C::dim);
    Widget& model = b.sel({1000, 196, 760, 84}, "MODEL", {&kEqModelInfo, [&rig] { return static_cast<float>(rig.eqModel()); },
                                                         [&rig](float v) { rig.setEqModel(static_cast<int>(std::lround(v))); }});
    model.tip = "Which model sits in the rack: STANDARD (7 steps, RANGE) or SPECIAL EDITION (21 steps per band, HPF, M/S). Each keeps its own settings";
    Widget& ms = b.sw({2540, 196, 320, 84}, "M/S", fxp(se, X::MidSide), C::cyan);
    ms.visible = isSe;
    ms.tip = "M/S: channel 1 EQs the MID, channel 2 the SIDE, recombined at the output";
    b.sw({2900, 196, 300, 84}, "RACK IN", unitIn(rig, ConsoleRig::UnitStepEq), C::violet);
    Widget& byS = b.sw({3240, 196, 300, 84}, "BYPASS", fxp(eq, E::Bypass), C::red);
    byS.visible = isStd;
    Widget& byE = b.sw({3240, 196, 300, 84}, "BYPASS", fxp(se, X::Bypass), C::red);
    byE.visible = isSe;
    const char* bandName[] = {"LO", "LO-MID", "HI-MID", "HI"};
    const int gainId[] = {E::LoGain, E::LoMidGain, E::HiMidGain, E::HiGain};
    const int freqId[] = {E::LoFreq, E::LoMidFreq, E::HiMidFreq, E::HiFreq};
    const int gainSe[] = {X::LoGain, X::LoMidGain, X::HiMidGain, X::HiGain};
    const int freqSe[] = {X::LoFreq, X::LoMidFreq, X::HiMidFreq, X::HiFreq};
    for (int c = 0; c < 2; ++c) {
        const float X0 = 40 + 1890.0f * static_cast<float>(c);
        b.section({X0, 340, 1870, 1640}, c == 0 ? "CHANNEL 1" : "CHANNEL 2");
        Widget& msCap = b.readout({X0 + 330, 352, 420, 56}, [c] { return std::string(c == 0 ? "= MID" : "= SIDE"); }, C::cyan);
        msCap.visible = [&rig, &se] { return rig.eqModel() == ConsoleRig::EqSpecial && se.get(stepeqse_ids::MidSide) > 0.5f; };
        for (int band = 0; band < 4; ++band) {
            const float cx = X0 + 250 + 405.0f * static_cast<float>(band);
            // standard model
            auto en = [&eq, c] { return eq.get(E::id(c, E::In)) > 0.5f && eq.get(E::Bypass) < 0.5f; };
            Widget& g = b.stepped(sq(cx, 730, 250), bandName[band], fxp(eq, E::id(c, gainId[band])));
            g.tip = std::string(bandName[band]) + " GAIN"; g.enabled = en; g.bipolar = true; g.visible = isStd;
            Widget& f = b.stepped(sq(cx, 1270, 210), "FREQ", fxp(eq, E::id(c, freqId[band])), C::cyan);
            f.tip = std::string(bandName[band]) + " FREQUENCY"; f.enabled = en; f.visible = isStd;
            if (band == 0) b.sw(box(cx, 1700, 200, 70), "SHELF", fxp(eq, E::id(c, E::LoShelf)), C::lime).visible = isStd;
            if (band == 3) b.sw(box(cx, 1700, 200, 70), "SHELF", fxp(eq, E::id(c, E::HiShelf)), C::lime).visible = isStd;
            // special edition
            auto enSe = [&se, c] { return se.get(X::id(c, X::In)) > 0.5f && se.get(X::Bypass) < 0.5f; };
            Widget& gs = b.stepped(sq(cx, 730, 250), bandName[band], fxp(se, X::id(c, gainSe[band])));
            gs.tip = std::string(bandName[band]) + " GAIN (21 steps)"; gs.enabled = enSe; gs.bipolar = true; gs.visible = isSe;
            Widget& fs = b.stepped(sq(cx, 1270, 210), "FREQ", fxp(se, X::id(c, freqSe[band])), C::cyan);
            fs.tip = std::string(bandName[band]) + " FREQUENCY"; fs.enabled = enSe; fs.visible = isSe;
            if (band == 0) b.sw(box(cx, 1700, 200, 70), "SHELF", fxp(se, X::id(c, X::LoShelf)), C::lime).visible = isSe;
            if (band == 3) b.sw(box(cx, 1700, 200, 70), "SHELF", fxp(se, X::id(c, X::HiShelf)), C::lime).visible = isSe;
        }
        const float kx = X0 + 1750;
        b.sw(box(kx, 560, 170, 70), "IN", fxp(eq, E::id(c, E::In)), C::lime).visible = isStd;
        b.stepped(sq(kx, 1000, 150), "RANGE", fxp(eq, E::id(c, E::Range)), C::amber).visible = isStd;
        b.sw(box(kx, 560, 170, 70), "IN", fxp(se, X::id(c, X::In)), C::lime).visible = isSe;
        Widget& hk = b.knob(sq(kx, 1000, 150), "HPF", fxp(se, X::id(c, X::HpfHz)), false, C::amber);
        hk.visible = isSe;
        hk.enabled = [&se, c] { return se.get(X::id(c, X::HpfOn)) > 0.5f && se.get(X::id(c, X::In)) > 0.5f; };
        hk.tip = "HIGH-PASS FILTER frequency (40 .. 600 Hz, 12 dB/oct)";
        b.sw(box(kx, 1200, 170, 70), "HP-FILT", fxp(se, X::id(c, X::HpfOn)), C::amber).visible = isSe;
    }
}

// ---- TWIN BUS ---------------------------------------------------------------------------------------
void twinBusPage(B& b, ConsoleRig& rig) {
    namespace T = twinbus_ids;
    fx::Effect& tb = rig.twinBus();
    b.engrave("HEM TWIN BUS", 60, 250, 0, false, C::lime);
    b.engrave("2-CHANNEL BUS COMPRESSOR + DYNAMIC EQ", 60, 300, 0, true, C::dim);
    for (int s = 0; s < 2; ++s) {
        const float X = s == 0 ? 40.0f : 2160.0f;
        auto P = [&](int p) { return fxp(tb, T::id(s, p)); };
        auto sideOn = [&tb, s] { return tb.get(T::id(s, T::In)) > 0.5f; };
        // the right side's controls do nothing in the stereo modes (except its GAIN = HF gain), like the reference
        auto rightLive = [&tb, s, sideOn] { return sideOn() && (s == 0 || static_cast<int>(tb.get(T::Mode) + 0.5f) >= 2); };
        b.section({X, 340, 1640, 520}, s == 0 ? "CH 1 COMPRESSOR" : "CH 2 COMPRESSOR");
        const char* names[] = {"THRESHOLD", "ATTACK", "RATIO", "RELEASE", "MAKE-UP", "MIX", "S/C HPF"};
        const int ids[] = {T::Threshold, T::Attack, T::Ratio, T::Release, T::Makeup, T::Mix, T::ScHpf};
        for (int k = 0; k < 7; ++k) {
            Widget& w = b.stepped(sq(X + 125 + 232.0f * static_cast<float>(k), 545, k == 0 ? 160.0f : 136.0f), names[k], P(ids[k]), k == 0 ? C::amber : C::lime);
            w.enabled = rightLive;
        }
        b.sw({X + 50, 700, 150, 60}, "IN", P(T::In), C::lime);
        b.sw({X + 975, 700, 150, 60}, "FINE", P(T::Fine), C::lime).enabled = rightLive;
        b.sw({X + 1207, 700, 160, 60}, "POST D-EQ", P(T::PostDeq), C::lime).enabled = rightLive;
        b.section({X, 880, 1640, 240}, "COLOUR");
        b.sw({X + 50, 960, 190, 64}, "LOW THD", P(T::LowThd), C::amber).enabled = rightLive;
        b.sw({X + 270, 960, 190, 64}, "F/B", P(T::Feedback), C::amber).enabled = rightLive;
        b.sw({X + 490, 960, 190, 64}, "GRIT", P(T::Grit), C::amber).enabled = rightLive;
        Widget& gl = b.stepped(sq(X + 800, 992, 104), "LEVEL", P(T::GritLevel), C::amber);
        gl.enabled = [&tb, s, rightLive] { return rightLive() && tb.get(T::id(s, T::Grit)) > 0.5f; };
        b.sw({X + 1100, 960, 260, 64}, "TRANSIENT EXP", P(T::TransientExp), C::cyan).enabled = rightLive;
        for (int band = 0; band < 2; ++band) {
            const float Y = band == 0 ? 1136.0f : 1556.0f;
            const bool lf = band == 0;
            b.section({X, Y, 1640, 404}, lf ? "DYNAMIC EQ  LF" : "DYNAMIC EQ  HF");
            auto bandOn = [&tb, s, lf, rightLive] { return rightLive() && tb.get(T::id(s, lf ? T::LfOn : T::HfOn)) > 0.5f; };
            b.sw({X + 50, Y + 90, 150, 60}, "ON", P(lf ? T::LfOn : T::HfOn), C::lime).enabled = rightLive;
            Widget& ce = b.stepped(sq(X + 360, Y + 230, 160), "COMP / EXP", P(lf ? T::LfCompExp : T::HfCompExp), C::amber);
            ce.bipolar = true; ce.enabled = bandOn;
            b.stepped(sq(X + 590, Y + 230, 136), "FREQ", P(lf ? T::LfFreq : T::HfFreq), C::cyan).enabled = bandOn;
            b.stepped(sq(X + 800, Y + 230, 136), "RANGE", P(lf ? T::LfRange : T::HfRange), C::lime).enabled = bandOn;
            b.sel({X + 960, Y + 200, 230, 64}, "SPEED", P(lf ? T::LfTime : T::HfTime)).enabled = bandOn;
            if (lf) {
                Widget& g = b.stepped(sq(X + 1340, Y + 230, 136), s == 0 ? "LF GAIN" : "GAIN", P(T::LfGain), C::lime);
                g.bipolar = true; g.enabled = sideOn;
                g.tip = s == 0 ? "LF GAIN (static)" : "Right GAIN: HF gain in the stereo modes, LF gain in DUAL MONO / MID-SIDE";
                b.sw({X + 1250, Y + 316, 180, 56}, "RESO", P(T::Reso), C::amber).enabled = rightLive;
            } else {
                b.sw({X + 1250, Y + 200, 180, 64}, "BELL", P(T::HfBell), C::amber).enabled = bandOn;
            }
            b.lamp({X + 1500, Y + 220, 40, 40}, "", [&tb, s, lf] { return std::fabs(tb.meter(s == 0 ? (lf ? T::MeterLf1 : T::MeterHf1) : (lf ? T::MeterLf2 : T::MeterHf2))) > 0.5f ? 1.0f : 0.0f; },
                   C::amber).tip = "D-EQ working";
        }
    }
    // centre column: meters + global switches
    const float cx = 1700;
    b.section({cx, 340, 440, 1640}, "");
    b.custom({cx + 15, 370, 410, 300}, CustomId::GrNeedle, 0).tip = "Gain reduction, CH 1";
    b.custom({cx + 15, 690, 410, 300}, CustomId::GrNeedle, 1).tip = "Gain reduction, CH 2";
    b.lamp({cx + 40, 1010, 34, 34}, "OVER 1", [&tb] { return tb.meter(T::MeterOver1); }, C::red);
    b.lamp({cx + 250, 1010, 34, 34}, "OVER 2", [&tb] { return tb.meter(T::MeterOver2); }, C::red);
    b.sel({cx + 20, 1100, 400, 70}, "MODE", fxp(tb, T::Mode));
    b.sel({cx + 20, 1215, 400, 64}, "MIX MODE", fxp(tb, T::MixMode));
    b.sel({cx + 20, 1325, 400, 64}, "INPUT TRIM", fxp(tb, T::InputTrim));
    b.sw({cx + 20, 1420, 190, 60}, "LPF 26k", fxp(tb, T::Lpf), C::amber);
    b.sw({cx + 230, 1420, 190, 60}, "R METER", fxp(tb, T::RightMeter), C::amber).tip = "Right meter shows the right channel in the stereo modes";
    b.sw({cx + 20, 1520, 190, 60}, "MID SOLO", fxp(tb, T::MidSolo), C::cyan);
    b.sw({cx + 230, 1520, 190, 60}, "SIDE SOLO", fxp(tb, T::SideSolo), C::cyan);
    b.sel({cx + 20, 1610, 400, 60}, "SIDE SOLO", fxp(tb, T::SideSoloMode)).tip = "Side solo: in phase or decoded";
    b.sel({cx + 20, 1690, 400, 60}, "EXT S/C", fxp(tb, T::ExtSc)).tip = "External side-chain key = the file in EXT 2 (desk EXT RETURNS)";
    Widget& bands = b.sel({cx + 20, 1770, 400, 60}, "KEY BANDS", fxp(tb, T::ExtScBands));
    bands.tip = "Which D-EQ bands the EXT 2 key drives";
    bands.enabled = [&tb] { const int v = static_cast<int>(tb.get(T::ExtSc) + 0.5f); return v >= 2; };
    b.sw({cx + 20, 1858, 400, 70}, "RACK IN", unitIn(rig, ConsoleRig::UnitTwinBus), C::violet);
}

// ---- COLOUR STAGE -----------------------------------------------------------------------------------
void colourPage(B& b, ConsoleRig& rig, const AppState& app) {
    namespace K = colour_ids;
    fx::Effect& cs = rig.colour();
    auto P = [&](int p) { return fxp(cs, p); };
    auto live = [&cs] { return cs.get(K::Bypass) < 0.5f; };
    b.engrave("HEM COLOUR STAGE", 60, 250, 0, false, C::lime);
    b.engrave("STEREO DRIVE / TONE / HF COMPRESSOR / IMAGE / TRANSFORMER", 60, 300, 0, true, C::dim);
    b.sw({3240, 196, 300, 84}, "RACK IN", unitIn(rig, ConsoleRig::UnitColour), C::violet);
    const float Y = 360, H = 1620, cy = Y + 640;
    float x = 40;
    auto sect = [&](float w, const char* t) { b.section({x, Y, w, H}, t); const float x0 = x; x += w + 16; return x0; };
    // INPUT
    float s = sect(420, "INPUT");
    b.lamp({s + 70, Y + 110, 40, 40}, "O/L L", [&cs] { return cs.meter(K::MeterOverL); }, C::red);
    b.lamp({s + 290, Y + 110, 40, 40}, "O/L R", [&cs] { return cs.meter(K::MeterOverR); }, C::red);
    b.knob(sq(s + 210, cy - 40, 200), "INPUT TRIM", P(K::InputTrim), true).enabled = live;
    b.stepped(sq(s + 210, cy + 600, 160), "HPF", P(K::Hpf), C::cyan).enabled = live;
    // DRIVE
    s = sect(560, "DRIVE");
    auto driveOn = [&cs, live] { return live() && cs.get(K::DriveIn) > 0.5f; };
    b.knob(sq(s + 150, cy - 40, 200), "DRIVE", P(K::Drive), false, C::amber).enabled = driveOn;
    b.knob(sq(s + 410, cy - 40, 200), "DENSITY", P(K::Density), false, C::amber).enabled = driveOn;
    b.sw(box(s + 150, cy + 600, 180, 74), "IN", P(K::DriveIn), C::amber);
    Widget& led = b.lamp(sq(s + 410, cy + 600, 50), "THD", [&cs] { return colourDriveLedColour(cs.meter(K::MeterDriveThd)); });
    led.ledColours = {C::green, C::amber, C::red};
    led.tip = "Drive distortion: green / orange / red";
    // TONE EQ
    s = sect(860, "TONE EQ");
    auto eqOn = [&cs, live] { return live() && cs.get(K::EqIn) > 0.5f; };
    b.stepped(sq(s + 130, cy - 400, 150), "LOW FREQ", P(K::LowFreq), C::cyan).enabled = eqOn;
    b.knob(sq(s + 130, cy + 160, 200), "LOW", P(K::LowGain), true).enabled = eqOn;
    b.stepped(sq(s + 430, cy - 400, 150), "HIGH FREQ", P(K::HighFreq), C::cyan).enabled = eqOn;
    b.knob(sq(s + 430, cy + 160, 200), "HIGH", P(K::HighGain), true).enabled = eqOn;
    b.sw(box(s + 700, cy - 40, 180, 74), "IN", P(K::EqIn), C::lime);
    b.sw(box(s + 700, cy + 600, 180, 74), "PRE EQ", P(K::PreEq), C::violet).tip = "INSERT before the TONE EQ";
    // HF COMPRESSOR
    s = sect(560, "HF COMPRESSOR");
    auto hfcOn = [&cs, live] { return live() && cs.get(K::HfcIn) > 0.5f; };
    b.knob(sq(s + 150, cy - 40, 200), "THRESHOLD", P(K::HfcThreshold), false, C::amber).enabled = hfcOn;
    Widget& xo = b.knob(sq(s + 410, cy - 40, 200), "X-OVER", P(K::HfcXover), false, C::amber);
    xo.enabled = hfcOn;
    xo.tip = "X-OVER (CRUSH mode: WET / DRY)";
    b.sw(box(s + 150, cy + 600, 180, 74), "IN", P(K::HfcIn), C::amber);
    b.sw(box(s + 410, cy + 520, 180, 64), "CRUSH", P(K::HfcCrush), C::red).enabled = hfcOn;
    b.lamp(sq(s + 410, cy + 660, 40), "", [&cs] { return cs.meter(K::MeterHfcGr) < -0.5f ? 1.0f : 0.0f; }, C::amber).tip = "HF compressor working";
    // STEREO IMAGE
    s = sect(460, "STEREO IMAGE");
    auto imgOn = [&cs, live] { return live() && cs.get(K::ImageIn) > 0.5f; };
    b.knob(sq(s + 120, cy - 40, 180), "SPACE", P(K::Space), true, C::cyan).enabled = imgOn;
    b.knob(sq(s + 340, cy - 40, 180), "WIDTH", P(K::Width), true, C::cyan).enabled = imgOn;
    b.sw(box(s + 230, cy + 600, 180, 74), "IN", P(K::ImageIn), C::cyan);
    // OUTPUT
    s = sect(520, "OUTPUT");
    b.sw({s + 30, Y + 90, 220, 66}, "TRANSFORMER", P(K::Transformer), C::amber).enabled = live;
    b.sw({s + 270, Y + 90, 220, 66}, "INSERT", P(K::Insert), C::violet).enabled = live;
    b.sw({s + 270, Y + 176, 220, 60}, "M/S", P(K::InsertMs), C::violet).enabled = live;
    b.sel({s + 30, Y + 176, 150, 60}, "", toolType(rig.colourTool())).tip = "TOOL behind the INSERT switch";
    b.button({s + 186, Y + 176, 64, 60}, "ED", Act::EditTool, kColourToolTarget);
    b.knob(sq(s + 260, cy + 160, 200), "OUTPUT TRIM", P(K::OutputTrim), true);
    b.sw(box(s + 150, cy + 600, 200, 74), "BYPASS", P(K::Bypass), C::red);
    b.sw(box(s + 380, cy + 600, 200, 74), "POST I/P", P(K::BypassPostTrim), C::red).tip = "BYPASS keeps the INPUT TRIM";
    // METER
    s = sect(3800 - x, "METER");
    const float mw = (3800 - s - 60) / 2;
    for (int side = 0; side < 2; ++side) {
        Widget& m = b.bar({s + 20 + (mw + 20) * static_cast<float>(side), Y + 90, mw, H - 200}, -21, 24, true,
                          [&cs, side] { return cs.meter(side == 0 ? K::MeterL : K::MeterR); }, side == 0 ? "L" : "R");
        m.tip = "Output (dBu); in BYPASS it shows the input";
        m.value2 = [&cs, side] { return cs.meter(side == 0 ? K::MeterHoldL : K::MeterHoldR); };
    }
    (void)app;
}

// ---- METER BRIDGE ----------------------------------------------------------------------------------
void meterPage(B& b, ConsoleRig& rig) {
    (void)rig;
    b.engrave("HEM METER BRIDGE", 60, 250, 0, false, C::lime);
    b.engrave("LOUDNESS (ITU-R BS.1770-4 / EBU R128)  /  TRUE PEAK  /  SPECTRUM  /  STEREO", 60, 300, 0, true, C::dim);
    b.button({3440, 196, 330, 84}, "RESET", Act::ResetMeter, 0, C::red).tip = "Start integrated loudness, range, maxima and history again";
    b.section({40, 340, 3760, 1650}, "");
    b.custom({70, 380, 1240, 1240}, CustomId::Radar).tip = "Loudness radar: short-term loudness, one turn = 4 minutes (ASSUMED speed)";
    b.custom({1340, 380, 860, 1580}, CustomId::LoudNumbers);
    b.custom({2230, 380, 1540, 800}, CustomId::Rta).tip = "1/3-octave spectrum (L+R), peak hold";
    b.custom({2230, 1210, 740, 750}, CustomId::Scope).tip = "Vectorscope (mid up, side across)";
    b.custom({3000, 1210, 770, 750}, CustomId::StereoBars);
    b.engrave("ONE TURN = 4 MIN", 690, 1700, 1, true);
}

// ---- TOOLS -----------------------------------------------------------------------------------------
void toolsPage(B& b, ConsoleRig& rig, const AppState& app) {
    auto type = [&rig, &app] { return targetSlot(rig, app).type(); };
    b.engrave("HEM FRONT-END TOOLS", 60, 250, 0, false, C::lime);
    b.engrave("RESO TAMER  /  SHUFFLE  /  TAP ECHO  (in the slots where the desk has its mic preamps)", 60, 300, 0, true, C::dim);
    b.button({1400, 196, 110, 84}, "<", Act::ToolTarget, -1).tip = "Previous slot";
    b.readout({1520, 196, 900, 84}, [&app] {
        if (app.toolTarget >= kColourToolTarget) return std::string("COLOUR STAGE INSERT");
        const auto& st = app.stems[static_cast<std::size_t>(std::clamp(app.toolTarget, 0, kStemSlots - 1))];
        return "CHANNEL " + std::to_string(app.toolTarget + 1) + (st.name.empty() ? std::string() : "  " + st.name);
    }, C::text);
    b.button({2430, 196, 110, 84}, ">", Act::ToolTarget, 1).tip = "Next slot";
    Widget& ty = b.sel({2600, 196, 420, 84}, "TOOL", Ctl{&kToolTypeInfo, [type] { return static_cast<float>(type()); },
                                                         [&rig, &app](float v) { targetSlot(rig, app).setType(static_cast<int>(std::lround(v))); }});
    ty.tip = "Which tool this slot runs";
    static const ParamInfo kInsInfo{"insert", "Insert", "", 0, 1, 0, Taper::Toggle, kOnOff};
    b.sw({3060, 196, 300, 84}, "INSERT", Ctl{&kInsInfo,
        [&rig, &app] { return app.toolTarget >= kColourToolTarget ? rig.colour().get(colour_ids::Insert) : rig.desk().channel(app.toolTarget, StudioDesk::Insert); },
        [&rig, &app](float v) { if (app.toolTarget >= kColourToolTarget) rig.colour().set(colour_ids::Insert, v); else rig.desk().setChannel(app.toolTarget, StudioDesk::Insert, v); }},
         C::violet).tip = "The INSERT switch of this slot (the tool only runs while it is lit)";

    auto only = [type](int t) { return std::function<bool()>([type, t] { return type() == t; }); };
    auto offMsg = only(ToolSlot::Off);
    b.section({40, 340, 3760, 1650}, "");
    Widget& none = b.readout({1320, 1000, 1200, 120}, [] { return std::string("NO TOOL IN THIS SLOT: PICK ONE WITH  TOOL"); }, C::dim);
    none.visible = offMsg;

    // RESO TAMER
    {
        namespace R = reso_ids;
        const int T = ToolSlot::Reso;
        auto vis = only(T);
        for (int band = 0; band < 2; ++band) {
            const float X = 80 + 1860.0f * static_cast<float>(band);
            auto P = [&](int p) { return toolP(rig, app, T, R::id(band, p)); };
            auto on = [&rig, &app, band] { return targetSlot(rig, app).tool(ToolSlot::Reso).get(R::id(band, R::On)) > 0.5f; };
            Widget& onSw = b.sw({X, 400, 200, 80}, band == 0 ? "BAND 1" : "BAND 2", P(R::On), C::lime);
            onSw.visible = vis;
            const char* names[] = {"FREQ", "WIDTH (Q)", "THRESHOLD", "DEPTH"};
            const int ids[] = {R::Freq, R::Width, R::Threshold, R::Depth};
            for (int k = 0; k < 4; ++k) {
                Widget& w = b.knob(sq(X + 230 + 420.0f * static_cast<float>(k), 820, k == 0 ? 280.0f : 240.0f), names[k], P(ids[k]), false, k == 0 ? C::cyan : C::lime);
                w.visible = vis; w.enabled = on;
            }
            Widget& sp = b.sel({X + 120, 1180, 380, 80}, "SPEED", P(R::Speed));
            sp.visible = vis; sp.enabled = on;
            Widget& li = b.sw({X + 640, 1180, 260, 80}, "LISTEN", P(R::Listen), C::amber);
            li.visible = vis; li.enabled = on;
            Widget& gr = b.bar({X + 120, 1400, 1500, 70}, -18, 0, false,
                               [&rig, &app, band] { return targetSlot(rig, app).tool(ToolSlot::Reso).meter(band == 0 ? R::MeterGr1 : R::MeterGr2); }, "CUT");
            gr.visible = vis;
            gr.tip = "How much this band is cutting right now (dB)";
        }
    }
    // SHUFFLE
    {
        namespace S = shuffle_ids;
        const int T = ToolSlot::Shuffle;
        auto vis = only(T);
        const char* names[] = {"SHUFFLE FREQ", "SHUFFLE", "MUD FREQ", "MUD CUT", "WIDTH", "OUTPUT"};
        const int ids[] = {S::ShuffleFreq, S::Shuffle, S::MudFreq, S::Mud, S::Width, S::Output};
        const Rgba cols[] = {C::cyan, C::lime, C::cyan, C::amber, C::lime, C::lime};
        for (int k = 0; k < 6; ++k) {
            Widget& w = b.knob(sq(330 + 610.0f * static_cast<float>(k), 820, 300), names[k], toolP(rig, app, T, ids[k]), ids[k] == S::Output, cols[k]);
            w.visible = vis;
        }
        Widget& mb = b.sw({250, 1200, 300, 90}, "MONO BASS", toolP(rig, app, T, S::MonoBass), C::amber);
        mb.visible = vis;
        Widget& corr = b.custom({900, 1180, 2600, 160}, CustomId::ToolMeters, T);
        corr.visible = vis;
    }
    // TAP ECHO
    {
        namespace E = echo_ids;
        const int T = ToolSlot::Echo;
        auto vis = only(T);
        Widget& tap = b.button({140, 560, 440, 440}, "TAP", Act::Tap, 0, C::amber);
        tap.visible = vis;
        tap.tip = "Tap the beat: sets TEMPO (tap 2 or more times)";
        const char* names[] = {"TEMPO", "TIME", "FEEDBACK", "MIX", "LOW CUT", "HIGH CUT"};
        const int ids[] = {E::Tempo, E::Time, E::Feedback, E::Mix, E::LowCut, E::HighCut};
        for (int k = 0; k < 6; ++k) {
            Widget& w = b.knob(sq(900 + 480.0f * static_cast<float>(k), 760, 250), names[k], toolP(rig, app, T, ids[k]), false, k < 2 ? C::cyan : C::lime);
            w.visible = vis;
            if (ids[k] == E::Time || ids[k] == E::Tempo)
                w.enabled = [&rig, &app, id = ids[k]] { return targetSlot(rig, app).tool(ToolSlot::Echo).paramActive(id); };
        }
        Widget& note = b.sel({700, 1150, 520, 90}, "NOTE", toolP(rig, app, T, E::Note));
        note.visible = vis;
        Widget& mode = b.sel({1300, 1150, 520, 90}, "MODE", toolP(rig, app, T, E::Mode));
        mode.visible = vis;
        Widget& d = b.readout({1900, 1150, 600, 90}, [&rig, &app] { return fmt("DELAY %.1f ms", targetSlot(rig, app).tool(ToolSlot::Echo).meter(E::MeterDelayMs)); });
        d.visible = vis;
    }
}

// ---- BIG DESK: five channel strips at a time, every control bigger; arrows / the numbers move across all 13 ----------
void bigDeskPage(B& b, ConsoleRig& rig, AppState& app) {
    using D = StudioDesk;
    constexpr int kN = kBigChannels;
    // navigation row
    Widget& prev = b.button({30, 168, 220, 80}, "<  BACK", Act::BigNav, -kN, C::violet);
    prev.tip = "Show the channels to the left (or the Left arrow key)";
    for (int k = 0; k < ConsoleRig::kChannels; ++k) {
        Widget& w = b.button({270 + 104.0f * static_cast<float>(k), 168, 94, 80}, std::to_string(k + 1), Act::BigNav, 100 + k, C::lime);
        w.value = [&app, k] { return k >= app.bigFirst && k < app.bigFirst + kN ? 1.0f : 0.0f; };
        w.tip = "Show channel " + std::to_string(k + 1);
    }
    Widget& nxt = b.button({270 + 104.0f * ConsoleRig::kChannels + 10, 168, 220, 80}, "NEXT  >", Act::BigNav, kN, C::violet);
    nxt.tip = "Show the channels to the right (or the Right arrow key)";
    b.engrave("5 CHANNELS AT A TIME  -  ALL 13: THE DESK TAB", 3480, 222, 2, true, C::dim);

    constexpr float kTop = 262, kPitch = 692, kW = 676;
    for (int s = 0; s < kN; ++s) {
        const float x = 30 + kPitch * static_cast<float>(s);
        auto P = [&](int p) { return deskBig(rig, app, s, p); };
        auto chOf = [&app, s] { return app.bigFirst + s; };
        b.section({x, kTop, kW, 2000 - kTop}, "");
        // header: channel number, TOOL slot, edit
        b.readout({x + 12, kTop + 12, 150, 84}, [&app, s] { return "CH " + std::to_string(app.bigFirst + s + 1); }, C::violet);
        Widget& tool = b.sel({x + 174, kTop + 12, 300, 84}, "TOOL", {&kToolTypeInfo,
            [&rig, chOf] { return static_cast<float>(rig.channelTool(chOf()).type()); },
            [&rig, chOf](float v) { rig.channelTool(chOf()).setType(static_cast<int>(std::lround(v))); }});
        tool.tip = "TOOL behind this channel's INSERT switch";
        Widget& ed = b.button({x + 486, kTop + 12, 178, 84}, "EDIT TOOL", Act::EditTool, 0);
        ed.argFn = chOf;
        ed.tip = "Open this channel's tool on the TOOLS page";

        const float c0 = x + 95, c1 = x + 250, c2 = x + 400;   // knob columns
        constexpr float kK = 120;                                 // knob size
        auto swAt = [&](float cx, float y, const char* t, int p, Rgba col) -> Widget& { return b.sw({cx - 68, y, 136, 56}, t, P(p), col); };
        // input
        b.knob(sq(c0, 470, kK), "TRIM", P(D::Trim));
        swAt(c1, 412, "MONO", D::Mono, C::amber);
        swAt(c2, 412, "\xC3\x98", D::Polarity, C::amber);
        swAt(c1, 482, "HPF", D::Hpf, C::amber);
        // cue sends
        b.knob(sq(c0, 660, kK), "CUE 1", P(D::Cue1Level), false, C::cyan);
        b.knob(sq(c1, 660, kK), "PAN", P(D::Cue1Pan), true, C::cyan);
        swAt(c2, 602, "ON", D::Cue1On, C::cyan);
        swAt(c2, 668, "POST", D::Cue1Post, C::cyan);
        b.knob(sq(c0, 850, kK), "CUE 2", P(D::Cue2Level), false, C::cyan);
        b.knob(sq(c1, 850, kK), "PAN", P(D::Cue2Pan), true, C::cyan);
        swAt(c2, 792, "ON", D::Cue2On, C::cyan);
        swAt(c2, 858, "POST", D::Cue2Post, C::cyan);
        // EQ
        auto eqOn = [&rig, chOf] { return rig.desk().channel(chOf(), D::EqIn) > 0.5f; };
        b.knob(sq(c0, 1050, kK), "HF", P(D::HfGain), true).enabled = eqOn;
        b.knob(sq(c1, 1050, kK), "MF", P(D::MfGain), true).enabled = eqOn;
        b.knob(sq(c2, 1050, kK), "LF", P(D::LfGain), true).enabled = eqOn;
        swAt(c0, 1132, "BELL", D::HfBell, C::lime);
        swAt(c1, 1132, "EQ IN", D::EqIn, C::lime);
        swAt(c2, 1132, "BELL", D::LfBell, C::lime);
        // compressor
        b.knob(sq(c0, 1320, kK), "COMP", P(D::CompThreshold), false, C::amber).enabled = [&rig, chOf] { return rig.desk().channel(chOf(), D::CompIn) > 0.5f; };
        swAt(c1, 1292, "IN", D::CompIn, C::amber);
        b.leds({c2 - 66, 1304, 132, 32}, 3, false, [&rig, chOf] { return static_cast<float>(D::channelCompLeds(rig.desk().channelCompGrDb(chOf()))); },
               {C::amber, C::amber, C::red}).tip = "Compressor gain reduction: 3, 6, 10 dB";
        // output
        b.knob(sq(c0, 1530, kK), "PAN", P(D::Pan), true);
        swAt(c1, 1472, "INS", D::Insert, C::violet).tip = "INSERT: run this channel's TOOL";
        swAt(c1, 1542, "D.POST", D::DirectPostFader, C::violet).tip = "Direct out (PRINT STEMS) after the fader";
        swAt(c2, 1472, "BUS B", D::MuteBusB, C::red).tip = "MUTE / BUS B: off the main mix, onto Bus B";
        swAt(c2, 1542, "PFL", D::Pfl, C::amber);
        // fader + meter
        const float fx = x + 545;
        constexpr float kFy = 400, kFh = 1420;
        b.fader({fx - 16, kFy, 32, kFh}, "", P(D::Fader));
        for (float db : {10.0f, 0.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f}) {
            const float y = kFy + kFh * (1.0f - faderTravel(db));
            b.engrave(db > 0 ? "+10" : std::to_string(static_cast<int>(db)), x + 482, y + 10, 1, true);
        }
        for (int side = 0; side < 2; ++side)
            b.leds({x + 600 + 32.0f * static_cast<float>(side), kFy + 4, 26, kFh - 8}, 8, true,
                   [&rig, chOf, side] { return static_cast<float>(D::channelMeterLeds(rig.desk().channelPeakDbu(chOf(), side))); }, kChannelLedCols)
                .tip = "Channel meter (dBu): -21, -15, -6, 0, +6, +12, +18, +24";
        Widget& name = b.custom({x + 12, 1868, kW - 24, 116}, CustomId::StemName, 0);
        name.argFn = chOf;
        name.tip = "Click: load a file into this channel. Right-click: empty it";
    }

    // master column: what you need while working on channels
    const float mx = 30 + kPitch * kN;
    b.section({mx, kTop, kWidth - 30 - mx, 2000 - kTop}, "MAIN");
    b.knob(sq(mx + 160, 440, 150), "MONITOR", deskM(rig, D::MonitorLevel), false, C::lime);
    b.sw({mx + 20, 560, 130, 60}, "DIM", deskM(rig, D::Dim), C::amber);
    b.sw({mx + 168, 560, 130, 60}, "CUT", deskM(rig, D::Cut), C::red);
    for (int side = 0; side < 2; ++side)
        b.leds({mx + 40 + 36.0f * static_cast<float>(side), 700, 28, 1120}, 12, true,
               [&rig, side] { return static_cast<float>(D::mainMeterLeds(rig.desk().mainMeterDbu(side))); }, kMainLedCols)
            .tip = "Main meter (dBu, follows the monitor source)";
    b.fader({mx + 214, 700, 32, 1120}, "MAIN", deskM(rig, D::MainFader));
    for (float db : {10.0f, 0.0f, -10.0f, -20.0f, -40.0f}) {
        const float y = 700 + 1120 * (1.0f - faderTravel(db));
        b.engrave(db > 0 ? "+10" : std::to_string(static_cast<int>(db)), mx + 150, y + 10, 1, true);
    }
    b.engrave("MAIN FADER", mx + 157, 1900, 1, true, C::text);
}

} // namespace

const char* pageName(PageId p) noexcept {
    static const char* const n[] = {"DESK", "STEP EQ", "TWIN BUS", "COLOUR", "METER", "TOOLS", "BIG DESK"};
    return n[std::clamp(static_cast<int>(p), 0, kPageCount - 1)];
}

std::vector<Page> buildPages(ConsoleRig& rig, AppState& app) {
    std::vector<Page> pages(static_cast<std::size_t>(kPageCount));
    for (int i = 0; i < kPageCount; ++i) {
        Page& p = pages[static_cast<std::size_t>(i)];
        p.id = static_cast<PageId>(i);
        B b{p};
        switch (p.id) {
        case PageId::Desk: deskPage(b, rig, app); break;
        case PageId::StepEq: stepEqPage(b, rig); break;
        case PageId::TwinBus: twinBusPage(b, rig); break;
        case PageId::Colour: colourPage(b, rig, app); break;
        case PageId::Meter: meterPage(b, rig); break;
        case PageId::Tools: toolsPage(b, rig, app); break;
        case PageId::DeskBig: bigDeskPage(b, rig, app); break;
        default: break;
        }
        topBar(b, rig, app, p.id);
        bottomBar(b, app);
    }
    return pages;
}

// ---- fader scale ---------------------------------------------------------------------------------
namespace {
constexpr float kFdDb[] = {-80.0f, -60.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f, 10.0f};
constexpr float kFdPos[] = {0.0f, 0.08f, 0.20f, 0.30f, 0.42f, 0.58f, 0.78f, 1.0f};
} // namespace
float faderTravel(float db) noexcept {
    db = std::clamp(db, kFdDb[0], kFdDb[7]);
    for (int i = 0; i < 7; ++i)
        if (db <= kFdDb[i + 1]) return kFdPos[i] + (kFdPos[i + 1] - kFdPos[i]) * (db - kFdDb[i]) / (kFdDb[i + 1] - kFdDb[i]);
    return 1.0f;
}
float faderDb(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    for (int i = 0; i < 7; ++i)
        if (t <= kFdPos[i + 1]) return kFdDb[i] + (kFdDb[i + 1] - kFdDb[i]) * (t - kFdPos[i]) / (kFdPos[i + 1] - kFdPos[i]);
    return kFdDb[7];
}

float widgetNorm(const Widget& w) {
    if (!w.ctl.valid()) return w.value ? w.value() : 0.0f;
    if (w.kind == Kind::Fader) return faderTravel(w.ctl.get());
    return fx::toNormalized(*w.ctl.info, w.ctl.get());
}
void widgetSetNorm(const Widget& w, float n) {
    if (!w.ctl.valid()) return;
    n = std::clamp(n, 0.0f, 1.0f);
    if (w.kind == Kind::Fader) w.ctl.set(faderDb(n));
    else w.ctl.set(fx::clampPlain(*w.ctl.info, fx::toPlain(*w.ctl.info, n)));
}
std::string widgetValueText(const Widget& w) {
    if (!w.ctl.valid()) return w.text ? w.text() : std::string();
    const float v = w.ctl.get();
    if (w.kind == Kind::Fader && v <= w.ctl.info->min + 0.01f) return "OFF";
    if ((w.kind == Kind::Knob) && w.ctl.info->unit && std::string(w.ctl.info->unit) == "dB" && v <= -79.9f) return "OFF";
    return fx::formatValue(*w.ctl.info, v);
}
bool widgetVisible(const Widget& w) { return !w.visible || w.visible(); }
bool widgetEnabled(const Widget& w) { return !w.enabled || w.enabled(); }
int widgetArg(const Widget& w) { return w.argFn ? w.argFn() : w.kind == Kind::Custom ? w.customArg : w.arg; }
int bigDeskFirst(int first, int delta, int jumpTo) noexcept {
    constexpr int kLast = ConsoleRig::kChannels - kBigChannels;
    if (jumpTo >= 0) {
        jumpTo = std::clamp(jumpTo, 0, ConsoleRig::kChannels - 1);
        if (jumpTo >= first && jumpTo < first + kBigChannels) return std::clamp(first, 0, kLast);   // already on screen
        return std::clamp(jumpTo - kBigChannels / 2, 0, kLast);
    }
    return std::clamp(first + delta, 0, kLast);
}

// ---- the plate --------------------------------------------------------------------------------------
hc::ui::PanelLayout platePockets(const Page& page) {
    hc::ui::PanelLayout L;
    L.width = kWidth; L.height = kHeight; L.zoom = 1.0f;
    auto pocket = [&](Rect r, float radius, float depth, bool circle, bool rings) {
        hc::ui::PanelPocket p;
        p.r = r; p.radius = radius; p.depth = depth; p.circle = circle; p.rings = rings;
        L.pockets.push_back(p);
    };
    for (const Section& s : page.sections) pocket(s.r, 18, 3, false, false);
    // the bars
    pocket({18, 24, kWidth - 36, 116}, 20, 3, false, false);
    pocket({18, 2016, kWidth - 36, 128}, 20, 3, false, false);
    for (const Widget& w : page.widgets) {
        if (w.visible) continue;   // controls that come and go (TOOLS page) are drawn with their wells live
        switch (w.kind) {
        case Kind::Knob:
        case Kind::Stepped: {
            const float d = w.r.w * 1.16f;
            pocket({w.r.cx() - d / 2, w.r.cy() - d / 2, d, d}, d / 2, 10, true, true);
            break;
        }
        case Kind::Switch:
        case Kind::Button: pocket(w.r.inset(-5), 12, 6, false, false); break;
        case Kind::Selector:
        case Kind::Readout:
        case Kind::Custom: pocket(w.r.inset(-6), 14, 7, false, false); break;
        case Kind::Fader: pocket(w.r.inset(-4, -30), w.r.w * 0.5f + 4, 16, false, false); break;
        case Kind::Leds:
        case Kind::LedBar:
        case Kind::Seek: pocket(w.r.inset(-6), 8, 6, false, false); break;
        case Kind::Lamp: {
            const float d = std::min(w.r.w, w.r.h) * 1.5f;
            pocket({w.r.cx() - d / 2, w.r.cy() - d / 2, d, d}, d / 2, 5, true, false);
            break;
        }
        }
    }
    for (const auto& n : page.neon) L.grooves.push_back({n[0], n[1], n[2], n[3], false});
    return L;
}

} // namespace hc::console::ui
