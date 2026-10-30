#include "hybrid/hiss/HissReport.hpp"

#include "hybrid/hiss/HissLab.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace hc::hiss::report {

Finding withAbsRange(std::string name, double v, double tol, std::string unit, std::string status, std::string note) {
    Finding f; f.name = std::move(name); f.value = v; f.lo = v - tol; f.hi = v + tol; f.unit = std::move(unit); f.status = std::move(status); f.note = std::move(note);
    if (f.unit == "%") { f.lo = std::max(0.0, f.lo); f.hi = std::min(100.0, f.hi); }
    return f;
}
Finding withRelRange(std::string name, double v, double rel, std::string unit, std::string status, std::string note) {
    return withAbsRange(std::move(name), v, std::fabs(v) * rel, std::move(unit), std::move(status), std::move(note));
}
Finding noRange(std::string name, double v, std::string unit, std::string status, std::string note) {
    Finding f; f.name = std::move(name); f.value = v; f.lo = f.hi = std::nan(""); f.unit = std::move(unit); f.status = std::move(status); f.note = std::move(note);
    return f;
}
Finding nothingFound(std::string name, std::string what, std::string status) {
    Finding f; f.name = std::move(name); f.hasNumber = false; f.status = std::move(status); f.note = std::move(what);
    return f;
}
Finding refused(std::string name, std::string why) {
    Finding f; f.name = std::move(name); f.hasNumber = false; f.status = "UNKNOWN"; f.note = std::move(why);
    return f;
}

std::string makeReport(const std::string& title, const std::vector<Finding>& findings, const std::vector<std::string>& caveats) {
    std::ostringstream o;
    o << "# " << title << "\n\n";
    int refusedCount = 0, nothing = 0, assumed = 0, tested = 0;
    for (const auto& f : findings) {
        if (!f.hasNumber) { if (f.status == "UNKNOWN") ++refusedCount; else ++nothing; }
        else if (f.status == "ASSUMED") ++assumed; else if (f.status == "TESTED") ++tested;
    }
    o << "Lines: " << findings.size() << ". Could not be measured: " << refusedCount << ". Checked, nothing found: " << nothing << ". ASSUMED: " << assumed << ". TESTED on made-up signals: " << tested << ".\n";
    o << "TESTED here means checked on signals we made ourselves, where the right answer was known. It does not mean checked on real Suno files.\n\n";
    if (refusedCount > 0) {
        o << "## Could not be measured (bad news first)\n";
        for (const auto& f : findings) if (!f.hasNumber && f.status == "UNKNOWN") o << "- " << f.name << ": " << f.note << " [" << f.status << "]\n";
        o << "\n";
    }
    if (nothing > 0) {
        o << "## Checked, nothing found\n";
        for (const auto& f : findings) if (!f.hasNumber && f.status != "UNKNOWN") o << "- " << f.name << ": " << f.note << " [" << f.status << "]\n";
        o << "\n";
    }
    o << "## Readings (value, then the range it should sit in)\n";
    char b[512];
    for (const auto& f : findings) {
        if (!f.hasNumber) continue;
        if (std::isnan(f.lo)) std::snprintf(b, sizeof b, "- %s: %.4g %s  (range UNKNOWN)  [%s]", f.name.c_str(), f.value, f.unit.c_str(), f.status.c_str());
        else std::snprintf(b, sizeof b, "- %s: %.4g %s  (range %.4g to %.4g)  [%s]", f.name.c_str(), f.value, f.unit.c_str(), f.lo, f.hi, f.status.c_str());
        o << b;
        if (!f.note.empty()) o << "  " << f.note;
        o << "\n";
    }
    if (!caveats.empty()) {
        o << "\n## Read this before trusting any of it\n";
        for (const auto& c : caveats) o << "- " << c << "\n";
    }
    return o.str();
}

BlindPlan blindPlan(const std::vector<std::string>& hashA, const std::vector<std::string>& hashB, const std::string& salt) {
    BlindPlan p;
    const std::size_t n = std::min(hashA.size(), hashB.size());
    std::ostringstream key;
    key << "salt " << salt << "\n";
    for (std::size_t i = 0; i < n; ++i) {
        const std::string s = lab::sha256Hex((hashA[i] + hashB[i] + salt + std::to_string(i)).data(), hashA[i].size() + hashB[i].size() + salt.size() + std::to_string(i).size());
        const bool xa = ((std::stoul(s.substr(0, 2), nullptr, 16)) & 1u) != 0u;
        p.xIsA.push_back(xa);
        char line[256];
        std::snprintf(line, sizeof line, "pair %02zu: X is %s, Y is %s  (A %s, B %s)\n", i + 1, xa ? "A" : "B", xa ? "B" : "A", hashA[i].substr(0, 12).c_str(), hashB[i].substr(0, 12).c_str());
        key << line;
    }
    p.keyText = key.str();
    p.commitHex = lab::sha256Hex(p.keyText.data(), p.keyText.size());
    return p;
}

GateResult tamerGate(const std::string& proofText) {
    GateResult g;
    bool proven = false, blind = false, evidence = false;
    std::istringstream in(proofText);
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon), v = line.substr(colon + 1);
        auto trim = [](std::string& s) { while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin()); while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back(); };
        trim(k); trim(v);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string vl = v; std::transform(vl.begin(), vl.end(), vl.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "cause_proven" && vl == "yes") proven = true;
        if (k == "blind_result" && !v.empty()) blind = true;
        if (k == "evidence" && !v.empty()) evidence = true;
    }
    if (proofText.empty()) { g.reason = "No proof file. The Hiss Tamer waits until a cause is proven."; return g; }
    if (!proven) { g.reason = "The proof file does not say 'cause_proven: yes'."; return g; }
    if (!blind) { g.reason = "The proof file has no 'blind_result:' line."; return g; }
    if (!evidence) { g.reason = "The proof file has no 'evidence:' line."; return g; }
    g.open = true;
    g.reason = "Proof file has all three lines. The Hiss Tamer may run. (It still checks its own work and hands the file back untouched when it is not sure.)";
    return g;
}

} // namespace hc::hiss::report
