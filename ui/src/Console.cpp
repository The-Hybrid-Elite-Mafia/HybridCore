#include "hybrid/ui/Console.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hc::ui {

namespace {
double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

bool Console::init(const std::filesystem::path& assetDir, const std::filesystem::path& dataDir, int msaaSamples, std::string* error) {
    if (!renderer_.init(msaaSamples, error)) return false;
    const auto plateFile = resolveFaceplate(assetDir, dataDir);
    art_ = loadFaceplateArt(plateFile, error);
    if (!art_) return false;
    faceplateInfo_ = art_->origin;
    auto fonts = FontSet::load(assetDir / "fonts", error);
    if (!fonts) return false;
    fonts_ = std::move(*fonts);
    const PartsAtlas parts = buildPartsAtlas(art_->master);
    if (!renderer_.setParts(parts, error)) return false;
    return true;
}

bool Console::resize(int w, int h, std::string* error) {
    const Canvas c = Canvas::fit(static_cast<float>(w), static_cast<float>(h));
    if (haveCanvas_ && c == canvas_) return true;
    if (!renderer_.setTargetSize(w, h, error)) return false;
    const std::uint32_t texW = ui::chassisTextureWidth(c.scale);
    if (texW != chassisWidth_) {
        if (!renderer_.setChassis(bakeChassis(*art_, texW), error)) return false;
        chassisWidth_ = texW;
    }
    if (std::fabs(c.scale - fontScale_) > 0.005f * std::max(fontScale_, 0.01f)) {
        if (!font_.bake(fonts_, c.scale)) { if (error) *error = "could not bake the Poppins atlas"; return false; }
        if (!renderer_.setFont(font_, error)) return false;
        fontScale_ = c.scale;
    }
    if (!layout_ || layout_->extraH() != c.extraH) {
        layout_ = std::make_unique<Layout>(tree_, c.extraH);
        layout_->setFaderBank(bank_);
        layout_->setPage(page_);
        layout_->setScope(scope_);
        view_ = std::make_unique<MixerView>(*layout_, tree_);
        if (interaction_) interaction_->rebind(*layout_);
        else interaction_ = std::make_unique<Interaction>(*layout_, tree_);
    }
    canvas_     = c;
    haveCanvas_ = true;
    staticDirty_ = true;
    return true;
}

void Console::setFaderBank(std::uint32_t bank) {
    bank_ = std::min(bank, kFaderBanks - 1);
    if (layout_) layout_->setFaderBank(bank_);
    staticDirty_ = true;
}

void Console::setScope(bool on) {
    scope_ = on;
    if (layout_) layout_->setScope(on);
    if (interaction_) interaction_->mouseUp();
}

void Console::setPage(Page page) {
    page_ = page;
    if (layout_) layout_->setPage(page_);
    if (interaction_) interaction_->rebind(*layout_);   // drop any drag on a knob that changed meaning
    staticDirty_ = true;
}

void Console::drawStaticIfDirty() {
    if (!staticDirty_) return;
    const auto t0 = std::chrono::steady_clock::now();
    renderer_.beginStatic(canvas_);
    view_->drawStatic(renderer_, canvas_);
    renderer_.endStatic();
    timings_.staticMs = msSince(t0);
    staticDirty_ = false;
}

void Console::drawFrame(const ViewState& view, const MeterState& meters) {
    drawStaticIfDirty();
    renderer_.beginFrame(canvas_);
    const auto t0 = std::chrono::steady_clock::now();
    view_->drawDynamic(renderer_, view, meters);
    if (overlay_) overlay_(renderer_, canvas_);
    timings_.recordMs = msSince(t0);
}

void Console::render(const ViewState& view, const MeterState& meters, unsigned int targetFbo) {
    const auto t0 = std::chrono::steady_clock::now();
    drawFrame(view, meters);
    renderer_.endFrame(targetFbo);
    timings_.frameCpuMs = msSince(t0);
}

void Console::renderToCapture(const ViewState& view, const MeterState& meters) {
    const auto t0 = std::chrono::steady_clock::now();
    drawFrame(view, meters);
    renderer_.endFrameToCapture();
    timings_.frameCpuMs = msSince(t0);
}

} // namespace hc::ui
