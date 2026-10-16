#pragma once
// =============================================================================
//  hc::params — thread-safe parameter tree.
//
//  Values are std::atomic<float> (plain units). Any thread may read; writers are the
//  UI/message thread (knobs, settings load) or a plug-in host thread. A write marks the
//  parameter dirty; the message thread calls dispatchChanges() to run bound listeners
//  (which push values into DSP objects through their own wait-free setters).
//  The audio thread never runs listeners and never touches this tree's containers.
//
//  Structure (add/bind) is built once at start-up on the message thread before any
//  other thread sees the tree.
// =============================================================================
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hc::params {

enum class Kind : std::uint8_t {
    Decibel,   // min..max dB; optional "off" at the bottom (-inf); skewed fader taper
    Pan,       // -1 (L) .. +1 (R), linear
    Toggle,    // 0 / 1
    Linear,    // min..max, linear
    Log,       // min..max on a logarithmic taper (frequency, time, ratio); min > 0
    Choice,    // 0..choices-1 (whole numbers), shown by name
};

struct ParamSpec {
    std::string id;          // stable key: "stem01.trim"
    std::string name;        // display: "Stem 1 Trim"
    Kind        kind = Kind::Linear;
    float       min  = 0.0f;
    float       max  = 1.0f;
    float       def  = 0.0f;
    bool        minIsOff = false;      // Decibel: bottom of travel means -inf / off
    // Decibel taper: this plain value sits at anchorNorm on the control (e.g. 0 dB at 0.75).
    float       anchorPlain = 0.0f;
    float       anchorNorm  = 0.0f;    // 0 = linear taper
    std::string unit;                  // "dB", "Hz", "ms", ":1" — display / host unit ("" = none)
    bool        offLabel = false;      // Log / Linear: the bottom of travel shows "OFF"
    std::vector<std::string> choices;  // Choice: the names (min/max are set from it)
};

class Parameter {
public:
    Parameter(ParamSpec spec, std::uint32_t index);

    [[nodiscard]] const ParamSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] std::uint32_t    index() const noexcept { return index_; }

    [[nodiscard]] float plain() const noexcept { return value_.load(std::memory_order_relaxed); }
    [[nodiscard]] float normalized() const noexcept { return toNormalized(plain()); }
    [[nodiscard]] bool  isOn() const noexcept { return plain() >= 0.5f; }
    // True when a Decibel parameter with minIsOff sits at the bottom (signal fully off).
    [[nodiscard]] bool  isOff() const noexcept;
    // Decibel value to apply to DSP: -144 when off, else plain().
    [[nodiscard]] float effectiveDb() const noexcept;

    void setPlain(float v) noexcept;
    void setNormalized(float n) noexcept { setPlain(toPlain(n)); }
    void resetToDefault() noexcept { setPlain(spec_.def); }

    [[nodiscard]] float toNormalized(float plain) const noexcept;
    [[nodiscard]] float toPlain(float normalized) const noexcept;
    [[nodiscard]] std::string format() const { return formatValue(plain()); }
    [[nodiscard]] std::string formatValue(float plain) const;

    // Internal to ParameterTree.
    bool takeDirty() noexcept { return dirty_.exchange(false, std::memory_order_acq_rel); }

private:
    float clampPlain(float v) const noexcept;

    ParamSpec          spec_;
    std::uint32_t      index_;
    float              taperExp_ = 1.0f;
    std::atomic<float> value_;
    std::atomic<bool>  dirty_{true};   // first dispatch pushes defaults into DSP
    std::atomic<std::uint64_t>* treeVersion_ = nullptr;
    friend class ParameterTree;
};

class ParameterTree {
public:
    using Listener = std::function<void(const Parameter&)>;

    ParameterTree() = default;
    ParameterTree(const ParameterTree&)            = delete;
    ParameterTree& operator=(const ParameterTree&) = delete;

    Parameter& add(ParamSpec spec);
    void       bind(Parameter& p, Listener listener);

    [[nodiscard]] Parameter*       find(std::string_view id) noexcept;
    [[nodiscard]] const Parameter* find(std::string_view id) const noexcept;
    [[nodiscard]] Parameter&       at(std::size_t i) noexcept { return *params_[i]; }
    [[nodiscard]] const Parameter& at(std::size_t i) const noexcept { return *params_[i]; }
    [[nodiscard]] std::size_t      size() const noexcept { return params_.size(); }

    // Bumped on every value change; cheap "anything changed?" check for redraws.
    [[nodiscard]] std::uint64_t version() const noexcept { return version_.load(std::memory_order_acquire); }

    // Message thread: run listeners for every parameter changed since the last call.
    std::size_t dispatchChanges();

    // "id=value" lines. Unknown ids and malformed lines are skipped on load.
    [[nodiscard]] std::string serialize() const;
    std::size_t               deserialize(std::string_view text);

private:
    std::vector<std::unique_ptr<Parameter>> params_;
    std::vector<std::vector<Listener>>      listeners_;
    std::atomic<std::uint64_t>              version_{0};
};

} // namespace hc::params
