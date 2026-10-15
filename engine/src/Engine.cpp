#include "hybrid/engine/Engine.hpp"

#include <windows.h>
#include <immintrin.h>

#include <cmath>
#include <cstring>

namespace hc::engine {

namespace {

// Linear gain ramp g0 -> g1 across the block, AVX2.
void applyGainRamp(float* buf, std::uint32_t frames, float g0, float g1) noexcept {
    const float step = (g1 - g0) / static_cast<float>(frames);
    std::uint32_t i = 0;
    __m256       g   = _mm256_fmadd_ps(_mm256_setr_ps(0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f), _mm256_set1_ps(step),
                                       _mm256_set1_ps(g0));
    const __m256 inc = _mm256_set1_ps(step * 8.0f);
    for (; i + 8 <= frames; i += 8) {
        _mm256_storeu_ps(buf + i, _mm256_mul_ps(_mm256_loadu_ps(buf + i), g));
        g = _mm256_add_ps(g, inc);
    }
    for (; i < frames; ++i) buf[i] *= g0 + step * static_cast<float>(i);
}

void applyGain(float* buf, std::uint32_t frames, float g) noexcept {
    const __m256  vg = _mm256_set1_ps(g);
    std::uint32_t i  = 0;
    for (; i + 8 <= frames; i += 8) _mm256_storeu_ps(buf + i, _mm256_mul_ps(_mm256_loadu_ps(buf + i), vg));
    for (; i < frames; ++i) buf[i] *= g;
}

inline std::int64_t qpcNow() noexcept {
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
}

} // namespace

Engine::~Engine() {
    // Stream must be stopped/closed before the engine is destroyed.
    Command c;
    while (commands_.tryPop(c))
        if (c.type == Command::Type::Swap) delete c.proc;
    delete active_;
    active_ = nullptr;
    collectGarbage();
}

// ---- message thread --------------------------------------------------------

bool Engine::install(std::unique_ptr<AudioProcessor>& processor) {
    if (!processor || !prepared_) return false;

    if (swapsInFlight_.load(std::memory_order_acquire) >= kRetireCapacity - 1) {
        collectGarbage();
        if (swapsInFlight_.load(std::memory_order_acquire) >= kRetireCapacity - 1) return false;
    }

    processor->prepare(config_);

    swapsInFlight_.fetch_add(1, std::memory_order_acq_rel);
    if (!commands_.tryPush(Command{Command::Type::Swap, processor.get(), 0.0f})) {
        swapsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    (void)processor.release();
    return true;
}

bool Engine::setMasterGain(float linear) noexcept {
    if (!std::isfinite(linear) || linear < 0.0f) return false;
    return commands_.tryPush(Command{Command::Type::Gain, nullptr, linear});
}

bool Engine::setMuted(bool muted) noexcept {
    return commands_.tryPush(Command{Command::Type::Mute, nullptr, muted ? 1.0f : 0.0f});
}

void Engine::collectGarbage() noexcept {
    AudioProcessor* p = nullptr;
    while (retired_.tryPop(p)) {
        delete p;
        swapsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool Engine::popTelemetry(Telemetry& out) noexcept { return telemetry_.tryPop(out); }

void Engine::prepare(const io::StreamConfig& config) {
    config_ = config;

    // Stream is stopped: this thread may act as the command consumer.
    Command c;
    while (commands_.tryPop(c)) applyCommand(c);
    if (active_) active_->prepare(config_);

    gainCurrent_ = muted_ ? 0.0f : gainTarget_;
    sampleTime_  = 0;
    loadSum_     = 0.0;
    loadPeak_    = 0.0f;
    windowBlocks_ = 0;
    overruns_     = 0;

    const std::uint32_t perSecond = config.blockFrames ? config.sampleRate / config.blockFrames : 1;
    blocksPerReport_ = perSecond / 20 ? perSecond / 20 : 1;

    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    const double periodSeconds = config.sampleRate ? static_cast<double>(config.blockFrames) / config.sampleRate : 1.0;
    ticksToLoad_ = 1.0 / (static_cast<double>(freq.QuadPart) * periodSeconds);

    prepared_ = true;
}

// ---- RT thread --------------------------------------------------------------

void Engine::applyCommand(const Command& c) noexcept {
    switch (c.type) {
    case Command::Type::Swap:
        // Always retire (even nullptr) so swapsInFlight_ stays balanced.
        // Capacity is guaranteed by the install() in-flight limit.
        (void)retired_.tryPush(active_);
        active_ = c.proc;
        break;
    case Command::Type::Gain:
        gainTarget_ = c.value;
        break;
    case Command::Type::Mute:
        muted_ = c.value != 0.0f;
        break;
    }
}

void Engine::process(const float* const* inputs, std::uint32_t numInputs,
                     float* const* outputs, std::uint32_t numOutputs,
                     std::uint32_t frames) noexcept {
    const std::int64_t t0 = qpcNow();

    Command c;
    for (std::uint32_t n = 0; n < kMaxCommandsPerBlock && commands_.tryPop(c); ++n) applyCommand(c);

    for (std::uint32_t ch = 0; ch < numOutputs; ++ch) std::memset(outputs[ch], 0, frames * sizeof(float));

    if (active_) {
        const ProcessContext ctx{inputs, numInputs, outputs, numOutputs, frames, sampleTime_};
        active_->process(ctx);
    }

    const float target = muted_ ? 0.0f : gainTarget_;
    if (target != gainCurrent_) {
        for (std::uint32_t ch = 0; ch < numOutputs; ++ch) applyGainRamp(outputs[ch], frames, gainCurrent_, target);
        gainCurrent_ = target;
    } else if (gainCurrent_ != 1.0f) {
        for (std::uint32_t ch = 0; ch < numOutputs; ++ch) applyGain(outputs[ch], frames, gainCurrent_);
    }

    sampleTime_ += frames;

    const float load = static_cast<float>(static_cast<double>(qpcNow() - t0) * ticksToLoad_);
    loadSum_ += load;
    if (load > loadPeak_) loadPeak_ = load;
    if (load >= 1.0f) ++overruns_;

    if (++windowBlocks_ >= blocksPerReport_) {
        Telemetry t{};
        t.sampleTime = sampleTime_;
        t.loadAvg    = static_cast<float>(loadSum_ / windowBlocks_);
        t.loadPeak   = loadPeak_;
        t.overruns   = overruns_;
        t.masterGain = gainCurrent_;
        (void)telemetry_.tryPush(t);   // dropped if the UI is not draining — never blocks
        loadSum_      = 0.0;
        loadPeak_     = 0.0f;
        windowBlocks_ = 0;
    }
}

} // namespace hc::engine
