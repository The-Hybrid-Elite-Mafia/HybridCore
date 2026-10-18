#pragma once
// =============================================================================
//  FxRackView — the FX RACK over the stem strips (FX in the top bar opens it).
//
//    left  : the 13 strips (pick one), then its 3 insert slots: effect name, ON / BYPASS, remove
//    right : the picked slot's effect on its own obsidian faceplate (FxPanelView, live knobs and
//            meters), or the list of all HEM effects to put in the slot
//  Talks to the engine through FxRackModel (message thread); knob turns go straight to the
//  effect's lock-free parameters. Coordinates are console design units.
// =============================================================================
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelView.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hc::fx { class Effect; }

namespace hc::ui {

class FxRackModel {
public:
    virtual ~FxRackModel() = default;
    [[nodiscard]] virtual int         type(std::uint32_t stem, int slot) const = 0;      // index into fx::effects(), -1 empty
    [[nodiscard]] virtual fx::Effect* effect(std::uint32_t stem, int slot) const = 0;
    [[nodiscard]] virtual bool        bypass(std::uint32_t stem, int slot) const = 0;
    virtual void setBypass(std::uint32_t stem, int slot, bool on) = 0;
    virtual void setType(std::uint32_t stem, int slot, int type) = 0;                    // -1 = remove
    [[nodiscard]] virtual std::string stemName(std::uint32_t stem) const = 0;
};

class FxRackView {
public:
    static constexpr int kSlots = 3;

    void setArea(Rect area) noexcept;   // console design units (the ANALYZER's rectangle)
    // Mouse in console design units; true = the rack used it.
    bool mouseDown(float x, float y, bool doubleClick, bool fine, FxRackModel& model);
    void mouseMove(float x, float y, bool fine);
    void mouseUp(float x, float y);
    bool wheel(float x, float y, float notches, bool fine);
    // Draws the rack; (re)uploads the faceplate texture when the shown effect changes.
    void draw(Renderer& r, float canvasScale, FxRackModel& model);
    [[nodiscard]] bool contains(float x, float y) const noexcept { return area_.contains(x, y); }
    [[nodiscard]] std::uint32_t stem() const noexcept { return stem_; }
    [[nodiscard]] int slot() const noexcept { return slot_; }
    void select(std::uint32_t stem, int slot) noexcept;

private:
    void layout();
    void syncPanel(FxRackModel& model, Renderer* r, float canvasScale);
    [[nodiscard]] Rect stemButton(std::uint32_t s) const noexcept;
    [[nodiscard]] Rect slotRow(int k) const noexcept;
    [[nodiscard]] Rect slotName(int k) const noexcept;
    [[nodiscard]] Rect slotOn(int k) const noexcept;
    [[nodiscard]] Rect slotRemove(int k) const noexcept;
    [[nodiscard]] Rect pickerButton(std::size_t i) const noexcept;

    Rect area_, left_, right_;
    std::uint32_t stem_ = 0;
    int           slot_ = 0;
    bool          picker_ = false;
    int           hoverStem_ = -1, hoverSlot_ = -1, hoverPick_ = -1;

    // the faceplate on the right
    struct Panel;
    std::shared_ptr<Panel> panel_;
    fx::Effect*   shown_ = nullptr;
    float         uploadedScale_ = 0.0f;
    float         ox_ = 0, oy_ = 0;
};

} // namespace hc::ui
