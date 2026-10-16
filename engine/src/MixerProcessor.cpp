#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/dsp/Kernels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace hc::engine {

namespace {
constexpr double kToneLevel = 0.031622776601683794;   // -30 dBFS: 13 tones summed stay below 0 dBFS
} // namespace

MixerProcessor::MixerProcessor(Source source) noexcept : source_(source) {}

MixerProcessor::~MixerProcessor() {
    for (StemAudio*& s : slots_) { delete s; s = nullptr; }
    StemCommand c;
    while (commands_.tryPop(c)) delete c.audio;
    collectRetired();
}

bool MixerProcessor::submitStem(std::uint32_t slot, std::unique_ptr<StemAudio>& audio) noexcept {
    if (slot >= dsp::StemMatrix::kStems) return false;
    if (!commands_.tryPush(StemCommand{slot, audio.get()})) return false;
    audio.release();   // now owned by the queue / the audio thread
    return true;
}

void MixerProcessor::collectRetired() noexcept {
    StemAudio* s = nullptr;
    while (retired_.tryPop(s)) delete s;
}

void MixerProcessor::drainStemCommands() noexcept {
    bool changed = false;
    for (std::size_t k = 0; k < kStemQueue; ++k) {
        // Only take a command when the old stem is guaranteed a place in the retire ring.
        if (retired_.sizeApprox() >= kRetireQueue) break;
        StemCommand c;
        if (!commands_.tryPop(c)) break;
        StemAudio* old = slots_[c.slot];
        slots_[c.slot] = c.audio;
        if (old) (void)retired_.tryPush(old);
        changed = true;
    }
    if (changed) {
        stemFrames_ = 0;
        for (const StemAudio* s : slots_)
            if (s) stemFrames_ = std::max(stemFrames_, s->frames);
    }
}

namespace {
constexpr std::uint32_t kFadeIn = 64;
void fadeIn(float* x, std::uint32_t n) noexcept {
    const std::uint32_t m = std::min(n, kFadeIn);
    for (std::uint32_t i = 0; i < m; ++i) x[i] *= static_cast<float>(i) / static_cast<float>(kFadeIn);
}
void fadeOut(float* x, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) x[i] *= 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
}
} // namespace

bool MixerProcessor::renderStems(std::uint32_t frames) noexcept {
    constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;
    if (songFrames_ == 0) {   // everything unloaded
        playing_  = false;
        playhead_ = 0;
        seekReq_.store(-1, std::memory_order_relaxed);
        return false;
    }
    const std::int64_t seek = seekReq_.exchange(-1, std::memory_order_acq_rel);
    auto clampSeek = [&] { return songFrames_ ? std::min<std::uint64_t>(static_cast<std::uint64_t>(seek), songFrames_ - 1) : 0; };
    const bool want = wantPlay_.load(std::memory_order_acquire) && songFrames_ > 0;
    bool startFade = false, stopping = false;
    if (want && !playing_) { playing_ = true; startFade = true; }
    else if (!want && playing_) stopping = true;   // finish this block from where we are, faded out, then stop
    if (seek >= 0 && !stopping) { playhead_ = clampSeek(); startFade = true; }
    if (!playing_) return false;

    std::uint64_t pos = playhead_;
    std::uint32_t done = 0;
    bool endedAt = false;
    std::uint32_t endFrame = frames;
    while (done < frames) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames - done, songFrames_ - pos));
        for (std::uint32_t s = 0; s < kStems; ++s) {
            float* L = stemData_.data() + (static_cast<std::size_t>(s) * 2) * maxFrames_ + done;
            float* R = L + maxFrames_;
            const StemAudio* a = slots_[s];
            std::uint32_t have = 0;
            if (a && pos < a->frames) {
                have = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, a->frames - pos));
                std::memcpy(L, a->left.data() + pos, sizeof(float) * have);
                std::memcpy(R, a->right.data() + pos, sizeof(float) * have);
            }
            if (have < chunk) {
                std::memset(L + have, 0, sizeof(float) * (chunk - have));
                std::memset(R + have, 0, sizeof(float) * (chunk - have));
            }
        }
        if (forgeActive_) { forge_.render(done, chunk, pos, true); forgeRendered_ = true; }
        done += chunk;
        pos += chunk;
        if (pos >= songFrames_) {
            pos = 0;
            if (!loop_.load(std::memory_order_relaxed)) { endedAt = true; endFrame = done; break; }
        }
    }
    for (std::uint32_t s = 0; s < kStems * 2; ++s) {
        float* x = stemData_.data() + static_cast<std::size_t>(s) * maxFrames_;
        if (endFrame < frames) std::memset(x + endFrame, 0, sizeof(float) * (frames - endFrame));
        if (startFade && startFadeOn_) fadeIn(x, frames);
        if (stopping) fadeOut(x, frames);
    }
    playhead_ = pos;
    if (stopping) {
        playing_ = false;
        if (seek >= 0) playhead_ = clampSeek();   // e.g. STOP: the jump to the start happens after the fade
    }
    if (endedAt) {   // loop off: the song ended — stop and go back to the start
        playing_ = false;
        wantPlay_.store(false, std::memory_order_release);
    }
    return true;
}

double MixerProcessor::testToneHz(std::uint32_t stem) noexcept {
    return 110.0 * std::pow(2.0, 3.0 * static_cast<double>(stem) / 12.0);
}

void MixerProcessor::prepare(const io::StreamConfig& config) {
    maxFrames_ = config.blockFrames;
    matrix_.prepare(maxFrames_, static_cast<double>(config.sampleRate));
    forge_.prepare(static_cast<double>(config.sampleRate), maxFrames_);
    mainMix_.allocate(static_cast<std::size_t>(2) * maxFrames_);
    limiter_.prepare(static_cast<double>(config.sampleRate), maxFrames_);
    toneData_.allocate(static_cast<std::size_t>(dsp::StemMatrix::kStems) * maxFrames_);
    stemData_.allocate(static_cast<std::size_t>(dsp::StemMatrix::kStems) * 2 * maxFrames_);
    for (std::uint32_t s = 0; s < dsp::StemMatrix::kStems; ++s) {
        const double w = 2.0 * std::numbers::pi * testToneHz(s) / static_cast<double>(config.sampleRate);
        osc_[s] = Osc{std::cos(w), std::sin(w), 1.0, 0.0};
    }
}

void MixerProcessor::process(const ProcessContext& ctx) noexcept {
    if (ctx.frames == 0 || ctx.frames > maxFrames_) return;
    constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;

    std::array<const float*, kStems> L{};
    std::array<const float*, kStems> R{};

    drainStemCommands();
    const Source src = source_.load(std::memory_order_relaxed);

    // Beat Forge: on with the Stems source and forge.on.
    const bool forgeOn = src == Source::Stems && forge_.enabled();
    if (forgeOn && !forgeWasOn_) forge_.prepareVoicesForStart();
    forgeWasOn_ = forgeActive_ = forgeOn;
    forgeRendered_ = false;
    if (forgeOn) forge_.beginBlock(ctx.frames);
    songFrames_ = std::max(stemFrames_, forgeOn ? forge_.songFrames() : 0);
    if (songFrames_ == 0 || playhead_ >= songFrames_) playhead_ = 0;

    if (src == Source::Stems) {
        if (renderStems(ctx.frames)) {
            for (std::uint32_t s = 0; s < kStems; ++s) {
                L[s] = stemData_.data() + (static_cast<std::size_t>(s) * 2) * maxFrames_;
                R[s] = L[s] + maxFrames_;
            }
        }   // stopped or nothing loaded: every stem is silent (nullptr)
        if (forgeOn) {
            if (!forgeRendered_) forge_.render(0, ctx.frames, playhead_, false);   // stopped: tails and hand-played notes
            forge_.endBlock(ctx.frames);
            for (int t = 0; t < forge::kTracks; ++t) {
                L[static_cast<std::size_t>(t)] = forge_.track(t, 0);
                R[static_cast<std::size_t>(t)] = forge_.track(t, 1);
            }
        }
    } else if (src == Source::TestTones) {
        for (std::uint32_t s = 0; s < kStems; ++s) {
            float* out = toneData_.data() + static_cast<std::size_t>(s) * maxFrames_;
            Osc&   o   = osc_[s];
            double x = o.x, y = o.y;
            for (std::uint32_t i = 0; i < ctx.frames; ++i) {
                const double nx = o.c * x - o.s * y;
                y = o.s * x + o.c * y;
                x = nx;
                out[i] = static_cast<float>(y * kToneLevel);
            }
            const double r = 1.0 / std::sqrt(x * x + y * y);
            o.x = x * r;
            o.y = y * r;
            L[s] = R[s] = out;
        }
    } else {
        for (std::uint32_t s = 0; s < kStems; ++s) {
            const std::uint32_t l = 2 * s, r = 2 * s + 1;
            L[s] = (l < ctx.numInputs && ctx.inputs) ? ctx.inputs[l] : nullptr;
            R[s] = (r < ctx.numInputs && ctx.inputs) ? ctx.inputs[r] : L[s];
        }
    }

    matrix_.process(L.data(), R.data(), ctx.frames);
    playheadPub_.store(playhead_, std::memory_order_relaxed);
    songPub_.store(songFrames_, std::memory_order_relaxed);
    playingPub_.store(playing_, std::memory_order_relaxed);

    std::array<const float*, dsp::BusLimiter::kChannels> buses{};
    for (std::uint32_t b = 0; b < dsp::StemMatrix::kBuses; ++b)
        for (std::uint32_t c = 0; c < 2; ++c) buses[2 * b + c] = matrix_.busChannel(b, c);
    if (forgeOn) {   // Beat Forge's reverb / delay return joins Main before the limiter
        for (std::uint32_t c = 0; c < 2; ++c) {
            float* m = mainMix_.data() + static_cast<std::size_t>(c) * maxFrames_;
            dsp::copy(m, matrix_.busChannel(0, c), ctx.frames);
            dsp::mixAdd(m, forge_.fxReturn(static_cast<int>(c)), 1.0f, ctx.frames);
            buses[c] = m;
        }
    }
    forgePub_.store(forgeOn, std::memory_order_relaxed);
    limiter_.process(buses.data(), ctx.frames);

    for (std::uint32_t b = 0; b < dsp::StemMatrix::kBuses; ++b)
        for (std::uint32_t c = 0; c < 2; ++c) {
            const std::uint32_t out = 2 * b + c;
            if (out < ctx.numOutputs) dsp::copy(ctx.outputs[out], limiter_.channel(b, c), ctx.frames);
        }
}

} // namespace hc::engine
