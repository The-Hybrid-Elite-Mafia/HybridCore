#pragma once
// =============================================================================
//  hc::ui image pipeline (CPU side, no GPU calls).
//
//  Everything the GPU receives is RGBA8 in sRGB encoding with PREMULTIPLIED alpha
//  (premultiplied in linear light, then sRGB-encoded) plus a full mip chain built
//  in linear light. Uploaded as GL_SRGB8_ALPHA8, the GPU decodes to linear, filters
//  trilinearly and blends in linear light.
//
//  Rule: masters (4K faceplate, 8K skins) stay in RAM only; the GPU receives a copy
//  scaled to the window (see Faceplate.hpp), never wider or taller than kMaxTextureSide,
//  and every upload is charged against the UI VRAM budget.
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hc::ui {

inline constexpr std::uint32_t kMaxTextureSide  = 4096;                 // hard cap for anything uploaded
inline constexpr std::uint64_t kVramBudgetBytes = 64ull * 1024 * 1024;  // all UI textures together
inline constexpr std::uint32_t kMaxDecodeSide   = 16384;                // refuse absurd source files

struct Image {
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> rgba;     // 8-bit sRGB, row-major (straight or premultiplied: see function docs)
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const noexcept {
        return &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
    }
};

// sRGB8 premultiplied RGBA, level 0 first, each level half the previous (min 1).
struct TextureLevels {
    std::uint32_t                          width  = 0;
    std::uint32_t                          height = 0;
    std::vector<std::vector<std::uint8_t>> levels;
    [[nodiscard]] std::uint64_t bytes() const noexcept;
};

// sRGB <-> linear (exact IEC 61966-2-1 curves).
[[nodiscard]] float srgbToLinear(float s) noexcept;
[[nodiscard]] float linearToSrgb(float l) noexcept;
[[nodiscard]] std::uint8_t linearToSrgb8(float l) noexcept;
[[nodiscard]] float srgb8ToLinear(std::uint8_t v) noexcept;   // table lookup

// Decoding / encoding ------------------------------------------------------------
[[nodiscard]] std::optional<Image> loadImageFile(const std::filesystem::path& file, std::string* error = nullptr);
bool writePng(const std::filesystem::path& file, const Image& image);

// Scaling of straight-alpha sRGB images, filtered in linear light.
[[nodiscard]] Image resizeSrgb(const Image& src, std::uint32_t width, std::uint32_t height);

// Straight sRGB -> premultiplied sRGB (alpha applied in linear light).
[[nodiscard]] Image premultiply(const Image& straight);

// Premultiplied sRGB level 0 -> full mip chain (linear-light box filter).
[[nodiscard]] TextureLevels buildMips(Image premultipliedLevel0);

// Copies a sub-rectangle (clamped to the image).
[[nodiscard]] Image crop(const Image& src, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);

// VRAM accounting -------------------------------------------------------------------
class TextureBudget {
public:
    explicit TextureBudget(std::uint64_t limit = kVramBudgetBytes) noexcept : limit_(limit) {}
    [[nodiscard]] bool reserve(std::uint64_t bytes) noexcept;   // false if it would exceed the limit
    void release(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t used() const noexcept { return used_; }
    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

private:
    std::uint64_t limit_;
    std::uint64_t used_ = 0;
};

} // namespace hc::ui
