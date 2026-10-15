#pragma once
// =============================================================================
//  MixerView — draws the whole 13-stem console with the Renderer: faceplate,
//  header, knobs, buttons, faders, neon meters. Reads parameters (atomics) and
//  meter values only; never touches DSP objects.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/Renderer.hpp"
#include "hybrid/ui/Theme.hpp"

#include <array>
#include <string>

namespace hc::ui {

// Linear peak levels from the engine plus UI-side ballistics (decay + peak hold).
struct MeterState {
    std::array<float, hc::kStemCount> stem{};
    std::array<float, hc::kBusCount>  busL{}, busR{};
    std::array<float, hc::kStemCount> stemHold{};
    std::array<float, hc::kBusCount>  busHoldL{}, busHoldR{};

    // Feed new linear peaks; dt in seconds. Falls 24 dB/s, holds peaks 1.5 s.
    void update(const float* stemPeaks, const float* busPeaksL, const float* busPeaksR, float dt) noexcept;

private:
    std::array<float, hc::kStemCount> stemHoldAge_{};
    std::array<float, hc::kBusCount>  busHoldAgeL_{}, busHoldAgeR_{};
};

struct ViewState {
    ThemeId     theme = ThemeId::Pearl;
    std::string deviceLabel;    // header DEVICE button
    std::string sourceLabel;    // header SOURCE button
    std::string status;         // header status line 1
    std::string detail;         // header status line 2 (errors in neonAlt colour when `alert`)
    bool        alert  = false;
    int         hover  = -1;    // widget indices from Interaction
    int         active = -1;
};

class MixerView {
public:
    MixerView(const Layout& layout, const params::ParameterTree& tree) noexcept : layout_(layout), tree_(tree) {}

    void draw(Renderer& r, const ViewState& view, const MeterState& meters) const;

    // Fader-taper position (0..1) for a linear level, so meters line up with fader scales.
    [[nodiscard]] static float levelToNorm(float linear) noexcept;

private:
    void drawKnob(Renderer& r, const Palette& p, const Widget& w, bool hot) const;
    void drawButton(Renderer& r, const Palette& p, const Widget& w, bool hot) const;
    void drawFader(Renderer& r, const Palette& p, const Widget& w, const StripLayout& st, bool hot) const;
    void drawMeter(Renderer& r, const Palette& p, Rect rect, float level, float hold) const;

    const Layout&                layout_;
    const params::ParameterTree& tree_;
};

} // namespace hc::ui
