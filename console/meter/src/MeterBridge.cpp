// HEM METER BRIDGE — implementation (ITU-R BS.1770-4, EBU R128 / Tech 3341 / Tech 3342) and the reference meter's
// EDIT-menu settings (source MTR-UM; numbers it does not give: ConsoleSpecs.hpp, namespace meter, ASSUMED).
#include "hybrid/console/MeterBridge.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace hc::console {
namespace {

namespace S = spec::meter;
using namespace meter_ids;
using fx::ParamInfo;
using fx::Taper;

struct Bq {   // double-precision biquad for measurement filters
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) noexcept { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void clear() noexcept { z1 = z2 = 0; }
};

// K-weighting (BS.1770-4): pre-filter (high shelf) + RLB high-pass, designed for any rate.
void kWeighting(double fs, Bq& shelf, Bq& rlb) noexcept {
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(std::numbers::pi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0; shelf.b1 = 2.0 * (K * K - Vh) / a0; shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0; shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(std::numbers::pi * f0 / fs), a0 = 1.0 + K / Q + K * K;
        rlb.b0 = 1.0; rlb.b1 = -2.0; rlb.b2 = 1.0;
        rlb.a1 = 2.0 * (K * K - 1.0) / a0; rlb.a2 = (1.0 - K / Q + K * K) / a0;
    }
}
[[nodiscard]] inline double lufs(double meanSquareSum) noexcept { return meanSquareSum > 0 ? -0.691 + 10.0 * std::log10(meanSquareSum) : MeterBridge::kSilent; }
[[nodiscard]] inline double dbOf(double v) noexcept { return v > 0 ? 20.0 * std::log10(v) : MeterBridge::kSilent; }

// Loudness histogram: fixed bins, exact energy sums per bin (no allocation after construction, any duration).
struct Histogram {
    static constexpr int kBins = static_cast<int>((S::kHistogramTopLufs - S::kAbsGateLufs) / S::kHistogramStepLu) + 1;
    std::vector<double> energy = std::vector<double>(kBins, 0.0);
    std::vector<std::uint64_t> count = std::vector<std::uint64_t>(kBins, 0);
    void clear() noexcept { std::fill(energy.begin(), energy.end(), 0.0); std::fill(count.begin(), count.end(), 0); }
    static int binOf(double l) noexcept { return std::clamp(static_cast<int>(std::floor((l - S::kAbsGateLufs) / S::kHistogramStepLu)), 0, kBins - 1); }
    static double binLufs(int b) noexcept { return S::kAbsGateLufs + (b + 0.5) * S::kHistogramStepLu; }
    void add(double e) noexcept {
        const double l = lufs(e);
        if (l < S::kAbsGateLufs) return;
        const int b = binOf(l);
        energy[static_cast<std::size_t>(b)] += e;
        ++count[static_cast<std::size_t>(b)];
    }
    // Mean energy of blocks at or above loudness `gate`.
    [[nodiscard]] double meanAbove(double gate, std::uint64_t& n) const noexcept {
        const int from = std::max(0, static_cast<int>(std::ceil((gate - S::kAbsGateLufs) / S::kHistogramStepLu - 1e-9)));
        double e = 0; n = 0;
        for (int b = from; b < kBins; ++b) { e += energy[static_cast<std::size_t>(b)]; n += count[static_cast<std::size_t>(b)]; }
        return n ? e / static_cast<double>(n) : 0.0;
    }
    // BS.1770 gated loudness (absolute -70, relative -10 LU).
    [[nodiscard]] double gated() const noexcept {
        std::uint64_t n = 0;
        const double absMean = meanAbove(S::kAbsGateLufs, n);
        if (!n) return MeterBridge::kSilent;
        std::uint64_t m = 0;
        const double rel = meanAbove(lufs(absMean) + S::kRelGateLu, m);
        return m ? lufs(rel) : MeterBridge::kSilent;
    }
};

struct AtomicD {
    std::atomic<double> v{MeterBridge::kSilent};
    void set(double x) noexcept { v.store(x, std::memory_order_relaxed); }
    [[nodiscard]] double get() const noexcept { return v.load(std::memory_order_relaxed); }
};

// ---- the settings table ---------------------------------------------------------------------------
template <std::size_t N>
struct Names {
    std::array<std::string, N> s;
    std::array<const char*, N> p{};
    template <typename F> explicit Names(F f) { for (std::size_t i = 0; i < N; ++i) { s[i] = f(i); p[i] = s[i].c_str(); } }
};
const char* const kOnOff[] = {"OFF", "ON"};
const char* const kRunN[] = {"PAUSED", "RUNNING"};
const char* const kViewN[] = {"RADAR", "RTA", "SCOPE"};
const Names<12> kSpeedN([](std::size_t i) {
    const double s = S::kRadarSpeedS[i];
    char b[24];
    if (s < 3600) std::snprintf(b, sizeof b, "%.0f min", s / 60.0); else std::snprintf(b, sizeof b, "%.0f h", s / 3600.0);
    return std::string(b);
});
const Names<10> kRadarResN([](std::size_t i) { return std::to_string(3 + i) + " dB"; });
const Names<31> kTargetN([](std::size_t i) { return std::to_string(-36 + static_cast<int>(i)) + " LUFS"; });
const Names<13> kLoudAlertN([](std::size_t i) { return i == 0 ? std::string("OFF") : std::to_string(i) + " LU"; });
const char* const kMomRangeN[] = {"EBU +9", "EBU +18"};
const char* const kRtaChN[] = {"L", "R", "L+R"};
const char* const kRtaBandsN[] = {"10 BANDS (1/1 OCT)", "31 BANDS (1/3 OCT)"};
const char* const kRtaTimeN[] = {"SLOW", "FAST", "IMPULSE"};
const char* const kRtaHoldN[] = {"OFF", "0.5 s", "1 s", "2 s", "3 s", "5 s", "10 s", "HOLD"};
const char* const kRtaRangeN[] = {"10 dB", "20 dB", "30 dB", "40 dB", "50 dB", "60 dB"};
const Names<13> kRtaTopN([](std::size_t i) { return i == 0 ? std::string("0 dBFS") : "-" + std::to_string(5 * i) + " dBFS"; });
const char* const kRtaAutoN[] = {"RANGE & TOP", "TOP"};
const Names<MeterBridge::kBands> kCursorN([](std::size_t i) {
    const double hz = MeterBridge::bandHz(static_cast<int>(i));
    char b[24];
    if (hz >= 1000) std::snprintf(b, sizeof b, "%.3g kHz", hz / 1000.0); else std::snprintf(b, sizeof b, "%.3g Hz", hz);
    return std::string(b);
});
const char* const kPersistN[] = {"OFF (FAST)", "SHORT", "MEDIUM", "LONG"};
const Names<27> kScopeScaleN([](std::size_t i) {
    if (i == 25) return std::string("AUTO FAST");
    if (i == 26) return std::string("AUTO SLOW");
    return i == 0 ? std::string("0 dB") : "-" + std::to_string(i) + " dB";
});
const char* const kStyleN[] = {"PALE BLUE", "GREEN", "AMBER", "WHITE"};
const char* const kLayoutN[] = {"NORMAL", "FULL LIST", "BASIC", "LIVE PRODUCTION", "SINGLE SLIDING", "SINGLE PROGRAM", "CUSTOM"};
const char* const kLmaxN[] = {"MOMENTARY (M)", "SHORT-TERM (S)"};
const char* const kUnitN[] = {"LUFS", "LKFS", "LU"};
const char* const kDmxN[] = {"SUM", "-3 dB"};
const char* const kSpanN[] = {"SHORT-TERM (S)", "PROGRAM (I)"};
const Names<26> kPeakAlertN([](std::size_t i) {
    if (i == 0) return std::string("OFF");
    char b[24];
    std::snprintf(b, sizeof b, "%.1f dBTP", -12.0 + 0.5 * static_cast<double>(i - 1));
    return std::string(b);
});
const char* const kPeakHoldN[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "HOLD"};
const char* const kScaleN[] = {"TRUE-PEAK 60 dB", "TRUE-PEAK 30 dB", "TRUE-PEAK 20 dB", "DIGITAL PPM", "DIN", "NORDIC", "BBC", "EBU"};
const char* const kAlignN[] = {"-18 dBFS (EBU)", "-20 dBFS (SMPTE)"};
const char* const kDescN[] = {"--", "PROGRAM LOUDNESS", "TRUE-PEAK MAX", "LOUDNESS MAX", "LOUDNESS RANGE", "PEAK TO LOUDNESS",
                              "SLIDING 10 SEC", "SHORT-TERM", "MOMENTARY", "CORRELATION", "MONO DEVIATION", "BALANCE-O-METER"};

std::array<ParamInfo, kCount> makeParams() {
    std::array<ParamInfo, kCount> t{};
    auto choice = [&](int i, const char* id, const char* name, int n, int def, const char* const* names) {
        t[static_cast<std::size_t>(i)] = {id, name, "", 0, static_cast<float>(n - 1), static_cast<float>(def), Taper::Choice, names};
    };
    auto tog = [&](int i, const char* id, const char* name, int def) { t[static_cast<std::size_t>(i)] = {id, name, "", 0, 1, static_cast<float>(def), Taper::Toggle, kOnOff}; };
    t[Run] = {"run", "Run / Pause", "", 0, 1, 1, Taper::Toggle, kRunN};
    choice(View, "view", "View", 3, ViewRadar, kViewN);
    choice(RadarSpeed, "radar_speed", "Radar Speed", 12, S::kRadarSpeedDefault, kSpeedN.p.data());
    choice(RadarRes, "radar_res", "Radar Resolution", 10, static_cast<int>(S::kRadarResDefault - S::kRadarResMin), kRadarResN.p.data());
    choice(Target, "target", "Target Loudness", 31, static_cast<int>(S::kTargetDefault - S::kTargetMin), kTargetN.p.data());
    choice(LoudAlert, "loud_alert", "Loud Alert", 13, static_cast<int>(S::kLoudAlertDefault), kLoudAlertN.p.data());
    choice(MomRange, "mom_range", "Momentary Range", 2, S::kMomRangeDefault, kMomRangeN);
    choice(RtaChannels, "rta_ch", "RTA Channels", 3, 2, kRtaChN);
    choice(RtaBands, "rta_res", "RTA Resolution", 2, 1, kRtaBandsN);
    choice(RtaTime, "rta_time", "RTA Integration Time", 3, 1, kRtaTimeN);
    choice(RtaHold, "rta_hold", "RTA Peak Hold", 8, S::kRtaHoldDefault, kRtaHoldN);
    choice(RtaRange, "rta_range", "RTA Scale Range", 6, 5, kRtaRangeN);
    choice(RtaTop, "rta_top", "RTA Scale Top", 13, 0, kRtaTopN.p.data());
    choice(RtaAuto, "rta_auto", "RTA Auto Scale", 2, 0, kRtaAutoN);
    choice(RtaCursor, "rta_cursor", "RTA Cursor", MeterBridge::kBands, 17, kCursorN.p.data());
    choice(ScopePersist, "scope_persist", "Scope Persistence", 4, 0, kPersistN);
    choice(ScopeScale, "scope_scale", "Scope Scale", 27, 25, kScopeScaleN.p.data());
    choice(ScopeStyle, "scope_style", "Scope Style", 4, 0, kStyleN);
    choice(Layout, "layout", "Screen Layout", 7, 1, kLayoutN);
    choice(LoudMaxMetric, "lmax_metric", "Loudness Max Metric", 2, 1, kLmaxN);
    choice(Unit, "unit", "Loudness Unit", 3, 0, kUnitN);
    choice(MonoDownmix, "mono_dmx", "Stereo-Mono Downmix", 2, 1, kDmxN);
    choice(DevSpan, "dev_span", "Deviation Span", 2, 1, kSpanN);
    choice(PeakAlert, "peak_alert", "Peak Alert", 26, static_cast<int>((S::kPeakAlertDefault - S::kPeakAlertMin) / 0.5f) + 1, kPeakAlertN.p.data());
    choice(PeakHold, "peak_hold", "Peak Alert Hold", 6, S::kPeakHoldDefault, kPeakHoldN);
    choice(PeakScale, "peak_scale", "Peak Meter Scale", kScaleCount, ScaleTp60, kScaleN);
    choice(PpmAlign, "ppm_align", "PPM Alignment Level", 2, 0, kAlignN);
    tog(TpFull, "tp_full", "True-Peak Meter Full Height", 0);
    const char* slotIds[] = {"slot1", "slot2", "slot3", "slot4", "slot5", "slot6"};
    const char* slotNames[] = {"Custom Slot 1", "Custom Slot 2", "Custom Slot 3", "Custom Slot 4", "Custom Slot 5", "Custom Slot 6"};
    const int slotDef[] = {DescProgram, DescTruePeakMax, DescLoudMax, DescRange, DescPlr, DescSliding10};
    for (int k = 0; k < 6; ++k) choice(Slot1 + k, slotIds[k], slotNames[k], kDescCount, slotDef[k], kDescN);
    tog(FollowTransport, "follow", "Follow Transport", 0);
    tog(ResetOnPlay, "reset_on_play", "Reset On Play", 0);
    return t;
}
const std::array<ParamInfo, kCount> kParams = makeParams();

const std::array<std::array<int, 6>, 6> kLayouts{{
    {DescProgram, DescTruePeakMax, DescShortTerm, DescMomentary, DescNone, DescSliding10},   // NORMAL ("Normal" ASSUMED S and M)
    {DescProgram, DescTruePeakMax, DescLoudMax, DescRange, DescPlr, DescSliding10},          // FULL LIST
    {DescProgram, DescTruePeakMax, DescLoudMax, DescNone, DescNone, DescNone},                // BASIC
    {DescSliding10, DescNone, DescNone, DescNone, DescProgram, DescRange},                    // LIVE PRODUCTION
    {DescSliding10, DescNone, DescNone, DescNone, DescNone, DescNone},                        // SINGLE SLIDING
    {DescProgram, DescNone, DescNone, DescNone, DescNone, DescNone},                          // SINGLE PROGRAM
}};

const S::PpmBallistics& ballistics(int scale) noexcept {
    switch (scale) {
    case ScaleDigitalPpm: return S::kDigitalPpm;
    case ScaleDin: return S::kDinPpm;
    case ScaleNordic: return S::kNordicPpm;
    case ScaleBbc: return S::kBbcPpm;
    case ScaleEbu: return S::kEbuPpm;
    default: return S::kTpBallistics;
    }
}

} // namespace

struct MeterBridge::Impl {
    double fs = 48000;
    std::uint32_t maxBlock = 512;
    std::array<std::atomic<float>, kCount> vals{};
    std::atomic<bool> playing{false};
    // loudness
    Bq kShelf[2], kRlb[2];
    std::size_t stepLen = 4800, stepFill = 0;
    double stepE[4] = {0, 0, 0, 0};   // this 100 ms step: L, R (K-weighted mean square sums), mid, side
    static constexpr int kSteps = 30;
    std::array<std::array<double, 4>, kSteps> steps{};   // last 3 s of 100 ms steps
    int stepPos = 0, stepsFilled = 0;
    Histogram integ, lra, integMono;
    double maxM = kSilent, maxS = kSilent;
    static constexpr int kSliding = 100;                  // 10 s of momentary values (one per 100 ms step)
    std::array<double, kSliding> sliding{};
    int slidePos = 0;
    std::uint64_t measuredSamples = 0;
    // peaks
    fx::Oversampler4 os[2] = {fx::Oversampler4(1.0), fx::Oversampler4(1.0)};
    std::vector<float> up[2];
    double tpMax[2] = {0, 0};
    double ppm[2] = {0, 0}, ppmHold[2] = {0, 0};
    std::int64_t ppmHoldLeft[2] = {0, 0};
    std::int64_t peakAlertLeft = 0;
    // stereo
    double cLR = 0, cLL = 0, cRR = 0, cK = 0;
    // RTA
    std::array<std::array<Bq, 2>, kBands> band{};
    std::array<double, kBands> bandMs{}, bandAvg{}, bandPk{};   // bandAvg: the mean square; bandMs: shown (IMPULSE: held)
    std::array<std::int64_t, kBands> bandHold{};
    std::array<std::array<Bq, 2>, kOctaves> octBand{};
    std::array<double, kOctaves> octMs{}, octAvg{}, octPk{};
    std::array<std::int64_t, kOctaves> octHold{};
    double rtaUp[3] = {0, 0, 0}, rtaDown[3] = {0, 0, 0};
    // history / scope
    std::unique_ptr<std::atomic<float>[]> hist;
    std::uint64_t histCap = 0;
    std::atomic<std::uint64_t> histCount{0};
    std::unique_ptr<std::atomic<float>[]> scope;
    std::atomic<std::uint64_t> scopeCount{0};
    // published readings
    AtomicD M, Sh, I, LRA, MaxM, MaxS, TP[2], TPnow[2], SP[2], PLR, Corr, BalLR, BalMS, MonoDev, Slide, Ppm[2], PpmHold[2], Measured;
    std::array<AtomicD, kBands> rta, rtaPk;
    std::array<AtomicD, kOctaves> oct, octPkA;
    std::atomic<bool> peakLamp{false};

    [[nodiscard]] float v(int i) const noexcept { return vals[static_cast<std::size_t>(i)].load(std::memory_order_relaxed); }
    [[nodiscard]] int c(int i) const noexcept { return static_cast<int>(std::lround(v(i))); }
    [[nodiscard]] bool measuring() const noexcept {
        return v(Run) > 0.5f && (v(FollowTransport) < 0.5f || playing.load(std::memory_order_relaxed));
    }

    void computeStep() noexcept {
        auto& st = steps[static_cast<std::size_t>(stepPos)];
        for (int k = 0; k < 4; ++k) st[static_cast<std::size_t>(k)] = stepE[k] / static_cast<double>(stepLen);
        stepPos = (stepPos + 1) % kSteps;
        stepsFilled = std::min(stepsFilled + 1, kSteps);
        std::fill(std::begin(stepE), std::end(stepE), 0.0);
        auto window = [&](int nSteps, int k) {
            double e = 0;
            for (int i = 1; i <= nSteps; ++i) e += steps[static_cast<std::size_t>((stepPos - i + kSteps) % kSteps)][static_cast<std::size_t>(k)];
            return e / nSteps;
        };
        const int mN = static_cast<int>(std::lround(S::kMomentarySeconds / S::kStepSeconds));
        const int sN = static_cast<int>(std::lround(S::kShortTermSeconds / S::kStepSeconds));
        const double sumOffset = c(MonoDownmix) == 0 ? S::kSumDownmixDb : 0.0;   // SUM: (L+R) un-compensated
        if (stepsFilled >= mN) {
            const double e = window(mN, 0) + window(mN, 1);
            const double l = lufs(e);
            M.set(l);
            maxM = std::max(maxM, l); MaxM.set(maxM);
            sliding[static_cast<std::size_t>(slidePos)] = l;
            slidePos = (slidePos + 1) % kSliding;
            double sl = kSilent;
            for (double x : sliding) sl = std::max(sl, x);
            Slide.set(sl);
            integ.add(e);
            integMono.add(2.0 * window(mN, 2));   // mono downmix (L+R)/sqrt2 as one channel
            I.set(integ.gated());
        }
        if (stepsFilled >= sN) {
            const double eL = window(sN, 0), eR = window(sN, 1), eM = window(sN, 2), eS = window(sN, 3);
            const double l = lufs(eL + eR);
            Sh.set(l);
            maxS = std::max(maxS, l); MaxS.set(maxS);
            lra.add(eL + eR);
            BalLR.set(eL > 0 && eR > 0 ? 10.0 * std::log10(eL / eR) : 0.0);
            BalMS.set(eM > 0 && eS > 0 ? 10.0 * std::log10(eS / eM) : (eS > 0 ? 144.0 : -144.0));
            if (c(DevSpan) == 0) {   // SHORT-TERM: live, over the last 3 s
                MonoDev.set(eL + eR > 0 ? (eM > 0 ? 10.0 * std::log10(2.0 * eM / (eL + eR)) + sumOffset : -144.0) : 0.0);
            } else {                 // PROGRAM: gated mono vs gated stereo since RESET
                const double im = integMono.gated(), is = I.get();
                MonoDev.set(im > kSilent && is > kSilent ? im - is + sumOffset : (is > kSilent ? -144.0 : 0.0));
            }
            // loudness range
            std::uint64_t n = 0;
            const double absMean = lra.meanAbove(S::kAbsGateLufs, n);
            if (n) {
                const double gate = lufs(absMean) + S::kLraRelGateLu;
                const int from = std::max(0, static_cast<int>(std::ceil((gate - S::kAbsGateLufs) / S::kHistogramStepLu - 1e-9)));
                std::uint64_t total = 0;
                for (int b = from; b < Histogram::kBins; ++b) total += lra.count[static_cast<std::size_t>(b)];
                if (total) {
                    const double lo = S::kLraLow * static_cast<double>(total), hi = S::kLraHigh * static_cast<double>(total);
                    std::uint64_t acc = 0; double l10 = 0, l95 = 0; bool got10 = false;
                    for (int b = from; b < Histogram::kBins; ++b) {
                        acc += lra.count[static_cast<std::size_t>(b)];
                        if (!got10 && static_cast<double>(acc) > lo) { l10 = Histogram::binLufs(b); got10 = true; }
                        if (static_cast<double>(acc) > hi) { l95 = Histogram::binLufs(b); break; }
                    }
                    LRA.set(l95 - l10);
                }
            }
            const std::uint64_t h = histCount.load(std::memory_order_relaxed);
            hist[h % histCap].store(static_cast<float>(l), std::memory_order_relaxed);
            histCount.store(h + 1, std::memory_order_release);
        }
        const double i = I.get();
        PLR.set(i > kSilent ? dbOf(std::max(tpMax[0], tpMax[1])) - i : kSilent);
    }
};

MeterBridge::MeterBridge() : d_(std::make_unique<Impl>()) {
    setDefaults();
    prepare(48000.0, 512);
}
MeterBridge::~MeterBridge() = default;

std::span<const fx::ParamInfo> MeterBridge::params() noexcept { return kParams; }
void MeterBridge::set(int id, float v) noexcept {
    if (id < 0 || id >= kCount) return;
    d_->vals[static_cast<std::size_t>(id)].store(fx::clampPlain(kParams[static_cast<std::size_t>(id)], v), std::memory_order_relaxed);
}
float MeterBridge::get(int id) const noexcept { return id >= 0 && id < kCount ? d_->v(id) : 0.0f; }
void MeterBridge::setDefaults() noexcept { for (int i = 0; i < kCount; ++i) set(i, kParams[static_cast<std::size_t>(i)].def); }
int MeterBridge::factoryPresetCount() noexcept { return 1 + static_cast<int>(S::kFactoryPresets.size()); }
const char* MeterBridge::factoryPresetName(int p) noexcept {
    if (p <= 0 || p >= factoryPresetCount()) return "DEFAULT";
    return S::kFactoryPresets[static_cast<std::size_t>(p - 1)].name;
}
void MeterBridge::applyFactoryPreset(int p) noexcept {
    const float run = get(Run);
    setDefaults();
    set(Run, run);
    if (p <= 0 || p >= factoryPresetCount()) return;
    const auto& f = S::kFactoryPresets[static_cast<std::size_t>(p - 1)];
    set(Target, static_cast<float>(f.targetLufs - static_cast<int>(S::kTargetMin)));
    set(Unit, static_cast<float>(f.unit));
    set(PeakAlert, (f.peakAlertDbtp - S::kPeakAlertMin) / 0.5f + 1.0f);
    set(MomRange, static_cast<float>(f.momRange));
}
double MeterBridge::targetLufs() const noexcept { return S::kTargetMin + d_->c(Target); }
double MeterBridge::radarResolutionDb() const noexcept { return S::kRadarResMin + d_->c(RadarRes); }
double MeterBridge::radarSpeedSeconds(int choice) noexcept { return S::kRadarSpeedS[static_cast<std::size_t>(std::clamp(choice, 0, 11))]; }
std::array<int, 6> MeterBridge::descriptors() const noexcept {
    const int l = d_->c(Layout);
    if (l >= 0 && l < 6) return kLayouts[static_cast<std::size_t>(l)];
    std::array<int, 6> o{};
    for (int k = 0; k < 6; ++k) o[static_cast<std::size_t>(k)] = d_->c(Slot1 + k);
    return o;
}
void MeterBridge::setTransportPlaying(bool p) noexcept { d_->playing.store(p, std::memory_order_relaxed); }
bool MeterBridge::measuring() const noexcept { return d_->measuring(); }
double MeterBridge::measuredSeconds() const noexcept { return d_->Measured.get(); }

std::string MeterBridge::settingsText() const {
    std::string s;
    for (int i = 0; i < kCount; ++i) {
        if (i == Run) continue;   // a preset never pauses the meter
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", static_cast<double>(get(i)));
        s += kParams[static_cast<std::size_t>(i)].id; s += '='; s += b; s += '\n';
    }
    return s;
}
int MeterBridge::applySettingsText(std::string_view text) {
    int n = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t e = text.find('\n', pos);
        std::string_view ln = text.substr(pos, (e == std::string_view::npos ? text.size() : e) - pos);
        pos = e == std::string_view::npos ? text.size() : e + 1;
        const std::size_t eq = ln.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = ln.substr(0, eq);
        std::string_view vs = ln.substr(eq + 1);
        while (!vs.empty() && (vs.back() == '\r' || vs.back() == ' ')) vs.remove_suffix(1);
        float v = 0;
        if (std::from_chars(vs.data(), vs.data() + vs.size(), v).ec != std::errc() || !std::isfinite(v)) continue;
        for (int i = 0; i < kCount; ++i)
            if (i != Run && k == kParams[static_cast<std::size_t>(i)].id) { set(i, v); ++n; break; }
    }
    return n;
}

void MeterBridge::rtaAutoScale() noexcept {
    const bool oct = d_->c(RtaBands) == 0;
    double mx = -1e9, mn = 1e9;
    const int n = oct ? kOctaves : kBands;
    for (int b = 0; b < n; ++b) {
        const double v = oct ? octaveDb(b) : bandDb(b);
        if (v <= -120.0) continue;
        mx = std::max(mx, v); mn = std::min(mn, v);
    }
    if (mx < -1e8) return;   // nothing playing: the manual's AUTO only works while audio plays
    const double top = std::clamp(std::ceil(mx / 5.0) * 5.0, static_cast<double>(S::kRtaTopMin), static_cast<double>(S::kRtaTopMax));
    set(RtaTop, static_cast<float>(-top / 5.0));
    if (d_->c(RtaAuto) == 0) {
        const double range = std::clamp(std::ceil((top - mn) / 10.0) * 10.0, static_cast<double>(S::kRtaRangeMin), static_cast<double>(S::kRtaRangeMax));
        set(RtaRange, static_cast<float>(range / 10.0 - 1.0));
    }
}

void MeterBridge::prepare(double fs, std::uint32_t maxBlock, double historySeconds) {
    auto& d = *d_;
    d.fs = fs;
    d.maxBlock = std::max<std::uint32_t>(1, maxBlock);
    for (int c = 0; c < 2; ++c) { kWeighting(fs, d.kShelf[c], d.kRlb[c]); d.up[c].assign(static_cast<std::size_t>(d.maxBlock) * 4, 0.0f); }
    d.stepLen = static_cast<std::size_t>(std::llround(S::kStepSeconds * fs));
    d.cK = std::exp(-1.0 / (S::kCorrelationSeconds * fs));
    const double ups[3] = {S::kRtaSlowS, S::kRtaFastS, S::kRtaImpulseUpS}, downs[3] = {S::kRtaSlowS, S::kRtaFastS, S::kRtaImpulseDownS};
    for (int m = 0; m < 3; ++m) { d.rtaUp[m] = std::exp(-1.0 / (ups[m] * fs)); d.rtaDown[m] = std::exp(-1.0 / (downs[m] * fs)); }
    for (int b = 0; b < kBands; ++b) {
        const double hz = std::min(bandHz(b), 0.45 * fs);
        const auto c = fx::BiquadCoefs::make(fx::BiquadCoefs::Type::BandPass, fs, hz, S::kRtaFilterQ);
        for (auto& f : d.band[static_cast<std::size_t>(b)]) { f.b0 = c.b0; f.b1 = c.b1; f.b2 = c.b2; f.a1 = c.a1; f.a2 = c.a2; }
    }
    for (int o = 0; o < kOctaves; ++o) {
        const double hz = std::min(octaveHz(o), 0.45 * fs);
        const auto c = fx::BiquadCoefs::make(fx::BiquadCoefs::Type::BandPass, fs, hz, S::kRtaOctaveQ);
        for (auto& f : d.octBand[static_cast<std::size_t>(o)]) { f.b0 = c.b0; f.b1 = c.b1; f.b2 = c.b2; f.a1 = c.a1; f.a2 = c.a2; }
    }
    d.histCap = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(historySeconds / S::kStepSeconds));
    d.hist.reset(new std::atomic<float>[d.histCap]);
    d.scope.reset(new std::atomic<float>[2 * kScopeFrames]);
    for (std::uint32_t i = 0; i < 2 * kScopeFrames; ++i) d.scope[i].store(0.0f);
    reset();
}

void MeterBridge::reset() noexcept {
    auto& d = *d_;
    for (int c = 0; c < 2; ++c) {
        d.kShelf[c].clear(); d.kRlb[c].clear(); d.os[c].clear(); d.tpMax[c] = 0;
        d.ppm[c] = d.ppmHold[c] = 0; d.ppmHoldLeft[c] = 0;
    }
    d.peakAlertLeft = 0;
    d.peakLamp.store(false);
    d.stepFill = 0; std::fill(std::begin(d.stepE), std::end(d.stepE), 0.0);
    for (auto& s : d.steps) s.fill(0.0);
    d.stepPos = d.stepsFilled = 0;
    d.integ.clear(); d.lra.clear(); d.integMono.clear();
    d.maxM = d.maxS = kSilent;
    d.sliding.fill(kSilent); d.slidePos = 0;
    d.measuredSamples = 0;
    d.cLR = d.cLL = d.cRR = 0;
    for (auto& b : d.band) for (auto& f : b) f.clear();
    for (auto& b : d.octBand) for (auto& f : b) f.clear();
    d.octMs.fill(0); d.octAvg.fill(0);
    d.bandMs.fill(0); d.bandAvg.fill(0); d.bandPk.fill(0); d.bandHold.fill(0); d.octPk.fill(0); d.octHold.fill(0);
    d.histCount.store(0);
    d.scopeCount.store(0);
    for (AtomicD* a : {&d.M, &d.Sh, &d.I, &d.LRA, &d.MaxM, &d.MaxS, &d.TP[0], &d.TP[1], &d.TPnow[0], &d.TPnow[1], &d.SP[0], &d.SP[1], &d.PLR, &d.Slide,
                       &d.Ppm[0], &d.Ppm[1], &d.PpmHold[0], &d.PpmHold[1]})
        a->set(kSilent);
    d.LRA.set(0.0); d.Corr.set(0.0); d.BalLR.set(0.0); d.BalMS.set(0.0); d.MonoDev.set(0.0); d.Measured.set(0.0);
    for (auto& a : d.rta) a.set(kSilent);
    for (auto& a : d.rtaPk) a.set(kSilent);
    for (auto& a : d.oct) a.set(kSilent);
    for (auto& a : d.octPkA) a.set(kSilent);
}

void MeterBridge::process(const float* L, const float* R, std::uint32_t frames) noexcept {
    auto& d = *d_;
    const bool measure = d.measuring();
    const auto& bal = ballistics(d.c(PeakScale));
    const double fall = std::pow(10.0, -bal.fallDbPerS / 20.0 / d.fs);   // per sample
    const double att = bal.integrationS > 0 ? 1.0 - std::exp(-2.2 / (bal.integrationS * d.fs)) : 1.0;   // 1 - e^-2.2 = -1 dB after the integration time
    const auto peakHoldN = static_cast<std::int64_t>(S::kPeakHoldS[static_cast<std::size_t>(std::clamp(d.c(PeakHold), 0, 5))] * d.fs);
    const int alertIdx = d.c(PeakAlert);
    const double alertLin = alertIdx > 0 ? std::pow(10.0, (S::kPeakAlertMin + 0.5 * (alertIdx - 1)) / 20.0) : 1e30;
    const int rtaMode = std::clamp(d.c(RtaTime), 0, 2);   // SLOW / FAST / IMPULSE
    // SLOW / FAST: the mean square averaged over 1 s / 125 ms. IMPULSE: averaged over 35 ms, rising with it at once and
    // falling at most with the 1.5 s decay (asymmetric ballistics on the averaged level, not on the raw waveform).
    const double kAvg = d.rtaUp[rtaMode], kDecay = rtaMode == 2 ? d.rtaDown[2] : 0.0;
    const int rtaCh = std::clamp(d.c(RtaChannels), 0, 2);
    const bool octaves = d.c(RtaBands) == 0;   // the octave filters only run while the 10-band resolution is shown
    const auto rtaHoldN = static_cast<std::int64_t>(std::min(S::kRtaHoldS[static_cast<std::size_t>(std::clamp(d.c(RtaHold), 0, 7))] * d.fs, 9.0e18));
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(frames - done, d.maxBlock);
        const float* x[2] = {L + done, R + done};
        // ---- peaks (sample and 4x true peak), peak meter ballistics, PEAK alert ----
        bool over = false;
        for (int c = 0; c < 2; ++c) {
            double sp = 0;
            for (std::uint32_t i = 0; i < n; ++i) sp = std::max(sp, static_cast<double>(std::fabs(x[c][i])));
            d.os[c].up(x[c], d.up[c].data(), n);
            double tp = sp;
            for (std::uint32_t j = 0; j < 4 * n; ++j) tp = std::max(tp, static_cast<double>(std::fabs(d.up[c][j])));
            d.tpMax[c] = std::max(d.tpMax[c], tp);
            d.SP[c].set(dbOf(sp)); d.TPnow[c].set(dbOf(tp)); d.TP[c].set(dbOf(d.tpMax[c]));
            over = over || tp >= alertLin;
            double& env = d.ppm[c];
            if (bal.integrationS <= 0) {   // true-peak bars: instant rise, steady fall
                env = std::max(tp, env * std::pow(fall, static_cast<double>(n)));
            } else {
                for (std::uint32_t i = 0; i < n; ++i) {
                    const double a = std::fabs(x[c][i]);
                    env = a > env ? env + att * (a - env) : env * fall;
                }
            }
            d.Ppm[c].set(dbOf(env));
            if (env >= d.ppmHold[c] || d.ppmHoldLeft[c] <= 0) { if (env >= d.ppmHold[c]) d.ppmHoldLeft[c] = peakHoldN; d.ppmHold[c] = env; }
            else d.ppmHoldLeft[c] -= n;
            d.PpmHold[c].set(dbOf(d.ppmHold[c]));
        }
        if (over) d.peakAlertLeft = peakHoldN; else d.peakAlertLeft -= n;
        d.peakLamp.store(d.peakAlertLeft > 0, std::memory_order_relaxed);
        // ---- per sample: K-weighting, steps, correlation, RTA, scope ----
        std::uint64_t sc = d.scopeCount.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < n; ++i) {
            const double l = x[0][i], r = x[1][i];
            const double kl = d.kRlb[0].run(d.kShelf[0].run(l)), kr = d.kRlb[1].run(d.kShelf[1].run(r));
            if (measure) {   // PAUSE stops the loudness measurement only (MTR-UM p.9)
                const double km = 0.5 * (kl + kr), ks = 0.5 * (kl - kr);
                d.stepE[0] += kl * kl; d.stepE[1] += kr * kr; d.stepE[2] += km * km; d.stepE[3] += ks * ks;
                if (++d.stepFill >= d.stepLen) { d.stepFill = 0; d.computeStep(); }
            }
            d.cLR = l * r + d.cK * (d.cLR - l * r);
            d.cLL = l * l + d.cK * (d.cLL - l * l);
            d.cRR = r * r + d.cK * (d.cRR - r * r);
            const double m = rtaCh == 0 ? l : rtaCh == 1 ? r : 0.5 * (l + r);
            for (int b = 0; b < kBands; ++b) {
                auto& f = d.band[static_cast<std::size_t>(b)];
                const double y = f[1].run(f[0].run(m));
                auto& avg = d.bandAvg[static_cast<std::size_t>(b)];
                const double y2 = y * y;
                avg = y2 + kAvg * (avg - y2);
                auto& ms = d.bandMs[static_cast<std::size_t>(b)];
                ms = kDecay > 0.0 ? std::max(avg, ms * kDecay) : avg;
            }
            if (octaves) {
                for (int o = 0; o < kOctaves; ++o) {
                    auto& f = d.octBand[static_cast<std::size_t>(o)];
                    const double y = f[1].run(f[0].run(m));
                    auto& avg = d.octAvg[static_cast<std::size_t>(o)];
                    const double y2 = y * y;
                    avg = y2 + kAvg * (avg - y2);
                    auto& ms = d.octMs[static_cast<std::size_t>(o)];
                    ms = kDecay > 0.0 ? std::max(avg, ms * kDecay) : avg;
                }
            }
            d.scope[2 * (sc % kScopeFrames)].store(static_cast<float>(l), std::memory_order_relaxed);
            d.scope[2 * (sc % kScopeFrames) + 1].store(static_cast<float>(r), std::memory_order_relaxed);
            ++sc;
        }
        if (measure) d.measuredSamples += n;
        d.Measured.set(static_cast<double>(d.measuredSamples) / d.fs);
        d.scopeCount.store(sc, std::memory_order_release);
        const double den = std::sqrt(d.cLL * d.cRR);
        d.Corr.set(den > 1e-20 ? std::clamp(d.cLR / den, -1.0, 1.0) : 0.0);
        auto hold = [&](double lv, double& pk, std::int64_t& left) {
            if (lv >= pk) { pk = lv; left = rtaHoldN; }
            else if ((left -= static_cast<std::int64_t>(n)) <= 0) pk = lv;
        };
        for (int b = 0; b < kBands; ++b) {
            const double lv = d.bandMs[static_cast<std::size_t>(b)] * 2.0;   // a full-scale sine reads 0 dB
            hold(lv, d.bandPk[static_cast<std::size_t>(b)], d.bandHold[static_cast<std::size_t>(b)]);
            d.rta[static_cast<std::size_t>(b)].set(lv > 0 ? 10.0 * std::log10(lv) : kSilent);
            d.rtaPk[static_cast<std::size_t>(b)].set(d.bandPk[static_cast<std::size_t>(b)] > 0 ? 10.0 * std::log10(d.bandPk[static_cast<std::size_t>(b)]) : kSilent);
        }
        for (int o = 0; o < kOctaves && octaves; ++o) {
            const double lv = d.octMs[static_cast<std::size_t>(o)] * 2.0;
            hold(lv, d.octPk[static_cast<std::size_t>(o)], d.octHold[static_cast<std::size_t>(o)]);
            d.oct[static_cast<std::size_t>(o)].set(lv > 0 ? 10.0 * std::log10(lv) : kSilent);
            d.octPkA[static_cast<std::size_t>(o)].set(d.octPk[static_cast<std::size_t>(o)] > 0 ? 10.0 * std::log10(d.octPk[static_cast<std::size_t>(o)]) : kSilent);
        }
        done += n;
    }
}

double MeterBridge::momentaryLufs() const noexcept { return d_->M.get(); }
double MeterBridge::shortTermLufs() const noexcept { return d_->Sh.get(); }
double MeterBridge::integratedLufs() const noexcept { return d_->I.get(); }
double MeterBridge::loudnessRangeLu() const noexcept { return d_->LRA.get(); }
double MeterBridge::maxMomentaryLufs() const noexcept { return d_->MaxM.get(); }
double MeterBridge::maxShortTermLufs() const noexcept { return d_->MaxS.get(); }
double MeterBridge::truePeakDbtp(int s) const noexcept { return d_->TP[s ? 1 : 0].get(); }
double MeterBridge::truePeakNowDbtp(int s) const noexcept { return d_->TPnow[s ? 1 : 0].get(); }
double MeterBridge::samplePeakDbfs(int s) const noexcept { return d_->SP[s ? 1 : 0].get(); }
double MeterBridge::plrDb() const noexcept { return d_->PLR.get(); }
double MeterBridge::sliding10Lufs() const noexcept { return d_->Slide.get(); }
double MeterBridge::ppmDb(int s) const noexcept { return d_->Ppm[s ? 1 : 0].get(); }
double MeterBridge::peakHoldDb(int s) const noexcept { return d_->PpmHold[s ? 1 : 0].get(); }
bool MeterBridge::peakAlert() const noexcept { return d_->peakLamp.load(std::memory_order_relaxed); }
bool MeterBridge::loudAlert() const noexcept {
    const int a = d_->c(LoudAlert);
    return a > 0 && d_->M.get() >= targetLufs() + a;
}
double MeterBridge::correlation() const noexcept { return d_->Corr.get(); }
double MeterBridge::balanceLrLu() const noexcept { return d_->BalLR.get(); }
double MeterBridge::balanceMsDb() const noexcept { return d_->BalMS.get(); }
double MeterBridge::monoDeviationLu() const noexcept { return d_->MonoDev.get(); }
double MeterBridge::bandHz(int b) noexcept { return 1000.0 * std::pow(10.0, (b - 17) / 10.0); }   // base-10 1/3-octave centres, 20 Hz .. 20 kHz
double MeterBridge::bandDb(int b) const noexcept { return b >= 0 && b < kBands ? d_->rta[static_cast<std::size_t>(b)].get() : kSilent; }
double MeterBridge::bandPeakDb(int b) const noexcept { return b >= 0 && b < kBands ? d_->rtaPk[static_cast<std::size_t>(b)].get() : kSilent; }
double MeterBridge::octaveHz(int o) noexcept { return bandHz(2 + 3 * std::clamp(o, 0, kOctaves - 1)); }
double MeterBridge::octaveDb(int o) const noexcept { return o >= 0 && o < kOctaves ? d_->oct[static_cast<std::size_t>(o)].get() : kSilent; }
double MeterBridge::octavePeakDb(int o) const noexcept { return o >= 0 && o < kOctaves ? d_->octPkA[static_cast<std::size_t>(o)].get() : kSilent; }
std::uint64_t MeterBridge::historyCount() const noexcept { return d_->histCount.load(std::memory_order_acquire); }
float MeterBridge::historyLufs(std::uint64_t i) const noexcept { return d_->hist[i % d_->histCap].load(std::memory_order_relaxed); }
std::uint64_t MeterBridge::scopeWritten() const noexcept { return d_->scopeCount.load(std::memory_order_acquire); }
void MeterBridge::scopeFrame(std::uint64_t i, float& l, float& r) const noexcept {
    l = d_->scope[2 * (i % kScopeFrames)].load(std::memory_order_relaxed);
    r = d_->scope[2 * (i % kScopeFrames) + 1].load(std::memory_order_relaxed);
}

} // namespace hc::console
