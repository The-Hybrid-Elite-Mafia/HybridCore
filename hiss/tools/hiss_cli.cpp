// hc_hiss_cli — fix a file, a folder of stems, or run the built-in demo scenes.
//   hc_hiss_cli auto  <in.wav>  <outdir>
//   hc_hiss_cli stems <indir>   <outdir>
//   hc_hiss_cli demo            <outdir>
// For every file it writes: <name>_fixed.wav, <name>_removed.wav, <name>_report.txt, <name>_result.json
// A file the tool leaves alone is written back with exactly the same samples.
#include "HissTestSignals.hpp"
#include "HissWav.hpp"
#include "hybrid/hiss/HissAuto.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace hc::hiss;

namespace {

void writeText(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n') o += "\\n"; else o += c; }
    return o + "\"";
}

void writeResult(const fs::path& dir, const std::string& name, const wav::File& in, const AutoResult& r) {
    wav::File fixed = in;
    fixed.left = r.left;
    fixed.right = r.right;
    wav::File removed = in;
    removed.isFloat = true;
    removed.bits = 32;
    removed.left = r.removedLeft;
    removed.right = r.removedRight;
    std::string err;
    if (!wav::write((dir / (name + "_fixed.wav")).string(), fixed, err)) std::cerr << err << "\n";
    if (!wav::write((dir / (name + "_removed.wav")).string(), removed, err)) std::cerr << err << "\n";
    writeText(dir / (name + "_report.txt"), r.report);
    const auto& d = r.diagnosis;
    const auto& c = r.finalCheck;
    char b[1600];
    std::snprintf(b, sizeof b,
        "{\"name\":%s,\"sampleRate\":%d,\"touched\":%s,\"confidence\":%s,\"kind\":%s,\"centreHz\":%.2f,\"widthHz\":%.2f,\"bumpDb\":%.2f,"
        "\"mode\":%s,\"focusHz\":%.2f,\"chosenWidthHz\":%.2f,\"reductionDb\":%.2f,\"method\":%s,"
        "\"dropDb\":%.3f,\"peakLossDb\":%.3f,\"envelopeErrDb\":%.3f,\"attackShiftMs\":%.3f,\"outOfBandDb\":%.4f,\"musicalNoise\":%.3f,"
        "\"levelDiffDb\":%.4f,\"levelMatchGainDb\":%.4f,\"hitsChecked\":%d,\"backOffSteps\":%d}\n",
        jsonStr(name).c_str(), in.sampleRate, r.touched ? "true" : "false", jsonStr(confidenceName(r.confidence)).c_str(), jsonStr(kindName(d.kind)).c_str(),
        d.centreHz, d.widthHz, d.bumpDb, jsonStr(modeName(r.chosen.mode)).c_str(), static_cast<double>(r.chosen.focusHz), static_cast<double>(r.chosen.widthHz),
        static_cast<double>(r.chosen.reductionDb), jsonStr(methodName(r.chosen.method)).c_str(), c.dropDb, c.peakLossDb, c.envelopeErrDb, c.attackShiftMs,
        c.outOfBandDb, c.musicalNoise, c.levelDiffDb, r.levelMatchGainDb, c.hitsChecked, r.backOffSteps);
    writeText(dir / (name + "_result.json"), b);
}

AutoOptions options() {
    AutoOptions o;
    o.progress = [](const std::string& s) { std::cout << "   - " << s << "\n" << std::flush; };
    return o;
}

int cmdAuto(const fs::path& in, const fs::path& out) {
    wav::File f;
    std::string err;
    if (!wav::read(in.string(), f, err)) { std::cerr << err << "\n"; return 2; }
    fs::create_directories(out);
    const std::string name = in.stem().string();
    std::cout << name << "\n";
    const auto r = autoFix(f.left.data(), f.right.data(), f.left.size(), f.sampleRate, options(), name);
    writeResult(out, name, f, r);
    std::cout << r.report;
    return 0;
}

int cmdStems(const fs::path& in, const fs::path& out) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(in))
        if (e.is_regular_file() && (e.path().extension() == ".wav" || e.path().extension() == ".WAV")) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::cerr << "no .wav files in " << in << "\n"; return 2; }
    fs::create_directories(out);
    std::vector<wav::File> loaded(files.size());
    std::vector<StemRef> refs;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string err;
        if (!wav::read(files[i].string(), loaded[i], err)) { std::cerr << err << "\n"; return 2; }
        refs.push_back({files[i].stem().string(), &loaded[i].left, &loaded[i].right});
    }
    const double fsHz = loaded[0].sampleRate;
    const auto results = autoFixStems(refs, fsHz, options());
    for (std::size_t i = 0; i < files.size(); ++i) writeResult(out, refs[i].name, loaded[i], results[i]);
    const auto text = stemsReport(results);
    writeText(out / "stems_report.txt", text);
    std::cout << text;
    return 0;
}

int cmdDemo(const fs::path& out) {
    fs::create_directories(out);
    struct Demo { const char* name; test::SceneSpec spec; };
    std::vector<Demo> demos;
    { test::SceneSpec s; demos.push_back({"demo_steady", s}); }
    { test::SceneSpec s; s.hiss = test::Hiss::Tone; demos.push_back({"demo_tone", s}); }
    { test::SceneSpec s; s.hiss = test::Hiss::Flutter; demos.push_back({"demo_flutter", s}); }
    { test::SceneSpec s; s.hiss = test::Hiss::None; demos.push_back({"demo_clean", s}); }
    { test::SceneSpec s; s.denseHats = true; s.hiss = test::Hiss::None; demos.push_back({"demo_densehats_nohiss", s}); }
    for (const auto& d : demos) {
        const auto sc = test::makeScene(d.spec);
        wav::File f;
        f.sampleRate = static_cast<int>(d.spec.fs);
        f.left = sc.L;
        f.right = sc.R;
        std::string err;
        wav::write((out / (std::string(d.name) + "_before.wav")).string(), f, err);
        // The demo files are 24-bit: judge exactly what is stored, not the float original.
        wav::File back;
        wav::read((out / (std::string(d.name) + "_before.wav")).string(), back, err);
        {
            std::string t = "{\"hitStarts\":[";
            for (std::size_t i = 0; i < sc.hitStarts.size(); ++i) t += (i ? "," : "") + std::to_string(sc.hitStarts[i]);
            writeText(out / (std::string(d.name) + "_truth.json"), t + "]}\n");
        }
        std::cout << d.name << "\n";
        const auto r = autoFix(back.left.data(), back.right.data(), back.left.size(), back.sampleRate, options(), d.name);
        writeResult(out, d.name, back, r);
        wav::File hiss = back;
        hiss.left = sc.hissL;
        hiss.right = sc.hissR;
        hiss.isFloat = true;
        hiss.bits = 32;
        wav::write((out / (std::string(d.name) + "_truth_artifact.wav")).string(), hiss, err);
        std::cout << r.report << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "auto") return cmdAuto(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "stems") return cmdStems(argv[2], argv[3]);
    if (argc >= 3 && std::string(argv[1]) == "demo") return cmdDemo(argv[2]);
    std::cerr << "usage:\n  hc_hiss_cli auto  <in.wav> <outdir>\n  hc_hiss_cli stems <indir> <outdir>\n  hc_hiss_cli demo  <outdir>\n";
    return 1;
}
