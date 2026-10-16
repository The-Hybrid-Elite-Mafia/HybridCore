#pragma once
// =============================================================================
//  ChannelStrip — per-stem EQ + compressor (runs inside StemMatrix, after trim).
//
//  EQ (RBJ biquads, double-precision state, in this order):
//    HPF        12 dB/oct, 20..1000 Hz, bottom of travel (20 Hz) = off
//    LOW        shelf at 100 Hz,  ±15 dB
//    MID        peak, 150 Hz..8 kHz, Q 0.9, ±15 dB
//    HIGH       shelf at 10 kHz,  ±15 dB
//  Compressor (feed-forward, stereo-linked peak detector, 6 dB soft knee):
//    threshold -48..0 dB, ratio 1..20 (1 = off), attack 0.1..100 ms,
//    release 10..2000 ms, makeup 0..24 dB
//
//  Threading: setters / gainReductionDb() from any thread (relaxed atomics).
//  prepare() on the message thread; process() on the audio thread only — no allocation,
//  no locks. Every setting glides to its new value (20 ms time constant, updated per
//  block) and filter coefficients are recomputed only while something moves.
//  A band at 0 dB (HPF at 20 Hz, ratio 1:1 with no makeup) is skipped entirely.
// =============================================================================
#include <atomic>
#include <cstdint>

namespace hc::dsp {

class ChannelStrip {
public:
    static constexpr float kHpfOffHz  = 20.0f;
    static constexpr float kLowHz     = 100.0f;
    static constexpr float kHighHz    = 10000.0f;
    static constexpr float kMidQ      = 0.9f;
    static constexpr float kKneeDb    = 6.0f;
    static constexpr float kGlideMs   = 20.0f;

    ChannelStrip() noexcept { reset(); }
    ChannelStrip(const ChannelStrip&)            = delete;
    ChannelStrip& operator=(const ChannelStrip&) = delete;

    void prepare(double sampleRate) noexcept;   // message thread; also resets the state
    void reset() noexcept;                      // clears filter memory and the gain envelope (not the settings)

    // ---- any thread ------------------------------------------------------------
    void setHpfHz(float hz) noexcept        { t_.hpf.store(hz, kRelaxed); }
    void setLowDb(float db) noexcept        { t_.low.store(db, kRelaxed); }
    void setMidDb(float db) noexcept        { t_.mid.store(db, kRelaxed); }
    void setMidHz(float hz) noexcept        { t_.midHz.store(hz, kRelaxed); }
    void setHighDb(float db) noexcept       { t_.high.store(db, kRelaxed); }
    void setThresholdDb(float db) noexcept  { t_.thr.store(db, kRelaxed); }
    void setRatio(float r) noexcept         { t_.ratio.store(r, kRelaxed); }
    void setAttackMs(float ms) noexcept     { t_.att.store(ms, kRelaxed); }
    void setReleaseMs(float ms) noexcept    { t_.rel.store(ms, kRelaxed); }
    void setMakeupDb(float db) noexcept     { t_.makeup.store(db, kRelaxed); }
    // Deepest gain reduction of the last block, in dB (>= 0).
    [[nodiscard]] float gainReductionDb() const noexcept { return grPub_.load(kRelaxed); }

    // ---- audio thread ----------------------------------------------------------
    // True when the strip changes the signal (or is gliding / releasing back to neutral).
    [[nodiscard]] bool engaged() const noexcept;
    void process(float* left, float* right, std::uint32_t frames) noexcept;

    // Pure helpers (exposed for tests): static gain curve in dB (<= 0) for an input level.
    [[nodiscard]] static float gainComputerDb(float inDb, float thresholdDb, float ratio) noexcept;

private:
    static constexpr auto kRelaxed = std::memory_order_relaxed;
    struct Targets {
        std::atomic<float> hpf{kHpfOffHz}, low{0.0f}, mid{0.0f}, midHz{1000.0f}, high{0.0f};
        std::atomic<float> thr{-18.0f}, ratio{1.0f}, att{10.0f}, rel{150.0f}, makeup{0.0f};
    };
    struct Values { float hpf, low, mid, midHz, high, thr, ratio, att, rel, makeup; };
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2]{}, z2[2]{};
        void clear() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    };

    [[nodiscard]] Values targets() const noexcept;
    void glide(const Values& to, std::uint32_t frames) noexcept;
    void updateCoefficients() noexcept;
    static void run(Biquad& q, float* x, std::uint32_t n, int ch) noexcept;

    Targets             t_;
    std::atomic<float>  grPub_{0.0f};

    // Audio-thread state.
    double  fs_ = 48000.0;
    Values  cur_{};
    Values  coefFor_{};     // values the coefficients were computed for
    bool    coefValid_ = false;
    Biquad  hpf_, low_, mid_, high_;
    float   envDb_ = 0.0f;  // smoothed gain change (<= 0)
};

} // namespace hc::dsp
