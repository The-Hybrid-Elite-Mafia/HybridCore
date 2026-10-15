#pragma once
// =============================================================================
//  OfflineStream — a clock instead of a sound card.
//
//  Calls an AudioCallback from its own thread at real-time pace (one block every
//  blockFrames / sampleRate seconds, high-resolution waitable timer), with silent
//  inputs and discarded outputs. Used where no audio device exists (GitHub's CI
//  machines) and for headless UI renders, so the engine, telemetry and meters run
//  exactly as they do on hardware. Nothing is audible.
//
//  Threads: open/start/stop on the message thread; the callback runs on the clock thread.
// =============================================================================
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/Config.hpp"
#include "hybrid/io/AudioCallback.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace hc::io {

class OfflineStream {
public:
    struct Options {
        std::uint32_t sampleRate  = hc::kSampleRate;
        std::uint32_t blockFrames = 480;   // 10 ms at 48 kHz
        std::uint32_t numInputs   = 0;
        std::uint32_t numOutputs  = 2 * hc::kBusCount;
    };

    OfflineStream() = default;
    ~OfflineStream();
    OfflineStream(const OfflineStream&)            = delete;
    OfflineStream& operator=(const OfflineStream&) = delete;

    // Allocates buffers and calls callback.prepare(). Stream must be stopped.
    [[nodiscard]] StreamConfig open(const Options& options, AudioCallback& callback);
    void start();
    void stop() noexcept;
    void close() noexcept { stop(); callback_ = nullptr; }
    [[nodiscard]] std::uint64_t blocks() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

private:
    void run() noexcept;

    AudioCallback*                          callback_ = nullptr;
    StreamConfig                            config_{};
    std::vector<core::AlignedBuffer<float>> in_, out_;
    std::vector<const float*>               inPtrs_;
    std::vector<float*>                     outPtrs_;
    std::thread                             thread_;
    std::atomic<bool>                       quit_{false};
    std::atomic<std::uint64_t>              blocks_{0};
};

} // namespace hc::io
