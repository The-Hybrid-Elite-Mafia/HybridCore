#pragma once
// =============================================================================
//  hc::ui image pipeline (CPU side, no GPU calls).
//
//  Rule: full-size masters (8K+) are decoded in RAM only long enough to be scaled
//  to a screen tier; the GPU only ever receives tier-sized, mipmapped, DXT5
//  (BC3) compressed data — 1 byte per pixel — and never anything wider or taller
//  than kMaxTextureSide. Baked results are cached on disk so masters are decoded
//  once per tier.
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hc::ui {

inline constexpr std::uint32_t kMaxTextureSide = 4096;                 // hard cap for anything uploaded
inline constexpr std::uint64_t kVramBudgetBytes = 64ull * 1024 * 1024;  // all UI textures together
inline constexpr std::uint32_t kMaxDecodeSide  = 16384;                // refuse absurd source files

struct Image {
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> rgba;     // 8-bit sRGB, straight alpha, row-major
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

struct Tier {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};
// 1x = 1676×940 (design 1673×940 padded to a multiple of 4), 2x = 3348×1880.
inline constexpr Tier kTier1x{1676, 940};
inline constexpr Tier kTier2x{3348, 1880};
[[nodiscard]] Tier chooseTier(std::uint32_t windowPixelWidth, std::uint32_t windowPixelHeight) noexcept;

struct CompressedTexture {
    std::uint32_t                          width  = 0;   // level 0, multiple of 4
    std::uint32_t                          height = 0;
    std::vector<std::vector<std::uint8_t>> levels;       // DXT5 blocks per mip level
    [[nodiscard]] std::uint64_t bytes() const noexcept;
};

// Decoding / encoding ------------------------------------------------------------
[[nodiscard]] std::optional<Image> loadImageFile(const std::filesystem::path& file, std::string* error = nullptr);
bool writePng(const std::filesystem::path& file, const Image& image);

// Scaling (sRGB-correct) ----------------------------------------------------------
[[nodiscard]] Image resizeSrgb(const Image& src, std::uint32_t width, std::uint32_t height);
// Scale to cover width×height keeping aspect ratio, then centre-crop to exactly that size.
[[nodiscard]] Image coverResize(const Image& src, std::uint32_t width, std::uint32_t height);
[[nodiscard]] std::vector<Image> buildMipChain(Image base);

// Block compression ----------------------------------------------------------------
[[nodiscard]] CompressedTexture compressDxt5(const std::vector<Image>& mips);
[[nodiscard]] Image decompressDxt5(const std::uint8_t* blocks, std::uint32_t width, std::uint32_t height);

// Full bake: any-size image -> tier-sized, mipmapped, DXT5 texture (dimensions <= kMaxTextureSide).
[[nodiscard]] CompressedTexture bakeForTier(const Image& source, Tier tier);

// Disk cache ("HCTX" files) ----------------------------------------------------------
bool saveTextureCache(const std::filesystem::path& file, const CompressedTexture& tex);
[[nodiscard]] std::optional<CompressedTexture> loadTextureCache(const std::filesystem::path& file);
// Stable key from file path, size, modification time and tier.
[[nodiscard]] std::string textureCacheKey(const std::filesystem::path& source, Tier tier);

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
