// Phase 6 tests: WAV / RF64 / MP3 / FLAC decoding (incl. malformed and fuzzed input),
// resampler accuracy (sine residual, passband, alias rejection, length, no drift),
// background stem loader (results, errors, cancel, replace, no leaks) and lock-free
// stem playback in MixerProcessor (zero allocation, loop, retire, sample accuracy).
#include "TestHarness.hpp"

#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/engine/StemAudio.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/Resampler.hpp"
#include "hybrid/media/StemLoader.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

namespace hc::test {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double rate, std::size_t n, double amp, double phase = 0.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate + phase));
    return v;
}

// Least-squares fit of a sine of known frequency over [from, to): returns amplitude and
// residual (dB relative to the fitted amplitude, RMS of what is left).
struct Fit { double amp = 0, residualDb = 0; };
Fit fitSine(const float* x, std::size_t from, std::size_t to, double freq, double rate) {
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / rate, s = std::sin(w), c = std::cos(w);
        ss += s * s; sc += s * c; cc += c * c; xs += x[i] * s; xc += x[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double res = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        const double e = x[i] - (a * std::sin(w) + b * std::cos(w));
        res += e * e;
    }
    Fit f;
    f.amp = std::sqrt(a * a + b * b);
    const double rms = std::sqrt(res / static_cast<double>(to - from));
    f.residualDb = 20.0 * std::log10(std::max(rms, 1e-12) / (f.amp / std::sqrt(2.0)));
    return f;
}

double rmsDb(const float* x, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(s / static_cast<double>(to - from), 1e-30));
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void le16(std::vector<std::uint8_t>& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
void le32(std::vector<std::uint8_t>& b, std::uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
void le64(std::vector<std::uint8_t>& b, std::uint64_t v) { le32(b, static_cast<std::uint32_t>(v)); le32(b, static_cast<std::uint32_t>(v >> 32)); }

// Hand-built WAV with an arbitrary format chunk.
std::vector<std::uint8_t> craftWav(std::uint16_t tag, std::uint16_t ch, std::uint32_t rate, std::uint16_t bits,
                                   const std::vector<std::uint8_t>& pcm, bool extensible = false, bool rf64 = false) {
    std::vector<std::uint8_t> b;
    b.insert(b.end(), {static_cast<std::uint8_t>(rf64 ? 'R' : 'R'), static_cast<std::uint8_t>(rf64 ? 'F' : 'I'),
                       static_cast<std::uint8_t>(rf64 ? '6' : 'F'), static_cast<std::uint8_t>(rf64 ? '4' : 'F')});
    le32(b, rf64 ? 0xFFFFFFFFu : 0);
    b.insert(b.end(), {'W', 'A', 'V', 'E'});
    if (rf64) {
        b.insert(b.end(), {'d', 's', '6', '4'});
        le32(b, 28);
        le64(b, 0);
        le64(b, pcm.size());
        le64(b, 0);
        le32(b, 0);
    }
    b.insert(b.end(), {'L', 'I', 'S', 'T'});   // an unrelated chunk with odd length + pad byte
    le32(b, 3);
    b.insert(b.end(), {'a', 'b', 'c', 0});
    b.insert(b.end(), {'f', 'm', 't', ' '});
    le32(b, extensible ? 40 : 16);
    le16(b, extensible ? 0xFFFE : tag);
    le16(b, ch);
    le32(b, rate);
    le32(b, rate * ch * bits / 8);
    le16(b, static_cast<std::uint16_t>(ch * bits / 8));
    le16(b, bits);
    if (extensible) {
        le16(b, 22);
        le16(b, bits);
        le32(b, 0);
        le16(b, tag);   // SubFormat GUID begins with the format tag
        for (int i = 0; i < 14; ++i) b.push_back(0);
    }
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le32(b, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(pcm.size()));
    b.insert(b.end(), pcm.begin(), pcm.end());
    return b;
}

std::filesystem::path tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}
} // namespace

std::filesystem::path testDataDir() {
    std::error_code ec;
    for (const auto& d : {assetDir().parent_path() / "testdata", assetDir().parent_path() / "bin" / "testdata"})
        if (std::filesystem::is_directory(d, ec)) return d;
    return assetDir().parent_path() / "testdata";
}

void testMediaFiles() {
    std::printf("[media files]\n");
    const auto dir = tempDir("hc_media_test");
    const std::size_t n = 11025;
    const auto l = sine(1000, 44100, n, 0.5), r = sine(2500, 44100, n, 0.25);

    // Round trips through our own writer.
    struct Case { media::WavEncoding enc; float tol; const char* fmt; };
    for (const Case c : {Case{media::WavEncoding::Pcm16, 1.0f / 32767.0f, "WAV 16-bit"}, Case{media::WavEncoding::Pcm24, 1.0f / 8388607.0f, "WAV 24-bit"},
                         Case{media::WavEncoding::Float32, 0.0f, "WAV 32-bit float"}}) {
        const auto f = dir / "rt.wav";
        CHECK(media::writeWav(f, l.data(), r.data(), n, 44100, c.enc));
        const auto d = media::decodeFile(f);
        CHECK(d.has_value());
        if (!d) continue;
        float err = 0;
        for (std::size_t i = 0; i < n; ++i) err = std::max({err, std::fabs(d->left[i] - l[i]), std::fabs(d->right[i] - r[i])});
        CHECK(d->sampleRate == 44100 && d->channels == 2 && d->frames() == n && d->format == c.fmt);
        CHECK(err <= c.tol * 1.5f + 1e-7f);   // writer scales by 2^(n-1)-1, reader by 2^(n-1)
    }
    {   // mono is duplicated to both sides
        const auto f = dir / "mono.wav";
        CHECK(media::writeWav(f, l.data(), nullptr, n, 48000, media::WavEncoding::Float32));
        const auto d = media::decodeFile(f);
        CHECK(d && d->channels == 1 && d->left == d->right && d->left[100] == l[100]);
    }
    {   // 8-bit unsigned, 64-bit float, extensible 24-bit with 6 channels, RF64
        const auto d8 = media::decodeMemory(craftWav(1, 1, 22050, 8, {0, 128, 255}).data(), craftWav(1, 1, 22050, 8, {0, 128, 255}).size());
        CHECK(d8 && d8->frames() == 3 && d8->left[0] == -1.0f && d8->left[1] == 0.0f && std::fabs(d8->left[2] - 127.0f / 128.0f) < 1e-6f);
        std::vector<std::uint8_t> f64(16);
        const double a = 0.25, b = -0.75;
        std::memcpy(f64.data(), &a, 8);
        std::memcpy(f64.data() + 8, &b, 8);
        const auto w64 = craftWav(3, 2, 96000, 64, f64);
        const auto d64 = media::decodeMemory(w64.data(), w64.size());
        CHECK(d64 && d64->frames() == 1 && d64->left[0] == 0.25f && d64->right[0] == -0.75f && d64->format == "WAV 64-bit float");
        std::vector<std::uint8_t> six;
        for (int c = 0; c < 6; ++c) { const std::int32_t v = (c + 1) * 100000; six.push_back(static_cast<std::uint8_t>(v)); six.push_back(static_cast<std::uint8_t>(v >> 8)); six.push_back(static_cast<std::uint8_t>(v >> 16)); }
        const auto wx = craftWav(1, 6, 48000, 24, six, true);
        const auto dx = media::decodeMemory(wx.data(), wx.size());
        CHECK(dx && dx->channels == 6 && std::fabs(dx->left[0] - 100000.0f / 8388608.0f) < 1e-7f && std::fabs(dx->right[0] - 200000.0f / 8388608.0f) < 1e-7f);
        CHECK(dx && dx->note.find("6 channels") != std::string::npos);
        std::vector<std::uint8_t> p16 = {0x00, 0x40, 0x00, 0xC0};
        const auto rf = craftWav(1, 2, 48000, 16, p16, false, true);
        const auto drf = media::decodeMemory(rf.data(), rf.size());
        CHECK(drf && drf->frames() == 1 && drf->left[0] == 0.5f && drf->right[0] == -0.5f);
    }
    // Bad input: clear errors, never a crash.
    const auto adpcm = craftWav(2, 1, 44100, 4, {1, 2, 3, 4});
    CHECK(!media::decodeMemory(adpcm.data(), adpcm.size()) && media::decodeMemory(adpcm.data(), adpcm.size()).error().find("compressed") != std::string::npos);
    const std::uint8_t junk[] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!'};
    CHECK(!media::decodeMemory(junk, sizeof junk));
    CHECK(!media::decodeFile(dir / "does-not-exist.wav"));
    {
        const auto good = readAll(dir / "rt.wav");
        std::mt19937 rng(1234);
        int decodedOk = 0;
        for (int i = 0; i < 400; ++i) {   // mutated and truncated files
            std::vector<std::uint8_t> m(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(good.size(), 60 + rng() % 400)));
            for (int k = 0; k < 6; ++k) m[rng() % std::min<std::size_t>(m.size(), 60)] = static_cast<std::uint8_t>(rng());
            if (media::decodeMemory(m.data(), m.size())) ++decodedOk;
        }
        std::printf("  400 corrupted / truncated WAV headers: no crash (%d still decodable)\n", decodedOk);
        CHECK(true);
    }
    CHECK(media::sniffFormat(reinterpret_cast<const std::uint8_t*>("fLaC...."), 8) == "flac");
    CHECK(media::isAudioFileName("Kick.WAV") && media::isAudioFileName("a.mp3") && media::isAudioFileName("b.Flac") && !media::isAudioFileName("c.ogg"));

    // Encoded test vectors (made with ffmpeg: LAME 192k MP3, FLAC 24-bit).
    const auto data = testDataDir();
    const auto mp3 = media::decodeFile(data / "tone-44k1-stereo.mp3");
    CHECK(mp3.has_value());
    if (mp3) {
        const Fit fl = fitSine(mp3->left.data(), 4410, 84000, 1000, 44100), fr = fitSine(mp3->right.data(), 4410, 84000, 2500, 44100);
        std::printf("  MP3: %u Hz, %llu frames (gapless, expect 88200), 1 kHz amp %.4f residual %.1f dB, 2.5 kHz amp %.4f residual %.1f dB\n",
                    mp3->sampleRate, static_cast<unsigned long long>(mp3->frames()), fl.amp, fl.residualDb, fr.amp, fr.residualDb);
        CHECK(mp3->sampleRate == 44100 && mp3->channels == 2 && mp3->format == "MP3");
        CHECK(mp3->frames() == 88200);
        // ffmpeg's own decoder reads this file as 88200 frames at amplitude 0.4850 (the encoder is 0.26 dB low).
        CHECK(std::fabs(fl.amp - 0.4850) < 0.002 && std::fabs(fr.amp - 0.4850) < 0.002 && fl.residualDb < -70.0 && fr.residualDb < -70.0);
    } else {
        std::printf("  MP3 test vector: %s (%s)\n", mp3.error().c_str(), (data / "tone-44k1-stereo.mp3").string().c_str());
    }
    const auto flac = media::decodeFile(data / "tone-96k-24bit.flac");
    CHECK(flac.has_value());
    if (flac) {
        const Fit fl = fitSine(flac->left.data(), 0, 48000, 1000, 96000), fr = fitSine(flac->right.data(), 0, 48000, 3000, 96000);
        std::printf("  FLAC: %u Hz, %llu frames, %s, 1 kHz residual %.1f dB, 3 kHz residual %.1f dB\n", flac->sampleRate,
                    static_cast<unsigned long long>(flac->frames()), flac->format.c_str(), fl.residualDb, fr.residualDb);
        CHECK(flac->sampleRate == 96000 && flac->frames() == 48000 && flac->format == "FLAC 24-bit");
        CHECK(std::fabs(fl.amp - 0.5) < 0.001 && fl.residualDb < -120.0 && fr.residualDb < -120.0);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testResampler() {
    std::printf("[resampler]\n");
    {   // 44.1 -> 48 kHz: length, sine accuracy, passband
        const media::Resampler rs(44100, 48000);
        CHECK(rs.outputLength(88200) == 96000 && rs.outputLength(1) == 2);
        const std::size_t n = 88200;
        const auto in = sine(997, 44100, n, 0.5);
        std::vector<float> o(rs.outputLength(n)), o2(o.size());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(rs.process(in.data(), in.data(), n, o.data(), o2.data()));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const Fit f = fitSine(o.data(), 2000, o.size() - 2000, 997, 48000);
        std::printf("  44.1k->48k: %d taps, pass %.0f Hz, stop %.0f Hz; 997 Hz amp %.6f residual %.1f dB; 2 s stereo in %.1f ms\n", rs.taps(),
                    rs.passbandHz(), rs.stopbandHz(), f.amp, f.residualDb, ms);
        CHECK(std::fabs(f.amp - 0.5) < 1e-4 && f.residualDb < -100.0);
        CHECK(o == o2);
        for (double hz : {100.0, 10000.0, 18000.0, 20000.0}) {
            const auto s = sine(hz, 44100, n, 0.5);
            CHECK(rs.process(s.data(), s.data(), n, o.data(), o2.data()));
            const Fit g = fitSine(o.data(), 2000, o.size() - 2000, hz, 48000);
            const double db = 20.0 * std::log10(g.amp / 0.5);
            std::printf("    %5.0f Hz: gain %+.4f dB, residual %.1f dB\n", hz, db, g.residualDb);
            CHECK(std::fabs(db) < (hz <= 18000.0 ? 0.01 : 0.1));
        }
    }
    {   // 96 -> 48 kHz: 30 kHz must not alias into the band; 10 kHz passes
        const media::Resampler rs(96000, 48000);
        const std::size_t n = 96000;
        const auto hi = sine(30000, 96000, n, 0.5), mid = sine(10000, 96000, n, 0.5);
        std::vector<float> a(rs.outputLength(n)), b(a.size());
        CHECK(rs.process(hi.data(), mid.data(), n, a.data(), b.data()));
        const double alias = rmsDb(a.data(), 2000, a.size() - 2000) - 20.0 * std::log10(0.5 / std::sqrt(2.0));
        const Fit g = fitSine(b.data(), 2000, b.size() - 2000, 10000, 48000);
        std::printf("  96k->48k: %d taps; 30 kHz alias %.1f dB; 10 kHz gain %+.4f dB residual %.1f dB\n", rs.taps(), alias, 20.0 * std::log10(g.amp / 0.5), g.residualDb);
        CHECK(alias < -95.0 && std::fabs(g.amp - 0.5) < 1e-4 && g.residualDb < -100.0);
    }
    {   // No drift over a long file: a 5-minute 44.1k sine is still exact at the end.
        const media::Resampler rs(44100, 48000);
        const std::size_t n = 44100ull * 300;
        const auto in = sine(440, 44100, n, 0.5);
        std::vector<float> o(rs.outputLength(n));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(rs.process(in.data(), in.data(), n, o.data(), o.data()));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const Fit end = fitSine(o.data(), o.size() - 48000, o.size() - 1000, 440, 48000);
        std::printf("  5-minute stereo file: %.2f s to convert; last second residual %.1f dB (no drift)\n", s, end.residualDb);
        CHECK(end.residualDb < -100.0);
    }
    {   // Same rate: bit exact, cancellation honoured.
        const media::Resampler same(48000, 48000);
        const auto in = sine(1000, 48000, 1000, 0.3);
        std::vector<float> o(1000), o2(1000);
        CHECK(same.passthrough() && same.process(in.data(), in.data(), 1000, o.data(), o2.data()) && o == in);
        const media::Resampler rs(44100, 48000);
        std::vector<float> big(441000), out(rs.outputLength(big.size()));
        CHECK(!rs.process(big.data(), big.data(), big.size(), out.data(), out.data(), [](float) { return false; }));
    }
}

void testStemLoader() {
    std::printf("[stem loader]\n");
    const auto dir = tempDir("hc_loader_test");
    const std::int64_t live0 = engine::StemAudio::liveCount().load();
    const auto a = sine(500, 44100, 44100, 0.5), b = sine(700, 48000, 24000, 0.25), c = sine(900, 96000, 96000, 0.4);
    CHECK(media::writeWav(dir / "01 Kick.wav", a.data(), a.data(), a.size(), 44100, media::WavEncoding::Pcm16));
    CHECK(media::writeWav(dir / "02 Bass.wav", b.data(), nullptr, b.size(), 48000, media::WavEncoding::Float32));
    CHECK(media::writeWav(dir / "03 Pad.wav", c.data(), c.data(), c.size(), 96000, media::WavEncoding::Pcm24));
    { std::ofstream bad(dir / "04 Broken.wav", std::ios::binary); bad << "RIFF0000WAVEnothing here"; }
    {
        media::StemLoader loader(3);
        loader.load(0, dir / "01 Kick.wav");
        loader.load(1, dir / "02 Bass.wav");
        loader.load(2, dir / "03 Pad.wav");
        loader.load(3, dir / "04 Broken.wav");
        loader.load(4, dir / "missing.wav");
        loader.load(5, dir / "03 Pad.wav");
        loader.load(5, dir / "01 Kick.wav");   // replaces the queued job for slot 5
        loader.load(6, dir / "03 Pad.wav");
        loader.cancel(6);
        CHECK(loader.busy());
        std::vector<media::StemLoader::Result> got;
        const auto t0 = std::chrono::steady_clock::now();
        while (got.size() < 6 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
            media::StemLoader::Result r;
            if (loader.poll(r)) got.push_back(std::move(r));
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        media::StemLoader::Result extra;
        CHECK(!loader.poll(extra));             // nothing for the cancelled slot
        CHECK(got.size() == 6 && !loader.busy());
        for (const auto& r : got) {
            if (r.slot == 0) CHECK(r.audio && r.audio->frames == 48000 && r.audio->name == "01 Kick" && r.audio->sourceRate == 44100 && r.audio->format == "WAV 16-bit");
            if (r.slot == 1) CHECK(r.audio && r.audio->frames == 24000 && r.audio->sourceChannels == 1 && r.audio->right[123] == r.audio->left[123]);
            if (r.slot == 2) CHECK(r.audio && r.audio->frames == 48000 && r.audio->sourceRate == 96000);
            if (r.slot == 3) CHECK(!r.audio && !r.error.empty());
            if (r.slot == 4) CHECK(!r.audio && r.error.find("open") != std::string::npos);
            if (r.slot == 5) CHECK(r.audio && r.audio->name == "01 Kick");
            if (r.slot == 2 && r.audio) {
                const Fit f = fitSine(r.audio->left.data(), 1000, 47000, 900, 48000);
                CHECK(std::fabs(f.amp - 0.4) < 1e-3 && f.residualDb < -90.0);
                // Overview: 2048 peak bins; a steady 0.4 sine reads ~0.4 in every bin.
                float lo = 1.0f, hi = 0.0f;
                for (float v : r.audio->overview) { lo = std::min(lo, v); hi = std::max(hi, v); }
                CHECK(r.audio->overview.size() == engine::StemAudio::kOverviewBins && hi < 0.41f && lo > 0.35f);
            }
            if (r.slot == 3) std::printf("  broken file -> \"%s\"\n", r.error.c_str());
        }
    }
    CHECK(engine::StemAudio::liveCount().load() == live0);   // every buffer freed

    // Natural order, folders expanded, non-audio skipped.
    { std::ofstream(dir / "10 Vox.mp3") << "x"; std::ofstream(dir / "2 snare.FLAC") << "x"; std::ofstream(dir / "notes.txt") << "x"; }
    const auto files = media::collectAudioFiles({dir});
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(f.filename().string());
    CHECK((names == std::vector<std::string>{"01 Kick.wav", "02 Bass.wav", "2 snare.FLAC", "03 Pad.wav", "04 Broken.wav", "10 Vox.mp3"}));
    engine::StemAudio d;
    d.format = "MP3"; d.sourceRate = 44100; d.frames = 48000ull * 222;
    CHECK(media::describeStem(d) == "MP3 44.1k 3:42");
    d.format = "WAV 24-bit (6 channels: using channels 1+2)"; d.sourceRate = 48000; d.frames = 48000ull * 61;
    CHECK(media::describeStem(d) == "WAV 24b 1:01");
    d.format = "WAV 32-bit float";
    CHECK(media::describeStem(d) == "WAV 32f 1:01");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testStemPlayback() {
    std::printf("[stem playback]\n");
    const std::int64_t live0 = engine::StemAudio::liveCount().load();
    auto make = [](std::uint64_t frames, float base) {
        auto s = std::make_unique<engine::StemAudio>();
        s->frames = frames;
        s->left.allocate(static_cast<std::size_t>(frames));
        s->right.allocate(static_cast<std::size_t>(frames));
        for (std::uint64_t i = 0; i < frames; ++i) {
            s->left[static_cast<std::size_t>(i)]  = base + static_cast<float>(i) * 1e-4f;
            s->right[static_cast<std::size_t>(i)] = -(base + static_cast<float>(i) * 1e-4f);
        }
        return s;
    };
    {
        constexpr std::uint32_t F = 256;
        engine::MixerProcessor mp(engine::MixerProcessor::Source::Stems);
        io::StreamConfig cfg;
        cfg.sampleRate = 48000; cfg.blockFrames = F; cfg.numOutputs = 8;
        mp.prepare(cfg);
        for (std::uint32_t s = 1; s < kStemCount; ++s) mp.matrix().setStemMute(s, true);   // only stem 1 is heard
        std::vector<float> out(8 * F);
        float* outs[8];
        for (int i = 0; i < 8; ++i) outs[i] = out.data() + i * F;
        engine::ProcessContext ctx;
        ctx.outputs = outs; ctx.numOutputs = 8; ctx.frames = F;

        // Nothing loaded: silence.
        mp.process(ctx);
        CHECK(rmsDb(outs[0], 0, F) < -200.0 && mp.stemSongFrames() == 0);

        mp.setLoop(true);
        mp.play();
        auto s0 = make(1000, 0.1f), s1 = make(1500, 0.2f);
        CHECK(mp.submitStem(0, s0) && !s0);
        CHECK(mp.submitStem(1, s1) && !s1);
        std::vector<float> heard;
        for (int blk = 0; blk < 12; ++blk) {   // 3072 frames: two loops of 1500 plus change
            mp.process(ctx);
            heard.insert(heard.end(), outs[0], outs[0] + F);
        }
        CHECK(mp.stemSongFrames() == 1500);
        // Stem 1 (1000 frames) then 500 frames of silence, repeating every 1500 frames, at unity gain.
        // The first block fades in (the matrix ramps a stem in from silence), so compare from block 2.
        int bad = 0;
        for (std::size_t i = F; i < heard.size(); ++i) {
            const std::size_t pos = i % 1500;
            const float want = pos < 1000 ? 0.1f + static_cast<float>(pos) * 1e-4f : 0.0f;
            if (std::fabs(heard[i] - want) > 1e-6f) ++bad;
        }
        std::printf("  loop of 1500 frames over 3072 samples: %d mismatches at unity gain, playhead %llu\n", bad,
                    static_cast<unsigned long long>(mp.stemPlayhead()));
        CHECK(bad == 0 && heard[F / 2] > 0.0f && heard[F / 2] < 0.1f + 0.0128f && mp.stemPlayhead() == 3072 % 1500);

        // Swapping stems while running: no heap allocation on the audio path, old stems retired.
        std::vector<std::unique_ptr<engine::StemAudio>> spare;
        for (int i = 0; i < 40; ++i) spare.push_back(make(800 + static_cast<std::uint64_t>(i), 0.3f));
        std::uint64_t allocs = 0;
        int submitted = 0;
        {
            AllocScope scope;
            for (int blk = 0; blk < 2000; ++blk) {
                if (blk % 50 == 0 && submitted < 40) { if (mp.submitStem(static_cast<std::uint32_t>(submitted % 3), spare[static_cast<std::size_t>(submitted)])) ++submitted; }
                mp.process(ctx);
                if (blk % 7 == 0) mp.collectRetired();
            }
            allocs = scope.count();
        }
        mp.collectRetired();
        std::printf("  2,000 blocks with %d live stem swaps: %llu heap allocations\n", submitted, static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0 && submitted == 40);
        CHECK(engine::StemAudio::liveCount().load() == live0 + 3);   // slots 0..2 hold the last three

        mp.seek(0);
        mp.process(ctx);
        CHECK(mp.stemPlayhead() == F);
        std::unique_ptr<engine::StemAudio> none;
        CHECK(mp.submitStem(0, none) && mp.submitStem(1, none) && mp.submitStem(2, none));
        mp.process(ctx);
        mp.collectRetired();
        CHECK(mp.stemSongFrames() == 0 && engine::StemAudio::liveCount().load() == live0);

        auto pending = make(100, 0.0f);
        CHECK(mp.submitStem(7, pending));   // still queued when the processor is destroyed
    }
    CHECK(engine::StemAudio::liveCount().load() == live0);   // destructor frees queued and loaded stems
}

void testTransport() {
    std::printf("[transport]\n");
    constexpr std::uint32_t F = 256;
    auto make = [](std::uint64_t frames) {
        auto s = std::make_unique<engine::StemAudio>();
        s->frames = frames;
        s->left.allocate(static_cast<std::size_t>(frames));
        s->right.allocate(static_cast<std::size_t>(frames));
        for (std::uint64_t i = 0; i < frames; ++i) s->left[static_cast<std::size_t>(i)] = s->right[static_cast<std::size_t>(i)] = 0.5f;
        return s;
    };
    engine::MixerProcessor mp(engine::MixerProcessor::Source::Stems);
    io::StreamConfig cfg;
    cfg.sampleRate = 48000; cfg.blockFrames = F; cfg.numOutputs = 2;
    mp.prepare(cfg);
    std::vector<float> out(2 * F);
    float* outs[2] = {out.data(), out.data() + F};
    engine::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.frames = F;
    auto st = make(10 * F);
    CHECK(mp.submitStem(0, st));

    // Stopped by default: silence, playhead stays.
    for (int i = 0; i < 3; ++i) mp.process(ctx);
    CHECK(!mp.playing() && mp.stemPlayhead() == 0 && mp.stemSongFrames() == 10 * F && std::fabs(outs[0][100]) < 1e-9f);

    // Play: the first block fades in (no click), then steady.
    mp.play();
    mp.process(ctx);
    CHECK(mp.playing() && std::fabs(outs[0][0]) < 1e-6f && outs[0][F - 1] > 0.4f);
    float maxStep = 0;
    for (std::uint32_t i = 1; i < F; ++i) maxStep = std::max(maxStep, std::fabs(outs[0][i] - outs[0][i - 1]));
    mp.process(ctx);
    CHECK(mp.stemPlayhead() == 2 * F && maxStep < 0.01f);

    // Pause: one faded block, then silence; the position is kept.
    mp.pause();
    mp.process(ctx);
    CHECK(!mp.playing() && mp.stemPlayhead() == 3 * F && outs[0][F - 1] < 0.01f && outs[0][0] > 0.3f);
    mp.process(ctx);
    CHECK(mp.stemPlayhead() == 3 * F && std::fabs(outs[0][10]) < 1e-9f);

    // Seek while stopped, then play from there.
    mp.seek(7 * F);
    mp.process(ctx);
    CHECK(mp.stemPlayhead() == 7 * F && !mp.playing());
    mp.play();
    mp.process(ctx);
    CHECK(mp.stemPlayhead() == 8 * F);

    // Loop off: stops at the end of the song and returns to the start.
    mp.setLoop(false);
    for (int i = 0; i < 4; ++i) mp.process(ctx);
    CHECK(!mp.playing() && !mp.wantsPlay() && mp.stemPlayhead() == 0);

    // Loop on: wraps.
    mp.setLoop(true);
    mp.play();
    for (int i = 0; i < 12; ++i) mp.process(ctx);
    CHECK(mp.playing() && mp.stemPlayhead() == 2 * F);

    // Stop while playing: fades out from the current place, then sits at the start.
    mp.stop();
    mp.process(ctx);
    CHECK(!mp.playing() && mp.stemPlayhead() == 0 && outs[0][0] > 0.3f && outs[0][F - 1] < 0.01f);

    // Transport changes never allocate on the audio thread.
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int i = 0; i < 500; ++i) {
            if (i % 7 == 0) mp.play();
            if (i % 11 == 0) mp.pause();
            if (i % 13 == 0) mp.seek(static_cast<std::uint64_t>(i) * 37u);
            if (i % 17 == 0) mp.setLoop(i % 2 == 0);
            mp.process(ctx);
        }
        allocs = scope.count();
    }
    std::printf("  play / pause / stop / seek / loop: 500 blocks of random transport changes, %llu heap allocations\n",
                static_cast<unsigned long long>(allocs));
    CHECK(allocs == 0);
}

} // namespace hc::test
