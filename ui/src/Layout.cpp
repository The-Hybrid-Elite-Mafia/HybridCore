#include "hybrid/ui/Layout.hpp"
#include "hybrid/params/MixerParams.hpp"

#include <algorithm>
#include <stdexcept>

namespace hc::ui {

namespace {
constexpr float kStripTop    = 84.0f;
constexpr float kStripHeight = 840.0f;
constexpr float kStemPitch   = 100.0f;
constexpr float kStemWidth   = 96.0f;
constexpr float kLeft        = 16.0f;
constexpr float kBusPitch    = 82.0f;
constexpr float kBusWidth    = 78.0f;

int paramIndex(const params::ParameterTree& tree, const std::string& id) {
    const params::Parameter* p = tree.find(id);
    if (!p) throw std::runtime_error("UI layout: missing parameter " + id);
    return static_cast<int>(p->index());
}
} // namespace

Layout::Layout(const params::ParameterTree& tree) {
    const std::uint32_t stems = hc::kStemCount;
    const std::uint32_t buses = hc::kBusCount;

    // Header buttons (right side).
    const Rect theme{kDesignWidth - 24 - 110, 19, 110, 34};
    const Rect source{theme.x - 10 - 130, 19, 130, 34};
    const Rect device{source.x - 10 - 380, 19, 380, 34};
    widgets_.push_back({WidgetKind::HeaderButton, device, -1, 0, HeaderAction::CycleDevice, "DEVICE"});
    widgets_.push_back({WidgetKind::HeaderButton, source, -1, 0, HeaderAction::ToggleSource, "SOURCE"});
    widgets_.push_back({WidgetKind::HeaderButton, theme, -1, 0, HeaderAction::CycleTheme, "THEME"});

    for (std::uint32_t s = 0; s < stems; ++s) {
        const float x  = kLeft + static_cast<float>(s) * kStemPitch;
        const float cx = x + kStemWidth * 0.5f;
        const float y0 = kStripTop;

        StripLayout st;
        st.panel = {x, y0, kStemWidth, kStripHeight};
        st.index = s;
        st.title = std::to_string(s + 1);
        st.faderTrack = {cx - 18, y0 + 290, 8, 492};
        st.faderParam = paramIndex(tree, params::stemParamId(s, "main"));
        strips_.push_back(st);

        widgets_.push_back({WidgetKind::Knob, {cx - 22, y0 + 40, 44, 44}, paramIndex(tree, params::stemParamId(s, "trim")), s, HeaderAction::None, "TRIM"});
        widgets_.push_back({WidgetKind::Knob, {cx - 22, y0 + 108, 44, 44}, paramIndex(tree, params::stemParamId(s, "pan")), s, HeaderAction::None, "PAN"});
        const char* auxIds[]  = {"aux1", "aux2", "aux3"};
        const char* auxCaps[] = {"A1", "A2", "A3"};
        for (int a = 0; a < 3; ++a)
            widgets_.push_back({WidgetKind::SmallKnob, {cx - 44 + static_cast<float>(a) * 30, y0 + 182, 28, 28},
                                paramIndex(tree, params::stemParamId(s, auxIds[a])), s, HeaderAction::None, auxCaps[a]});
        const char* btnIds[]  = {"mute", "solo", "invert"};
        const char* btnCaps[] = {"M", "S", "\xC3\x98"};   // UTF-8 "Ø"
        for (int b = 0; b < 3; ++b)
            widgets_.push_back({WidgetKind::Button, {cx - 43 + static_cast<float>(b) * 30, y0 + 236, 26, 24},
                                paramIndex(tree, params::stemParamId(s, btnIds[b])), s, HeaderAction::None, btnCaps[b]});
        // Fader hit area is wider than the slot so the cap is easy to grab.
        widgets_.push_back({WidgetKind::Fader, {cx - 36, y0 + 278, 44, 516}, st.faderParam, s, HeaderAction::None, "MAIN"});
        widgets_.push_back({WidgetKind::StemMeter, {cx + 18, y0 + 290, 8, 492}, -1, s, HeaderAction::None, ""});
    }

    const float busLeft = kLeft + static_cast<float>(stems) * kStemPitch + 12.0f;
    for (std::uint32_t b = 0; b < buses; ++b) {
        const float x  = busLeft + static_cast<float>(b) * kBusPitch;
        const float cx = x + kBusWidth * 0.5f;
        const float y0 = kStripTop;

        StripLayout st;
        st.panel = {x, y0, kBusWidth, kStripHeight};
        st.isBus = true;
        st.index = b;
        st.title = params::busName(b);
        st.faderTrack = {cx - 20, y0 + 90, 8, 692};
        st.faderParam = paramIndex(tree, params::busParamId(b, "gain"));
        strips_.push_back(st);

        widgets_.push_back({WidgetKind::Button, {cx - 16, y0 + 44, 32, 26}, paramIndex(tree, params::busParamId(b, "mute")), b, HeaderAction::None, "M"});
        widgets_.push_back({WidgetKind::Fader, {cx - 38, y0 + 78, 44, 716}, st.faderParam, b, HeaderAction::None, "LEVEL"});
        widgets_.push_back({WidgetKind::BusMeterL, {cx + 10, y0 + 90, 7, 692}, -1, b, HeaderAction::None, ""});
        widgets_.push_back({WidgetKind::BusMeterR, {cx + 20, y0 + 90, 7, 692}, -1, b, HeaderAction::None, ""});
    }
}

int Layout::hitTest(float x, float y) const noexcept {
    for (std::size_t i = 0; i < widgets_.size(); ++i) {
        const Widget& w = widgets_[i];
        if (w.kind == WidgetKind::StemMeter || w.kind == WidgetKind::BusMeterL || w.kind == WidgetKind::BusMeterR) continue;
        if (w.rect.contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

Viewport Viewport::fit(float w, float h) noexcept {
    Viewport v;
    if (w <= 0 || h <= 0) return v;
    v.scale   = std::min(w / kDesignWidth, h / kDesignHeight);
    v.offsetX = (w - kDesignWidth * v.scale) * 0.5f;
    v.offsetY = (h - kDesignHeight * v.scale) * 0.5f;
    return v;
}

HeaderAction Interaction::mouseDown(float x, float y, bool doubleClick, bool /*fine*/) {
    const int hit = layout_.hitTest(x, y);
    active_ = -1;
    if (hit < 0) return HeaderAction::None;
    const Widget& w = layout_.widgets()[static_cast<std::size_t>(hit)];
    if (w.kind == WidgetKind::HeaderButton) return w.action;
    if (w.param < 0) return HeaderAction::None;
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));

    if (w.kind == WidgetKind::Button) {
        p.setPlain(p.isOn() ? 0.0f : 1.0f);
        return HeaderAction::None;
    }
    if (doubleClick) {
        p.resetToDefault();
        return HeaderAction::None;
    }
    active_ = hit;
    lastY_  = y;
    return HeaderAction::None;
}

void Interaction::mouseMove(float x, float y, bool fine) {
    if (active_ < 0) {
        hover_ = layout_.hitTest(x, y);
        return;
    }
    const Widget& w = layout_.widgets()[static_cast<std::size_t>(active_)];
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));
    const float dy = y - lastY_;
    lastY_ = y;
    float range = kKnobPixelsFullRange;
    if (w.kind == WidgetKind::Fader) {
        // Fader travel equals the engraved slot length, so the cap follows the mouse.
        for (const auto& st : layout_.strips())
            if (st.faderParam == w.param) { range = st.faderTrack.h; break; }
    }
    if (fine) range *= kFineDivider;
    p.setNormalized(p.normalized() - dy / range);
}

void Interaction::wheel(float x, float y, float notches, bool fine) {
    const int hit = layout_.hitTest(x, y);
    if (hit < 0) return;
    const Widget& w = layout_.widgets()[static_cast<std::size_t>(hit)];
    if (w.param < 0 || w.kind == WidgetKind::Button || w.kind == WidgetKind::HeaderButton) return;
    params::Parameter& p = tree_.at(static_cast<std::size_t>(w.param));
    p.setNormalized(p.normalized() + notches * (fine ? 0.002f : 0.02f));
}

} // namespace hc::ui
