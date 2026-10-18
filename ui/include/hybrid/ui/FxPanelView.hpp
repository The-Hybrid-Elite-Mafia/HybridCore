#pragma once
// =============================================================================
//  FxPanelView — draws a HEM plug-in faceplate with the Renderer: the obsidian plate
//  (uploaded once with Renderer::setPanel), then every frame the live parts on top:
//  neon in the routed channels, the HEM crown, title, section captions, knobs in their
//  wells (value arc in HEM lime), lit buttons, value windows, meters and BYPASS.
//  Reads values through a PanelHost only; never touches DSP objects.
//  Everything is drawn at (originX, originY) in the renderer's design units, so the same
//  view works full-window (VST3 editor) and inside The Hybrid Core's console (FX rack).
// =============================================================================
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <string>

namespace hc::ui {

// Text and bar position for a meter value ("452 Hz", "A4", "-6.0 dB" ...).
[[nodiscard]] std::string formatPanelMeter(const PanelMeter& m, float value);
[[nodiscard]] float       panelMeterFraction(const PanelMeter& m, float value) noexcept;   // 0..1 (centred: -1..1)

class FxPanelView {
public:
    FxPanelView(const PanelLayout& layout, const PanelSpec& spec, FxPalette palette = {}) noexcept
        : layout_(layout), spec_(spec), pal_(palette) {}
    // plate = false when the obsidian plate is already in the renderer's static layer.
    void draw(Renderer& r, const PanelHost& host, PanelHit hover, PanelHit active, float originX = 0, float originY = 0,
              bool plate = true) const;

private:
    void drawKnob(Renderer& r, const PanelKnob& k, const PanelHost& host, bool hot, float ox, float oy) const;
    void drawButton(Renderer& r, Rect b, const std::string& text, bool lit, bool hot, Rgba colour) const;
    void drawMeter(Renderer& r, const PanelMeterSlot& m, float value, float ox, float oy) const;
    void drawCrown(Renderer& r, Rect area) const;
    const PanelLayout& layout_;
    const PanelSpec&   spec_;
    FxPalette          pal_;
};

} // namespace hc::ui
