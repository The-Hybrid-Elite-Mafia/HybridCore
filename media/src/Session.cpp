#include "hybrid/media/Session.hpp"
#include "hybrid/params/Settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace hc::media {

namespace {
std::string toUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string slotKey(std::uint32_t s) {
    char b[16];
    std::snprintf(b, sizeof b, "slot%02u", s + 1);
    return b;
}
} // namespace

bool saveSession(const std::filesystem::path& file, const SessionData& d, const std::string& appVersion, std::string* error) {
    params::Settings s;
    s.set("session", "format", "HybridCore Session");
    s.set("session", "version", std::to_string(kSessionVersion));
    s.set("session", "app", appVersion);
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::absolute(file, ec).parent_path();
    for (std::uint32_t i = 0; i < hc::kStemCount; ++i) {
        if (d.stems[i].empty()) continue;
        const std::filesystem::path abs = std::filesystem::absolute(d.stems[i], ec).lexically_normal();
        const std::filesystem::path rel = abs.lexically_relative(base);
        // Relative only when it is a real relative path (same drive); otherwise the absolute path alone.
        if (!rel.empty() && rel.is_relative()) s.set("stems", slotKey(i), toUtf8(rel.generic_u8string()));
        s.set("stems", slotKey(i) + ".abs", toUtf8(abs));
    }
    s.setSectionText("params", d.params);
    s.set("state", "loop", d.loop ? "1" : "0");
    s.set("state", "page", std::to_string(d.page));
    s.set("state", "faderBank", std::to_string(d.faderBank));
    s.set("state", "source", d.source);
    s.set("state", "playhead", std::to_string(d.playhead));
    if (!s.save(file)) {
        if (error) *error = "could not write " + toUtf8(file.filename()) + " (folder read-only or disk full?)";
        return false;
    }
    return true;
}

std::expected<LoadedSession, std::string> loadSession(const std::filesystem::path& file) {
    params::Settings s;
    if (!s.load(file)) return std::unexpected("could not open " + toUtf8(file.filename()));
    if (s.get("session", "format") != "HybridCore Session") return std::unexpected(toUtf8(file.filename()) + " is not a Hybrid Core session");
    const int version = std::atoi(s.get("session", "version", "0").c_str());
    if (version < 1 || version > kSessionVersion)
        return std::unexpected(toUtf8(file.filename()) + " was saved by a newer version (session format " + std::to_string(version) + ")");

    LoadedSession out;
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::absolute(file, ec).parent_path();
    for (std::uint32_t i = 0; i < hc::kStemCount; ++i) {
        const std::string rel = s.get("stems", slotKey(i));
        const std::string abs = s.get("stems", slotKey(i) + ".abs");
        if (rel.empty() && abs.empty()) continue;
        std::filesystem::path found;
        if (!rel.empty() && std::filesystem::is_regular_file(base / fromUtf8(rel), ec)) found = (base / fromUtf8(rel)).lexically_normal();
        else if (!abs.empty() && std::filesystem::is_regular_file(fromUtf8(abs), ec)) found = fromUtf8(abs);
        if (found.empty()) out.missing.push_back("slot " + std::to_string(i + 1) + ": " + toUtf8(fromUtf8(abs.empty() ? rel : abs).filename()));
        else out.data.stems[i] = found;
    }
    out.data.params    = s.sectionText("params");
    out.data.loop      = s.get("state", "loop", "1") != "0";
    out.data.page      = static_cast<std::uint32_t>(std::clamp(std::atoi(s.get("state", "page", "0").c_str()), 0, 2));
    out.data.faderBank = static_cast<std::uint32_t>(std::clamp(std::atoi(s.get("state", "faderBank", "0").c_str()), 0, 3));
    out.data.source    = s.get("state", "source", "stems");
    out.data.playhead  = std::strtoull(s.get("state", "playhead", "0").c_str(), nullptr, 10);
    return out;
}

} // namespace hc::media
