#pragma once
// =============================================================================
//  UI layout and interaction model (pure C++, no GPU, unit-tested).
//  Coordinates are DESIGN units = Main-Faceplate.png pixels (3840 wide). Everything
//  sits on a measured faceplate region (Faceplate.hpp):
//    top bar      : status lamp in the logo socket, engraved title
//    name panel   : device + format (engraved)
//    6 sockets    : ◀ device, device ▶, LOAD stems, SOURCE (tones / inputs / stems), clear SOLO, clear MUTE
//    glass display: live engine numbers (latency, DSP load, buffer, dropouts)
//    4 tabs       : fader bank — MAIN MIX / AUX 1 / AUX 2 / AUX 3 sends on the stem faders
//    left panel   : 4 bus strips (Main, Aux 1–3)
//    main panel   : 13 stem strips
//    bottom bar   : status line + main bus L/R meter
//  The left/main panels grow with Canvas::extraH (windows taller than 16:9).
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/Geometry.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hc::ui {

enum class WidgetKind : std::uint8_t { Knob, SendKnob, Fader, Button, StemMeter, BusMeter, MasterMeter, Transport, Tab };
enum class UiAction : std::uint8_t { None, PrevDevice, NextDevice, LoadStems, CycleSource, ClearSolo, ClearMute, FaderBank };

inline constexpr std::uint32_t kFaderBanks = hc::kBusCount;   // MAIN + AUX 1..3

struct Widget {
    WidgetKind    kind   = WidgetKind::Knob;
    Rect          rect;                  // hit area / drawn bounds (design units)
    int           param  = -1;           // parameter index, -1 if none
    std::uint32_t index  = 0;            // stem, bus, socket or tab index
    UiAction      action = UiAction::None;
    std::string   caption;
};

struct StripLayout {
    Rect          bed;                   // glass bed of the strip
    bool          isBus = false;
    std::uint32_t index = 0;
    std::string   title;
    Rgba          accent;
    Rect          faderTrack;            // recessed slot; the cap travels inside it
    float         capW = 0, capH = 0;    // fader cap size
    Rect          meter;                 // glass meter window
    Rect          valueBox;              // glass readout under the fader
    int           faderWidget = -1;
};

// Strip accent colours (sRGB): 13 stems, then Main / Aux 1–3.
[[nodiscard]] Rgba stemAccent(std::uint32_t stem) noexcept;
[[nodiscard]] Rgba busAccent(std::uint32_t bus) noexcept;

class Layout {
public:
    // Builds every widget. Throws if a mixer parameter is missing.
    Layout(const params::ParameterTree& tree, float extraH);

    [[nodiscard]] const std::vector<Widget>&      widgets() const noexcept { return widgets_; }
    [[nodiscard]] const std::vector<StripLayout>& strips() const noexcept { return strips_; }
    [[nodiscard]] int   hitTest(float x, float y) const noexcept;   // widget index or -1
    // Stem strip under a point (0-based stem), or -1. Used for drag-and-drop and right-click.
    [[nodiscard]] int   stemStripAt(float x, float y) const noexcept;
    // The title area at the top of a stem strip (name, load state).
    [[nodiscard]] static Rect titleArea(const StripLayout& st) noexcept { return {st.bed.x, st.bed.y, st.bed.w, 140}; }
    [[nodiscard]] float extraH() const noexcept { return extraH_; }

    // Which send the 13 stem faders control (0 = MAIN mix, 1..3 = AUX sends).
    [[nodiscard]] std::uint32_t faderBank() const noexcept { return bank_; }
    void setFaderBank(std::uint32_t bank) noexcept;
    [[nodiscard]] int faderParam(std::uint32_t stem, std::uint32_t bank) const noexcept { return stemFader_[stem][bank]; }

    [[nodiscard]] Rect masterMeter() const noexcept { return master_; }
    [[nodiscard]] Rect statusArea() const noexcept { return status_; }

private:
    std::vector<Widget>      widgets_;
    std::vector<StripLayout> strips_;
    std::array<std::array<int, kFaderBanks>, hc::kStemCount> stemFader_{};
    std::uint32_t bank_   = 0;
    float         extraH_ = 0;
    Rect          master_, status_;
};

// Mouse handling. Pointer positions are in design units.
class Interaction {
public:
    Interaction(Layout& layout, params::ParameterTree& tree) noexcept : layout_(layout), tree_(tree) {}

    // Returns the action of a clicked socket/tab (FaderBank: index in `actionIndex`).
    UiAction mouseDown(float x, float y, bool doubleClick, bool fine, std::uint32_t* actionIndex = nullptr);
    void     mouseMove(float x, float y, bool fine);
    void     mouseUp() noexcept { active_ = -1; pressed_ = -1; }
    void     wheel(float x, float y, float notches, bool fine);
    void     rebind(Layout& layout) noexcept { layout_ = std::ref(layout); active_ = hover_ = pressed_ = -1; }

    [[nodiscard]] int activeWidget() const noexcept { return active_; }
    [[nodiscard]] int hoverWidget() const noexcept { return hover_; }
    [[nodiscard]] int pressedWidget() const noexcept { return pressed_; }   // socket/tab held down

    static constexpr float kKnobUnitsFullRange = 700.0f;   // vertical drag (design units) for 0..1
    static constexpr float kFineDivider        = 10.0f;    // Shift / Ctrl

private:
    std::reference_wrapper<Layout> layout_;
    params::ParameterTree& tree_;
    int   active_  = -1;
    int   hover_   = -1;
    int   pressed_ = -1;
    float lastY_   = 0.0f;
};

} // namespace hc::ui
