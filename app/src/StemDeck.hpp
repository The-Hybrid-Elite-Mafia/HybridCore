#pragma once
// =============================================================================
//  StemDeck — the 13 stem slots as the app sees them (message thread only).
//  Owns the background StemLoader, remembers which file sits in which slot, hands
//  finished stems to the audio thread through AudioHost (retrying while its queue is
//  full) and keeps a short notice for the status bar (load errors, skipped files).
// =============================================================================
#include "AudioHost.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/media/StemLoader.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hc::app {

struct StemSlot {
    std::filesystem::path file;      // empty = nothing assigned
    std::string           name;      // set when loaded
    std::string           info;      // describeStem()
    std::string           error;     // set when the load failed
    std::uint64_t         bytes = 0; // RAM held by the loaded audio
    std::uint64_t         frames = 0;
    std::vector<float>    overview;  // StemAudio::overview, copied before the audio goes to the audio thread
};

class StemDeck {
public:
    explicit StemDeck(AudioHost& host, unsigned workers = 4) : host_(host), loader_(workers) {}

    // Loads files (folders are expanded, natural order) into consecutive slots from `first`.
    // Returns how many were queued; files past slot 13 are skipped (reported in the notice).
    std::size_t load(std::uint32_t first, const std::vector<std::filesystem::path>& files);
    // Empties every slot, then loads `files` from slot 1 (the LOAD button).
    std::size_t replaceAll(const std::vector<std::filesystem::path>& files);
    void clear(std::uint32_t slot);
    [[nodiscard]] int firstEmptySlot() const noexcept;

    // Call every UI frame: collects finished loads, submits them to the audio thread.
    void pump();

    [[nodiscard]] const StemSlot& slot(std::uint32_t s) const noexcept { return slots_[s]; }
    [[nodiscard]] float progress(std::uint32_t s) const noexcept { return loader_.progress(s); }
    [[nodiscard]] bool  loading() const noexcept { return loader_.busy() || !pending_.empty(); }
    [[nodiscard]] std::uint32_t loadedCount() const noexcept;
    [[nodiscard]] std::uint64_t loadedBytes() const noexcept;
    // Peaks across the whole song (longest stem = full width), kSongBins values 0..1; empty when nothing is loaded.
    [[nodiscard]] const std::vector<float>& songOverview() const noexcept { return song_; }
    [[nodiscard]] std::uint64_t songFrames() const noexcept;
    static constexpr std::size_t kSongBins = 1024;
    // Notice for the status bar, empty when there is nothing recent to say.
    [[nodiscard]] std::string notice() const;

private:
    void setNotice(std::string text);
    void rebuildOverview();
    std::vector<float> song_;

    AudioHost&         host_;
    media::StemLoader  loader_;
    std::array<StemSlot, hc::kStemCount> slots_{};
    std::deque<std::pair<std::uint32_t, std::unique_ptr<engine::StemAudio>>> pending_;   // waiting for queue space
    std::string        notice_;
    std::chrono::steady_clock::time_point noticeUntil_{};
};

} // namespace hc::app
