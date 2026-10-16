// Phase 9 tests: session files (.hcs) round trip, moved song folders, missing stems,
// offline WAV export (sample accuracy, mute, limiter, determinism, cancel, background thread).
#include "TestHarness.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/MixExporter.hpp"
#include "hybrid/media/Session.hpp"
#include "hybrid/params/MixerParams.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>
#include <thread>
#include <vector>

namespace hc::test {

namespace {
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / name;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::vector<float> sine(std::size_t n, double hz, float amp) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0));
    return v;
}

std::vector<char> bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

void testSession() {
    std::printf("[session files]\n");
    const fs::path dir = freshDir("hc_session_test");
    const auto a = sine(48000, 440.0, 0.3f), b = sine(24000, 1000.0, 0.2f);
    CHECK(media::writeWav(dir / "01 Kick.wav", a.data(), a.data(), a.size(), 48000, media::WavEncoding::Float32));
    fs::create_directories(dir / "stems");
    CHECK(media::writeWav(dir / "stems" / "Bass \xC3\xA9t\xC3\xA9.wav", b.data(), nullptr, b.size(), 48000, media::WavEncoding::Pcm24));

    params::ParameterTree tree;
    params::addMixerParameters(tree);
    tree.find("stem01.trim")->setPlain(-3.5f);
    tree.find("stem06.ratio")->setPlain(4.0f);
    tree.find("main.limit")->setPlain(0.0f);

    media::SessionData d;
    d.stems[0] = dir / "01 Kick.wav";
    d.stems[5] = dir / "stems" / "Bass \xC3\xA9t\xC3\xA9.wav";
    d.params = tree.serialize();
    d.loop = false; d.page = 2; d.faderBank = 1; d.source = "stems"; d.playhead = 123456;
    std::string err;
    CHECK(media::saveSession(dir / "Song.hcs", d, "test", &err));

    auto l = media::loadSession(dir / "Song.hcs");
    CHECK(l.has_value());
    if (l) {
        std::error_code ec;
        CHECK(l->missing.empty() && fs::equivalent(l->data.stems[0], d.stems[0], ec) && fs::equivalent(l->data.stems[5], d.stems[5], ec));
        CHECK(l->data.stems[1].empty() && !l->data.loop && l->data.page == 2 && l->data.faderBank == 1 && l->data.playhead == 123456);
        params::ParameterTree t2;
        params::addMixerParameters(t2);
        t2.deserialize(l->data.params);
        CHECK(t2.find("stem01.trim")->plain() == -3.5f && t2.find("stem06.ratio")->plain() == 4.0f && !t2.find("main.limit")->isOn());
    }

    // The whole song folder moved: stems are found next to the session (relative paths).
    const fs::path moved = freshDir("hc_session_test_moved");
    std::error_code ec;
    fs::copy(dir, moved, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::remove_all(dir, ec);
    auto m = media::loadSession(moved / "Song.hcs");
    CHECK(m && m->missing.empty() && m->data.stems[5].parent_path().filename() == "stems" &&
          fs::equivalent(m->data.stems[5].parent_path().parent_path(), moved, ec));

    // A stem deleted: reported by slot and name, the rest still loads.
    fs::remove(moved / "01 Kick.wav", ec);
    auto x = media::loadSession(moved / "Song.hcs");
    CHECK(x && x->missing.size() == 1 && x->missing[0] == "slot 1: 01 Kick.wav" && x->data.stems[0].empty() && !x->data.stems[5].empty());
    if (x && !x->missing.empty()) std::printf("  moved folder: stems found by relative path; deleted stem reported as \"%s\"\n", x->missing[0].c_str());

    // Not a session / newer format.
    { std::ofstream(moved / "junk.hcs") << "hello"; }
    CHECK(!media::loadSession(moved / "junk.hcs").has_value());
    { std::ofstream(moved / "future.hcs") << "[session]\nformat=HybridCore Session\nversion=99\n"; }
    auto f = media::loadSession(moved / "future.hcs");
    CHECK(!f && f.error().find("newer") != std::string::npos);
    fs::remove_all(moved, ec);
}

void testExport() {
    std::printf("[export]\n");
    const fs::path dir = freshDir("hc_export_test");
    const auto a = sine(72000, 440.0, 0.3f);    // 1.5 s
    const auto b = sine(24000, 1000.0, 0.2f);   // 0.5 s
    CHECK(media::writeWav(dir / "a.wav", a.data(), a.data(), a.size(), 48000, media::WavEncoding::Float32));
    CHECK(media::writeWav(dir / "b.wav", b.data(), b.data(), b.size(), 48000, media::WavEncoding::Float32));
    params::ParameterTree tree;
    params::addMixerParameters(tree);

    media::ExportRequest req;
    req.stems[0] = dir / "a.wav";
    req.stems[4] = dir / "b.wav";
    req.params = tree.serialize();
    req.out = dir / "mix.wav";
    req.encoding = media::WavEncoding::Float32;
    auto r = media::renderMix(req);
    CHECK(r.has_value());
    auto mix = media::decodeFile(dir / "mix.wav");
    CHECK(mix.has_value());
    if (r && mix) {
        // Exactly the song length, from its first sample, equal to the sum of the stems.
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const float want = a[i] + (i < b.size() ? b[i] : 0.0f);
            worst = std::max(worst, static_cast<double>(std::fabs(mix->left[i] - want)));
            worst = std::max(worst, static_cast<double>(std::fabs(mix->right[i] - want)));
        }
        std::printf("  2 stems (1.5 s + 0.5 s), defaults: %llu frames, worst difference from the plain sum %.2e, peak %.4f, %.2f s\n",
                    static_cast<unsigned long long>(mix->frames()), worst, static_cast<double>(r->peak), r->seconds);
        CHECK(mix->frames() == a.size() && mix->sampleRate == 48000 && worst < 1e-6 && r->clipped == 0);
    }

    // Mute is part of the snapshot.
    tree.find("stem05.mute")->setPlain(1.0f);
    req.params = tree.serialize();
    auto r2 = media::renderMix(req);
    auto mix2 = media::decodeFile(dir / "mix.wav");
    if (r2 && mix2) {
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(mix2->left[i] - a[i])));
        CHECK(worst < 1e-6);
    } else CHECK(false);
    tree.find("stem05.mute")->setPlain(0.0f);

    // Loud mix: the limiter holds the ceiling; with it off, PCM clips and the clips are counted.
    tree.find("stem01.trim")->setPlain(12.0f);
    tree.find("stem01.lo")->setPlain(6.0f);
    tree.find("stem01.ratio")->setPlain(2.0f);
    req.params = tree.serialize();
    req.encoding = media::WavEncoding::Pcm24;
    auto loud = media::renderMix(req);
    CHECK(loud && loud->peak <= std::pow(10.0f, -0.3f / 20.0f) * 1.000001f && loud->clipped == 0);
    const auto first = bytes(dir / "mix.wav");
    auto again = media::renderMix(req);
    const auto second = bytes(dir / "mix.wav");
    CHECK(again && first.size() > 44 && first == second);   // deterministic, byte for byte
    if (loud) std::printf("  +12 dB trim, EQ and compressor on: peak %.4f with the limiter (ceiling %.4f); two renders byte-identical: %s\n",
                          static_cast<double>(loud->peak), std::pow(10.0, -0.3 / 20.0), first == second ? "yes" : "NO");
    tree.find("main.limit")->setPlain(0.0f);
    req.params = tree.serialize();
    auto clip = media::renderMix(req);
    CHECK(clip && clip->clipped > 0 && clip->peak > 1.0f);

    // Cancel, missing stem, nothing to export.
    auto c = media::renderMix(req, [](float f) { return f < 0.2f; });
    CHECK(!c && c.error() == "cancelled");
    media::ExportRequest bad = req;
    bad.stems[4] = dir / "nope.wav";
    auto w = media::renderMix(bad);
    CHECK(w && !w->warning.empty());
    media::ExportRequest none;
    none.out = dir / "x.wav";
    CHECK(!media::renderMix(none).has_value());

    // Background exporter: runs off the calling thread, reports progress, returns once.
    media::MixExporter ex;
    req.out = dir / "bg.wav";
    CHECK(ex.start(req) && !ex.start(req));
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<std::expected<media::ExportResult, std::string>> res;
    while (!(res = ex.poll()) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(res && res->has_value() && !ex.busy() && !ex.poll() && fs::exists(dir / "bg.wav"));
    std::error_code ec;
    CHECK(!fs::exists(dir / "bg.wav.part", ec));
    fs::remove_all(dir, ec);
}

} // namespace hc::test
