#include "StemDeck.hpp"

#include <algorithm>

namespace hc::app {

namespace {
std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

std::size_t StemDeck::load(std::uint32_t first, const std::vector<std::filesystem::path>& inputs) {
    const auto files = media::collectAudioFiles(inputs);
    std::size_t queued = 0;
    for (const auto& f : files) {
        const std::uint32_t s = first + static_cast<std::uint32_t>(queued);
        if (s >= hc::kStemCount) break;
        slots_[s] = {};
        slots_[s].file = f;
        loader_.load(s, f);
        ++queued;
    }
    if (files.empty()) setNotice("No WAV, MP3 or FLAC files in what was dropped");
    else if (queued < files.size())
        setNotice(std::to_string(files.size() - queued) + " file(s) not loaded: only 13 stem slots (from slot " + std::to_string(first + 1) + ")");
    if (queued) host_.setSource(engine::MixerProcessor::Source::Stems);
    return queued;
}

std::size_t StemDeck::replaceAll(const std::vector<std::filesystem::path>& files) {
    host_.stop();
    for (std::uint32_t s = 0; s < hc::kStemCount; ++s) clear(s);
    return load(0, files);
}

void StemDeck::clear(std::uint32_t s) {
    if (s >= hc::kStemCount) return;
    loader_.cancel(s);
    std::erase_if(pending_, [s](const auto& p) { return p.first == s; });
    const bool had = !slots_[s].name.empty();
    slots_[s] = {};
    if (had) pending_.emplace_back(s, nullptr);   // tell the audio thread to drop it
    rebuildOverview();
}

int StemDeck::firstEmptySlot() const noexcept {
    for (std::uint32_t s = 0; s < hc::kStemCount; ++s)
        if (slots_[s].file.empty()) return static_cast<int>(s);
    return -1;
}

void StemDeck::pump() {
    media::StemLoader::Result r;
    while (loader_.poll(r)) {
        StemSlot& sl = slots_[r.slot];
        if (r.audio) {
            sl.name  = r.audio->name;
            sl.info  = media::describeStem(*r.audio);
            sl.bytes = r.audio->bytes();
            sl.frames = r.audio->frames;
            sl.overview = r.audio->overview;
            sl.error.clear();
            std::erase_if(pending_, [&](const auto& p) { return p.first == r.slot; });
            pending_.emplace_back(r.slot, std::move(r.audio));
        } else {
            sl.name.clear();
            sl.error = r.error;
            setNotice("Stem " + std::to_string(r.slot + 1) + " (" + u8(r.file.filename()) + "): " + r.error);
        }
    }
    rebuildOverview();
    while (!pending_.empty()) {
        auto& [slot, audio] = pending_.front();
        if (!host_.submitStem(slot, audio)) break;   // queue full: try again next frame
        pending_.pop_front();
    }
}

std::uint32_t StemDeck::loadedCount() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(slots_.begin(), slots_.end(), [](const StemSlot& s) { return !s.name.empty(); }));
}

std::uint64_t StemDeck::loadedBytes() const noexcept {
    std::uint64_t b = 0;
    for (const auto& s : slots_) b += s.bytes;
    return b;
}

std::uint64_t StemDeck::songFrames() const noexcept {
    std::uint64_t n = 0;
    for (const auto& s : slots_)
        if (!s.name.empty()) n = std::max(n, s.frames);
    return n;
}

void StemDeck::rebuildOverview() {
    const std::uint64_t song = songFrames();
    if (song == 0) { song_.clear(); return; }
    song_.assign(kSongBins, 0.0f);
    for (const auto& s : slots_) {
        if (s.name.empty() || s.overview.empty() || s.frames == 0) continue;
        for (std::size_t b = 0; b < s.overview.size(); ++b) {
            const std::uint64_t at = s.frames * b / s.overview.size();          // frame where this stem bin starts
            const std::size_t   sb = static_cast<std::size_t>(at * kSongBins / song);
            song_[std::min(sb, kSongBins - 1)] = std::max(song_[std::min(sb, kSongBins - 1)], s.overview[b]);
        }
    }
}

void StemDeck::setNotice(std::string text) {
    notice_ = std::move(text);
    noticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(12);
}

std::string StemDeck::notice() const {
    return std::chrono::steady_clock::now() < noticeUntil_ ? notice_ : std::string();
}

} // namespace hc::app
