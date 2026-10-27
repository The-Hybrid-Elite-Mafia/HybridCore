#pragma once
// =============================================================================
//  HEM FRONT-END TOOLS — the slots where the reference desk has its microphone preamps (hc::console::tools).
//  Our own designs for the engineer's in-computer front end (see ConsoleSpecs.hpp, namespace tools):
//
//    HEM RESO TAMER  two dynamic notches for two annoying frequencies. Per band: ON, FREQ, WIDTH, THRESHOLD,
//                    DEPTH, SPEED, LISTEN. A band only cuts while its narrow band pokes out above the music
//                    around it, by up to DEPTH. Meters: gain reduction of each band.
//    HEM SHUFFLE     mud and width in mid/side: SHUFFLE (side low shelf at SHUFFLE FREQ), MUD (mid bell cut at
//                    MUD FREQ), WIDTH (side level), MONO BASS (side high-pass at 120 Hz), OUTPUT. Meter: correlation.
//    HEM TAP ECHO    delay with tap tempo: TAP (sets TEMPO), NOTE (FREE or a note value of the tempo), TIME (FREE),
//                    FEEDBACK, MIX, LOW CUT, HIGH CUT (in the echo path), MODE (STEREO / PING-PONG / MONO).
//
//  Each tool is a hc::fx::Effect (same threading contract). A ToolSlot is the InsertSlot behind a desk channel's
//  INSERT switch (or the COLOUR STAGE's): it holds one of each tool and runs the one TYPE selects.
//  Controls start at zero (or centre) unless the control needs a set value (frequencies, WIDTH 100 %).
// =============================================================================
#include "hybrid/console/InsertSlot.hpp"
#include "hybrid/fx/Fx.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace hc::console {

[[nodiscard]] std::unique_ptr<fx::Effect> makeResoTamer();
[[nodiscard]] std::unique_ptr<fx::Effect> makeShuffle();
[[nodiscard]] std::unique_ptr<fx::Effect> makeTapEcho();

namespace reso_ids {
enum : int { On = 0, Freq, Width, Threshold, Depth, Speed, Listen, kPerBand };   // per band
[[nodiscard]] constexpr int id(int band, int p) noexcept { return band * kPerBand + p; }
inline constexpr int kCount = 2 * kPerBand;
enum : int { MeterGr1 = 0, MeterGr2 };
} // namespace reso_ids

namespace shuffle_ids {
enum : int { ShuffleFreq = 0, Shuffle, MudFreq, Mud, Width, MonoBass, Output, kCount };
enum : int { MeterCorrelation = 0 };
} // namespace shuffle_ids

namespace echo_ids {
enum : int { Tempo = 0, Note, Time, Feedback, Mix, LowCut, HighCut, Mode, kCount };
enum : int { NoteFree = 0 };
enum : int { MeterDelayMs = 0 };
} // namespace echo_ids

// Delay in milliseconds for a NOTE choice at a tempo (NoteFree returns `freeMs`).
[[nodiscard]] double echoNoteMs(int note, double bpm, double freeMs) noexcept;

// TAP button logic (UI thread): call tap() with the time of each press in seconds.
// Returns true when a tempo is known (two or more taps without a long pause).
class TapTempo {
public:
    bool tap(double seconds) noexcept;
    [[nodiscard]] double bpm() const noexcept { return bpm_; }
    void clear() noexcept { count_ = 0; last_ = -1e9; }

private:
    std::array<double, 8> intervals_{};
    int count_ = 0;
    double last_ = -1e9, bpm_ = 120.0;
};

class ToolSlot final : public InsertSlot {
public:
    enum Type : int { Off = 0, Reso, Shuffle, Echo, kTypeCount };
    ToolSlot();
    ~ToolSlot() override;
    void prepare(double sampleRate, std::uint32_t maxBlock);   // message thread
    void reset() noexcept;
    void setType(int type) noexcept;                           // any thread
    [[nodiscard]] int type() const noexcept { return type_.load(std::memory_order_relaxed); }
    [[nodiscard]] fx::Effect& tool(int type) noexcept;         // Reso / Shuffle / Echo
    [[nodiscard]] const fx::Effect& tool(int type) const noexcept;
    [[nodiscard]] static const char* typeName(int type) noexcept;      // "OFF", "RESO TAMER", "SHUFFLE", "TAP ECHO"
    [[nodiscard]] static const char* typeKey(int type) noexcept;       // "off", "reso", "shuffle", "echo" (saved state)
    void process(float* left, float* right, std::uint32_t frames) noexcept override;

private:
    std::unique_ptr<fx::Effect> reso_, shuffle_, echo_;
    std::atomic<int> type_{Off};
    int running_ = Off;   // audio thread: the tool that ran last block (a change resets the new one)
};

} // namespace hc::console
