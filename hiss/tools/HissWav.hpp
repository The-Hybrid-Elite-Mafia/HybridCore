#pragma once
// Small WAV reader / writer for the Hiss tools. PCM 16 / 24 / 32 and float 32. Mono or stereo.
// Offline only. A file read and written back with no change comes out bit for bit the same samples.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hc::hiss::wav {

struct File {
    int sampleRate = 48000;
    int channels = 2;       // as stored in the file (1 or 2)
    int bits = 24;          // 16, 24, 32
    bool isFloat = false;
    std::vector<float> left, right;   // right == left for mono
};

inline std::uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
inline std::uint16_t rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

inline bool read(const std::string& path, File& f, std::string& err) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = "cannot open " + path; return false; }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    std::size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, fp)) > 0) d.insert(d.end(), buf, buf + got);
    std::fclose(fp);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) { err = path + " is not a WAV file"; return false; }
    std::size_t pos = 12;
    int fmtTag = 0, ch = 0, bits = 0, rate = 0;
    const unsigned char* data = nullptr;
    std::size_t dataLen = 0;
    while (pos + 8 <= d.size()) {
        const std::uint32_t len = rd32(&d[pos + 4]);
        const unsigned char* body = &d[pos + 8];
        if (std::memcmp(&d[pos], "fmt ", 4) == 0 && len >= 16) {
            fmtTag = rd16(body); ch = rd16(body + 2); rate = static_cast<int>(rd32(body + 4)); bits = rd16(body + 14);
            if (fmtTag == 0xFFFE && len >= 26) fmtTag = rd16(body + 24);
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            data = body;
            dataLen = std::min<std::size_t>(len, d.size() - (pos + 8));
        }
        pos += 8 + static_cast<std::size_t>(len) + (len & 1u);
    }
    if (!data || ch < 1 || ch > 2) { err = path + ": only mono or stereo WAV files with a data chunk are supported"; return false; }
    const bool isFloat = fmtTag == 3;
    if (!((fmtTag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (isFloat && bits == 32))) { err = path + ": unsupported sample format"; return false; }
    const std::size_t bps = static_cast<std::size_t>(bits / 8), frames = dataLen / (bps * static_cast<std::size_t>(ch));
    f.sampleRate = rate; f.channels = ch; f.bits = bits; f.isFloat = isFloat;
    f.left.assign(frames, 0.0f);
    f.right.assign(frames, 0.0f);
    auto sample = [&](const unsigned char* p) -> float {
        if (isFloat) { float v; std::memcpy(&v, p, 4); return v; }
        if (bits == 16) return static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
        if (bits == 24) { std::int32_t v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; return static_cast<float>(v) / 8388608.0f; }
        return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(rd32(p))) / 2147483648.0);
    };
    for (std::size_t i = 0; i < frames; ++i) {
        f.left[i] = sample(data + i * bps * static_cast<std::size_t>(ch));
        f.right[i] = ch == 2 ? sample(data + (i * static_cast<std::size_t>(ch) + 1) * bps) : f.left[i];
    }
    return true;
}

inline bool write(const std::string& path, const File& f, std::string& err) {
    const std::size_t frames = f.left.size();
    const int bps = f.bits / 8;
    const std::size_t bytes = frames * static_cast<std::size_t>(f.channels * bps);
    std::vector<unsigned char> o(44 + bytes);
    auto w32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) o[at + static_cast<std::size_t>(i)] = static_cast<unsigned char>(v >> (8 * i)); };
    auto w16 = [&](std::size_t at, std::uint16_t v) { o[at] = static_cast<unsigned char>(v); o[at + 1] = static_cast<unsigned char>(v >> 8); };
    std::memcpy(&o[0], "RIFF", 4); w32(4, static_cast<std::uint32_t>(36 + bytes)); std::memcpy(&o[8], "WAVEfmt ", 8);
    w32(16, 16); w16(20, f.isFloat ? 3 : 1); w16(22, static_cast<std::uint16_t>(f.channels)); w32(24, static_cast<std::uint32_t>(f.sampleRate));
    w32(28, static_cast<std::uint32_t>(f.sampleRate * f.channels * bps)); w16(32, static_cast<std::uint16_t>(f.channels * bps)); w16(34, static_cast<std::uint16_t>(f.bits));
    std::memcpy(&o[36], "data", 4); w32(40, static_cast<std::uint32_t>(bytes));
    auto put = [&](std::size_t at, float v) {
        if (f.isFloat) { std::memcpy(&o[at], &v, 4); return; }
        const double full = f.bits == 16 ? 32768.0 : (f.bits == 24 ? 8388608.0 : 2147483648.0);
        double s = static_cast<double>(v) * full;
        const double hi = full - 1.0;
        s = s > hi ? hi : (s < -full ? -full : s);
        const auto q = static_cast<std::int64_t>(s < 0 ? s - 0.5 : s + 0.5);
        for (int i = 0; i < bps; ++i) o[at + static_cast<std::size_t>(i)] = static_cast<unsigned char>(static_cast<std::uint64_t>(q) >> (8 * i));
    };
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t base = 44 + i * static_cast<std::size_t>(f.channels * bps);
        put(base, f.left[i]);
        if (f.channels == 2) put(base + static_cast<std::size_t>(bps), f.right[i]);
    }
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) { err = "cannot write " + path; return false; }
    const bool ok = std::fwrite(o.data(), 1, o.size(), fp) == o.size();
    std::fclose(fp);
    if (!ok) err = "short write " + path;
    return ok;
}

} // namespace hc::hiss::wav
