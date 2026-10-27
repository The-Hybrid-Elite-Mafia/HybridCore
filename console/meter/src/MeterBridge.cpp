// HEM METER BRIDGE — implementation (ITU-R BS.1770-4, EBU R128 / Tech 3341 / Tech 3342).
#include "hybrid/console/MeterBridge.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace hc::console {
namespace {

namespace S = spec::meter;

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
};

struct AtomicD {
    std::atomic<double> v{MeterBridge::kSilent};
    void set(double x) noexcept { v.store(x, std::memory_order_relaxed); }
    [[nodiscard]] double get() const noexcept { return v.load(std::memory_order_relaxed); }
};

} // namespace

struct MeterBridge::Impl {
    double fs = 48000;
    std::uint32_t maxBlock = 512;
    // loudness
    Bq kShelf[2], kRlb[2];
    std::size_t stepLen = 4800, stepFill = 0;
    double stepE[4] = {0, 0, 0, 0};   // this 100 ms step: L, R (K-weighted mean square sums), mid, side
    static constexpr int kSteps = 30;
    std::array<std::array<double, 4>, kSteps> steps{};   // last 3 s of 100 ms steps
    int stepPos = 0, stepsFilled = 0;
    Histogram integ, lra;
    double maxM = kSilent, maxS = kSilent;
    // peaks
    fx::Oversampler4 os[2] = {fx::Oversampler4(1.0), fx::Oversampler4(1.0)};
    std::vector<float> up[2];
    double tpMax[2] = {0, 0};
    // stereo
    double cLR = 0, cLL = 0, cRR = 0, cK = 0;
    // RTA
    std::array<std::array<Bq, 2>, kBands> band{};
    std::array<double, kBands> bandMs{}, bandPk{};
    std::array<int, kBands> bandHold{};
    double rtaK = 0;
    int holdN = 0;
    // history / scope
    std::unique_ptr<std::atomic<float>[]> hist;
    std::uint64_t histCap = 0;
    std::atomic<std::uint64_t> histCount{0};
    std::unique_ptr<std::atomic<float>[]> scope;
    std::atomic<std::uint64_t> scopeCount{0};
    // published readings
    AtomicD M, Sh, I, LRA, MaxM, MaxS, TP[2], TPnow[2], SP[2], PLR, Corr, BalLR, BalMS, MonoDev;
    std::array<AtomicD, kBands> rta, rtaPk;

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
        if (stepsFilled >= mN) {
            const double e = window(mN, 0) + window(mN, 1);
            const double l = lufs(e);
            M.set(l);
            maxM = std::max(maxM, l); MaxM.set(maxM);
            integ.add(e);
            std::uint64_t n = 0;
            const double absMean = integ.meanAbove(S::kAbsGateLufs, n);
            if (n) {
                std::uint64_t m = 0;
                const double rel = integ.meanAbove(lufs(absMean) + S::kRelGateLu, m);
                I.set(m ? lufs(rel) : kSilent);
            }
        }
        if (stepsFilled >= sN) {
            const double eL = window(sN, 0), eR = window(sN, 1), eM = window(sN, 2), eS = window(sN, 3);
            const double l = lufs(eL + eR);
            Sh.set(l);
            maxS = std::max(maxS, l); MaxS.set(maxS);
            lra.add(eL + eR);
            BalLR.set(eL > 0 && eR > 0 ? 10.0 * std::log10(eL / eR) : 0.0);
            BalMS.set(eM > 0 && eS > 0 ? 10.0 * std::log10(eS / eM) : (eS > 0 ? 144.0 : -144.0));
            MonoDev.set(eL + eR > 0 ? (eM > 0 ? 10.0 * std::log10(2.0 * eM / (eL + eR)) : -144.0) : 0.0);
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

MeterBridge::MeterBridge() : d_(std::make_unique<Impl>()) { prepare(48000.0, 512); }
MeterBridge::~MeterBridge() = default;

void MeterBridge::prepare(double fs, std::uint32_t maxBlock, double historySeconds) {
    auto& d = *d_;
    d.fs = fs;
    d.maxBlock = std::max<std::uint32_t>(1, maxBlock);
    for (int c = 0; c < 2; ++c) { kWeighting(fs, d.kShelf[c], d.kRlb[c]); d.up[c].assign(static_cast<std::size_t>(d.maxBlock) * 4, 0.0f); }
    d.stepLen = static_cast<std::size_t>(std::llround(S::kStepSeconds * fs));
    d.cK = std::exp(-1.0 / (S::kCorrelationSeconds * fs));
    d.rtaK = std::exp(-1.0 / (S::kRtaAverageSeconds * fs));
    d.holdN = static_cast<int>(S::kRtaPeakHoldSeconds * fs);
    for (int b = 0; b < kBands; ++b) {
        const double hz = std::min(bandHz(b), 0.45 * fs);
        const auto c = fx::BiquadCoefs::make(fx::BiquadCoefs::Type::BandPass, fs, hz, S::kRtaFilterQ);
        for (auto& f : d.band[static_cast<std::size_t>(b)]) { f.b0 = c.b0; f.b1 = c.b1; f.b2 = c.b2; f.a1 = c.a1; f.a2 = c.a2; }
    }
    d.histCap = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(historySeconds / S::kStepSeconds));
    d.hist.reset(new std::atomic<float>[d.histCap]);
    d.scope.reset(new std::atomic<float>[2 * kScopeFrames]);
    for (std::uint32_t i = 0; i < 2 * kScopeFrames; ++i) d.scope[i].store(0.0f);
    reset();
}

void MeterBridge::reset() noexcept {
    auto& d = *d_;
    for (int c = 0; c < 2; ++c) { d.kShelf[c].clear(); d.kRlb[c].clear(); d.os[c].clear(); d.tpMax[c] = 0; }
    d.stepFill = 0; std::fill(std::begin(d.stepE), std::end(d.stepE), 0.0);
    for (auto& s : d.steps) s.fill(0.0);
    d.stepPos = d.stepsFilled = 0;
    d.integ.clear(); d.lra.clear();
    d.maxM = d.maxS = kSilent;
    d.cLR = d.cLL = d.cRR = 0;
    for (auto& b : d.band) for (auto& f : b) f.clear();
    d.bandMs.fill(0); d.bandPk.fill(0); d.bandHold.fill(0);
    d.histCount.store(0);
    d.scopeCount.store(0);
    for (AtomicD* a : {&d.M, &d.Sh, &d.I, &d.LRA, &d.MaxM, &d.MaxS, &d.TP[0], &d.TP[1], &d.TPnow[0], &d.TPnow[1], &d.SP[0], &d.SP[1], &d.PLR})
        a->set(kSilent);
    d.LRA.set(0.0); d.Corr.set(0.0); d.BalLR.set(0.0); d.BalMS.set(0.0); d.MonoDev.set(0.0);
    for (auto& a : d.rta) a.set(kSilent);
    for (auto& a : d.rtaPk) a.set(kSilent);
}

void MeterBridge::process(const float* L, const float* R, std::uint32_t frames) noexcept {
    auto& d = *d_;
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(frames - done, d.maxBlock);
        const float* x[2] = {L + done, R + done};
        // ---- peaks (sample and 4x true peak) ----
        for (int c = 0; c < 2; ++c) {
            double sp = 0;
            for (std::uint32_t i = 0; i < n; ++i) sp = std::max(sp, static_cast<double>(std::fabs(x[c][i])));
            d.os[c].up(x[c], d.up[c].data(), n);
            double tp = sp;
            for (std::uint32_t j = 0; j < 4 * n; ++j) tp = std::max(tp, static_cast<double>(std::fabs(d.up[c][j])));
            d.tpMax[c] = std::max(d.tpMax[c], tp);
            d.SP[c].set(dbOf(sp)); d.TPnow[c].set(dbOf(tp)); d.TP[c].set(dbOf(d.tpMax[c]));
        }
        // ---- per sample: K-weighting, steps, correlation, RTA, scope ----
        std::uint64_t sc = d.scopeCount.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < n; ++i) {
            const double l = x[0][i], r = x[1][i];
            const double kl = d.kRlb[0].run(d.kShelf[0].run(l)), kr = d.kRlb[1].run(d.kShelf[1].run(r));
            const double km = 0.5 * (kl + kr), ks = 0.5 * (kl - kr);
            d.stepE[0] += kl * kl; d.stepE[1] += kr * kr; d.stepE[2] += km * km; d.stepE[3] += ks * ks;
            if (++d.stepFill >= d.stepLen) { d.stepFill = 0; d.computeStep(); }
            d.cLR = l * r + d.cK * (d.cLR - l * r);
            d.cLL = l * l + d.cK * (d.cLL - l * l);
            d.cRR = r * r + d.cK * (d.cRR - r * r);
            const double m = S::kRtaMonoSum ? 0.5 * (l + r) : l;
            for (int b = 0; b < kBands; ++b) {
                auto& f = d.band[static_cast<std::size_t>(b)];
                const double y = f[1].run(f[0].run(m));
                auto& ms = d.bandMs[static_cast<std::size_t>(b)];
                ms = y * y + d.rtaK * (ms - y * y);
            }
            d.scope[2 * (sc % kScopeFrames)].store(static_cast<float>(l), std::memory_order_relaxed);
            d.scope[2 * (sc % kScopeFrames) + 1].store(static_cast<float>(r), std::memory_order_relaxed);
            ++sc;
        }
        d.scopeCount.store(sc, std::memory_order_release);
        const double den = std::sqrt(d.cLL * d.cRR);
        d.Corr.set(den > 1e-20 ? std::clamp(d.cLR / den, -1.0, 1.0) : 0.0);
        for (int b = 0; b < kBands; ++b) {
            const double lv = d.bandMs[static_cast<std::size_t>(b)] * 2.0;   // a full-scale sine reads 0 dB
            auto& pk = d.bandPk[static_cast<std::size_t>(b)];
            auto& hold = d.bandHold[static_cast<std::size_t>(b)];
            if (lv >= pk) { pk = lv; hold = d.holdN; }
            else if ((hold -= static_cast<int>(n)) <= 0) pk = lv;
            d.rta[static_cast<std::size_t>(b)].set(lv > 0 ? 10.0 * std::log10(lv) : kSilent);
            d.rtaPk[static_cast<std::size_t>(b)].set(pk > 0 ? 10.0 * std::log10(pk) : kSilent);
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
double MeterBridge::correlation() const noexcept { return d_->Corr.get(); }
double MeterBridge::balanceLrLu() const noexcept { return d_->BalLR.get(); }
double MeterBridge::balanceMsDb() const noexcept { return d_->BalMS.get(); }
double MeterBridge::monoDeviationLu() const noexcept { return d_->MonoDev.get(); }
double MeterBridge::bandHz(int b) noexcept { return 1000.0 * std::pow(10.0, (b - 17) / 10.0); }   // base-10 1/3-octave centres, 20 Hz .. 20 kHz
double MeterBridge::bandDb(int b) const noexcept { return b >= 0 && b < kBands ? d_->rta[static_cast<std::size_t>(b)].get() : kSilent; }
double MeterBridge::bandPeakDb(int b) const noexcept { return b >= 0 && b < kBands ? d_->rtaPk[static_cast<std::size_t>(b)].get() : kSilent; }
std::uint64_t MeterBridge::historyCount() const noexcept { return d_->histCount.load(std::memory_order_acquire); }
float MeterBridge::historyLufs(std::uint64_t i) const noexcept { return d_->hist[i % d_->histCap].load(std::memory_order_relaxed); }
std::uint64_t MeterBridge::scopeWritten() const noexcept { return d_->scopeCount.load(std::memory_order_acquire); }
void MeterBridge::scopeFrame(std::uint64_t i, float& l, float& r) const noexcept {
    l = d_->scope[2 * (i % kScopeFrames)].load(std::memory_order_relaxed);
    r = d_->scope[2 * (i % kScopeFrames) + 1].load(std::memory_order_relaxed);
}

} // namespace hc::console
