#pragma once
// =============================================================================
//  HEM CONSOLE RIG — the whole rig as one signal path (hc::console::rig).
//
//    13 stems ─► HEM STUDIO DESK channels (each channel INSERT holds a front-end TOOL slot)
//             ─► desk main bus ─► MAIN INSERT ─► RACK: HEM STEP EQ (standard or SE model), HEM TWIN BUS, HEM COLOUR STAGE
//                                                (any order, each with its own IN switch)
//             ─► desk bus compressor ─► main fader ─► MAIN OUT ─► HEM METER BRIDGE
//             └─► desk monitor section ─► MONITOR OUT (what you hear) / ALT OUT
//  The COLOUR STAGE's own INSERT holds a TOOL slot too. The TWIN BUS's external side-chain key is the EXT 2 return.
//
//  Threads:
//    message thread : prepare() (allocates), reset(), loadText(), setDefaults()
//    any thread     : every control (desk / effects / rack / tools are lock-free atomics), saveText(), meters
//    audio thread   : process() — no allocation, no locks, no waiting
//  Saved state is plain text, one "key=value" per line, with the stable ids of every control,
//  so a session opens the same after updates that add controls (missing keys keep defaults).
// =============================================================================
#include "hybrid/console/FrontTools.hpp"
#include "hybrid/console/MeterBridge.hpp"
#include "hybrid/console/StudioDesk.hpp"
#include "hybrid/fx/Fx.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hc::console {

class ConsoleRig {
public:
    static constexpr int kChannels = static_cast<int>(StudioDesk::kChannels);
    enum Unit : int { UnitStepEq = 0, UnitTwinBus, UnitColour, kUnitCount };
    static constexpr int kOrderCount = 6;   // every order of the three rack units

    struct Io {
        std::array<const float*, StudioDesk::kChannels> stemL{}, stemR{};   // nullptr = silent channel; R nullptr = mono
        const float *ext1L = nullptr, *ext1R = nullptr, *ext2L = nullptr, *ext2R = nullptr;   // EXT 1 / EXT 2 returns
        float *mainL = nullptr, *mainR = nullptr;   // MAIN OUT (what is printed / exported)
        float *monL = nullptr, *monR = nullptr;     // MONITOR OUT
        float *altL = nullptr, *altR = nullptr;     // ALT OUT
        float *cue1L = nullptr, *cue1R = nullptr, *cue2L = nullptr, *cue2R = nullptr;   // ST CUE 1 / 2 mixes
        std::array<float*, StudioDesk::kChannels> directL{}, directR{};   // direct outs (stem printing)
    };

    ConsoleRig();
    ~ConsoleRig();
    ConsoleRig(const ConsoleRig&) = delete;
    ConsoleRig& operator=(const ConsoleRig&) = delete;

    void prepare(double sampleRate, std::uint32_t maxBlock);
    void reset() noexcept;
    void process(const Io& io, std::uint32_t frames) noexcept;

    // ---- the gear ----
    [[nodiscard]] StudioDesk& desk() noexcept;
    [[nodiscard]] const StudioDesk& desk() const noexcept;
    [[nodiscard]] fx::Effect& unit(int u) noexcept;               // Unit enum
    [[nodiscard]] const fx::Effect& unit(int u) const noexcept;
    [[nodiscard]] fx::Effect& stepEq() noexcept { return unit(UnitStepEq); }       // the standard model
    [[nodiscard]] fx::Effect& stepEqSe() noexcept;                                  // the special-edition model
    // Which STEP EQ model sits in the rack's EQ slot: 0 = STANDARD, 1 = SPECIAL EDITION (both keep their settings).
    enum EqModel : int { EqStandard = 0, EqSpecial = 1 };
    void setEqModel(int model) noexcept;
    [[nodiscard]] int eqModel() const noexcept;
    [[nodiscard]] fx::Effect& twinBus() noexcept { return unit(UnitTwinBus); }
    [[nodiscard]] fx::Effect& colour() noexcept { return unit(UnitColour); }
    [[nodiscard]] MeterBridge& meter() noexcept;
    // The meter's RESET from the UI thread: done by the audio thread at the start of the next process().
    void requestMeterReset() noexcept;
    [[nodiscard]] ToolSlot& channelTool(int ch) noexcept;          // behind each channel's INSERT switch
    [[nodiscard]] const ToolSlot& channelTool(int ch) const noexcept;
    [[nodiscard]] ToolSlot& colourTool() noexcept;                 // behind the COLOUR STAGE's INSERT switch
    [[nodiscard]] const ToolSlot& colourTool() const noexcept;

    // ---- rack (desk MAIN INSERT) ----
    void setRackOrder(int order) noexcept;                          // 0..kOrderCount-1
    [[nodiscard]] int rackOrder() const noexcept;
    void setUnitIn(int u, bool on) noexcept;
    [[nodiscard]] bool unitIn(int u) const noexcept;
    [[nodiscard]] static int rackUnitAt(int order, int position) noexcept;   // which unit sits at position 0..2
    [[nodiscard]] static const char* unitName(int u) noexcept;              // "STEP EQ" ...
    [[nodiscard]] static std::string orderName(int order);                  // "EQ > BUS > COLOUR"
    [[nodiscard]] std::uint32_t rackLatency() const noexcept;               // samples the rack adds in the main bus

    // ---- state ----
    void setDefaults() noexcept;
    [[nodiscard]] std::string saveText() const;
    // Applies every recognised "key=value" line; unknown keys are ignored, missing keys keep their value.
    // Returns the number of values applied.
    int loadText(std::string_view text);
    // One control by its saved key ("desk.ch03.hf", "twinbus.thr1", "rack.order" ...). False if unknown.
    bool setByKey(std::string_view key, float plain) noexcept;
    [[nodiscard]] bool getByKey(std::string_view key, float& plain) const noexcept;

private:
    friend struct RackInsert;
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace hc::console
