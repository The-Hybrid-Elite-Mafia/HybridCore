#pragma once
// =============================================================================
//  Renderer — OpenGL 3.3 core, built for photographed-hardware look:
//
//   * Linear-light pipeline: every texture is SRGB8_ALPHA8 with premultiplied alpha
//     and a full mip chain (trilinear); render targets are sRGB; blending happens in
//     linear light with ONE premultiplied blend mode, so "over" and "additive" quads
//     batch together (additive = premultiplied colour with zero alpha).
//   * MSAA scene target (8× when the GPU allows it, else the GPU maximum).
//   * Static layer cache: the chassis and everything that doesn't move is rendered
//     once into a texture (beginStatic/endStatic) and re-used every frame; a frame
//     only re-draws moving parts on top of it.
//   * Bloom: quads flagged emissive are also drawn into a half-resolution light
//     buffer, blurred at two radii and added back before the final sRGB encode.
//   * Textures: chassis (faceplate), parts atlas, Poppins atlas — all charged to the
//     UI VRAM budget. Draw calls per frame: 1 scene batch + 1 bloom batch + post passes.
//  All drawing coordinates are DESIGN units (faceplate pixels); the Canvas maps them.
// =============================================================================
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Geometry.hpp"
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Parts.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hc::ui {

enum class Blend : std::uint8_t { Over, Add };
enum class Align : std::uint8_t { Left, Center, Right };
enum class TextFx : std::uint8_t {
    Plain,
    Engraved,   // on chrome: dark text with a 1 px light edge underneath
    Shadowed,   // on dark glass: light text with a soft shadow down-right
    Neon,       // emissive text with glow (glass display, lit captions)
    NeonCrisp,  // small neon numbers: a soft glow behind, a sharp core on top that the bloom doesn't smear
};

struct RenderStats {
    std::uint32_t drawCalls  = 0;
    std::uint32_t quads      = 0;
    std::uint32_t bloomQuads = 0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Requires a current GL 3.3 core context. `wantSamples` is the MSAA request (8).
    bool init(int wantSamples, std::string* error);
    void shutdown() noexcept;

    bool setChassis(const TextureLevels& tex, std::string* error);
    bool setParts(const PartsAtlas& atlas, std::string* error);
    bool setFont(const FontAtlas& font, std::string* error);
    // A plug-in faceplate (FxPanel): uploaded on its own texture unit, drawn with panel().
    bool setPanel(const TextureLevels& tex, std::string* error);
    void clearPanel() noexcept;
    // A picture atlas (e.g. a grid of thumbnails): its own texture unit, drawn a cell at a time with atlas().
    bool setAtlas(const TextureLevels& tex, std::string* error);
    void clearAtlas() noexcept;
    [[nodiscard]] bool hasAtlas() const noexcept { return atlasTex_ != 0; }
    // (Re)creates the render targets for a framebuffer size. Cheap when unchanged.
    bool setTargetSize(int width, int height, std::string* error);

    // Static layer: draw into the cached texture. Call again whenever it changes.
    void beginStatic(const Canvas& canvas);
    void endStatic();
    // A frame: static layer + whatever is drawn now, bloom, sRGB encode into
    // `targetFbo` (0 = window) or into the capture buffer for readback.
    void beginFrame(const Canvas& canvas);
    void endFrame(unsigned int targetFbo);
    void endFrameToCapture();
    [[nodiscard]] Image capture() const;   // RGBA8 sRGB, top row first (after endFrameToCapture)

    // ---- primitives (design units) --------------------------------------------------
    void chassis(Rect plateSrc, Rect dst);   // a region of the faceplate texture
    void panel(Rect dst);                    // the whole plug-in faceplate texture
    void atlas(Rect uv, Rect dst, Rgba tint = {1, 1, 1, 1});   // uv: 0..1 region of the picture atlas
    void sprite(Sprite s, Rect dst, Rgba tint = {}, Blend blend = Blend::Over, bool emissive = false);
    void spriteRotated(Sprite s, float cx, float cy, float w, float h, float angleRad, Rgba tint = {},
                       Blend blend = Blend::Over, bool emissive = false);
    // 9-slice: corners keep their pixel size × cornerScale (design units per sprite pixel).
    void nineSlice(Sprite s, Rect dst, float cornerScale, Rgba tint = {}, Blend blend = Blend::Over, bool emissive = false);
    // Value arc, angles in radians, 0 = up, clockwise. Soft-edged, optionally emissive.
    void arc(float cx, float cy, float radius, float width, float a0, float a1, Rgba c, bool emissive);
    // Light spilling around a rounded rectangle (additive, Gaussian falloff of `spread`).
    void glow(Rect core, float radius, float spread, Rgba c, bool emissive = true);
    // Soft-edged rounded rectangle of light or tint.
    void fillRound(Rect r, float radius, Rgba c, Blend blend, bool emissive = false);
    // Any four-cornered shape (corners in order around it, e.g. TL TR BR BL) with its own colour
    // at each corner, blended across: flat shading, no texture. For 3D meshes projected by the caller.
    void colorQuad(const float xs[4], const float ys[4], const Rgba cols[4], Blend blend = Blend::Over, bool emissive = false);
    // Connected line through n points, `width` design units wide, one colour.
    void polyline(const float* xs, const float* ys, int n, float width, Rgba c, Blend blend = Blend::Over, bool emissive = false);
    void text(TextStyle style, std::string_view utf8, float x, float baseline, Rgba c, Align align = Align::Left,
              TextFx fx = TextFx::Plain);
    [[nodiscard]] float textWidth(TextStyle style, std::string_view utf8) const;
    [[nodiscard]] float capHeight(TextStyle style) const;

    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t textureBytes() const noexcept { return budget_.used(); }
    [[nodiscard]] std::uint64_t targetBytes() const noexcept { return targetBytes_; }
    [[nodiscard]] int  samples() const noexcept { return samples_; }
    [[nodiscard]] int  maxSamples() const noexcept { return maxSamples_; }
    [[nodiscard]] const std::string& glVersion() const noexcept { return version_; }
    [[nodiscard]] const std::string& glRenderer() const noexcept { return rendererName_; }
    // While on, only emissive quads are recorded, and only into the bloom buffer (used to
    // give emissive parts of the cached static layer their glow every frame).
    void setBloomOnly(bool on) noexcept { bloomOnly_ = on; }
    [[nodiscard]] float pixelSize() const noexcept { return 1.0f / canvas_.scale; }   // one window pixel in design units

private:
    struct Vertex {
        float x, y;            // framebuffer pixels
        float u, v;            // texture coords
        float lx, ly;          // local coords (pixels) for analytic shapes
        float r, g, b, a;      // linear premultiplied
        float mode, tex, p0, p1;
        float q0, q1, q2, q3;
    };
    struct Target {
        unsigned int fbo = 0, tex = 0, rb = 0;
        int w = 0, h = 0;
    };

    void quad(const float xs[4], const float ys[4], const float us[4], const float vs[4], const float lx[4], const float ly[4],
              const float color[4], float mode, float tex, const float p[2], const float q[4], bool emissive);
    void rectQuad(Rect px, float u0, float v0, float u1, float v1, const float color[4], float mode, float tex,
                  const float p[2], const float q[4], bool emissive, float halfW = 0, float halfH = 0);
    void premul(Rgba c, Blend b, float out[4]) const;
    void spriteUv(Sprite s, float fx0, float fy0, float fx1, float fy1, float& u0, float& v0, float& u1, float& v1) const;
    void flushTo(std::vector<Vertex>& verts);
    void beginPass(const Canvas& canvas);
    void drawFullscreen(unsigned int program, int mode);
    bool makeTarget(Target& t, int w, int h, unsigned int internalFormat, unsigned int format, unsigned int type, bool texture);
    void freeTarget(Target& t) noexcept;
    [[nodiscard]] float px(float x) const noexcept { return canvas_.offsetX + x * canvas_.scale; }
    [[nodiscard]] float py(float y) const noexcept { return canvas_.offsetY + y * canvas_.scale; }

    unsigned int program_ = 0, post_ = 0, vao_ = 0, vbo_ = 0, emptyVao_ = 0;
    unsigned int chassisTex_ = 0, partsTex_ = 0, fontTex_ = 0, panelTex_ = 0, atlasTex_ = 0;
    std::uint64_t chassisBytes_ = 0, partsBytes_ = 0, fontBytes_ = 0, panelBytes_ = 0, atlasBytes_ = 0, targetBytes_ = 0;
    float chassisW_ = 1, chassisH_ = 1;
    int   uScale_ = -1, uPostMode_ = -1, uPostTexel_ = -1, uPostStrength_ = -1;
    Target msaa_, static_, scene_, bloomA_, bloomB_, wideA_, wideB_, capture_;
    int    fbW_ = 0, fbH_ = 0, samples_ = 1, maxSamples_ = 1;
    std::vector<Vertex> verts_, bloomVerts_;
    Canvas         canvas_;
    TextureBudget  budget_;
    RenderStats    stats_;
    PartsAtlas     parts_;
    const FontAtlas* font_ = nullptr;
    bool           inStatic_ = false;
    bool           bloomOnly_ = false;
    std::string    version_, rendererName_;
};

} // namespace hc::ui
