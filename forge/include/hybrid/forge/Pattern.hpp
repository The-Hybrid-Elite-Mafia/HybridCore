#pragma once
// =============================================================================
//  Beat Forge song data: a pattern (1–8 bars of notes per track) and an arrangement
//  (which tracks play in each bar of the song). Plain data, copied whole between threads
//  through a lock-free triple buffer.
// =============================================================================
#include "hybrid/forge/Forge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hc::forge {

inline constexpr int kStepsPerBar = 16;   // 16th notes
inline constexpr int kMaxBars     = 8;
inline constexpr int kMaxSteps    = kStepsPerBar * kMaxBars;
inline constexpr int kMaxNotes    = 512;  // per track
inline constexpr int kMaxSongBars = 256;

// One note. Times are in 16th-note steps from the start of the pattern (fractions allowed:
// 32nd rolls, triplets, humanize). Drum tracks use the pitch to pick the sound (GM numbers).
struct Note {
    float         start    = 0.0f;
    float         length   = 1.0f;
    std::uint8_t  pitch    = 60;
    std::uint8_t  velocity = 100;   // 1..127
};

namespace drum {
inline constexpr int kick = 36, rim = 37, snare = 38, clap = 39, snare2 = 40;
inline constexpr int hatClosed = 42, hatPedal = 44, hatOpen = 46;
inline constexpr int tomLow = 45, tomMid = 47, tomHigh = 50, crash = 49, tamb = 54, cowbell = 56, shaker = 70, clave = 75;
} // namespace drum

struct TrackNotes {
    int count = 0;
    std::array<Note, kMaxNotes> notes{};

    void clear() noexcept { count = 0; }
    bool add(const Note& n) noexcept {   // ignored when full; call sort() after a batch
        if (count >= kMaxNotes) return false;
        notes[static_cast<std::size_t>(count++)] = n;
        return true;
    }
    void sort() noexcept {
        std::stable_sort(notes.begin(), notes.begin() + count, [](const Note& a, const Note& b) { return a.start < b.start; });
    }
};

struct Pattern {
    int bars = 4;
    std::array<TrackNotes, kTracks> tracks{};

    [[nodiscard]] int lengthSteps() const noexcept { return std::clamp(bars, 1, kMaxBars) * kStepsPerBar; }
    [[nodiscard]] bool isEmpty() const noexcept {
        for (const auto& t : tracks) if (t.count > 0) return false;
        return true;
    }
    [[nodiscard]] int noteCount() const noexcept {
        int n = 0;
        for (const auto& t : tracks) n += t.count;
        return n;
    }
    void clear() noexcept { for (auto& t : tracks) t.clear(); }
};

// Song form: one entry per bar. bars == 0 means "loop the pattern" (no arrangement).
enum Section : std::uint8_t { kIntro = 0, kVerse, kHook, kBridge, kDrop, kOutro, kSections };
inline constexpr const char* kSectionNames[kSections] = {"INTRO", "VERSE", "HOOK", "BRIDGE", "DROP", "OUTRO"};

struct Arrangement {
    int bars = 0;
    std::array<std::uint8_t, kMaxSongBars> mask{};      // bit t set = track t plays in that bar
    std::array<std::uint8_t, kMaxSongBars> section{};   // Section of that bar (display)
    [[nodiscard]] bool active() const noexcept { return bars > 0; }
};

struct Song {
    Pattern     pattern;
    Arrangement arrangement;
};

// ---- text form (sessions, export requests) ----
// Lines "key=value": bars, one line per track ("start,length,pitch,velocity;..."), arr.bars,
// arr.mask (2 hex digits per bar), arr.sect (1 digit per bar).
[[nodiscard]] std::string songToText(const Song& song);
// Tolerant: unknown keys are ignored, values are clamped. Returns false if nothing usable was found.
bool songFromText(std::string_view text, Song& out);

// Lock-free single-writer / single-reader triple buffer. The writer (message thread) fills
// writeBuffer() and publishes; the reader (audio thread) calls update() once per block and
// reads read(). Neither side ever waits, allocates or sees a half-written object.
template <class T>
class TripleBuffer {
public:
    TripleBuffer() : slots_(std::make_unique<T[]>(3)) {}
    T&   writeBuffer() noexcept { return slots_[static_cast<std::size_t>(writeIdx_)]; }
    void publish() noexcept { writeIdx_ = middle_.exchange(writeIdx_ | kDirty, std::memory_order_acq_rel) & kIndex; }
    bool update() noexcept {   // reader: true when a newer object was swapped in
        if ((middle_.load(std::memory_order_acquire) & kDirty) == 0) return false;
        readIdx_ = middle_.exchange(readIdx_, std::memory_order_acq_rel) & kIndex;
        return true;
    }
    [[nodiscard]] const T& read() const noexcept { return slots_[static_cast<std::size_t>(readIdx_)]; }

private:
    static constexpr int kIndex = 3, kDirty = 4;
    std::unique_ptr<T[]> slots_;
    std::atomic<int>     middle_{1};
    int                  writeIdx_ = 0, readIdx_ = 2;
};

} // namespace hc::forge
