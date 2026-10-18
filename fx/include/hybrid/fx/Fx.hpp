#pragma once
// =============================================================================
//  hc::fx — the HEM effect plug-ins (our own DSP, our own names and look).
//
//  Every effect is a stereo, in-place processor with a flat list of parameters:
//    message thread : prepare(sampleRate, maxBlock) — may allocate; reset()
//    any thread     : set(i, plain) / get(i) — lock-free (one relaxed atomic per parameter)
//    audio thread   : process(L, R, n) with n <= min(maxBlock, kMaxBlock) — no allocation, no locks, no waiting
//  The VST3 wrapper (plugin/src/HemSuiteVst3.cpp) and the tests use only this interface,
//  so the same code later runs inside The Hybrid Core's own strips.
// =============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace hc::fx {

enum class Taper : std::uint8_t {
    Linear,   // min..max straight
    Log,      // min..max on a log scale (frequencies, times); min must be > 0
    Choice,   // whole numbers 0..max, shown by name
    Toggle    // 0 = OFF, 1 = ON
};

// A live readout an effect publishes for its faceplate (pitch, correlation, band moves ...).
struct MeterInfo {
    const char* name;          // "PITCH"
    const char* unit;          // "Hz", "dB", "cents", "note", "corr", "%", "ms"
    float       min, max;      // bar range
    bool        centred = false;   // bar grows from the middle (signed values)
};

struct ParamInfo {
    const char*        id;        // stable, used in saved state ("drive")
    const char*        name;      // shown in hosts ("Drive")
    const char*        unit;      // "dB", "Hz", "ms", "%", ":1", "" ...
    float              min, max, def;
    Taper              taper = Taper::Linear;
    const char* const* choices = nullptr;   // Choice: max + 1 names
};

[[nodiscard]] float       toPlain(const ParamInfo& p, float normalized) noexcept;
[[nodiscard]] float       toNormalized(const ParamInfo& p, float plain) noexcept;
[[nodiscard]] float       clampPlain(const ParamInfo& p, float plain) noexcept;   // also rounds Choice / Toggle
[[nodiscard]] std::string formatValue(const ParamInfo& p, float plain);
[[nodiscard]] bool        parseValue(const ParamInfo& p, const std::string& text, float& plain);

class Effect {
public:
    static constexpr int kMaxParams = 32;
    static constexpr std::uint32_t kMaxBlock = 4096;   // process() is never called with more (hosts' blocks are split)

    virtual ~Effect() = default;
    Effect(const Effect&)            = delete;
    Effect& operator=(const Effect&) = delete;

    [[nodiscard]] virtual std::span<const ParamInfo> params() const noexcept = 0;

    void set(int i, float plain) noexcept {
        const auto ps = params();
        if (i >= 0 && i < static_cast<int>(ps.size())) vals_[static_cast<std::size_t>(i)].store(clampPlain(ps[static_cast<std::size_t>(i)], plain), std::memory_order_relaxed);
    }
    [[nodiscard]] float get(int i) const noexcept {
        return i >= 0 && i < kMaxParams ? vals_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) : 0.0f;
    }
    void setDefaults() noexcept {
        const auto ps = params();
        for (std::size_t i = 0; i < ps.size(); ++i) vals_[i].store(ps[i].def, std::memory_order_relaxed);
    }

    virtual void prepare(double sampleRate, std::uint32_t maxBlock) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(float* left, float* right, std::uint32_t frames) noexcept = 0;
    [[nodiscard]] virtual std::uint32_t latency() const noexcept { return 0; }
    // What the effect is doing right now, for meters: gain reduction in dB (<= 0) where it applies.
    [[nodiscard]] float reductionDb() const noexcept { return grDb_.load(std::memory_order_relaxed); }

    // Extra readouts (optional): what each one is, and its latest value (any thread, lock-free).
    static constexpr int kMaxMeters = 10;
    [[nodiscard]] virtual std::span<const MeterInfo> meters() const noexcept { return {}; }
    [[nodiscard]] float meter(int i) const noexcept {
        return i >= 0 && i < kMaxMeters ? meters_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) : 0.0f;
    }

    // Song tempo for tempo-synced effects (the host's or Beat Forge's). Any thread.
    void setTempo(double bpm) noexcept { tempo_.store(static_cast<float>(std::clamp(bpm, 20.0, 400.0)), std::memory_order_relaxed); }
    [[nodiscard]] float tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }

protected:
    Effect() = default;
    [[nodiscard]] float p(int i) const noexcept { return vals_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed); }
    void publishReduction(float db) noexcept { grDb_.store(db, std::memory_order_relaxed); }
    void publishMeter(int i, float v) noexcept {
        if (i >= 0 && i < kMaxMeters) meters_[static_cast<std::size_t>(i)].store(v, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, kMaxMeters> meters_{};
    std::atomic<float> tempo_{120.0f};
    std::array<std::atomic<float>, kMaxParams> vals_{};
    std::atomic<float> grDb_{0.0f};
};

struct EffectInfo {
    const char* name;          // "HEM CLIP"
    const char* subCategory;   // VST3 sub-category: "Fx|Dynamics", "Fx|EQ" ...
    const char* blurb;         // one line, plain words
    std::array<std::uint32_t, 4> processorUid, controllerUid;
    std::unique_ptr<Effect> (*create)();
};

// Every HEM effect, in menu order.
[[nodiscard]] std::span<const EffectInfo> effects() noexcept;

// Helpers shared by the effects.
[[nodiscard]] inline float dbToGain(float db) noexcept;
[[nodiscard]] inline float gainToDb(float g) noexcept;

} // namespace hc::fx

#include <cmath>
namespace hc::fx {
inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) noexcept { return 20.0f * std::log10(std::max(g, 1e-9f)); }
} // namespace hc::fx
