// HEM DIGITAL CONSOLE — GPU screen: plates, static engraving, live controls, meters.
#include "hybrid/console/ui/ConsoleScreen.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/TwinBus.hpp"
#include "hybrid/ui/Faceplate.hpp"

#include <algorithm>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace hc::console::ui {

using namespace hc::ui;
namespace C = colours;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kKnobMin = -0.75f * kPi, kKnobMax = 0.75f * kPi;
const Rgba kGlass = rgb8(0x050507, 0.88f);
const Rgba kSlot = rgb8(0x020203, 0.96f);

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string lufsText(double v) { return v <= -143.0 ? std::string("--") : fmt("%.1f", v); }

constexpr TextStyle kLadder[] = {TextStyle::Micro, TextStyle::Caption, TextStyle::Value, TextStyle::Label, TextStyle::Title, TextStyle::Tab,
                                 TextStyle::Panel,   TextStyle::Heading, TextStyle::Display};
// The largest style no larger than `base` that fits `width`.
TextStyle fitStyle(const Renderer& r, TextStyle base, std::string_view text, float width) {
    int i = 0;
    for (int k = 0; k < static_cast<int>(std::size(kLadder)); ++k) if (kLadder[k] == base) i = k;
    while (i > 0 && r.textWidth(kLadder[i], text) > width) --i;
    return kLadder[i];
}
std::string shorten(const Renderer& r, TextStyle st, std::string s, float width) {
    if (r.textWidth(st, s) <= width) return s;
    while (s.size() > 3 && r.textWidth(st, s + "...") > width) s.pop_back();
    return s + "...";
}
bool ascii(std::string_view s) { for (unsigned char c : s) if (c >= 128) return false; return true; }

// Plate pixel density buckets (re-routing a plate takes a moment, so small window changes reuse it).
float plateBucket(float scale) {
    for (float b : {0.30f, 0.40f, 0.50f, 0.60f, 0.75f, 0.90f}) if (scale <= b) return b;
    return std::min(1.0f, 4096.0f / kWidth);
}

// An arc at any angles (0 = 12 o'clock, clockwise, may pass +/-pi or span a full turn): the renderer's arcs live in
// -pi .. pi, so the span is cut where it crosses the bottom.
void arcCw(Renderer& r, float cx, float cy, float radius, float width, float a0, float a1, Rgba col, bool emissive) {
    if (a1 < a0) std::swap(a0, a1);
    if (a1 - a0 >= 2 * kPi - 1e-4f) { r.arc(cx, cy, radius, width, -kPi, kPi, col, emissive); return; }
    while (a0 >= kPi) { a0 -= 2 * kPi; a1 -= 2 * kPi; }
    while (a0 < -kPi) { a0 += 2 * kPi; a1 += 2 * kPi; }
    if (a1 <= kPi) { r.arc(cx, cy, radius, width, a0, a1, col, emissive); return; }
    r.arc(cx, cy, radius, width, a0, kPi, col, emissive);
    r.arc(cx, cy, radius, width, -kPi, a1 - 2 * kPi, col, emissive);
}

// Detented knobs big enough get their step names printed around them.
bool hasStepRing(const Widget& w) { return w.kind == Kind::Stepped && w.r.w >= 180 && w.ctl.valid() && w.ctl.info->choices; }
void drawStepRing(Renderer& r, const Widget& w) {
    // every step up to 11 steps; more: the ends, the middle and the round numbers (-6 -3 -1 +1 +3 +6 ...)
    const int n = static_cast<int>(w.ctl.info->max + 0.5f);
    for (int k = 0; k <= n; ++k) {
        std::string t = w.ctl.info->choices[k];
        for (const char* suf : {" dB", " Hz"}) if (t.size() > 3 && t.ends_with(suf)) t.resize(t.size() - 3);
        if (t.size() > 4 && t.ends_with(" kHz")) t = t.substr(0, t.size() - 4) + "k";
        if (n > 10 && k != 0 && k != n && k != n / 2) {
            static const char* const major[] = {"-6", "-3", "-1", "+1", "+3", "+6"};
            bool show = false;
            for (const char* m : major) if (t == m) show = true;
            if (!show) continue;
        }
        const float a = kKnobMin + (kKnobMax - kKnobMin) * static_cast<float>(k) / static_cast<float>(std::max(1, n));
        const float R = w.r.w * 0.5f * 1.40f;
        r.text(TextStyle::Caption, t, w.r.cx() + std::sin(a) * R, w.r.cy() - std::cos(a) * R + 9, C::dim, Align::Center, TextFx::Shadowed);
    }
}

} // namespace

// Per-frame display state (meter ballistics).
struct ConsoleScreen::ViewState {
    float needle[2] = {0, 0};
    std::array<float, MeterBridge::kBands> rta{};
    std::array<float, MeterBridge::kOctaves> oct{};
    float scopeGain = 1.0f;                                     // AUTO scope scale (smoothed)
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

ConsoleScreen::ConsoleScreen(ConsoleRig& rig, AppState& app, std::function<void(const ActionEvent&)> onAction)
    : rig_(rig), app_(app), onAction_(std::move(onAction)), controller_([this](const ActionEvent& e) { handle(e); }),
      view_(std::make_unique<ViewState>()) {
    pages_ = buildPages(rig_, app_);
    controller_.setPage(&pages_[0]);
}
ConsoleScreen::~ConsoleScreen() {
    for (Plate& p : plates_) if (p.job.valid()) p.job.wait();
}

bool ConsoleScreen::init(const std::filesystem::path& assetDir, int msaa, std::string* error) {
    auto fonts = FontSet::load(assetDir / "fonts", error);
    if (!fonts) return false;
    fonts_ = std::move(*fonts);
    auto art = loadFaceplateArt(assetDir / "faceplates" / kFaceplateFile, error);
    if (!art) return false;
    if (!renderer_.init(msaa, error)) return false;
    if (!renderer_.setParts(buildPartsAtlas(art->master), error)) return false;
    ready_ = true;
    return true;
}

void ConsoleScreen::shutdown() noexcept {
    renderer_.shutdown();
    ready_ = false;
}

bool ConsoleScreen::resize(int w, int h, std::string* error) {
    if (!ready_ || w <= 0 || h <= 0) return false;
    if (w != fbW_ || h != fbH_) {
        fbW_ = w; fbH_ = h;
        if (!renderer_.setTargetSize(w, h, error)) return false;
    }
    if (std::fabs(app_.zoom - zoomApplied_) > 1e-4f) {   // the ZOOM selector (or the app) changed it: keep the centre
        const float cx = haveCanvas_ ? canvas_.toDesignX(static_cast<float>(w) * 0.5f) : kWidth * 0.5f;
        const float cy = haveCanvas_ ? canvas_.toDesignY(static_cast<float>(h) * 0.5f) : kHeight * 0.5f;
        const float fit = std::min(static_cast<float>(w) / kWidth, static_cast<float>(h) / kHeight);
        const float maxZ = std::max(kZoomMin, std::min(kZoomMax, 1.0f / fit));
        zoomApplied_ = std::clamp(app_.zoom, kZoomMin, maxZ);
        app_.zoom = zoomApplied_;
        const float sc = fit * zoomApplied_;
        viewX_ = cx - static_cast<float>(w) * 0.5f / sc;
        viewY_ = cy - static_cast<float>(h) * 0.5f / sc;
    }
    layoutCanvas();
    if (std::fabs(fontScale_ - canvas_.scale) > 1e-4f) {
        if (!font_.bake(fonts_, canvas_.scale, kMinTextPx)) { if (error) *error = "could not bake the Poppins atlas"; return false; }
        if (!renderer_.setFont(font_, error)) return false;
        fontScale_ = canvas_.scale;
        staticDirty_ = true;
    }
    const float bucket = plateBucket(canvas_.scale);
    if (bucket != plateBucket_) { plateBucket_ = bucket; routePlates(); }
    return true;
}

// Canvas from the window size, the zoom and the view position (letterboxed and centred when everything fits).
void ConsoleScreen::layoutCanvas() {
    const float w = static_cast<float>(fbW_), h = static_cast<float>(fbH_);
    const float fit = std::min(w / kWidth, h / kHeight);
    const float maxZ = std::max(kZoomMin, std::min(kZoomMax, 1.0f / fit));
    zoomApplied_ = std::clamp(zoomApplied_, kZoomMin, maxZ);
    app_.zoom = zoomApplied_;
    Canvas c;
    c.scale = fit * zoomApplied_;
    if (kWidth * c.scale <= w + 0.5f) { viewX_ = 0; c.offsetX = std::round((w - kWidth * c.scale) * 0.5f); }
    else { viewX_ = std::clamp(viewX_, 0.0f, kWidth - w / c.scale); c.offsetX = std::round(-viewX_ * c.scale); }
    if (kHeight * c.scale <= h + 0.5f) { viewY_ = 0; c.offsetY = std::round((h - kHeight * c.scale) * 0.5f); }
    else { viewY_ = std::clamp(viewY_, 0.0f, kHeight - h / c.scale); c.offsetY = std::round(-viewY_ * c.scale); }
    c.pixelW = w;
    c.pixelH = h;
    if (haveCanvas_ && c == canvas_) return;
    canvas_ = c;
    haveCanvas_ = true;
    staticDirty_ = true;
}

void ConsoleScreen::setZoom(float z, float ax, float ay) {
    if (fbW_ <= 0 || !haveCanvas_) { app_.zoom = z; return; }
    const float dx = canvas_.toDesignX(ax), dy = canvas_.toDesignY(ay);
    const float fit = std::min(static_cast<float>(fbW_) / kWidth, static_cast<float>(fbH_) / kHeight);
    const float maxZ = std::max(kZoomMin, std::min(kZoomMax, 1.0f / fit));
    zoomApplied_ = std::clamp(z, kZoomMin, maxZ);
    app_.zoom = zoomApplied_;
    const float sc = fit * zoomApplied_;
    viewX_ = dx - ax / sc;
    viewY_ = dy - ay / sc;
    std::string err;
    (void)resize(fbW_, fbH_, &err);
}

void ConsoleScreen::scrollBy(float dxPx, float dyPx) {
    if (!haveCanvas_) return;
    viewX_ += dxPx / canvas_.scale;
    viewY_ += dyPx / canvas_.scale;
    layoutCanvas();
}

void ConsoleScreen::routePlates() {
    // Every page's obsidian plate, routed on background threads (the current page first).
    for (int k = 0; k < kPageCount; ++k) {
        const int i = (static_cast<int>(page_) + k) % kPageCount;
        Plate& p = plates_[static_cast<std::size_t>(i)];
        if (p.job.valid()) p.job.wait();
        p.tex.reset();
        p.ppu = plateBucket_;
        hc::ui::PanelLayout L = platePockets(pages_[static_cast<std::size_t>(i)]);
        const float ppu = plateBucket_;
        p.job = std::async(std::launch::async, [L = std::move(L), ppu] { return buildMips(premultiply(renderFxPlate(L, FxPalette{}, ppu))); });
    }
    uploadedPage_ = -1;
}

bool ConsoleScreen::ensurePlate() {
    Plate& p = plates_[static_cast<std::size_t>(page_)];
    if (!p.tex && p.job.valid() && p.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) p.tex = p.job.get();
    if (!p.tex) return false;
    if (uploadedPage_ != static_cast<int>(page_)) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        if (!renderer_.setPanel(*p.tex, &err)) return false;
        plateMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        uploadedPage_ = static_cast<int>(page_);
        staticDirty_ = true;
    }
    return true;
}

bool ConsoleScreen::waitForPlate(double seconds) {
    Plate& p = plates_[static_cast<std::size_t>(page_)];
    if (!p.tex && p.job.valid()) {
        if (p.job.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) return false;
    }
    return ensurePlate();
}

void ConsoleScreen::setPage(PageId p) {
    if (p == page_) return;
    page_ = p;
    controller_.setPage(&pages_[static_cast<std::size_t>(p)]);
    staticDirty_ = true;
}

void ConsoleScreen::handle(const ActionEvent& e) {
    if (e.act == Act::Page) setPage(static_cast<PageId>(std::clamp(e.arg, 0, kPageCount - 1)));
    if (e.act == Act::MeterTab) app_.meterTab = std::clamp(e.arg, 0, 4);
    if (e.act == Act::MeterAuto) rig_.meter().rtaAutoScale();
    if (e.act == Act::BigNav) app_.bigFirst = e.arg >= 100 ? bigDeskFirst(app_.bigFirst, 0, e.arg - 100) : bigDeskFirst(app_.bigFirst, e.arg);
    if (e.act == Act::EditTool) {
        app_.toolTarget = e.arg;
        setPage(PageId::Tools);
    }
    if (onAction_) onAction_(e);
}

void ConsoleScreen::mouseDown(float px, float py, bool dbl, bool fine) {
    const bool zoomed = canvas_.scale * kWidth > static_cast<float>(fbW_) + 0.5f || canvas_.scale * kHeight > static_cast<float>(fbH_) + 0.5f;
    if (zoomed && controller_.hit(toX(px), toY(py)) < 0) {   // bare plate: grab it and move the view
        panning_ = leftPan_ = true;
        panLastX_ = px; panLastY_ = py;
        return;
    }
    controller_.mouseDown(toX(px), toY(py), dbl, fine);
}
void ConsoleScreen::rightDown(float px, float py) { controller_.rightDown(toX(px), toY(py)); }
void ConsoleScreen::mouseMove(float px, float py, bool fine) {
    if (panning_) {
        scrollBy(panLastX_ - px, panLastY_ - py);
        panLastX_ = px; panLastY_ = py;
        return;
    }
    controller_.mouseMove(toX(px), toY(py), fine);
}
void ConsoleScreen::mouseUp(float px, float py) {
    if (panning_ && leftPan_) { panning_ = leftPan_ = false; return; }
    controller_.mouseUp(toX(px), toY(py));
}
void ConsoleScreen::middleDown(float px, float py) { panning_ = true; leftPan_ = false; panLastX_ = px; panLastY_ = py; }
void ConsoleScreen::middleUp(float, float) { if (!leftPan_) panning_ = false; }
void ConsoleScreen::wheel(float px, float py, float n, bool fine, bool ctrl, bool shift, float nx) {
    if (ctrl && n != 0.0f) { setZoom(zoomApplied_ * std::pow(1.1f, n), px, py); return; }
    const float stepPx = 0.12f * static_cast<float>(std::max(fbW_, fbH_));
    if (nx != 0.0f) { scrollBy(nx * stepPx, 0); return; }
    if (n == 0.0f) return;
    if (controller_.wheel(toX(px), toY(py), n, fine)) return;
    const bool zoomed = canvas_.scale * kWidth > static_cast<float>(fbW_) + 0.5f || canvas_.scale * kHeight > static_cast<float>(fbH_) + 0.5f;
    if (zoomed) {
        if (shift) scrollBy(-n * stepPx, 0);
        else scrollBy(0, -n * stepPx);
    } else if (page_ == PageId::DeskBig) {
        app_.bigFirst = bigDeskFirst(app_.bigFirst, n > 0 ? -1 : 1);   // the wheel on bare plate moves along the desk
    }
}
bool ConsoleScreen::key(std::uint32_t vk, bool ctrl) {
    constexpr std::uint32_t kLeft = 0x25, kRight = 0x27, kPlus = 0xBB, kMinus = 0xBD, kAdd = 0x6B, kSub = 0x6D, kNum0 = 0x60;
    const float cx = static_cast<float>(fbW_) * 0.5f, cy = static_cast<float>(fbH_) * 0.5f;
    if (ctrl) {
        if (vk == kPlus || vk == kAdd) { setZoom(zoomApplied_ * 1.25f, cx, cy); return true; }
        if (vk == kMinus || vk == kSub) { setZoom(zoomApplied_ / 1.25f, cx, cy); return true; }
        if (vk == '0' || vk == kNum0) { setZoom(1.0f, cx, cy); return true; }
        return false;
    }
    if (page_ == PageId::DeskBig && (vk == kLeft || vk == kRight)) {
        app_.bigFirst = bigDeskFirst(app_.bigFirst, vk == kLeft ? -1 : 1);
        return true;
    }
    return false;
}

// =====================================================================================================
//  STATIC LAYER: plate, section titles, engraving, captions
// =====================================================================================================
void ConsoleScreen::drawStatic() {
    Renderer& r = renderer_;
    const Page& pg = currentPage();
    r.beginStatic(canvas_);
    // letterbox: the obsidian's own black, so nothing flat shows around a 16:10 window
    r.fillRound({canvas_.visibleLeft() - 10, -2000, canvas_.visibleRight() - canvas_.visibleLeft() + 20, kHeight + 4000}, 0, rgb8(0x050507), Blend::Over);
    if (uploadedPage_ == static_cast<int>(page_)) r.panel({0, 0, kWidth, kHeight});
    else r.fillRound({0, 0, kWidth, kHeight}, 30, rgb8(0x0B0B0E), Blend::Over);

    for (const Section& s : pg.sections)
        if (!s.title.empty()) r.text(TextStyle::Label, s.title, s.r.x + 22, s.r.y + 44, s.colour, Align::Left, TextFx::Shadowed);
    for (const Engraving& e : pg.engravings) {
        const TextStyle st = e.small ? TextStyle::Caption : (e.colour.r == C::text.r && e.colour.g == C::text.g ? TextStyle::Heading : TextStyle::Panel);
        const TextStyle use = (!e.small && e.text.size() <= 3) ? TextStyle::Title : st;
        r.text(use, e.text, e.x, e.y, e.colour, e.align == 0 ? Align::Left : e.align == 2 ? Align::Right : Align::Center, TextFx::Shadowed);
    }
    // crown (the HEM mark) in the top bar
    {
        const Rect a{60, 30, 120, 110};
        const float x0 = a.x + a.w * 0.12f, x1 = a.right() - a.w * 0.12f, base = a.y + a.h * 0.72f, top = a.y + a.h * 0.26f, mid = a.y + a.h * 0.50f, w = x1 - x0;
        const float xs[] = {x0, x0, x0 + w * 0.25f, x0 + w * 0.5f, x0 + w * 0.75f, x1, x1, x0};
        const float ys[] = {base, top, mid, top - a.h * 0.06f, mid, top, base, base};
        r.polyline(xs, ys, 8, 7.0f, C::lime.withAlpha(0.95f), Blend::Over, false);
        r.fillRound({x0, base + 8, w, 7}, 3.5f, C::violet, Blend::Over, false);
    }
    // captions of the controls
    for (const Widget& w : pg.widgets) {
        if (w.label.empty() || w.visible) continue;   // hidden-able widgets (TOOLS page) caption themselves live
        if (w.kind == Kind::Knob || w.kind == Kind::Stepped) {
            const TextStyle st = w.r.w < 100 ? TextStyle::Caption : w.r.w < 170 ? TextStyle::Value : TextStyle::Label;
            const bool ring = hasStepRing(w);
            r.text(st, w.label, w.r.cx(), w.r.y - (ring ? 84.0f : w.r.w < 100 ? 10.0f : 18.0f), C::text, Align::Center, TextFx::Shadowed);
            if (ring) drawStepRing(r, w);
        } else if (w.kind == Kind::Lamp) {
            r.text(TextStyle::Caption, w.label, w.r.right() + 16, w.r.cy() + r.capHeight(TextStyle::Caption) * 0.5f, C::text, Align::Left, TextFx::Shadowed);
        } else if (w.kind == Kind::Selector && w.r.w < 300) {
            r.text(TextStyle::Caption, w.label, w.r.cx(), w.r.y - 10, C::dim, Align::Center, TextFx::Shadowed);
        } else if (w.kind == Kind::LedBar) {
            r.text(TextStyle::Caption, w.label, w.vertical ? w.r.cx() : w.r.x - 14, w.vertical ? w.r.bottom() + 36 : w.r.cy() + 9, C::text,
                   w.vertical ? Align::Center : Align::Right, TextFx::Shadowed);
        }
    }
    r.endStatic();
    staticDirty_ = false;
}

// =====================================================================================================
//  LIVE PARTS
// =====================================================================================================
namespace {

struct Draw {
    Renderer& r;
    void knob(const Widget& w, bool hot, bool stepped) {
        const float R = w.r.w * 0.5f, cx = w.r.cx(), cy = w.r.cy();
        const float n = std::clamp(widgetNorm(w), 0.0f, 1.0f);
        const float ang = kKnobMin + (kKnobMax - kKnobMin) * n;
        const bool on = widgetEnabled(w);
        const float a0 = kKnobMin + (kKnobMax - kKnobMin) * (w.bipolar ? 0.5f : 0.0f);
        const float ar = knob::kArcRadius * R, aw = knob::kArcWidth * R * 1.15f;
        r.sprite(Sprite::KnobTrack, {cx - R, cy - R, 2 * R, 2 * R});
        r.arc(cx, cy, ar, aw, kKnobMin, kKnobMax, w.colour.withAlpha(0.14f), false);
        if (stepped && w.ctl.valid()) {   // a dot at every detent
            const int nSteps = static_cast<int>(w.ctl.info->max - w.ctl.info->min + 0.5f);
            const float dot = std::max(5.0f, aw * 0.9f);
            for (int k = 0; k <= nSteps && nSteps > 0; ++k) {
                const float a = kKnobMin + (kKnobMax - kKnobMin) * static_cast<float>(k) / static_cast<float>(nSteps);
                const float dx = cx + std::sin(a) * R * 1.02f, dy = cy - std::cos(a) * R * 1.02f;
                r.fillRound({dx - dot * 0.5f, dy - dot * 0.5f, dot, dot}, dot * 0.5f, C::text.withAlpha(0.35f), Blend::Over);
            }
        }
        // lit arc only while the control is working (a switched-off section keeps its knobs, just unlit)
        if (on && std::fabs(ang - a0) > 0.004f) {
            r.arc(cx, cy, ar, aw, std::min(a0, ang), std::max(a0, ang), w.colour, true);
            const float tx = cx + std::sin(ang) * ar, ty = cy - std::cos(ang) * ar;
            r.sprite(Sprite::GlowDot, {tx - aw * 2.4f, ty - aw * 2.4f, aw * 4.8f, aw * 4.8f}, w.colour.withAlpha(0.95f), Blend::Add, true);
        }
        const float bodyR = knob::kBody * R;
        r.sprite(Sprite::KnobShadow, {cx - bodyR * 1.14f + 0.06f * bodyR, cy - bodyR * 1.14f + 0.10f * bodyR, bodyR * 2.28f, bodyR * 2.28f});
        const float bd = bodyR * 2.0f * 256.0f / 254.0f;
        r.spriteRotated(Sprite::KnobBody, cx, cy, bd, bd, ang);
        const float len = (knob::kIndicatorTo - knob::kIndicatorFrom) * bodyR;
        const float mid = 0.5f * (knob::kIndicatorTo + knob::kIndicatorFrom) * bodyR;
        const float ih = len * 96.0f / 72.0f, iw = ih * 24.0f / 96.0f;
        const Rgba ind = on ? Rgba{0.55f + 0.45f * w.colour.r, 0.55f + 0.45f * w.colour.g, 0.55f + 0.45f * w.colour.b, 1.0f} : Rgba{0.75f, 0.75f, 0.78f, 1};
        r.spriteRotated(Sprite::KnobIndicator, cx + std::sin(ang) * mid, cy - std::cos(ang) * mid, iw, ih, ang, ind, Blend::Over, on);
        r.sprite(Sprite::KnobLight, {cx - bd / 2, cy - bd / 2, bd, bd});
        if (hot) r.glow(w.r.inset(w.r.w * 0.1f), w.r.w * 0.42f, 18.0f, C::violet.withAlpha(0.35f), true);
    }

    void cap(Rect b, const std::string& text, bool lit, bool hot, bool pressed, Rgba colour, bool dim) {
        const float maxW = b.w - 12;
        TextStyle base = b.h >= 70 ? TextStyle::Label : b.h >= 44 ? TextStyle::Value : TextStyle::Caption;
        if (!ascii(text) && base == TextStyle::Caption) base = TextStyle::Value;   // Ø needs a Latin-1 face
        TextStyle st = fitStyle(r, base, text, maxW);
        if (!ascii(text) && st == TextStyle::Caption) st = TextStyle::Value;
        const std::string shown = shorten(r, st, text, maxW);
        const Rect in = pressed ? b.offset(1.5f, 1.5f) : b;
        if (lit) {
            r.glow(in.inset(4), 10, 12.0f, colour.withAlpha(0.40f), true);
            r.fillRound(in.inset(2), 10, colour.withAlpha(0.92f), Blend::Over, true);
            r.fillRound({in.x + 8, in.y + 5, in.w - 16, in.h * 0.35f}, 8, {1, 1, 1, 0.16f}, Blend::Add);
            r.text(st, shown, in.cx(), in.cy() + r.capHeight(st) * 0.5f, rgb8(0x0A0812), Align::Center, TextFx::Plain);
        } else {
            r.fillRound(in.inset(2), 10, kGlass, Blend::Over);
            r.fillRound({in.x + 4, in.y + 3, in.w - 8, 3}, 1.5f, colour.withAlpha(hot ? 0.85f : 0.40f), Blend::Over, true);
            r.text(st, shown, in.cx(), in.cy() + r.capHeight(st) * 0.5f, dim ? C::dim : hot ? C::text : rgb8(0xC8C4D8), Align::Center, TextFx::Shadowed);
        }
    }

    void fader(const Widget& w, bool hot) {
        const Rect t = w.r;
        r.fillRound(t, t.w * 0.5f, kSlot, Blend::Over);
        r.fillRound({t.cx() - 1.5f, t.y + 8, 3, t.h - 16}, 1.5f, rgb8(0x2A2833), Blend::Over);
        const float cy = t.y + t.h * (1.0f - widgetNorm(w));
        const float capW = std::max(72.0f, t.w * 2.9f), capH = capW * 0.52f;
        const float cw = capW * 256.0f / 248.0f, ch = capH * 136.0f / 124.0f;
        r.sprite(Sprite::FaderShadow, {t.cx() - cw * 0.62f + cw * 0.06f, cy - ch * 0.75f + ch * 0.16f, cw * 1.24f, ch * 1.5f}, {1, 1, 1, 0.95f});
        r.sprite(Sprite::FaderCap, {t.cx() - cw / 2, cy - ch / 2, cw, ch});
        r.sprite(Sprite::FaderLine, {t.cx() - capW * 0.43f, cy - capH * 0.085f, capW * 0.86f, capH * 0.17f}, hot ? C::violet : C::lime, Blend::Over, true);
    }

    void leds(const Widget& w) {
        const int n = std::max(1, w.count);
        const int lit = w.value ? static_cast<int>(std::lround(w.value())) : 0;
        const float pitch = (w.vertical ? w.r.h : w.r.w) / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const Rgba c = i < static_cast<int>(w.ledColours.size()) ? w.ledColours[static_cast<std::size_t>(i)] : C::green;
            const Rect seg = w.vertical ? Rect{w.r.x, w.r.bottom() - (static_cast<float>(i) + 1) * pitch + pitch * 0.14f, w.r.w, pitch * 0.72f}
                                        : Rect{w.r.x + static_cast<float>(i) * pitch + pitch * 0.12f, w.r.y, pitch * 0.76f, w.r.h};
            if (i < lit) r.sprite(Sprite::LedSegment, seg, c, Blend::Over, true);
            else r.sprite(Sprite::LedSegment, seg, c.withAlpha(0.10f));
        }
    }

    void ledBar(const Widget& w) {
        const float v = w.value ? w.value() : w.min;
        const float hold = w.value2 ? w.value2() : w.min - 1;
        r.fillRound(w.r, 8, kSlot, Blend::Over);
        const float span = w.max - w.min;
        const bool reduction = w.max <= 0.0f;   // gain reduction: hangs from the top / right
        const int n = std::max(8, static_cast<int>((w.vertical ? w.r.h : w.r.w) / 22.0f));
        const Rect in = w.r.inset(6);
        const float pitch = (w.vertical ? in.h : in.w) / static_cast<float>(n);
        const float f = std::clamp((v - w.min) / span, 0.0f, 1.0f);
        const float hf = std::clamp((hold - w.min) / span, 0.0f, 1.0f);
        for (int i = 0; i < n; ++i) {
            const float segNorm = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
            const float db = w.min + segNorm * span;
            Rgba c = reduction ? C::amber : db > w.max - span * 0.12f ? C::red : db > w.max - span * 0.30f ? C::amber : C::green;
            const Rect seg = w.vertical ? Rect{in.x, in.bottom() - (static_cast<float>(i) + 1) * pitch + pitch * 0.12f, in.w, pitch * 0.76f}
                                        : Rect{in.x + static_cast<float>(i) * pitch + pitch * 0.1f, in.y, pitch * 0.8f, in.h};
            const bool on = reduction ? segNorm >= f : segNorm <= f;
            const bool holdSeg = w.value2 && hold > w.min && std::fabs(segNorm - hf) < 0.5f / static_cast<float>(n);
            if (on || holdSeg) r.sprite(Sprite::LedSegment, seg, c, Blend::Over, true);
            else r.sprite(Sprite::LedSegment, seg, c.withAlpha(0.08f));
        }
    }

    void lamp(const Widget& w) {
        const float v = w.value ? w.value() : 0.0f;
        Rgba c = w.colour;
        bool lit = v > 0.5f;
        if (!w.ledColours.empty()) {   // value = which colour (0 = off)
            const int k = static_cast<int>(std::lround(v));
            lit = k > 0;
            if (lit) c = w.ledColours[static_cast<std::size_t>(std::min<int>(k, static_cast<int>(w.ledColours.size())) - 1)];
        }
        if (lit) {
            r.sprite(Sprite::GlowDot, w.r.inset(-w.r.w * 0.8f), c.withAlpha(0.8f), Blend::Add, true);
            r.sprite(Sprite::Lamp, w.r, c, Blend::Over, true);
        } else {
            r.sprite(Sprite::Lamp, w.r, c.withAlpha(0.18f));
        }
    }

    void glassText(Rect b, const std::string& s, Rgba col, TextStyle base, bool neon = true) {
        r.fillRound(b.inset(2), 12, kGlass, Blend::Over);
        const TextStyle st = fitStyle(r, base, s, b.w - 24);
        const std::string t = shorten(r, st, s, b.w - 24);
        r.text(st, t, b.cx(), b.cy() + r.capHeight(st) * 0.5f, col, Align::Center, neon ? TextFx::NeonCrisp : TextFx::Shadowed);
    }
};

} // namespace

void ConsoleScreen::drawFrame() {
    Renderer& r = renderer_;
    Draw d{r};
    const Page& pg = currentPage();
    const int hover = controller_.hover(), active = controller_.active();
    MeterBridge& mb = rig_.meter();

    // neon tubes
    for (const auto& n : pg.neon) {
        const float xs[] = {n[0], n[2]}, ys[] = {n[1], n[3]};
        r.polyline(xs, ys, 2, 9.0f, C::violet.withAlpha(0.35f), Blend::Add, true);
        r.polyline(xs, ys, 2, 3.0f, C::violet, Blend::Over, true);
    }

    for (std::size_t i = 0; i < pg.widgets.size(); ++i) {
        const Widget& w = pg.widgets[i];
        if (!widgetVisible(w)) continue;
        const bool hot = static_cast<int>(i) == hover || static_cast<int>(i) == active;
        const bool pressed = static_cast<int>(i) == active;
        // live captions for widgets that come and go (TOOLS page)
        if (w.visible && !w.label.empty() && (w.kind == Kind::Knob || w.kind == Kind::Stepped)) {
            const bool ring = hasStepRing(w);
            r.text(w.r.w < 170 ? TextStyle::Value : TextStyle::Label, w.label, w.r.cx(), w.r.y - (ring ? 84.0f : 18.0f), C::text, Align::Center, TextFx::Shadowed);
            if (ring) drawStepRing(r, w);
        }
        if (w.visible && !w.label.empty() && w.kind == Kind::Selector && w.r.w < 300)
            r.text(TextStyle::Caption, w.label, w.r.cx(), w.r.y - 10, C::dim, Align::Center, TextFx::Shadowed);
        if (w.visible && !w.label.empty() && w.kind == Kind::LedBar)
            r.text(TextStyle::Caption, w.label, w.r.x - 14, w.r.cy() + 9, C::text, Align::Right, TextFx::Shadowed);
        if (w.visible && (w.kind == Kind::Knob || w.kind == Kind::Stepped)) {   // no routed pocket under it: a live well instead
            const float wd = w.r.w * 1.16f;
            r.fillRound({w.r.cx() - wd / 2, w.r.cy() - wd / 2, wd, wd}, wd / 2, rgb8(0x020203, 0.85f), Blend::Over);
        }
        switch (w.kind) {
        case Kind::Knob: d.knob(w, hot, false); break;
        case Kind::Stepped: d.knob(w, hot, true); break;
        case Kind::Switch: d.cap(w.r, w.label, w.ctl.valid() && w.ctl.get() > 0.5f, hot, pressed, w.colour, !widgetEnabled(w)); break;
        case Kind::Button: {
            const bool lit = (w.act == Act::Page && w.arg == static_cast<int>(page_)) || (w.value && w.value() > 0.5f);
            d.cap(w.r, w.text ? w.text() : w.label, lit, hot, pressed, w.colour, false);
            break;
        }
        case Kind::Fader: d.fader(w, hot); break;
        case Kind::Selector: {
            const std::string v = w.text ? w.text() : widgetValueText(w);
            const std::string s = w.r.w >= 300 && !w.label.empty() && !w.text ? w.label + "   " + v : v;
            r.fillRound(w.r.inset(2), 12, kGlass, Blend::Over);
            if (hot) r.fillRound({w.r.x + 4, w.r.y + 3, w.r.w - 8, 3}, 1.5f, C::violet.withAlpha(0.85f), Blend::Over, true);
            const bool arrows = w.r.w >= 160;   // small windows (desk TOOL) give the room to the text
            const float room = w.r.w - (arrows ? 40.0f : 14.0f);
            const TextStyle st = fitStyle(r, w.r.h >= 70 ? TextStyle::Title : w.r.h >= 56 ? TextStyle::Label : TextStyle::Value, s, room);
            const bool on = widgetEnabled(w);
            r.text(st, shorten(r, st, s, room), w.r.cx(), w.r.cy() + r.capHeight(st) * 0.5f, on ? C::lime : C::dim, Align::Center, on ? TextFx::NeonCrisp : TextFx::Shadowed);
            if (arrows) {
                r.text(TextStyle::Caption, "<", w.r.x + 12, w.r.cy() + 8, C::dim, Align::Left, TextFx::Shadowed);
                r.text(TextStyle::Caption, ">", w.r.right() - 12, w.r.cy() + 8, C::dim, Align::Right, TextFx::Shadowed);
            }
            break;
        }
        case Kind::Leds: d.leds(w); break;
        case Kind::LedBar: d.ledBar(w); break;
        case Kind::Lamp: d.lamp(w); break;
        case Kind::Readout: d.glassText(w.r, w.text ? w.text() : std::string(), w.colour, w.r.h >= 80 ? TextStyle::Title : TextStyle::Value); break;
        case Kind::Seek: {
            r.fillRound(w.r, 10, kSlot, Blend::Over);
            const float f = std::clamp(w.value ? w.value() : 0.0f, 0.0f, 1.0f);
            const Rect in = w.r.inset(8, 18);
            r.fillRound({in.x, in.y, in.w, in.h}, 6, C::lime.withAlpha(0.10f), Blend::Over);
            if (f > 0) r.fillRound({in.x, in.y, in.w * f, in.h}, 6, C::lime.withAlpha(0.55f), Blend::Over, true);
            r.fillRound({in.x + in.w * f - 3, w.r.y + 8, 6, w.r.h - 16}, 3, C::text, Blend::Over, true);
            break;
        }
        case Kind::Custom: {
            switch (w.custom) {
            case CustomId::StemName: {
                const int slot = widgetArg(w);
                const auto& st = app_.stems[static_cast<std::size_t>(std::clamp(slot, 0, kStemSlots - 1))];
                r.fillRound(w.r.inset(2), 10, kGlass, Blend::Over);
                if (hot) r.fillRound({w.r.x + 4, w.r.y + 3, w.r.w - 8, 3}, 1.5f, C::violet.withAlpha(0.85f), Blend::Over, true);
                const float lh = w.r.h * 0.5f;
                if (st.progress >= 0.0f) {
                    r.text(TextStyle::Caption, "LOADING", w.r.cx(), w.r.y + lh * 0.75f + 8, C::amber, Align::Center, TextFx::Shadowed);
                    const Rect pb{w.r.x + 12, w.r.y + lh + 8, (w.r.w - 24) * std::clamp(st.progress, 0.0f, 1.0f), 10};
                    r.fillRound(pb, 5, C::amber, Blend::Over, true);
                } else if (st.name.empty()) {
                    const char* t = slot == kExt1Slot ? "EXT 1: LOAD" : slot == kExt2Slot ? "EXT 2: LOAD" : "LOAD";
                    r.text(TextStyle::Caption, shorten(r, TextStyle::Caption, st.error.empty() ? t : "CAN'T LOAD", w.r.w - 16), w.r.cx(), w.r.cy() + 9, st.error.empty() ? C::dim : C::red, Align::Center, TextFx::Shadowed);
                } else {
                    const std::string n = shorten(r, TextStyle::Caption, st.name, w.r.w - 16);
                    r.text(TextStyle::Caption, n, w.r.cx(), w.r.y + lh * 0.5f + 12, C::text, Align::Center, TextFx::Shadowed);
                    r.text(TextStyle::Caption, shorten(r, TextStyle::Caption, st.info, w.r.w - 16), w.r.cx(), w.r.y + lh * 1.5f + 6, C::dim, Align::Center, TextFx::Shadowed);
                }
                break;
            }
            case CustomId::DeskLoud: {
                r.fillRound(w.r.inset(4), 18, kGlass, Blend::Over);
                const char* caps[] = {"MOMENTARY", "SHORT-TERM", "INTEGRATED", "RANGE", "TRUE PEAK", "PLR"};
                const double tp = std::max(mb.truePeakDbtp(0), mb.truePeakDbtp(1));
                const std::string vals[] = {lufsText(mb.momentaryLufs()), lufsText(mb.shortTermLufs()), lufsText(mb.integratedLufs()),
                                            mb.integratedLufs() <= -143 ? "--" : fmt("%.1f", mb.loudnessRangeLu()), lufsText(tp),
                                            mb.plrDb() <= -143 ? "--" : fmt("%.1f", mb.plrDb())};
                const char* units[] = {"LUFS", "LUFS", "LUFS", "LU", "dBTP", "dB"};
                const float cw = (w.r.w - 40) / 3.0f, ch = (w.r.h - 30) / 2.0f;
                for (int k = 0; k < 6; ++k) {
                    const float x = w.r.x + 20 + cw * static_cast<float>(k % 3), y = w.r.y + 15 + ch * static_cast<float>(k / 3);
                    r.text(TextStyle::Caption, caps[k], x + 20, y + 40, C::dim, Align::Left, TextFx::Shadowed);
                    const Rgba col = k == 4 && tp > -1.0 ? C::red : C::lime;
                    r.text(TextStyle::Title, vals[k], x + 20, y + ch - 24, col, Align::Left, TextFx::NeonCrisp);
                    r.text(TextStyle::Caption, units[k], x + cw - 30, y + ch - 24, C::dim, Align::Right, TextFx::Shadowed);
                }
                break;
            }
            case CustomId::LoudNumbers: {   // the loudness descriptors (SCREEN LAYOUT), with the source line on top
                namespace M = meter_ids;
                r.fillRound(w.r.inset(4), 18, kGlass, Blend::Over);
                const int unit = static_cast<int>(mb.get(M::Unit) + 0.5f);
                const double target = mb.targetLufs();
                const char* lu = unit == 1 ? "LKFS" : unit == 2 ? "LU" : "LUFS";
                auto loud = [&](double v) { return v <= -143.0 ? std::string("--") : fmt("%.1f", unit == 2 ? v - target : v); };
                r.fillRound({w.r.x + 30, w.r.y + 30, 26, 26}, 4, C::green, Blend::Over, true);
                r.text(TextStyle::Label, "HEM MAIN OUT", w.r.x + 72, w.r.y + 55, C::text, Align::Left, TextFx::Shadowed);
                const auto ds = mb.descriptors();
                const float top = w.r.y + 90, slotH = (w.r.h - 110) / 6.0f;
                for (int k = 0; k < 6; ++k) {
                    const int dsc = ds[static_cast<std::size_t>(k)];
                    const float y = top + slotH * static_cast<float>(k);
                    if (dsc == M::DescNone) continue;
                    if (dsc == M::DescBalance) {   // BALANCE-O-METER: two slots, a triangle in a half circle
                        const float h = slotH * (k < 5 ? 2.0f : 1.0f), cx = w.r.cx(), base = y + h - 30, R = std::min(h - 60, w.r.w * 0.42f);
                        r.arc(cx, base, R, 3.0f, -0.5f * kPi, 0.5f * kPi, C::text.withAlpha(0.35f), false);
                        const float tilt = static_cast<float>(std::clamp(mb.balanceLrLu() / 6.0, -1.0, 1.0)) * 0.45f * kPi;   // left louder: top leans left
                        const double ms = mb.balanceMsDb();
                        const float width = static_cast<float>(std::clamp((ms + 30.0) / 30.0, 0.02, 1.0));   // side vs mid: base width
                        const float tx = cx - std::sin(tilt) * R * 0.92f, ty = base - std::cos(tilt) * R * 0.92f;
                        const float xs[] = {tx, cx - R * width, cx + R * width, tx}, ys[] = {ty, base, base, ty};
                        r.polyline(xs, ys, 4, 4.0f, C::lime, Blend::Over, true);
                        r.text(TextStyle::Caption, "L", cx - R - 20, base, C::dim, Align::Right, TextFx::Shadowed);
                        r.text(TextStyle::Caption, "R", cx + R + 20, base, C::dim, Align::Left, TextFx::Shadowed);
                        r.text(TextStyle::Caption, "MID / SIDE", cx, base + 26, C::dim, Align::Center, TextFx::Shadowed);
                        ++k;
                        continue;
                    }
                    std::string cap = M::DescNone < dsc && dsc < M::kDescCount ? MeterBridge::params()[M::Slot1].choices[dsc] : "";
                    std::string val, un;
                    bool warn = false;
                    switch (dsc) {
                    case M::DescProgram: val = loud(mb.integratedLufs()); un = lu; break;
                    case M::DescTruePeakMax: { const double tp = std::max(mb.truePeakDbtp(0), mb.truePeakDbtp(1)); val = lufsText(tp); un = "dBTP"; warn = mb.peakAlert(); break; }
                    case M::DescLoudMax: val = loud(mb.get(M::LoudMaxMetric) < 0.5f ? mb.maxMomentaryLufs() : mb.maxShortTermLufs()); un = lu;
                                         cap += mb.get(M::LoudMaxMetric) < 0.5f ? " (M)" : " (S)"; break;
                    case M::DescRange: val = mb.integratedLufs() <= -143 ? "--" : fmt("%.1f", mb.loudnessRangeLu()); un = "LU"; break;
                    case M::DescPlr: val = mb.plrDb() <= -143 ? "--" : fmt("%.1f", mb.plrDb()); un = "dB"; break;
                    case M::DescSliding10: val = loud(mb.sliding10Lufs()); un = lu; break;
                    case M::DescShortTerm: val = loud(mb.shortTermLufs()); un = lu; break;
                    case M::DescMomentary: val = loud(mb.momentaryLufs()); un = lu; warn = mb.loudAlert(); break;
                    case M::DescCorrelation: val = fmt("%+.2f", mb.correlation()); break;
                    case M::DescMonoDev: { const double md = mb.monoDeviationLu(); val = md < -100 ? "CANCELS" : fmt("%+.1f", md); un = "LU"; break; }
                    default: break;
                    }
                    r.text(TextStyle::Label, cap, w.r.x + 40, y + 48, C::text, Align::Left, TextFx::Shadowed);
                    r.text(TextStyle::Display, val, w.r.right() - 170, y + slotH - 34, warn ? C::red : C::amber, Align::Right, TextFx::NeonCrisp);
                    r.text(TextStyle::Label, un, w.r.right() - 40, y + slotH - 40, C::dim, Align::Right, TextFx::Shadowed);
                }
                break;
            }
            case CustomId::Radar: {
                namespace M = meter_ids;
                const float cx = w.r.cx(), cy = w.r.cy(), R = std::min(w.r.w, w.r.h) * 0.47f;
                r.fillRound(w.r.inset(4), 18, kGlass, Blend::Over);
                const double target = mb.targetLufs(), res = mb.radarResolutionDb();
                const int unit = static_cast<int>(mb.get(M::Unit) + 0.5f);
                const float Rr = R * 0.80f;   // the radar; the momentary ring sits outside it
                // radius: ring 1 (outer) = target + res, ring 2 = target, ... ring 5 = target - 3 res, centre = target - 4 res
                auto rad = [&](double l) { return static_cast<float>(std::clamp((l - (target - 4 * res)) / (5 * res), 0.0, 1.0)) * Rr; };
                auto ringCol = [&](double l) {   // black, blue, dark green, light green, orange (MTR-UM p.9)
                    if (l >= target) return rgb8(0xFFB020);
                    if (l >= target - res) return rgb8(0x9BE15D);
                    if (l >= target - 2 * res) return rgb8(0x2E9E4F);
                    if (l >= target - 3 * res) return rgb8(0x3A7BD5);
                    return rgb8(0x1A2A40);
                };
                const double turn = MeterBridge::radarSpeedSeconds(static_cast<int>(mb.get(M::RadarSpeed) + 0.5f));
                const std::uint64_t perTurn = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(turn / 0.1));
                const std::uint64_t n = mb.historyCount();
                constexpr std::uint64_t kSegs = 720;
                const std::uint64_t stride = std::max<std::uint64_t>(1, perTurn / kSegs);
                for (std::uint64_t sgi = 0; sgi < kSegs && sgi * stride < n && sgi * stride < perTurn; ++sgi) {
                    const std::uint64_t idx = n - 1 - sgi * stride;
                    double l = -200;
                    for (std::uint64_t q = 0; q < std::min<std::uint64_t>(stride, 8) && q <= idx; ++q) l = std::max(l, static_cast<double>(mb.historyLufs(idx - q * std::max<std::uint64_t>(1, stride / 8))));
                    if (l <= -100) continue;
                    const float a1 = 2 * kPi * static_cast<float>(idx % perTurn) / static_cast<float>(perTurn);
                    const float a0 = a1 - 2 * kPi * static_cast<float>(stride) / static_cast<float>(perTurn) * 1.15f;
                    const float rr = rad(l);
                    const Rgba c = ringCol(l);
                    const float xs[4] = {cx, cx + std::sin(a0) * rr, cx + std::sin(a1) * rr, cx};
                    const float ys[4] = {cy, cy - std::cos(a0) * rr, cy - std::cos(a1) * rr, cy};
                    const Rgba cc[4] = {c.withAlpha(0.35f), c.withAlpha(0.95f), c.withAlpha(0.95f), c.withAlpha(0.35f)};
                    r.colorQuad(xs, ys, cc, Blend::Over, true);
                }
                for (int k = 1; k <= 5; ++k) {
                    const double l = target + res * (2 - k);
                    const float rr = rad(l);
                    arcCw(r, cx, cy, rr, k == 2 ? 3.0f : 2.0f, 0, 2 * kPi - 0.001f, (k == 2 ? C::amber : C::text).withAlpha(k == 2 ? 0.55f : 0.18f), false);
                    r.text(TextStyle::Caption, fmt("%.0f", unit == 2 ? l - target : l), cx + 8, cy - rr + 30, C::dim, Align::Left, TextFx::Shadowed);
                }
                if (n > 0) {   // sweep arm
                    const float a = 2 * kPi * static_cast<float>((n - 1) % perTurn) / static_cast<float>(perTurn);
                    const float xs[] = {cx, cx + std::sin(a) * Rr}, ys[] = {cy, cy - std::cos(a) * Rr};
                    r.polyline(xs, ys, 2, 3.0f, C::text.withAlpha(0.7f), Blend::Over, true);
                }
                // momentary ring: 6 o'clock (target - 2 x range) clockwise to 3 o'clock (target + range); blue / green / orange
                const double mr = spec::meter::kMomRangeLu[static_cast<std::size_t>(std::clamp(static_cast<int>(mb.get(M::MomRange) + 0.5f), 0, 1))];
                const double lo = target - 2 * mr, hi = target + mr;
                auto ang = [&](double l) { return kPi + static_cast<float>(std::clamp((l - lo) / (hi - lo), 0.0, 1.0)) * 1.5f * kPi; };
                const float mrR = R * 0.93f, mrW = R * 0.05f;
                arcCw(r, cx, cy, mrR, mrW, kPi, 2.5f * kPi, C::text.withAlpha(0.08f), false);
                const double m = mb.momentaryLufs();
                if (m > lo) {
                    const float am = ang(m);
                    const float aG = ang(target - mr), aO = ang(target);
                    arcCw(r, cx, cy, mrR, mrW, kPi, std::min(am, aG), rgb8(0x3A7BD5), true);
                    if (am > aG) arcCw(r, cx, cy, mrR, mrW, aG, std::min(am, aO), C::green, true);
                    if (am > aO) arcCw(r, cx, cy, mrR, mrW, aO, am, C::amber, true);
                }
                for (int k = 0; k <= 6; ++k) {   // numbers round the ring every quarter of the range
                    const double l = lo + (hi - lo) * k / 6.0;
                    const float a = ang(l), tr = R * 1.02f;
                    r.text(TextStyle::Caption, fmt("%.0f", unit == 2 ? l - target : l), cx + std::sin(a) * tr, cy - std::cos(a) * tr + 9, C::dim, Align::Center, TextFx::Shadowed);
                }
                if (mb.get(M::LoudAlert) > 0.5f) {   // the LOUD level marked in red on the ring
                    const float a = ang(target + mb.get(M::LoudAlert));
                    const float xs[] = {cx + std::sin(a) * (mrR - mrW), cx + std::sin(a) * (mrR + mrW)}, ys[] = {cy - std::cos(a) * (mrR - mrW), cy - std::cos(a) * (mrR + mrW)};
                    r.polyline(xs, ys, 2, 5.0f, C::red, Blend::Over, true);
                }
                // corner lamps and texts, as on the unit
                const Rect pk{w.r.x + 40, w.r.y + 40, 26, 26}, ld{w.r.right() - 66, w.r.y + 40, 26, 26};
                r.fillRound(pk, 5, mb.peakAlert() ? C::red : C::red.withAlpha(0.18f), Blend::Over, mb.peakAlert());
                r.fillRound(ld, 5, mb.loudAlert() ? C::red : C::red.withAlpha(0.18f), Blend::Over, mb.loudAlert());
                r.text(TextStyle::Label, "PEAK", pk.right() + 14, pk.bottom(), C::text, Align::Left, TextFx::Shadowed);
                r.text(TextStyle::Label, "LOUD", ld.x - 14, ld.bottom(), C::text, Align::Right, TextFx::Shadowed);
                const double t = mb.measuredSeconds();
                const int hh = static_cast<int>(t / 3600), mi = static_cast<int>(t / 60) % 60, se = static_cast<int>(t) % 60;
                char tb[32]; std::snprintf(tb, sizeof tb, "%02d:%02d:%02d", hh, mi, se);
                r.text(TextStyle::Label, unit == 1 ? "LKFS" : unit == 2 ? "LU" : "LUFS", w.r.right() - 40, w.r.bottom() - 110, C::text, Align::Right, TextFx::Shadowed);
                r.text(TextStyle::Panel, tb, w.r.right() - 40, w.r.bottom() - 50, C::text, Align::Right, TextFx::NeonCrisp);
                r.text(TextStyle::Label, app_.meterEdited ? "EDITED:" : "PRESET:", w.r.x + 40, w.r.bottom() - 130, C::dim, Align::Left, TextFx::Shadowed);
                r.text(TextStyle::Label, app_.meterPreset, w.r.x + 40, w.r.bottom() - 80, C::text, Align::Left, TextFx::Shadowed);
                r.text(TextStyle::Label, fmt("TARGET %.0f", unit == 2 ? 0.0 : target) + (unit == 2 ? " LU" : unit == 1 ? " LKFS" : " LUFS"), w.r.x + 40, w.r.bottom() - 20, C::amber, Align::Left, TextFx::Shadowed);
                if (!mb.measuring()) r.text(TextStyle::Heading, "PAUSED", cx, cy + 20, C::amber, Align::Center, TextFx::NeonCrisp);
                break;
            }
            case CustomId::Rta: {
                namespace M = meter_ids;
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                const bool oct = mb.get(M::RtaBands) < 0.5f;
                const int nb = oct ? MeterBridge::kOctaves : MeterBridge::kBands;
                const double topDb = -5.0 * std::lround(mb.get(M::RtaTop)), range = 10.0 * (std::lround(mb.get(M::RtaRange)) + 1);
                const Rect in{w.r.x + 110, w.r.y + 110, w.r.w - 150, w.r.h - 200};
                auto yOf = [&](double db) { return in.y + in.h * static_cast<float>(std::clamp((topDb - db) / range, 0.0, 1.0)); };
                const double step = range <= 20 ? 2.0 : range <= 40 ? 5.0 : 10.0;
                for (double db = topDb; db >= topDb - range - 0.01; db -= step) {
                    const float y = yOf(db);
                    r.fillRound({in.x, y - 1, in.w, 2}, 1, C::text.withAlpha(0.10f), Blend::Over);
                    r.text(TextStyle::Caption, fmt("%.0f", db), in.x - 16, y + 9, C::dim, Align::Right, TextFx::Shadowed);
                }
                r.text(TextStyle::Caption, "dB", in.x - 16, in.y - 30, C::dim, Align::Right, TextFx::Shadowed);
                const int cursor = static_cast<int>(mb.get(M::RtaCursor) + 0.5f);
                const int cur = oct ? std::clamp((cursor - 1) / 3, 0, MeterBridge::kOctaves - 1) : cursor;
                const float bw = in.w / static_cast<float>(nb);
                for (int b = 0; b < nb; ++b) {
                    float& sm = oct ? view_->oct[static_cast<std::size_t>(b)] : view_->rta[static_cast<std::size_t>(b)];
                    const float v = static_cast<float>(oct ? mb.octaveDb(b) : mb.bandDb(b));
                    sm = v;   // the engine does the ballistics (INTEGRATION TIME)
                    const float yTop = yOf(sm), yPk = yOf(oct ? mb.octavePeakDb(b) : mb.bandPeakDb(b));
                    const float x = in.x + bw * static_cast<float>(b);
                    const Rgba col = b == cur ? C::amber : C::lime;
                    if (b == cur) r.fillRound({x, in.y, bw, in.h}, 3, C::amber.withAlpha(0.12f), Blend::Over);
                    if (in.bottom() - yTop > 1) r.fillRound({x + bw * 0.12f, yTop, bw * 0.76f, in.bottom() - yTop}, 3, col.withAlpha(0.85f), Blend::Over, true);
                    if (mb.get(M::RtaHold) > 0.5f && in.bottom() - yPk > 1) r.fillRound({x + bw * 0.12f, yPk - 3, bw * 0.76f, 6}, 2, C::text, Blend::Over, true);
                }
                const char* labels[] = {"31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
                for (int k = 0; k < 10; ++k) {
                    const float bx = oct ? in.x + bw * (static_cast<float>(k) + 0.5f) : in.x + bw * (static_cast<float>(2 + 3 * k) + 0.5f);
                    r.text(TextStyle::Caption, labels[k], bx, in.bottom() + 44, C::dim, Align::Center, TextFx::Shadowed);
                }
                const double hz = oct ? MeterBridge::octaveHz(cur) : MeterBridge::bandHz(cur);
                const double lv = oct ? mb.octaveDb(cur) : mb.bandDb(cur);
                const std::string fs = hz >= 1000 ? fmt("%.3g kHz", hz / 1000.0) : fmt("%.3g Hz", hz);
                r.text(TextStyle::Label, "FREQUENCY  " + fs + "     LEVEL  " + (lv <= -143 ? std::string("--") : fmt("%.1f dB", lv)), w.r.right() - 40, w.r.y + 64,
                       C::amber, Align::Right, TextFx::Shadowed);
                r.text(TextStyle::Label, std::string(MeterBridge::params()[M::RtaChannels].choices[static_cast<int>(mb.get(M::RtaChannels) + 0.5f)]) + "   " +
                       MeterBridge::params()[M::RtaTime].choices[static_cast<int>(mb.get(M::RtaTime) + 0.5f)], in.x, w.r.y + 64, C::dim, Align::Left, TextFx::Shadowed);
                break;
            }
            case CustomId::Scope: {
                namespace M = meter_ids;
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                const float cx = w.r.cx(), cy = w.r.cy() + 20, S = std::min(w.r.w, w.r.h) * 0.42f;
                const float xs1[] = {cx - S, cx + S}, ys1[] = {cy + S, cy - S}, xs2[] = {cx - S, cx + S}, ys2[] = {cy - S, cy + S};
                r.polyline(xs1, ys1, 2, 2, C::text.withAlpha(0.12f));   // the L and R axes
                r.polyline(xs2, ys2, 2, 2, C::text.withAlpha(0.12f));
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::clamp(std::chrono::duration<float>(now - view_->last).count(), 0.0f, 0.5f);
                view_->last = now;
                const std::uint64_t n = mb.scopeWritten();
                const int persist = static_cast<int>(mb.get(M::ScopePersist) + 0.5f);
                const auto frames = static_cast<std::uint64_t>(std::min<double>(spec::meter::kScopePersistS[static_cast<std::size_t>(std::clamp(persist, 0, 3))] * 48000.0,
                                                                                MeterBridge::kScopeFrames - 1));
                const std::uint64_t have = std::min(n, frames);
                constexpr std::uint64_t kMaxPts = 6000;
                const std::uint64_t step = std::max<std::uint64_t>(1, have / kMaxPts);
                // scale: fixed (0 .. -24 dB = the level that fills the screen) or AUTO FAST / AUTO SLOW
                const int sc = static_cast<int>(mb.get(M::ScopeScale) + 0.5f);
                float peak = 1e-6f;
                for (std::uint64_t k = 0; k < have; k += step) { float l = 0, rr = 0; mb.scopeFrame(n - have + k, l, rr); peak = std::max({peak, std::fabs(l), std::fabs(rr)}); }
                if (sc >= 25) {
                    const float tau = static_cast<float>(sc == 25 ? spec::meter::kScopeAutoFastS : spec::meter::kScopeAutoSlowS);
                    const float want = 0.8f / std::max(peak, 1e-4f);
                    view_->scopeGain += (want - view_->scopeGain) * (1.0f - std::exp(-dt / tau));
                } else {
                    view_->scopeGain = std::pow(10.0f, static_cast<float>(sc) / 20.0f);
                }
                const float g = view_->scopeGain;
                static const Rgba styles[] = {rgb8(0x9FD8FF), C::green, C::amber, C::text};
                const Rgba col = styles[std::clamp(static_cast<int>(mb.get(M::ScopeStyle) + 0.5f), 0, 3)];
                if (have > 8) {
                    static thread_local std::vector<float> px, py;
                    const std::size_t cnt = static_cast<std::size_t>(have / step);
                    px.resize(cnt); py.resize(cnt);
                    for (std::size_t k = 0; k < cnt; ++k) {
                        float l = 0, rr = 0;
                        mb.scopeFrame(n - have + k * step, l, rr);
                        const float side = (l - rr) * 0.7071f * g, mid = (l + rr) * 0.7071f * g;
                        px[k] = cx + std::clamp(side, -1.0f, 1.0f) * S;
                        py[k] = cy - std::clamp(mid, -1.0f, 1.0f) * S;
                    }
                    r.polyline(px.data(), py.data(), static_cast<int>(cnt), 2.0f, col.withAlpha(persist == 0 ? 0.65f : 0.35f), Blend::Add, true);
                }
                const std::string mode = sc == 25 ? "AUTO FAST" : sc == 26 ? "AUTO SLOW" : "SCALE";
                r.text(TextStyle::Label, mode + fmt("  %.1f dB", 20.0 * std::log10(std::max(1e-6f, 1.0f / g))), cx, w.r.y + 64, C::text, Align::Center, TextFx::Shadowed);
                r.text(TextStyle::Label, "L", w.r.x + 50, w.r.y + 64, C::dim, Align::Center, TextFx::Shadowed);
                r.text(TextStyle::Label, "R", w.r.right() - 50, w.r.y + 64, C::dim, Align::Center, TextFx::Shadowed);
                break;
            }
            case CustomId::PeakMeter: {   // the true-peak meter / PPM (PEAK METER SCALE, PPM ALIGNMENT)
                namespace M = meter_ids;
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                const int scale = static_cast<int>(mb.get(M::PeakScale) + 0.5f);
                const double align = spec::meter::kPpmAlignDbfs[static_cast<std::size_t>(std::clamp(static_cast<int>(mb.get(M::PpmAlign) + 0.5f), 0, 1))];
                // display units: dBTP / dBFS, or the standard's own scale re the alignment level (ASSUMED layouts, ConsoleSpecs)
                double lo = -60, hi = 0, off = 0, mul = 1;   // unit = (dBFS - off) * mul
                std::vector<double> marks;
                const char* unitName = "dBTP";
                switch (scale) {
                case M::ScaleTp60: lo = -60; marks = {0, -6, -12, -18, -24, -30, -40, -50, -60}; break;
                case M::ScaleTp30: lo = -30; marks = {0, -3, -6, -9, -12, -15, -18, -21, -24, -27, -30}; break;
                case M::ScaleTp20: lo = -20; marks = {0, -2, -4, -6, -8, -10, -12, -14, -16, -18, -20}; break;
                case M::ScaleDigitalPpm: lo = -50; marks = {0, -5, -10, -15, -20, -30, -40, -50}; unitName = "dBFS"; break;
                case M::ScaleDin: off = align + 9; lo = -50; hi = 5; marks = {5, 0, -5, -10, -20, -30, -40, -50}; unitName = "DIN dB"; break;
                case M::ScaleNordic: off = align; lo = -36; hi = 12; marks = {12, 6, 0, -6, -12, -18, -24, -30, -36}; unitName = "NORDIC"; break;
                case M::ScaleBbc: off = align - 16; mul = 0.25; lo = 0.5; hi = 7.5; marks = {7, 6, 5, 4, 3, 2, 1}; unitName = "BBC"; break;
                default: off = align; lo = -12; hi = 12; marks = {12, 8, 4, 0, -4, -8, -12}; unitName = "EBU"; break;
                }
                const Rect in{w.r.x + 40, w.r.y + 90, w.r.w - 80, w.r.h - 150};
                auto yOf = [&](double dbfs) { const double u = (dbfs - off) * mul; return in.bottom() - in.h * static_cast<float>(std::clamp((u - lo) / (hi - lo), 0.0, 1.0)); };
                r.text(TextStyle::Label, unitName, w.r.x + 30, w.r.y + 56, C::dim, Align::Left, TextFx::Shadowed);
                const float barW = in.w * 0.24f, gap = in.w * 0.06f, scaleX = in.x + in.w * 0.5f;
                for (double mk : marks) {
                    const double dbfs = mk / mul + off;
                    const float y = yOf(dbfs);
                    r.fillRound({in.x, y - 1, in.w, 2}, 1, C::text.withAlpha(0.08f), Blend::Over);
                    r.text(TextStyle::Caption, (scale == M::ScaleBbc ? fmt("%.0f", mk) : fmt("%+.0f", mk)), scaleX, y + 9, C::dim, Align::Center, TextFx::Shadowed);
                }
                const int alertIdx = static_cast<int>(mb.get(M::PeakAlert) + 0.5f);
                const double alertDb = alertIdx > 0 ? -12.0 + 0.5 * (alertIdx - 1) : 1e9;
                for (int side = 0; side < 2; ++side) {
                    const float x = side == 0 ? scaleX - gap - barW - 30 : scaleX + gap + 30;
                    const Rect bar{x, in.y, barW, in.h};
                    r.fillRound(bar, 6, kSlot, Blend::Over);
                    const double v = mb.ppmDb(side), h = mb.peakHoldDb(side);
                    const float yv = yOf(v);
                    if (bar.bottom() - yv > 1) {
                        const float ya = std::clamp(yOf(alertDb), yv, bar.bottom());
                        r.fillRound({bar.x + 4, ya, bar.w - 8, bar.bottom() - ya}, 4, rgb8(0xE8E04A), Blend::Over, true);
                        if (ya > yv) r.fillRound({bar.x + 4, yv, bar.w - 8, ya - yv}, 4, C::red, Blend::Over, true);
                    }
                    if (h > -143) r.fillRound({bar.x + 2, yOf(h) - 3, bar.w - 4, 6}, 2, h >= alertDb ? C::red : C::text, Blend::Over, true);
                    r.text(TextStyle::Label, side == 0 ? "L" : "R", bar.cx(), w.r.bottom() - 26, C::text, Align::Center, TextFx::Shadowed);
                }
                break;
            }
            case CustomId::MeterStatus: break;
            case CustomId::StereoBars:
            case CustomId::ToolMeters: {
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                struct Bar { std::string cap; float v, lo, hi; std::string text; };
                std::vector<Bar> bars;
                if (w.custom == CustomId::StereoBars) {   // CHANNEL MEASURES (MTR-UM p.12): correlation, mono deviation
                    bars.push_back({"CORRELATION", static_cast<float>(mb.correlation()), -1, 1, fmt("%+.2f", mb.correlation())});
                    const double md = mb.monoDeviationLu();
                    bars.push_back({mb.get(meter_ids::DevSpan) < 0.5f ? "MONO DEV. (S)" : "MONO DEV. (I)", static_cast<float>(std::clamp(md, -4.0, 4.0)), -4, 4,
                                    md < -100 ? std::string("CANCELS") : fmt("%+.1f LU", md)});
                } else {
                    const float c = targetSlotCorrelation(w);
                    bars.push_back({"CORRELATION", c, -1, 1, fmt("%+.2f", c)});
                }
                const float rh = (w.r.h - 30) / static_cast<float>(bars.size());
                for (std::size_t k = 0; k < bars.size(); ++k) {
                    const Bar& b = bars[k];
                    const float y = w.r.y + 15 + rh * static_cast<float>(k);
                    r.text(TextStyle::Caption, b.cap, w.r.x + 30, y + 36, C::dim, Align::Left, TextFx::Shadowed);
                    r.text(TextStyle::Value, b.text, w.r.right() - 30, y + 38, C::lime, Align::Right, TextFx::NeonCrisp);
                    const Rect track{w.r.x + 30, y + 54, w.r.w - 60, std::min(30.0f, rh - 70)};
                    r.fillRound(track, 6, kSlot, Blend::Over);
                    const float zero = std::clamp((0.0f - b.lo) / (b.hi - b.lo), 0.0f, 1.0f);
                    const float f = std::clamp((b.v - b.lo) / (b.hi - b.lo), 0.0f, 1.0f);
                    const float x0 = track.x + track.w * std::min(zero, f), x1 = track.x + track.w * std::max(zero, f);
                    r.fillRound({track.x + track.w * zero - 1.5f, track.y - 4, 3, track.h + 8}, 1.5f, C::text.withAlpha(0.5f), Blend::Over);
                    // correlation: out-of-phase (below 0) in orange, in phase in blue-green (MTR-UM p.12)
                    const Rgba col = b.cap == "CORRELATION" ? (b.v < 0 ? C::amber : C::cyan) : C::lime;
                    if (x1 - x0 > 1) r.fillRound({x0, track.y + 4, x1 - x0, track.h - 8}, 4, col, Blend::Over, true);
                }
                break;
            }
            case CustomId::GrNeedle: {
                namespace T = twinbus_ids;
                const int side = w.customArg;
                const float target = -rig_.twinBus().meter(side == 0 ? T::MeterGr1 : T::MeterGr2);   // >= 0 dB
                float& nd = view_->needle[side];
                nd += (std::clamp(target, 0.0f, 20.0f) - nd) * 0.35f;   // moving-coil damping
                r.fillRound(w.r.inset(2), 18, rgb8(0xE9E2CC, 0.92f), Blend::Over);   // meter card
                const float cx = w.r.cx(), cy = w.r.bottom() - 40, R = w.r.h * 0.78f;
                const float aMin = -0.32f * kPi, aMax = 0.32f * kPi;
                const float marks[] = {0, 1, 2, 3, 4, 6, 8, 10, 14, 20};
                for (float m : marks) {
                    const float a = aMax - (aMax - aMin) * std::sqrt(m / 20.0f);
                    const float xs[] = {cx + std::sin(a) * R * 0.86f, cx + std::sin(a) * R * 0.96f}, ys[] = {cy - std::cos(a) * R * 0.86f, cy - std::cos(a) * R * 0.96f};
                    r.polyline(xs, ys, 2, 3.0f, rgb8(0x1A1A1A));
                    r.text(TextStyle::Caption, fmt("%.0f", m), cx + std::sin(a) * R * 0.76f, cy - std::cos(a) * R * 0.76f + 8, rgb8(0x1A1A1A), Align::Center, TextFx::Plain);
                }
                const float a = aMax - (aMax - aMin) * std::sqrt(nd / 20.0f);
                const float xs[] = {cx, cx + std::sin(a) * R * 0.95f}, ys[] = {cy, cy - std::cos(a) * R * 0.95f};
                r.polyline(xs, ys, 2, 4.0f, rgb8(0xB01010));
                r.fillRound({cx - 14, cy - 14, 28, 28}, 14, rgb8(0x101010), Blend::Over);
                r.text(TextStyle::Caption, side == 0 ? "GR  CH 1" : "GR  CH 2", w.r.x + 24, w.r.y + 40, rgb8(0x1A1A1A), Align::Left, TextFx::Plain);
                break;
            }
            default: break;
            }
            break;
        }
        }
    }

    drawScrollbars();

    // value bubble for the control under the mouse
    const int show = active >= 0 ? active : hover;
    if (show >= 0 && show < static_cast<int>(pg.widgets.size())) {
        const Widget& w = pg.widgets[static_cast<std::size_t>(show)];
        std::string name = !w.tip.empty() ? w.tip : w.ctl.valid() ? std::string(w.ctl.info->name) : w.label;
        std::string s = name;
        if (w.ctl.valid()) s += ":  " + widgetValueText(w);
        if (!s.empty()) {
            const TextStyle st = TextStyle::Value;
            const float tw = r.textWidth(st, s) + 40, th = 64;
            float x = std::clamp(w.r.cx() - tw * 0.5f, 20.0f, kWidth - 20 - tw);
            float y = w.r.y - th - 26;
            if (y < 10) y = w.r.bottom() + 20;
            r.fillRound({x, y, tw, th}, 14, rgb8(0x0B0D10, 0.94f), Blend::Over);
            r.fillRound({x + 6, y + 4, tw - 12, 3}, 1.5f, C::violet, Blend::Over, true);
            r.text(st, s, x + tw * 0.5f, y + th * 0.5f + r.capHeight(st) * 0.5f, C::text, Align::Center, TextFx::Plain);
        }
    }
}

// Zoomed in: thin bars on the window edges show which part of the faceplate is on screen.
void ConsoleScreen::drawScrollbars() {
    Renderer& r = renderer_;
    const float s = canvas_.scale, W = static_cast<float>(fbW_), H = static_cast<float>(fbH_);
    const float th = 7.0f / s, pad = 4.0f / s;
    const float left = canvas_.toDesignX(0), right = canvas_.toDesignX(W), top = canvas_.toDesignY(0), bottom = canvas_.toDesignY(H);
    if (kWidth * s > W + 0.5f) {
        const float y = bottom - th - pad;
        r.fillRound({left + pad, y, right - left - 2 * pad, th}, th * 0.5f, rgb8(0x000000, 0.55f), Blend::Over);
        const float a = left + pad + (right - left - 2 * pad) * (left / kWidth), len = (right - left - 2 * pad) * ((right - left) / kWidth);
        r.fillRound({a, y, len, th}, th * 0.5f, C::violet.withAlpha(0.85f), Blend::Over, true);
    }
    if (kHeight * s > H + 0.5f) {
        const float x = right - th - pad;
        r.fillRound({x, top + pad, th, bottom - top - 2 * pad}, th * 0.5f, rgb8(0x000000, 0.55f), Blend::Over);
        const float a = top + pad + (bottom - top - 2 * pad) * (top / kHeight), len = (bottom - top - 2 * pad) * ((bottom - top) / kHeight);
        r.fillRound({x, a, th, len}, th * 0.5f, C::violet.withAlpha(0.85f), Blend::Over, true);
    }
}

float ConsoleScreen::targetSlotCorrelation(const Widget&) const {
    const ToolSlot& s = app_.toolTarget >= kColourToolTarget ? rig_.colourTool() : rig_.channelTool(app_.toolTarget);
    return s.tool(ToolSlot::Shuffle).meter(shuffle_ids::MeterCorrelation);
}

void ConsoleScreen::render(unsigned int fbo) {
    if (!ready_ || !haveCanvas_) return;
    const auto t0 = std::chrono::steady_clock::now();
    ensurePlate();
    if (staticDirty_) drawStatic();
    renderer_.beginFrame(canvas_);
    drawFrame();
    renderer_.endFrame(fbo);
    frameMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ConsoleScreen::renderToCapture() {
    if (!ready_ || !haveCanvas_) return;
    ensurePlate();
    if (staticDirty_) drawStatic();
    renderer_.beginFrame(canvas_);
    drawFrame();
    renderer_.endFrameToCapture();
}

} // namespace hc::console::ui
