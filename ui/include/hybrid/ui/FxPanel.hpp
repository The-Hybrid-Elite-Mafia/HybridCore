#pragma once
// =============================================================================
//  FxPanel — the faceplate of every HEM plug-in (the 22 effects and HEM WAVE).
//
//  Look: a slab of polished obsidian, rendered on the CPU once per plug-in and lit from the
//  top-left like the console (kLight). Every control sits in its own CNC-routed pocket: round
//  wells for the knobs, rectangular pockets for value windows, buttons and meters, each with a
//  bright 45° chamfer and a milled floor (concentric rings in the wells, parallel tool passes in
//  the rectangles). Thin routed channels carry the neon. Chrome hex screws hold the slab.
//  Two neon colours (FxPalette): electric violet (frame, sections) and HEM lime (values, arcs).
//
//  Three layers, so the same faceplate serves the VST3 editors and The Hybrid Core's own FX rack:
//    layoutFxPanel()   : where everything goes (pure, unit-tested)
//    renderFxPlate()   : the static obsidian slab with its pockets (CPU, deterministic)
//    FxPanelController : mouse / wheel / keys → parameter edits through a PanelHost (pure)
//    FxPanelView       : draws the plate + live knobs, buttons, values, meters (Renderer)
//  Coordinates are panel design units (the panel is ~1100–2000 units wide; text uses the
//  console's TextStyles, which are sized for the 3840-wide console, so they read the same).
// =============================================================================
#include "hybrid/ui/Geometry.hpp"
#include "hybrid/ui/Image.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hc::ui {

// One control as the panel sees it (filled from hc::fx::ParamInfo or hc::synth::ParamInfo).
struct PanelParam {
    enum class Kind : std::uint8_t { Knob, Toggle, Choice };
    std::string              name;            // "Attack"
    Kind                     kind = Kind::Knob;
    std::vector<std::string> choices;          // Choice: every option's name
    float                    defNormalized = 0.0f;
    bool                     bipolar = false;  // knob arc starts in the middle (e.g. -100..+100 %)
};
struct PanelMeter {
    std::string name;      // "PITCH"
    std::string unit;      // "Hz", "dB", "cents", "note", "corr", "%", "ms", "dBFS"
    float       min = 0, max = 1;
    bool        centred = false;
};
// Optional sections (HEM WAVE: OSC, FILTER ...). Empty = one section holding everything.
struct PanelGroup {
    std::string name;
    int         first = 0, count = 0;   // parameter index range
};
struct PanelSpec {
    std::string             name;    // "HEM PUNCH"
    std::string             blurb;   // one line
    std::vector<PanelParam> params;
    std::vector<PanelMeter> meters;
    std::vector<PanelGroup> groups;
    bool                    hasBypass = true;   // instruments have none
};

struct FxPalette {
    Rgba violet = rgb8(0x9B5CFF);   // frame neon, section captions, lit buttons
    Rgba lime   = rgb8(0xC0FF3E);   // HEM lime: title, values, knob arcs
};

// ---- layout -------------------------------------------------------------------------
struct PanelPocket {
    Rect  r;
    float radius = 0;      // corner radius (circle: r.w / 2)
    float depth  = 10;     // design units
    bool  circle = false;
    bool  rings  = false;  // milled finish: concentric rings (wells) or parallel passes
};
struct PanelGroove {       // routed neon channel
    float x0, y0, x1, y1;
    bool  lime = false;    // false = violet
};
struct PanelKnob {
    int  param = -1;
    Rect well;             // the round pocket (knob drawn inside)
    Rect caption, value;   // caption line, value window
};
struct PanelButton {
    int  param  = -1;
    int  choice = -1;      // Choice: which option this button selects; -1 = toggle
    Rect rect;
    std::string caption;
};
struct PanelSelector {     // choices with more than 4 options: a window with ◀ ▶
    int  param = -1;
    Rect caption, box, prev, next;
};
struct PanelMeterSlot {
    int  meter = -1;
    Rect bar, caption, value;
};
struct PanelCaption {      // the name above every control cell
    int  param = -1;
    Rect r;
};
struct PanelSection {
    Rect        bay;       // routed bay
    std::string caption;
    Rect        captionArea;
};

struct PanelLayout {
    float width = 0, height = 0;
    float zoom = 1.0f;     // every size × zoom (text picks a matching larger style)
    Rect  header, titleArea, footer, bypass, logo;
    std::string name, blurb;
    bool hasBypass = true;
    std::vector<PanelPocket>    pockets;
    std::vector<PanelGroove>    grooves;
    std::vector<PanelSection>   sections;
    std::vector<PanelKnob>      knobs;
    std::vector<PanelButton>    buttons;
    std::vector<PanelSelector>  selectors;
    std::vector<PanelMeterSlot> meters;
    std::vector<PanelCaption>   captions;
    std::vector<std::array<float, 2>> screws;   // chrome screw centres
    Rect meterBay;
};

inline constexpr float kPanelCellW = 230.0f, kPanelCellH = 270.0f;
inline constexpr int   kPanelMaxCols = 6;

// zoom > 1 makes the whole plate bigger (the FX rack fills its space with it); < 1 smaller.
// maxCols > 0 overrides how many controls sit side by side (the FX rack picks the shape that fills its space).
[[nodiscard]] PanelLayout layoutFxPanel(const PanelSpec& spec, float zoom = 1.0f, int maxCols = 0);

// ---- the obsidian slab ------------------------------------------------------------------
// Straight-alpha sRGB, opaque, `pixelsPerUnit` × the layout size. Deterministic.
[[nodiscard]] Image renderFxPlate(const PanelLayout& layout, const FxPalette& palette, float pixelsPerUnit = 1.0f);

// ---- interaction ----------------------------------------------------------------------
class PanelHost {
public:
    virtual ~PanelHost() = default;
    [[nodiscard]] virtual float normalized(int param) const = 0;
    virtual void beginEdit(int param) = 0;
    virtual void setNormalized(int param, float value) = 0;
    virtual void endEdit(int param) = 0;
    [[nodiscard]] virtual std::string text(int param) const = 0;   // formatted current value
    [[nodiscard]] virtual float meter(int) const { return 0.0f; }
    [[nodiscard]] virtual bool  active(int) const { return true; }   // false: the control does nothing right now (dimmed)
    [[nodiscard]] virtual bool  hasBypass() const { return true; }
    [[nodiscard]] virtual bool  bypassed() const { return false; }
    virtual void setBypassed(bool) {}
};

struct PanelHit {
    enum class Kind : std::uint8_t { None, Knob, Button, SelectorPrev, SelectorNext, SelectorBox, Bypass };
    Kind kind  = Kind::None;
    int  index = -1;   // into layout.knobs / buttons / selectors
};
[[nodiscard]] PanelHit hitTestPanel(const PanelLayout& layout, float x, float y) noexcept;

// Text and bar position for a meter value ("452", "A4", "-6.0" ...).
[[nodiscard]] std::string formatPanelMeter(const PanelMeter& m, float value);
[[nodiscard]] float       panelMeterFraction(const PanelMeter& m, float value) noexcept;   // 0..1 (centred: -1..1)

// Mouse handling in panel design units. Knobs: drag up/down (Shift / Ctrl = fine), wheel,
// double-click = default. Buttons select / toggle; selectors step (wrap around).
class FxPanelController {
public:
    FxPanelController(const PanelLayout& layout, const PanelSpec& spec, PanelHost& host) noexcept
        : layout_(layout), spec_(spec), host_(host) {}
    void mouseDown(float x, float y, bool doubleClick, bool fine);
    void mouseMove(float x, float y, bool fine);
    void mouseUp(float x, float y);
    void wheel(float x, float y, float notches, bool fine);
    [[nodiscard]] PanelHit hover() const noexcept { return hover_; }
    [[nodiscard]] PanelHit active() const noexcept { return active_; }

    // Drag distance for the whole range at zoom 1 (× layout zoom). Shift / Ctrl = fine (× 0.15).
    static constexpr float kDragUnitsForFullRange = 560.0f;

private:
    void stepChoice(int param, int delta);
    const PanelLayout& layout_;
    const PanelSpec&   spec_;
    PanelHost&         host_;
    PanelHit hover_, active_;
    float    lastY_ = 0, dragValue_ = 0;
};

} // namespace hc::ui

// ---- HEM effects -> panel ------------------------------------------------------------------
namespace hc::fx { class Effect; struct EffectInfo; }
namespace hc::ui {
// The faceplate description of one HEM effect: its parameters, meters (plus a gain-reduction
// meter for dynamics effects that publish one and have no meter of their own).
[[nodiscard]] PanelSpec panelSpecFor(const fx::EffectInfo& info, const fx::Effect& effect);
} // namespace hc::ui

namespace hc::ui {
// PanelHost straight onto an hc::fx::Effect (The Hybrid Core's FX rack, snapshots, tests).
// Parameter writes go to the effect's lock-free atomics; meters read its published values
// (the extra gain-reduction meter of panelSpecFor() reads reductionDb()).
class EffectPanelHost : public PanelHost {
public:
    explicit EffectPanelHost(fx::Effect& effect) noexcept : fx_(effect) {}
    [[nodiscard]] float normalized(int param) const override;
    void beginEdit(int) override {}
    void setNormalized(int param, float value) override;
    void endEdit(int) override {}
    [[nodiscard]] std::string text(int param) const override;
    [[nodiscard]] float meter(int m) const override;
    [[nodiscard]] bool active(int param) const override;
    [[nodiscard]] bool bypassed() const override { return bypass_; }
    void setBypassed(bool on) override { bypass_ = on; }

private:
    fx::Effect& fx_;
    bool bypass_ = false;
};
} // namespace hc::ui
