#pragma once
// Blind A/B pack (engine 18, file part) and name-strip (engine 19, file part).
// Names carry nothing: files are called pair_01_X.wav / pair_01_Y.wav. WAV files are written fresh with only the format and
// audio chunks, so no title, comment or tag from the source survives. Samples are copied exactly (same bits, same format).
#include "HissWav.hpp"
#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissReport.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hc::hiss::pack {

inline std::string fileHash(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return lab::sha256Hex(d.data(), d.size());
}

// Writes outDir/pair_NN_X.wav, outDir/pair_NN_Y.wav, outDir/KEY.txt (keep private until the listening is done) and outDir/COMMIT.txt (publish first).
inline bool makePack(const std::vector<std::string>& a, const std::vector<std::string>& b, const std::string& outDir, const std::string& salt, std::string& err) {
    if (a.size() != b.size() || a.empty()) { err = "need the same number of A and B files, at least one pair"; return false; }
    std::vector<std::string> ha, hb;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ha.push_back(fileHash(a[i])); hb.push_back(fileHash(b[i]));
        if (ha.back() == hb.back()) { err = "pair " + std::to_string(i + 1) + ": A and B are the same file"; return false; }
    }
    const auto plan = report::blindPlan(ha, hb, salt);
    std::filesystem::create_directories(outDir);
    for (std::size_t i = 0; i < a.size(); ++i) {
        wav::File fa, fb;
        if (!wav::read(a[i], fa, err) || !wav::read(b[i], fb, err)) return false;
        const wav::File& x = plan.xIsA[i] ? fa : fb;
        const wav::File& y = plan.xIsA[i] ? fb : fa;
        char nm[64];
        std::snprintf(nm, sizeof nm, "pair_%02zu_X.wav", i + 1);
        if (!wav::write((std::filesystem::path(outDir) / nm).string(), x, err)) return false;
        std::snprintf(nm, sizeof nm, "pair_%02zu_Y.wav", i + 1);
        if (!wav::write((std::filesystem::path(outDir) / nm).string(), y, err)) return false;
    }
    std::ofstream(std::filesystem::path(outDir) / "KEY.txt", std::ios::binary) << plan.keyText;
    std::ofstream(std::filesystem::path(outDir) / "COMMIT.txt", std::ios::binary) << "SHA-256 of KEY.txt: " << plan.commitHex << "\nPublish this line BEFORE listening. After listening, show KEY.txt; anyone can hash it and compare.\n";
    return true;
}

// Re-writes a WAV with no extra chunks and a name that says nothing: outDir/HEM-xxxxxxxx.wav (8 hex digits of the audio's fingerprint).
inline bool stripName(const std::string& in, const std::string& outDir, std::string& outPath, std::string& err) {
    wav::File f;
    if (!wav::read(in, f, err)) return false;
    std::filesystem::create_directories(outDir);
    const std::string tmp = (std::filesystem::path(outDir) / ".hem_tmp.wav").string();
    if (!wav::write(tmp, f, err)) return false;
    const std::string name = lab::blindName(fileHash(tmp));
    outPath = (std::filesystem::path(outDir) / (name + ".wav")).string();
    std::filesystem::rename(tmp, outPath);
    return true;
}

} // namespace hc::hiss::pack
