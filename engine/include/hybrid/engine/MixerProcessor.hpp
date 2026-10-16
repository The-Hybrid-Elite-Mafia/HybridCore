#pragma once
// =============================================================================
//  MixerProcessor — runs the 13-stem matrix inside the engine.
//
//  Stem sources:
//    Hardware  : stem s <- input channels 2s / 2s+1 (mono if only 2s exists, silent if neither)
//    TestTones : stem s <- internal sine, A2 (110 Hz) stacked in minor thirds, -30 dBFS each
//    Stems     : stem s <- loaded audio (StemAudio), all slots on one transport: play / pause /
//                stop / seek, loop on (wraps at the end of the song) or off (stops at the end and
//                returns to the start). Song length = longest loaded stem; shorter stems play
//                silence after their end. Starts and seeks fade in over 64 samples, pause / stop
//                fade out over one block, so the transport never clicks.
//  Bus b is written to output channels 2b / 2b+1 when the device has them (bus 0 = Main = outs 1/2).
//
//  Stem hand-off is lock-free and allocation-free on the audio thread:
//    message thread  submitStem()  --commands ring-->  audio thread swaps the slot pointer
//    message thread  collectRetired() <--retired ring--  previous StemAudio (deleted here)
//  matrix() setters and meters are callable from the message thread while the processor
//  is installed in the engine; lifetime ends when the engine retires the processor.
// =============================================================================
#include "hybrid/engine/AudioProcessor.hpp"
#include "hybrid/engine/StemAudio.hpp"
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/dsp/StemMatrix.hpp"

#include <array>
#include <atomic>
#include <memory>

namespace hc::engine {

class MixerProcessor final : public AudioProcessor {
public:
    enum class Source : std::uint8_t { Hardware, TestTones, Stems };

    explicit MixerProcessor(Source source = Source::Hardware) noexcept;
    ~MixerProcessor() override;   // message thread, after the engine released it: frees every stem it still holds
    MixerProcessor(const MixerProcessor&)            = delete;
    MixerProcessor& operator=(const MixerProcessor&) = delete;

    void setSource(Source s) noexcept { source_.store(s, std::memory_order_relaxed); }
    [[nodiscard]] Source source() const noexcept { return source_.load(std::memory_order_relaxed); }
    [[nodiscard]] dsp::StemMatrix&       matrix() noexcept { return matrix_; }
    [[nodiscard]] const dsp::StemMatrix& matrix() const noexcept { return matrix_; }

    // ---- stems (message thread) ----------------------------------------------------
    // Queues `audio` for `slot` (nullptr clears the slot). On success ownership moves to
    // the processor and `audio` is left empty; false when the queue is full (try again later).
    [[nodiscard]] bool submitStem(std::uint32_t slot, std::unique_ptr<StemAudio>& audio) noexcept;
    // Deletes stems the audio thread has released. Call regularly (each UI frame).
    void collectRetired() noexcept;
    // ---- transport (message thread; applied at the next block) ----------------------
    void play() noexcept { wantPlay_.store(true, std::memory_order_release); }
    void pause() noexcept { wantPlay_.store(false, std::memory_order_release); }
    void stop() noexcept { pause(); seek(0); }
    void seek(std::uint64_t frame) noexcept { seekReq_.store(static_cast<std::int64_t>(frame), std::memory_order_release); }
    void setLoop(bool on) noexcept { loop_.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool loop() const noexcept { return loop_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool wantsPlay() const noexcept { return wantPlay_.load(std::memory_order_relaxed); }
    // Published by the audio thread after every block.
    [[nodiscard]] bool          playing() const noexcept { return playingPub_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t stemPlayhead() const noexcept { return playheadPub_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t stemSongFrames() const noexcept { return songPub_.load(std::memory_order_relaxed); }

    // Test-tone frequency for a stem (Hz). Exposed for tests and UI labels.
    [[nodiscard]] static double testToneHz(std::uint32_t stem) noexcept;

    void prepare(const io::StreamConfig& config) override;
    void process(const ProcessContext& ctx) noexcept override;

    static constexpr std::size_t kStemQueue   = 32;
    static constexpr std::size_t kRetireQueue = 64;

private:
    struct Osc { double c = 1.0, s = 0.0, x = 1.0, y = 0.0; };
    struct StemCommand {
        std::uint32_t slot  = 0;
        StemAudio*    audio = nullptr;
    };

    void drainStemCommands() noexcept;   // audio thread
    bool renderStems(std::uint32_t frames) noexcept;   // false: silent this block

    std::atomic<Source>                               source_;
    dsp::StemMatrix                                   matrix_;
    core::AlignedBuffer<float>                        toneData_;
    core::AlignedBuffer<float>                        stemData_;   // 13 × 2 × maxFrames
    std::array<Osc, dsp::StemMatrix::kStems>          osc_{};
    std::uint32_t                                     maxFrames_ = 0;

    // Stems: slots_ and playhead_ are audio-thread state (message thread only touches them in the destructor).
    std::array<StemAudio*, dsp::StemMatrix::kStems>   slots_{};
    std::uint64_t                                     playhead_   = 0;
    std::uint64_t                                     songFrames_ = 0;
    bool                                              playing_    = false;   // audio-thread view of the transport
    core::SpscRing<StemCommand, kStemQueue>           commands_;
    core::SpscRing<StemAudio*, kRetireQueue>          retired_;
    std::atomic<bool>                                 wantPlay_{false};
    std::atomic<bool>                                 loop_{true};
    std::atomic<std::int64_t>                         seekReq_{-1};
    std::atomic<bool>                                 playingPub_{false};
    std::atomic<std::uint64_t>                        playheadPub_{0};
    std::atomic<std::uint64_t>                        songPub_{0};
};

} // namespace hc::engine
