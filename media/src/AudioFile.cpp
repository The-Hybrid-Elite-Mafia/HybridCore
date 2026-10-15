#include "hybrid/media/AudioFile.hpp"

#include <dr_flac.h>
#include <dr_mp3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace hc::media {

namespace {

constexpr std::size_t kChunkFrames = 65536;

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t rd64(const std::uint8_t* p) { return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32); }
bool tag(const std::uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

std::unexpected<std::string> fail(std::string why) { return std::unexpected(std::move(why)); }

// Splits interleaved float frames into planar L/R (mono duplicated, extra channels dropped).
void appendInterleaved(DecodedAudio& out, const float* src, std::size_t frames, std::uint32_t channels) {
    const std::size_t base = out.left.size();
    out.left.resize(base + frames);
    out.right.resize(base + frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = src[i * channels];
        out.left[base + i]  = l;
        out.right[base + i] = channels > 1 ? src[i * channels + 1] : l;
    }
}

void noteChannels(DecodedAudio& d) {
    if (d.channels > 2) d.note = std::to_string(d.channels) + " channels: using channels 1+2";
}

// ---- WAV / RF64 ------------------------------------------------------------------------
std::expected<DecodedAudio, std::string> decodeWav(const std::uint8_t* data, std::size_t size, const Progress& progress) {
    if (size < 12) return fail("file too short for WAV");
    const bool rf64 = tag(data, "RF64");
    std::size_t pos = 12;
    std::uint16_t fmtTag = 0, channels = 0, blockAlign = 0, bits = 0;
    std::uint32_t rate = 0;
    bool haveFmt = false;
    std::uint64_t ds64Data = 0;
    const std::uint8_t* pcm = nullptr;
    std::uint64_t pcmBytes = 0;
    while (pos + 8 <= size) {
        const std::uint8_t* ch = data + pos;
        std::uint64_t len = rd32(ch + 4);
        const std::uint8_t* body = ch + 8;
        const std::size_t avail = size - pos - 8;
        if (tag(ch, "ds64") && len >= 16 && avail >= 16) {
            ds64Data = rd64(body + 8);
        } else if (tag(ch, "fmt ") && len >= 16 && avail >= 16) {
            fmtTag     = rd16(body);
            channels   = rd16(body + 2);
            rate       = rd32(body + 4);
            blockAlign = rd16(body + 12);
            bits       = rd16(body + 14);
            if (fmtTag == 0xFFFE) {
                if (len < 40 || avail < 40) return fail("WAV: truncated extensible format chunk");
                fmtTag = rd16(body + 24);   // first two bytes of the SubFormat GUID
            }
            haveFmt = true;
        } else if (tag(ch, "data")) {
            if (rf64 && len == 0xFFFFFFFFu) len = ds64Data;
            pcm = body;
            pcmBytes = std::min<std::uint64_t>(len, avail);   // streamed / truncated files: take what is there
            break;
        }
        if (len > avail) return fail("WAV: chunk runs past the end of the file");
        pos += 8 + static_cast<std::size_t>(len) + (len & 1);   // chunks are word aligned
    }
    if (!haveFmt) return fail("WAV: no format chunk");
    if (!pcm) return fail("WAV: no audio data chunk");
    if (channels == 0 || channels > 64) return fail("WAV: unsupported channel count " + std::to_string(channels));
    if (rate < 1000 || rate > 768000) return fail("WAV: unsupported sample rate " + std::to_string(rate));
    const bool isFloat = fmtTag == 3;
    if (!(fmtTag == 1 || isFloat)) return fail("WAV: compressed format (tag " + std::to_string(fmtTag) + ") is not supported");
    if (isFloat ? !(bits == 32 || bits == 64) : !(bits == 8 || bits == 16 || bits == 24 || bits == 32))
        return fail("WAV: unsupported bit depth " + std::to_string(bits));
    const std::uint32_t bytesPer = bits / 8u;
    if (blockAlign != channels * bytesPer) return fail("WAV: inconsistent block alignment");

    DecodedAudio out;
    out.sampleRate = rate;
    out.channels   = channels;
    out.format     = "WAV " + std::to_string(bits) + (isFloat ? "-bit float" : "-bit");
    noteChannels(out);
    const std::uint64_t frames = pcmBytes / blockAlign;
    out.left.resize(static_cast<std::size_t>(frames));
    out.right.resize(static_cast<std::size_t>(frames));
    const std::uint32_t useCh = std::min<std::uint32_t>(channels, 2);
    for (std::uint64_t f0 = 0; f0 < frames; f0 += kChunkFrames) {
        const std::uint64_t f1 = std::min<std::uint64_t>(frames, f0 + kChunkFrames);
        for (std::uint64_t f = f0; f < f1; ++f) {
            const std::uint8_t* fr = pcm + f * blockAlign;
            float v[2] = {0, 0};
            for (std::uint32_t c = 0; c < useCh; ++c) {
                const std::uint8_t* s = fr + c * bytesPer;
                switch (bits) {
                case 8:  v[c] = (static_cast<float>(s[0]) - 128.0f) / 128.0f; break;
                case 16: v[c] = static_cast<float>(static_cast<std::int16_t>(rd16(s))) / 32768.0f; break;
                case 24: {
                    const std::int32_t x = static_cast<std::int32_t>((static_cast<std::uint32_t>(s[0]) << 8) | (static_cast<std::uint32_t>(s[1]) << 16) |
                                                                     (static_cast<std::uint32_t>(s[2]) << 24)) >> 8;
                    v[c] = static_cast<float>(x) / 8388608.0f;
                    break;
                }
                case 32:
                    if (isFloat) { float x; std::memcpy(&x, s, 4); v[c] = x; }
                    else v[c] = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(rd32(s))) / 2147483648.0);
                    break;
                case 64: { double x; std::memcpy(&x, s, 8); v[c] = static_cast<float>(x); break; }
                default: break;
                }
            }
            out.left[static_cast<std::size_t>(f)]  = v[0];
            out.right[static_cast<std::size_t>(f)] = useCh > 1 ? v[1] : v[0];
        }
        if (progress && !progress(static_cast<float>(f1) / static_cast<float>(std::max<std::uint64_t>(frames, 1)))) return fail("cancelled");
    }
    return out;
}

// ---- MP3 -----------------------------------------------------------------------------------
std::expected<DecodedAudio, std::string> decodeMp3(const std::uint8_t* data, std::size_t size, const Progress& progress) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, data, size, nullptr)) return fail("MP3: not a decodable MPEG audio stream");
    struct Closer { drmp3* m; ~Closer() { drmp3_uninit(m); } } closer{&mp3};
    if (mp3.channels == 0 || mp3.sampleRate == 0) return fail("MP3: no audio frames");
    const drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);   // after delay/padding trimming
    if (!drmp3_seek_to_pcm_frame(&mp3, 0)) return fail("MP3: could not rewind after scanning");
    DecodedAudio out;
    out.sampleRate = mp3.sampleRate;
    out.channels   = mp3.channels;
    out.format     = "MP3";
    noteChannels(out);
    out.left.reserve(static_cast<std::size_t>(total));
    out.right.reserve(static_cast<std::size_t>(total));
    std::vector<float> buf(kChunkFrames * mp3.channels);
    for (;;) {
        const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, kChunkFrames, buf.data());
        if (got == 0) break;
        appendInterleaved(out, buf.data(), static_cast<std::size_t>(got), mp3.channels);
        if (progress && !progress(total ? std::min(1.0f, static_cast<float>(out.left.size()) / static_cast<float>(total)) : 0.5f)) return fail("cancelled");
    }
    if (out.left.empty()) return fail("MP3: no audio frames");
    return out;
}

// ---- FLAC ----------------------------------------------------------------------------------
std::expected<DecodedAudio, std::string> decodeFlac(const std::uint8_t* data, std::size_t size, const Progress& progress) {
    drflac* flac = drflac_open_memory(data, size, nullptr);
    if (!flac) return fail("FLAC: not a decodable FLAC stream");
    struct Closer { drflac* f; ~Closer() { drflac_close(f); } } closer{flac};
    if (flac->channels == 0 || flac->sampleRate == 0) return fail("FLAC: no audio frames");
    DecodedAudio out;
    out.sampleRate = flac->sampleRate;
    out.channels   = flac->channels;
    out.format     = "FLAC " + std::to_string(flac->bitsPerSample) + "-bit";
    noteChannels(out);
    const drflac_uint64 total = flac->totalPCMFrameCount;   // 0 when the stream doesn't say
    out.left.reserve(static_cast<std::size_t>(total));
    out.right.reserve(static_cast<std::size_t>(total));
    std::vector<float> buf(kChunkFrames * flac->channels);
    for (;;) {
        const drflac_uint64 got = drflac_read_pcm_frames_f32(flac, kChunkFrames, buf.data());
        if (got == 0) break;
        appendInterleaved(out, buf.data(), static_cast<std::size_t>(got), flac->channels);
        if (progress && !progress(total ? std::min(1.0f, static_cast<float>(out.left.size()) / static_cast<float>(total)) : 0.5f)) return fail("cancelled");
    }
    if (out.left.empty()) return fail("FLAC: no audio frames");
    return out;
}

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) { put16(b, static_cast<std::uint16_t>(v)); put16(b, static_cast<std::uint16_t>(v >> 16)); }

} // namespace

std::string sniffFormat(const std::uint8_t* d, std::size_t n) noexcept {
    if (n >= 12 && (tag(d, "RIFF") || tag(d, "RF64")) && tag(d + 8, "WAVE")) return "wav";
    if (n >= 4 && tag(d, "fLaC")) return "flac";
    if (n >= 3 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') return "mp3";   // ID3v2 tag in front of MPEG audio
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "mp3";      // MPEG frame sync
    return "";
}

std::expected<DecodedAudio, std::string> decodeMemory(const std::uint8_t* data, std::size_t size, const Progress& progress) {
    const std::string kind = sniffFormat(data, size);
    if (kind == "wav") return decodeWav(data, size, progress);
    if (kind == "flac") return decodeFlac(data, size, progress);
    if (kind == "mp3") return decodeMp3(data, size, progress);
    return fail("not a WAV, MP3 or FLAC file");
}

std::expected<DecodedAudio, std::string> decodeFile(const std::filesystem::path& file, const Progress& progress) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("cannot open the file");
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0) return fail("empty file");
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return fail("read error");
    return decodeMemory(bytes.data(), bytes.size(), progress);
}

bool writeWav(const std::filesystem::path& file, const float* left, const float* right, std::uint64_t frames, std::uint32_t rate,
              WavEncoding enc) {
    const std::uint16_t ch = right ? 2 : 1;
    const std::uint16_t bits = enc == WavEncoding::Pcm16 ? 16 : enc == WavEncoding::Pcm24 ? 24 : 32;
    const std::uint16_t align = static_cast<std::uint16_t>(ch * bits / 8);
    const std::uint64_t dataBytes = frames * align;
    if (dataBytes > 0xFFFFFFFFull - 64) return false;
    std::vector<std::uint8_t> b;
    b.reserve(static_cast<std::size_t>(44 + dataBytes));
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, static_cast<std::uint32_t>(36 + dataBytes));
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, enc == WavEncoding::Float32 ? 3 : 1);
    put16(b, ch);
    put32(b, rate);
    put32(b, rate * align);
    put16(b, align);
    put16(b, bits);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, static_cast<std::uint32_t>(dataBytes));
    for (std::uint64_t f = 0; f < frames; ++f)
        for (std::uint16_t c = 0; c < ch; ++c) {
            const float v = c == 0 ? left[f] : right[f];
            if (enc == WavEncoding::Float32) {
                std::uint32_t u;
                std::memcpy(&u, &v, 4);
                put32(b, u);
            } else {
                const double full = enc == WavEncoding::Pcm16 ? 32767.0 : 8388607.0;
                const auto q = static_cast<std::int32_t>(std::lround(std::clamp(static_cast<double>(v), -1.0, 1.0) * full));
                b.push_back(static_cast<std::uint8_t>(q));
                b.push_back(static_cast<std::uint8_t>(q >> 8));
                if (enc == WavEncoding::Pcm24) b.push_back(static_cast<std::uint8_t>(q >> 16));
            }
        }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

bool isAudioFileName(const std::filesystem::path& file) {
    std::string e = file.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".wav" || e == ".wave" || e == ".mp3" || e == ".flac";
}

} // namespace hc::media
