#include "RemakeJob.hpp"

#include "AudioHost.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/Resampler.hpp"

#include <algorithm>
#include <cwctype>

namespace hc::app {

StemRole roleFromName(const std::filesystem::path& file) {
    std::wstring n = file.stem().wstring();
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    auto has = [&](const wchar_t* w) { return n.find(w) != std::wstring::npos; };
    if (has(L"vocal") || has(L"vox") || has(L"acapella") || has(L"a cappella") || has(L"adlib")) return StemRole::Skip;
    if (has(L"808") || has(L"bass") || has(L"sub")) return StemRole::Bass;
    if (has(L"drum") || has(L"kick") || has(L"snare") || has(L"clap") || has(L"hat") || has(L"perc") || has(L"beat")) return StemRole::Drums;
    if (has(L"mix") || has(L"master") || has(L"full")) return StemRole::Mix;
    return StemRole::Music;
}

RemakeJob::~RemakeJob() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool RemakeJob::start(std::vector<std::filesystem::path> files, std::uint32_t rate) {
    if (busy() || files.empty()) return false;
    if (thread_.joinable()) thread_.join();
    cancel_.store(false);
    progress_.store(0);
    {
        std::lock_guard l(m_);
        done_.reset();
    }
    what_ = files.size() == 1 ? toUtf8(files.front().filename().wstring()) : std::to_string(files.size()) + " stems";
    busy_.store(true, std::memory_order_release);
    thread_ = std::thread([this, f = std::move(files), rate]() mutable { run(std::move(f), rate); });
    return true;
}

std::optional<forge::RemakeResult> RemakeJob::poll() {
    std::lock_guard l(m_);
    if (!done_) return std::nullopt;
    auto r = std::move(done_);
    done_.reset();
    return r;
}

void RemakeJob::run(std::vector<std::filesystem::path> files, std::uint32_t rate) {
    forge::RemakeResult result;
    const bool single = files.size() == 1;
    std::vector<float> mixL, mixR, drums, bass, music;
    auto addTo = [](std::vector<float>& dst, const std::vector<float>& l, const std::vector<float>& r) {
        if (dst.size() < l.size()) dst.resize(l.size(), 0.0f);
        for (std::size_t i = 0; i < l.size(); ++i) dst[i] += 0.5f * (l[i] + r[i]);
    };
    const float decodeShare = 0.5f / static_cast<float>(files.size());
    bool failed = false;
    for (std::size_t i = 0; i < files.size() && !failed; ++i) {
        const StemRole role = single ? StemRole::Mix : roleFromName(files[i]);
        if (role == StemRole::Skip) continue;
        const float base = decodeShare * static_cast<float>(i);
        auto prog = [&](float p) { progress_.store(base + decodeShare * p, std::memory_order_relaxed); return !cancel_.load(std::memory_order_relaxed); };
        auto d = media::decodeFile(files[i], prog);
        if (!d) { result.error = files[i].filename().string() + ": " + d.error(); failed = true; break; }
        if (d->sampleRate != rate && !media::resampleTo(*d, rate, prog)) { result.error = cancel_.load() ? "cancelled" : "could not convert the sample rate"; failed = true; break; }
        switch (role) {
        case StemRole::Mix:
            if (mixL.size() < d->left.size()) { mixL.resize(d->left.size(), 0.0f); mixR.resize(d->left.size(), 0.0f); }
            for (std::size_t k = 0; k < d->left.size(); ++k) { mixL[k] += d->left[k]; mixR[k] += d->right[k]; }
            break;
        case StemRole::Drums: addTo(drums, d->left, d->right); break;
        case StemRole::Bass:  addTo(bass, d->left, d->right); break;
        default:              addTo(music, d->left, d->right); break;
        }
    }
    if (!failed) {
        if (!single && mixL.empty()) {   // stems only: their sum is the mix (tempo, sections)
            std::size_t n = std::max({drums.size(), bass.size(), music.size()});
            mixL.assign(n, 0.0f);
            for (auto* v : {&drums, &bass, &music})
                for (std::size_t k = 0; k < v->size(); ++k) mixL[k] += (*v)[k];
            mixR = mixL;
        }
        forge::RemakeInput in;
        in.sampleRate = rate;
        in.mixL = mixL.data(); in.mixR = mixR.data(); in.mixFrames = mixL.size();
        if (!drums.empty()) { in.drums = drums.data(); in.drumFrames = drums.size(); }
        if (!bass.empty())  { in.bass = bass.data();   in.bassFrames = bass.size(); }
        if (!music.empty()) { in.music = music.data(); in.musicFrames = music.size(); }
        if (mixL.empty()) result.error = "no usable audio (only vocal stems?)";
        else result = forge::remake(in, [&](float p) { progress_.store(0.5f + 0.5f * p, std::memory_order_relaxed); return !cancel_.load(std::memory_order_relaxed); });
    }
    {
        std::lock_guard l(m_);
        done_ = std::move(result);
    }
    busy_.store(false, std::memory_order_release);
}

} // namespace hc::app
