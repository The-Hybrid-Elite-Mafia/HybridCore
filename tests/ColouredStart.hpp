#pragma once
// The settings these tests (and the QA report's extra context) start from: each plug-in's defaults from
// before Steven's zero-colour rule. The plug-ins now LOAD with every colour control at zero and pass the
// sound untouched; the quality checks still exercise the DSP from this coloured starting point.
// Generated from the measured old / new defaults (19 plug-ins, 27 controls).
#include <string>
#include <utility>
#include <vector>

namespace colourstart {

struct Entry {
    const char* name;
    std::vector<std::pair<const char*, float>> values;
};

inline const std::vector<Entry>& table() {
    static const std::vector<Entry> t = {
        {"HEM BALANCE", {{"amount", 50.0f}}},
        {"HEM BLOOM", {{"mix", 25.0f}}},
        {"HEM BUS PRESS", {{"mix", 100.0f}}},
        {"HEM CALM", {{"depth", 5.0f}, {"mix", 100.0f}}},
        {"HEM CEILING", {{"ceiling", -1.0f}}},
        {"HEM CLIP", {{"mix", 100.0f}}},
        {"HEM DE-ESS", {{"range", 10.0f}}},
        {"HEM DESK", {{"drive", 6.0f}, {"crosstalk", 30.0f}, {"mix", 100.0f}}},
        {"HEM DOUBLE", {{"amount", 50.0f}}},
        {"HEM DUST", {{"wow", 20.0f}, {"flutter", 15.0f}, {"vinyl", 20.0f}, {"mix", 100.0f}}},
        {"HEM ECHO", {{"mix", 25.0f}}},
        {"HEM HEAT", {{"drive", 6.0f}, {"mix", 100.0f}}},
        {"HEM HUSH", {{"amount", 50.0f}}},
        {"HEM PUNCH", {{"mix", 100.0f}}},
        {"HEM SWIRL KILLER", {{"amount", 50.0f}}},
        {"HEM TRIBAND", {{"mix", 100.0f}}},
        {"HEM TUNE", {{"amount", 100.0f}}},
        {"HEM VOX ONE", {{"amount", 40.0f}, {"mix", 100.0f}}},
        {"HEM WIDE", {{"monobelow", 120.0f}}},
    };
    return t;
}

// The old defaults for the plug-in called `name` (empty when nothing changed).
inline const std::vector<std::pair<const char*, float>>& forName(const std::string& name) {
    static const std::vector<std::pair<const char*, float>> none;
    for (const auto& e : table()) if (name == e.name) return e.values;
    return none;
}

} // namespace colourstart
