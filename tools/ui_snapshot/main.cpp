// hc_ui_snapshot — renders the real GPU UI off-screen (hidden window + framebuffer object)
// for every theme, saves PNG screenshots and checks the pixels. Used by CI (with a
// software OpenGL driver) and works on the rig too.
//
//   hc_ui_snapshot --out <folder> [--scale 1|2]
//
// Exit codes: 0 pass, 1 bad args, 2 GPU init failed, 3 pixel checks failed.
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Gl.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/MixerView.hpp"
#include "hybrid/ui/Renderer.hpp"
#include "hybrid/ui/Theme.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace hc;

namespace {

double regionStdDev(const ui::Image& img, int x0, int y0, int x1, int y1) {
    double sum = 0, sq = 0;
    std::size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            sum += l; sq += l * l; ++n;
        }
    const double m = sum / static_cast<double>(n);
    return std::sqrt(std::max(0.0, sq / static_cast<double>(n) - m * m));
}

std::size_t countNear(const ui::Image& img, ui::Rgb c, int tol) {
    const int r = static_cast<int>(c.r * 255), g = static_cast<int>(c.g * 255), b = static_cast<int>(c.b * 255);
    std::size_t n = 0;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4)
        if (std::abs(img.rgba[i] - r) <= tol && std::abs(img.rgba[i + 1] - g) <= tol && std::abs(img.rgba[i + 2] - b) <= tol) ++n;
    return n;
}

void setDemoState(params::ParameterTree& t) {
    t.find("stem01.main")->setPlain(0.0f);
    t.find("stem02.main")->setPlain(-6.0f);
    t.find("stem03.mute")->setPlain(1.0f);
    t.find("stem04.solo")->setPlain(1.0f);
    t.find("stem05.invert")->setPlain(1.0f);
    t.find("stem06.pan")->setPlain(-0.6f);
    t.find("stem07.pan")->setPlain(0.8f);
    t.find("stem08.trim")->setPlain(9.0f);
    t.find("stem09.aux1")->setPlain(-3.0f);
    t.find("stem10.aux2")->setPlain(0.0f);
    t.find("stem11.main")->setNormalized(0.0f);
    t.find("stem12.main")->setPlain(4.0f);
    t.find("aux1.mute")->setPlain(1.0f);
    t.find("main.gain")->setPlain(-2.0f);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    io::configureWideConsole();
    std::filesystem::path out = ".";
    float scale = 1.0f;
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--out" && i + 1 < argc) out = argv[++i];
        else if (k == L"--scale" && i + 1 < argc) scale = static_cast<float>(std::wcstod(argv[++i], nullptr));
        else { std::wprintf(L"usage: hc_ui_snapshot --out <folder> [--scale 1|2]\n"); return 1; }
    }
    if (!(scale >= 0.5f && scale <= 2.0f)) return 1;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);

    ui::GlWindow window;
    std::string err;
    if (!window.create(L"hc_ui_snapshot", 320, 200, false, &err)) {
        std::wprintf(L"GPU init failed: %hs\n", err.c_str());
        return 2;
    }
    ui::Renderer renderer;
    if (!renderer.init(&err)) { std::wprintf(L"renderer init failed: %hs\n", err.c_str()); return 2; }
    std::wprintf(L"OpenGL  : %hs\nRenderer: %hs\nS3TC/DXT5 upload: %ls\n", renderer.glVersion().c_str(),
                 renderer.glRenderer().c_str(), renderer.s3tcSupported() ? L"native (compressed in VRAM)" : L"fallback (decoded to RGBA8)");

    const int W = static_cast<int>(std::lround(ui::kDesignWidth * scale));
    const int H = static_cast<int>(std::lround(ui::kDesignHeight * scale));
    GLuint fbo = 0, rb = 0;
    gl::glGenFramebuffers(1, &fbo);
    gl::glBindFramebuffer(gl::kFramebuffer, fbo);
    gl::glGenRenderbuffers(1, &rb);
    gl::glBindRenderbuffer(gl::kRenderbuffer, rb);
    gl::glRenderbufferStorage(gl::kRenderbuffer, GL_RGBA8, W, H);
    gl::glFramebufferRenderbuffer(gl::kFramebuffer, gl::kColorAttachment0, gl::kRenderbuffer, rb);
    if (gl::glCheckFramebufferStatus(gl::kFramebuffer) != gl::kFramebufferComplete) {
        std::wprintf(L"framebuffer incomplete\n");
        return 2;
    }

    params::ParameterTree tree;
    params::addMixerParameters(tree);
    setDemoState(tree);
    const ui::Layout layout(tree);
    const ui::MixerView view(layout, tree);
    const auto vp = ui::Viewport::fit(static_cast<float>(W), static_cast<float>(H));

    const auto fontBytes = ui::FontAtlas::loadSystemFont();
    ui::FontAtlas font;
    if (!fontBytes || !font.bake(*fontBytes, vp.scale) || !renderer.setFont(font, &err)) {
        std::wprintf(L"font failed: %hs\n", err.c_str());
        return 2;
    }

    ui::MeterState meters;
    float sp[kStemCount], bl[kBusCount], br[kBusCount];
    for (std::uint32_t s = 0; s < kStemCount; ++s) sp[s] = std::pow(10.0f, (-48.0f + 3.5f * static_cast<float>(s)) / 20.0f);
    for (std::uint32_t b = 0; b < kBusCount; ++b) { bl[b] = std::pow(10.0f, (-20.0f + 6.0f * b) / 20.0f); br[b] = bl[b] * 0.8f; }
    meters.update(sp, bl, br, 0.016f);

    const std::filesystem::path dataDir = out / "ui-data";
    int failures = 0;
    for (std::uint32_t t = 0; t < ui::kThemeCount; ++t) {
        const auto theme = static_cast<ui::ThemeId>(t);
        const ui::Palette& pal = ui::palette(theme);
        std::string info;
        const auto plateT0 = std::chrono::steady_clock::now();
        const ui::CompressedTexture tex = ui::loadFaceplate(theme, ui::chooseTier(static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(H)), layout, dataDir, &info);
        const double plateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - plateT0).count();
        if (!renderer.setFaceplate(tex, &err)) { std::wprintf(L"faceplate failed: %hs\n", err.c_str()); return 2; }

        ui::ViewState vs;
        vs.theme       = theme;
        vs.deviceLabel = "ASIO \xC2\xB7 Focusrite USB ASIO";
        vs.sourceLabel = "TEST TONES";
        vs.status      = "ASIO \xC2\xB7 48000 Hz \xC2\xB7 64 frames = 1.33 ms \xC2\xB7 latency in 2.50 / out 3.10 ms";
        vs.detail      = "CPU 0.4% avg \xC2\xB7 1.1% peak \xC2\xB7 overruns 0  (snapshot demo values)";
        vs.hover       = 14;

        constexpr int kFrames = 60;
        double cpuMs = 0, gpuMs = 0;
        for (int f = 0; f < kFrames; ++f) {
            const auto c0 = std::chrono::steady_clock::now();
            renderer.begin(W, H, vp, {pal.plateBottom.r * 0.6f, pal.plateBottom.g * 0.6f, pal.plateBottom.b * 0.6f, 1});
            view.draw(renderer, vs, meters);
            renderer.end();
            const auto c1 = std::chrono::steady_clock::now();
            ::glFinish();
            const auto c2 = std::chrono::steady_clock::now();
            cpuMs += std::chrono::duration<double, std::milli>(c1 - c0).count();
            gpuMs += std::chrono::duration<double, std::milli>(c2 - c1).count();
        }

        ui::Image img;
        img.width = static_cast<std::uint32_t>(W);
        img.height = static_cast<std::uint32_t>(H);
        img.rgba.resize(static_cast<std::size_t>(W) * H * 4);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        for (int y = 0; y < H / 2; ++y)   // GL rows are bottom-up
            std::swap_ranges(img.rgba.begin() + static_cast<std::ptrdiff_t>(y) * W * 4,
                             img.rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * W * 4,
                             img.rgba.begin() + static_cast<std::ptrdiff_t>(H - 1 - y) * W * 4);
        const auto file = out / (std::string("ui_") + pal.fileStem + ".png");
        ui::writePng(file, img);

        const double whole = regionStdDev(img, 0, 0, W, H);
        const double title = regionStdDev(img, static_cast<int>(30 * vp.scale), static_cast<int>(14 * vp.scale),
                                          static_cast<int>(300 * vp.scale), static_cast<int>(48 * vp.scale));
        const std::size_t neon = countNear(img, pal.neon, 40);
        const bool ok = whole > 8.0 && title > 12.0 && neon > 2000 && ::glGetError() == GL_NO_ERROR;
        failures += ok ? 0 : 1;
        std::wprintf(L"%-12hs %dx%d  plate: %hs (%.0f ms)  VRAM %.2f MB  frame CPU %.2f ms GPU %.2f ms  "
                     L"draw calls %u quads %u  checks: contrast %.1f title %.1f neon px %zu -> %ls\n",
                     pal.name, W, H, info.c_str(), plateMs, static_cast<double>(renderer.vramBytes()) / (1024.0 * 1024.0),
                     cpuMs / kFrames, gpuMs / kFrames, renderer.stats().drawCalls, renderer.stats().quads, whole, title, neon,
                     ok ? L"PASS" : L"FAIL");
    }

    gl::glBindFramebuffer(gl::kFramebuffer, 0);
    gl::glDeleteRenderbuffers(1, &rb);
    gl::glDeleteFramebuffers(1, &fbo);
    std::wprintf(failures ? L"\nUI SNAPSHOT FAIL\n" : L"\nUI SNAPSHOT PASS\n");
    return failures ? 3 : 0;
}
