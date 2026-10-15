#pragma once
// =============================================================================
//  Settings — small INI store ([section] key=value) with crash-safe saving
//  (write temp file, then atomic rename over the old one).
//  Default location: %APPDATA%\HybridCore\settings.ini — never touched by the installer.
// =============================================================================
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace hc::params {

class Settings {
public:
    void        set(const std::string& section, const std::string& key, const std::string& value);
    [[nodiscard]] std::string get(const std::string& section, const std::string& key,
                                  const std::string& fallback = {}) const;
    void        setSectionText(const std::string& section, std::string_view keyValueLines);
    [[nodiscard]] std::string sectionText(const std::string& section) const;   // "key=value\n"...

    [[nodiscard]] std::string serialize() const;
    void        parse(std::string_view text);

    bool save(const std::filesystem::path& file) const;
    bool load(const std::filesystem::path& file);

    [[nodiscard]] static std::filesystem::path appDataDir();     // %APPDATA%\HybridCore (created)
    [[nodiscard]] static std::filesystem::path defaultFile();    // appDataDir()/settings.ini

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

} // namespace hc::params
