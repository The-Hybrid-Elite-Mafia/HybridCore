#pragma once
// =============================================================================
//  The four measuring tools of the HEM Hiss set. Each one bundles engines and returns a plain-words report with ranges.
//  (The fifth tool, the Hiss Tamer, is the existing fixer plus its proof gate: HissReport.hpp tamerGate + HissAuto.hpp autoFix.)
//    Hiss Scanner          engines 2 (loudness, peaks), 4 (bandwidth audit), 5 (band levels, 16-20 kHz ratio), 6 (loud-part detector)
//    Comb and Click Finder engines 7 (25 Hz comb), 8 (40 ms seam clicks)
//    Air and Smear Lab     engines 9 (notch), 10 (flutter), 11 (smear), 12 (masking), 13 (hit average), 14 (echo), 15 (wobble), 16 (phase)
//    Proof Bench           engines 17 (maker), 18 (blind plan), 19 (fingerprint), 20 (report)
//  Offline only. Mono inputs are the average of left and right; stereo is used where an engine needs it (phase).
// =============================================================================
#include "hybrid/hiss/HissReport.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace hc::hiss::tools {

struct ToolOutput {
    std::vector<report::Finding> findings;
    std::string text;       // the finished plain-words page
};

[[nodiscard]] ToolOutput hissScanner(const float* left, const float* right, std::size_t n, double fs, const std::string& name);
[[nodiscard]] ToolOutput combAndClickFinder(const float* left, const float* right, std::size_t n, double fs, const std::string& name);
// artifactDb < -150 means "no artifact level given": the masking line is then reported as not run.
[[nodiscard]] ToolOutput airAndSmearLab(const float* left, const float* right, std::size_t n, double fs, const std::string& name,
                                        double artifactLoHz = 11000.0, double artifactHiHz = 12000.0, double artifactDb = -200.0);

} // namespace hc::hiss::tools
