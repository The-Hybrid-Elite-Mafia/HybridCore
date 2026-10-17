#pragma once
// =============================================================================
//  RemakeJob — runs REMAKE on a background thread (message thread starts / polls it):
//  decodes the chosen file(s), converts to the rig rate, sorts stems by file name
//  (drum / kick / snare / hat / perc → drums; bass / 808 / sub → bass; vocal / vox /
//  acapella → skipped; anything else → music) and calls forge::remake().
//  Never touches the audio thread; nothing leaves this PC.
// =============================================================================
#include "hybrid/forge/Remake.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hc::app {

enum class StemRole { Mix, Drums, Bass, Music, Skip };
// Which part a file is, from its name. One file alone is always the full mix.
[[nodiscard]] StemRole roleFromName(const std::filesystem::path& file);

class RemakeJob {
public:
    ~RemakeJob();
    bool start(std::vector<std::filesystem::path> files, std::uint32_t rate);   // false if busy or no files
    [[nodiscard]] bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }
    void cancel() noexcept { cancel_.store(true, std::memory_order_release); }
    [[nodiscard]] float progress() const noexcept { return progress_.load(std::memory_order_relaxed); }
    // The finished result once (then empty until the next start).
    [[nodiscard]] std::optional<forge::RemakeResult> poll();
    [[nodiscard]] const std::string& what() const noexcept { return what_; }   // "Hybrid Elite Mafia.wav" or "4 stems"

private:
    void run(std::vector<std::filesystem::path> files, std::uint32_t rate);
    std::thread              thread_;
    std::atomic<bool>        busy_{false}, cancel_{false};
    std::atomic<float>       progress_{0};
    std::mutex               m_;
    std::optional<forge::RemakeResult> done_;
    std::string              what_;
};

} // namespace hc::app
