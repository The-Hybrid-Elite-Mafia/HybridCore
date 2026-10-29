#pragma once
// =============================================================================
//  FontAtlas — Poppins (SIL OFL 1.1, bundled in assets/fonts) baked into ONE
//  two-channel atlas at the current canvas scale, so text is pixel-crisp at any size:
//    R = glyph coverage, G = the same coverage blurred (soft shadows, engraving, glow).
//  Glyph advances only: Poppins keeps its kerning in a GPOS layout stb_truetype can't
//  read, so pairs like "AV" are not tightened. Pure CPU; the renderer uploads pixels() as RG8.
// =============================================================================
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hc::ui {

enum class Face : std::uint8_t { Medium, SemiBold, Bold };
inline constexpr std::size_t kFaceCount = 3;

enum class TextStyle : std::uint8_t {
    Caption,      // tiny captions inside caps
    Label,        // TRIM / PAN / A1
    Value,        // live values on strips
    Title,        // strip titles
    Button,       // M S Ø
    Tab,          // tab captions
    Panel,        // name panel, large
    Sub,          // name panel, second line
    Heading,      // top bar title
    Status,       // bottom bar
    Display,      // big neon digits in the glass display
    DisplayCap,   // captions in the glass display
    Micro,        // smallest readable print (table cells, hints)
    Small,        // small labels
    SmallBold,    // small headings, buttons in dense rows
    Body,         // running text
    BodyBold,     // emphasised running text
};
inline constexpr std::size_t kTextStyleCount = 17;

struct TextStyleSpec {
    Face  face;
    float em;          // em size in design units (3840-wide canvas)
    bool  latin1;      // true: U+0020..U+00FF, false: printable ASCII only
};
inline constexpr std::array<TextStyleSpec, kTextStyleCount> kTextStyles{{
    {Face::SemiBold, 24.0f, false},   // Caption
    {Face::SemiBold, 30.0f, false},   // Label
    {Face::SemiBold, 28.0f, true},    // Value
    {Face::SemiBold, 34.0f, true},    // Title
    {Face::Bold, 34.0f, true},        // Button
    {Face::SemiBold, 40.0f, false},   // Tab
    {Face::Bold, 52.0f, true},        // Panel
    {Face::Medium, 28.0f, true},      // Sub
    {Face::Bold, 56.0f, false},       // Heading
    {Face::Medium, 32.0f, true},      // Status
    {Face::Bold, 96.0f, false},       // Display
    {Face::SemiBold, 26.0f, false},   // DisplayCap
    {Face::Medium, 18.0f, true},      // Micro
    {Face::SemiBold, 20.0f, true},    // Small
    {Face::Bold, 22.0f, true},        // SmallBold
    {Face::Medium, 24.0f, true},      // Body
    {Face::Bold, 26.0f, true},        // BodyBold
}};

struct FontSet {
    std::array<std::vector<std::uint8_t>, kFaceCount> ttf;
    // Loads Poppins-Medium/SemiBold/Bold.ttf from `dir`.
    [[nodiscard]] static std::optional<FontSet> load(const std::filesystem::path& dir, std::string* error);
};

struct GlyphQuad {
    float x0, y0, x1, y1;   // design units
    float s0, t0, s1, t1;   // atlas UVs
};

class FontAtlas {
public:
    FontAtlas() = default;
    FontAtlas(const FontAtlas& other);              // re-binds the font parsers to the copied bytes
    FontAtlas& operator=(const FontAtlas& other);

    // Bakes every style at em × canvasScale pixels into the smallest square atlas (512..4096)
    // that fits. Returns false if a font can't be parsed or nothing fits.
    bool bake(const FontSet& fonts, float canvasScale);

    [[nodiscard]] float measure(TextStyle style, std::string_view utf8) const;   // design units
    [[nodiscard]] float capHeight(TextStyle style) const noexcept;                // design units
    // Emits one quad per visible glyph; `pad` grows each quad (design units) for the blur channel.
    void layout(TextStyle style, std::string_view utf8, float x, float baselineY, float pad,
                const std::function<void(const GlyphQuad&)>& emit) const;

    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }   // RG8
    [[nodiscard]] std::uint32_t atlasSize() const noexcept { return size_; }
    [[nodiscard]] float bakedScale() const noexcept { return scale_; }
    [[nodiscard]] bool  valid() const noexcept { return valid_; }
    // Blur reach of the G channel in atlas pixels (glyph quads must grow by this much to show it).
    static constexpr int kBlurRadius = 4;

private:
    struct Packed {
        std::uint16_t x0, y0, x1, y1;
        float xoff, yoff, xadvance, xoff2, yoff2;
    };
    struct StyleData {
        std::vector<Packed> glyphs;     // indexed by codepoint - 32
        float capPx    = 0;
    };
    bool bakeAt(std::uint32_t size);
    bool initInfos();
    [[nodiscard]] const Packed* glyph(std::size_t style, std::uint32_t cp) const noexcept;

    FontSet                                        fonts_;
    std::array<std::vector<std::uint8_t>, kFaceCount> infoStorage_;   // stbtt_fontinfo blobs
    std::array<StyleData, kTextStyleCount>         styles_;
    std::vector<std::uint8_t>                      pixels_;
    float         scale_ = 1.0f;
    std::uint32_t size_  = 0;
    bool          valid_ = false;
};

// Decodes one UTF-8 code point; advances `i`. Invalid bytes yield '?'.
[[nodiscard]] std::uint32_t nextCodepoint(std::string_view s, std::size_t& i) noexcept;

} // namespace hc::ui
