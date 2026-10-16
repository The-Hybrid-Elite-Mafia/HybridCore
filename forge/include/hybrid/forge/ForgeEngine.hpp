#pragma once
// =============================================================================
//  ForgeEngine — plays a Beat Forge song in time with the console's transport.
//
//  Output: 8 separate stereo tracks (KICK .. PAD) that MixerProcessor feeds into stem
//  strips 1–8, plus a stereo return of Beat Forge's own reverb and ping-pong delay that is
//  added to the Main bus. Reverb / delay sends are post-fader: they follow the strip's
//  fader, mute and solo, and the return follows the Main bus fader and mute.
//
//  THREAD CONTRACT
//    Message thread : bind(), prepare(), setSong() (triple-buffer publish), song().
//    UI thread      : audition() (single producer, lock-free ring).
//    Audio thread   : beginBlock() / render() / endBlock() only — no allocation, no locks.
//    Any thread     : telemetry atomics.
// =============================================================================
#include "hybrid/core/SpscRing.hpp"
#include "hybrid/forge/Pattern.hpp"
#include "hybrid/forge/Synth.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace hc::params { class ParameterTree; class Parameter; }

namespace hc::forge {

class ForgeEngine {
public:
    static constexpr int kMaxEvents = 512;

    ForgeEngine();
    ~ForgeEngine();
    ForgeEngine(const ForgeEngine&)            = delete;
    ForgeEngine& operator=(const ForgeEngine&) = delete;

    // ---- message thread ----
    // Takes every forge.* parameter (and the strip / Main bus controls it follows) from `tree`,
    // which must outlive the engine. Without bind() the engine stays silent.
    void bind(const params::ParameterTree& tree);
    void prepare(double sampleRate, std::uint32_t maxBlock);
    void setSong(const Song& song);
    [[nodiscard]] const Song& song() const noexcept { return *edit_; }

    // ---- UI thread: plays one note now (pads, note clicks), also while stopped ----
    void audition(int track, int pitch, int velocity) noexcept;

    // ---- audio thread ----
    [[nodiscard]] bool enabled() const noexcept;    // forge.on (and bound)
    // Called when Beat Forge comes (back) on: silences voices left over from earlier and the effect tails.
    void prepareVoicesForStart() noexcept;
    // Song length in samples at the current tempo: the arrangement when "full song" is on,
    // otherwise one pattern (it loops). 0 when there is nothing to play.
    [[nodiscard]] std::uint64_t songFrames() const noexcept;
    void beginBlock(std::uint32_t frames) noexcept;                                      // clears outputs, takes the newest song
    void render(std::uint32_t at, std::uint32_t n, std::uint64_t songPos, bool playing) noexcept;   // [at, at+n) of this block
    void endBlock(std::uint32_t frames) noexcept;                                        // voice gains, sends, effects
    [[nodiscard]] const float* track(int t, int ch) const noexcept {
        return trackBuf_.data() + (static_cast<std::size_t>(t) * 2 + static_cast<std::size_t>(ch)) * maxBlock_;
    }
    [[nodiscard]] const float* fxReturn(int ch) const noexcept { return fxOut_.data() + static_cast<std::size_t>(ch) * maxBlock_; }

    [[nodiscard]] static double stepSamples(double sampleRate, double bpm) noexcept;
    // Length of a song in samples: the arrangement when `fullSong` (and there is one), else one pattern.
    [[nodiscard]] static std::uint64_t songLength(const Song& song, double sampleRate, double bpm, bool fullSong) noexcept;

    // ---- telemetry (audio -> UI) ----
    std::array<std::atomic<std::uint32_t>, kTracks> trackHit{};   // +1 on every note start
    std::atomic<float> playStep{-1.0f};                           // position in the pattern (16ths), -1 = stopped
    std::atomic<int>   songBar{-1};                                // bar of the song, -1 = stopped

private:
    struct Event { std::uint32_t offset; int track; int gate; std::uint8_t pitch, velocity; };
    struct Voices;
    struct Fx;

    void startNote(const Event&) noexcept;
    void renderVoices(std::uint32_t from, std::uint32_t to) noexcept;
    void releaseHeld() noexcept;
    void chokeAll() noexcept;
    [[nodiscard]] float p(const params::Parameter* q, float fallback) const noexcept;

    std::unique_ptr<Voices> voices_;
    std::unique_ptr<Fx>     fx_;
    TripleBuffer<Song>      handoff_;
    std::unique_ptr<Song>   edit_;            // message-thread copy
    const Song*             playing_ = nullptr;

    // parameters (null until bind)
    struct TrackParams {
        const params::Parameter* type = nullptr;
        std::array<const params::Parameter*, kKnobs> k{};
        const params::Parameter* rev = nullptr;
        const params::Parameter* dly = nullptr;
        const params::Parameter* stripMain = nullptr;
        const params::Parameter* stripMute = nullptr;
        const params::Parameter* stripSolo = nullptr;
    };
    std::array<TrackParams, kTracks> tp_{};
    std::array<const params::Parameter*, 13> anySolo_{};   // every stem strip's solo (a solo anywhere mutes the rest)
    const params::Parameter *on_ = nullptr, *bpm_ = nullptr, *swing_ = nullptr, *arrange_ = nullptr;
    const params::Parameter *revSize_ = nullptr, *revTone_ = nullptr, *dlyTime_ = nullptr, *dlyFb_ = nullptr, *fxLevel_ = nullptr;
    const params::Parameter *mainGain_ = nullptr, *mainMute_ = nullptr;

    // per-block snapshot
    std::array<std::array<float, kKnobs>, kTracks> knob_{};
    std::array<int, kTracks> type_{};

    // audio state
    double        sr_       = 48000.0;
    std::uint32_t maxBlock_ = 0;
    double        stepLen_  = 6000.0;
    bool          wasPlaying_ = false;
    std::uint64_t expectPos_  = 0;        // where the last render ended (seek / loop detection)
    std::uint64_t ageCounter_ = 0;
    synth::Noise  jitter_;
    std::array<Event, kMaxEvents> events_{};
    std::vector<float> trackBuf_;         // 8 tracks x 2 x maxBlock
    std::vector<float> revIn_, dlyIn_, fxOut_;   // 2 x maxBlock each
    std::array<float, kTracks> sendRev_{}, sendDly_{};   // last block's send gains (ramped)
    float fxGain_ = 0.0f;
    core::SpscRing<Event, 64> audition_;
};

} // namespace hc::forge
