// hc_hiss_tools: the five HEM hiss tools, one command each. Pure C++, no JUCE. Offline.
//   hc_hiss_tools scanner   <in.wav> <out.md>
//   hc_hiss_tools combclick <in.wav> <out.md>
//   hc_hiss_tools airlab    <in.wav> <out.md> [artifactLoHz artifactHiHz artifactDb]
//   hc_hiss_tools proof inject comb|wobble|clicks|echo|notch <in.wav> <out.wav> <numbers...>
//   hc_hiss_tools proof pack <outdir> <salt> <a1.wav> <b1.wav> [<a2.wav> <b2.wav> ...]
//   hc_hiss_tools proof hash <file...>
//   hc_hiss_tools proof strip <in.wav> <outdir>
//   hc_hiss_tools tamer     <in.wav> <outdir> <proof.txt>
#include "HissBlindPack.hpp"
#include "HissWav.hpp"
#include "hybrid/hiss/HissAuto.hpp"
#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissReport.hpp"
#include "hybrid/hiss/HissTools.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hc::hiss;
namespace fsys = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage:\n  hc_hiss_tools scanner <in.wav> <out.md>\n  hc_hiss_tools combclick <in.wav> <out.md>\n  hc_hiss_tools airlab <in.wav> <out.md> [artLoHz artHiHz artDb]\n"
                 "  hc_hiss_tools proof inject <comb|wobble|clicks|echo|notch> <in.wav> <out.wav> <numbers...>\n"
                 "      comb   lineDb loHz hiHz seed\n      wobble modHz depth loHz hiHz\n      clicks periodMs peakDb startMs\n      echo   delayMs gainDb\n      notch  f0Hz f1Hz depthDb sigmaHz\n"
                 "  hc_hiss_tools proof pack <outdir> <salt> <a1.wav> <b1.wav> [...]\n  hc_hiss_tools proof hash <file...>\n  hc_hiss_tools proof strip <in.wav> <outdir>\n"
                 "  hc_hiss_tools tamer <in.wav> <outdir> <proof.txt>\n";
    return 1;
}

bool load(const std::string& p, wav::File& f) {
    std::string err;
    if (!wav::read(p, f, err)) { std::cerr << err << "\n"; return false; }
    return true;
}

std::string base(const std::string& p) { return fsys::path(p).stem().string(); }

int runTool(const std::string& which, const std::string& in, const std::string& out, const std::vector<double>& extra) {
    wav::File f;
    if (!load(in, f)) return 2;
    const std::size_t n = f.left.size();
    tools::ToolOutput o;
    if (which == "scanner") o = tools::hissScanner(f.left.data(), f.right.data(), n, f.sampleRate, base(in));
    else if (which == "combclick") o = tools::combAndClickFinder(f.left.data(), f.right.data(), n, f.sampleRate, base(in));
    else if (extra.size() == 3) o = tools::airAndSmearLab(f.left.data(), f.right.data(), n, f.sampleRate, base(in), extra[0], extra[1], extra[2]);
    else o = tools::airAndSmearLab(f.left.data(), f.right.data(), n, f.sampleRate, base(in));
    std::ofstream(out, std::ios::binary) << o.text;
    std::cout << o.text;
    return 0;
}

int proofInject(int argc, char** argv) {
    if (argc < 7) return usage();
    const std::string kind = argv[3], in = argv[4], out = argv[5];
    std::vector<double> v;
    for (int i = 6; i < argc; ++i) v.push_back(std::atof(argv[i]));
    wav::File f;
    if (!load(in, f)) return 2;
    const double fs = f.sampleRate;
    auto doOne = [&](std::vector<float>& x) -> bool {
        if (kind == "comb" && v.size() >= 4) lab::maker::injectComb(x, fs, v[0], v[1], v[2], static_cast<std::uint64_t>(v[3]));
        else if (kind == "wobble" && v.size() >= 4) lab::maker::injectBandWobble(x, fs, v[0], v[1], v[2], v[3]);
        else if (kind == "clicks" && v.size() >= 3) lab::maker::injectSeamClicks(x, fs, v[0], v[1], v[2]);
        else if (kind == "echo" && v.size() >= 2) lab::maker::injectEcho(x, fs, v[0], v[1]);
        else if (kind == "notch" && v.size() >= 4) lab::maker::injectNotchSweep(x, fs, v[0], v[1], v[2], v[3]);
        else return false;
        return true;
    };
    if (!doOne(f.left)) return usage();
    if (f.channels == 2) doOne(f.right); else f.right = f.left;
    if (!f.isFloat && f.bits < 24) f.bits = 24;     // a small injected problem sits near the 16-bit step; keep it
    std::string err;
    if (!wav::write(out, f, err)) { std::cerr << err << "\n"; return 2; }
    std::cout << "wrote " << out << " (problem: " << kind << "; the answer is the numbers you gave)\n";
    return 0;
}

int proof(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string sub = argv[2];
    if (sub == "inject") return proofInject(argc, argv);
    if (sub == "hash") { for (int i = 3; i < argc; ++i) std::cout << pack::fileHash(argv[i]) << "  " << base(argv[i]) << "\n"; return 0; }
    if (sub == "strip" && argc >= 5) { std::string outPath, err; if (!pack::stripName(argv[3], argv[4], outPath, err)) { std::cerr << err << "\n"; return 2; } std::cout << outPath << "\n"; return 0; }
    if (sub == "pack" && argc >= 7 && (argc - 5) % 2 == 0) {
        std::vector<std::string> a, b; std::string err;
        for (int i = 5; i + 1 < argc; i += 2) { a.push_back(argv[i]); b.push_back(argv[i + 1]); }
        if (!pack::makePack(a, b, argv[3], argv[4], err)) { std::cerr << err << "\n"; return 2; }
        std::cout << "pack written to " << argv[3] << "; publish COMMIT.txt first, keep KEY.txt private until the listening is done\n";
        return 0;
    }
    return usage();
}

int tamer(const std::string& in, const std::string& outDir, const std::string& proofPath) {
    std::string text;
    { std::ifstream p(proofPath, std::ios::binary); std::stringstream ss; ss << p.rdbuf(); text = ss.str(); }
    const auto gate = report::tamerGate(text);
    if (!gate.open) { std::cout << "REFUSED. " << gate.reason << "\nThe file was not touched and nothing was written.\n"; return 3; }
    wav::File f;
    if (!load(in, f)) return 2;
    AutoOptions opt;
    opt.progress = [](const std::string& s) { std::cout << "   - " << s << "\n" << std::flush; };
    const AutoResult r = autoFix(f.left.data(), f.right.data(), f.left.size(), f.sampleRate, opt, base(in));
    fsys::create_directories(outDir);
    wav::File fixed = f; fixed.left = r.left; fixed.right = r.right;
    std::string err;
    if (!wav::write((fsys::path(outDir) / (base(in) + "_fixed.wav")).string(), fixed, err)) { std::cerr << err << "\n"; return 2; }
    std::ofstream(fsys::path(outDir) / (base(in) + "_report.txt"), std::ios::binary) << gate.reason << "\n\n" << r.report;
    std::cout << gate.reason << "\n" << r.report;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string c = argv[1];
    if ((c == "scanner" || c == "combclick" || c == "airlab") && argc >= 4) {
        std::vector<double> extra;
        for (int i = 4; i < argc; ++i) extra.push_back(std::atof(argv[i]));
        return runTool(c, argv[2], argv[3], extra);
    }
    if (c == "proof") return proof(argc, argv);
    if (c == "tamer" && argc >= 5) return tamer(argv[2], argv[3], argv[4]);
    return usage();
}
