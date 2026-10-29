#pragma once
// =============================================================================
//  HEM DIGITAL CONSOLE — files around a session (hc::console):
//    * which desk channel each stem goes to (Suno stem names get fixed channels, the rest go in name order)
//    * finding stems that were moved (relink by file name in a folder the user points at)
//    * the recent-sessions list
//  Pure C++ (no Windows calls), so it is tested on every platform the tests run on.
// =============================================================================
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hc::console {

inline constexpr int kDeskChannels = 13;   // == ConsoleRig::kChannels (checked in SessionFiles.cpp)

// ---- stem -> channel ----------------------------------------------------------------------------------------
// The fixed channel of each stem a Suno "Get Stems" export can contain. Real names seen on the rig (Oct 2026):
// "0 Lead Vocals.wav", "8 Backing_Vocals.wav", "9 Vocals.wav", "Two hundred forty days (Vocals).wav"; file 0 can be
// the whole song ("0 There's a Difference.wav").
struct StemRole {
    const char* name;      // shown to the user ("LEAD VOCALS")
    int channel;           // 0-based desk channel
};
[[nodiscard]] const std::vector<StemRole>& stemRoles();
// The role a file name matches, or -1. Leading track numbers, separators and the song's name (taken from the folder,
// "<song> Stems") are ignored: "0 Lead Vocals", "9 Backing_Vocals", "Two hundred forty days (Vocals)" all match.
[[nodiscard]] int stemRoleOf(const std::filesystem::path& file);
// A finished mix rather than a stem: the name is just the song's name (Suno's "0 <song>.wav") or says master / full
// mix / mixdown ("<song> - HEM Master.wav").
[[nodiscard]] bool isFullMix(const std::filesystem::path& file);

struct StemPlan {
    std::array<std::filesystem::path, kDeskChannels> channel{};   // empty = nothing for that channel
    std::vector<std::filesystem::path> leftOut;                   // more than 13 files: these did not fit
    std::vector<std::filesystem::path> fullMix;                   // whole-song mixes / masters found among the files
    int byName = 0;                                              // how many went to their Suno channel
};
// `files` in any order (folders already expanded). Known stems go to their own channel; the rest fill the free
// channels in natural name order (lowest channel first). Only the first 13 files that fit are used. Full mixes and
// masters are set aside in `fullMix` (the app puts the first one in EXT 1 as a reference).
[[nodiscard]] StemPlan planStems(std::vector<std::filesystem::path> files);

// ---- moved stems --------------------------------------------------------------------------------------------
// For each wanted file, the file with the same name (case-insensitive) in `folder` or its sub-folders (2 levels).
// Missing ones stay empty. `wanted` holds the old full paths (only their file names are used).
[[nodiscard]] std::vector<std::filesystem::path> relinkStems(const std::vector<std::filesystem::path>& wanted, const std::filesystem::path& folder);

// ---- recent sessions ----------------------------------------------------------------------------------------
inline constexpr int kRecentMax = 10;
// Puts `file` first, drops older copies of it (same path, case-insensitive) and anything past kRecentMax.
void pushRecent(std::vector<std::filesystem::path>& list, const std::filesystem::path& file);

} // namespace hc::console
