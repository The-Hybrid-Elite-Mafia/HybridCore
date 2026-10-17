#pragma once
// =============================================================================
//  Console — the complete GPU screen of the 13-stem console, as one object.
//  Owns the renderer, faceplate master, parts atlas, font atlas, layout, view and
//  mouse interaction, and keeps them matched to the framebuffer size:
//    * chassis texture re-baked only when the on-screen size crosses a bucket,
//    * Poppins re-baked when the canvas scale changes,
//    * layout rebuilt when the window's aspect changes the stretch band,
//    * the static layer re-rendered only when one of the above (or the fader bank) changes.
//  Used by HybridCore.exe and by hc_ui_snapshot, so screenshots are the real app's pixels.
//  UI thread only (needs the current GL context).
// =============================================================================
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/MixerView.hpp"
#include "hybrid/ui/Parts.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace hc::ui {

struct ConsoleTimings {
    double staticMs   = 0.0;   // CPU time to record the static layer (last rebuild)
    double recordMs   = 0.0;   // CPU time of the UI code for the last frame (layout + vertex recording)
    double frameCpuMs = 0.0;   // CPU time to record + submit the last frame (includes the GL driver)
};

class Console {
public:
    explicit Console(params::ParameterTree& tree) noexcept : tree_(tree) {}

    // assetDir holds faceplates/ and fonts/; dataDir is %APPDATA%\HybridCore (user skins).
    bool init(const std::filesystem::path& assetDir, const std::filesystem::path& dataDir, int msaaSamples, std::string* error);

    // Call before drawing whenever the framebuffer may have changed size.
    bool resize(int framebufferW, int framebufferH, std::string* error);

    void setFaderBank(std::uint32_t bank);
    [[nodiscard]] std::uint32_t faderBank() const noexcept { return bank_; }
    void setPage(Page page);
    void setScope(bool on);
    [[nodiscard]] bool scope() const noexcept { return scope_; }
    [[nodiscard]] Page page() const noexcept { return page_; }

    // Draws one frame into `targetFbo` (0 = window) or into the capture buffer.
    void render(const ViewState& view, const MeterState& meters, unsigned int targetFbo);
    void renderToCapture(const ViewState& view, const MeterState& meters);
    [[nodiscard]] Image capture() const { return renderer_.capture(); }

    [[nodiscard]] Interaction&       interaction() noexcept { return *interaction_; }
    [[nodiscard]] const Layout&      layout() const noexcept { return *layout_; }
    [[nodiscard]] const Canvas&      canvas() const noexcept { return canvas_; }
    [[nodiscard]] const Renderer&    renderer() const noexcept { return renderer_; }
    [[nodiscard]] const std::string& faceplateInfo() const noexcept { return faceplateInfo_; }
    [[nodiscard]] const ConsoleTimings& timings() const noexcept { return timings_; }
    [[nodiscard]] std::uint32_t chassisTextureWidth() const noexcept { return chassisWidth_; }

private:
    void drawStaticIfDirty();
    void drawFrame(const ViewState& view, const MeterState& meters);

    params::ParameterTree&        tree_;
    Renderer                      renderer_;
    std::optional<FaceplateArt>   art_;
    FontSet                       fonts_;
    FontAtlas                     font_;
    std::unique_ptr<Layout>       layout_;
    std::unique_ptr<MixerView>    view_;
    std::unique_ptr<Interaction>  interaction_;
    Canvas                        canvas_;
    bool                          haveCanvas_   = false;
    bool                          staticDirty_  = true;
    std::uint32_t                 chassisWidth_ = 0;
    float                         fontScale_    = 0.0f;
    std::uint32_t                 bank_         = 0;
    Page                          page_         = Page::Mix;
    bool                          scope_        = false;
    std::string                   faceplateInfo_;
    ConsoleTimings                timings_;
};

} // namespace hc::ui
