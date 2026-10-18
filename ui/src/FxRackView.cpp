#include "hybrid/ui/FxRackView.hpp"
#include "hybrid/fx/Effects.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace hc::ui {
namespace {

const Rgba kViolet = rgb8(0x9B5CFF), kLime = rgb8(0xC0FF3E);
const Rgba kText = rgb8(0xE8E6F2), kDim = rgb8(0x8C88A0);
const Rgba kGlass = rgb8(0x050507, 0.92f);

void button(Renderer& r, Rect b, const std::string& text, bool lit, bool hot, Rgba colour, TextStyle ts = TextStyle::Label) {
    if (lit) {
        r.glow(b.inset(4), 12, 10.0f, colour.withAlpha(0.35f), true);
        r.fillRound(b, 12, colour.withAlpha(0.92f), Blend::Over, false);
        r.fillRound({b.x + 8, b.y + 5, b.w - 16, b.h * 0.33f}, 9, {1, 1, 1, 0.15f}, Blend::Add);
        r.text(ts, text, b.cx(), b.cy() + r.capHeight(ts) * 0.5f, rgb8(0x0A0812), Align::Center, TextFx::Plain);
    } else {
        r.fillRound(b, 12, kGlass, Blend::Over);
        r.fillRound({b.x + 3, b.y + 3, b.w - 6, 3}, 1.5f, colour.withAlpha(hot ? 0.9f : 0.35f), Blend::Over, true);
        r.text(ts, text, b.cx(), b.cy() + r.capHeight(ts) * 0.5f, hot ? kText : kDim, Align::Center, TextFx::Shadowed);
    }
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Shortens `s` with ".." until it fits `width` in style `ts`.
std::string fitText(const Renderer& r, TextStyle ts, std::string s, float width) {
    if (r.textWidth(ts, s) <= width) return s;
    while (s.size() > 3 && r.textWidth(ts, s + "..") > width) s.pop_back();
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "..";
}

// The faceplate size that fills the space: as big as fits, never more than twice the base size.
float zoomFor(const PanelLayout& base, Rect room) {
    return std::clamp(std::min(room.w * 0.97f / base.width, room.h * 0.97f / base.height), 0.5f, 2.0f);
}

// The faceplate's host: knobs go to the live effect, BYPASS goes to the rack slot.
class RackHost final : public EffectPanelHost {
public:
    RackHost(fx::Effect& e, FxRackModel& m, std::uint32_t stem, int slot) : EffectPanelHost(e), model_(m), stem_(stem), slot_(slot) {}
    bool bypassed() const override { return model_.bypass(stem_, slot_); }
    void setBypassed(bool on) override { model_.setBypass(stem_, slot_, on); }
private:
    FxRackModel&  model_;
    std::uint32_t stem_;
    int           slot_;
};

} // namespace

struct FxRackView::Panel {
    PanelSpec   spec;
    PanelLayout layout;
    std::unique_ptr<RackHost>          host;
    std::unique_ptr<FxPanelView>       view;
    std::unique_ptr<FxPanelController> ctrl;
};

void FxRackView::setArea(Rect area) noexcept {
    area_ = area;
    layout();
}

void FxRackView::layout() {
    left_  = {area_.x + 30, area_.y + 30, 720, area_.h - 60};
    right_ = {left_.right() + 40, area_.y + 30, area_.right() - 30 - (left_.right() + 40), area_.h - 60};
}

void FxRackView::select(std::uint32_t stem, int slot) noexcept {
    stem_ = std::min<std::uint32_t>(stem, hc::kStemCount - 1);
    slot_ = std::clamp(slot, 0, kSlots - 1);
}

Rect FxRackView::stemButton(std::uint32_t s) const noexcept {
    const float w = (left_.w - 16) / 2, h = 64, gap = 10;
    const float col = static_cast<float>(s % 2), row = static_cast<float>(s / 2);
    return {left_.x + col * (w + 16), left_.y + 84 + row * (h + gap), w, h};
}
Rect FxRackView::slotRow(int k) const noexcept { return {left_.x, left_.y + 680 + static_cast<float>(k) * 118, left_.w, 102}; }
Rect FxRackView::slotName(int k) const noexcept { const Rect r = slotRow(k); return {r.x + 70, r.y + 10, r.w - 70 - 250, r.h - 20}; }
Rect FxRackView::slotOn(int k) const noexcept { const Rect r = slotRow(k); return {r.right() - 240, r.y + 18, 136, r.h - 36}; }
Rect FxRackView::slotRemove(int k) const noexcept { const Rect r = slotRow(k); return {r.right() - 92, r.y + 18, 92, r.h - 36}; }
Rect FxRackView::tempoDown() const noexcept { return {left_.x + 330, left_.y + 6, 62, 68}; }
Rect FxRackView::tempoBox() const noexcept { return {left_.x + 400, left_.y + 2, left_.w - 400 - 70, 76}; }
Rect FxRackView::tempoUp() const noexcept { return {left_.right() - 62, left_.y + 6, 62, 68}; }
Rect FxRackView::pickerButton(std::size_t i) const noexcept {
    const int cols = 4;
    const float gap = 18;
    const float w = (right_.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const std::size_t n = fx::effects().size();
    const int rows = static_cast<int>((n + cols - 1) / cols);
    const float h = std::min(140.0f, (right_.h - 260 - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows));
    return {right_.x + static_cast<float>(i % cols) * (w + gap), right_.y + 90 + static_cast<float>(i / cols) * (h + gap), w, h};
}

void FxRackView::syncPanel(FxRackModel& model, Renderer* r, float scale) {
    fx::Effect* e = model.effect(stem_, slot_);
    if (e != shown_) {
        panel_.reset();
        shown_ = e;
        uploadedScale_ = 0.0f;
        if (e) {
            const int t = model.type(stem_, slot_);
            const auto list = fx::effects();
            if (t >= 0 && t < static_cast<int>(list.size())) {
                auto p = std::make_shared<Panel>();
                p->spec = panelSpecFor(list[static_cast<std::size_t>(t)], *e);
                // the shape (controls per row) that lets the plate grow the most in this space
                int bestCols = 0;
                zoom_ = zoomFor(layoutFxPanel(p->spec), right_);
                for (int cols = 2; cols <= 10; ++cols) {
                    const float zc = zoomFor(layoutFxPanel(p->spec, 1.0f, cols), right_);
                    if (zc > zoom_ + 0.01f) { zoom_ = zc; bestCols = cols; }
                }
                p->layout = layoutFxPanel(p->spec, zoom_, bestCols);
                p->host = std::make_unique<RackHost>(*e, model, stem_, slot_);
                p->view = std::make_unique<FxPanelView>(p->layout, p->spec);
                p->ctrl = std::make_unique<FxPanelController>(p->layout, p->spec, *p->host);
                panel_ = std::move(p);
            }
        }
    }
    if (panel_ && r && std::fabs(uploadedScale_ - scale) > 1e-4f) {
        const PanelLayout& L = panel_->layout;
        const float ppu = std::min({scale, 4096.0f / L.width, 4096.0f / L.height});
        std::string err;
        if (r->setPanel(buildMips(premultiply(renderFxPlate(L, FxPalette{}, ppu))), &err)) uploadedScale_ = scale;
    }
    if (panel_) {
        ox_ = std::round(right_.cx() - panel_->layout.width * 0.5f);
        oy_ = std::round(right_.y + std::max(0.0f, (right_.h - panel_->layout.height) * 0.5f));
    }
}

bool FxRackView::mouseDown(float x, float y, bool doubleClick, bool fine, FxRackModel& model) {
    if (!area_.contains(x, y)) return false;
    const float step = fine ? 0.1f : 1.0f;
    if (tempoDown().contains(x, y)) { model.setSongTempo(std::round((model.tempo() - step) * 10.0f) / 10.0f); return true; }
    if (tempoUp().contains(x, y))   { model.setSongTempo(std::round((model.tempo() + step) * 10.0f) / 10.0f); return true; }
    if (tempoBox().contains(x, y)) {
        if (doubleClick) { model.setSongTempo(0.0f); return true; }   // back to following Beat Forge
        tempoDrag_ = true;
        tempoY_ = y;
        tempoStart_ = model.tempo();
        dragModel_ = &model;
        return true;
    }
    for (std::uint32_t s = 0; s < hc::kStemCount; ++s)
        if (stemButton(s).contains(x, y)) { stem_ = s; picker_ = false; return true; }
    for (int k = 0; k < kSlots; ++k) {
        if (slotOn(k).contains(x, y) && model.type(stem_, k) >= 0) { model.setBypass(stem_, k, !model.bypass(stem_, k)); slot_ = k; return true; }
        if (slotRemove(k).contains(x, y) && model.type(stem_, k) >= 0) { model.setType(stem_, k, -1); slot_ = k; picker_ = false; return true; }
        if (slotRow(k).contains(x, y)) {
            slot_ = k;
            picker_ = model.type(stem_, k) < 0 || doubleClick;
            return true;
        }
    }
    if (picker_) {
        const auto list = fx::effects();
        for (std::size_t i = 0; i < list.size(); ++i)
            if (pickerButton(i).contains(x, y)) { model.setType(stem_, slot_, static_cast<int>(i)); picker_ = false; return true; }
        const Rect cancel{right_.right() - 300, right_.bottom() - 110, 300, 90};
        if (cancel.contains(x, y)) { picker_ = false; return true; }
        return true;
    }
    const Rect change{left_.x, left_.bottom() - 100, left_.w, 90};
    if (change.contains(x, y)) { picker_ = true; return true; }
    syncPanel(model, nullptr, 0.0f);
    if (panel_) panel_->ctrl->mouseDown(x - ox_, y - oy_, doubleClick, fine);
    return true;
}

void FxRackView::mouseMove(float x, float y, bool fine) {
    if (tempoDrag_ && dragModel_) {   // drag up / down: 1 BPM per 10 units (Shift: 0.1)
        const float bpm = tempoStart_ + (tempoY_ - y) / 10.0f * (fine ? 0.1f : 1.0f);
        dragModel_->setSongTempo(std::round(bpm * 10.0f) / 10.0f);
        return;
    }
    hoverStem_ = hoverSlot_ = hoverPick_ = -1;
    for (std::uint32_t s = 0; s < hc::kStemCount; ++s) if (stemButton(s).contains(x, y)) hoverStem_ = static_cast<int>(s);
    for (int k = 0; k < kSlots; ++k) if (slotRow(k).contains(x, y)) hoverSlot_ = k;
    if (picker_) for (std::size_t i = 0; i < fx::effects().size(); ++i) if (pickerButton(i).contains(x, y)) hoverPick_ = static_cast<int>(i);
    if (panel_ && !picker_) panel_->ctrl->mouseMove(x - ox_, y - oy_, fine);
}

void FxRackView::mouseUp(float x, float y) {
    tempoDrag_ = false;
    dragModel_ = nullptr;
    if (panel_) panel_->ctrl->mouseUp(x - ox_, y - oy_);
}

bool FxRackView::wheel(float x, float y, float notches, bool fine) {
    if (!area_.contains(x, y)) return false;
    if (tempoBox().contains(x, y) && lastModel_) {
        lastModel_->setSongTempo(std::round((lastModel_->tempo() + notches * (fine ? 0.1f : 1.0f)) * 10.0f) / 10.0f);
        return true;
    }
    if (panel_ && !picker_) panel_->ctrl->wheel(x - ox_, y - oy_, notches, fine);
    return true;
}

void FxRackView::draw(Renderer& r, float canvasScale, FxRackModel& model) {
    lastModel_ = &model;
    syncPanel(model, &r, canvasScale);
    // the rack's own glass, over the stem strips
    r.fillRound(area_, 28, rgb8(0x060509, 1.0f), Blend::Over);
    r.fillRound({area_.x + 30, area_.y + 6, area_.w - 60, 4}, 2, kViolet, Blend::Over, true);
    r.fillRound({area_.x + 30, area_.bottom() - 10, area_.w - 60, 4}, 2, kViolet, Blend::Over, true);

    // ---- left: strips and inserts
    r.text(TextStyle::Heading, "FX RACK", left_.x, left_.y + 56, kLime, Align::Left, TextFx::Neon);
    for (std::uint32_t s = 0; s < hc::kStemCount; ++s) {
        int used = 0;
        for (int k = 0; k < kSlots; ++k) used += model.type(s, k) >= 0;
        const Rect b = stemButton(s);
        const std::string tag = used ? "  [" + std::to_string(used) + "]" : std::string();
        const std::string name = fitText(r, TextStyle::Caption, upper(model.stemName(s)), b.w - 24 - r.textWidth(TextStyle::Caption, tag)) + tag;
        button(r, b, name, s == stem_, hoverStem_ == static_cast<int>(s), used ? kLime : kViolet, TextStyle::Caption);
    }
    // song tempo (HEM ECHO's sync): - / + buttons, drag or wheel the box, double-click = follow Beat Forge
    {
        char bpm[32];
        std::snprintf(bpm, sizeof bpm, "%.1f BPM", static_cast<double>(model.tempo()));
        const Rect tb = tempoBox();
        r.fillRound(tb, 14, kGlass, Blend::Over);
        r.fillRound({tb.x + 3, tb.y + 3, tb.w - 6, 3}, 1.5f, (model.tempoSet() ? kLime : kViolet).withAlpha(0.7f), Blend::Over, true);
        r.text(TextStyle::Caption, model.tempoSet() ? "SONG TEMPO" : "TEMPO  (BEAT FORGE)", tb.cx(), tb.y + 30, kDim, Align::Center, TextFx::Shadowed);
        r.text(TextStyle::Title, bpm, tb.cx(), tb.y + 66, kLime, Align::Center, TextFx::NeonCrisp);
        button(r, tempoDown(), "-", false, false, kViolet, TextStyle::Title);
        button(r, tempoUp(), "+", false, false, kViolet, TextStyle::Title);
    }
    const std::string title = fitText(r, TextStyle::Label, "INSERTS  -  " + upper(model.stemName(stem_)), left_.w);
    r.text(TextStyle::Label, title, left_.x, slotRow(0).y - 22, kViolet, Align::Left, TextFx::Neon);
    const auto list = fx::effects();
    for (int k = 0; k < kSlots; ++k) {
        const Rect row = slotRow(k);
        const int t = model.type(stem_, k);
        const bool sel = k == slot_;
        r.fillRound(row, 16, sel ? rgb8(0x14101F, 0.95f) : kGlass, Blend::Over);
        if (sel) r.fillRound({row.x, row.y + 8, 6, row.h - 16}, 3, kLime, Blend::Over, true);
        r.text(TextStyle::Panel, std::to_string(k + 1), row.x + 36, row.cy() + r.capHeight(TextStyle::Panel) * 0.5f, sel ? kLime : kDim, Align::Center,
               TextFx::Neon);
        const Rect nm = slotName(k);
        if (t >= 0) {
            r.text(TextStyle::Title, list[static_cast<std::size_t>(t)].name, nm.x + 10, nm.cy() + r.capHeight(TextStyle::Title) * 0.5f,
                   model.bypass(stem_, k) ? kDim : kText, Align::Left, TextFx::Shadowed);
            button(r, slotOn(k), model.bypass(stem_, k) ? "OFF" : "ON", !model.bypass(stem_, k), false, kLime, TextStyle::Caption);
            button(r, slotRemove(k), "X", false, false, kViolet, TextStyle::Caption);
        } else {
            r.text(TextStyle::Label, "+  ADD EFFECT", nm.x + 10, nm.cy() + r.capHeight(TextStyle::Label) * 0.5f, hoverSlot_ == k ? kLime : kDim,
                   Align::Left, TextFx::Shadowed);
        }
    }
    button(r, {left_.x, left_.bottom() - 100, left_.w, 90}, model.type(stem_, slot_) >= 0 ? "CHANGE EFFECT" : "CHOOSE EFFECT", picker_, false, kViolet);

    // ---- right: picker or faceplate
    if (picker_) {
        r.text(TextStyle::Heading, "CHOOSE AN EFFECT FOR SLOT " + std::to_string(slot_ + 1), right_.x, right_.y + 56, kLime, Align::Left, TextFx::Neon);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Rect b = pickerButton(i);
            const bool hot = hoverPick_ == static_cast<int>(i);
            r.fillRound(b, 14, hot ? rgb8(0x1A1328, 0.97f) : kGlass, Blend::Over);
            r.fillRound({b.x + 3, b.y + 3, b.w - 6, 3}, 1.5f, (hot ? kLime : kViolet).withAlpha(hot ? 0.95f : 0.5f), Blend::Over, true);
            r.text(TextStyle::Title, fitText(r, TextStyle::Title, list[i].name, b.w - 44), b.x + 22, b.y + 54, hot ? kLime : kText, Align::Left, TextFx::Neon);
            r.text(TextStyle::Caption, fitText(r, TextStyle::Caption, list[i].blurb, b.w - 44), b.x + 22, b.y + 96, kDim, Align::Left, TextFx::Shadowed);
        }
        button(r, {right_.right() - 300, right_.bottom() - 110, 300, 90}, "CANCEL", false, false, kViolet);
    } else if (panel_) {
        panel_->view->draw(r, *panel_->host, panel_->ctrl->hover(), panel_->ctrl->active(), ox_, oy_, true);
    } else {
        r.text(TextStyle::Heading, "PICK A STRIP, THEN A SLOT", right_.cx(), right_.cy() - 30, kViolet, Align::Center, TextFx::Neon);
        r.text(TextStyle::Status, "22 HEM effects - each strip takes 3, in order: slot 1, then 2, then 3", right_.cx(), right_.cy() + 40, kDim,
               Align::Center, TextFx::Shadowed);
    }
}

} // namespace hc::ui
