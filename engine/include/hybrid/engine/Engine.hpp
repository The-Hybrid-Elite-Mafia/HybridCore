#pragma once
// =============================================================================
//  Engine — the lock-free audio callback.
//
//  Thread contract
//    Message thread : install(), setMasterGain(), setMuted(), collectGarbage(),
//                     popTelemetry(), prepare() (stream stopped only).
//    RT thread      : process() only.
//
//  Communication is exclusively through three wait-free SPSC rings:
//    commands_  : message -> RT   (processor swaps, gain, mute)
//    retired_   : RT -> message   (replaced processors, deleted off the RT thread)
//    telemetry_ : RT -> message   (load, overruns, ~20 reports/sec)
//  The RT thread never locks, allocates, frees, or waits.
// =============================================================================
#include "hybrid/engine/AudioProcessor.hpp"
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/io/AudioCallback.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace hc::engine {

struct Telemetry {
    std::uint64_t sampleTime = 0;
    float         loadAvg    = 0.0f;   // fraction of the block period spent in process()
    float         loadPeak   = 0.0f;
    std::uint32_t overruns   = 0;      // blocks whose processing exceeded the period (cumulative)
    float         masterGain = 0.0f;   // effective gain at end of window
};

class Engine final : public io::AudioCallback {
public:
    Engine() = default;
    ~Engine() override;
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- message thread ----------------------------------------------------
    // On success the engine takes ownership and `processor` is left empty.
    [[nodiscard]] bool install(std::unique_ptr<AudioProcessor>& processor);
    [[nodiscard]] bool setMasterGain(float linear) noexcept;
    [[nodiscard]] bool setMuted(bool muted) noexcept;
    void               collectGarbage() noexcept;
    [[nodiscard]] bool popTelemetry(Telemetry& out) noexcept;
    [[nodiscard]] bool isPrepared() const noexcept { return prepared_; }
    [[nodiscard]] const io::StreamConfig& config() const noexcept { return config_; }

    // ---- io::AudioCallback ---------------------------------------------------
    void prepare(const io::StreamConfig& config) override;
    void process(const float* const* inputs, std::uint32_t numInputs,
                 float* const* outputs, std::uint32_t numOutputs,
                 std::uint32_t frames) noexcept override;

private:
    struct Command {
        enum class Type : std::uint8_t { Swap, Gain, Mute };
        Type            type  = Type::Gain;
        AudioProcessor* proc  = nullptr;
        float           value = 0.0f;
    };

    static constexpr std::size_t   kCommandCapacity   = 256;
    static constexpr std::size_t   kRetireCapacity    = 64;
    static constexpr std::size_t   kTelemetryCapacity = 64;
    static constexpr std::uint32_t kMaxCommandsPerBlock = 32;

    void applyCommand(const Command& c) noexcept;   // RT (or message thread while stopped)

    core::SpscRing<Command, kCommandCapacity>      commands_;
    core::SpscRing<AudioProcessor*, kRetireCapacity> retired_;
    core::SpscRing<Telemetry, kTelemetryCapacity>  telemetry_;
    std::atomic<std::uint32_t>                     swapsInFlight_{0};

    // RT-owned state (touched by the message thread only while the stream is stopped)
    AudioProcessor* active_      = nullptr;
    float           gainCurrent_ = 1.0f;
    float           gainTarget_  = 1.0f;
    bool            muted_       = false;
    std::uint64_t   sampleTime_  = 0;

    double        loadSum_         = 0.0;
    float         loadPeak_        = 0.0f;
    std::uint32_t windowBlocks_    = 0;
    std::uint32_t blocksPerReport_ = 1;
    std::uint32_t overruns_        = 0;
    double        ticksToLoad_     = 0.0;   // QPC ticks -> fraction of one block period

    io::StreamConfig config_{};
    bool             prepared_ = false;
};

} // namespace hc::engine
