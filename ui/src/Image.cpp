#include "hybrid/ui/Image.hpp"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace hc::ui {

namespace {
const std::array<float, 256>& decodeTable() {
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) a[static_cast<std::size_t>(i)] = srgbToLinear(static_cast<float>(i) / 255.0f);
        return a;
    }();
    return t;
}
} // namespace

float srgbToLinear(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float l) noexcept {
    l = std::clamp(l, 0.0f, 1.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
std::uint8_t linearToSrgb8(float l) noexcept { return static_cast<std::uint8_t>(linearToSrgb(l) * 255.0f + 0.5f); }
float        srgb8ToLinear(std::uint8_t v) noexcept { return decodeTable()[v]; }

std::uint64_t TextureLevels::bytes() const noexcept {
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

Image premultiply(const Image& s) {
    Image d = s;
    for (std::size_t i = 0; i < d.rgba.size(); i += 4) {
        const std::uint8_t a8 = d.rgba[i + 3];
        if (a8 == 255) continue;
        const float a = static_cast<float>(a8) / 255.0f;
        for (int c = 0; c < 3; ++c) d.rgba[i + c] = linearToSrgb8(srgb8ToLinear(d.rgba[i + c]) * a);
    }
    return d;
}

TextureLevels buildMips(Image base) {
    TextureLevels t;
    if (base.empty()) return t;
    t.width  = base.width;
    t.height = base.height;
    const auto& lut = decodeTable();
    std::uint32_t w = base.width, h = base.height;
    t.levels.push_back(std::move(base.rgba));
    while (w > 1 || h > 1) {
        const std::vector<std::uint8_t>& src = t.levels.back();
        const std::uint32_t nw = std::max<std::uint32_t>(1, w / 2), nh = std::max<std::uint32_t>(1, h / 2);
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) {
            const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const std::uint8_t* p[4] = {&src[(static_cast<std::size_t>(y0) * w + x0) * 4], &src[(static_cast<std::size_t>(y0) * w + x1) * 4],
                                            &src[(static_cast<std::size_t>(y1) * w + x0) * 4], &src[(static_cast<std::size_t>(y1) * w + x1) * 4]};
                std::uint8_t* o = &dst[(static_cast<std::size_t>(y) * nw + x) * 4];
                for (int c = 0; c < 3; ++c)
                    o[c] = linearToSrgb8(0.25f * (lut[p[0][c]] + lut[p[1][c]] + lut[p[2][c]] + lut[p[3][c]]));
                o[3] = static_cast<std::uint8_t>((p[0][3] + p[1][3] + p[2][3] + p[3][3] + 2) / 4);
            }
        }
        t.levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return t;
}

Image crop(const Image& src, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    Image out;
    if (src.empty() || x >= src.width || y >= src.height) return out;
    w = std::min(w, src.width - x);
    h = std::min(h, src.height - y);
    out.width  = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t r = 0; r < h; ++r)
        std::memcpy(&out.rgba[static_cast<std::size_t>(r) * w * 4], src.at(x, y + r), static_cast<std::size_t>(w) * 4);
    return out;
}

bool TextureBudget::reserve(std::uint64_t bytes) noexcept {
    if (used_ + bytes > limit_) return false;
    used_ += bytes;
    return true;
}

void TextureBudget::release(std::uint64_t bytes) noexcept { used_ = bytes > used_ ? 0 : used_ - bytes; }

} // namespace hc::ui
