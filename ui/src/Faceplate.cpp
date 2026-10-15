#include "hybrid/ui/Faceplate.hpp"

#include <algorithm>
#include <cmath>

namespace hc::ui {

Canvas Canvas::fit(float w, float h) noexcept {
    Canvas c;
    if (!(w > 0.0f) || !(h > 0.0f)) return c;
    c.pixelW = w;
    c.pixelH = h;
    if (w / h <= plate::kWidth / plate::kHeight) {
        c.scale   = w / plate::kWidth;
        c.extraH  = std::max(0.0f, h / c.scale - plate::kHeight);
        c.offsetX = 0.0f;
        c.offsetY = std::round((h - (plate::kHeight + c.extraH) * c.scale) * 0.5f);
    } else {
        c.scale   = h / plate::kHeight;
        c.extraH  = 0.0f;
        c.offsetX = std::round((w - plate::kWidth * c.scale) * 0.5f);
        c.offsetY = 0.0f;
    }
    return c;
}

float Canvas::plateY(float y) const noexcept {
    if (y <= plate::kStretchY0) return y;
    if (y >= plate::kStretchY1) return y + extraH;
    const float band = plate::kStretchY1 - plate::kStretchY0;
    return plate::kStretchY0 + (y - plate::kStretchY0) * (band + extraH) / band;
}

Rect Canvas::plateRect(Rect r) const noexcept {
    const float top = plateY(r.y), bottom = plateY(r.bottom());
    return {r.x, top, r.w, bottom - top};
}

std::filesystem::path resolveFaceplate(const std::filesystem::path& assetDir, const std::filesystem::path& dataDir) {
    std::error_code ec;
    for (const char* ext : {".png", ".jpg"}) {
        const auto skin = dataDir / "skins" / (std::string("Main-Faceplate") + ext);
        if (!dataDir.empty() && std::filesystem::is_regular_file(skin, ec)) return skin;
    }
    return assetDir / "faceplates" / kFaceplateFile;
}

std::optional<FaceplateArt> loadFaceplateArt(const std::filesystem::path& file, std::string* error) {
    std::string why;
    auto img = loadImageFile(file, &why);
    if (!img) {
        if (error) *error = file.filename().string() + ": " + why;
        return std::nullopt;
    }
    const double aspect = static_cast<double>(img->width) / img->height;
    if (std::fabs(aspect / (16.0 / 9.0) - 1.0) > 0.01 || img->width < 1920) {
        if (error) *error = file.filename().string() + ": must be 16:9 and at least 1920 px wide (got " + std::to_string(img->width) + "x" + std::to_string(img->height) + ")";
        return std::nullopt;
    }
    FaceplateArt art;
    const auto W = static_cast<std::uint32_t>(plate::kWidth), H = static_cast<std::uint32_t>(plate::kHeight);
    art.origin = file.filename().string() + " (" + std::to_string(img->width) + "x" + std::to_string(img->height) + ")";
    art.master = (img->width == W && img->height == H) ? std::move(*img) : resizeSrgb(*img, W, H);
    for (std::size_t i = 3; i < art.master.rgba.size(); i += 4) art.master.rgba[i] = 255;   // the chassis is opaque
    return art;
}

std::uint32_t chassisTextureWidth(float canvasScale) noexcept {
    const float need = plate::kWidth * canvasScale;
    for (std::uint32_t w : {960u, 1280u, 1920u, 2560u})
        if (static_cast<float>(w) >= need) return w;
    return static_cast<std::uint32_t>(plate::kWidth);
}

TextureLevels bakeChassis(const FaceplateArt& art, std::uint32_t textureWidth) {
    textureWidth = std::min(textureWidth, static_cast<std::uint32_t>(plate::kWidth));
    const std::uint32_t h = textureWidth * 9 / 16;
    Image level0 = (textureWidth == art.master.width) ? art.master : resizeSrgb(art.master, textureWidth, h);
    return buildMips(std::move(level0));   // opaque: premultiplied == straight
}

} // namespace hc::ui
