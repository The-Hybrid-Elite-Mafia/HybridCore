#pragma once
// =============================================================================
//  HEM WAVE — Steven's morphing wavetable synth (idea: a single-oscillator wavetable synth
//  you sweep through; design, tables and code are our own).
//
//  Sound: 8 wavetables built in code (64 frames each, band-limited per octave so high notes
//  don't alias) → POSITION morphs through the frames, WARP darkens (−) or adds an octave
//  sheen (+) → up to 7 unison copies (detune, stereo spread) + sine sub one octave down →
//  zero-delay-feedback state-variable filter (LP / BP / HP, resonance, drive, key tracking)
//  → amp envelope. A second envelope and one LFO move position, cutoff and pitch.
//  16 voices (poly), or MONO / LEGATO with glide.
//
//  Threads: prepare() / applyPreset() on the message thread (prepare may allocate);
//  set() / get() from any thread (one relaxed atomic per parameter); noteOn / noteOff /
//  pitchBend / modWheel / render on the audio thread — no allocation, locks or waiting.
//  Note events carry a sample offset into the next render() call and land on that sample.
// =============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace hc::synth {

enum class Taper : std::uint8_t { Linear, Log, Choice, Toggle };

struct ParamInfo {
    const char*        id;
    const char*        name;
    const char*        unit;    // "dB", "Hz", "ms", "%", "st", "ct", ""
    float              min, max, def;
    Taper              taper = Taper::Linear;
    const char* const* choices = nullptr;   // Choice: max + 1 names (min is 0)
};

[[nodiscard]] float       toPlain(const ParamInfo& p, float normalized) noexcept;
[[nodiscard]] float       toNormalized(const ParamInfo& p, float plain) noexcept;
[[nodiscard]] float       clampPlain(const ParamInfo& p, float plain) noexcept;
[[nodiscard]] std::string formatValue(const ParamInfo& p, float plain);
[[nodiscard]] bool        parseValue(const ParamInfo& p, const std::string& text, float& plain);

// Parameter indices (stable: saved state uses the ids, hosts use these numbers).
enum Param : int {
    Table, Position, Warp, Unison, Detune, Spread, Sub, Octave, Fine,
    FilterType, Cutoff, Resonance, KeyTrack, Drive, EnvCutoff, VelCutoff,
    AmpAttack, AmpDecay, AmpSustain, AmpRelease,
    ModAttack, ModDecay, ModSustain, ModRelease, EnvPosition,
    LfoShape, LfoRate, LfoPosition, LfoCutoff, LfoPitch,
    VelAmp, Mode, Glide, BendRange, Volume,
    kParamCount
};

class Voice;   // internal

class WaveSynth {
public:
    static constexpr int kVoices = 16, kMaxUnison = 7, kMaxEvents = 256;

    WaveSynth();
    ~WaveSynth();
    WaveSynth(const WaveSynth&)            = delete;
    WaveSynth& operator=(const WaveSynth&) = delete;

    [[nodiscard]] static std::span<const ParamInfo> params() noexcept;
    void set(int i, float plain) noexcept;
    [[nodiscard]] float get(int i) const noexcept;
    void setDefaults() noexcept;
    [[nodiscard]] int paramIndex(const std::string& id) const noexcept;   // -1 if unknown

    [[nodiscard]] static int         presetCount() noexcept;
    [[nodiscard]] static const char* presetName(int i) noexcept;
    void applyPreset(int i) noexcept;   // sets every parameter (defaults + the preset's values)

    void prepare(double sampleRate, std::uint32_t maxBlock);   // builds the shared wavetables once per process
    void reset() noexcept;                                      // silence, all voices off, events dropped

    // Audio thread. offset = sample index inside the next render() call (later offsets carry over).
    void noteOn(int note, float velocity, std::uint32_t offset = 0) noexcept;
    void noteOff(int note, std::uint32_t offset = 0) noexcept;
    void allNotesOff() noexcept;                // release everything now
    void pitchBend(float bend) noexcept;        // -1 .. +1 (× BEND RANGE semitones)
    void modWheel(float amount) noexcept;       // 0 .. 1 (adds vibrato)
    void render(float* left, float* right, std::uint32_t frames) noexcept;   // writes (does not add)

    [[nodiscard]] int activeVoices() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept { return fs_; }

    // Shared by the voices (internal).
    struct Block;

private:
    struct Event { std::uint32_t offset; std::int16_t note; float velocity; bool on; };
    void startNote(int note, float velocity) noexcept;
    void stopNote(int note) noexcept;
    Voice& pickVoice(int note) noexcept;
    void computeBlock() noexcept;
    float lfoTick() noexcept;
    std::uint32_t nextRandom() noexcept;

    std::array<std::atomic<float>, kParamCount> vals_{};
    double fs_ = 48000.0;
    bool   prepared_ = false;
    Voice* voices_ = nullptr;           // kVoices, allocated in the constructor
    Block* block_ = nullptr;            // parameter snapshot for one render() call
    std::array<Event, kMaxEvents> events_{};
    int    eventCount_ = 0;
    std::array<std::int16_t, 16> held_{};   // MONO / LEGATO: keys held, last = sounding
    int    heldCount_ = 0;
    std::uint64_t order_ = 0;
    std::atomic<float> bend_{0.0f}, wheel_{0.0f};
    // LFO (one for the whole synth, free-running from reset())
    double lfoPhase_ = 0.0;
    float  lfoHold_ = 0.0f;
    std::uint32_t rng_ = 0x9E3779B9u;
    std::array<float, 256> lfoBuf_{};
    // Master volume smoothing
    float  volCur_ = 0.0f, volCoef_ = 0.0f;
};

} // namespace hc::synth
