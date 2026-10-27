// HEM DIGITAL CONSOLE — mouse handling. Knobs: drag up / down (Shift or Ctrl = fine), wheel, double-click = start
// position. Stepped knobs and selectors move one step per wheel notch. Switches latch on press. Buttons fire on
// release over the button. Faders follow the mouse (grabbed where you clicked). The song bar seeks.
#include "hybrid/console/ui/ConsoleUi.hpp"

#include <algorithm>
#include <cmath>

namespace hc::console::ui {
namespace {

bool clickable(const Widget& w) {
    if (!widgetVisible(w)) return false;
    switch (w.kind) {
    case Kind::Knob: case Kind::Stepped: case Kind::Switch: case Kind::Fader: case Kind::Selector: case Kind::Button: case Kind::Seek: return true;
    case Kind::Custom: return w.custom == CustomId::StemName;
    default: return false;
    }
}
// Fader cap: where the cap sits for the current value (design units).
float faderCapY(const Widget& w) { return w.r.y + w.r.h * (1.0f - widgetNorm(w)); }
int steps(const Widget& w) { return w.ctl.valid() ? static_cast<int>(w.ctl.info->max - w.ctl.info->min + 0.5f) : 0; }

} // namespace

int Controller::hit(float x, float y) const noexcept {
    if (!page_) return -1;
    // last drawn on top wins
    for (int i = static_cast<int>(page_->widgets.size()) - 1; i >= 0; --i) {
        const Widget& w = page_->widgets[static_cast<std::size_t>(i)];
        if (!clickable(w)) continue;
        Rect r = w.r;
        if (w.kind == Kind::Knob || w.kind == Kind::Stepped) r = r.inset(-r.w * 0.08f);
        if (w.kind == Kind::Fader) r = r.inset(-std::max(30.0f, r.w), -24);
        if (r.contains(x, y)) return i;
    }
    return -1;
}

void Controller::step(const Widget& w, int delta) {
    if (!w.ctl.valid()) return;
    const float v = w.ctl.get();
    const fx::ParamInfo& p = *w.ctl.info;
    if (p.taper == fx::Taper::Choice || p.taper == fx::Taper::Toggle) {
        const int n = steps(w) + 1;
        int k = static_cast<int>(std::lround(v - p.min)) + delta;
        if (w.kind == Kind::Selector) k = ((k % n) + n) % n;   // selectors wrap
        w.ctl.set(fx::clampPlain(p, p.min + static_cast<float>(std::clamp(k, 0, n - 1))));
    }
}

void Controller::mouseDown(float x, float y, bool doubleClick, bool fine) {
    (void)fine;
    const int i = hit(x, y);
    hover_ = i;
    active_ = i;
    if (i < 0) return;
    const Widget& w = page_->widgets[static_cast<std::size_t>(i)];
    lastY_ = y;
    switch (w.kind) {
    case Kind::Knob:
    case Kind::Stepped:
        if (doubleClick && w.ctl.valid()) { w.ctl.set(w.ctl.info->def); active_ = -1; return; }
        dragNorm_ = widgetNorm(w);
        break;
    case Kind::Fader: {
        if (doubleClick && w.ctl.valid()) { w.ctl.set(w.ctl.info->def); active_ = -1; return; }
        const float capY = faderCapY(w);
        // grabbed on the cap: keep the offset; elsewhere on the slot: the cap jumps to the mouse
        grabOffset_ = std::fabs(y - capY) < 40.0f ? y - capY : 0.0f;
        if (grabOffset_ == 0.0f) widgetSetNorm(w, 1.0f - (y - w.r.y) / w.r.h);
        break;
    }
    case Kind::Switch:
        if (w.ctl.valid()) w.ctl.set(w.ctl.get() > 0.5f ? 0.0f : 1.0f);
        break;
    case Kind::Selector: step(w, +1); break;
    case Kind::Seek:
        if (emit_) emit_({Act::Seek, 0, std::clamp((x - w.r.x) / w.r.w, 0.0f, 1.0f)});
        break;
    case Kind::Custom:
        if (w.custom == CustomId::StemName && emit_) emit_({Act::LoadSlot, widgetArg(w), 0});
        active_ = -1;
        break;
    default: break;
    }
}

void Controller::rightDown(float x, float y) {
    const int i = hit(x, y);
    if (i < 0) return;
    const Widget& w = page_->widgets[static_cast<std::size_t>(i)];
    if (w.kind == Kind::Selector) step(w, -1);
    else if (w.kind == Kind::Custom && w.custom == CustomId::StemName && emit_) emit_({Act::ClearSlot, widgetArg(w), 0});
    else if ((w.kind == Kind::Knob || w.kind == Kind::Stepped || w.kind == Kind::Fader) && w.ctl.valid()) w.ctl.set(w.ctl.info->def);
}

void Controller::mouseMove(float x, float y, bool fine) {
    if (active_ < 0) { hover_ = hit(x, y); return; }
    const Widget& w = page_->widgets[static_cast<std::size_t>(active_)];
    switch (w.kind) {
    case Kind::Knob:
    case Kind::Stepped: {
        const float dy = lastY_ - y;
        lastY_ = y;
        dragNorm_ = std::clamp(dragNorm_ + dy / kDragUnitsForFullRange * (fine ? 0.15f : 1.0f), 0.0f, 1.0f);
        widgetSetNorm(w, dragNorm_);
        break;
    }
    case Kind::Fader: {
        const float target = y - grabOffset_;
        if (fine) {
            const float dy = lastY_ - y;
            widgetSetNorm(w, widgetNorm(w) + dy / w.r.h * 0.15f);
        } else {
            widgetSetNorm(w, 1.0f - (target - w.r.y) / w.r.h);
        }
        lastY_ = y;
        break;
    }
    case Kind::Seek:
        if (emit_) emit_({Act::Seek, 0, std::clamp((x - w.r.x) / w.r.w, 0.0f, 1.0f)});
        break;
    default: break;
    }
}

void Controller::mouseUp(float x, float y) {
    if (active_ >= 0) {
        const Widget& w = page_->widgets[static_cast<std::size_t>(active_)];
        if (w.kind == Kind::Button && hit(x, y) == active_ && emit_) emit_({w.act, widgetArg(w), 0});
    }
    active_ = -1;
    hover_ = hit(x, y);
}

bool Controller::wheel(float x, float y, float notches, bool fine) {
    const int i = hit(x, y);
    if (i < 0) return false;
    const Widget& w = page_->widgets[static_cast<std::size_t>(i)];
    if (!w.ctl.valid()) return true;
    const int dir = notches > 0 ? 1 : -1;
    if (w.kind == Kind::Stepped || w.kind == Kind::Selector || w.ctl.info->taper == fx::Taper::Choice) {
        for (int k = 0; k < static_cast<int>(std::max(1.0f, std::fabs(notches))); ++k) step(w, dir);
    } else if (w.kind == Kind::Knob || w.kind == Kind::Fader) {
        widgetSetNorm(w, widgetNorm(w) + notches * (fine ? 0.005f : 0.025f));
    }
    return true;
}

} // namespace hc::console::ui
