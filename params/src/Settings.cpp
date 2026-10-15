#include "hybrid/params/Settings.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace hc::params {

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
} // namespace

void Settings::set(const std::string& section, const std::string& key, const std::string& value) {
    data_[section][key] = value;
}

std::string Settings::get(const std::string& section, const std::string& key, const std::string& fallback) const {
    const auto s = data_.find(section);
    if (s == data_.end()) return fallback;
    const auto k = s->second.find(key);
    return k == s->second.end() ? fallback : k->second;
}

void Settings::setSectionText(const std::string& section, std::string_view text) {
    auto& sec = data_[section];
    sec.clear();
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const auto eq = line.find('=');
        if (line.empty() || eq == std::string_view::npos) continue;
        sec[std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
    }
}

std::string Settings::sectionText(const std::string& section) const {
    std::string out;
    const auto s = data_.find(section);
    if (s == data_.end()) return out;
    for (const auto& [k, v] : s->second) out += k + "=" + v + "\n";
    return out;
}

std::string Settings::serialize() const {
    std::string out = "; The Hybrid Core settings. Safe to delete: defaults are restored on next start.\n";
    for (const auto& [section, kv] : data_) {
        out += "\n[" + section + "]\n";
        for (const auto& [k, v] : kv) out += k + "=" + v + "\n";
    }
    return out;
}

void Settings::parse(std::string_view text) {
    data_.clear();
    std::string section = "general";
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        data_[section][std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
    }
}

bool Settings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string text = serialize();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(tmp, file, ec);   // replaces atomically on NTFS (MoveFileEx REPLACE_EXISTING)
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

bool Settings::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

std::filesystem::path Settings::appDataDir() {
    std::filesystem::path base;
#if defined(_WIN32)
    wchar_t* v = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&v, &len, L"APPDATA") == 0 && v) { base = v; std::free(v); }
#else
    if (const char* h = std::getenv("HOME")) base = std::filesystem::path(h) / ".config";
#endif
    if (base.empty()) base = std::filesystem::temp_directory_path();
    std::filesystem::path dir = base / "HybridCore";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path Settings::defaultFile() { return appDataDir() / "settings.ini"; }

} // namespace hc::params
