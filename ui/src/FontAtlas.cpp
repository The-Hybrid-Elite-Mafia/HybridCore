#include "hybrid/ui/FontAtlas.hpp"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace hc::ui {

namespace {
constexpr std::uint32_t kFirst = 32, kLatin1Count = 224, kAsciiCount = 95;
const char* const kFaceFiles[kFaceCount] = {"Poppins-Medium.ttf", "Poppins-SemiBold.ttf", "Poppins-Bold.ttf"};
constexpr int kPad = FontAtlas::kBlurRadius + 2;

const stbtt_fontinfo* info(const std::vector<std::uint8_t>& blob) { return reinterpret_cast<const stbtt_fontinfo*>(blob.data()); }

// Separable Gaussian blur of channel 0 into channel 1 (sigma 1.6 px, radius kBlurRadius).
void blurIntoG(std::vector<std::uint8_t>& rg, std::uint32_t n) {
    constexpr int R = FontAtlas::kBlurRadius;
    float k[2 * R + 1];
    float sum = 0;
    for (int i = -R; i <= R; ++i) { k[i + R] = std::exp(-0.5f * (i * i) / (1.6f * 1.6f)); sum += k[i + R]; }
    for (float& v : k) v /= sum;
    std::vector<float> tmp(static_cast<std::size_t>(n) * n);
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            float acc = 0;
            for (int i = -R; i <= R; ++i) {
                const int xx = std::clamp(static_cast<int>(x) + i, 0, static_cast<int>(n) - 1);
                acc += k[i + R] * rg[(static_cast<std::size_t>(y) * n + xx) * 2];
            }
            tmp[static_cast<std::size_t>(y) * n + x] = acc;
        }
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            float acc = 0;
            for (int i = -R; i <= R; ++i) {
                const int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(n) - 1);
                acc += k[i + R] * tmp[static_cast<std::size_t>(yy) * n + x];
            }
            rg[(static_cast<std::size_t>(y) * n + x) * 2 + 1] = static_cast<std::uint8_t>(std::min(255.0f, acc + 0.5f));
        }
}
} // namespace

std::uint32_t nextCodepoint(std::string_view s, std::size_t& i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i++]);
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && i < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[i]);
        if ((b1 & 0xC0) == 0x80) { ++i; return ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu); }
    }
    // Longer sequences are outside the baked range: skip continuation bytes.
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return '?';
}

std::optional<FontSet> FontSet::load(const std::filesystem::path& dir, std::string* error) {
    FontSet f;
    for (std::size_t i = 0; i < kFaceCount; ++i) {
        std::ifstream in(dir / kFaceFiles[i], std::ios::binary);
        if (in) f.ttf[i].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (f.ttf[i].empty()) {
            if (error) *error = "missing font " + (dir / kFaceFiles[i]).string();
            return std::nullopt;
        }
    }
    return f;
}

FontAtlas::FontAtlas(const FontAtlas& o) : fonts_(o.fonts_), styles_(o.styles_), pixels_(o.pixels_), scale_(o.scale_), minPx_(o.minPx_), size_(o.size_), valid_(o.valid_) {
    initInfos();
}

FontAtlas& FontAtlas::operator=(const FontAtlas& o) {
    if (this != &o) {
        fonts_ = o.fonts_; styles_ = o.styles_; pixels_ = o.pixels_; scale_ = o.scale_; minPx_ = o.minPx_; size_ = o.size_; valid_ = o.valid_;
        initInfos();
    }
    return *this;
}

bool FontAtlas::initInfos() {
    for (std::size_t f = 0; f < kFaceCount; ++f) {
        infoStorage_[f].assign(sizeof(stbtt_fontinfo), 0);
        if (fonts_.ttf[f].empty()) return false;
        auto* fi = reinterpret_cast<stbtt_fontinfo*>(infoStorage_[f].data());
        if (!stbtt_InitFont(fi, fonts_.ttf[f].data(), stbtt_GetFontOffsetForIndex(fonts_.ttf[f].data(), 0))) return false;
    }
    return true;
}

bool FontAtlas::bake(const FontSet& fonts, float canvasScale, float minPixels) {
    valid_ = false;
    if (!(canvasScale > 0.0f)) return false;
    fonts_ = fonts;
    scale_ = canvasScale;
    minPx_ = std::max(6.0f, minPixels);
    if (!initInfos()) return false;
    for (std::uint32_t size : {512u, 1024u, 2048u, 4096u})
        if (bakeAt(size)) { valid_ = true; return true; }
    return false;
}

bool FontAtlas::bakeAt(std::uint32_t atlas) {
    size_ = atlas;
    std::vector<std::uint8_t> cov(static_cast<std::size_t>(atlas) * atlas, 0);
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, cov.data(), static_cast<int>(atlas), static_cast<int>(atlas), 0, kPad, nullptr)) return false;

    std::array<std::vector<stbtt_packedchar>, kTextStyleCount> packed;
    bool ok = true;
    for (std::size_t s = 0; s < kTextStyleCount && ok; ++s) {
        const TextStyleSpec& spec = kTextStyles[s];
        const float px = s == static_cast<std::size_t>(TextStyle::Micro) ? std::max(9.0f, spec.em * scale_) : std::max(minPx_, spec.em * scale_);
        stbtt_PackSetOversampling(&pc, px < 24.0f ? 2 : 1, 1);
        const std::uint32_t count = spec.latin1 ? kLatin1Count : kAsciiCount;
        packed[s].assign(count, stbtt_packedchar{});
        stbtt_pack_range r{};
        r.font_size                        = STBTT_POINT_SIZE(px);
        r.first_unicode_codepoint_in_range = static_cast<int>(kFirst);
        r.num_chars                        = static_cast<int>(count);
        r.chardata_for_range               = packed[s].data();
        const auto& ttf = fonts_.ttf[static_cast<std::size_t>(spec.face)];
        ok = stbtt_PackFontRanges(&pc, ttf.data(), 0, &r, 1) != 0;

        const stbtt_fontinfo* fi = info(infoStorage_[static_cast<std::size_t>(spec.face)]);
        StyleData& sd = styles_[s];
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1);
        sd.capPx = static_cast<float>(y1) * stbtt_ScaleForMappingEmToPixels(fi, px);
        sd.glyphs.resize(count);
        for (std::uint32_t g = 0; g < count; ++g) {
            const auto& p = packed[s][g];
            sd.glyphs[g] = {p.x0, p.y0, p.x1, p.y1, p.xoff, p.yoff, p.xadvance, p.xoff2, p.yoff2};
        }
    }
    stbtt_PackEnd(&pc);
    if (!ok) return false;

    pixels_.assign(static_cast<std::size_t>(atlas) * atlas * 2, 0);
    for (std::size_t i = 0; i < cov.size(); ++i) pixels_[i * 2] = cov[i];
    blurIntoG(pixels_, atlas);
    return true;
}

const FontAtlas::Packed* FontAtlas::glyph(std::size_t style, std::uint32_t cp) const noexcept {
    const auto& g = styles_[style].glyphs;
    if (cp < kFirst || cp - kFirst >= g.size()) cp = '?';
    return &g[cp - kFirst];
}

float FontAtlas::measure(TextStyle style, std::string_view s) const {
    if (!valid_) return 0.0f;
    const auto st = static_cast<std::size_t>(style);
    float w = 0.0f;
    for (std::size_t i = 0; i < s.size();) w += glyph(st, nextCodepoint(s, i))->xadvance;
    return w / scale_;
}

float FontAtlas::capHeight(TextStyle style) const noexcept {
    return valid_ ? styles_[static_cast<std::size_t>(style)].capPx / scale_ : kTextStyles[static_cast<std::size_t>(style)].em * 0.7f;
}

void FontAtlas::layout(TextStyle style, std::string_view s, float x, float baselineY, float pad,
                       const std::function<void(const GlyphQuad&)>& emit) const {
    if (!valid_) return;
    const auto st = static_cast<std::size_t>(style);
    const float inv = 1.0f / static_cast<float>(size_);
    const float padPx = pad * scale_;
    float pen = x * scale_;
    const float base = baselineY * scale_;
    for (std::size_t i = 0; i < s.size();) {
        const std::uint32_t cp = nextCodepoint(s, i);
        const Packed& p = *glyph(st, cp);
        // Spaces draw nothing: a padded quad around their empty cell would pick up the blurred
        // edge of the next glyph in the atlas (thin bars between words in neon text).
        if (cp != 0x20 && cp != 0xA0 && p.x1 > p.x0 && p.y1 > p.y0) {
            // Pixel-snap the pen so small text stays sharp.
            const float px0 = std::round(pen + p.xoff), py0 = std::round(base + p.yoff);
            const float w = p.xoff2 - p.xoff, h = p.yoff2 - p.yoff;
            // The blur reaches kBlurRadius TEXELS around each glyph; with horizontal oversampling a
            // texel is narrower than a screen pixel, so the padding is limited in texels (beyond it
            // the quad would sample the neighbouring glyph: thin bars beside neon text).
            const float rx = static_cast<float>(p.x1 - p.x0) / std::max(w, 1e-3f), ry = static_cast<float>(p.y1 - p.y0) / std::max(h, 1e-3f);
            const float tp = std::min(padPx * rx, static_cast<float>(kBlurRadius));
            const float tq = std::min(padPx * ry, static_cast<float>(kBlurRadius));
            const float qpx = rx > 0 ? tp / rx : 0.0f, qpy = ry > 0 ? tq / ry : 0.0f;
            GlyphQuad q;
            q.x0 = (px0 - qpx) / scale_;
            q.y0 = (py0 - qpy) / scale_;
            q.x1 = (px0 + w + qpx) / scale_;
            q.y1 = (py0 + h + qpy) / scale_;
            q.s0 = (p.x0 - tp) * inv; q.t0 = (p.y0 - tq) * inv;
            q.s1 = (p.x1 + tp) * inv; q.t1 = (p.y1 + tq) * inv;
            emit(q);
        }
        pen += p.xadvance;
    }
}

} // namespace hc::ui
