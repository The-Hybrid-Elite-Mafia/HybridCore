#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

/**
    Beat Forge sound generators (ported from Hybrid Studio 2.0.9 BeatForgeSynth.h, JUCE-free). Every instrument is synthesized: no samples, nothing to license.

    Rules kept by every voice (audio thread):
      - no allocation (all state, including the guitar string delay lines, lives inside the voice);
      - no hard jumps: a re-triggered voice restarts its envelope from the level it is at, and
        oscillator phases run on, so a retrigger or a stolen voice never clicks;
      - render() ADDS into the track's stereo buffer.
*/
namespace hc::forge::synth
{
inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;

inline float semis (float st) noexcept { return std::exp2 (st * (1.0f / 12.0f)); }
inline float midiHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f)); }
inline float msToCoeff (float ms, double sr) noexcept   // per-sample multiplier that falls 60 dB in `ms`
{
    return (float) std::exp (-6.907755 / std::max (1.0, (double) ms * 0.001 * sr));
}
inline float softClip (float x, float drive01) noexcept
{
    if (drive01 <= 0.0f) return x;
    const float g = 1.0f + 6.0f * drive01;
    return std::tanh (x * g) / std::tanh (g);
}

struct Noise
{
    uint32_t s = 0x9E3779B9u;
    void seed (uint32_t v) noexcept { s = v != 0 ? v : 0x9E3779B9u; }
    float next() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) (int32_t) s * (1.0f / 2147483648.0f);
    }
};

inline float polyBlep (double t, double dt) noexcept
{
    if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
    return 0.0f;
}

/** Band-limited oscillator (PolyBLEP saw / square, plain sine). Phase in [0, 1). */
struct Osc
{
    double ph = 0.0;
    float saw (double dt) noexcept
    {
        const float v = (float) (2.0 * ph - 1.0) - polyBlep (ph, dt);
        advance (dt);
        return v;
    }
    float square (double dt) noexcept
    {
        float v = ph < 0.5 ? 1.0f : -1.0f;
        v += polyBlep (ph, dt);
        double t2 = ph + 0.5; if (t2 >= 1.0) t2 -= 1.0;
        v -= polyBlep (t2, dt);
        advance (dt);
        return v;
    }
    float sine (double dt) noexcept
    {
        const float v = (float) std::sin (kTwoPi * ph);
        advance (dt);
        return v;
    }
    void advance (double dt) noexcept { ph += dt; ph -= std::floor (ph); }
};

/** Topology-preserving state variable filter (Zavalishin). */
struct Svf
{
    float a1 = 0, a2 = 0, a3 = 0, k = 1.4f, ic1 = 0, ic2 = 0;
    float lp = 0, bp = 0, hp = 0;

    void set (float fc, float q, double sr) noexcept
    {
        fc = std::clamp (fc, 10.0f, (float) (0.45 * sr));
        const float g = (float) std::tan (kPi * fc / sr);
        k = 1.0f / std::max (0.3f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = lp = bp = hp = 0.0f; }
    void process (float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
};

/** Exponential ADSR. gateOn() starts the attack from the current level (click-free retrigger). */
struct Adsr
{
    enum Stage { idle, attack, decay, sustain, release };
    Stage stage = idle;
    float v = 0.0f, aInc = 1.0f, dCoef = 0.999f, sus = 1.0f, rCoef = 0.999f;

    void set (double sr, float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept
    {
        aInc = 1.0f / (float) std::max (1.0, (double) attackMs * 0.001 * sr);
        dCoef = msToCoeff (decayMs, sr);
        sus = std::clamp (sustainLevel, 0.0f, 1.0f);
        rCoef = msToCoeff (releaseMs, sr);
    }
    void gateOn() noexcept { stage = attack; }
    void gateOff() noexcept { if (stage != idle) stage = release; }
    bool active() const noexcept { return stage != idle; }
    float next() noexcept
    {
        switch (stage)
        {
            case attack:  v += aInc; if (v >= 1.0f) { v = 1.0f; stage = decay; } break;
            case decay:   v = sus + (v - sus) * dCoef; if (v - sus < 1.0e-4f) { v = sus; stage = sus > 0.0f ? sustain : idle; } break;
            case sustain: v = sus; break;
            case release: v *= rCoef; if (v < 1.0e-5f) { v = 0.0f; stage = idle; } break;
            case idle:    v = 0.0f; break;
        }
        return v;
    }
};

/** Shared voice bookkeeping: gate countdown, quick "choke" fade, age for stealing. */
struct VoiceBase
{
    bool on = false;
    int gate = 0;                  // samples until note-off (melodic) or 0 = already released
    float choke = 1.0f, chokeStep = 0.0f;
    uint64_t age = 0;
    float pan = 0.0f;              // -1..1 per-voice spread (track pan is applied later)
    float vel = 1.0f, velTarget = 1.0f, velCoef = 0.005f;   // velocity glides (4 ms) when a sounding voice is re-used

    void setVelocity (float v, bool fresh, double sr) noexcept
    {
        velTarget = v;
        velCoef = (float) (1.0 - std::exp (-1.0 / (0.004 * sr)));
        if (fresh) vel = v;
    }
    void stepVelocity() noexcept { vel += (velTarget - vel) * velCoef; }

    void startChoke (double sr) noexcept { if (on && chokeStep <= 0.0f) chokeStep = (float) (1.0 / (0.004 * sr)); }
    bool stepChoke() noexcept   // false when the choke has finished
    {
        if (chokeStep <= 0.0f) return true;
        choke -= chokeStep;
        if (choke > 0.0f) return true;
        on = false; choke = 1.0f; chokeStep = 0.0f;
        return false;
    }
    void resetChoke() noexcept { choke = 1.0f; chokeStep = 0.0f; }
    static void spread (float x, float pan, float& l, float& r) noexcept
    {
        const float a = (pan + 1.0f) * 0.25f * (float) kPi;
        l += x * std::cos (a) * 1.41421356f;
        r += x * std::sin (a) * 1.41421356f;
    }
};

//==============================================================================
// DRUMS
//==============================================================================
struct KickVoice : VoiceBase
{
    Osc osc; Noise nz; Svf clickHp;
    double sr = 48000.0;
    float f0 = 50.0f, depth = 3.0f, pCoef = 0.999f, amp = 0.0f, aCoef = 0.999f, attackInc = 0.05f;
    float clickAmt = 0.3f, clickEnv = 0.0f, clickCoef = 0.99f, drive = 0.2f, pitchEnv = 0.0f;
    bool attacking = false;

    void prepare (double rate) noexcept { sr = rate; clickHp.set (1800.0f, 0.7f, sr); }
    /** k: tune st, decay ms, punch %, click %, drive % */
    void start (int pitch, float velocity, const float* k) noexcept
    {
        on = true; resetChoke();
        f0 = 48.0f * semis (k[0] + (float) (pitch - 36));
        depth = 0.5f + 5.5f * k[2] * 0.01f;
        pitchEnv = 1.0f;
        pCoef = msToCoeff (90.0f, sr);
        aCoef = msToCoeff (k[1], sr);
        clickAmt = k[3] * 0.01f; clickEnv = 1.0f; clickCoef = msToCoeff (6.0f, sr);
        drive = k[4] * 0.01f;
        vel = velocity;
        attackInc = (float) (1.0 / (0.0004 * sr));   // 0.4 ms from the current level: never a step
        attacking = true;
        if (amp <= 1.0e-4f) { osc.ph = 0.0; clickHp.reset(); }
    }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i)
        {
            if (attacking) { amp = std::min (1.0f, amp + attackInc); if (amp >= 1.0f) attacking = false; }
            else amp *= aCoef;
            pitchEnv *= pCoef;
            const double f = f0 * (1.0f + depth * pitchEnv * pitchEnv);
            float x = osc.sine (f / sr) * amp;
            clickHp.process (nz.next());
            x += clickAmt * 0.6f * clickEnv * clickHp.hp;
            clickEnv *= clickCoef;
            x = softClip (x, drive) * vel;
            if (! stepChoke()) break;
            x *= choke;
            spread (x, pan, L[i], R[i]);
            if (! attacking && amp < 1.0e-4f) on = false;
        }
    }
};

struct SnareVoice : VoiceBase
{
    Osc b1, b2; Noise nz; Svf hp, lp, band;
    double sr = 48000.0;
    int kind = 0;   // 0 snare, 1 clap, 2 rim, 3 snare + clap
    float bodyAmp = 0, bodyCoef = 0.99f, noiseAmp = 0, noiseCoef = 0.99f, bodyGain = 0.5f, noiseGain = 0.5f;
    float k1 = 1.0f, drive = 0.1f, ramp = 0.0f, rampInc = 0.05f, clapEnv = 0, clapCoef = 0.99f, tail = 0, tailCoef = 0.99f, rimCoef = 0.99f;
    int t = 0;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: tune st, decay ms, tone %, snap %, drive % */
    void start (int pitch, float velocity, const float* k) noexcept
    {
        on = true; resetChoke();
        kind = pitch == 39 ? 1 : (pitch == 37 ? 2 : (pitch == 40 ? 3 : 0));
        k1 = semis (k[0]);
        const float decay = k[1], tone = k[2] * 0.01f, snap = k[3] * 0.01f;
        bodyCoef = msToCoeff (decay * 0.35f, sr);
        noiseCoef = msToCoeff (decay, sr);
        bodyGain = 0.9f * (1.0f - 0.7f * tone);
        noiseGain = 0.35f + 0.65f * tone;
        hp.set (700.0f + snap * 4200.0f, 0.7f, sr);
        lp.set (9000.0f + snap * 6000.0f, 0.7f, sr);
        band.set (kind == 2 ? 2400.0f * k1 : 1150.0f * k1, kind == 2 ? 2.0f : 1.4f, sr);
        clapCoef = msToCoeff (9.0f, sr);
        tailCoef = msToCoeff (decay * 0.9f, sr);
        drive = k[4] * 0.01f;
        vel = velocity;
        rimCoef = msToCoeff (decay * 0.12f, sr);
        bodyAmp = kind != 1 ? 1.0f : 0.0f;
        noiseAmp = (kind == 0 || kind == 3) ? 1.0f : 0.0f;
        clapEnv = (kind == 1 || kind == 3) ? 1.0f : 0.0f;
        tail = 0.0f; t = 0;
        rampInc = (float) (1.0 / (0.0004 * sr));
        ramp = 0.0f;
        b1.ph = b2.ph = 0.0;
    }
    void render (float* L, float* R, int n) noexcept
    {
        const int burst = (int) (0.0095 * sr);
        for (int i = 0; i < n && on; ++i, ++t)
        {
            ramp = std::min (1.0f, ramp + rampInc);
            const float w = nz.next();
            float x = 0.0f;
            if (kind == 0 || kind == 3)
            {
                const float body = 0.6f * b1.sine (185.0 * k1 / sr) + 0.4f * b2.sine (330.0 * k1 / sr);
                hp.process (w); lp.process (hp.hp);
                x += bodyGain * body * bodyAmp + noiseGain * lp.lp * noiseAmp;
                bodyAmp *= bodyCoef; noiseAmp *= noiseCoef;
            }
            if (kind == 1 || kind == 3)
            {
                band.process (w);
                if (t < 3 * burst) { if (t % burst == 0) clapEnv = 1.0f; }
                else if (t == 3 * burst) tail = 0.8f;
                x += 2.2f * band.bp * (clapEnv + tail);
                clapEnv *= clapCoef; tail *= tailCoef;
            }
            if (kind == 2)
            {
                band.process (w);
                x += (0.7f * b1.sine (1650.0 * k1 / sr) + 1.5f * band.bp) * bodyAmp;
                bodyAmp *= rimCoef;
            }
            x = softClip (x * ramp, drive) * vel;
            if (! stepChoke()) break;
            spread (x * choke, pan, L[i], R[i]);
            if (std::max ({ bodyAmp, noiseAmp, clapEnv, tail }) < 1.0e-4f && (t > 3 * burst || (kind != 1 && kind != 3))) on = false;
        }
    }
};

/** 808-style metallic hats: six band-limited squares at the classic ratios + noise, band-passed. */
struct HatVoice : VoiceBase
{
    std::array<Osc, 6> osc; Noise nz; Svf bp, hp;
    double sr = 48000.0;
    float amp = 0.0f, coef = 0.99f, k1 = 1.0f, noiseMix = 0.3f, ramp = 1.0f, rampInc = 0.05f;
    bool open = false;
    static constexpr float kRatios[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

    void prepare (double rate) noexcept { sr = rate; }
    /** k: tune st, closed ms, open ms, tone %, width % */
    void start (int pitch, float velocity, const float* k, float panJitter) noexcept
    {
        on = true; resetChoke();
        open = pitch == 46;
        const float decay = open ? k[2] : (pitch == 44 ? k[1] * 0.6f : k[1]);
        coef = msToCoeff (decay, sr);
        k1 = semis (k[0]);
        const float tone = k[3] * 0.01f;
        bp.set (8000.0f + 2500.0f * tone, 1.1f, sr);
        hp.set (5200.0f + 3800.0f * tone, 0.7f, sr);
        noiseMix = 0.15f + 0.35f * tone;
        vel = velocity * (pitch == 44 ? 0.6f : 1.0f);
        pan = panJitter;
        rampInc = (float) (1.0 / (0.0003 * sr));
        ramp = 0.0f;
        amp = 1.0f;
    }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i)
        {
            float m = 0.0f;
            for (size_t o = 0; o < 6; ++o) m += osc[o].square (kRatios[o] * 2.0 * k1 / sr);
            const float src = (1.0f - noiseMix) * m * (1.0f / 6.0f) + noiseMix * nz.next();
            bp.process (src); hp.process (bp.bp);
            ramp = std::min (1.0f, ramp + rampInc);
            const float x = 2.4f * hp.hp * amp * ramp * vel;
            amp *= coef;
            if (! stepChoke()) break;
            spread (x * choke, pan, L[i], R[i]);
            if (amp < 1.0e-4f) on = false;
        }
    }
};

/** Toms, crash, tambourine, cowbell, shaker, clave (picked by the note number). */
struct PercVoice : VoiceBase
{
    std::array<Osc, 6> metal; Osc o1, o2; Noise nz; Svf f1, f2, bright;
    double sr = 48000.0;
    int kind = 0;   // 0 tom, 1 crash, 2 tambourine, 3 cowbell, 4 shaker, 5 clave
    float base = 120.0f, amp = 0, coef = 0.99f, pEnv = 0, pCoef = 0.99f, drive = 0, att = 0, attInc = 1, jingle = 0;
    int t = 0;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: tune st, length %, tone %, drive %, width % */
    void start (int pitch, float velocity, const float* k, float panJitter) noexcept
    {
        on = true; resetChoke();
        const float kk = semis (k[0]), len = k[1] * 0.01f, tone = k[2] * 0.01f;
        drive = k[3] * 0.01f; vel = velocity; pan = panJitter; t = 0; pEnv = 1.0f;
        bright.set (2500.0f + tone * 15000.0f, 0.7f, sr);
        att = 0.0f; attInc = (float) (1.0 / (0.0005 * sr));
        switch (pitch)
        {
            case 49: case 57: kind = 1; coef = msToCoeff (1600.0f * len, sr); f1.set (4500.0f * kk, 0.7f, sr); break;
            case 54:          kind = 2; coef = msToCoeff (170.0f * len, sr); f1.set (7000.0f * kk, 0.8f, sr); break;
            case 56:          kind = 3; coef = msToCoeff (260.0f * len, sr); f1.set (2650.0f * kk, 2.2f, sr); base = 562.0f * kk; break;
            case 70: case 69: kind = 4; coef = msToCoeff (75.0f * len, sr); f1.set (6500.0f * kk, 1.5f, sr); attInc = (float) (1.0 / (0.008 * sr)); break;
            case 75: case 76: kind = 5; coef = msToCoeff (28.0f * len, sr); base = 2500.0f * kk; break;
            default:
                kind = 0;
                base = 62.0f * semis ((float) (std::clamp (pitch, 41, 53) - 41)) * kk;   // low floor tom .. high tom
                coef = msToCoeff (380.0f * len, sr); pCoef = msToCoeff (120.0f, sr);
                f2.set (500.0f, 0.7f, sr);
                o1.ph = 0.0;
                break;
        }
        amp = 1.0f;
    }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i, ++t)
        {
            float x = 0.0f;
            const float w = nz.next();
            switch (kind)
            {
                case 0:
                    pEnv *= pCoef;
                    f2.process (w);
                    x = o1.sine (base * (1.0f + 0.7f * pEnv) / sr) * 0.9f + 0.25f * f2.lp * pEnv;
                    break;
                case 1:
                {
                    float m = 0.0f;
                    for (size_t o = 0; o < 6; ++o) m += metal[o].square (HatVoice::kRatios[o] * 1.7 / sr);
                    f1.process (0.5f * w + 0.5f * m * (1.0f / 6.0f));
                    x = 1.3f * f1.hp;
                    break;
                }
                case 2:
                {
                    f1.process (w);
                    const float ph = std::fmod ((float) t / (float) (0.016 * sr), 1.0f);
                    jingle = std::exp (-6.0f * ph);
                    x = 1.6f * f1.hp * (0.4f + 0.6f * jingle);
                    break;
                }
                case 3:
                    f1.process (0.5f * o1.square (base / sr) + 0.5f * o2.square (base * 1.5 / sr));
                    x = 0.7f * f1.bp;
                    break;
                case 4:
                    f1.process (w);
                    x = 1.5f * f1.bp;
                    break;
                case 5:
                    x = o1.sine (base / sr);
                    break;
                default: break;
            }
            att = std::min (1.0f, att + attInc);
            bright.process (x);
            x = softClip (bright.lp * amp * att, drive) * vel;
            amp *= coef;
            if (! stepChoke()) break;
            spread (x * choke, pan, L[i], R[i]);
            if (amp < 1.0e-4f) on = false;
        }
    }
};

//==============================================================================
// MELODIC
//==============================================================================
/** Mono bass with legato glide. Types: 808, Reese, Pluck, Upright. */
struct BassVoice : VoiceBase
{
    Osc s1, s2, s3, sub; Noise nz; Svf filt, thump;
    Adsr env;
    double sr = 48000.0;
    int type = 0;
    float freq = 55.0f, target = 55.0f, glideCoef = 0.0f, punchEnv = 0.0f, punchCoef = 0.99f, fEnv = 0, fCoef = 0.99f;
    float drive = 0.3f, tone = 0.5f, punch = 0.4f;
    int ctr = 0;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: decay ms, glide ms, drive %, tone %, punch % */
    void start (int pitch, float velocity, const float* k, int typeIndex, int gateSamples) noexcept
    {
        const bool legato = on && gate > 0;
        type = typeIndex;
        target = midiHz ((float) pitch);
        const float glideMs = k[1];
        glideCoef = glideMs > 1.0f ? (float) std::exp (-1.0 / (glideMs * 0.001 * sr)) : 0.0f;
        drive = k[2] * 0.01f; tone = k[3] * 0.01f; punch = k[4] * 0.01f;
        setVelocity (velocity, ! on || env.v < 1.0e-3f, sr);
        gate = gateSamples;
        if (legato && glideCoef > 0.0f)
            return;                       // 808 slide: keep the envelope, glide to the new pitch
        if (! on || glideCoef <= 0.0f) freq = target;
        on = true; resetChoke();
        switch (type)
        {
            case 0:  env.set (sr, 1.0f, k[0], 0.0f, 90.0f); break;
            case 1:  env.set (sr, 4.0f, k[0], 0.75f, 110.0f); break;
            case 2:  env.set (sr, 1.0f, k[0] * 0.5f, 0.0f, 60.0f); break;
            default: env.set (sr, 1.5f, k[0] * 0.6f, 0.0f, 70.0f); break;
        }
        env.gateOn();
        punchEnv = 1.0f; punchCoef = msToCoeff (60.0f, sr);
        fEnv = 1.0f; fCoef = msToCoeff (std::max (40.0f, k[0] * 0.3f), sr);
        if (env.v < 1.0e-3f) { s1.ph = 0.0; sub.ph = 0.0; }
        thump.set (260.0f, 0.7f, sr);
        ctr = 0;
    }
    void release() noexcept { gate = 0; env.gateOff(); }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i)
        {
            if (gate > 0 && --gate == 0) env.gateOff();
            freq = glideCoef > 0.0f ? target + (freq - target) * glideCoef : target;
            stepVelocity();
            const float a = env.next();
            float x = 0.0f;
            switch (type)
            {
                case 0:
                {
                    punchEnv *= punchCoef;
                    const double f = freq * (1.0f + 0.12f * punch * punchEnv);
                    x = s1.sine (f / sr) + 0.12f * sub.sine (2.0 * f / sr);
                    x = softClip (x, 0.15f + 0.85f * drive);
                    if ((ctr & 15) == 0) filt.set (120.0f + tone * tone * 9000.0f, 0.7f, sr);
                    filt.process (x); x = filt.lp;
                    break;
                }
                case 1:
                {
                    x = 0.4f * s1.saw (freq * 1.007 / sr) + 0.4f * s2.saw (freq * 0.993 / sr) + 0.6f * sub.sine (freq / sr);
                    if ((ctr & 15) == 0) filt.set (70.0f + tone * tone * 4000.0f, 0.9f + punch, sr);
                    filt.process (x); x = softClip (filt.lp, drive);
                    break;
                }
                case 2:
                {
                    fEnv *= fCoef;
                    x = 0.5f * s1.saw (freq / sr) + 0.35f * s2.square (freq / sr);
                    if ((ctr & 15) == 0) filt.set (90.0f + (tone * 1500.0f) * (1.0f + 6.0f * punch * fEnv), 1.2f, sr);
                    filt.process (x); x = softClip (filt.lp, drive);
                    break;
                }
                default:
                {
                    punchEnv *= punchCoef;
                    thump.process (nz.next());
                    x = s1.sine (freq / sr) + 0.18f * s3.sine (3.0 * freq / sr) + 0.5f * punch * thump.lp * punchEnv;
                    if ((ctr & 15) == 0) filt.set (300.0f + tone * 3000.0f, 0.7f, sr);
                    filt.process (x); x = softClip (filt.lp, drive);
                    break;
                }
            }
            ++ctr;
            x *= a * vel * 0.8f;
            if (! stepChoke()) break;
            spread (x * choke, 0.0f, L[i], R[i]);
            if (! env.active()) on = false;
        }
    }
};

/** Polyphonic keys. Types: E-Piano (FM), Bell (FM), Organ (drawbars), Guitar (plucked string). */
struct KeysVoice : VoiceBase
{
    static constexpr int kString = 8192;
    Osc car, mod, tine, d1, d2, d3, d4, d5; Noise nz; Svf exc;
    Adsr env;
    double sr = 48000.0;
    int type = 0, note = 60;
    float f = 261.6f, index = 1.0f, idxCoef = 0.999f, tineEnv = 0, tineCoef = 0.99f, tone = 0.5f, drive = 0.0f, perc = 0, percCoef = 0.99f, percAmp = 0, percInc = 0.01f;
    // plucked string
    std::array<float, kString> line {};
    double delay = 100.0; int wpos = 0; float last = 0.0f, damp = 0.996f, dampRelease = 0.9f; int excite = 0;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: tone %, decay ms, release ms, width %, drive % */
    void start (int pitch, float velocity, const float* k, int typeIndex, int gateSamples) noexcept
    {
        const bool wasOn = on;
        on = true; resetChoke();
        type = typeIndex; note = pitch; gate = gateSamples;
        f = midiHz ((float) pitch); tone = k[0] * 0.01f; drive = k[4] * 0.01f;
        setVelocity (velocity, ! wasOn || env.v < 1.0e-3f, sr);
        const float decayMs = k[1], releaseMs = k[2];
        pan = std::clamp ((float) (pitch - 60) / 24.0f, -1.0f, 1.0f) * k[3] * 0.01f;
        switch (type)
        {
            case 0:   // E-Piano
                env.set (sr, 2.0f, decayMs, 0.0f, releaseMs);
                index = (0.4f + 2.6f * tone) * (0.5f + 0.5f * velocity); idxCoef = msToCoeff (600.0f, sr);
                tineEnv = 0.35f * velocity; tineCoef = msToCoeff (40.0f, sr);
                break;
            case 1:   // Bell
                env.set (sr, 1.0f, decayMs * 1.4f, 0.0f, releaseMs * 2.0f);
                index = 1.0f + 4.0f * tone; idxCoef = msToCoeff (decayMs * 0.4f, sr);
                break;
            case 2:   // Organ
                env.set (sr, 6.0f, 50.0f, 1.0f, std::min (releaseMs, 400.0f));
                perc = 0.6f; percCoef = msToCoeff (220.0f, sr);
                percAmp = 0.0f; percInc = (float) (1.0 / (0.003 * sr));   // key click rises in 3 ms (also on a re-used voice)
                break;
            default:  // Guitar: Karplus-Strong string
            {
                env.set (sr, 0.5f, 60000.0f, 1.0f, releaseMs);
                const double minHz = sr / (kString - 4);
                delay = sr / std::max ((double) f, minHz);
                const double t60 = std::max (0.2, decayMs * 0.001 * 1.5);
                damp = (float) std::pow (10.0, -3.0 / (t60 * f));
                dampRelease = (float) std::pow (10.0, -3.0 / (std::max (0.02, releaseMs * 0.001) * f));
                excite = (int) delay;
                exc.set (1200.0f + 9000.0f * tone, 0.6f, sr);
                if (! wasOn) { line.fill (0.0f); wpos = 0; last = 0.0f; }
                break;
            }
        }
        if (type != 3 && env.v < 1.0e-3f) { car.ph = 0.0; mod.ph = 0.0; }
        env.gateOn();
    }
    void release() noexcept { gate = 0; env.gateOff(); }
    float stringSample() noexcept
    {
        float in = 0.0f;
        if (excite > 0) { exc.process (nz.next()); in = exc.lp * 0.9f * vel; --excite; }
        double rp = (double) wpos - delay;
        while (rp < 0.0) rp += kString;
        const int i0 = (int) rp; const float fr = (float) (rp - i0);
        const float a = line[(size_t) (i0 & (kString - 1))], b = line[(size_t) ((i0 + 1) & (kString - 1))];
        const float y = a + fr * (b - a);
        const float g = env.stage == Adsr::release ? dampRelease : damp;
        const float filtered = g * 0.5f * (y + last);
        last = y;
        line[(size_t) wpos] = in + filtered;
        wpos = (wpos + 1) & (kString - 1);
        return y;
    }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i)
        {
            if (gate > 0 && --gate == 0) env.gateOff();
            stepVelocity();
            const float a = env.next();
            float x = 0.0f;
            switch (type)
            {
                case 0:
                {
                    const float m = mod.sine (f / sr) * index; index *= idxCoef;
                    const float t = tine.sine (f * 14.0 / sr) * tineEnv; tineEnv *= tineCoef;
                    car.ph += m * 0.15; car.ph -= std::floor (car.ph);
                    x = car.sine (f / sr) + 0.3f * t;
                    break;
                }
                case 1:
                {
                    const float m = mod.sine (f * 3.5 / sr) * index; index *= idxCoef;
                    car.ph += m * 0.12; car.ph -= std::floor (car.ph);
                    x = 0.8f * car.sine (f / sr);
                    break;
                }
                case 2:
                {
                    x = 0.5f * d1.sine (0.5 * f / sr) + 0.8f * car.sine (f / sr) + 0.5f * d2.sine (2.0 * f / sr)
                      + 0.35f * tone * d3.sine (3.0 * f / sr) + 0.3f * tone * d4.sine (4.0 * f / sr)
                      + perc * percAmp * d5.sine (3.0 * f / sr);
                    perc *= percCoef; percAmp = std::min (1.0f, percAmp + percInc);
                    x *= 0.45f;
                    break;
                }
                default:
                    x = stringSample() * 3.2f;
                    break;
            }
            x = softClip (x * a * vel * 0.5f, drive);
            if (! stepChoke()) break;
            spread (x * choke, pan, L[i], R[i]);
            if (! env.active()) on = false;
        }
    }
};

/** Mono lead with glide. Types: Saw, Square, Supersaw, Flute. */
struct LeadVoice : VoiceBase
{
    Osc o1, o2, o3, vib; Noise nz; Svf filt, breath;
    Adsr env;
    double sr = 48000.0;
    int type = 0, sinceStart = 0;
    float freq = 440, target = 440, glideCoef = 0, fEnv = 0, fCoef = 0.99f, cutoff = 0.6f, reso = 0.25f, envAmt = 0.4f;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: cutoff %, reso %, env %, glide ms, release ms */
    void start (int pitch, float velocity, const float* k, int typeIndex, int gateSamples) noexcept
    {
        const bool legato = on && gate > 0;
        type = typeIndex; target = midiHz ((float) pitch);
        glideCoef = k[3] > 1.0f ? (float) std::exp (-1.0 / (k[3] * 0.001 * sr)) : 0.0f;
        cutoff = k[0] * 0.01f; reso = k[1] * 0.01f; envAmt = k[2] * 0.01f;
        setVelocity (velocity, ! on || env.v < 1.0e-3f, sr);
        gate = gateSamples;
        env.set (sr, type == 3 ? 35.0f : 6.0f, 350.0f, 0.8f, k[4]);
        if (! legato || glideCoef <= 0.0f)
        {
            if (! on || glideCoef <= 0.0f) freq = target;
            fEnv = 1.0f; fCoef = msToCoeff (450.0f, sr); sinceStart = 0;
            env.gateOn();
        }
        on = true; resetChoke();
        breath.set (std::max (300.0f, target * 2.0f), 1.2f, sr);
    }
    void release() noexcept { gate = 0; env.gateOff(); }
    void render (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n && on; ++i, ++sinceStart)
        {
            if (gate > 0 && --gate == 0) env.gateOff();
            freq = glideCoef > 0.0f ? target + (freq - target) * glideCoef : target;
            stepVelocity();
            const float vibDepth = sinceStart > (int) (0.25 * sr) ? 0.006f : 0.0f;
            const double f = freq * (1.0f + vibDepth * vib.sine (5.5 / sr));
            const float a = env.next();
            float l = 0.0f, r = 0.0f;
            switch (type)
            {
                case 0: l = r = o1.saw (f / sr); break;
                case 1: l = r = o1.square (f / sr) * 0.8f; break;
                case 2:
                {
                    const float c = o1.saw (f / sr), lo = o2.saw (f * 0.9897 / sr), hi = o3.saw (f * 1.0104 / sr);
                    l = 0.45f * (c + 1.3f * lo); r = 0.45f * (c + 1.3f * hi);
                    break;
                }
                default:
                {
                    breath.process (nz.next());
                    l = r = o1.sine (f / sr) + 0.12f * o2.sine (2.0 * f / sr) + 0.25f * breath.bp;
                    break;
                }
            }
            fEnv *= fCoef;
            if ((sinceStart & 15) == 0)
            {
                const float oct = 4.0f * cutoff + 5.0f * envAmt * fEnv;
                filt.set (std::min (18000.0f, 90.0f * std::exp2 (oct + 2.5f)), 0.6f + 7.0f * reso * reso, sr);
            }
            filt.process (0.5f * (l + r));
            const float mono = filt.lp;
            const float side = 0.5f * (l - r);
            const float g = a * vel * 0.45f;
            if (! stepChoke()) break;
            L[i] += (mono + side * 0.6f) * g * choke;
            R[i] += (mono - side * 0.6f) * g * choke;
            if (! env.active()) on = false;
        }
    }
};

/** Polyphonic pads. Types: Warm (supersaw), Strings, Choir (formants), Glass (FM). */
struct PadVoice : VoiceBase
{
    std::array<Osc, 5> o; Osc lfo, mod; Svf filt, fA, fB, fC;
    Adsr env;
    double sr = 48000.0;
    int type = 0;
    float f = 220, cutoff = 0.45f, width = 0.7f, motion = 0.3f, lfoRate = 0.25f;
    int ctr = 0;

    void prepare (double rate) noexcept { sr = rate; }
    /** k: cutoff %, attack ms, release ms, width %, motion % */
    void start (int pitch, float velocity, const float* k, int typeIndex, int gateSamples, float lfoPhase) noexcept
    {
        const bool fresh = ! on || env.v < 1.0e-3f;
        on = true; resetChoke();
        type = typeIndex; gate = gateSamples;
        f = midiHz ((float) pitch);
        setVelocity (velocity, fresh, sr);
        cutoff = k[0] * 0.01f; width = k[3] * 0.01f; motion = k[4] * 0.01f;
        env.set (sr, k[1], 200.0f, 1.0f, k[2]);
        env.gateOn();
        if (fresh) { lfo.ph = lfoPhase; lfoRate = 0.15f + 0.3f * lfoPhase; }   // a re-used voice keeps its filter motion
        fA.set (730.0f, 4.0f, sr); fB.set (1090.0f, 5.0f, sr); fC.set (2440.0f, 6.0f, sr);
        ctr = 0;
        if (env.v < 1.0e-3f) for (size_t i = 0; i < o.size(); ++i) o[i].ph = 0.13 * (double) i;
    }
    void release() noexcept { gate = 0; env.gateOff(); }
    void render (float* L, float* R, int n) noexcept
    {
        static constexpr float det[5] = { 0.0f, -0.11f, 0.11f, -0.21f, 0.21f };
        static constexpr float pos[5] = { 0.0f, -0.6f, 0.6f, -1.0f, 1.0f };
        for (int i = 0; i < n && on; ++i)
        {
            if (gate > 0 && --gate == 0) env.gateOff();
            stepVelocity();
            const float a = env.next();
            const float lf = lfo.sine (lfoRate / sr);
            float l = 0.0f, r = 0.0f;
            const int count = type == 3 ? 2 : (type == 1 ? 3 : 5);
            for (int v = 0; v < count; ++v)
            {
                float x;
                const double fv = f * semis (det[v] * (type == 1 ? 0.8f : 1.0f));
                if (type == 3)
                {
                    const float m = mod.sine (2.0 * fv / sr) * (0.4f + 1.4f * cutoff);
                    o[(size_t) v].ph += m * 0.03; o[(size_t) v].ph -= std::floor (o[(size_t) v].ph);
                    x = o[(size_t) v].sine ((v == 0 ? fv : fv * 2.0) / sr) * (v == 0 ? 1.0f : 0.25f);
                }
                else
                    x = o[(size_t) v].saw (fv / sr);
                const float p = pos[v] * width;
                l += x * (1.0f - p) * 0.5f; r += x * (1.0f + p) * 0.5f;
            }
            float m = 0.5f * (l + r), s = 0.5f * (l - r);
            if ((ctr++ & 15) == 0)
            {
                const float oct = 2.5f + 5.5f * cutoff + motion * lf;
                filt.set (std::min (16000.0f, 60.0f * std::exp2 (oct)), type == 1 ? 0.8f : 0.6f, sr);
            }
            filt.process (m);
            m = filt.lp;
            if (type == 2)
            {
                fA.process (m); fB.process (m); fC.process (m);
                m = 1.6f * fA.bp + 1.1f * fB.bp + 0.6f * fC.bp;
                s *= 0.3f;
            }
            const float g = a * vel * (type == 3 ? 0.8f : (type == 1 ? 0.33f : 0.22f));
            if (! stepChoke()) break;
            L[i] += (m + s) * g * choke;
            R[i] += (m - s) * g * choke;
            if (! env.active()) on = false;
        }
    }
};
} // namespace hc::forge::synth
