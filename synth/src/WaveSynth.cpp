#include "WaveBank.hpp"
#include "hybrid/synth/Wave.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::synth {

using detail::WaveBank;

namespace {
constexpr double kPi = std::numbers::pi;
constexpr int    kControlEvery = 16;      // pitch / filter / position update rate (samples, counted per voice)
constexpr float  kVoiceGain = 0.25f;      // 4 full-velocity notes ≈ 0 dB before the master volume
constexpr float  kSoftKnee = 0.7f;        // output safety: untouched below −3 dBFS, never reaches 0 dBFS

float onePole(double fs, double ms) { return ms <= 0.0 ? 0.0f : static_cast<float>(std::exp(-1.0 / (fs * ms * 0.001))); }
// Coefficient that falls by 60 dB over `ms`.
float fall60(double fs, double ms) { return static_cast<float>(std::exp(std::log(1e-3) / std::max(1.0, fs * ms * 0.001))); }

float softClip(float x) noexcept {
    const float a = std::fabs(x);
    if (a <= kSoftKnee) return x;
    return std::copysign(kSoftKnee + (1.0f - kSoftKnee) * std::tanh((a - kSoftKnee) / (1.0f - kSoftKnee)), x);
}

inline float readWave(const float* w, int len, double phase) noexcept {
    const double x = phase * len;
    const int i = static_cast<int>(x);
    const float f = static_cast<float>(x - i);
    return w[i] + f * (w[i + 1] - w[i]);
}

struct Adsr {
    enum class St : std::uint8_t { Idle, Attack, Decay, Release };
    St st = St::Idle;
    float lvl = 0.0f;
    void on() noexcept { st = St::Attack; }   // from the current level (no jump on a retrigger)
    void off() noexcept { if (st != St::Idle) st = St::Release; }
    void kill() noexcept { st = St::Idle; lvl = 0.0f; }
    float tick(float attInc, float decC, float sus, float relC, bool endAtZeroSustain) noexcept {
        switch (st) {
        case St::Attack: lvl += attInc; if (lvl >= 1.0f) { lvl = 1.0f; st = St::Decay; } break;
        case St::Decay:
            lvl = sus + (lvl - sus) * decC;
            if (endAtZeroSustain && sus <= 0.0f && lvl < 1e-5f) kill();
            break;
        case St::Release: lvl *= relC; if (lvl < 1e-5f) kill(); break;
        case St::Idle: break;
        }
        return lvl;
    }
};
} // namespace

// Parameter snapshot for one render() call (audio thread only).
struct WaveSynth::Block {
    int table = 0, unison = 1, ftype = 0, mode = 0, lfoShape = 0;
    float position = 0, warp = 0, sub = 0, semis = 0, k = 2, cutoff = 20000, keytrack = 0.5f, envCut = 0, velCut = 0;
    float driveGain = 1, driveNorm = 1;
    bool  drive = false;
    float aAtt = 0, aDec = 0, aSus = 1, aRel = 0, mAtt = 0, mDec = 0, mSus = 0, mRel = 0, envPos = 0;
    float lfoPos = 0, lfoCut = 0, lfoPitchCents = 0, velAmp = 0.5f, glideCoef = 0, bendSemis = 0;
    double lfoInc = 0, maxRatio = 1;
    float uniGain = 1;
    std::array<double, kMaxUnison> ratio{};
    std::array<float, kMaxUnison> panL{}, panR{};
};

class Voice {
public:
    bool active = false, gate = false;
    int note = 60;
    float vel = 1.0f;
    std::uint64_t order = 0;
    double cur = 60, target = 60;
    Adsr amp, mod;
    std::array<double, WaveSynth::kMaxUnison> phase{}, inc{};
    double subPhase = 0, subInc = 0;
    double ic1[2]{}, ic2[2]{};
    double a1 = 1, a2 = 0, a3 = 0;
    int countdown = 0, mip = -1, dark = -1, octMip = -1, f0 = 0, f1 = 0;
    float frac = 0, modLevel = 0;

    void clearFilter() noexcept { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0; }

    void control(const WaveSynth::Block& b, double fs, float lfo) noexcept {
        cur = b.glideCoef > 0.0f ? target + (cur - target) * b.glideCoef : target;
        const double semis = cur + b.semis + b.bendSemis + lfo * b.lfoPitchCents / 100.0;
        const double hz = 440.0 * std::pow(2.0, (semis - 69.0) / 12.0);
        for (std::size_t u = 0; u < static_cast<std::size_t>(b.unison); ++u) inc[u] = hz * b.ratio[u] / fs;
        subInc = 0.5 * hz / fs;
        const double fmax = hz * b.maxRatio;
        mip = WaveBank::mipFor(0.45 * fs / fmax);
        dark = mip < 0 ? -1 : std::min(mip + 3, detail::kMips - 1);
        octMip = WaveBank::mipFor(0.45 * fs / (2.0 * fmax));
        const float pos = std::clamp(b.position + b.envPos * modLevel + b.lfoPos * lfo, 0.0f, 1.0f) * (detail::kFrames - 1);
        f0 = std::min(static_cast<int>(pos), detail::kFrames - 2);
        f1 = f0 + 1;
        frac = pos - static_cast<float>(f0);
        const double st = b.keytrack * (cur - 60.0) + b.envCut * modLevel + b.velCut * vel + b.lfoCut * lfo;
        const double fc = std::clamp(b.cutoff * std::pow(2.0, st / 12.0), 20.0, 0.45 * fs);
        const double g = std::tan(kPi * fc / fs);
        a1 = 1.0 / (1.0 + g * (g + b.k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void render(const WaveSynth::Block& b, const WaveBank& bank, double fs, const float* lfo, float* L, float* R, std::uint32_t n) noexcept {
        const float velGain = (1.0f - b.velAmp * (1.0f - vel)) * kVoiceGain;
        const int U = b.unison;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float a = amp.tick(b.aAtt, b.aDec, b.aSus, b.aRel, true);
            modLevel = mod.tick(b.mAtt, b.mDec, b.mSus, b.mRel, false);
            if (countdown <= 0) { control(b, fs, lfo[i]); countdown = kControlEvery; }
            --countdown;
            float l = 0.0f, r = 0.0f;
            if (mip >= 0) {
                const int len = bank.len[static_cast<std::size_t>(mip)];
                const float* wa = bank.wave(b.table, f0, mip);
                const float* wb = bank.wave(b.table, f1, mip);
                const int dlen = bank.len[static_cast<std::size_t>(dark)];
                const float* da = bank.wave(b.table, f0, dark);
                const float* db = bank.wave(b.table, f1, dark);
                const int olen = octMip >= 0 ? bank.len[static_cast<std::size_t>(octMip)] : 0;
                const float* oa = octMip >= 0 ? bank.wave(b.table, f0, octMip) : nullptr;
                const float* ob = octMip >= 0 ? bank.wave(b.table, f1, octMip) : nullptr;
                for (std::size_t u = 0; u < static_cast<std::size_t>(U); ++u) {
                    const double ph = phase[u];
                    const float x0 = readWave(wa, len, ph), x1 = readWave(wb, len, ph);
                    float v = x0 + frac * (x1 - x0);
                    if (b.warp < 0.0f) {          // darker: blend toward the same frame with 1/8 of the harmonics
                        const float d0 = readWave(da, dlen, ph), d1 = readWave(db, dlen, ph);
                        v += -b.warp * (d0 + frac * (d1 - d0) - v);
                    } else if (b.warp > 0.0f && oa) {   // brighter: add the frame an octave up
                        double p2 = ph * 2.0;
                        if (p2 >= 1.0) p2 -= 1.0;
                        const float o0 = readWave(oa, olen, p2), o1 = readWave(ob, olen, p2);
                        v += 0.5f * b.warp * (o0 + frac * (o1 - o0));
                    }
                    l += v * b.panL[u];
                    r += v * b.panR[u];
                    double np = ph + inc[u];
                    if (np >= 1.0) np -= 1.0;
                    phase[u] = np;
                }
                l *= b.uniGain;
                r *= b.uniGain;
            }
            if (b.sub > 0.0f) {
                const float s = b.sub * static_cast<float>(std::sin(2.0 * kPi * subPhase));
                l += s;
                r += s;
            }
            subPhase += subInc;
            if (subPhase >= 1.0) subPhase -= 1.0;
            if (b.drive) {
                l = std::tanh(l * b.driveGain) * b.driveNorm;
                r = std::tanh(r * b.driveGain) * b.driveNorm;
            }
            float out[2] = {l, r};
            for (int c = 0; c < 2; ++c) {   // zero-delay-feedback state-variable filter (trapezoidal)
                const double v0 = out[c];
                const double v3 = v0 - ic2[c];
                const double v1 = a1 * ic1[c] + a2 * v3;
                const double v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
                ic1[c] = 2.0 * v1 - ic1[c];
                ic2[c] = 2.0 * v2 - ic2[c];
                const double y = b.ftype == 0 ? v2 : b.ftype == 1 ? b.k * v1 : v0 - b.k * v1 - v2;
                out[c] = static_cast<float>(y);
            }
            const float g = a * velGain;
            L[i] += out[0] * g;
            R[i] += out[1] * g;
            if (amp.st == Adsr::St::Idle) { active = false; gate = false; clearFilter(); return; }
        }
        for (int c = 0; c < 2; ++c) {   // denormal guard on the filter memory
            if (std::fabs(ic1[c]) < 1e-20) ic1[c] = 0.0;
            if (std::fabs(ic2[c]) < 1e-20) ic2[c] = 0.0;
        }
    }
};

WaveSynth::WaveSynth() : voices_(new Voice[kVoices]), block_(new Block) { setDefaults(); }
WaveSynth::~WaveSynth() { delete[] voices_; delete block_; }

void WaveSynth::prepare(double sampleRate, std::uint32_t) {
    fs_ = sampleRate > 0 ? sampleRate : 48000.0;
    (void)WaveBank::get();
    prepared_ = true;
    reset();
}

void WaveSynth::reset() noexcept {
    for (int v = 0; v < kVoices; ++v) {
        Voice& x = voices_[v];
        x.active = x.gate = false;
        x.amp.kill(); x.mod.kill();
        x.clearFilter();
        x.phase.fill(0.0);
        x.subPhase = 0;
    }
    eventCount_ = 0;
    heldCount_ = 0;
    order_ = 0;
    lfoPhase_ = 0;
    lfoHold_ = 0;
    rng_ = 0x9E3779B9u;
    volCoef_ = onePole(fs_, 20.0);
    volCur_ = std::pow(10.0f, get(Volume) * 0.05f);
}

std::uint32_t WaveSynth::nextRandom() noexcept {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return rng_;
}

void WaveSynth::noteOn(int note, float velocity, std::uint32_t offset) noexcept {
    if (eventCount_ >= kMaxEvents || note < 0 || note > 127) return;
    int i = eventCount_++;
    while (i > 0 && events_[static_cast<std::size_t>(i - 1)].offset > offset) { events_[static_cast<std::size_t>(i)] = events_[static_cast<std::size_t>(i - 1)]; --i; }
    events_[static_cast<std::size_t>(i)] = {offset, static_cast<std::int16_t>(note), std::clamp(velocity, 0.0f, 1.0f), velocity > 0.0f};
}
void WaveSynth::noteOff(int note, std::uint32_t offset) noexcept { noteOn(note, 0.0f, offset); }

void WaveSynth::allNotesOff() noexcept {
    heldCount_ = 0;
    for (int v = 0; v < kVoices; ++v) if (voices_[v].active) { voices_[v].gate = false; voices_[v].amp.off(); voices_[v].mod.off(); }
}
void WaveSynth::pitchBend(float b) noexcept { bend_.store(std::clamp(b, -1.0f, 1.0f), std::memory_order_relaxed); }
void WaveSynth::modWheel(float m) noexcept { wheel_.store(std::clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }

int WaveSynth::activeVoices() const noexcept {
    int n = 0;
    for (int v = 0; v < kVoices; ++v) n += voices_[v].active ? 1 : 0;
    return n;
}

Voice& WaveSynth::pickVoice(int note) noexcept {
    for (int v = 0; v < kVoices; ++v) if (voices_[v].active && voices_[v].note == note) return voices_[v];   // same key again
    for (int v = 0; v < kVoices; ++v) if (!voices_[v].active) return voices_[v];
    Voice* best = nullptr;
    for (int v = 0; v < kVoices; ++v)   // oldest released
        if (!voices_[v].gate && (!best || voices_[v].order < best->order)) best = &voices_[v];
    if (best) return *best;
    best = &voices_[0];
    for (int v = 1; v < kVoices; ++v) if (voices_[v].order < best->order) best = &voices_[v];   // oldest
    return *best;
}

void WaveSynth::startNote(int note, float velocity) noexcept {
    const Block& b = *block_;
    Voice* v = nullptr;
    bool retriggerEnv = true, glideFrom = false;
    if (b.mode == 0) {
        v = &pickVoice(note);
    } else {
        if (heldCount_ < static_cast<int>(held_.size())) held_[static_cast<std::size_t>(heldCount_++)] = static_cast<std::int16_t>(note);
        for (int i = 1; i < kVoices; ++i) if (voices_[i].gate) { voices_[i].gate = false; voices_[i].amp.off(); voices_[i].mod.off(); }
        v = &voices_[0];
        glideFrom = v->active;
        if (v->active && v->gate && b.mode == 2) retriggerEnv = false;   // LEGATO: keep the envelopes running
    }
    if (!v->active) {   // a fresh voice: clean filter, envelopes from zero, unison phases spread
        v->clearFilter();
        v->amp.kill(); v->mod.kill();
        for (std::size_t u = 0; u < static_cast<std::size_t>(kMaxUnison); ++u)
            v->phase[u] = b.unison > 1 ? static_cast<double>(nextRandom() >> 8) / 16777216.0 : 0.0;
        v->subPhase = 0;
    }
    v->note = note;
    v->target = note;
    if (!(glideFrom && b.glideCoef > 0.0f)) v->cur = note;
    v->vel = velocity;
    v->order = ++order_;
    v->gate = true;
    v->active = true;
    if (retriggerEnv) { v->amp.on(); v->mod.on(); }
    v->countdown = 0;
}

void WaveSynth::stopNote(int note) noexcept {
    const Block& b = *block_;
    if (b.mode == 0) {
        for (int v = 0; v < kVoices; ++v)
            if (voices_[v].active && voices_[v].gate && voices_[v].note == note) { voices_[v].gate = false; voices_[v].amp.off(); voices_[v].mod.off(); }
        return;
    }
    int k = 0;
    for (int i = 0; i < heldCount_; ++i) if (held_[static_cast<std::size_t>(i)] != note) held_[static_cast<std::size_t>(k++)] = held_[static_cast<std::size_t>(i)];
    heldCount_ = k;
    Voice& v = voices_[0];
    if (!v.active || !v.gate || v.note != note) return;
    if (heldCount_ > 0) {   // back to the last key still held, without a new attack
        v.note = held_[static_cast<std::size_t>(heldCount_ - 1)];
        v.target = v.note;
        if (b.glideCoef <= 0.0f) v.cur = v.note;
    } else {
        v.gate = false;
        v.amp.off();
        v.mod.off();
    }
}

void WaveSynth::computeBlock() noexcept {
    Block& b = *block_;
    const double fs = fs_;
    b.table = static_cast<int>(get(Table));
    b.position = get(Position) * 0.01f;
    b.warp = get(Warp) * 0.01f;
    b.unison = static_cast<int>(get(Unison)) + 1;
    b.sub = get(Sub) * 0.01f;
    b.semis = (get(Octave) - 2.0f) * 12.0f + get(Fine) * 0.01f;
    b.ftype = static_cast<int>(get(FilterType));
    b.cutoff = get(Cutoff);
    b.k = 2.0f - 1.96f * get(Resonance) * 0.01f;
    b.keytrack = get(KeyTrack) * 0.01f;
    const float dDb = get(Drive);
    b.drive = dDb > 0.01f;
    b.driveGain = std::pow(10.0f, dDb * 0.05f);
    b.driveNorm = 1.0f / std::tanh(b.driveGain);
    b.envCut = get(EnvCutoff);
    b.velCut = get(VelCutoff);
    b.aAtt = static_cast<float>(1.0 / std::max(1.0, fs * get(AmpAttack) * 0.001));
    b.aDec = fall60(fs, get(AmpDecay));
    b.aSus = get(AmpSustain) * 0.01f;
    b.aRel = fall60(fs, get(AmpRelease));
    b.mAtt = static_cast<float>(1.0 / std::max(1.0, fs * get(ModAttack) * 0.001));
    b.mDec = fall60(fs, get(ModDecay));
    b.mSus = get(ModSustain) * 0.01f;
    b.mRel = fall60(fs, get(ModRelease));
    b.envPos = get(EnvPosition) * 0.01f;
    b.lfoShape = static_cast<int>(get(LfoShape));
    b.lfoInc = get(LfoRate) / fs;
    b.lfoPos = get(LfoPosition) * 0.01f;
    b.lfoCut = get(LfoCutoff);
    b.lfoPitchCents = get(LfoPitch) + 50.0f * wheel_.load(std::memory_order_relaxed);
    b.velAmp = get(VelAmp) * 0.01f;
    b.mode = static_cast<int>(get(Mode));
    const float gl = get(Glide);
    b.glideCoef = (b.mode != 0 && gl > 0.0f) ? static_cast<float>(std::exp(-kControlEvery / (fs * gl * 0.001))) : 0.0f;
    b.bendSemis = bend_.load(std::memory_order_relaxed) * get(BendRange);
    const float det = get(Detune), spr = get(Spread) * 0.01f;
    b.maxRatio = 1.0;
    for (int u = 0; u < b.unison; ++u) {
        const float d = b.unison > 1 ? 2.0f * static_cast<float>(u) / static_cast<float>(b.unison - 1) - 1.0f : 0.0f;
        b.ratio[static_cast<std::size_t>(u)] = std::pow(2.0, det * d / 1200.0);
        b.maxRatio = std::max(b.maxRatio, b.ratio[static_cast<std::size_t>(u)]);
        const double ang = (spr * d + 1.0) * kPi / 4.0;   // equal power, centre = 1 / 1
        b.panL[static_cast<std::size_t>(u)] = static_cast<float>(std::cos(ang) * std::numbers::sqrt2);
        b.panR[static_cast<std::size_t>(u)] = static_cast<float>(std::sin(ang) * std::numbers::sqrt2);
    }
    b.uniGain = 1.0f / std::sqrt(static_cast<float>(b.unison));
}

float WaveSynth::lfoTick() noexcept {
    const Block& b = *block_;
    const double ph = lfoPhase_;
    float v;
    switch (b.lfoShape) {
    case 0:  v = static_cast<float>(std::sin(2.0 * kPi * ph)); break;
    case 1:  v = static_cast<float>(1.0 - 4.0 * std::fabs(ph - 0.5)); break;
    case 2:  v = static_cast<float>(2.0 * ph - 1.0); break;
    case 3:  v = ph < 0.5 ? 1.0f : -1.0f; break;
    default: v = lfoHold_; break;
    }
    lfoPhase_ += b.lfoInc;
    if (lfoPhase_ >= 1.0) {
        lfoPhase_ -= 1.0;
        lfoHold_ = static_cast<float>(nextRandom() >> 8) / 8388608.0f - 1.0f;
    }
    return v;
}

void WaveSynth::render(float* L, float* R, std::uint32_t n) noexcept {
    std::fill(L, L + n, 0.0f);
    std::fill(R, R + n, 0.0f);
    if (!prepared_) { eventCount_ = 0; return; }
    computeBlock();
    const WaveBank& bank = WaveBank::get();
    std::uint32_t pos = 0;
    int ei = 0;
    while (pos < n) {
        while (ei < eventCount_ && events_[static_cast<std::size_t>(ei)].offset <= pos) {
            const Event& e = events_[static_cast<std::size_t>(ei++)];
            if (e.on) startNote(e.note, e.velocity); else stopNote(e.note);
        }
        std::uint32_t end = std::min<std::uint32_t>(n, pos + static_cast<std::uint32_t>(lfoBuf_.size()));
        if (ei < eventCount_) end = std::min(end, events_[static_cast<std::size_t>(ei)].offset);
        const std::uint32_t len = end - pos;
        for (std::uint32_t i = 0; i < len; ++i) lfoBuf_[i] = lfoTick();
        for (int v = 0; v < kVoices; ++v)
            if (voices_[v].active) voices_[v].render(*block_, bank, fs_, lfoBuf_.data(), L + pos, R + pos, len);
        pos = end;
    }
    int k = 0;   // events for later calls
    for (; ei < eventCount_; ++ei) {
        Event e = events_[static_cast<std::size_t>(ei)];
        e.offset -= n;
        events_[static_cast<std::size_t>(k++)] = e;
    }
    eventCount_ = k;
    const float target = std::pow(10.0f, get(Volume) * 0.05f);
    for (std::uint32_t i = 0; i < n; ++i) {
        volCur_ = target + volCoef_ * (volCur_ - target);
        L[i] = softClip(L[i] * volCur_);
        R[i] = softClip(R[i] * volCur_);
    }
}

} // namespace hc::synth
