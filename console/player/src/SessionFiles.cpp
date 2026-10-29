#include "hybrid/console/SessionFiles.hpp"

#include "hybrid/console/ConsoleRig.hpp"
#include "hybrid/media/StemLoader.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

namespace hc::console {

static_assert(kDeskChannels == ConsoleRig::kChannels, "SessionFiles: desk channel count changed");

namespace {

// "0_Lead_Vocals" -> "lead vocals"; "12 - FX" -> "fx"
std::string normalName(const std::filesystem::path& file) {
    const auto u = file.stem().u8string();
    std::string s;
    for (char8_t c8 : u) {
        const auto c = static_cast<unsigned char>(c8);
        if (c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']') s += ' ';
        else s += static_cast<char>(c < 128 ? std::tolower(c) : c);
    }
    // collapse spaces
    std::string out;
    for (char c : s) if (!(c == ' ' && (out.empty() || out.back() == ' '))) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // drop a leading track number ("0 lead vocals", "03 bass")
    std::size_t i = 0;
    while (i < out.size() && std::isdigit(static_cast<unsigned char>(out[i]))) ++i;
    if (i > 0 && i < out.size() && out[i] == ' ') out.erase(0, i + 1);
    return out;
}
bool hasWord(const std::string& s, std::string_view w) {
    for (std::size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) {
        const bool l = p == 0 || s[p - 1] == ' ';
        const bool r = p + w.size() == s.size() || s[p + w.size()] == ' ';
        if (l && r) return true;
    }
    return false;
}
bool hasPart(const std::string& s, std::string_view w) { return s.find(w) != std::string::npos; }

std::string lowerPath(const std::filesystem::path& p) {
    const auto u = p.u8string();
    std::string s;
    for (char8_t c : u) s += static_cast<char>(static_cast<unsigned char>(c) < 128 ? std::tolower(static_cast<unsigned char>(c)) : static_cast<unsigned char>(c));
    return s;
}

} // namespace

const std::vector<StemRole>& stemRoles() {
    static const std::vector<StemRole> roles = {
        {"LEAD VOCALS", 0},  {"BACKING VOCALS", 1}, {"DRUMS", 2},   {"PERCUSSION", 3}, {"BASS", 4},       {"GUITAR", 5},
        {"KEYBOARD", 6},     {"SYNTH", 7},          {"STRINGS", 8}, {"BRASS", 9},      {"WOODWINDS", 10}, {"FX", 11},
        {"OTHER", 12},
    };
    return roles;
}

namespace {
int roleOfName(const std::string& s) {
    if (s.empty()) return -1;
    // most specific first: "backing vocals" also contains "vocals", "bass drum" is a drum
    if (hasPart(s, "backing") || hasPart(s, "background vocal") || hasWord(s, "bgv") || hasWord(s, "bvs") || hasPart(s, "harmon")) return 1;
    if (hasPart(s, "vocal") || hasWord(s, "vox") || hasWord(s, "voice")) return 0;
    if (hasPart(s, "percussion") || hasWord(s, "perc")) return 3;
    if (hasPart(s, "drum")) return 2;
    if (hasPart(s, "bass")) return 4;
    if (hasPart(s, "guitar") || hasWord(s, "gtr")) return 5;
    if (hasPart(s, "keyboard") || hasWord(s, "keys") || hasPart(s, "piano")) return 6;
    if (hasPart(s, "synth")) return 7;
    if (hasPart(s, "string")) return 8;
    if (hasPart(s, "brass")) return 9;
    if (hasPart(s, "woodwind")) return 10;
    if (hasWord(s, "fx") || hasWord(s, "sfx") || hasPart(s, "effect")) return 11;
    if (hasWord(s, "other")) return 12;
    if (s == "lead") return 0;   // a stem called just "Lead" is the lead vocal ("Lead Synth" is a synth, above)
    return -1;
}
// The song's name from the stems folder: "Two hundred forty days Stems" -> "two hundred forty days".
std::string songTitle(const std::filesystem::path& file) {
    if (file.parent_path().filename().empty()) return {};
    std::string t = normalName(file.parent_path().filename().wstring() + L".x");
    for (const char* tail : {" stems", " stem"})
        if (t.size() > std::strlen(tail) && t.ends_with(tail)) { t.resize(t.size() - std::strlen(tail)); break; }
    return t;
}
// The part of a stem's name after the song's name: "Two hundred forty days (Vocals)" -> "vocals". Empty = the name
// is just the song's name (Suno's "0 <song>.wav": the whole song).
std::string partName(const std::filesystem::path& file, bool* isSong) {
    std::string n = normalName(file);
    const std::string t = songTitle(file);
    *isSong = false;
    if (!t.empty() && n == t) { *isSong = true; return {}; }
    if (!t.empty() && n.size() > t.size() + 1 && n.starts_with(t + " ")) n = n.substr(t.size() + 1);
    // "- HEM Master", "Full Mix", "Mixdown": a finished mix, not a stem
    // ("Two hundred forty days (Keyboard) - HEM MTwo hundred forty days - HEM Masteraster": a render whose name
    // got the song's name pasted into it again, also a finished mix)
    if (hasPart(n, "master") || hasPart(n, "full mix") || hasPart(n, "mixdown") || hasWord(n, "mixed") || (t.size() >= 4 && hasPart(n, t))) *isSong = true;
    return n;
}
} // namespace

int stemRoleOf(const std::filesystem::path& file) {
    bool song = false;
    const std::string n = partName(file, &song);
    return song ? -1 : roleOfName(n);
}
bool isFullMix(const std::filesystem::path& file) {
    bool song = false;
    (void)partName(file, &song);
    return song;
}

StemPlan planStems(std::vector<std::filesystem::path> files) {
    std::sort(files.begin(), files.end(), media::naturalLess);
    StemPlan plan;
    // finished mixes (the whole song, a master) never go on a desk channel
    std::erase_if(files, [&](const std::filesystem::path& f) {
        if (!isFullMix(f)) return false;
        plan.fullMix.push_back(f);
        return true;
    });
    // the plainest name first ("<song> - HEM Master" before a longer render name): that one goes to EXT 1
    std::stable_sort(plan.fullMix.begin(), plan.fullMix.end(), [](const auto& x, const auto& y) { return x.filename().native().size() < y.filename().native().size(); });
    // Each known channel takes the plainest name of its kind ("Drums" before "Drums 2"); ties go by name order.
    std::vector<int> chOf(files.size(), -1);
    std::array<int, kDeskChannels> pick;
    pick.fill(-1);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const int role = stemRoleOf(files[i]);
        if (role < 0) continue;
        const int ch = stemRoles()[static_cast<std::size_t>(role)].channel;
        int& cur = pick[static_cast<std::size_t>(ch)];
        if (cur < 0 || normalName(files[i]).size() < normalName(files[static_cast<std::size_t>(cur)]).size()) cur = static_cast<int>(i);
    }
    for (int ch = 0; ch < kDeskChannels; ++ch)
        if (const int i = pick[static_cast<std::size_t>(ch)]; i >= 0) { plan.channel[static_cast<std::size_t>(ch)] = files[static_cast<std::size_t>(i)]; chOf[static_cast<std::size_t>(i)] = ch; ++plan.byName; }
    std::vector<std::filesystem::path> rest;
    for (std::size_t i = 0; i < files.size(); ++i) if (chOf[i] < 0) rest.push_back(files[i]);
    for (const auto& f : rest) {
        auto it = std::find_if(plan.channel.begin(), plan.channel.end(), [](const auto& p) { return p.empty(); });
        if (it == plan.channel.end()) plan.leftOut.push_back(f);
        else *it = f;
    }
    return plan;
}

std::vector<std::filesystem::path> relinkStems(const std::vector<std::filesystem::path>& wanted, const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> out(wanted.size());
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return out;
    // the folder first, then deeper (a file in the picked folder wins over one with the same name further down)
    std::vector<std::filesystem::path> found;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= 2) it.disable_recursion_pending();
        if (it->is_regular_file(ec)) found.push_back(it->path());
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        const auto da = std::distance(a.begin(), a.end()), db = std::distance(b.begin(), b.end());
        return da < db;
    });
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i].empty()) continue;
        const std::string name = lowerPath(wanted[i].filename());
        for (const auto& f : found)
            if (lowerPath(f.filename()) == name) { out[i] = f; break; }
    }
    return out;
}

void pushRecent(std::vector<std::filesystem::path>& list, const std::filesystem::path& file) {
    if (file.empty()) return;
    const std::string key = lowerPath(file);
    std::erase_if(list, [&](const auto& p) { return p.empty() || lowerPath(p) == key; });
    list.insert(list.begin(), file);
    if (list.size() > static_cast<std::size_t>(kRecentMax)) list.resize(static_cast<std::size_t>(kRecentMax));
}

} // namespace hc::console
