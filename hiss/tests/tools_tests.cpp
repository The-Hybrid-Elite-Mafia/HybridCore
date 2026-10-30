// Tests for the report maker (20), blind plan and pack (18), fingerprint and strip (19), the Tamer gate, and the four measuring tools.
#include "HissBlindPack.hpp"
#include "HissHarness.hpp"
#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissMeters.hpp"
#include "hybrid/hiss/HissReport.hpp"
#include "hybrid/hiss/HissTools.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace hc::hiss;
namespace mk = hc::hiss::lab::maker;
namespace fsys = std::filesystem;

static bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

static void testReport() {
    std::printf("[20 report maker]\n");
    std::vector<report::Finding> f;
    f.push_back(report::withAbsRange("Level", -23.0, 0.01, "LUFS", "TESTED"));
    f.push_back(report::refused("Flutter", "clip under 19.5 s"));
    f.push_back(report::noRange("Covered", 80.0, "%", "ASSUMED", "guess"));
    const std::string t = report::makeReport("Title", f, {"a caveat"});
    HCHECK(contains(t, "range -23.01 to -22.99") && contains(t, "[TESTED]"), "a tested line shows its value, its range and its status");
    HCHECK(contains(t, "range UNKNOWN") && contains(t, "[ASSUMED]"), "a line with no range says the range is UNKNOWN and carries ASSUMED");
    HCHECK(t.find("Could not be measured") < t.find("Readings"), "refused lines come before the numbers (bad news first)");
    HCHECK(contains(t, "clip under 19.5 s") && contains(t, "a caveat"), "reasons and caveats are printed");
    HCHECK(contains(t, "does not mean checked on real Suno files"), "the report says TESTED is not a real-file check");
    HCHECK(t == report::makeReport("Title", f, {"a caveat"}), "the same input gives the same report");
}

static void testBlind() {
    std::printf("[18 blind plan and pack, 19 strip]\n");
    std::vector<std::string> a, b;
    for (int i = 0; i < 400; ++i) { a.push_back(lab::sha256Hex(&i, sizeof i)); const int j = i + 100000; b.push_back(lab::sha256Hex(&j, sizeof j)); }
    const auto p1 = report::blindPlan(a, b, "salt-one"), p2 = report::blindPlan(a, b, "salt-one"), p3 = report::blindPlan(a, b, "salt-two");
    int xa = 0; for (bool v : p1.xIsA) xa += v;
    HMEASURED("share of pairs with A as X (400 pairs)", xa / 400.0, "");
    HCHECK(xa > 160 && xa < 240, "A is X in 40-60 %% of pairs (no lean)");
    HCHECK(p1.keyText == p2.keyText && p1.commitHex == p2.commitHex, "same files and salt: same plan");
    HCHECK(p1.commitHex != p3.commitHex, "another salt: another plan");
    HCHECK(p1.commitHex == lab::sha256Hex(p1.keyText.data(), p1.keyText.size()), "the commitment is the SHA-256 of the key text");
    std::string altered = p1.keyText; altered[altered.size() / 2] ^= 1;
    HCHECK(lab::sha256Hex(altered.data(), altered.size()) != p1.commitHex, "changing one character of the key breaks the commitment");

    // files
    const fsys::path dir = fsys::temp_directory_path() / "hem_hiss_pack_test";
    fsys::remove_all(dir); fsys::create_directories(dir);
    std::vector<std::string> fa, fb;
    for (int i = 0; i < 4; ++i) {
        for (int w = 0; w < 2; ++w) {
            hc::hiss::wav::File f; f.sampleRate = 48000; f.channels = 2; f.bits = 24;
            f.left = mk::whiteNoise(4800, -20.0, 100 + i * 2 + w); f.right = mk::whiteNoise(4800, -20.0, 200 + i * 2 + w);
            std::string err; const std::string path = (dir / ((w ? "songB_" : "songA_") + std::to_string(i) + ".wav")).string();
            hc::hiss::wav::write(path, f, err);
            (w ? fb : fa).push_back(path);
        }
    }
    std::string err;
    HCHECK(pack::makePack(fa, fb, (dir / "pack").string(), "s1", err), "a 4-pair pack is written (%s)", err.c_str());
    bool namesBlind = true, audioSame = true, keyOk = true;
    std::ifstream kf(dir / "pack" / "KEY.txt"); std::string key((std::istreambuf_iterator<char>(kf)), std::istreambuf_iterator<char>());
    for (const auto& e : fsys::directory_iterator(dir / "pack")) { const std::string nm = e.path().filename().string(); if (contains(nm, "song") ) namesBlind = false; }
    const auto plan = report::blindPlan({pack::fileHash(fa[0]), pack::fileHash(fa[1]), pack::fileHash(fa[2]), pack::fileHash(fa[3])}, {pack::fileHash(fb[0]), pack::fileHash(fb[1]), pack::fileHash(fb[2]), pack::fileHash(fb[3])}, "s1");
    for (int i = 0; i < 4; ++i) {
        char nm[64]; std::snprintf(nm, sizeof nm, "pair_%02d_X.wav", i + 1);
        wav::File x, src; std::string e2;
        wav::read((dir / "pack" / nm).string(), x, e2);
        wav::read((plan.xIsA[i] ? fa[i] : fb[i]), src, e2);
        if (x.left != src.left || x.right != src.right || x.bits != src.bits) audioSame = false;
        char want[40]; std::snprintf(want, sizeof want, "pair %02d: X is %s", i + 1, plan.xIsA[i] ? "A" : "B");
        if (!contains(key, want)) keyOk = false;
    }
    HCHECK(namesBlind, "no file in the pack carries a source name");
    HCHECK(audioSame, "X files hold exactly the samples of the file the key says (same bits, same format)");
    HCHECK(keyOk, "KEY.txt agrees with which file went where");
    std::ifstream cf(dir / "pack" / "COMMIT.txt"); std::string commit((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
    HCHECK(contains(commit, lab::sha256Hex(key.data(), key.size())), "COMMIT.txt holds the SHA-256 of KEY.txt as written");
    HCHECK(!pack::makePack(fa, fa, (dir / "pack2").string(), "s1", err), "a pair of two identical files is refused");

    // strip: a file with a LIST chunk comes out without it, same audio, blind name
    wav::File f; std::string e3; wav::read(fa[0], f, e3);
    const std::string tagged = (dir / "tagged_title_secret.wav").string();
    wav::write(tagged, f, e3);
    { std::ifstream in(tagged, std::ios::binary); std::vector<char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const char list[] = {'L','I','S','T',12,0,0,0,'I','N','A','M',4,0,0,0,'T','o','p','s'};
      d.insert(d.end(), list, list + sizeof list);
      const std::uint32_t riff = static_cast<std::uint32_t>(d.size() - 8); std::memcpy(&d[4], &riff, 4);
      std::ofstream(tagged, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size())); }
    std::string outPath;
    HCHECK(pack::stripName(tagged, (dir / "stripped").string(), outPath, e3), "strip works (%s)", e3.c_str());
    std::ifstream sf(outPath, std::ios::binary); std::string sd((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
    HCHECK(!contains(sd, "LIST") && !contains(sd, "Tops"), "the stripped file has no LIST chunk and no title text");
    HCHECK(!contains(outPath, "tagged") && !contains(outPath, "secret") && contains(fsys::path(outPath).filename().string(), "HEM-"), "the stripped file name says nothing about the source");
    wav::File g; wav::read(outPath, g, e3);
    HCHECK(g.left == f.left && g.right == f.right, "the audio is bit for bit the same after stripping");
    kf.close(); cf.close(); sf.close();           // Windows cannot delete a file that is still open
    std::error_code ec; fsys::remove_all(dir, ec);
}

static void testGate() {
    std::printf("[Hiss Tamer gate]\n");
    HCHECK(!report::tamerGate("").open, "no proof file: refused");
    HCHECK(!report::tamerGate("cause_proven: no\nblind_result: x\nevidence: y\n").open, "'no' is refused");
    HCHECK(!report::tamerGate("cause_proven: yes\nevidence: y\n").open, "missing blind result: refused");
    HCHECK(!report::tamerGate("cause_proven: yes\nblind_result: x\n").open, "missing evidence: refused");
    HCHECK(!report::tamerGate("cause_proven: yes\nblind_result:\nevidence: y\n").open, "empty blind result: refused");
    HCHECK(report::tamerGate("Cause_Proven: YES\r\nblind_result: listening 2026-11-01, KEY.txt matches COMMIT.txt\r\nevidence: comb 25 Hz, -88 dBFS per line\r\n").open, "all three lines: allowed");
}

static void testTools() {
    std::printf("[the four measuring tools, run on a made-up song with known problems]\n");
    const double fs = 48000.0;
    const std::size_t n = static_cast<std::size_t>(fs * 24.0);
    std::vector<float> l, r;
    // bed: hits (so smear/echo/average have something to read) + noise bed, then the known problems
    auto hits = mk::hitTrain(n, fs, 1000.0, 40.0, -22.0, 6000.0, 16000.0, 311);
    auto bed = mk::whiteNoise(n, -45.0, 312);
    l.resize(n); for (std::size_t i = 0; i < n; ++i) l[i] = hits.audio[i] + bed[i];
    mk::injectComb(l, fs, -75.0, 3000.0, 20000.0, 7);
    mk::injectBandWobble(l, fs, 7.0, 0.20, 10000.0, 16000.0);
    mk::injectSeamClicks(l, fs, 40.0, -20.0, 11.0);
    r = l;
    const std::vector<float> l0 = l, r0 = r;

    const auto sc = tools::hissScanner(l.data(), r.data(), n, fs, "madeup");
    HCHECK(contains(sc.text, "Loudness (integrated)") && contains(sc.text, "Ratio 16-20 over 5-10 kHz") && contains(sc.text, "Where the sound ends"), "Scanner prints loudness, ratio and bandwidth lines");
    const auto cc = tools::combAndClickFinder(l.data(), r.data(), n, fs, "madeup");
    double combLine = 0; bool pattern = false;
    for (const auto& f : cc.findings) {
        if (f.name.rfind("Comb line level", 0) == 0) combLine = f.value;
        if (f.name.rfind("Share of gaps at 40 ms", 0) == 0 && f.value >= 60.0) pattern = true;
    }
    HMEASURED("Comb and Click Finder: comb line read (injected -75)", combLine, "dBFS");
    HCHECK(std::fabs(combLine + 75.0) < 1.5, "the comb line is read within 1.5 dB of the injected -75 dBFS");
    HCHECK(pattern && contains(cc.text, "YES, a 40 ms pattern"), "the 40 ms click train is called a pattern");
    const auto al = tools::airAndSmearLab(l.data(), r.data(), n, fs, "madeup");
    double wobRate = 0, tail = 0;
    for (const auto& f : al.findings) {
        if (f.name.rfind("Tail time", 0) == 0) tail = f.value;
        if (f.name.rfind("Wobble line", 0) == 0 && f.name.find("rate") != std::string::npos && std::fabs(f.value - 7.0) < 0.5) wobRate = f.value;
    }
    HMEASURED("Air and Smear Lab: wobble rate (injected 7)", wobRate, "Hz");
    HMEASURED("Air and Smear Lab: tail time (true 92.1 ms)", tail, "ms");
    HCHECK(std::fabs(wobRate - 7.0) < 0.3, "the lab lists a wobble line at the injected 7 Hz (within 0.3 Hz)");
    HCHECK(contains(al.text, "NOT trusted") && contains(al.text, "RHYTHM-LIKE"), "under the made-up beat the lab says the wobble depth is NOT trusted and marks the beat as RHYTHM-LIKE");
    HCHECK(tail > 70.0 && tail < 110.0, "the lab reads the injected hit tail within about 20 %%");
    HCHECK(contains(al.text, "Left/right phase") && contains(al.text, "nothing to compare"), "a mono file gets 'nothing to compare' for phase, not a made-up number");
    HCHECK(contains(al.text, "Masking: not run"), "masking says it was not run when no artifact level is given");
    HCHECK(l == l0 && r == r0, "the tools never change the audio they read (bypass check: 0 samples differ)");
    const auto al2 = tools::airAndSmearLab(l.data(), r.data(), n, fs, "madeup", 11000.0, 12000.0, -200.0 + 150.0);
    HCHECK(contains(al2.text, "Share of loud frames where the artifact is covered"), "with an artifact level given, masking is reported with its ASSUMED label");
    // stereo with two different noises: no phase angle may be given
    auto o1 = mk::whiteNoise(n / 2, -30.0, 321), o2 = mk::whiteNoise(n / 2, -30.0, 322);
    const auto st = tools::airAndSmearLab(o1.data(), o2.data(), n / 2, fs, "two noises");
    HCHECK(contains(st.text, "would mean nothing") && !contains(st.text, " deg "), "two different noises: no phase angle is printed, the page says why");
    HCHECK(contains(st.text, "Checked, nothing found"), "clean negatives are listed under 'Checked, nothing found', not under 'Could not be measured'");
    const auto sh = tools::hissScanner(l.data(), r.data(), 1000, fs, "short");
    HCHECK(contains(sh.text, "clip shorter than 1 s"), "a clip shorter than 1 s is refused with a reason");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);     // so a crash cannot swallow what was already printed
    try {
        testReport();
        testBlind();
        testGate();
        testTools();
    } catch (const std::exception& e) {
        std::printf("  FAIL  an exception stopped the tests: %s\n", e.what());
        return 2;
    }
    std::printf("\n%d checks, %d failed\n", test::gChecks, test::gFails);
    return test::gFails ? 1 : 0;
}
