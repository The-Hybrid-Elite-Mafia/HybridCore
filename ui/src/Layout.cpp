#include "hybrid/ui/Layout.hpp"
#include "hybrid/params/MixerParams.hpp"

#include <algorithm>
#include <stdexcept>

namespace hc::ui {

namespace {
// Strip geometry (design units).
constexpr float kPanelMargin = 14.0f;
constexpr float kStripGap    = 12.0f;
constexpr float kKnobD       = 112.0f;   // trim / pan socket diameter
constexpr float kSendD       = 66.0f;
constexpr float kSendPitch   = 70.0f;
constexpr float kButtonW     = 60.0f, kButtonH = 46.0f, kButtonPitch = 66.0f;
constexpr float kValueBoxH   = 52.0f;

int paramIndex(const params::ParameterTree& tree, const std::string& id) {
    const params::Parameter* p = tree.find(id);
    if (!p) throw std::runtime_error("UI layout: missing parameter " + id);
    return static_cast<int>(p->index());
}

constexpr std::uint32_t kStemColours[] = {0xFF8A1F, 0x22C7E8, 0xF04848, 0x3FD15A, 0xA35CF0, 0x18C9A6, 0xFFC02E,
                                           0xFF5FA8, 0x9BE33C, 0x3F8CFF, 0xE8D84A, 0xE06CFF, 0x5CE1FF};
constexpr std::uint32_t kBusColours[]  = {0xF2B33D, 0x3F8CFF, 0x2EC4FF, 0x7D7BFF};
} // namespace

Rgba stemAccent(std::uint32_t s) noexcept { return rgb8(kStemColours[s % std::size(kStemColours)]); }
Rgba busAccent(std::uint32_t b) noexcept { return rgb8(kBusColours[b % std::size(kBusColours)]); }

Layout::Layout(const params::ParameterTree& tree, float extraH) : extraH_(std::max(0.0f, extraH)) {
    // ---- transport sockets and tabs ----
    static const UiAction kSocketActions[6] = {UiAction::PrevDevice, UiAction::NextDevice, UiAction::LoadStems,
                                               UiAction::CycleSource, UiAction::ClearSolo, UiAction::ClearMute};
    static const char* const kSocketCaps[6] = {"DEVICE", "DEVICE", "LOAD", "SOURCE", "CLR SOLO", "CLR MUTE"};
    for (std::uint32_t i = 0; i < plate::kSockets.size(); ++i) {
        const Circle c = plate::kSockets[i];
        const float R = plate::kSocketOuterRadius;
        widgets_.push_back({WidgetKind::Transport, {c.cx - R, c.cy - R, 2 * R, 2 * R}, -1, i, kSocketActions[i], kSocketCaps[i]});
    }
    static const char* const kTabCaps[4] = {"MAIN MIX", "AUX 1 SENDS", "AUX 2 SENDS", "AUX 3 SENDS"};
    for (std::uint32_t i = 0; i < plate::kTabFaces.size(); ++i)
        widgets_.push_back({WidgetKind::Tab, plate::kTabFaces[i], -1, i, UiAction::FaderBank, kTabCaps[i]});

    // ---- stem strips (main panel) ----
    const float bottomPad = kPanelMargin;
    const Rect  mainP{plate::kMainPanel.x, plate::kMainPanel.y, plate::kMainPanel.w, plate::kMainPanel.h + extraH_};
    const float stemPitch = (mainP.w - 2 * kPanelMargin + kStripGap) / static_cast<float>(hc::kStemCount);
    const float stemW     = stemPitch - kStripGap;
    const float y0        = mainP.y + kPanelMargin;
    const float stripH    = mainP.h - kPanelMargin - bottomPad;

    for (std::uint32_t s = 0; s < hc::kStemCount; ++s) {
        const float x  = mainP.x + kPanelMargin + static_cast<float>(s) * stemPitch;
        const float cx = x + stemW * 0.5f;
        StripLayout st;
        st.bed    = {x, y0, stemW, stripH};
        st.index  = s;
        st.title  = "STEM " + std::to_string(s + 1);
        st.accent = stemAccent(s);
        for (std::uint32_t b = 0; b < kFaderBanks; ++b)
            stemFader_[s][b] = paramIndex(tree, params::stemParamId(s, b == 0 ? "main" : (b == 1 ? "aux1" : b == 2 ? "aux2" : "aux3")));

        widgets_.push_back({WidgetKind::Knob, {cx - kKnobD / 2, y0 + 222 - kKnobD / 2, kKnobD, kKnobD},
                            paramIndex(tree, params::stemParamId(s, "trim")), s, UiAction::None, "TRIM"});
        widgets_.push_back({WidgetKind::Knob, {cx - kKnobD / 2, y0 + 382 - kKnobD / 2, kKnobD, kKnobD},
                            paramIndex(tree, params::stemParamId(s, "pan")), s, UiAction::None, "PAN"});
        static const char* const kAux[3] = {"aux1", "aux2", "aux3"};
        static const char* const kAuxCap[3] = {"A1", "A2", "A3"};
        for (int a = 0; a < 3; ++a) {
            const float kx = cx + static_cast<float>(a - 1) * kSendPitch;
            widgets_.push_back({WidgetKind::SendKnob, {kx - kSendD / 2, y0 + 520 - kSendD / 2, kSendD, kSendD},
                                paramIndex(tree, params::stemParamId(s, kAux[a])), s, UiAction::None, kAuxCap[a]});
        }
        static const char* const kBtn[3] = {"mute", "solo", "invert"};
        static const char* const kBtnCap[3] = {"M", "S", "\xC3\x98"};   // UTF-8 "Ø"
        for (int b = 0; b < 3; ++b) {
            const float bx = cx + static_cast<float>(b - 1) * kButtonPitch;
            widgets_.push_back({WidgetKind::Button, {bx - kButtonW / 2, y0 + 580, kButtonW, kButtonH},
                                paramIndex(tree, params::stemParamId(s, kBtn[b])), s, UiAction::None, kBtnCap[b]});
        }

        const float fTop = y0 + 652, fBottom = y0 + stripH - kValueBoxH - 30;
        st.capW = 100.0f;
        st.capH = 53.0f;
        st.faderTrack = {cx - 54 - 13, fTop, 26, fBottom - fTop};
        st.meter      = {cx + 50, fTop, 38, fBottom - fTop};
        st.valueBox   = {x + 22, y0 + stripH - kValueBoxH - 16, stemW - 44, kValueBoxH};
        st.faderWidget = static_cast<int>(widgets_.size());
        widgets_.push_back({WidgetKind::Fader, {cx - 54 - st.capW / 2, fTop, st.capW, fBottom - fTop}, stemFader_[s][0], s, UiAction::None, "MAIN"});
        widgets_.push_back({WidgetKind::StemMeter, st.meter, -1, s, UiAction::None, ""});
        strips_.push_back(st);
    }

    // ---- bus strips (left panel) ----
    const Rect  leftP{plate::kLeftPanel.x, plate::kLeftPanel.y, plate::kLeftPanel.w, plate::kLeftPanel.h + extraH_};
    const float busPitch = (leftP.w - 2 * kPanelMargin + kStripGap) / static_cast<float>(hc::kBusCount);
    const float busW     = busPitch - kStripGap;
    for (std::uint32_t b = 0; b < hc::kBusCount; ++b) {
        const float x  = leftP.x + kPanelMargin + static_cast<float>(b) * busPitch;
        const float cx = x + busW * 0.5f;
        StripLayout st;
        st.bed    = {x, y0, busW, stripH};
        st.isBus  = true;
        st.index  = b;
        st.title  = b == 0 ? "MAIN" : "AUX " + std::to_string(b);
        st.accent = busAccent(b);
        widgets_.push_back({WidgetKind::Button, {cx - kButtonW / 2, y0 + 112, kButtonW, kButtonH},
                            paramIndex(tree, params::busParamId(b, "mute")), b, UiAction::None, "M"});
        const float fTop = y0 + 200, fBottom = y0 + stripH - kValueBoxH - 30;
        st.capW = 84.0f;
        st.capH = 45.0f;
        st.faderTrack = {cx - 28 - 12, fTop, 24, fBottom - fTop};
        st.meter      = {cx + 24, fTop, 34, fBottom - fTop};
        st.valueBox   = {x + 12, y0 + stripH - kValueBoxH - 16, busW - 24, kValueBoxH};
        st.faderWidget = static_cast<int>(widgets_.size());
        widgets_.push_back({WidgetKind::Fader, {cx - 28 - st.capW / 2, fTop, st.capW, fBottom - fTop},
                            paramIndex(tree, params::busParamId(b, "gain")), b, UiAction::None, "LEVEL"});
        widgets_.push_back({WidgetKind::BusMeter, st.meter, -1, b, UiAction::None, ""});
        strips_.push_back(st);
    }

    // ---- bottom bar ----
    const float by = plate::kBottomBar.y + extraH_;
    master_ = {2900, by + 34, 820, 74};
    status_ = {150, by, 2700, plate::kBottomBar.h};
    widgets_.push_back({WidgetKind::MasterMeter, master_, -1, 0, UiAction::None, ""});
}

void Layout::setFaderBank(std::uint32_t bank) noexcept {
    bank_ = std::min(bank, kFaderBanks - 1);
    for (const StripLayout& st : strips_)
        if (!st.isBus) widgets_[static_cast<std::size_t>(st.faderWidget)].param = stemFader_[st.index][bank_];
}

int Layout::hitTest(float x, float y) const noexcept {
    for (std::size_t i = 0; i < widgets_.size(); ++i) {
        const Widget& w = widgets_[i];
        if (w.kind == WidgetKind::StemMeter || w.kind == WidgetKind::BusMeter || w.kind == WidgetKind::MasterMeter) continue;
        if (!w.rect.contains(x, y)) continue;
        if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::SendKnob || w.kind == WidgetKind::Transport) {
            const float dx = x - w.rect.cx(), dy = y - w.rect.cy(), r = w.rect.w * 0.5f;
            if (dx * dx + dy * dy > r * r) continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

int Layout::stemStripAt(float x, float y) const noexcept {
    for (const StripLayout& st : strips_)
        if (!st.isBus && st.bed.contains(x, y)) return static_cast<int>(st.index);
    return -1;
}

UiAction Interaction::mouseDown(float x, float y, bool doubleClick, bool /*fine*/, std::uint32_t* actionIndex) {
    const int hit = layout_.get().hitTest(x, y);
    active_ = -1;
    pressed_ = -1;
    if (hit < 0) return UiAction::None;
    const Widget& w = layout_.get().widgets()[static_cast<std::size_t>(hit)];
    if (w.kind == WidgetKind::Transport || w.kind == WidgetKind::Tab) {
        pressed_ = hit;
        if (actionIndex) *actionIndex = w.index;
        return w.action;
    }
    if (w.param < 0) return UiAction::None;
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));
    if (w.kind == WidgetKind::Button) {
        p.setPlain(p.isOn() ? 0.0f : 1.0f);
        pressed_ = hit;
        return UiAction::None;
    }
    if (doubleClick) {
        p.resetToDefault();
        return UiAction::None;
    }
    active_ = hit;
    lastY_  = y;
    return UiAction::None;
}

void Interaction::mouseMove(float x, float y, bool fine) {
    if (active_ < 0) {
        hover_ = layout_.get().hitTest(x, y);
        return;
    }
    const Widget& w = layout_.get().widgets()[static_cast<std::size_t>(active_)];
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));
    const float dy = y - lastY_;
    lastY_ = y;
    float range = kKnobUnitsFullRange;
    if (w.kind == WidgetKind::Fader) {
        // Cap travel equals the slot length minus the cap, so the cap follows the mouse.
        for (const auto& st : layout_.get().strips())
            if (st.faderWidget == active_) { range = std::max(1.0f, st.faderTrack.h - st.capH); break; }
    }
    if (fine) range *= kFineDivider;
    p.setNormalized(p.normalized() - dy / range);
}

void Interaction::wheel(float x, float y, float notches, bool fine) {
    const int hit = layout_.get().hitTest(x, y);
    if (hit < 0) return;
    const Widget& w = layout_.get().widgets()[static_cast<std::size_t>(hit)];
    if (w.param < 0 || w.kind == WidgetKind::Button) return;
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));
    p.setNormalized(p.normalized() + notches * (fine ? 0.002f : 0.02f));
}

} // namespace hc::ui
