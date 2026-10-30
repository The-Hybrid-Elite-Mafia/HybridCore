#pragma once
// =============================================================================
//  HEM Hiss Report (engine 20) + blind plan (engine 18, the pure part) + Hiss Tamer gate.
//  Pure C++23, no JUCE. Offline only (allocates strings).
//  A report never gives a bare number: every finding carries a range and a status word.
//  Status words: TESTED (checked on made-up signals with a known answer, and/or against an outside tool),
//                ASSUMED (a working guess nobody has checked), UNKNOWN (cannot be given).
//  TESTED here always means "on our own test signals". Real Suno files are UNKNOWN until a real-file run is done.
// =============================================================================
#include <cstddef>
#include <string>
#include <vector>

namespace hc::hiss::report {

struct Finding {
    std::string name;
    double value = 0, lo = 0, hi = 0;     // the range is where the true value should sit if the meter behaves as it did in its tests
    std::string unit;
    std::string status;                   // "TESTED" / "ASSUMED" / "UNKNOWN"
    std::string note;                     // plain words
    bool hasNumber = true;                // false: the meter refused; the line says why in `note`
};

[[nodiscard]] Finding withAbsRange(std::string name, double v, double tol, std::string unit, std::string status, std::string note = {});
[[nodiscard]] Finding withRelRange(std::string name, double v, double relTol, std::string unit, std::string status, std::string note = {});
[[nodiscard]] Finding noRange(std::string name, double v, std::string unit, std::string status, std::string note);   // a number with no range (the range is UNKNOWN)
[[nodiscard]] Finding refused(std::string name, std::string why);
// A clean negative: the meter ran and found nothing (listed under "Checked, nothing found", not under "Could not be measured").
[[nodiscard]] Finding nothingFound(std::string name, std::string what, std::string status);

// Plain-words page. Bad news first: refused and UNKNOWN lines are listed before the numbers.
[[nodiscard]] std::string makeReport(const std::string& title, const std::vector<Finding>& findings, const std::vector<std::string>& caveats);

// ---- Blind listening plan (engine 18) ------------------------------------------------------------------------------
// For pair i, which of A / B is played as "X". Decided only by the file fingerprints, the salt and the pair number.
// `commitHex` is the SHA-256 of the key text. Publish it BEFORE listening; it proves the key was not changed afterwards.
struct BlindPlan {
    std::vector<bool> xIsA;
    std::string keyText;
    std::string commitHex;
};
[[nodiscard]] BlindPlan blindPlan(const std::vector<std::string>& hashA, const std::vector<std::string>& hashB, const std::string& salt);

// ---- Hiss Tamer gate ----------------------------------------------------------------------------------------------------
// Steven's rule: the fixer waits until a cause is proven. The proof file is plain text with these lines (any order):
//    cause_proven: yes
//    blind_result: <where the blind result is written down>
//    evidence: <what was measured>
// No file, a missing line, or anything but "yes" = refuse.
struct GateResult { bool open = false; std::string reason; };
[[nodiscard]] GateResult tamerGate(const std::string& proofText);

} // namespace hc::hiss::report
