#pragma once
// =============================================================================
//  FontAtlas — bakes a TrueType font (Latin-1 range) into one 8-bit coverage atlas
//  at a few fixed design sizes, rendered at the current UI scale so text stays crisp.
//  Pure CPU; the renderer uploads atlas() as a single-channel texture.
// =============================================================================
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace hc::ui {

enum class TextSize : std::uint8_t { Small = 0, Medium = 1, Large = 2 };
inline constexpr float kTextDesignPx[3] = {12.0f, 15.0f, 26.0f};

struct GlyphQuad {
    float x0, y0, x1, y1;   // design units
    float s0, t0, s1, t1;   // atlas UVs
};

class FontAtlas {
public:
    // Bakes at designPx * uiScale pixels into a 1024² atlas, or 2048² when that is too small
    // (high-DPI). Returns false if the font can't be parsed or doesn't fit either.
    bool bake(const std::vector<std::uint8_t>& ttf, float uiScale);

    [[nodiscard]] float measure(TextSize size, std::string_view utf8) const;
    void layout(TextSize size, std::string_view utf8, float x, float baselineY,
                const std::function<void(const GlyphQuad&)>& emit) const;

    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t atlasSize() const noexcept { return size_; }
    [[nodiscard]] float bakedScale() const noexcept { return scale_; }
    [[nodiscard]] bool  valid() const noexcept { return valid_; }

    // %WINDIR%\Fonts\segoeuib.ttf -> segoeui.ttf -> arial.ttf (Windows); DejaVu on other systems (tests).
    [[nodiscard]] static std::optional<std::vector<std::uint8_t>> loadSystemFont();

private:
    struct Packed {
        std::uint16_t x0, y0, x1, y1;
        float xoff, yoff, xadvance, xoff2, yoff2;
    };
    static constexpr int kFirst = 32, kCount = 224;   // U+0020 .. U+00FF
    std::vector<std::uint8_t> pixels_;
    std::vector<Packed>       glyphs_[3];
    bool  bakeAt(const std::vector<std::uint8_t>& ttf, std::uint32_t size);
    float         scale_ = 1.0f;
    std::uint32_t size_  = 1024;
    bool          valid_ = false;
};

// Decodes one UTF-8 code point; advances `i`. Invalid bytes yield '?'.
[[nodiscard]] std::uint32_t nextCodepoint(std::string_view s, std::size_t& i) noexcept;

} // namespace hc::ui
