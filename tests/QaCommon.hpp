#pragma once
// =============================================================================
//  HEM QA pass — shared pieces (plain C++, no plug-in SDK, no GPU, no sound card).
//  The same file sits in HybridCore, HEM-Elite-Rack and HEM Dark Matter's tests.
//  Report only: it writes a CSV of findings and never changes a plug-in.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hemqa {

inline constexpr double kRate = 48000.0;
inline constexpr int    kBlock = 512;
inline constexpr int    kMaxSweepValues = 64;   // a choice with more steps is sampled evenly (noted in the report)

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    [[nodiscard]] std::size_t size() const noexcept { return l.size(); }
};

// The test signal: 100 Hz, 1 kHz and 5 kHz sines at -18 dBFS each, pink noise at about -24 dBFS RMS,
// a hot 2 kHz burst peaking at -1 dBFS at 0.25 s, and the left / right sides slightly different
// (so stereo processing has something to do). Deterministic.
inline Stereo testAudio(double seconds) {
    const std::size_t n = static_cast<std::size_t>(seconds * kRate);
    Stereo s(n);
    const double tau = 6.283185307179586;
    const float sine = std::pow(10.0f, -18.0f / 20.0f);
    std::uint32_t seed = 0x48454D51u;   // "HEMQ"
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        seed = seed * 1664525u + 1013904223u;
        const float white = (static_cast<float>(seed >> 8) / 16777216.0f) * 2.0f - 1.0f;
        b0 = 0.99886f * b0 + white * 0.0555179f; b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f; b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f; b5 = -0.7616f * b5 - white * 0.0168980f;
        const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.11f;
        b6 = white * 0.115926f;
        const float pinkScaled = pink * 0.35f;
        const float base = sine * static_cast<float>(std::sin(tau * 100.0 * t) + std::sin(tau * 1000.0 * t) + std::sin(tau * 5000.0 * t));
        s.l[i] = base + pinkScaled;
        s.r[i] = sine * static_cast<float>(std::sin(tau * 100.0 * t + 0.3) + std::sin(tau * 1000.0 * t + 0.7) + std::sin(tau * 5000.0 * t + 1.1))
                 + pinkScaled * 0.9f;
    }
    // hot transient: replaces the bed for 10 ms, peak 0.891 (-1 dBFS)
    const std::size_t at = static_cast<std::size_t>(0.25 * kRate);
    for (std::size_t k = 0; k < 480 && at + k < n; ++k) {
        const double t = static_cast<double>(k) / kRate;
        const float v = 0.891f * static_cast<float>(std::sin(tau * 2000.0 * t + 1.5707963) * std::exp(-t / 0.002));
        s.l[at + k] = v;
        s.r[at + k] = v * 0.95f;
    }
    return s;
}

[[nodiscard]] inline bool finite(const Stereo& s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}

// FNV-1a over the exact float bits (bit-identical output = same hash).
struct Hash {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* p, std::size_t bytes) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < bytes; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    void add(float v) noexcept { add(&v, sizeof v); }
    void add(const Stereo& s) noexcept { add(s.l.data(), s.l.size() * sizeof(float)); add(s.r.data(), s.r.size() * sizeof(float)); }
};

// The values a control is swept through: min, default, max, and every step of a stepped control.
inline std::vector<float> sweepValues(float mn, float def, float mx, bool stepped, bool* sampled = nullptr) {
    std::vector<float> v{mn, def, mx};
    if (sampled) *sampled = false;
    if (stepped) {
        const int lo = static_cast<int>(std::lround(mn)), hi = static_cast<int>(std::lround(mx));
        const int count = hi - lo + 1;
        if (count <= kMaxSweepValues) {
            for (int k = lo; k <= hi; ++k) v.push_back(static_cast<float>(k));
        } else {
            if (sampled) *sampled = true;
            for (int k = 0; k < kMaxSweepValues; ++k) v.push_back(static_cast<float>(lo + (count - 1) * k / (kMaxSweepValues - 1)));
        }
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

[[nodiscard]] inline bool closeTo(float a, float b) noexcept {
    return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

struct Rect { float x, y, w, h; };
[[nodiscard]] inline bool overlaps(const Rect& a, const Rect& b) noexcept {   // half a unit of slack, like the faceplate tests
    return a.x < b.x + b.w - 0.5f && b.x < a.x + a.w - 0.5f && a.y < b.y + b.h - 0.5f && b.y < a.y + a.h - 0.5f;
}
[[nodiscard]] inline bool within(const Rect& a, const Rect& box) noexcept {
    return a.x >= box.x - 0.5f && a.y >= box.y - 0.5f && a.x + a.w <= box.x + box.w + 0.5f && a.y + a.h <= box.y + box.h + 0.5f;
}

// ---- the report -------------------------------------------------------------------------------
struct Finding { std::string plugin, stage, flag, item, detail; };

class Report {
public:
    explicit Report(std::string product) : product_(std::move(product)) {}
    void plugin(const std::string& name) { if (std::find(plugins_.begin(), plugins_.end(), name) == plugins_.end()) plugins_.push_back(name); }
    void add(const std::string& plugin, const char* stage, const char* flag, const std::string& item, const std::string& detail) {
        this->plugin(plugin);
        findings_.push_back({plugin, stage, flag, item, detail});
    }
    void note(const std::string& plugin, const std::string& what) { notes_.push_back({plugin, "NOTE", "NOTE", "", what}); }

    [[nodiscard]] const std::vector<Finding>& findings() const noexcept { return findings_; }

    // plugin,stage,flag,item,detail — one row per finding, a PASS row for a plug-in with none, NOTE rows last.
    bool write(const std::filesystem::path& file) const {
        std::ofstream f(file, std::ios::binary);
        if (!f) return false;
        f << "plugin,stage,flag,item,detail\n";
        std::set<std::string> failed;
        for (const Finding& x : findings_) failed.insert(x.plugin);
        for (const std::string& p : plugins_) {
            if (!failed.count(p)) { f << csv(p) << ",ALL,PASS,,\n"; continue; }
            for (const Finding& x : findings_)
                if (x.plugin == p) f << csv(x.plugin) << ',' << csv(x.stage) << ',' << csv(x.flag) << ',' << csv(x.item) << ',' << csv(x.detail) << '\n';
        }
        for (const Finding& x : notes_) f << csv(x.plugin) << ",NOTE,NOTE,," << csv(x.detail) << '\n';
        return static_cast<bool>(f);
    }

    void printSummary() const {
        std::set<std::string> failed;
        std::map<std::string, int> byFlag;
        for (const Finding& x : findings_) { failed.insert(x.plugin); ++byFlag[x.flag]; }
        std::printf("\n[QA] %s: %zu plug-ins tested, %zu clean, %zu with findings\n", product_.c_str(), plugins_.size(),
                    plugins_.size() - failed.size(), failed.size());
        for (const auto& [flag, n] : byFlag) std::printf("[QA]   %-20s %d\n", flag.c_str(), n);
    }

private:
    static std::string csv(const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c == '\n' ? ' ' : c; }
        return o + '"';
    }
    std::string product_;
    std::vector<std::string> plugins_;
    std::vector<Finding> findings_;
    std::vector<Finding> notes_;
};

// Where the CSV goes: HEM_QA_DIR if set, else the working directory.
inline std::filesystem::path outDir() {
    if (const char* d = std::getenv("HEM_QA_DIR"); d && *d) return d;
    return std::filesystem::current_path();
}

inline std::string fmt(const char* f, double a) { char b[64]; std::snprintf(b, sizeof b, f, a); return b; }

} // namespace hemqa
