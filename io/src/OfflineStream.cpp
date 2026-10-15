#include "hybrid/io/OfflineStream.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace hc::io {

OfflineStream::~OfflineStream() { stop(); }

StreamConfig OfflineStream::open(const Options& o, AudioCallback& callback) {
    stop();
    callback_ = &callback;
    config_ = {};
    config_.sampleRate      = o.sampleRate;
    config_.blockFrames     = std::clamp<std::uint32_t>(o.blockFrames, 16, hc::kMaxBlockFrames);
    config_.numInputs       = std::min<std::uint32_t>(o.numInputs, hc::kMaxChannels);
    config_.numOutputs      = std::min<std::uint32_t>(o.numOutputs, hc::kMaxChannels);
    config_.inputLatencyMs  = 0.0;
    config_.outputLatencyMs = 0.0;
    in_.clear();
    out_.clear();
    inPtrs_.clear();
    outPtrs_.clear();
    for (std::uint32_t c = 0; c < config_.numInputs; ++c) {
        in_.emplace_back(config_.blockFrames);
        std::memset(in_.back().data(), 0, sizeof(float) * config_.blockFrames);
        inPtrs_.push_back(in_.back().data());
    }
    for (std::uint32_t c = 0; c < config_.numOutputs; ++c) {
        out_.emplace_back(config_.blockFrames);
        outPtrs_.push_back(out_.back().data());
    }
    callback.prepare(config_);
    return config_;
}

void OfflineStream::start() {
    if (!callback_ || thread_.joinable()) return;
    quit_.store(false, std::memory_order_relaxed);
    blocks_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
}

void OfflineStream::stop() noexcept {
    if (!thread_.joinable()) return;
    quit_.store(true, std::memory_order_release);
    thread_.join();
}

void OfflineStream::run() noexcept {
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, next;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&next);
    const double periodTicks = static_cast<double>(freq.QuadPart) * config_.blockFrames / config_.sampleRate;
    double due = static_cast<double>(next.QuadPart);
    while (!quit_.load(std::memory_order_acquire)) {
        callback_->process(inPtrs_.data(), config_.numInputs, outPtrs_.data(), config_.numOutputs, config_.blockFrames);
        blocks_.fetch_add(1, std::memory_order_relaxed);
        due += periodTicks;
        LARGE_INTEGER now;
        ::QueryPerformanceCounter(&now);
        const double waitTicks = due - static_cast<double>(now.QuadPart);
        if (waitTicks <= 0) { due = static_cast<double>(now.QuadPart); continue; }   // fell behind: don't burst
        if (timer) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>(waitTicks * 1.0e7 / static_cast<double>(freq.QuadPart));   // 100 ns units, relative
            ::SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
            ::WaitForSingleObject(timer, INFINITE);
        } else {
            ::Sleep(static_cast<DWORD>(waitTicks * 1000.0 / static_cast<double>(freq.QuadPart)));
        }
    }
    if (timer) ::CloseHandle(timer);
}

} // namespace hc::io
