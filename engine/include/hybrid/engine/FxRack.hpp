#pragma once
// =============================================================================
//  FxRack — HEM effect inserts on the 13 stem strips (3 slots per stem).
//
//  Signal: trim -> channel strip (EQ / compressor) -> INSERT 1 -> 2 -> 3 -> balance / sends.
//  Any HEM effect (hc::fx, the same code as the VST3 plug-ins) can sit in any slot.
//  Bypass per slot: the effect keeps running (its state stays warm) and the output fades over
//  10 ms to the dry signal delayed by the effect's latency, so bypass never clicks or shifts time.
//  Delay compensation: every stem is delayed to the stem with the most insert latency, so all
//  13 strips stay in time (latency() reports it, the console adds it to the limiter look-ahead).
//
//  Threads
//    message thread : setEffect / setBypass / fromText / toText / collectRetired. setEffect
//                     prepares the new effect here (it may allocate) and queues it.
//    audio thread   : process() via StemMatrix's insert hook. Swaps queued effects in, never
//                     allocates, locks or waits. Old effects go back through a ring and are
//                     deleted by collectRetired() on the message thread.
//  Effect parameters are atomics: the faceplate turns knobs on the live effect directly.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/fx/Fx.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace hc::engine {

class FxRack final : public dsp::StemInsertHook {
public:
    static constexpr std::uint32_t kStems = hc::kStemCount;
    static constexpr int           kSlots = 3;
    static constexpr std::uint32_t kMaxLatency = 8192;   // per stem, all three slots together

    FxRack();
    ~FxRack() override;
    FxRack(const FxRack&)            = delete;
    FxRack& operator=(const FxRack&) = delete;

    // Message thread, before the rack is used on the audio thread (and again when the block size changes).
    void prepare(double sampleRate, std::uint32_t maxFrames);

    // ---- message thread ------------------------------------------------------------------
    // type = index into hc::fx::effects(), -1 = empty the slot. False if the queue is full.
    bool setEffect(std::uint32_t stem, int slot, int type);
    [[nodiscard]] int         type(std::uint32_t stem, int slot) const noexcept;
    // The live effect in a slot (for the faceplate), or nullptr. Valid until the slot changes.
    [[nodiscard]] fx::Effect* effect(std::uint32_t stem, int slot) const noexcept;
    void setBypass(std::uint32_t stem, int slot, bool on) noexcept;
    [[nodiscard]] bool bypass(std::uint32_t stem, int slot) const noexcept;
    [[nodiscard]] bool anyEffect() const noexcept;
    [[nodiscard]] int  count(std::uint32_t stem) const noexcept;
    void setTempo(double bpm) noexcept;
    // The song's tempo as set in the FX RACK (message thread). 0 = follow Beat Forge's tempo.
    // Saved with the rack text ("tempo=92"), so sessions and exports keep it.
    void setSongTempo(float bpm) noexcept { songTempo_ = bpm <= 0.0f ? 0.0f : std::clamp(bpm, 40.0f, 250.0f); }
    [[nodiscard]] float songTempo() const noexcept { return songTempo_; }
    void clear();                     // empties every slot
    void collectRetired() noexcept;
    // Total delay the rack adds right now (all stems are aligned to it). Any thread.
    [[nodiscard]] std::uint32_t latency() const noexcept { return latencyPub_.load(std::memory_order_relaxed); }
    // Latency the rack will have once every queued change has reached the audio thread (message thread).
    [[nodiscard]] std::uint32_t plannedLatency() const noexcept;

    // Text for sessions / exports: one line per used slot,
    //   stem03.fx1=HEM PUNCH|bypass=0|attack=40|sustain=-10 ...
    [[nodiscard]] std::string toText() const;
    // Replaces every slot from text (unknown effects / ids are skipped). Returns a note on anything skipped.
    std::string fromText(const std::string& text);

    // ---- audio thread (StemInsertHook) -------------------------------------------------------
    [[nodiscard]] bool active(std::uint32_t stem) noexcept override;
    void process(std::uint32_t stem, float* L, float* R, std::uint32_t frames) noexcept override;
    void beginBlock() noexcept override;   // takes queued effects, works out the delay compensation
    // Delays a signal that does not pass through the stems (Beat Forge's FX return) by latency().
    void delayAux(int channel, float* x, std::uint32_t frames) noexcept;

private:
    struct Command { std::uint32_t stem; int slot; fx::Effect* effect; };
    struct Slot {
        fx::Effect*        fx = nullptr;     // audio thread's
        float              mix = 1.0f;       // 1 = effect, 0 = bypassed (ramped)
        std::vector<float> dry[2];           // delay line for the bypass path
        std::uint32_t      dryPos = 0;
    };
    struct Pdc { std::vector<float> buf[2]; std::uint32_t pos = 0; };

    void runSlot(Slot& s, std::uint32_t stem, int slot, float* L, float* R, std::uint32_t n) noexcept;

    double        fs_ = hc::kSampleRate;
    std::uint32_t maxFrames_ = 0;
    std::array<std::array<Slot, kSlots>, kStems>                    slots_{};
    std::array<std::array<std::atomic<bool>, kSlots>, kStems>       bypass_{};
    std::array<std::array<fx::Effect*, kSlots>, kStems>             msg_{};    // message thread's view
    std::array<std::array<int, kSlots>, kStems>                     types_{};
    std::array<Pdc, kStems> pdc_{};
    Pdc                     aux_;
    std::array<std::uint32_t, kStems> stemLat_{};
    std::uint32_t maxLat_ = 0;
    std::atomic<std::uint32_t> latencyPub_{0};
    std::atomic<float> tempo_{120.0f};
    float songTempo_ = 0.0f;
    float fadeStep_ = 0.002f;
    core::SpscRing<Command, 64>      commands_;
    core::SpscRing<fx::Effect*, 128> retired_;
};

} // namespace hc::engine
