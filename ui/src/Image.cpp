#include "hybrid/ui/Image.hpp"

#include <stb_dxt.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace hc::ui {

Tier chooseTier(std::uint32_t w, std::uint32_t h) noexcept {
    return (w > kTier1x.width || h > kTier1x.height) ? kTier2x : kTier1x;
}

std::uint64_t CompressedTexture::bytes() const noexcept {
    std::uint64_t n = 0;
    for (const auto& l : levels) n += l.size();
    return n;
}

std::optional<Image> loadImageFile(const std::filesystem::path& file, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Image> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("cannot open " + file.string());
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return fail("empty file");

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp))
        return fail(std::string("unsupported image: ") + stbi_failure_reason());
    if (w <= 0 || h <= 0 || static_cast<std::uint32_t>(w) > kMaxDecodeSide || static_cast<std::uint32_t>(h) > kMaxDecodeSide)
        return fail("image dimensions out of range");

    unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) return fail(std::string("decode failed: ") + stbi_failure_reason());
    Image img;
    img.width  = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return img;
}

bool writePng(const std::filesystem::path& file, const Image& image) {
    if (image.empty()) return false;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    auto sink = [](void* ctx, void* data, int size) {
        static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size);
    };
    const int ok = stbi_write_png_to_func(sink, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                                          image.rgba.data(), static_cast<int>(image.width * 4));
    return ok != 0 && static_cast<bool>(out);
}

Image resizeSrgb(const Image& src, std::uint32_t width, std::uint32_t height) {
    Image dst;
    if (src.empty() || width == 0 || height == 0) return dst;
    dst.width  = width;
    dst.height = height;
    dst.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    stbir_resize_uint8_srgb(src.rgba.data(), static_cast<int>(src.width), static_cast<int>(src.height),
                            static_cast<int>(src.width * 4), dst.rgba.data(), static_cast<int>(width),
                            static_cast<int>(height), static_cast<int>(width * 4), STBIR_RGBA);
    return dst;
}

Image coverResize(const Image& src, std::uint32_t width, std::uint32_t height) {
    if (src.empty() || width == 0 || height == 0) return {};
    const double scale = std::max(static_cast<double>(width) / src.width, static_cast<double>(height) / src.height);
    const auto   sw    = std::max<std::uint32_t>(width, static_cast<std::uint32_t>(std::ceil(src.width * scale)));
    const auto   sh    = std::max<std::uint32_t>(height, static_cast<std::uint32_t>(std::ceil(src.height * scale)));
    const Image  scaled = (sw == src.width && sh == src.height) ? src : resizeSrgb(src, sw, sh);
    if (sw == width && sh == height) return scaled;

    Image out;
    out.width  = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const std::uint32_t x0 = (sw - width) / 2;
    const std::uint32_t y0 = (sh - height) / 2;
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * width * 4,
                    scaled.rgba.data() + (static_cast<std::size_t>(y + y0) * sw + x0) * 4, static_cast<std::size_t>(width) * 4);
    return out;
}

std::vector<Image> buildMipChain(Image base) {
    std::vector<Image> mips;
    if (base.empty()) return mips;
    mips.push_back(std::move(base));
    while (mips.back().width > 1 || mips.back().height > 1) {
        const Image& prev = mips.back();
        const std::uint32_t w = std::max<std::uint32_t>(1, prev.width / 2);
        const std::uint32_t h = std::max<std::uint32_t>(1, prev.height / 2);
        mips.push_back(resizeSrgb(prev, w, h));
    }
    return mips;
}

CompressedTexture compressDxt5(const std::vector<Image>& mips) {
    CompressedTexture tex;
    if (mips.empty()) return tex;
    tex.width  = (mips[0].width + 3) & ~3u;
    tex.height = (mips[0].height + 3) & ~3u;
    for (const Image& m : mips) {
        const std::uint32_t bw = (m.width + 3) / 4, bh = (m.height + 3) / 4;
        std::vector<std::uint8_t> level(static_cast<std::size_t>(bw) * bh * 16);
        std::array<unsigned char, 64> block{};
        for (std::uint32_t by = 0; by < bh; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                for (std::uint32_t py = 0; py < 4; ++py)
                    for (std::uint32_t px = 0; px < 4; ++px) {
                        // Edge blocks repeat the last row/column (clamp) so padding never adds new colours.
                        const std::uint32_t sx = std::min(bx * 4 + px, m.width - 1);
                        const std::uint32_t sy = std::min(by * 4 + py, m.height - 1);
                        std::memcpy(&block[(py * 4 + px) * 4], &m.rgba[(static_cast<std::size_t>(sy) * m.width + sx) * 4], 4);
                    }
                stb_compress_dxt_block(&level[(static_cast<std::size_t>(by) * bw + bx) * 16], block.data(), 1, STB_DXT_HIGHQUAL);
            }
        }
        tex.levels.push_back(std::move(level));
    }
    return tex;
}

namespace {
void rgb565(std::uint16_t c, int& r, int& g, int& b) {
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
}
} // namespace

Image decompressDxt5(const std::uint8_t* blocks, std::uint32_t width, std::uint32_t height) {
    Image img;
    img.width  = width;
    img.height = height;
    img.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (std::uint32_t by = 0; by < bh; ++by)
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::uint8_t* b = blocks + (static_cast<std::size_t>(by) * bw + bx) * 16;
            // Alpha (BC4-style)
            int a[8];
            a[0] = b[0]; a[1] = b[1];
            if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
            else { for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
            std::uint64_t abits = 0;
            for (int i = 0; i < 6; ++i) abits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
            // Colour (BC1, 4-colour mode as used in DXT5)
            const std::uint16_t c0 = static_cast<std::uint16_t>(b[8] | (b[9] << 8));
            const std::uint16_t c1 = static_cast<std::uint16_t>(b[10] | (b[11] << 8));
            int col[4][3];
            rgb565(c0, col[0][0], col[0][1], col[0][2]);
            rgb565(c1, col[1][0], col[1][1], col[1][2]);
            for (int k = 0; k < 3; ++k) {
                col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
            }
            const std::uint32_t cbits = b[12] | (b[13] << 8) | (b[14] << 16) | (static_cast<std::uint32_t>(b[15]) << 24);
            for (std::uint32_t py = 0; py < 4; ++py)
                for (std::uint32_t px = 0; px < 4; ++px) {
                    const std::uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    const std::uint32_t i  = py * 4 + px;
                    const int           ci = (cbits >> (2 * i)) & 3;
                    const int           ai = static_cast<int>((abits >> (3 * i)) & 7);
                    std::uint8_t* d = &img.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                    d[0] = static_cast<std::uint8_t>(col[ci][0]);
                    d[1] = static_cast<std::uint8_t>(col[ci][1]);
                    d[2] = static_cast<std::uint8_t>(col[ci][2]);
                    d[3] = static_cast<std::uint8_t>(a[ai]);
                }
        }
    return img;
}

CompressedTexture bakeForTier(const Image& source, Tier tier) {
    Tier t = tier;
    t.width  = std::min(t.width, kMaxTextureSide);
    t.height = std::min(t.height, kMaxTextureSide);
    return compressDxt5(buildMipChain(coverResize(source, t.width, t.height)));
}

// ---- disk cache --------------------------------------------------------------------
namespace {
constexpr char          kMagic[4] = {'H', 'C', 'T', 'X'};
constexpr std::uint32_t kCacheVersion = 1;

template <typename T> void put(std::ofstream& o, T v) { o.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <typename T> bool get(std::ifstream& i, T& v) { return static_cast<bool>(i.read(reinterpret_cast<char*>(&v), sizeof(T))); }
} // namespace

bool saveTextureCache(const std::filesystem::path& file, const CompressedTexture& tex) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(kMagic, 4);
        put(o, kCacheVersion);
        put(o, tex.width);
        put(o, tex.height);
        put(o, static_cast<std::uint32_t>(tex.levels.size()));
        for (const auto& l : tex.levels) {
            put(o, static_cast<std::uint32_t>(l.size()));
            o.write(reinterpret_cast<const char*>(l.data()), static_cast<std::streamsize>(l.size()));
        }
        if (!o) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

std::optional<CompressedTexture> loadTextureCache(const std::filesystem::path& file) {
    std::ifstream i(file, std::ios::binary);
    if (!i) return std::nullopt;
    char magic[4];
    std::uint32_t ver = 0, levels = 0;
    CompressedTexture tex;
    if (!i.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return std::nullopt;
    if (!get(i, ver) || ver != kCacheVersion) return std::nullopt;
    if (!get(i, tex.width) || !get(i, tex.height) || !get(i, levels)) return std::nullopt;
    if (tex.width == 0 || tex.height == 0 || tex.width > kMaxTextureSide || tex.height > kMaxTextureSide || levels > 16)
        return std::nullopt;
    std::uint32_t w = tex.width, h = tex.height;
    for (std::uint32_t l = 0; l < levels; ++l) {
        std::uint32_t n = 0;
        if (!get(i, n)) return std::nullopt;
        const std::uint32_t expect = ((w + 3) / 4) * ((h + 3) / 4) * 16;
        if (n != expect) return std::nullopt;
        std::vector<std::uint8_t> data(n);
        if (!i.read(reinterpret_cast<char*>(data.data()), n)) return std::nullopt;
        tex.levels.push_back(std::move(data));
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }
    return tex;
}

std::string textureCacheKey(const std::filesystem::path& source, Tier tier) {
    std::error_code ec;
    const auto size  = std::filesystem::file_size(source, ec);
    const auto mtime = std::filesystem::last_write_time(source, ec).time_since_epoch().count();
    std::ostringstream ss;
    ss << source.generic_u8string().size() << ':';
    const auto u8 = source.generic_u8string();
    ss.write(reinterpret_cast<const char*>(u8.data()), static_cast<std::streamsize>(u8.size()));
    ss << ':' << size << ':' << mtime << ':' << tier.width << 'x' << tier.height;
    const std::string key = ss.str();
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool TextureBudget::reserve(std::uint64_t bytes) noexcept {
    if (used_ + bytes > limit_) return false;
    used_ += bytes;
    return true;
}

void TextureBudget::release(std::uint64_t bytes) noexcept { used_ = bytes > used_ ? 0 : used_ - bytes; }

} // namespace hc::ui
