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
        loopFrames_ = 0;
        for (const StemAudio* s : slots_)
            if (s) loopFrames_ = std::max(loopFrames_, s->frames);
        if (loopFrames_ == 0 || playhead_ >= loopFrames_) playhead_ = 0;
    }
}

void MixerProcessor::renderStems(std::uint32_t frames) noexcept {
    for (std::uint32_t s = 0; s < dsp::StemMatrix::kStems; ++s) {
        float* L = stemData_.data() + (static_cast<std::size_t>(s) * 2) * maxFrames_;
        float* R = L + maxFrames_;
        const StemAudio* a = slots_[s];
        std::uint64_t pos = playhead_;
        std::uint32_t done = 0;
        while (done < frames) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames - done, loopFrames_ - pos));
            std::uint32_t have = 0;
            if (a && pos < a->frames) {
                have = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, a->frames - pos));
                std::memcpy(L + done, a->left.data() + pos, sizeof(float) * have);
                std::memcpy(R + done, a->right.data() + pos, sizeof(float) * have);
            }
            if (have < chunk) {
                std::memset(L + done + have, 0, sizeof(float) * (chunk - have));
                std::memset(R + done + have, 0, sizeof(float) * (chunk - have));
            }
            done += chunk;
            pos += chunk;
            if (pos >= loopFrames_) pos = 0;
        }
    }
    playhead_ = (playhead_ + frames) % loopFrames_;
}

double MixerProcessor::testToneHz(std::uint32_t stem) noexcept {
    return 110.0 * std::pow(2.0, 3.0 * static_cast<double>(stem) / 12.0);
}

void MixerProcessor::prepare(const io::StreamConfig& config) {
    maxFrames_ = config.blockFrames;
    matrix_.prepare(maxFrames_);
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
    if (restart_.exchange(false, std::memory_order_acq_rel)) playhead_ = 0;
    const Source src = source_.load(std::memory_order_relaxed);

    if (src == Source::Stems) {
        if (loopFrames_ > 0) {
            renderStems(ctx.frames);
            for (std::uint32_t s = 0; s < kStems; ++s) {
                L[s] = stemData_.data() + (static_cast<std::size_t>(s) * 2) * maxFrames_;
                R[s] = L[s] + maxFrames_;
            }
        }   // nothing loaded: every stem is silent (nullptr)
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
    loopPub_.store(loopFrames_, std::memory_order_relaxed);

    for (std::uint32_t b = 0; b < dsp::StemMatrix::kBuses; ++b)
        for (std::uint32_t c = 0; c < 2; ++c) {
            const std::uint32_t out = 2 * b + c;
            if (out < ctx.numOutputs) dsp::copy(ctx.outputs[out], matrix_.busChannel(b, c), ctx.frames);
        }
}

} // namespace hc::engine
