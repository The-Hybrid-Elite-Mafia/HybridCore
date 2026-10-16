#include "hybrid/media/Loudness.hpp"

#include "hybrid/dsp/BusLimiter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace hc::media {

namespace {

// Two biquads of the BS.1770 K-weighting (high shelf + RLB high-pass), derived for any rate
// (the same design as libebur128, so 48 kHz matches the coefficients printed in the standard).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double run(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

std::array<Biquad, 2> kWeighting(double fs) {
    std::array<Biquad, 2> f{};
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        f[0].b0 = (Vh + Vb * K / Q + K * K) / a0;
        f[0].b1 = 2.0 * (K * K - Vh) / a0;
        f[0].b2 = (Vh - Vb * K / Q + K * K) / a0;
        f[0].a1 = 2.0 * (K * K - 1.0) / a0;
        f[0].a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        f[1].b0 = 1.0; f[1].b1 = -2.0; f[1].b2 = 1.0;
        f[1].a1 = 2.0 * (K * K - 1.0) / a0;
        f[1].a2 = (1.0 - K / Q + K * K) / a0;
    }
    return f;
}

double toLufs(double meanSquare) { return -0.691 + 10.0 * std::log10(meanSquare); }

} // namespace

double integratedLufs(const float* left, const float* right, std::size_t frames, double fs) {
    const auto hop   = static_cast<std::size_t>(std::llround(0.1 * fs));   // 100 ms = a quarter block
    if (hop == 0 || frames < 4 * hop) return kSilentLufs;

    // Mean square of each 100 ms quarter, K-weighted, summed over both channels (weight 1 each).
    std::vector<double> quarter(frames / hop, 0.0);
    std::array<std::array<Biquad, 2>, 2> k{kWeighting(fs), kWeighting(fs)};
    const float* ch[2] = {left, right};
    for (int c = 0; c < 2; ++c) {
        for (std::size_t q = 0; q < quarter.size(); ++q) {
            double s = 0.0;
            for (std::size_t i = q * hop; i < (q + 1) * hop; ++i) {
                const double y = k[c][1].run(k[c][0].run(static_cast<double>(ch[c][i])));
                s += y * y;
            }
            quarter[q] += s / static_cast<double>(hop);
        }
    }
    // 400 ms blocks with 75 % overlap = 4 consecutive quarters.
    std::vector<double> block;
    block.reserve(quarter.size());
    for (std::size_t q = 3; q < quarter.size(); ++q) block.push_back(0.25 * (quarter[q - 3] + quarter[q - 2] + quarter[q - 1] + quarter[q]));

    auto gatedMean = [&](double gateLufs, double& mean) {
        double s = 0.0;
        std::size_t n = 0;
        for (double b : block)
            if (b > 0.0 && toLufs(b) > gateLufs) { s += b; ++n; }
        if (n == 0) return false;
        mean = s / static_cast<double>(n);
        return true;
    };
    double abs = 0.0, rel = 0.0;
    if (!gatedMean(-70.0, abs)) return kSilentLufs;
    if (!gatedMean(toLufs(abs) - 10.0, rel)) return kSilentLufs;
    return toLufs(rel);
}

MasterResult masterToLoudness(std::vector<float>& left, std::vector<float>& right, double fs, const MasterSettings& set) {
    MasterResult r;
    const std::size_t n = std::min(left.size(), right.size());
    r.lufsBefore = integratedLufs(left.data(), right.data(), n, fs);
    r.lufsAfter  = r.lufsBefore;
    if (r.lufsBefore <= kSilentLufs || r.lufsBefore >= set.targetLufs - 0.05) return r;   // silent, or loud enough already

    constexpr std::uint32_t kBlock = 1024;
    const std::vector<float> srcL(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(n));
    const std::vector<float> srcR(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(n));
    std::vector<float> inL(kBlock), inR(kBlock);

    double gainDb = std::min(set.targetLufs - r.lufsBefore, set.maxGainDb);
    for (int pass = 0; pass < 4; ++pass) {
        dsp::BusLimiter lim;
        lim.prepare(fs, kBlock);
        lim.setCeilingDb(static_cast<float>(set.ceilingDb));
        lim.setEnabled(true);
        const auto lat = static_cast<std::size_t>(lim.latency());
        const auto g = static_cast<float>(std::pow(10.0, gainDb / 20.0));
        double deepest = 0.0;
        std::size_t written = 0;
        for (std::size_t pos = 0; written < n; pos += kBlock) {   // n + latency samples in, the first `lat` dropped
            const std::uint32_t m = kBlock;
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::size_t s = pos + i;
                inL[i] = s < n ? srcL[s] * g : 0.0f;
                inR[i] = s < n ? srcR[s] * g : 0.0f;
            }
            const float* in[dsp::BusLimiter::kChannels] = {inL.data(), inR.data()};
            lim.process(in, m);
            deepest = std::max(deepest, static_cast<double>(lim.gainReductionDb()));
            const float* oL = lim.channel(0, 0);
            const float* oR = lim.channel(0, 1);
            for (std::uint32_t i = 0; i < m && written < n; ++i) {
                if (pos + i < lat) continue;
                left[written]  = oL[i];
                right[written] = oR[i];
                ++written;
            }
        }
        r.gainDb  = gainDb;
        r.limitDb = deepest;
        r.lufsAfter = integratedLufs(left.data(), right.data(), n, fs);
        const double shortBy = set.targetLufs - r.lufsAfter;
        if (shortBy < 0.2 || gainDb >= set.maxGainDb - 1.0e-9) break;
        gainDb = std::min(gainDb + shortBy, set.maxGainDb);
    }
    return r;
}

} // namespace hc::media
