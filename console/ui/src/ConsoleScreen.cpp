// HEM DIGITAL CONSOLE — GPU screen: plates, static engraving, live controls, meters.
#include "hybrid/console/ui/ConsoleScreen.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/TwinBus.hpp"
#include "hybrid/ui/Faceplate.hpp"

#include <algorithm>
#include <chrono>
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

} // namespace

// Per-frame display state (meter ballistics).
struct ConsoleScreen::ViewState {
    float needle[2] = {0, 0};
    std::array<float, MeterBridge::kBands> rta{};
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
            const bool ring = w.kind == Kind::Stepped && w.r.w >= 180 && w.ctl.valid() && w.ctl.info->choices;
            r.text(st, w.label, w.r.cx(), w.r.y - (ring ? 52.0f : w.r.w < 100 ? 10.0f : 18.0f), C::text, Align::Center, TextFx::Shadowed);
            if (ring) {   // the step names printed around a detented knob (every one up to 11 steps, else the ends and middle)
                const int n = static_cast<int>(w.ctl.info->max + 0.5f);
                for (int k = 0; k <= n; ++k) {
                    if (n > 10 && k != 0 && k != n && k != n / 2) continue;
                    std::string t = w.ctl.info->choices[k];
                    for (const char* suf : {" dB", " Hz"}) if (t.size() > 3 && t.ends_with(suf)) t.resize(t.size() - 3);
                    if (t.size() > 4 && t.ends_with(" kHz")) t = t.substr(0, t.size() - 4) + "k";
                    const float a = kKnobMin + (kKnobMax - kKnobMin) * static_cast<float>(k) / static_cast<float>(std::max(1, n));
                    const float R = w.r.w * 0.5f * 1.30f;
                    r.text(TextStyle::Caption, t, w.r.cx() + std::sin(a) * R, w.r.cy() - std::cos(a) * R + 9, C::dim, Align::Center, TextFx::Shadowed);
                }
            }
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
        if (w.visible && !w.label.empty() && (w.kind == Kind::Knob || w.kind == Kind::Stepped))
            r.text(w.r.w < 170 ? TextStyle::Value : TextStyle::Label, w.label, w.r.cx(), w.r.y - 18, C::text, Align::Center, TextFx::Shadowed);
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
            case CustomId::LoudNumbers: {
                r.fillRound(w.r.inset(4), 18, kGlass, Blend::Over);
                const double tpl = mb.truePeakDbtp(0), tpr = mb.truePeakDbtp(1);
                struct Row { const char* cap; std::string v; const char* unit; bool warn; };
                const Row rows[] = {
                    {"MOMENTARY", lufsText(mb.momentaryLufs()), "LUFS", false},
                    {"SHORT-TERM", lufsText(mb.shortTermLufs()), "LUFS", false},
                    {"INTEGRATED", lufsText(mb.integratedLufs()), "LUFS", false},
                    {"LOUDNESS RANGE", mb.integratedLufs() <= -143 ? "--" : fmt("%.1f", mb.loudnessRangeLu()), "LU", false},
                    {"MAX MOMENTARY", lufsText(mb.maxMomentaryLufs()), "LUFS", false},
                    {"MAX SHORT-TERM", lufsText(mb.maxShortTermLufs()), "LUFS", false},
                    {"TRUE PEAK L", lufsText(tpl), "dBTP", tpl > -1.0},
                    {"TRUE PEAK R", lufsText(tpr), "dBTP", tpr > -1.0},
                    {"PLR", mb.plrDb() <= -143 ? "--" : fmt("%.1f", mb.plrDb()), "dB", false},
                    {"SAMPLE PEAK", lufsText(std::max(mb.samplePeakDbfs(0), mb.samplePeakDbfs(1))), "dBFS", false},
                };
                const float rh = (w.r.h - 40) / static_cast<float>(std::size(rows));
                for (std::size_t k = 0; k < std::size(rows); ++k) {
                    const float y = w.r.y + 20 + rh * static_cast<float>(k);
                    r.text(TextStyle::Value, rows[k].cap, w.r.x + 40, y + rh * 0.5f + 12, C::dim, Align::Left, TextFx::Shadowed);
                    r.text(TextStyle::Panel, rows[k].v, w.r.right() - 150, y + rh * 0.5f + 20, rows[k].warn ? C::red : C::lime, Align::Right, TextFx::NeonCrisp);
                    r.text(TextStyle::Caption, rows[k].unit, w.r.right() - 40, y + rh * 0.5f + 12, C::dim, Align::Right, TextFx::Shadowed);
                }
                break;
            }
            case CustomId::Radar: {
                const float cx = w.r.cx(), cy = w.r.cy(), R = w.r.w * 0.47f;
                r.fillRound(w.r.inset(w.r.w * 0.02f), w.r.w * 0.48f, kGlass, Blend::Over);
                auto rad = [R](double lufs) { return static_cast<float>(std::clamp((lufs + 60.0) / 60.0, 0.0, 1.0)) * R * 0.86f; };
                // history: one turn = 4 minutes of short-term loudness (2400 points of 100 ms)
                const std::uint64_t n = mb.historyCount();
                constexpr std::uint64_t kTurn = 2400;
                const std::uint64_t from = n > kTurn ? n - kTurn : 0;
                for (std::uint64_t k = from; k < n; k += 2) {
                    const double l = mb.historyLufs(k);
                    if (l <= -100) continue;
                    const float a0 = 2 * kPi * static_cast<float>(k % kTurn) / static_cast<float>(kTurn);
                    const float a1 = a0 + 2 * kPi * 2.2f / static_cast<float>(kTurn);
                    const float rr = rad(l);
                    const float xs[4] = {cx, cx + std::sin(a0) * rr, cx + std::sin(a1) * rr, cx};
                    const float ys[4] = {cy, cy - std::cos(a0) * rr, cy - std::cos(a1) * rr, cy};
                    const Rgba c = l > -9 ? C::red : l > -16 ? C::amber : C::lime;
                    const Rgba cc[4] = {c.withAlpha(0.15f), c.withAlpha(0.85f), c.withAlpha(0.85f), c.withAlpha(0.15f)};
                    r.colorQuad(xs, ys, cc, Blend::Over, true);
                }
                for (int db = -50; db <= 0; db += 10) {
                    const float rr = rad(db);
                    r.arc(cx, cy, rr, 2.0f, 0, 2 * kPi - 0.001f, C::text.withAlpha(0.18f), false);
                    r.text(TextStyle::Caption, std::to_string(db), cx + 6, cy - rr - 6, C::dim, Align::Left, TextFx::Shadowed);
                }
                if (n > 0) {   // sweep arm
                    const float a = 2 * kPi * static_cast<float>((n - 1) % kTurn) / static_cast<float>(kTurn);
                    const float xs[] = {cx, cx + std::sin(a) * R * 0.86f}, ys[] = {cy, cy - std::cos(a) * R * 0.86f};
                    r.polyline(xs, ys, 2, 3.0f, C::text.withAlpha(0.7f), Blend::Over, true);
                }
                // momentary loudness: outer ring
                const double m = mb.momentaryLufs();
                const float mf = static_cast<float>(std::clamp((m + 60.0) / 60.0, 0.0, 1.0));
                r.arc(cx, cy, R * 0.94f, R * 0.05f, -kPi, kPi - 0.001f, C::violet.withAlpha(0.15f), false);
                if (mf > 0) r.arc(cx, cy, R * 0.94f, R * 0.05f, -kPi, -kPi + 2 * kPi * mf, m > -9 ? C::red : C::violet, true);
                r.text(TextStyle::Display, lufsText(mb.shortTermLufs()), cx, cy + 40, C::text, Align::Center, TextFx::NeonCrisp);
                r.text(TextStyle::Caption, "SHORT-TERM LUFS", cx, cy + 90, C::dim, Align::Center, TextFx::Shadowed);
                break;
            }
            case CustomId::Rta: {
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                const Rect in{w.r.x + 70, w.r.y + 30, w.r.w - 100, w.r.h - 90};
                for (int db = 0; db >= -80; db -= 20) {
                    const float y = in.y + in.h * static_cast<float>(-db) / 80.0f;
                    r.fillRound({in.x, y - 1, in.w, 2}, 1, C::text.withAlpha(0.10f), Blend::Over);
                    r.text(TextStyle::Caption, std::to_string(db), in.x - 12, y + 8, C::dim, Align::Right, TextFx::Shadowed);
                }
                const float bw = in.w / static_cast<float>(MeterBridge::kBands);
                for (int b = 0; b < MeterBridge::kBands; ++b) {
                    float& sm = view_->rta[static_cast<std::size_t>(b)];
                    const float v = static_cast<float>(std::max(mb.bandDb(b), -80.0));
                    sm = v > sm ? v : sm + (v - sm) * 0.25f;
                    const float f = std::clamp((sm + 80.0f) / 80.0f, 0.0f, 1.0f);
                    const float pk = std::clamp((static_cast<float>(mb.bandPeakDb(b)) + 80.0f) / 80.0f, 0.0f, 1.0f);
                    const float x = in.x + bw * static_cast<float>(b);
                    if (f > 0) r.fillRound({x + bw * 0.12f, in.bottom() - in.h * f, bw * 0.76f, in.h * f}, 3, C::lime.withAlpha(0.85f), Blend::Over, true);
                    if (pk > 0) r.fillRound({x + bw * 0.12f, in.bottom() - in.h * pk - 3, bw * 0.76f, 6}, 2, C::text, Blend::Over, true);
                }
                const char* labels[] = {"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
                const int idx[] = {1, 4, 7, 10, 13, 16, 19, 22, 25, 28};   // 31.6, 63, 125 ... Hz centres
                for (int k = 0; k < 10; ++k)
                    r.text(TextStyle::Caption, labels[k], in.x + bw * (static_cast<float>(idx[k]) + 0.5f), in.bottom() + 40, C::dim, Align::Center, TextFx::Shadowed);
                break;
            }
            case CustomId::Scope: {
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                const float cx = w.r.cx(), cy = w.r.cy(), S = std::min(w.r.w, w.r.h) * 0.42f;
                const float xs1[] = {cx - S, cx + S}, ys1[] = {cy, cy}, xs2[] = {cx, cx}, ys2[] = {cy - S, cy + S};
                r.polyline(xs1, ys1, 2, 2, C::text.withAlpha(0.12f));
                r.polyline(xs2, ys2, 2, 2, C::text.withAlpha(0.12f));
                const std::uint64_t n = mb.scopeWritten();
                constexpr int kPts = 1024;
                if (n > kPts) {
                    static thread_local std::vector<float> px, py;
                    px.resize(kPts); py.resize(kPts);
                    for (int k = 0; k < kPts; ++k) {
                        float l = 0, rr = 0;
                        mb.scopeFrame(n - kPts + static_cast<std::uint64_t>(k), l, rr);
                        const float side = (l - rr) * 0.7071f, mid = (l + rr) * 0.7071f;
                        px[static_cast<std::size_t>(k)] = cx + std::clamp(side, -1.0f, 1.0f) * S;
                        py[static_cast<std::size_t>(k)] = cy - std::clamp(mid, -1.0f, 1.0f) * S;
                    }
                    r.polyline(px.data(), py.data(), kPts, 2.0f, C::lime.withAlpha(0.55f), Blend::Add, true);
                }
                r.text(TextStyle::Caption, "M", cx, w.r.y + 40, C::dim, Align::Center, TextFx::Shadowed);
                r.text(TextStyle::Caption, "L", w.r.x + 40, w.r.y + 40, C::dim, Align::Center, TextFx::Shadowed);
                r.text(TextStyle::Caption, "R", w.r.right() - 40, w.r.y + 40, C::dim, Align::Center, TextFx::Shadowed);
                break;
            }
            case CustomId::StereoBars:
            case CustomId::ToolMeters: {
                r.fillRound(w.r.inset(4), 16, kGlass, Blend::Over);
                struct Bar { const char* cap; float v, lo, hi; std::string text; };
                std::vector<Bar> bars;
                if (w.custom == CustomId::StereoBars) {
                    bars.push_back({"CORRELATION", static_cast<float>(mb.correlation()), -1, 1, fmt("%+.2f", mb.correlation())});
                    bars.push_back({"BALANCE L / R", static_cast<float>(mb.balanceLrLu()), -6, 6, fmt("%+.1f LU", mb.balanceLrLu())});
                    const double ms = mb.balanceMsDb();
                    bars.push_back({"SIDE vs MID", static_cast<float>(std::clamp(ms, -30.0, 10.0)), -30, 10, std::fabs(ms) > 100 ? std::string("--") : fmt("%+.1f dB", ms)});
                    const double md = mb.monoDeviationLu();
                    bars.push_back({"MONO DEVIATION", static_cast<float>(std::clamp(md, -12.0, 3.0)), -12, 3, md < -100 ? std::string("CANCELS") : fmt("%+.1f LU", md)});
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
                    if (x1 - x0 > 1) r.fillRound({x0, track.y + 4, x1 - x0, track.h - 8}, 4, b.v < 0 && std::string(b.cap) == "CORRELATION" ? C::red : C::lime, Blend::Over, true);
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
