#pragma once
// =============================================================================
//  StemLoader — decodes and resamples stem files on background threads.
//
//  load(slot, file) queues a job (replacing any job for that slot); worker threads
//  decode (WAV/MP3/FLAC), convert to the rig rate and build an engine::StemAudio.
//  The message thread polls finished results and hands the audio to the engine
//  (MixerProcessor::submitStem). Progress per slot is readable at any time.
//  Never touches the audio thread. Thread-safe for one message thread.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/engine/StemAudio.hpp"
#include "hybrid/media/AudioFile.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hc::media {

// Expands folders (one level), keeps WAV/MP3/FLAC files and sorts them naturally by name
// ("2 Snare" before "10 Vox"), case-insensitive.
[[nodiscard]] std::vector<std::filesystem::path> collectAudioFiles(const std::vector<std::filesystem::path>& inputs);
[[nodiscard]] bool naturalLess(const std::filesystem::path& a, const std::filesystem::path& b);

// One short ASCII line for a loaded stem: codec, source rate if converted (else bit depth), length.
// e.g. "MP3 44.1k 3:42", "WAV 24b 4:05", "WAV 32f 1:10".
[[nodiscard]] std::string describeStem(const engine::StemAudio& stem);

// Decode + convert one file to a ready StemAudio on the calling thread (what the workers run).
// progress(0..1) may cancel by returning false.
[[nodiscard]] std::expected<std::unique_ptr<engine::StemAudio>, std::string> buildStem(const std::filesystem::path& file, std::uint32_t outRate,
                                                                                      const Progress& progress = {}) noexcept;

class StemLoader {
public:
    static constexpr std::uint32_t kSlots = hc::kStemCount;

    explicit StemLoader(unsigned workers = 4, std::uint32_t outRate = hc::kSampleRate);
    ~StemLoader();   // cancels running jobs and joins the workers
    StemLoader(const StemLoader&)            = delete;
    StemLoader& operator=(const StemLoader&) = delete;

    void load(std::uint32_t slot, const std::filesystem::path& file);
    void cancel(std::uint32_t slot);
    void cancelAll();

    struct Result {
        std::uint32_t                     slot = 0;
        std::filesystem::path             file;
        std::unique_ptr<engine::StemAudio> audio;   // null on failure
        std::string                       error;
        double                            seconds = 0;   // wall time of the job
    };
    // Message thread: takes one finished result (stale results of replaced jobs are dropped).
    [[nodiscard]] bool poll(Result& out);

    // 0..1 while the slot's job is queued or running, -1 when idle.
    [[nodiscard]] float progress(std::uint32_t slot) const noexcept;
    [[nodiscard]] bool  busy() const noexcept;

private:
    struct Job {
        std::uint32_t         slot = 0;
        std::uint64_t         generation = 0;
        std::filesystem::path file;
    };
    void worker() noexcept;
    void run(const Job& job) noexcept;

    const std::uint32_t outRate_;
    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::deque<Job>         queue_;
    std::deque<Result>      done_;
    bool                    quit_ = false;
    std::array<std::uint64_t, kSlots>              generation_{};   // guarded by mutex_
    std::array<std::atomic<std::uint64_t>, kSlots> live_{};         // generation allowed to report / continue
    std::array<std::atomic<float>, kSlots>         progress_{};
    std::vector<std::thread> workers_;
};

} // namespace hc::media
