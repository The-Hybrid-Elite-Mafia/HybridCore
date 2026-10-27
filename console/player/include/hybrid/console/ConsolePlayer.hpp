#pragma once
// =============================================================================
//  HEM DIGITAL CONSOLE — playing the rig (hc::console::player).
//
//  ConsoleProcessor : the engine's audio processor. 15 file slots (13 desk channels + EXT 1 + EXT 2) on one
//                     transport (play / pause / stop / seek / loop) feed the HEM CONSOLE RIG; the output device
//                     plays what LISTEN picks: the MONITOR section (monitor + ALT outputs) or a cue mix.
//                     Stem hand-off is lock-free (command ring in, retire ring out); the audio thread never
//                     allocates, frees, locks or waits. Starts and seeks fade in, pauses fade out (no clicks).
//  Export           : renders the song offline through a fresh rig with a snapshot of every control
//                     (the same code as playback): the MAIN OUT to one WAV, or every channel's DIRECT OUT to
//                     its own WAV (PRINT STEMS). Files are written as they render (no whole-song buffers).
//  Session          : the .hemc text file — stem files (absolute + relative paths), app choices, every control.
// =============================================================================
#include "hybrid/console/ConsoleRig.hpp"
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/engine/AudioProcessor.hpp"
#include "hybrid/engine/StemAudio.hpp"

#include <array>
#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hc::console {

inline constexpr int kPlayerSlots = ConsoleRig::kChannels + 2;   // 13 channels + EXT 1 + EXT 2
inline constexpr int kExt1 = ConsoleRig::kChannels, kExt2 = ConsoleRig::kChannels + 1;

class ConsoleProcessor final : public engine::AudioProcessor {
public:
    enum Listen : int { ListenMonitor = 0, ListenCue1, ListenCue2 };

    explicit ConsoleProcessor(ConsoleRig& rig) noexcept : rig_(rig) {}
    ~ConsoleProcessor() override;   // message thread: frees every stem it still holds
    ConsoleProcessor(const ConsoleProcessor&) = delete;
    ConsoleProcessor& operator=(const ConsoleProcessor&) = delete;

    void prepare(const io::StreamConfig& config) override;
    void process(const engine::ProcessContext& ctx) noexcept override;

    // ---- message thread ----
    // Queues `audio` for `slot` (nullptr clears it). On success ownership moves here and `audio` is emptied.
    [[nodiscard]] bool submit(int slot, std::unique_ptr<engine::StemAudio>& audio) noexcept;
    void collectRetired() noexcept;
    void play() noexcept { wantPlay_.store(true, std::memory_order_release); }
    void pause() noexcept { wantPlay_.store(false, std::memory_order_release); }
    void stop() noexcept { pause(); seek(0); }
    void seek(std::uint64_t frame) noexcept { seekReq_.store(static_cast<std::int64_t>(frame), std::memory_order_release); }
    void setLoop(bool on) noexcept { loop_.store(on, std::memory_order_relaxed); }
    void setListen(int l) noexcept { listen_.store(l, std::memory_order_relaxed); }
    [[nodiscard]] int listen() const noexcept { return listen_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool loop() const noexcept { return loop_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool wantsPlay() const noexcept { return wantPlay_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool playing() const noexcept { return playingPub_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t playhead() const noexcept { return playheadPub_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t songFrames() const noexcept { return songPub_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return rate_; }

private:
    struct Command { int slot; engine::StemAudio* audio; };
    static constexpr std::size_t kQueue = 64, kRetire = 64;
    void drain() noexcept;
    bool render(std::uint32_t frames) noexcept;   // stems into stem_, false = silent (stopped / nothing loaded)

    ConsoleRig& rig_;
    std::array<engine::StemAudio*, kPlayerSlots> slots_{};
    core::SpscRing<Command, kQueue> commands_;
    core::SpscRing<engine::StemAudio*, kRetire> retired_;
    std::vector<float> stem_;                          // kPlayerSlots × 2 × maxFrames
    std::vector<float> mainL_, mainR_, monL_, monR_, altL_, altR_, c1L_, c1R_, c2L_, c2R_;
    std::uint32_t maxFrames_ = 0, rate_ = 48000;
    std::uint64_t song_ = 0, playhead_ = 0;
    bool playing_ = false;
    std::atomic<bool> wantPlay_{false}, loop_{false}, playingPub_{false};
    std::atomic<std::int64_t> seekReq_{-1};
    std::atomic<int> listen_{ListenMonitor};
    std::atomic<std::uint64_t> playheadPub_{0}, songPub_{0};
};

// ---- export --------------------------------------------------------------------------------------
struct RenderSource {             // one slot's audio at the rig rate (null = empty slot)
    const float* left = nullptr;
    const float* right = nullptr;
    std::uint64_t frames = 0;
};
struct ExportJob {
    enum class Kind : std::uint8_t { Mix, Stems };
    Kind kind = Kind::Mix;
    std::array<std::filesystem::path, kPlayerSlots> files{};   // used by ConsoleExporter (decoded on its thread)
    std::array<std::string, ConsoleRig::kChannels> names{};    // PRINT STEMS file names (channel 3 "03 Vocals.wav")
    std::string rigState;                                      // ConsoleRig::saveText() taken when the export starts
    std::filesystem::path out;                                 // Mix: the .wav; Stems: the folder
    bool float32 = false;                                      // false = 24-bit PCM
};
struct ExportResult {
    std::vector<std::filesystem::path> files;
    std::uint64_t frames = 0;           // per file
    double seconds = 0;                 // wall time
    float peakDb = -144;                // highest sample of the main mix
    std::uint64_t clipped = 0;          // samples over full scale written (24-bit PCM clips them)
    double integratedLufs = -144;       // the main mix, measured by the HEM METER BRIDGE
    double truePeakDbtp = -144;
    std::uint32_t latencyTrimmed = 0;   // samples of rack latency removed from the start
    std::string warning;
};
using ExportProgress = std::function<bool(float)>;   // 0..1; return false to cancel
// Renders on the calling thread. The output starts on the song's first sample (rack latency removed) and lasts as
// long as the longest file, plus 2 s when an engaged TAP ECHO would otherwise be cut off.
[[nodiscard]] std::expected<ExportResult, std::string> renderExport(const ExportJob& job, const std::array<RenderSource, kPlayerSlots>& src,
                                                                    const ExportProgress& progress = {});

// Background export: decodes the job's files (any rate, converted to 48 kHz) and renders them.
class ConsoleExporter {
public:
    ConsoleExporter() = default;
    ~ConsoleExporter();
    ConsoleExporter(const ConsoleExporter&) = delete;
    ConsoleExporter& operator=(const ConsoleExporter&) = delete;
    bool start(ExportJob job);   // false while one is running
    void cancel() noexcept { cancel_.store(true); }
    [[nodiscard]] bool busy() const noexcept { return busy_.load(); }
    [[nodiscard]] float progress() const noexcept { return progress_.load(); }
    // Message thread: the finished result once (true), then nothing until the next export.
    bool takeResult(std::expected<ExportResult, std::string>& out);

private:
    std::thread thread_;
    std::atomic<bool> busy_{false}, cancel_{false}, done_{false};
    std::atomic<float> progress_{-1.0f};
    std::expected<ExportResult, std::string> result_{std::unexpected(std::string())};
};

// ---- session file (.hemc) --------------------------------------------------------------------------
struct SessionData {
    std::array<std::filesystem::path, kPlayerSlots> files{};
    int listen = 0;
    bool loop = false;
    std::string rigState;   // ConsoleRig::saveText()
};
[[nodiscard]] std::string writeSession(const SessionData& s, const std::filesystem::path& sessionFile);
// Missing files are reported in `missing` (slot numbers); relative paths are tried when absolute ones are gone.
[[nodiscard]] std::expected<SessionData, std::string> readSession(const std::string& text, const std::filesystem::path& sessionFile,
                                                                 std::vector<int>* missing = nullptr);
inline constexpr const char* kSessionExtension = ".hemc";

} // namespace hc::console
