#pragma once
// =============================================================================
//  Session files (.hcs) — one song: which file sits in each of the 13 stem slots,
//  every mixer parameter, and the transport / view state.
//
//  Text (INI) so it survives upgrades and can be read by a person:
//    [session] format=HybridCore Session, version=1, app=<version that wrote it>
//    [stems]   slot01=<path relative to the .hcs folder>   slot01.abs=<absolute path>
//    [params]  <id>=<value> for every parameter (ParameterTree::serialize)
//    [state]   loop, page, faderBank, source, playhead (frames at 48 kHz)
//  Saving is crash-safe (temp file + rename, via params::Settings).
//  Loading resolves each stem relative to the session file first (a song folder that was
//  moved or copied to another drive still opens), then by its absolute path.
// =============================================================================
#include "hybrid/core/Config.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace hc::media {

struct SessionData {
    std::array<std::filesystem::path, hc::kStemCount> stems{};   // empty = slot not used
    std::string   params;              // ParameterTree::serialize() text
    bool          loop      = true;
    std::uint32_t page      = 0;
    std::uint32_t faderBank = 0;
    std::string   source    = "stems";
    std::uint64_t playhead  = 0;
};

struct LoadedSession {
    SessionData                        data;      // stems resolved to files that exist
    std::vector<std::string>           missing;   // "slot 3: Bass.wav" for stems that could not be found
};

inline constexpr const char* kSessionExtension = ".hcs";
inline constexpr int         kSessionVersion   = 1;

[[nodiscard]] bool saveSession(const std::filesystem::path& file, const SessionData& data, const std::string& appVersion,
                               std::string* error = nullptr);
[[nodiscard]] std::expected<LoadedSession, std::string> loadSession(const std::filesystem::path& file);

} // namespace hc::media
