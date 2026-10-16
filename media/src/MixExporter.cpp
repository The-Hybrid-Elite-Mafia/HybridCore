#include "hybrid/media/MixExporter.hpp"
#include "hybrid/media/StemLoader.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/forge/Forge.hpp"
#include "hybrid/forge/ForgeEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace hc::media {

std::expected<ExportResult, std::string> renderMix(const ExportRequest& req, const Progress& progress) {
    const auto t0 = std::chrono::steady_clock::now();
    auto report = [&](float f) { return !progress || progress(std::clamp(f, 0.0f, 1.0f)); };
    try {
        // 1) Parameters: a private tree, so later changes in the app do not leak into the render.
        params::ParameterTree tree;
        params::addMixerParameters(tree);
        forge::addForgeParameters(tree);
        tree.deserialize(req.params);
        const params::MixerBindings bindings(tree);

        constexpr std::uint32_t kBlock = 1024;
        auto mp = std::make_unique<engine::MixerProcessor>(engine::MixerProcessor::Source::Stems);
        io::StreamConfig cfg;
        cfg.sampleRate = hc::kSampleRate; cfg.blockFrames = kBlock; cfg.numOutputs = 2;
        mp->prepare(cfg);
        mp->setStartFade(false);   // the file starts on the song's first sample
        bindings.applyAll(tree, mp->matrix(), &mp->limiter());
        mp->matrix().snapToTargets();
        mp->forge().bind(tree);
        forge::Song fsong;
        const bool forgeOn = tree.find(forge::id::on)->isOn() && forge::songFromText(req.forgeSong, fsong) && !fsong.pattern.isEmpty();
        if (forgeOn) mp->forge().setSong(fsong);
        else tree.find(forge::id::on)->setPlain(0.0f);
        const std::uint64_t forgeFrames = forgeOn ? forge::ForgeEngine::songLength(fsong, hc::kSampleRate, tree.find(forge::id::bpm)->plain(),
                                                                                     tree.find(forge::id::arrange)->isOn())
                                                  : 0;
        const std::uint64_t tail = forgeOn ? 2 * hc::kSampleRate : 0;   // reverb / delay tails

        // 2) Stems, decoded again (first half of the progress bar).
        std::uint32_t used = 0;
        for (const auto& p : req.stems) used += !p.empty();
        if (used == 0 && forgeFrames == 0) return std::unexpected(std::string("nothing to export (no stems, Beat Forge off or empty)"));
        std::uint64_t song = 0;
        std::uint32_t done = 0;
        std::string   warning;
        for (std::uint32_t s = 0; s < hc::kStemCount; ++s) {
            if (req.stems[s].empty()) continue;
            if (forgeOn && s < static_cast<std::uint32_t>(forge::kTracks)) continue;   // strips 1–8 play Beat Forge
            auto stem = buildStem(req.stems[s], hc::kSampleRate,
                                  [&](float f) { return report(0.5f * (static_cast<float>(done) + f) / static_cast<float>(std::max<std::uint32_t>(used, 1))); });
            ++done;
            if (!stem) {
                if (stem.error() == "cancelled") return std::unexpected(std::string("cancelled"));
                const std::u8string n = req.stems[s].filename().u8string();
                warning += (warning.empty() ? "" : ", ") + std::string(n.begin(), n.end()) + " not read (" + stem.error() + ")";
                continue;
            }
            song = std::max(song, (*stem)->frames);
            if (!mp->submitStem(s, *stem)) return std::unexpected(std::string("internal: stem queue full"));
        }
        if (song == 0 && forgeFrames == 0) return std::unexpected("none of the stems could be read: " + warning);
        song = std::max(song, forgeFrames) + tail;

        // 3) Render: song + limiter look-ahead, dropping the first look-ahead samples.
        const std::uint64_t lat = mp->latencyFrames();
        std::vector<float> outL(kBlock), outR(kBlock);
        float* outs[2] = {outL.data(), outR.data()};
        engine::ProcessContext ctx;
        ctx.outputs = outs; ctx.numOutputs = 2; ctx.frames = kBlock;
        mp->setLoop(false);
        mp->play();
        std::vector<float> L, R;
        L.reserve(static_cast<std::size_t>(song));
        R.reserve(static_cast<std::size_t>(song));
        std::uint64_t produced = 0;
        while (L.size() < song) {
            mp->process(ctx);
            for (std::uint32_t i = 0; i < kBlock && L.size() < song; ++i, ++produced) {
                if (produced < lat) continue;
                L.push_back(outL[i]);
                R.push_back(outR[i]);
            }
            if ((produced / kBlock) % 64 == 0 && !report(0.5f + 0.45f * static_cast<float>(L.size()) / static_cast<float>(song)))
                return std::unexpected(std::string("cancelled"));
        }
        mp->collectRetired();

        ExportResult res;
        res.file    = req.out;
        res.frames  = song;
        res.warning = warning;
        for (std::size_t i = 0; i < L.size(); ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            res.peak = std::max(res.peak, a);
            res.clipped += (std::fabs(L[i]) > 1.0f) + (std::fabs(R[i]) > 1.0f);
        }

        // 4) Write next to the target, then replace it (an old file survives a failed write).
        std::filesystem::path tmp = req.out;
        tmp += ".part";
        if (!writeWav(tmp, L.data(), R.data(), song, hc::kSampleRate, req.encoding)) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return std::unexpected(std::string("could not write the WAV file (folder read-only, disk full, or over 4 GB)"));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, req.out, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return std::unexpected(std::string("could not replace the file (is it open in another program?)"));
        }
        report(1.0f);
        res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return res;
    } catch (const std::exception& e) {
        return std::unexpected(std::string("export failed: ") + e.what());
    }
}

MixExporter::~MixExporter() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
}

bool MixExporter::start(ExportRequest request) {
    if (busy_.load()) return false;
    if (thread_.joinable()) thread_.join();
    cancel_.store(false);
    progress_.store(0.0f);
    busy_.store(true);
    thread_ = std::thread([this, req = std::move(request)] {
        auto r = renderMix(req, [this](float f) {
            progress_.store(f);
            return !cancel_.load();
        });
        {
            std::lock_guard lock(mutex_);
            done_ = std::move(r);
        }
        progress_.store(-1.0f);
        busy_.store(false);
    });
    return true;
}

std::optional<std::expected<ExportResult, std::string>> MixExporter::poll() {
    std::lock_guard lock(mutex_);
    auto r = std::move(done_);
    done_.reset();
    return r;
}

} // namespace hc::media
