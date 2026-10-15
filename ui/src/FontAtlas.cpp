#include "hybrid/ui/FontAtlas.hpp"

#include <stb_truetype.h>

#include <algorithm>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace hc::ui {

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

bool FontAtlas::bake(const std::vector<std::uint8_t>& ttf, float uiScale) {
    valid_ = false;
    if (ttf.empty() || !(uiScale > 0.0f)) return false;
    scale_ = std::min(uiScale, 3.0f);
    for (std::uint32_t size : {1024u, 2048u})
        if (bakeAt(ttf, size)) { valid_ = true; return true; }
    return false;
}

bool FontAtlas::bakeAt(const std::vector<std::uint8_t>& ttf, std::uint32_t atlas) {
    size_ = atlas;
    pixels_.assign(static_cast<std::size_t>(atlas) * atlas, 0);

    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, pixels_.data(), static_cast<int>(atlas), static_cast<int>(atlas), 0, 2, nullptr)) return false;
    stbtt_PackSetOversampling(&pc, 2, 1);   // horizontal oversampling: sharper small text

    std::vector<stbtt_packedchar> packed[3];
    stbtt_pack_range ranges[3];
    for (int k = 0; k < 3; ++k) {
        packed[k].resize(kCount);
        ranges[k] = {};
        ranges[k].font_size                        = kTextDesignPx[k] * scale_;
        ranges[k].first_unicode_codepoint_in_range = kFirst;
        ranges[k].num_chars                        = kCount;
        ranges[k].chardata_for_range               = packed[k].data();
    }
    const int ok = stbtt_PackFontRanges(&pc, ttf.data(), 0, ranges, 3);
    stbtt_PackEnd(&pc);
    if (!ok) return false;

    for (int k = 0; k < 3; ++k) {
        glyphs_[k].resize(kCount);
        for (int g = 0; g < kCount; ++g) {
            const auto& p = packed[k][static_cast<std::size_t>(g)];
            glyphs_[k][static_cast<std::size_t>(g)] = {p.x0, p.y0, p.x1, p.y1, p.xoff, p.yoff, p.xadvance, p.xoff2, p.yoff2};
        }
    }
    return true;
}

float FontAtlas::measure(TextSize size, std::string_view s) const {
    if (!valid_) return 0.0f;
    const auto& g = glyphs_[static_cast<int>(size)];
    float w = 0.0f;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = nextCodepoint(s, i);
        if (cp < kFirst || cp >= kFirst + kCount) cp = '?';
        w += g[cp - kFirst].xadvance;
    }
    return w / scale_;
}

void FontAtlas::layout(TextSize size, std::string_view s, float x, float baselineY,
                       const std::function<void(const GlyphQuad&)>& emit) const {
    if (!valid_) return;
    const auto& g = glyphs_[static_cast<int>(size)];
    const float inv = 1.0f / static_cast<float>(size_);
    float pen = x * scale_;
    const float base = baselineY * scale_;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = nextCodepoint(s, i);
        if (cp < kFirst || cp >= kFirst + kCount) cp = '?';
        const Packed& p = g[cp - kFirst];
        if (p.x1 > p.x0 && p.y1 > p.y0) {
            GlyphQuad q;
            q.x0 = (pen + p.xoff) / scale_;
            q.y0 = (base + p.yoff) / scale_;
            q.x1 = (pen + p.xoff2) / scale_;
            q.y1 = (base + p.yoff2) / scale_;
            q.s0 = p.x0 * inv; q.t0 = p.y0 * inv;
            q.s1 = p.x1 * inv; q.t1 = p.y1 * inv;
            emit(q);
        }
        pen += p.xadvance;
    }
}

std::optional<std::vector<std::uint8_t>> FontAtlas::loadSystemFont() {
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    std::filesystem::path fonts = "C:\\Windows\\Fonts";
    wchar_t* windir = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&windir, &len, L"WINDIR") == 0 && windir) { fonts = std::filesystem::path(windir) / "Fonts"; std::free(windir); }
    candidates = {fonts / "segoeuib.ttf", fonts / "segoeui.ttf", fonts / "arialbd.ttf", fonts / "arial.ttf"};
#else
    candidates = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
#endif
    for (const auto& f : candidates) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) return bytes;
    }
    return std::nullopt;
}

} // namespace hc::ui
