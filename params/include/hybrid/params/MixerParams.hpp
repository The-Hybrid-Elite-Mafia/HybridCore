#pragma once
// =============================================================================
//  The 13-stem mixer's parameter set and its binding to dsp::StemMatrix.
//
//  Per stem s (1-based ids "stem01".."stem13"):
//    trim  (-24..+24 dB)   pan (-1..+1)   main / aux1 / aux2 / aux3 sends (-60..+6 dB, bottom = off)
//    mute  solo  invert    (toggles)
//  Per bus ("main", "aux1".."aux3"):  gain (-60..+6 dB, bottom = off)   mute
// =============================================================================
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/dsp/StemMatrix.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hc::params {

[[nodiscard]] std::string stemParamId(std::uint32_t stem, const char* field);   // stem is 0-based
[[nodiscard]] std::string busParamId(std::uint32_t bus, const char* field);     // bus 0 = main
[[nodiscard]] const char* busName(std::uint32_t bus);                            // "Main", "Aux 1"...

void addMixerParameters(ParameterTree& tree);

// Parameter-index -> matrix-target table, resolved once from the ids (message thread).
// apply()/applyAll() are allocation-free and lock-free: safe on an audio thread
// (used by the VST3 processor).
class MixerBindings {
public:
    explicit MixerBindings(const ParameterTree& tree);
    void apply(const Parameter& p, dsp::StemMatrix& matrix) const noexcept;
    void applyAll(const ParameterTree& tree, dsp::StemMatrix& matrix) const noexcept;

    enum class Target : std::uint8_t { None, Trim, Pan, Send, Mute, Solo, Invert, BusGain, BusMute };
    struct Entry { Target target = Target::None; std::uint8_t index = 0; std::uint8_t bus = 0; };
    [[nodiscard]] const Entry& entry(std::size_t paramIndex) const noexcept { return table_[paramIndex]; }

private:
    std::vector<Entry> table_;
};

// Binds every mixer parameter to the matrix. Listeners run on dispatchChanges().
// `bindings` must outlive the tree's listeners.
void bindMixerParameters(ParameterTree& tree, const MixerBindings& bindings, dsp::StemMatrix& matrix);

} // namespace hc::params
