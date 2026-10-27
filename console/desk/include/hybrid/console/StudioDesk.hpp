#pragma once
// =============================================================================
//  HEM STUDIO DESK — 13-channel summing desk engine (hc::console::desk).
//
//  Every channel carries the full strip, in stereo (owner decision: the stems are stereo):
//    TRIM · MONO · Ø · HPF · 3-band EQ (HF/MF/LF, two BELL switches, EQ IN) · one-knob COMP (+IN)
//    · INSERT · two cue sends (ON, level, pan, POST) · PAN/BALANCE · direct out (PRE/POST FADER)
//    · MUTE/BUS B · PFL · fader · meters (8-LED peak L/R, 3-LED comp gain reduction)
//  Master section: EXT 1/2 returns (level, to MAIN, to each cue), MAIN INSERT, CUE 1 TO BUS COMP & SUM,
//    bus compressor (THRESHOLD, AUTO RELEASE, MAKE UP, IN, 5 LEDs), main fader, BUS B master
//    (level, mute), cue masters, monitor (sources MAIN/BUS B/EXT 1/EXT 2 that SUM, PFL, DIM level,
//    DIM, CUT, MONO, ALT, level 0..11), 12-LED main meter.
//  Signal order follows the reference block diagrams (see console/README.md, "Studio Desk").
//
//  Threads:
//    message thread : prepare() (allocates), reset(), setChannelInsert()/setMainInsert()
//    any thread     : setChannel()/setMaster() and every getter (one relaxed atomic each)
//    audio thread   : process() — no allocation, no locks, no waiting
// =============================================================================
#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"
#include "hybrid/core/Config.hpp"
#include "hybrid/fx/Fx.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hc::console {

class StudioDesk {
public:
    static constexpr std::uint32_t kChannels = hc::kStemCount;   // ONE place: hybrid/core/Config.hpp

    enum ChParam : int {
        Trim, Mono, Polarity, Hpf,
        EqIn, HfGain, HfBell, MfGain, LfGain, LfBell,
        CompIn, CompThreshold,
        Insert,
        Cue1On, Cue1Level, Cue1Pan, Cue1Post,
        Cue2On, Cue2Level, Cue2Pan, Cue2Post,
        Pan, DirectPostFader, MuteBusB, Pfl, Fader,
        kChParamCount
    };
    enum MasterParam : int {
        Ext1Level, Ext2Level, Ext1ToMain, Ext2ToMain, MainInsert, Cue1ToBusComp,
        BusCompIn, BusCompThreshold, BusCompAutoRelease, BusCompMakeup, MainFader,
        BusBLevel, BusBMute,
        Cue1Master, Cue1Ext1, Cue1Ext2, Cue2Master, Cue2Ext1, Cue2Ext2,
        MonMain, MonBusB, MonExt1, MonExt2, DimLevel, Dim, Cut, MonoMonitor, Alt, MonitorLevel,
        kMasterParamCount
    };

    // Ranges, defaults and names of every control (ids are stable for saved sessions).
    [[nodiscard]] static std::span<const fx::ParamInfo> channelParams() noexcept;
    [[nodiscard]] static std::span<const fx::ParamInfo> masterParams() noexcept;

    // A plug-in slot behind an INSERT switch (Phase 4 tools). Stereo, in place, real-time safe.
    class InsertSlot {
    public:
        virtual ~InsertSlot() = default;
        virtual void process(float* left, float* right, std::uint32_t frames) noexcept = 0;
    };

    struct Inputs {
        // Stem audio per channel. right == nullptr: mono source (left feeds both). left == nullptr: silence.
        std::array<const float*, kChannels> left{}, right{};
        const float *ext1L = nullptr, *ext1R = nullptr, *ext2L = nullptr, *ext2R = nullptr;   // EXT returns
    };
    struct Outputs {   // any nullptr = not wanted
        float *mainL = nullptr, *mainR = nullptr;
        float *busBL = nullptr, *busBR = nullptr;
        float *cue1L = nullptr, *cue1R = nullptr, *cue2L = nullptr, *cue2R = nullptr;
        float *monL = nullptr, *monR = nullptr;    // MAIN monitor output
        float *altL = nullptr, *altR = nullptr;    // ALT monitor output (gets the monitor feed while ALT is on)
        std::array<float*, kChannels> directL{}, directR{};   // direct outs (stem printing), pre or post fader
    };

    StudioDesk();
    ~StudioDesk();
    StudioDesk(const StudioDesk&) = delete;
    StudioDesk& operator=(const StudioDesk&) = delete;

    void prepare(double sampleRate, std::uint32_t maxBlock);
    void reset() noexcept;
    void process(const Inputs& in, const Outputs& out, std::uint32_t frames) noexcept;

    void setChannel(int ch, int param, float plain) noexcept;
    [[nodiscard]] float channel(int ch, int param) const noexcept;
    void setMaster(int param, float plain) noexcept;
    [[nodiscard]] float master(int param) const noexcept;
    void setDefaults() noexcept;

    void setChannelInsert(int ch, InsertSlot* slot) noexcept;   // nullptr = empty slot (INSERT passes audio)
    void setMainInsert(InsertSlot* slot) noexcept;

    // Meters (any thread).
    [[nodiscard]] float channelPeakDbu(int ch, int side) const noexcept;    // post-processing, pre-fader
    [[nodiscard]] float channelCompGrDb(int ch) const noexcept;             // >= 0
    [[nodiscard]] float mainMeterDbu(int side) const noexcept;              // follows the monitor source
    [[nodiscard]] float busCompGrDb() const noexcept;                        // >= 0
    [[nodiscard]] bool  pflActive() const noexcept;
    [[nodiscard]] static int channelMeterLeds(float dbu) noexcept;   // 0..8
    [[nodiscard]] static int mainMeterLeds(float dbu) noexcept;      // 0..12
    [[nodiscard]] static int channelCompLeds(float grDb) noexcept;   // 0..3
    [[nodiscard]] static int busCompLeds(float grDb) noexcept;       // 0..5

    // Pan laws (exposed for tests): gains for a pan/balance position p in [-1, 1].
    static void panGains(float p, float& gl, float& gr) noexcept;       // mono source, -4.5 dB centre
    static void balanceGains(float p, float& gl, float& gr) noexcept;   // stereo source, unity centre
    // Monitor knob 0..11 -> gain.
    [[nodiscard]] static float monitorKnobGain(float v) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace hc::console
