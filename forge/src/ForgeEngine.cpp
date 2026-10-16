#include "hybrid/forge/ForgeEngine.hpp"
#include "hybrid/forge/Forge.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc::forge {

using namespace synth;

struct ForgeEngine::Voices {
    std::array<KickVoice, 3>  kick;
    std::array<SnareVoice, 4> snare;
    std::array<HatVoice, 4>   hats;
    std::array<PercVoice, 6>  perc;
    BassVoice                 bass;
    std::array<KeysVoice, 8>  keys;
    LeadVoice                 lead;
    std::array<PadVoice, 8>   pad;

    template <class Fn> void forEach(Fn&& fn) {
        for (auto& v : kick) fn(v);
        for (auto& v : snare) fn(v);
        for (auto& v : hats) fn(v);
        for (auto& v : perc) fn(v);
        fn(bass);
        for (auto& v : keys) fn(v);
        fn(lead);
        for (auto& v : pad) fn(v);
    }
};

namespace {
// A free voice, else the oldest one (it restarts from its current level: no hard jump).
template <class Arr>
auto& pickVoice(Arr& pool, std::uint64_t& age) noexcept {
    auto* best = &pool[0];
    for (auto& v : pool) {
        if (!v.on) { best = &v; break; }
        if (v.age < best->age) best = &v;
    }
    best->age = ++age;
    return *best;
}
// Polyphonic: re-use the voice already playing this pitch.
template <class Arr>
auto& pickPoly(Arr& pool, int pitch, std::uint64_t& age) noexcept {
    for (auto& v : pool)
        if (v.on && v.note == pitch) { v.age = ++age; return v; }
    return pickVoice(pool, age);
}
float dbToGain(float db) noexcept { return db <= -144.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
std::uint32_t nextPow2(std::uint32_t v) noexcept { std::uint32_t p = 1; while (p < v) p <<= 1; return p; }
} // namespace

// ---- send effects: an 8-line feedback-delay-network reverb and a tempo-synced ping-pong delay ----
struct ForgeEngine::Fx {
    static constexpr int kLines = 8;
    double sr = 48000.0;

    std::array<std::vector<float>, kLines> line;
    std::array<int, kLines> len{}, pos{};
    std::array<float, kLines> gain{}, lp{};
    std::array<std::vector<float>, 4> ap;     // input diffusers (2 per side)
    std::array<int, 4> apLen{}, apPos{};
    std::vector<float> pre;
    int   preLen = 1, prePos = 0;
    float damp = 0.5f, lastSize = -1.0f, lastTone = -1.0f;

    std::vector<float> dl, dr;
    int   dSize = 1, dPos = 0;
    float dTime = 0.0f, dTimeCoef = 0.0f;
    float hpL = 0, hpR = 0, hpXL = 0, hpXR = 0, lpL = 0, lpR = 0, lpCoef = 0.5f, hpCoef = 0.99f;

    void prepare(double rate) {
        sr = rate;
        static constexpr double ms[kLines] = {31.7, 37.3, 41.9, 47.3, 53.9, 59.1, 67.7, 73.3};
        for (int i = 0; i < kLines; ++i) {
            len[static_cast<std::size_t>(i)] = std::max(8, static_cast<int>(std::lround(ms[i] * 0.001 * sr)));
            line[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(len[static_cast<std::size_t>(i)]), 0.0f);
        }
        static constexpr double apMs[4] = {4.7, 3.6, 4.3, 3.1};
        for (int i = 0; i < 4; ++i) {
            apLen[static_cast<std::size_t>(i)] = std::max(4, static_cast<int>(std::lround(apMs[i] * 0.001 * sr)));
            ap[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(apLen[static_cast<std::size_t>(i)]), 0.0f);
        }
        preLen = std::max(1, static_cast<int>(std::lround(0.014 * sr)));
        pre.assign(static_cast<std::size_t>(preLen) * 2, 0.0f);
        dSize = static_cast<int>(nextPow2(static_cast<std::uint32_t>(std::ceil(3.3 * sr))));
        dl.assign(static_cast<std::size_t>(dSize), 0.0f);
        dr.assign(static_cast<std::size_t>(dSize), 0.0f);
        dTime = static_cast<float>(0.3 * sr);
        dTimeCoef = static_cast<float>(1.0 - std::exp(-1.0 / (0.04 * sr)));   // 40 ms glide: tempo changes bend, never click
        lpCoef = static_cast<float>(std::exp(-2.0 * synth::kPi * 5500.0 / sr));
        hpCoef = static_cast<float>(std::exp(-2.0 * synth::kPi * 140.0 / sr));
        reset();
    }
    void reset() noexcept {
        for (auto& l : line) std::fill(l.begin(), l.end(), 0.0f);
        for (auto& a : ap) std::fill(a.begin(), a.end(), 0.0f);
        std::fill(pre.begin(), pre.end(), 0.0f);
        std::fill(dl.begin(), dl.end(), 0.0f);
        std::fill(dr.begin(), dr.end(), 0.0f);
        lp.fill(0.0f);
        pos.fill(0); apPos.fill(0); prePos = 0; dPos = 0;
        hpL = hpR = hpXL = hpXR = lpL = lpR = 0.0f;
        lastSize = lastTone = -1.0f;
    }
    void setReverb(float size01, float tone01) noexcept {
        if (std::fabs(size01 - lastSize) < 1.0e-6f && std::fabs(tone01 - lastTone) < 1.0e-6f) return;
        lastSize = size01; lastTone = tone01;
        const double rt60 = 0.5 + 6.5 * std::pow(static_cast<double>(size01), 1.6);
        for (int i = 0; i < kLines; ++i)
            gain[static_cast<std::size_t>(i)] = static_cast<float>(std::pow(10.0, -3.0 * len[static_cast<std::size_t>(i)] / (rt60 * sr)));
        damp = static_cast<float>(std::exp(-2.0 * synth::kPi * (1500.0 * std::exp2(3.2 * tone01)) / sr));
    }
    float allpass(int i, float x) noexcept {
        auto& buf = ap[static_cast<std::size_t>(i)];
        int& p = apPos[static_cast<std::size_t>(i)];
        const float d = buf[static_cast<std::size_t>(p)];
        const float y = d - 0.6f * x;
        buf[static_cast<std::size_t>(p)] = x + 0.6f * y;
        if (++p >= apLen[static_cast<std::size_t>(i)]) p = 0;
        return y;
    }
    void reverb(const float* inL, const float* inR, float* outL, float* outR, std::uint32_t n) noexcept {
        for (std::uint32_t s = 0; s < n; ++s) {
            const float pl = pre[static_cast<std::size_t>(prePos)], pr = pre[static_cast<std::size_t>(prePos + preLen)];
            pre[static_cast<std::size_t>(prePos)] = inL[s];
            pre[static_cast<std::size_t>(prePos + preLen)] = inR[s];
            if (++prePos >= preLen) prePos = 0;
            const float xl = allpass(1, allpass(0, pl));
            const float xr = allpass(3, allpass(2, pr));
            std::array<float, kLines> y{};
            for (int i = 0; i < kLines; ++i) {
                const auto k = static_cast<std::size_t>(i);
                const float v = line[k][static_cast<std::size_t>(pos[k])];
                lp[k] = v + damp * (lp[k] - v);   // one-pole low-pass inside the loop
                y[k] = lp[k];
            }
            std::array<float, kLines> h = y;      // fast Walsh-Hadamard mix
            for (int w = 1; w < kLines; w <<= 1)
                for (int i = 0; i < kLines; i += w << 1)
                    for (int j = i; j < i + w; ++j) {
                        const float a = h[static_cast<std::size_t>(j)], b = h[static_cast<std::size_t>(j + w)];
                        h[static_cast<std::size_t>(j)] = a + b;
                        h[static_cast<std::size_t>(j + w)] = a - b;
                    }
            for (int i = 0; i < kLines; ++i) {
                const auto k = static_cast<std::size_t>(i);
                const float inj = (i < 4 ? xl : xr) * 0.5f;
                float v = (h[k] * 0.35355339f + inj) * gain[k];
                if (std::fabs(v) < 1e-20f) v = 0.0f;   // no denormals in the tail
                line[k][static_cast<std::size_t>(pos[k])] = v;
                if (++pos[k] >= len[k]) pos[k] = 0;
            }
            outL[s] += 0.4f * (y[0] - y[2] + y[4] - y[6]);
            outR[s] += 0.4f * (y[1] - y[3] + y[5] - y[7]);
        }
    }
    void delay(const float* inL, const float* inR, float* outL, float* outR, std::uint32_t n, float timeSamples, float fb) noexcept {
        const float target = std::clamp(timeSamples, 16.0f, static_cast<float>(dSize - 4));
        const int mask = dSize - 1;
        for (std::uint32_t s = 0; s < n; ++s) {
            dTime += (target - dTime) * dTimeCoef;
            double rp = static_cast<double>(dPos) - static_cast<double>(dTime);
            while (rp < 0.0) rp += dSize;
            const int i0 = static_cast<int>(rp);
            const float fr = static_cast<float>(rp - i0);
            const float a0 = dl[static_cast<std::size_t>(i0 & mask)], a1 = dl[static_cast<std::size_t>((i0 + 1) & mask)];
            const float b0 = dr[static_cast<std::size_t>(i0 & mask)], b1 = dr[static_cast<std::size_t>((i0 + 1) & mask)];
            const float yl = a0 + fr * (a1 - a0), yr = b0 + fr * (b1 - b0);
            lpL = yl + lpCoef * (lpL - yl); lpR = yr + lpCoef * (lpR - yr);
            hpL = hpCoef * (hpL + lpL - hpXL); hpXL = lpL;
            hpR = hpCoef * (hpR + lpR - hpXR); hpXR = lpR;
            float wl = 0.5f * (inL[s] + inR[s]) + fb * hpR;   // left echo feeds the right line: ping-pong
            float wr = fb * hpL;
            if (std::fabs(wl) < 1e-20f) wl = 0.0f;
            if (std::fabs(wr) < 1e-20f) wr = 0.0f;
            dl[static_cast<std::size_t>(dPos)] = wl;
            dr[static_cast<std::size_t>(dPos)] = wr;
            dPos = (dPos + 1) & mask;
            outL[s] += yl;
            outR[s] += yr;
        }
    }
};

// ---------------------------------------------------------------------------------------------

ForgeEngine::ForgeEngine() : voices_(std::make_unique<Voices>()), fx_(std::make_unique<Fx>()), edit_(std::make_unique<Song>()) {
    handoff_.writeBuffer() = *edit_;   // the audio thread always has a valid object
    handoff_.publish();
    jitter_.seed(0x48454Du);
}

ForgeEngine::~ForgeEngine() = default;

double ForgeEngine::stepSamples(double sampleRate, double bpm) noexcept { return sampleRate * 60.0 / std::clamp(bpm, 20.0, 400.0) / 4.0; }

void ForgeEngine::bind(const params::ParameterTree& tree) {
    auto get = [&tree](const std::string& s) { return tree.find(s); };
    for (int t = 0; t < kTracks; ++t) {
        TrackParams& q = tp_[static_cast<std::size_t>(t)];
        q.type = hasType(t) ? get(id::type(t)) : nullptr;
        for (int k = 0; k < kKnobs; ++k) q.k[static_cast<std::size_t>(k)] = get(id::knob(t, k));
        q.rev = get(id::rev(t));
        q.dly = get(id::dly(t));
        char b[32];
        std::snprintf(b, sizeof b, "stem%02d.main", t + 1);  q.stripMain = get(b);
        std::snprintf(b, sizeof b, "stem%02d.mute", t + 1);  q.stripMute = get(b);
        std::snprintf(b, sizeof b, "stem%02d.solo", t + 1);  q.stripSolo = get(b);
    }
    for (int s = 0; s < 13; ++s) {
        char b[32];
        std::snprintf(b, sizeof b, "stem%02d.solo", s + 1);
        anySolo_[static_cast<std::size_t>(s)] = get(b);
    }
    on_ = get(id::on); bpm_ = get(id::bpm); swing_ = get(id::swing); arrange_ = get(id::arrange);
    revSize_ = get(id::revSize); revTone_ = get(id::revTone); dlyTime_ = get(id::dlyTime); dlyFb_ = get(id::dlyFb); fxLevel_ = get(id::fxLevel);
    mainGain_ = get("main.gain"); mainMute_ = get("main.mute");
}

void ForgeEngine::prepare(double sampleRate, std::uint32_t maxBlock) {
    sr_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max<std::uint32_t>(16, maxBlock);
    trackBuf_.assign(static_cast<std::size_t>(2 * kTracks) * maxBlock_, 0.0f);
    revIn_.assign(static_cast<std::size_t>(2) * maxBlock_, 0.0f);
    dlyIn_.assign(static_cast<std::size_t>(2) * maxBlock_, 0.0f);
    fxOut_.assign(static_cast<std::size_t>(2) * maxBlock_, 0.0f);
    voices_->forEach([this](auto& v) { v.prepare(sr_); v.on = false; v.gate = 0; v.resetChoke(); });
    fx_->prepare(sr_);
    sendRev_.fill(0.0f);
    sendDly_.fill(0.0f);
    fxGain_ = 0.0f;
    wasPlaying_ = false;
    playStep.store(-1.0f);
    songBar.store(-1);
}

void ForgeEngine::setSong(const Song& s) {
    if (&s != edit_.get()) *edit_ = s;
    edit_->pattern.bars = std::clamp(edit_->pattern.bars, 1, kMaxBars);
    for (auto& t : edit_->pattern.tracks) t.sort();
    handoff_.writeBuffer() = *edit_;
    handoff_.publish();
}

void ForgeEngine::audition(int track, int pitch, int velocity) noexcept {
    if (track < 0 || track >= kTracks) return;
    (void)audition_.tryPush(Event{0, track, 0, static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)), static_cast<std::uint8_t>(std::clamp(velocity, 1, 127))});
}

float ForgeEngine::p(const params::Parameter* q, float fallback) const noexcept { return q ? q->plain() : fallback; }

bool ForgeEngine::enabled() const noexcept { return on_ && on_->isOn(); }

std::uint64_t ForgeEngine::songLength(const Song& s, double sampleRate, double bpm, bool fullSong) noexcept {
    if (s.pattern.isEmpty()) return 0;
    const bool full = fullSong && s.arrangement.active();
    const double steps = full ? s.arrangement.bars * static_cast<double>(kStepsPerBar) : s.pattern.lengthSteps();
    return static_cast<std::uint64_t>(std::llround(steps * stepSamples(sampleRate, bpm)));
}

std::uint64_t ForgeEngine::songFrames() const noexcept {
    const Song* s = playing_ ? playing_ : &handoff_.read();
    return songLength(*s, sr_, p(bpm_, 120.0f), arrange_ && arrange_->isOn());
}

void ForgeEngine::prepareVoicesForStart() noexcept {
    voices_->forEach([](auto& v) { v.on = false; v.gate = 0; v.resetChoke(); });
    fx_->reset();
    sendRev_.fill(0.0f);
    sendDly_.fill(0.0f);
    fxGain_ = 0.0f;
    wasPlaying_ = false;
}

void ForgeEngine::chokeAll() noexcept { voices_->forEach([this](auto& v) { if (v.on) v.startChoke(sr_); v.gate = 0; }); }

void ForgeEngine::releaseHeld() noexcept {
    Voices& V = *voices_;
    V.bass.release();
    V.lead.release();
    for (auto& v : V.keys) v.release();
    for (auto& v : V.pad) v.release();
}

void ForgeEngine::startNote(const Event& e) noexcept {
    const float vel = std::pow(static_cast<float>(e.velocity) / 127.0f, 1.2f);
    const int t = e.track;
    const float* k = knob_[static_cast<std::size_t>(t)].data();
    const int ty = type_[static_cast<std::size_t>(t)];
    Voices& V = *voices_;
    switch (t) {
    case kick:  pickVoice(V.kick, ageCounter_).start(e.pitch, vel, k); break;
    case snare: pickVoice(V.snare, ageCounter_).start(e.pitch, vel, k); break;
    case hats:
        if (e.pitch != drum::hatOpen)
            for (auto& h : V.hats) if (h.on && h.open) h.startChoke(sr_);   // closed / pedal hat chokes the open one
        pickVoice(V.hats, ageCounter_).start(e.pitch, vel, k, jitter_.next() * k[4] * 0.006f);
        break;
    case perc:  pickVoice(V.perc, ageCounter_).start(e.pitch, vel, k, jitter_.next() * k[4] * 0.008f); break;
    case bass:  V.bass.start(e.pitch, vel, k, ty, std::max(1, e.gate)); break;
    case keys:  pickPoly(V.keys, e.pitch, ageCounter_).start(e.pitch, vel, k, ty, std::max(1, e.gate)); break;
    case lead:  V.lead.start(e.pitch, vel, k, ty, std::max(1, e.gate)); break;
    case pad: {
        PadVoice* target = nullptr;   // re-use the voice already holding this pitch
        const float f = midiHz(static_cast<float>(e.pitch));
        for (auto& v : V.pad) if (v.on && std::fabs(v.f - f) < 0.01f) { target = &v; break; }
        if (!target) target = &pickVoice(V.pad, ageCounter_);
        target->start(e.pitch, vel, k, ty, std::max(1, e.gate), 0.5f + 0.5f * jitter_.next());
        break;
    }
    default: break;
    }
    trackHit[static_cast<std::size_t>(t)].fetch_add(1, std::memory_order_relaxed);
}

void ForgeEngine::renderVoices(std::uint32_t from, std::uint32_t to) noexcept {
    if (to <= from) return;
    const int n = static_cast<int>(to - from);
    auto L = [this, from](int t) { return trackBuf_.data() + static_cast<std::size_t>(2 * t) * maxBlock_ + from; };
    auto R = [this, from](int t) { return trackBuf_.data() + static_cast<std::size_t>(2 * t + 1) * maxBlock_ + from; };
    Voices& V = *voices_;
    for (auto& v : V.kick)  if (v.on) v.render(L(kick), R(kick), n);
    for (auto& v : V.snare) if (v.on) v.render(L(snare), R(snare), n);
    for (auto& v : V.hats)  if (v.on) v.render(L(hats), R(hats), n);
    for (auto& v : V.perc)  if (v.on) v.render(L(perc), R(perc), n);
    if (V.bass.on) V.bass.render(L(bass), R(bass), n);
    for (auto& v : V.keys)  if (v.on) v.render(L(keys), R(keys), n);
    if (V.lead.on) V.lead.render(L(lead), R(lead), n);
    for (auto& v : V.pad)   if (v.on) v.render(L(pad), R(pad), n);
}

void ForgeEngine::beginBlock(std::uint32_t frames) noexcept {
    frames = std::min(frames, maxBlock_);
    if (handoff_.update() || !playing_) playing_ = &handoff_.read();
    for (int t = 0; t < kTracks; ++t) {
        const TrackParams& q = tp_[static_cast<std::size_t>(t)];
        for (int k = 0; k < kKnobs; ++k)
            knob_[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)] = p(q.k[static_cast<std::size_t>(k)], knobSpecs(t)[static_cast<std::size_t>(k)].def);
        type_[static_cast<std::size_t>(t)] = q.type ? static_cast<int>(std::lround(q.type->plain())) : 0;
    }
    stepLen_ = stepSamples(sr_, p(bpm_, 120.0f));
    std::memset(trackBuf_.data(), 0, sizeof(float) * trackBuf_.size());
}

void ForgeEngine::render(std::uint32_t at, std::uint32_t n, std::uint64_t songPos, bool playing) noexcept {
    if (n == 0 || at + n > maxBlock_) return;
    // Transport edges: a start chokes what still rings; a stop, a seek or a loop releases held notes.
    if (playing && !wasPlaying_) chokeAll();
    else if (!playing && wasPlaying_) releaseHeld();
    else if (playing && songPos != expectPos_) releaseHeld();
    wasPlaying_ = playing;
    expectPos_  = songPos + n;

    int count = 0;
    if (at == 0) {   // notes played by hand (once per block)
        Event e{};
        while (count < kMaxEvents && audition_.tryPop(e)) {
            e.offset = 0;
            e.gate = static_cast<int>(0.35 * sr_);
            events_[static_cast<std::size_t>(count++)] = e;
        }
    }

    const Song& song = *playing_;
    const Pattern& pat = song.pattern;
    const int patSteps = pat.lengthSteps();
    const bool full = song.arrangement.active() && arrange_ && arrange_->isOn();
    if (playing && !pat.isEmpty()) {
        const double s0 = static_cast<double>(songPos) / stepLen_, s1 = static_cast<double>(songPos + n) / stepLen_;
        playStep.store(static_cast<float>(std::fmod(s0, static_cast<double>(patSteps))), std::memory_order_relaxed);
        songBar.store(static_cast<int>(s0 / kStepsPerBar), std::memory_order_relaxed);
        const double swing = std::clamp(p(swing_, 50.0f) / 50.0 - 1.0, 0.0, 0.5);
        const auto loop0 = static_cast<std::int64_t>(std::floor(s0 / patSteps));
        const auto loop1 = static_cast<std::int64_t>(std::floor((s1 - 1.0e-9) / patSteps));
        for (std::int64_t loop = loop0; loop <= loop1; ++loop) {
            const double base = static_cast<double>(loop) * patSteps, lo = s0 - base, hi = s1 - base;
            for (int t = 0; t < kTracks; ++t) {
                const TrackNotes& tn = pat.tracks[static_cast<std::size_t>(t)];
                const auto first = std::lower_bound(tn.notes.begin(), tn.notes.begin() + tn.count, static_cast<float>(lo - 0.51),
                                                    [](const Note& a, float v) { return a.start < v; });
                for (auto it = first; it != tn.notes.begin() + tn.count && count < kMaxEvents; ++it) {
                    if (it->start >= hi || it->start >= static_cast<float>(patSteps)) break;
                    const double r = std::round(static_cast<double>(it->start));
                    const bool odd = std::fabs(static_cast<double>(it->start) - r) < 0.2 && (static_cast<std::int64_t>(r) & 1) != 0;
                    const double eff = static_cast<double>(it->start) + (odd ? swing : 0.0);
                    if (eff < lo || eff >= hi) continue;
                    if (full) {   // the arrangement decides which tracks play in this bar
                        const auto bar = static_cast<std::int64_t>((base + eff) / kStepsPerBar);
                        if (bar >= song.arrangement.bars || ((song.arrangement.mask[static_cast<std::size_t>(bar)] >> t) & 1u) == 0) continue;
                    }
                    const auto off = static_cast<std::int64_t>(std::floor((base + eff) * stepLen_ - static_cast<double>(songPos)));
                    events_[static_cast<std::size_t>(count++)] = {at + static_cast<std::uint32_t>(std::clamp<std::int64_t>(off, 0, n - 1)), t,
                                                                  static_cast<int>(std::max(1.0, static_cast<double>(it->length) * stepLen_)), it->pitch, it->velocity};
                }
            }
        }
    } else if (!playing) {
        playStep.store(-1.0f, std::memory_order_relaxed);
        songBar.store(-1, std::memory_order_relaxed);
    }

    for (int i = 1; i < count; ++i) {   // insertion sort by offset (bounded, allocation-free)
        const Event key = events_[static_cast<std::size_t>(i)];
        int j = i - 1;
        while (j >= 0 && events_[static_cast<std::size_t>(j)].offset > key.offset) { events_[static_cast<std::size_t>(j + 1)] = events_[static_cast<std::size_t>(j)]; --j; }
        events_[static_cast<std::size_t>(j + 1)] = key;
    }
    std::uint32_t cursor = at;
    for (int i = 0; i < count; ++i) {
        const std::uint32_t when = std::clamp(events_[static_cast<std::size_t>(i)].offset, at, at + n);
        renderVoices(cursor, when);
        cursor = when;
        startNote(events_[static_cast<std::size_t>(i)]);
    }
    renderVoices(cursor, at + n);
}

void ForgeEngine::endBlock(std::uint32_t frames) noexcept {
    frames = std::min(frames, maxBlock_);
    if (frames == 0) return;
    bool soloed = false;
    for (const auto* s : anySolo_) soloed = soloed || (s && s->isOn());

    float* rl = revIn_.data(); float* rr = rl + maxBlock_;
    float* dl = dlyIn_.data(); float* dr = dl + maxBlock_;
    std::memset(revIn_.data(), 0, sizeof(float) * revIn_.size());
    std::memset(dlyIn_.data(), 0, sizeof(float) * dlyIn_.size());
    const float inv = 1.0f / static_cast<float>(frames);
    for (int t = 0; t < kTracks; ++t) {
        const TrackParams& q = tp_[static_cast<std::size_t>(t)];
        float* L = trackBuf_.data() + static_cast<std::size_t>(2 * t) * maxBlock_;
        float* R = L + maxBlock_;
        const float vg = dbToGain(kVoiceGainDb[t] + kHeadroomDb);
        for (std::uint32_t s = 0; s < frames; ++s) { L[s] *= vg; R[s] *= vg; }
        // Post-fader sends: follow the strip's fader, mute and solo.
        const bool audible = !(q.stripMute && q.stripMute->isOn()) && (!soloed || (q.stripSolo && q.stripSolo->isOn()));
        const float fader = audible ? (q.stripMain ? dbToGain(q.stripMain->effectiveDb()) : 1.0f) : 0.0f;
        const float tr = fader * p(q.rev, 0.0f) * 0.01f, td = fader * p(q.dly, 0.0f) * 0.01f;
        float& gr = sendRev_[static_cast<std::size_t>(t)];
        float& gd = sendDly_[static_cast<std::size_t>(t)];
        if (gr != 0.0f || tr != 0.0f || gd != 0.0f || td != 0.0f)
            for (std::uint32_t s = 0; s < frames; ++s) {
                const float a = static_cast<float>(s) * inv;
                const float sr = gr + (tr - gr) * a, sd = gd + (td - gd) * a;
                rl[s] += L[s] * sr; rr[s] += R[s] * sr;
                dl[s] += L[s] * sd; dr[s] += R[s] * sd;
            }
        gr = tr;
        gd = td;
    }
    float* ol = fxOut_.data(); float* orr = ol + maxBlock_;
    std::memset(fxOut_.data(), 0, sizeof(float) * fxOut_.size());
    fx_->setReverb(p(revSize_, 55.0f) * 0.01f, p(revTone_, 50.0f) * 0.01f);
    fx_->reverb(rl, rr, ol, orr, frames);
    const int dIdx = std::clamp(static_cast<int>(std::lround(p(dlyTime_, 2.0f))), 0, kDelayChoices - 1);
    fx_->delay(dl, dr, ol, orr, frames, static_cast<float>(kDelaySteps[dIdx] * stepLen_), std::clamp(p(dlyFb_, 35.0f) * 0.01f, 0.0f, 0.95f));
    // The return joins the Main bus after its fader: apply the FX level and Main's fader / mute here.
    const bool mainOn = !(mainMute_ && mainMute_->isOn());
    const float target = mainOn ? (fxLevel_ ? dbToGain(fxLevel_->effectiveDb()) : 1.0f) * (mainGain_ ? dbToGain(mainGain_->effectiveDb()) : 1.0f) : 0.0f;
    for (std::uint32_t s = 0; s < frames; ++s) {
        const float g = fxGain_ + (target - fxGain_) * static_cast<float>(s) * inv;
        ol[s] *= g;
        orr[s] *= g;
    }
    fxGain_ = target;
}

} // namespace hc::forge
