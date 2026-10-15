#include "hybrid/io/AsioDuplex.hpp"
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/Config.hpp"
#include "hybrid/core/RealtimeThread.hpp"

#include "SampleConvert.hpp"

#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// Provided by the Steinberg SDK (host/asiodrivers.cpp).
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace hc::io {

namespace {
constexpr long kErrAlreadyOpen = -1000;
constexpr long kErrBadType     = -1001;
constexpr long kErrNoChannels  = -1002;
constexpr long kErrNameConvert = -1003;
} // namespace

struct AsioDuplex::Impl {
    std::uint32_t numIn  = 0;
    std::uint32_t numOut = 0;
    std::uint32_t frames = 0;
    bool          postOutput = false;
    bool          buffersCreated = false;
    bool          driverLoaded   = false;

    std::vector<ASIOBufferInfo> infos;     // inputs first, then outputs
    std::vector<ASIOSampleType> types;     // parallel to infos

    core::AlignedBuffer<float>           inScratch;
    core::AlignedBuffer<float>           outScratch;
    std::array<float*, hc::kMaxChannels> inPtrs{};
    std::array<float*, hc::kMaxChannels> outPtrs{};

    AudioCallback*             callback = nullptr;
    std::atomic<bool>          running{false};
    std::atomic<std::uint32_t> events{0};

    void raise(std::uint32_t flag) noexcept { events.fetch_or(flag, std::memory_order_relaxed); }

    static void toFloat(ASIOSampleType type, const void* src, float* dst, std::size_t n) noexcept {
        switch (type) {
        case ASIOSTInt16LSB:   convert::s16ToFloat(static_cast<const std::int16_t*>(src), dst, n); break;
        case ASIOSTInt24LSB:   convert::s24PackedToFloat(static_cast<const std::uint8_t*>(src), dst, n); break;
        case ASIOSTInt32LSB:   convert::s32ToFloat(static_cast<const std::int32_t*>(src), dst, n); break;
        case ASIOSTFloat32LSB: std::memcpy(dst, src, n * sizeof(float)); break;
        case ASIOSTFloat64LSB: convert::f64ToFloat(static_cast<const double*>(src), dst, n); break;
        default:               std::memset(dst, 0, n * sizeof(float)); break;
        }
    }

    static void fromFloat(ASIOSampleType type, const float* src, void* dst, std::size_t n) noexcept {
        switch (type) {
        case ASIOSTInt16LSB:   convert::floatToS16(src, static_cast<std::int16_t*>(dst), n); break;
        case ASIOSTInt24LSB:   convert::floatToS24Packed(src, static_cast<std::uint8_t*>(dst), n); break;
        case ASIOSTInt32LSB:   convert::floatToS32(src, static_cast<std::int32_t*>(dst), n); break;
        case ASIOSTFloat32LSB: std::memcpy(dst, src, n * sizeof(float)); break;
        case ASIOSTFloat64LSB: convert::floatToF64(src, static_cast<double*>(dst), n); break;
        default:               break;
        }
    }

    static bool supported(ASIOSampleType type) noexcept {
        return type == ASIOSTInt16LSB || type == ASIOSTInt24LSB || type == ASIOSTInt32LSB ||
               type == ASIOSTFloat32LSB || type == ASIOSTFloat64LSB;
    }

    // ---- Driver callback thread -------------------------------------------
    void processBlock(long index) noexcept {
        rt::enableFlushToZero();   // driver thread: cheap MXCSR write, idempotent

        for (std::uint32_t c = 0; c < numIn; ++c)
            toFloat(types[c], infos[c].buffers[index], inPtrs[c], frames);

        callback->process(inPtrs.data(), numIn, outPtrs.data(), numOut, frames);

        for (std::uint32_t c = 0; c < numOut; ++c)
            fromFloat(types[numIn + c], outPtrs[c], infos[numIn + c].buffers[index], frames);

        if (postOutput) ASIOOutputReady();
    }
};

namespace {

std::atomic<AsioDuplex::Impl*> gActive{nullptr};

void onBufferSwitch(long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed))
        p->processBlock(index);
}

ASIOTime* onBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed))
        p->processBlock(index);
    return time;
}

void onSampleRateChanged(ASIOSampleRate) {
    if (auto* p = gActive.load(std::memory_order_acquire)) p->raise(StreamEvent::SampleRateMoved);
}

long onAsioMessage(long selector, long value, void*, double*) {
    auto* p = gActive.load(std::memory_order_acquire);
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
                value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioOverload)
                   ? 1L : 0L;
    case kAsioEngineVersion:    return 2L;
    case kAsioResetRequest:     if (p) p->raise(StreamEvent::ResetRequest);   return 1L;
    case kAsioResyncRequest:    if (p) p->raise(StreamEvent::ResyncRequest);  return 1L;
    case kAsioLatenciesChanged: if (p) p->raise(StreamEvent::LatencyChanged); return 1L;
    case kAsioOverload:         if (p) p->raise(StreamEvent::Overload);       return 1L;
    case kAsioSupportsTimeInfo: return 1L;
    case kAsioSupportsTimeCode: return 0L;
    default:                    return 0L;
    }
}

ASIOCallbacks gCallbacks{&onBufferSwitch, &onSampleRateChanged, &onAsioMessage, &onBufferSwitchTimeInfo};

bool narrowName(const std::wstring& wide, std::vector<char>& out) {
    if (wide.empty()) return false;
    const int bytes = ::WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    out.assign(static_cast<std::size_t>(bytes), '\0');
    return ::WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), bytes, nullptr, nullptr) > 0;
}

} // namespace

AsioDuplex::AsioDuplex() : impl_(std::make_unique<Impl>()) {}
AsioDuplex::~AsioDuplex() { close(); }

std::expected<StreamConfig, BackendError> AsioDuplex::open(const Options& options, AudioCallback& callback) {
    close();
    Impl& s = *impl_;

    Impl* expected = nullptr;
    if (!gActive.compare_exchange_strong(expected, &s, std::memory_order_acq_rel))
        return std::unexpected(BackendError{kErrAlreadyOpen, "another ASIO stream is open"});

    auto fail = [this](long code, const char* stage) -> std::unexpected<BackendError> {
        close();
        return std::unexpected(BackendError{code, stage});
    };

    std::vector<char> name;
    if (!narrowName(options.driverName, name)) return fail(kErrNameConvert, "driver name");
    if (!loadAsioDriver(name.data())) return fail(ASE_NotPresent, "loadAsioDriver");
    s.driverLoaded = true;

    ASIODriverInfo info{};
    info.asioVersion = 2;
    HWND owner       = ::GetConsoleWindow();
    info.sysRef      = owner ? owner : ::GetDesktopWindow();
    ASIOError e      = ASIOInit(&info);
    if (e != ASE_OK) return fail(e, "ASIOInit");

    if ((e = ASIOCanSampleRate(static_cast<ASIOSampleRate>(hc::kSampleRate))) != ASE_OK)
        return fail(e, "ASIOCanSampleRate(kSampleRate)");
    ASIOSampleRate current = 0;
    if (ASIOGetSampleRate(&current) != ASE_OK || current != static_cast<ASIOSampleRate>(hc::kSampleRate)) {
        if ((e = ASIOSetSampleRate(static_cast<ASIOSampleRate>(hc::kSampleRate))) != ASE_OK)
            return fail(e, "ASIOSetSampleRate(kSampleRate)");
    }

    long hwIn = 0, hwOut = 0;
    if ((e = ASIOGetChannels(&hwIn, &hwOut)) != ASE_OK) return fail(e, "ASIOGetChannels");

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if ((e = ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity)) != ASE_OK)
        return fail(e, "ASIOGetBufferSize");

    long size = preferred;
    if (options.bufferFrames != 0) {
        const auto req = static_cast<long>(options.bufferFrames);
        const bool inRange = req >= minSize && req <= maxSize;
        const bool fitsGranularity =
            granularity == -1 ? ((req & (req - 1)) == 0)
                              : (granularity <= 0 || ((req - minSize) % granularity) == 0);
        if (inRange && fitsGranularity) size = req;
    }
    if (size <= 0 || size > static_cast<long>(hc::kMaxBlockFrames))
        return fail(ASE_InvalidMode, "buffer size outside kMaxBlockFrames");

    const auto cap = [](long hw, std::uint32_t limit) {
        long v = hw < static_cast<long>(limit) ? hw : static_cast<long>(limit);
        if (v > static_cast<long>(hc::kMaxChannels)) v = static_cast<long>(hc::kMaxChannels);
        return static_cast<std::uint32_t>(v < 0 ? 0 : v);
    };
    s.numIn  = cap(hwIn, options.maxInputs);
    s.numOut = cap(hwOut, options.maxOutputs);
    if (s.numOut == 0 && s.numIn == 0) return fail(kErrNoChannels, "driver exposes no channels");

    const std::uint32_t total = s.numIn + s.numOut;
    s.infos.assign(total, ASIOBufferInfo{});
    for (std::uint32_t c = 0; c < total; ++c) {
        s.infos[c].isInput    = c < s.numIn ? ASIOTrue : ASIOFalse;
        s.infos[c].channelNum = static_cast<long>(c < s.numIn ? c : c - s.numIn);
    }

    if ((e = ASIOCreateBuffers(s.infos.data(), static_cast<long>(total), size, &gCallbacks)) != ASE_OK)
        return fail(e, "ASIOCreateBuffers");
    s.buffersCreated = true;

    s.types.assign(total, ASIOSTInt32LSB);
    for (std::uint32_t c = 0; c < total; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = s.infos[c].channelNum;
        ci.isInput = s.infos[c].isInput;
        if ((e = ASIOGetChannelInfo(&ci)) != ASE_OK) return fail(e, "ASIOGetChannelInfo");
        if (!Impl::supported(ci.type)) return fail(kErrBadType, "unsupported ASIO sample type");
        s.types[c] = ci.type;
    }

    s.frames     = static_cast<std::uint32_t>(size);
    s.postOutput = (ASIOOutputReady() == ASE_OK);

    s.inScratch.allocate(static_cast<std::size_t>(s.numIn) * s.frames);
    s.outScratch.allocate(static_cast<std::size_t>(s.numOut) * s.frames);
    for (std::uint32_t c = 0; c < s.numIn; ++c)  s.inPtrs[c]  = s.inScratch.data() + c * s.frames;
    for (std::uint32_t c = 0; c < s.numOut; ++c) s.outPtrs[c] = s.outScratch.data() + c * s.frames;

    long inLat = 0, outLat = 0;
    ASIOGetLatencies(&inLat, &outLat);

    s.callback = &callback;

    StreamConfig cfg{};
    cfg.sampleRate      = hc::kSampleRate;
    cfg.blockFrames     = s.frames;
    cfg.numInputs       = s.numIn;
    cfg.numOutputs      = s.numOut;
    cfg.inputLatencyMs  = 1000.0 * inLat / hc::kSampleRate;
    cfg.outputLatencyMs = 1000.0 * outLat / hc::kSampleRate;

    callback.prepare(cfg);
    return cfg;
}

std::expected<void, BackendError> AsioDuplex::start() {
    Impl& s = *impl_;
    if (!s.buffersCreated || !s.callback) return std::unexpected(BackendError{ASE_NotPresent, "start before open"});
    if (s.running.load(std::memory_order_acquire)) return {};

    s.events.store(0, std::memory_order_relaxed);
    s.running.store(true, std::memory_order_release);
    if (const ASIOError e = ASIOStart(); e != ASE_OK) {
        s.running.store(false, std::memory_order_release);
        return std::unexpected(BackendError{e, "ASIOStart"});
    }
    return {};
}

void AsioDuplex::stop() noexcept {
    Impl& s = *impl_;
    if (s.running.exchange(false, std::memory_order_acq_rel)) ASIOStop();
}

void AsioDuplex::close() noexcept {
    stop();
    Impl& s = *impl_;
    if (s.buffersCreated) { ASIODisposeBuffers(); s.buffersCreated = false; }
    if (s.driverLoaded)   { ASIOExit();           s.driverLoaded   = false; }

    Impl* self = &s;
    gActive.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);

    s.callback = nullptr;
    s.numIn = s.numOut = s.frames = 0;
    s.infos.clear();
    s.types.clear();
    s.inPtrs.fill(nullptr);
    s.outPtrs.fill(nullptr);
}

std::uint32_t AsioDuplex::takeEvents() noexcept { return impl_->events.exchange(0, std::memory_order_acq_rel); }
bool AsioDuplex::isRunning() const noexcept { return impl_->running.load(std::memory_order_acquire); }

} // namespace hc::io
