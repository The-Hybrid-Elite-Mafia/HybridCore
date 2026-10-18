// hc_fx_snapshot — renders the obsidian faceplate of every HEM plug-in (the 22 effects and
// HEM WAVE) off-screen with the real faceplate code (hc::ui::FxPanelWindow, OpenGL), after
// running each effect on a few seconds of test audio so its meters are live, and saves PNGs
// plus one contact sheet. Used by CI (software OpenGL); works on the rig too.
//
//   hc_fx_snapshot [--out <folder>] [--scale 0.62]
// Exit 3 when a faceplate fails to open or comes out blank.
#include "hybrid/fx/Effects.hpp"
#include "hybrid/synth/Wave.hpp"
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelWindow.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace hc;

namespace {

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

double lumaStdDev(const ui::Image& img) {
    double s = 0, sq = 0;
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = &img.rgba[i * 4];
        const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        s += l; sq += l * l;
    }
    const double m = s / static_cast<double>(std::max<std::size_t>(n, 1));
    return std::sqrt(std::max(0.0, sq / static_cast<double>(std::max<std::size_t>(n, 1)) - m * m));
}

// A short "song": kick-ish hits, a held 452 Hz voice, hats, hiss, wide pad.
void feed(fx::Effect& e) {
    e.prepare(48000.0, 512);
    std::mt19937 g(5);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> l(512), r(512);
    for (int b = 0; b < 48000 * 4 / 512; ++b) {
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0, beat = std::fmod(t, 0.5);
            const float kick = 0.45f * static_cast<float>(std::exp(-beat * 18.0) * std::sin(2 * 3.14159265 * 60 * beat));
            const float voice = 0.18f * static_cast<float>(std::sin(2 * 3.14159265 * 452 * t));
            const float hat = (std::fmod(t, 0.25) < 0.02 ? 0.12f : 0.0f) * d(g);
            const float hiss = 0.01f * d(g), side = 0.05f * d(g);
            l[static_cast<std::size_t>(i)] = kick + voice + hat + hiss + side;
            r[static_cast<std::size_t>(i)] = kick + voice + hat + hiss - side;
        }
        e.process(l.data(), r.data(), 512);
    }
}

class WaveSnapshotHost final : public ui::PanelHost {
public:
    explicit WaveSnapshotHost(synth::WaveSynth& s) : s_(s) {}
    float normalized(int i) const override {
        if (i >= synth::kParamCount) return 0.0f;
        return synth::toNormalized(synth::WaveSynth::params()[static_cast<std::size_t>(i)], s_.get(i));
    }
    void beginEdit(int) override {}
    void setNormalized(int, float) override {}
    void endEdit(int) override {}
    std::string text(int i) const override {
        if (i >= synth::kParamCount) return synth::WaveSynth::presetName(0);
        const auto& p = synth::WaveSynth::params()[static_cast<std::size_t>(i)];
        return synth::formatValue(p, s_.get(i));
    }
    bool hasBypass() const override { return false; }
private:
    synth::WaveSynth& s_;
};

ui::PanelSpec waveSpec() {
    ui::PanelSpec s;
    s.name = "HEM WAVE";
    s.blurb = "Morphing wavetable synth: 8 tables, unison, sub, filter, two envelopes, LFO";
    for (const synth::ParamInfo& p : synth::WaveSynth::params()) {
        ui::PanelParam pp;
        pp.name = p.name;
        if (p.taper == synth::Taper::Toggle) pp.kind = ui::PanelParam::Kind::Toggle;
        else if (p.taper == synth::Taper::Choice) {
            pp.kind = ui::PanelParam::Kind::Choice;
            for (int c = 0; c <= static_cast<int>(p.max); ++c) pp.choices.emplace_back(p.choices ? p.choices[c] : std::to_string(c));
        }
        pp.defNormalized = synth::toNormalized(p, p.def);
        s.params.push_back(std::move(pp));
    }
    ui::PanelParam preset;
    preset.name = "Preset";
    preset.kind = ui::PanelParam::Kind::Choice;
    for (int i = 0; i < synth::WaveSynth::presetCount(); ++i) preset.choices.emplace_back(synth::WaveSynth::presetName(i));
    s.params.push_back(std::move(preset));
    using P = synth::Param;
    s.groups = {{"OSCILLATOR", P::Table, P::FilterType - P::Table}, {"FILTER", P::FilterType, P::AmpAttack - P::FilterType},
                {"AMP ENVELOPE", P::AmpAttack, P::ModAttack - P::AmpAttack}, {"MOD ENVELOPE", P::ModAttack, P::LfoShape - P::ModAttack},
                {"LFO", P::LfoShape, P::VelAmp - P::LfoShape}, {"VOICE", P::VelAmp, P::kParamCount - P::VelAmp + 1}};
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path out = "fx-shots";
    float scale = 0.62f;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out") out = argv[++i];
        else if (a == "--scale") scale = static_cast<float>(std::atof(argv[++i]));
    }
    std::filesystem::create_directories(out);
    const auto assets = exeDir() / "assets";
    int failures = 0;
    std::vector<ui::Image> shots;
    auto save = [&](const std::string& name, const ui::PanelSpec& spec, ui::PanelHost& host) {
        ui::FxPanelWindow w;
        std::string err;
        if (!w.openHidden(spec, host, assets, scale, &err)) {
            std::printf("FAIL %-18s could not open: %s\n", name.c_str(), err.c_str());
            ++failures;
            return;
        }
        ui::Image img = w.snapshot();
        const double sd = img.empty() ? 0.0 : lumaStdDev(img);
        std::string file = name;
        for (char& c : file) if (c == ' ') c = '_';
        ui::writePng(out / (file + ".png"), img);
        const bool ok = !img.empty() && sd > 4.0;
        std::printf("%s %-18s %4u x %4u  luma sd %.1f\n", ok ? "ok  " : "FAIL", name.c_str(), img.width, img.height, sd);
        if (!ok) ++failures;
        shots.push_back(std::move(img));
    };
    for (const fx::EffectInfo& info : fx::effects()) {
        auto e = info.create();
        if (std::string(info.name) == "HEM TUNE") e->set(0, 9.0f);   // key A
        feed(*e);
        const ui::PanelSpec spec = ui::panelSpecFor(info, *e);
        ui::EffectPanelHost host(*e);
        save(info.name, spec, host);
    }
    {
        synth::WaveSynth s;
        WaveSnapshotHost host(s);
        const ui::PanelSpec spec = waveSpec();
        save("HEM WAVE", spec, host);
    }
    // contact sheet: 4 across, each scaled to 640 wide
    if (!shots.empty()) {
        const std::uint32_t cw = 640, cols = 4;
        std::vector<ui::Image> small;
        std::uint32_t rowH = 0, sheetH = 0;
        std::vector<std::uint32_t> rowHeights;
        for (std::size_t i = 0; i < shots.size(); ++i) {
            const auto h = static_cast<std::uint32_t>(std::lround(static_cast<double>(shots[i].height) * cw / shots[i].width));
            small.push_back(ui::resizeSrgb(shots[i], cw, h));
            rowH = std::max(rowH, h);
            if ((i + 1) % cols == 0 || i + 1 == shots.size()) { rowHeights.push_back(rowH); sheetH += rowH + 12; rowH = 0; }
        }
        ui::Image sheet;
        sheet.width = cols * (cw + 12);
        sheet.height = sheetH;
        sheet.rgba.assign(static_cast<std::size_t>(sheet.width) * sheet.height * 4, 0);
        for (std::size_t i = 3; i < sheet.rgba.size(); i += 4) sheet.rgba[i] = 255;
        std::uint32_t y = 0;
        for (std::size_t i = 0; i < small.size(); ++i) {
            const std::uint32_t col = static_cast<std::uint32_t>(i % cols), row = static_cast<std::uint32_t>(i / cols);
            if (col == 0 && row > 0) y += rowHeights[row - 1] + 12;
            for (std::uint32_t yy = 0; yy < small[i].height; ++yy)
                std::copy_n(small[i].at(0, yy), static_cast<std::size_t>(cw) * 4,
                            &sheet.rgba[(static_cast<std::size_t>(y + yy) * sheet.width + col * (cw + 12)) * 4]);
        }
        ui::writePng(out / "HEM-plugins-contact-sheet.png", sheet);
    }
    std::printf("\nHEM FACEPLATES: %zu rendered, %d failed\n", shots.size(), failures);
    return failures ? 3 : 0;
}
