#pragma once
// =============================================================================
//  HEM DIGITAL CONSOLE — screen model (hc::console::ui). Pure code, no GPU calls:
//    * Widget / Page   : every control on every faceplate, where it sits and what it reads and writes
//    * buildPages()    : the six faceplates (DESK, STEP EQ, TWIN BUS, COLOUR STAGE, METER BRIDGE, TOOLS)
//                        with the top bar and the transport bar on each
//    * platePockets()  : the CNC pockets each faceplate is routed with (renderFxPlate draws the obsidian)
//    * Controller      : mouse / wheel → control edits and app actions
//  Design units: a 3840 × 2160 canvas, letterboxed into the window.
//
//  Threads: UI thread only. Controls are read and written through the rig's lock-free atomics
//  (the same values the audio thread reads); the UI never touches DSP state.
// =============================================================================
#include "hybrid/console/ConsoleRig.hpp"
#include "hybrid/fx/Fx.hpp"
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/Geometry.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hc::console::ui {

using hc::ui::Rect;
using hc::ui::Rgba;
using hc::ui::rgb8;

inline constexpr float kWidth = 3840.0f, kHeight = 2160.0f;
inline constexpr int kStemSlots = ConsoleRig::kChannels + 2;   // 13 channels + EXT 1 + EXT 2 returns
inline constexpr int kExt1Slot = ConsoleRig::kChannels, kExt2Slot = ConsoleRig::kChannels + 1;
inline constexpr int kColourToolTarget = ConsoleRig::kChannels;   // TOOLS page: 0..12 = channel, 13 = colour stage insert
inline constexpr int kBigChannels = 5;                             // BIG DESK page: channels on screen at a time
// View zoom (1 = the whole faceplate fits the window). The largest zoom is also limited so the plate is never shown
// above 1 window pixel per design unit (its routed texture stops there).
inline constexpr float kZoomMin = 1.0f, kZoomMax = 3.0f;
inline constexpr float kMinTextPx = 13.0f;   // no text is drawn smaller than this many screen pixels

namespace colours {
inline const Rgba violet = rgb8(0x9B5CFF);   // HEM: frame neon, section captions, lit switches
inline const Rgba lime   = rgb8(0xC0FF3E);   // HEM lime: values, knob arcs
inline const Rgba red    = rgb8(0xFF3B3B);
inline const Rgba amber  = rgb8(0xFFB020);
inline const Rgba green  = rgb8(0x3CFF6A);
inline const Rgba cyan   = rgb8(0x38E8FF);
inline const Rgba text   = rgb8(0xE8E6F2);
inline const Rgba dim    = rgb8(0x8C88A0);
} // namespace colours

// What the app tells the screen (UI thread, refreshed every frame).
struct StemSlotState {
    std::string name;          // file name ("" = empty)
    std::string info;          // "WAV 24b 3:42"
    float progress = -1.0f;    // 0..1 while loading, -1 idle
    std::string error;
};
struct AppState {
    std::array<StemSlotState, kStemSlots> stems{};
    bool playing = false, loop = false;
    double position = 0.0, length = 0.0;       // seconds
    std::string device = "NO DEVICE";          // output device line
    std::string status;                        // bottom-bar message
    std::string session = "UNTITLED";
    bool dirty = false;
    float dspLoad = 0.0f;                      // fraction
    bool alert = false;                        // device problem
    float exportProgress = -1.0f;              // 0..1 while exporting
    int listen = 0;                            // 0 MONITOR, 1 CUE 1, 2 CUE 2 (what the output device plays)
    int toolTarget = 0;                        // TOOLS page: which slot is shown
    int resetStage = 0;                        // RESET button: 0 idle, 1 asking "SURE?", 2 offering UNDO
    float zoom = 1.0f;                         // view zoom (kZoomMin..kZoomMax), saved in the settings
    int bigFirst = 0;                          // BIG DESK: first channel shown (0 .. kChannels - kBigChannels)
    int meterTab = 0;                          // METER page EDIT panel: 0 RADAR, 1 RTA, 2 SCOPE, 3 GENERAL, 4 LIBRARY
    int meterLibSlot = 0;                      // LIBRARY: preset picked (factory presets first, then the user slots)
    std::string meterPreset = "DEFAULT";       // the preset last recalled (shown on the radar; "EDITED:" once changed)
    bool meterEdited = false;
};
inline constexpr int kMeterUserPresets = 8;

// Everything the screen asks the app to do.
enum class Act : std::uint8_t {
    None, Page, LoadStems, LoadSlot, ClearSlot, Play, Stop, Rewind, Loop, Seek, Export, PrintStems,
    NewSession, OpenSession, SaveSession, Device, Listen, ToolTarget, EditTool, Tap, ResetMeter, ResetAll,
    BigNav,     // BIG DESK: arg = channels to move (+/-), or 100 + channel to show that channel
    MeterTab,   // METER: EDIT panel page (arg)
    MeterAuto,  // METER: RTA AUTO scale
    MeterRecall,// METER: LIBRARY recall (AppState::meterLibSlot)
    MeterStore, // METER: LIBRARY store into a user slot (AppState::meterLibSlot)
};
struct ActionEvent {
    Act act = Act::None;
    int arg = 0;
    float value = 0.0f;
};

// (DeskBig is last so saved page numbers of older versions still mean the same page.)
enum class PageId : std::uint8_t { Desk, StepEq, TwinBus, Colour, Meter, Tools, DeskBig, kCount };
inline constexpr int kPageCount = static_cast<int>(PageId::kCount);
[[nodiscard]] const char* pageName(PageId p) noexcept;
// The pages in the order of the tabs (and of the keys 1..7).
inline constexpr std::array<PageId, kPageCount> kTabOrder{PageId::Desk, PageId::DeskBig, PageId::StepEq, PageId::TwinBus,
                                                          PageId::Colour, PageId::Meter, PageId::Tools};
// First channel the BIG DESK shows after moving by `delta` (or jumping to show `channel` when jump >= 0).
[[nodiscard]] int bigDeskFirst(int first, int delta, int jumpTo = -1) noexcept;

// One control binding: the control's description plus its getter / setter (plain values).
struct Ctl {
    const fx::ParamInfo* info = nullptr;
    std::function<float()> get;
    std::function<void(float)> set;
    [[nodiscard]] bool valid() const noexcept { return info && get && set; }
};

enum class Kind : std::uint8_t {
    Knob,       // continuous knob, value arc
    Stepped,    // detented knob (Choice params): dots for every step
    Switch,     // latching push switch with a lit lens
    Fader,      // long-throw fader (console scale)
    Selector,   // glass window showing a choice; click = next, right-click = previous, wheel steps
    Button,     // momentary push: fires an action
    Leds,       // a row / column of LEDs, value() = how many are lit
    LedBar,     // level meter in dB, value() = dB, min/max = scale
    Lamp,       // one LED, value() > 0.5 = lit
    Readout,    // glass window with neon text from text()
    Seek,       // song position bar (click / drag = seek)
    Custom,     // drawn by the view (radar, spectrum, scope, number grid ...)
};

enum class CustomId : std::uint8_t { None, StemName, Radar, Rta, Scope, LoudNumbers, StereoBars, GrNeedle, DeskLoud, ToolMeters, PeakMeter, MeterStatus };

struct Widget {
    Kind kind = Kind::Knob;
    Rect r;                         // the control itself (knob = its circle's square)
    std::string label;              // engraved caption ("HF", "BELL"); switches print it on the cap
    Ctl ctl;                        // Knob / Stepped / Switch / Fader / Selector
    Rgba colour = colours::lime;    // arc / lens / LED colour
    Act act = Act::None;            // Button / Seek
    int arg = 0;
    std::function<int()> argFn;             // when set: the arg (Button) / customArg (Custom) right now (BIG DESK strips)
    std::function<float()> value;           // Leds / LedBar / Lamp / Seek (0..1)
    std::function<float()> value2;          // LedBar: peak-hold marker (optional)
    std::function<std::string()> text;      // Readout / Button caption (when set) / Selector text override
    std::function<bool()> enabled;          // false: the control does nothing right now (dimmed)
    std::function<bool()> visible;          // false: not drawn, not clickable (TOOLS page swaps panels)
    int count = 0;                  // Leds: how many
    bool vertical = false;          // Leds / LedBar orientation
    float min = 0, max = 1;         // LedBar scale
    std::vector<Rgba> ledColours;   // Leds: colour of each LED (bottom / left first)
    bool bipolar = false;           // Knob: arc grows from the middle
    bool small = false;             // smaller caption
    bool captionBelow = false;
    CustomId custom = CustomId::None;
    int customArg = 0;
    std::string tip;                // long name shown while hovered (defaults to the control's name)
};

struct Section {                    // a routed bay with an engraved title
    Rect r;
    std::string title;
    Rgba colour = colours::violet;
};
struct Engraving {                  // free text on the plate (scales, unit names)
    std::string text;
    float x = 0, y = 0;             // baseline
    int align = 1;                  // 0 left, 1 centre, 2 right
    bool small = true;
    Rgba colour = colours::dim;
};

struct Page {
    PageId id = PageId::Desk;
    std::vector<Widget> widgets;
    std::vector<Section> sections;
    std::vector<Engraving> engravings;
    std::vector<std::array<float, 4>> neon;   // neon tubes: x0, y0, x1, y1 (violet)
};

// The six pages for this rig. `app` must outlive the pages (the widgets read it).
[[nodiscard]] std::vector<Page> buildPages(ConsoleRig& rig, AppState& app);

// The plate a page is routed from (pockets for every control, bays, neon channels, screws).
[[nodiscard]] hc::ui::PanelLayout platePockets(const Page& page);

// Console fader scale: plain dB (-80..+10) <-> travel 0..1 (0 dB at 78 %).
[[nodiscard]] float faderTravel(float db) noexcept;
[[nodiscard]] float faderDb(float travel) noexcept;

// Normalized position of a control's current value and back (Choice / Toggle snap; faders use the console scale).
// METER LIBRARY: the name of preset slot k (factory presets, then USER 1..8).
[[nodiscard]] int meterLibCount() noexcept;
[[nodiscard]] const char* meterLibName(int k) noexcept;
[[nodiscard]] bool meterLibIsUser(int k) noexcept;

[[nodiscard]] float widgetNorm(const Widget& w);
void widgetSetNorm(const Widget& w, float n);
[[nodiscard]] std::string widgetValueText(const Widget& w);   // "+4.5 dB", "1.5 kHz", "ON"
[[nodiscard]] bool widgetVisible(const Widget& w);
[[nodiscard]] bool widgetEnabled(const Widget& w);
[[nodiscard]] int widgetArg(const Widget& w);   // argFn() if set, else arg (Button) / customArg (Custom)

// ---- interaction -------------------------------------------------------------------------------
class Controller {
public:
    // Coordinates in design units. `emit` receives every app action (Page changes included).
    explicit Controller(std::function<void(const ActionEvent&)> emit) : emit_(std::move(emit)) {}
    void setPage(const Page* page) noexcept { page_ = page; hover_ = active_ = -1; }
    void mouseDown(float x, float y, bool doubleClick, bool fine);
    void rightDown(float x, float y);
    void mouseMove(float x, float y, bool fine);
    void mouseUp(float x, float y);
    bool wheel(float x, float y, float notches, bool fine);   // false: no control under the mouse
    [[nodiscard]] int hover() const noexcept { return hover_; }    // widget index or -1
    [[nodiscard]] int active() const noexcept { return active_; }  // pressed / dragged widget or -1
    [[nodiscard]] int hit(float x, float y) const noexcept;
    static constexpr float kDragUnitsForFullRange = 520.0f;

private:
    void step(const Widget& w, int delta);
    const Page* page_ = nullptr;
    std::function<void(const ActionEvent&)> emit_;
    int hover_ = -1, active_ = -1;
    float lastY_ = 0, dragNorm_ = 0, grabOffset_ = 0;
};

} // namespace hc::console::ui
