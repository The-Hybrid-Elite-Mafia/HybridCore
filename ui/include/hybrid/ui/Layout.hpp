#pragma once
// =============================================================================
//  UI layout and interaction model (pure C++, no GPU, unit-tested).
//  All coordinates are in DESIGN units: a 1673×940 canvas that the renderer scales
//  (letterboxed) to the window. Widgets bind to parameters by index.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hc::ui {

inline constexpr float kDesignWidth  = 1673.0f;
inline constexpr float kDesignHeight = 940.0f;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool  contains(float px, float py) const noexcept { return px >= x && py >= y && px < x + w && py < y + h; }
    [[nodiscard]] float cx() const noexcept { return x + w * 0.5f; }
    [[nodiscard]] float cy() const noexcept { return y + h * 0.5f; }
};

enum class WidgetKind : std::uint8_t { Knob, SmallKnob, Fader, Button, StemMeter, BusMeterL, BusMeterR, HeaderButton };
enum class HeaderAction : std::uint8_t { None, CycleDevice, ToggleSource, CycleTheme };

struct Widget {
    WidgetKind    kind      = WidgetKind::Knob;
    Rect          rect;                    // hit area (design units)
    int           param     = -1;          // parameter index, -1 if none
    std::uint32_t channel   = 0;           // stem or bus index for meters / labels
    HeaderAction  action    = HeaderAction::None;
    std::string   caption;                 // short label drawn by the renderer
};

struct StripLayout {
    Rect          panel;                   // recessed panel behind the strip
    bool          isBus = false;
    std::uint32_t index = 0;
    std::string   title;
    Rect          faderTrack;              // engraved slot
    int           faderParam = -1;
};

class Layout {
public:
    // Builds every widget for the 13-stem mixer. Throws if a mixer parameter is missing.
    explicit Layout(const params::ParameterTree& tree);

    [[nodiscard]] const std::vector<Widget>&      widgets() const noexcept { return widgets_; }
    [[nodiscard]] const std::vector<StripLayout>& strips() const noexcept { return strips_; }
    [[nodiscard]] Rect headerBar() const noexcept { return {0, 0, kDesignWidth, 72}; }
    [[nodiscard]] int  hitTest(float x, float y) const noexcept;   // widget index or -1

private:
    std::vector<Widget>      widgets_;
    std::vector<StripLayout> strips_;
};

// Maps window pixels to design units (letterboxed, aspect preserved).
struct Viewport {
    float scale   = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    static Viewport fit(float windowW, float windowH) noexcept;
    [[nodiscard]] float toDesignX(float px) const noexcept { return (px - offsetX) / scale; }
    [[nodiscard]] float toDesignY(float py) const noexcept { return (py - offsetY) / scale; }
};

// Mouse handling. Pointer positions are in design units.
class Interaction {
public:
    Interaction(const Layout& layout, params::ParameterTree& tree) noexcept : layout_(layout), tree_(tree) {}

    // Returns a header action when a header button was clicked.
    HeaderAction mouseDown(float x, float y, bool doubleClick, bool fine);
    void         mouseMove(float x, float y, bool fine);
    void         mouseUp() noexcept { active_ = -1; }
    void         wheel(float x, float y, float notches, bool fine);

    [[nodiscard]] int activeWidget() const noexcept { return active_; }
    [[nodiscard]] int hoverWidget() const noexcept { return hover_; }

    static constexpr float kKnobPixelsFullRange = 250.0f;   // vertical drag for 0..1
    static constexpr float kFineDivider         = 10.0f;    // Shift / Ctrl

private:
    const Layout&          layout_;
    params::ParameterTree& tree_;
    int   active_ = -1;
    int   hover_  = -1;
    float lastY_  = 0.0f;
};

} // namespace hc::ui
