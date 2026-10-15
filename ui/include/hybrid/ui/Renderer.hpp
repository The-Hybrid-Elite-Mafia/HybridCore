#pragma once
// =============================================================================
//  Renderer — batched OpenGL 3.3 2D renderer for the skeuomorphic UI.
//  Everything is drawn by one shader: textured faceplate, signed-distance-field
//  circles / rounded rects / arcs (crisp at any size), additive neon glow, text.
//  The CPU only sends vertices (a few hundred quads per frame) — no pixel work.
//  All coordinates are DESIGN units; the Viewport maps them to framebuffer pixels.
// =============================================================================
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hc::ui {

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

enum class Align : std::uint8_t { Left, Center, Right };

struct RenderStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t quads     = 0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Requires a current GL 3.3 context.
    bool init(std::string* error);
    void shutdown() noexcept;

    // Uploads a baked faceplate. Uses DXT5 directly when the driver supports S3TC,
    // otherwise decompresses to RGBA8. Refuses anything over kMaxTextureSide or the VRAM budget.
    bool setFaceplate(const CompressedTexture& tex, std::string* error);
    bool setFont(const FontAtlas& font, std::string* error);

    void begin(int framebufferW, int framebufferH, const Viewport& vp, Rgba clear);
    void end();

    // --- primitives (design units) ---
    void faceplate(Rect r, Rgba tint = {});
    void rect(Rect r, Rgba c);
    void roundRect(Rect r, float radius, Rgba c);
    void roundRectRotated(float cx, float cy, float w, float h, float radius, float angleRad, Rgba c);
    void circle(float cx, float cy, float radius, Rgba c, float shading = 0.0f);
    void arc(float cx, float cy, float radius, float thickness, float a0, float a1, Rgba c);   // radians, 0 = up, clockwise
    void glow(Rect core, float spread, Rgba c);    // additive neon halo around `core`
    void text(TextSize size, std::string_view utf8, float x, float baseline, Rgba c, Align align = Align::Left);
    [[nodiscard]] float textWidth(TextSize size, std::string_view utf8) const;

    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t vramBytes() const noexcept { return budget_.used(); }
    [[nodiscard]] bool  s3tcSupported() const noexcept { return s3tc_; }
    [[nodiscard]] const std::string& glVersion() const noexcept { return version_; }
    [[nodiscard]] const std::string& glRenderer() const noexcept { return rendererName_; }

private:
    struct Vertex {
        float x, y;          // framebuffer pixels
        float u, v;          // texture coords
        float lx, ly;        // local SDF coords (pixels)
        std::uint8_t r, g, b, a;
        float p0, p1, p2, p3;   // mode, params
        float q0, q1, q2, q3;   // more params
    };
    enum class Blend : std::uint8_t { Alpha, Additive };

    void quad(const float xs[4], const float ys[4], const float us[4], const float vs[4], const float lxs[4],
              const float lys[4], Rgba c, float mode, const float p[3], const float q[4], Blend blend);
    void axisQuad(Rect px, float u0, float v0, float u1, float v1, Rgba c, float mode, const float p[3],
                  const float q[4], Blend blend, float localHalfW, float localHalfH);
    void flush();
    [[nodiscard]] float sx(float x) const noexcept { return vp_.offsetX + x * vp_.scale; }
    [[nodiscard]] float sy(float y) const noexcept { return vp_.offsetY + y * vp_.scale; }

    unsigned int program_ = 0, vao_ = 0, vbo_ = 0, faceTex_ = 0, fontTex_ = 0;
    int   uScale_ = -1;
    std::uint64_t faceBytes_ = 0, fontBytes_ = 0;
    std::vector<Vertex> verts_;
    Blend        blend_ = Blend::Alpha;
    Viewport     vp_;
    int          fbW_ = 1, fbH_ = 1;
    TextureBudget budget_;
    RenderStats  stats_;
    const FontAtlas* font_ = nullptr;
    bool         s3tc_ = false;
    std::string  version_, rendererName_;
};

} // namespace hc::ui
