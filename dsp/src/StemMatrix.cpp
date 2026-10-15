#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/dsp/Kernels.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::dsp {

namespace {
constexpr auto kRelaxed = std::memory_order_relaxed;
} // namespace

float StemMatrix::dbToGain(float db) noexcept {
    if (!(db > kOffDb)) return 0.0f;   // also catches NaN
    return std::pow(10.0f, db / 20.0f);
}

// Stereo balance with constant-power curve and no centre dip:
// centre = unity on both sides; hard left = L 1.0, R 0.0.
void StemMatrix::balanceGains(float balance, float& gL, float& gR) noexcept {
    const float b     = std::clamp(std::isfinite(balance) ? balance : 0.0f, -1.0f, 1.0f);
    const float theta = (b + 1.0f) * std::numbers::pi_v<float> / 4.0f;
    gL = std::min(1.0f, std::numbers::sqrt2_v<float> * std::cos(theta));
    gR = std::min(1.0f, std::numbers::sqrt2_v<float> * std::sin(theta));
    if (b <= -1.0f) gR = 0.0f;
    if (b >= 1.0f)  gL = 0.0f;
}

StemMatrix::StemMatrix() noexcept {
    for (auto& s : stems_) {
        for (auto& snd : s.send) snd.store(0.0f, kRelaxed);
        s.send[0].store(1.0f, kRelaxed);   // default: every stem feeds Main at unity
    }
}

void StemMatrix::prepare(std::uint32_t maxFrames) {
    maxFrames_ = maxFrames;
    busData_.allocate(static_cast<std::size_t>(kBuses) * 2 * maxFrames);
    for (auto& s : prevSend_) for (auto& b : s) b[0] = b[1] = 0.0f;
    for (std::uint32_t b = 0; b < kBuses; ++b)
        prevBus_[b] = buses_[b].mute.load(kRelaxed) ? 0.0f : buses_[b].gain.load(kRelaxed);
}

// ---- setters (message thread) ------------------------------------------------

void StemMatrix::setStemTrimDb(std::uint32_t s, float db) noexcept {
    if (s < kStems) stems_[s].trim.store(dbToGain(db), kRelaxed);
}
void StemMatrix::setStemBalance(std::uint32_t s, float balance) noexcept {
    if (s >= kStems) return;
    float l = 1.0f, r = 1.0f;
    balanceGains(balance, l, r);
    stems_[s].balL.store(l, kRelaxed);
    stems_[s].balR.store(r, kRelaxed);
}
void StemMatrix::setStemInvert(std::uint32_t s, bool v) noexcept { if (s < kStems) stems_[s].invert.store(v, kRelaxed); }
void StemMatrix::setStemMute(std::uint32_t s, bool v) noexcept   { if (s < kStems) stems_[s].mute.store(v, kRelaxed); }
void StemMatrix::setStemSolo(std::uint32_t s, bool v) noexcept   { if (s < kStems) stems_[s].solo.store(v, kRelaxed); }
void StemMatrix::setSendDb(std::uint32_t s, std::uint32_t b, float db) noexcept {
    if (s < kStems && b < kBuses) stems_[s].send[b].store(dbToGain(db), kRelaxed);
}
void StemMatrix::setBusGainDb(std::uint32_t b, float db) noexcept { if (b < kBuses) buses_[b].gain.store(dbToGain(db), kRelaxed); }
void StemMatrix::setBusMute(std::uint32_t b, bool v) noexcept      { if (b < kBuses) buses_[b].mute.store(v, kRelaxed); }

float StemMatrix::stemPeak(std::uint32_t s) const noexcept { return s < kStems ? stemMeters_[s].peak.load(kRelaxed) : 0.0f; }
float StemMatrix::stemRms(std::uint32_t s) const noexcept  { return s < kStems ? stemMeters_[s].rms.load(kRelaxed) : 0.0f; }
float StemMatrix::busPeak(std::uint32_t b, std::uint32_t c) const noexcept {
    return (b < kBuses && c < 2) ? busMeters_[b * 2 + c].peak.load(kRelaxed) : 0.0f;
}
float StemMatrix::busRms(std::uint32_t b, std::uint32_t c) const noexcept {
    return (b < kBuses && c < 2) ? busMeters_[b * 2 + c].rms.load(kRelaxed) : 0.0f;
}

// ---- RT ----------------------------------------------------------------------

void StemMatrix::process(const float* const* stemL, const float* const* stemR, std::uint32_t frames) noexcept {
    if (frames == 0 || frames > maxFrames_) return;

    float* bus = busData_.data();
    for (std::uint32_t c = 0; c < kBuses * 2; ++c) clear(bus + static_cast<std::size_t>(c) * maxFrames_, frames);

    bool anySolo = false;
    for (const auto& s : stems_) anySolo |= s.solo.load(kRelaxed);

    const float invFrames = 1.0f / static_cast<float>(frames);

    for (std::uint32_t s = 0; s < kStems; ++s) {
        const StemParams& p = stems_[s];
        const float* L = stemL ? stemL[s] : nullptr;
        const float* R = stemR ? stemR[s] : nullptr;
        if (L && !R) R = L;
        if (!L && R) L = R;

        const float trim = p.trim.load(kRelaxed);
        if (!L) {
            stemMeters_[s].peak.store(0.0f, kRelaxed);
            stemMeters_[s].rms.store(0.0f, kRelaxed);
            for (std::uint32_t b = 0; b < kBuses; ++b) prevSend_[s][b][0] = prevSend_[s][b][1] = 0.0f;
            continue;
        }

        // Stem meter: post-trim, pre-fader.
        const float  pk = std::max(peakAbs(L, frames), R == L ? 0.0f : peakAbs(R, frames)) * trim;
        const double ss = sumSquares(L, frames) + (R == L ? sumSquares(L, frames) : sumSquares(R, frames));
        stemMeters_[s].peak.store(pk, kRelaxed);
        stemMeters_[s].rms.store(static_cast<float>(std::sqrt(ss * 0.5 * invFrames)) * trim, kRelaxed);

        const bool  silenced = p.mute.load(kRelaxed) || (anySolo && !p.solo.load(kRelaxed));
        const float base     = silenced ? 0.0f : trim * (p.invert.load(kRelaxed) ? -1.0f : 1.0f);
        const float gL       = base * p.balL.load(kRelaxed);
        const float gR       = base * p.balR.load(kRelaxed);

        for (std::uint32_t b = 0; b < kBuses; ++b) {
            const float send = p.send[b].load(kRelaxed);
            const float tL   = gL * send;
            const float tR   = gR * send;
            float*      bl   = bus + (static_cast<std::size_t>(b) * 2) * maxFrames_;
            float*      br   = bl + maxFrames_;
            float&      pL   = prevSend_[s][b][0];
            float&      pR   = prevSend_[s][b][1];
            if (pL != 0.0f || tL != 0.0f) mixAddRamp(bl, L, pL, tL, frames);
            if (pR != 0.0f || tR != 0.0f) mixAddRamp(br, R, pR, tR, frames);
            pL = tL;
            pR = tR;
        }
    }

    for (std::uint32_t b = 0; b < kBuses; ++b) {
        const float target = buses_[b].mute.load(kRelaxed) ? 0.0f : buses_[b].gain.load(kRelaxed);
        for (std::uint32_t c = 0; c < 2; ++c) {
            float* ch = bus + (static_cast<std::size_t>(b) * 2 + c) * maxFrames_;
            if (prevBus_[b] != 1.0f || target != 1.0f) scaleRamp(ch, prevBus_[b], target, frames);
            busMeters_[b * 2 + c].peak.store(peakAbs(ch, frames), kRelaxed);
            busMeters_[b * 2 + c].rms.store(static_cast<float>(std::sqrt(sumSquares(ch, frames) * invFrames)), kRelaxed);
        }
        prevBus_[b] = target;
    }
}

} // namespace hc::dsp
