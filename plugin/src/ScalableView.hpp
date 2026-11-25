// A VST3 editor view whose faceplate keeps one shape at any size.
//
// Hosts size a plug-in's screen in two ways, and this view answers both:
//   1. Display scaling: the host says how big one logical pixel is (IPlugViewContentScaleSupport). The view
//      grows or shrinks to the same logical size at the new scale and tells the host its new pixel size.
//      JUCE hosts (HEM Studio Suite) also use this to fit a plug-in into a smaller pane.
//   2. Free resizing: the host drags the window (canResize / checkSizeConstraint / onSize). The view keeps the
//      faceplate's width : height and stays between a smallest and largest size.
// Either way the derived class is told the new pixel size in resized(); it redraws the same controls there.
#pragma once

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/common/pluginview.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace hc::vst3view {

class ScalableView : public Steinberg::CPluginView, public Steinberg::IPlugViewContentScaleSupport {
public:
    // baseW x baseH: the faceplate's pixel size at scale 1. startScale: the size it opens at on this computer
    // (already includes Windows display scaling).
    ScalableView(float baseW, float baseH, float startScale) : baseW_(baseW), baseH_(baseH) {
        const UINT dpi = ::GetDpiForSystem();
        content_ = dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
        logical_ = startScale / content_;
        scale_ = clampScale(startScale);
        const Steinberg::ViewRect r = rectFor(scale_);
        setRect(r);
    }

    OBJ_METHODS(ScalableView, CPluginView)
    DEFINE_INTERFACES
        DEF_INTERFACE(Steinberg::IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)

    // 1. display scaling
    Steinberg::tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) SMTG_OVERRIDE {
        if (!(factor > 0.0f) || !std::isfinite(factor)) return Steinberg::kInvalidArgument;
        content_ = factor;
        // never larger than the screen's work area (a host that reports a bigger factor, or a laptop screen)
        applyScale(std::min(logical_ * factor, screenFit()));
        return Steinberg::kResultOk;
    }

    // 2. free resizing
    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE { return Steinberg::kResultTrue; }
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* r) SMTG_OVERRIDE {
        if (!r) return Steinberg::kInvalidArgument;
        const Steinberg::ViewRect fit = rectFor(fitScale(r->getWidth(), r->getHeight()));
        r->right = r->left + fit.getWidth();
        r->bottom = r->top + fit.getHeight();
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* r) SMTG_OVERRIDE {
        if (!r) return Steinberg::kInvalidArgument;
        const int w = std::max(1, r->getWidth()), h = std::max(1, r->getHeight());
        rect = *r;
        scale_ = fitScale(w, h);
        logical_ = scale_ / content_;   // a dragged size is kept when the display scale changes later
        if (systemWindow) resized(w, h);
        return Steinberg::kResultTrue;
    }

    [[nodiscard]] float scale() const noexcept { return scale_; }

protected:
    // The editor's window must take this pixel size (only called while attached).
    virtual void resized(int width, int height) = 0;

    // Smallest and largest faceplate, as pixels per faceplate pixel.
    static constexpr float kMinScale = 0.12f, kMaxScale = 1.0f;
    // The largest scale whose window fits 90 % of the work area's width and 85 % of its height (the host's
    // title bar and buttons go around it): the same rule the faceplates open with.
    [[nodiscard]] float screenFit() const noexcept {
        RECT work{};
        if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left || work.bottom <= work.top) return kMaxScale;
        return std::max(kMinScale, std::min(0.9f * static_cast<float>(work.right - work.left) / baseW_,
                                            0.85f * static_cast<float>(work.bottom - work.top) / baseH_));
    }

private:
    [[nodiscard]] float clampScale(float s) const noexcept { return std::clamp(s, kMinScale, kMaxScale); }
    [[nodiscard]] float fitScale(int w, int h) const noexcept {
        return clampScale(std::min(static_cast<float>(w) / baseW_, static_cast<float>(h) / baseH_));
    }
    [[nodiscard]] Steinberg::ViewRect rectFor(float s) const noexcept {
        return Steinberg::ViewRect(0, 0, std::max(1, static_cast<int>(std::lround(baseW_ * s))),
                                   std::max(1, static_cast<int>(std::lround(baseH_ * s))));
    }
    void applyScale(float s) {
        scale_ = clampScale(s);
        Steinberg::ViewRect r = rectFor(scale_);
        if (r.getWidth() == rect.getWidth() && r.getHeight() == rect.getHeight()) return;
        rect = r;
        if (!systemWindow) return;   // not on screen yet: the host reads the size with getSize()
        resized(r.getWidth(), r.getHeight());
        if (plugFrame && !inResize_) {   // tell the host; it answers with onSize (same size: no loop)
            inResize_ = true;
            plugFrame->resizeView(this, &r);
            inResize_ = false;
        }
    }

    float baseW_, baseH_;
    float content_ = 1.0f;   // host's display scale (1 = 100 %)
    float logical_ = 0.45f;  // size at 100 % display scaling
    float scale_ = 0.45f;    // current size
    bool  inResize_ = false;
};

} // namespace hc::vst3view
