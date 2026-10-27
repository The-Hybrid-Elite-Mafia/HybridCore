// HEM STEP EQ SE — implementation. Every frequency, step and range comes from ConsoleSpecs.hpp (namespace stepeqse);
// the proportional-Q law and the shelf shape are the standard model's (namespace stepeq).
#include "hybrid/console/StepEqSe.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"
#include "hybrid/console/StepEq.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace hc::console {
namespace {

namespace S = spec::stepeqse;
namespace Q = spec::stepeq;
using namespace stepeqse_ids;
using fx::ParamInfo;
using fx::Taper;

constexpr int kFreqs = 8, kGains = 21, kBands = 4;
const std::array<const float*, kBands> kBandHz{S::kLoHz.data(), S::kLoMidHz.data(), S::kHiMidHz.data(), S::kHiHz.data()};

template <std::size_t N>
struct Names {
    std::array<std::string, N> s;
    std::array<const char*, N> p{};
    template <typename F> explicit Names(F f) { for (std::size_t i = 0; i < N; ++i) { s[i] = f(i); p[i] = s[i].c_str(); } }
};
std::string hzName(float hz) {
    char b[24];
    if (hz >= 1000.0f) std::snprintf(b, sizeof b, hz == std::floor(hz / 1000.0f) * 1000.0f ? "%.0f kHz" : "%.1f kHz", hz / 1000.0f);
    else std::snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}
std::string dbName(float db) {
    char b[16];
    if (db == 0.0f) return "0 dB";
    std::snprintf(b, sizeof b, db == std::floor(db) ? "%+.0f dB" : "%+.1f dB", static_cast<double>(db));
    return b;
}
const Names<8> kLoN([](std::size_t i) { return hzName(S::kLoHz[i]); });
const Names<8> kLoMidN([](std::size_t i) { return hzName(S::kLoMidHz[i]); });
const Names<8> kHiMidN([](std::size_t i) { return hzName(S::kHiMidHz[i]); });
const Names<8> kHiN([](std::size_t i) { return hzName(S::kHiHz[i]); });
const Names<21> kGainN([](std::size_t i) { return dbName(S::kGainDb[i]); });
const char* const kOnOff[] = {"OFF", "ON"};

std::array<ParamInfo, kCount> makeParams() {
    std::array<ParamInfo, kCount> t{};
    t[Bypass] = {"bypass", "Bypass", "", 0, 1, 0, Taper::Toggle, kOnOff};
    t[MidSide] = {"ms", "M/S", "", 0, 1, 0, Taper::Toggle, kOnOff};
    static const char* const ids[2][13] = {
        {"in1", "hpf1", "hpf_hz1", "lo_f1", "lo_g1", "lo_shelf1", "lm_f1", "lm_g1", "hm_f1", "hm_g1", "hi_f1", "hi_g1", "hi_shelf1"},
        {"in2", "hpf2", "hpf_hz2", "lo_f2", "lo_g2", "lo_shelf2", "lm_f2", "lm_g2", "hm_f2", "hm_g2", "hi_f2", "hi_g2", "hi_shelf2"}};
    static const char* const names[2][13] = {
        {"Ch 1 In", "Ch 1 HP-Filt", "Ch 1 HPF Freq", "Ch 1 Lo Freq", "Ch 1 Lo Gain", "Ch 1 Lo Shelf", "Ch 1 Lo-Mid Freq", "Ch 1 Lo-Mid Gain",
         "Ch 1 Hi-Mid Freq", "Ch 1 Hi-Mid Gain", "Ch 1 Hi Freq", "Ch 1 Hi Gain", "Ch 1 Hi Shelf"},
        {"Ch 2 In", "Ch 2 HP-Filt", "Ch 2 HPF Freq", "Ch 2 Lo Freq", "Ch 2 Lo Gain", "Ch 2 Lo Shelf", "Ch 2 Lo-Mid Freq", "Ch 2 Lo-Mid Gain",
         "Ch 2 Hi-Mid Freq", "Ch 2 Hi-Mid Gain", "Ch 2 Hi Freq", "Ch 2 Hi Gain", "Ch 2 Hi Shelf"}};
    constexpr float g0 = static_cast<float>(S::kGainZero);
    for (int c = 0; c < 2; ++c) {
        auto P = [&](int p) -> ParamInfo& { return t[static_cast<std::size_t>(id(c, p))]; };
        auto I = [&](int p) { return ids[c][p - In]; };
        auto N = [&](int p) { return names[c][p - In]; };
        P(In)        = {I(In), N(In), "", 0, 1, 1, Taper::Toggle, kOnOff};
        P(HpfOn)     = {I(HpfOn), N(HpfOn), "", 0, 1, 0, Taper::Toggle, kOnOff};
        P(HpfHz)     = {I(HpfHz), N(HpfHz), "Hz", S::kHpfMinHz, S::kHpfMaxHz, S::kHpfDefaultHz, Taper::Log, nullptr};
        P(LoFreq)    = {I(LoFreq), N(LoFreq), "", 0, 7, 4, Taper::Choice, kLoN.p.data()};
        P(LoGain)    = {I(LoGain), N(LoGain), "", 0, 20, g0, Taper::Choice, kGainN.p.data()};
        P(LoShelf)   = {I(LoShelf), N(LoShelf), "", 0, 1, 0, Taper::Toggle, kOnOff};
        P(LoMidFreq) = {I(LoMidFreq), N(LoMidFreq), "", 0, 7, 5, Taper::Choice, kLoMidN.p.data()};
        P(LoMidGain) = {I(LoMidGain), N(LoMidGain), "", 0, 20, g0, Taper::Choice, kGainN.p.data()};
        P(HiMidFreq) = {I(HiMidFreq), N(HiMidFreq), "", 0, 7, 2, Taper::Choice, kHiMidN.p.data()};
        P(HiMidGain) = {I(HiMidGain), N(HiMidGain), "", 0, 20, g0, Taper::Choice, kGainN.p.data()};
        P(HiFreq)    = {I(HiFreq), N(HiFreq), "", 0, 7, 3, Taper::Choice, kHiN.p.data()};
        P(HiGain)    = {I(HiGain), N(HiGain), "", 0, 20, g0, Taper::Choice, kGainN.p.data()};
        P(HiShelf)   = {I(HiShelf), N(HiShelf), "", 0, 1, 0, Taper::Toggle, kOnOff};
    }
    return t;
}
const std::array<ParamInfo, kCount> kParams = makeParams();

// ---- analogue prototypes -------------------------------------------------------------------------
struct Proto { int type; double f0, q, gainDb; };   // type 0 bell, 1 low shelf, 2 high shelf
Proto protoOf(int band, int fi, int gi, bool shelf) {
    const double hz = kBandHz[static_cast<std::size_t>(std::clamp(band, 0, 3))][std::clamp(fi, 0, kFreqs - 1)];
    const double g = S::kGainDb[static_cast<std::size_t>(std::clamp(gi, 0, kGains - 1))];
    if (shelf && (band == 0 || band == 3)) {
        const double oct = std::pow(2.0, static_cast<double>(Q::kShelfOctaves));
        return {band == 0 ? 1 : 2, band == 0 ? hz * oct : hz / oct, Q::kShelfQ, g};
    }
    return {0, hz, stepEqQ(static_cast<float>(g)), g};
}
double analogDb(const Proto& p, double hz) {
    const double A = std::pow(10.0, p.gainDb / 40.0), w = hz / p.f0;
    if (p.type == 0) {
        const double re = 1.0 - w * w, ni = w * A / p.q, di = w / (A * p.q);
        return 10.0 * std::log10((re * re + ni * ni) / (re * re + di * di));
    }
    const double k = std::sqrt(A) / p.q * w;
    double nr, dr;
    if (p.type == 1) { nr = A - w * w; dr = 1.0 - A * w * w; }
    else             { nr = 1.0 - A * w * w; dr = A - w * w; }
    return 20.0 * std::log10(A) + 10.0 * std::log10((nr * nr + k * k) / (dr * dr + k * k));
}

// ---- digital design ------------------------------------------------------------------------------
// The analogue curve on 48 log-spaced points, 20 Hz .. min(20 kHz, 0.48 fs), with cos(w) / cos(2w) cached.
struct Grid {
    static constexpr int kN = 48;
    std::array<double, kN> hz{}, cw{}, c2w{}, target{};
};
Grid gridFor(double fs, const Proto& p) {
    Grid g;
    const double top = std::min(S::kFitTopHz, 0.48 * fs);
    for (int i = 0; i < Grid::kN; ++i) {
        const auto k = static_cast<std::size_t>(i);
        g.hz[k] = 20.0 * std::pow(top / 20.0, i / static_cast<double>(Grid::kN - 1));
        const double w = 2.0 * std::numbers::pi * g.hz[k] / fs;
        g.cw[k] = std::cos(w); g.c2w[k] = std::cos(2.0 * w);
        g.target[k] = analogDb(p, g.hz[k]);
    }
    return g;
}
inline double magDb(const fx::BiquadCoefs& c, double cw, double c2w) noexcept {
    const double num = c.b0 * c.b0 + c.b1 * c.b1 + c.b2 * c.b2 + 2.0 * (c.b0 * c.b1 + c.b1 * c.b2) * cw + 2.0 * c.b0 * c.b2 * c2w;
    const double den = 1.0 + c.a1 * c.a1 + c.a2 * c.a2 + 2.0 * (c.a1 + c.a1 * c.a2) * cw + 2.0 * c.a2 * c2w;
    return 10.0 * std::log10(std::max(num, 1e-30) / std::max(den, 1e-30));
}
double maxErr(const fx::BiquadCoefs& c, const Grid& g) {
    double m = 0;
    for (int i = 0; i < Grid::kN; ++i) m = std::max(m, std::fabs(magDb(c, g.cw[static_cast<std::size_t>(i)], g.c2w[static_cast<std::size_t>(i)]) - g.target[static_cast<std::size_t>(i)]));
    return m;
}
fx::BiquadCoefs invert(const fx::BiquadCoefs& c) { return {1.0 / c.b0, c.a1 / c.b0, c.a2 / c.b0, c.b1 / c.b0, c.b2 / c.b0}; }
// Both roots of z^2 + p z + q strictly inside the unit circle (with a margin).
bool insideUnit(double p, double q) noexcept { return std::fabs(q) < 0.9995 && std::fabs(p) < 1.0 + q - 1e-6; }

// Least-squares fit of all five coefficients to the analogue dB curve (Nelder-Mead from a good start). Poles and
// zeros are kept inside the unit circle, so the filter is stable and its exact inverse (the cut) is stable too.
fx::BiquadCoefs fit(const fx::BiquadCoefs& start, const Grid& g) {
    using V = std::array<double, 5>;
    auto toC = [](const V& x) { return fx::BiquadCoefs{x[0], x[1], x[2], x[3], x[4]}; };
    auto cost = [&](const V& x) {
        if (x[0] <= 0 || !insideUnit(x[1] / x[0], x[2] / x[0]) || !insideUnit(x[3], x[4])) return 1e30;
        const auto c = toC(x);
        double e = 0, m = 0;
        for (int i = 0; i < Grid::kN; ++i) {
            const auto k = static_cast<std::size_t>(i);
            const double d = magDb(c, g.cw[k], g.c2w[k]) - g.target[k];
            e += d * d; m = std::max(m, std::fabs(d));
        }
        return e / Grid::kN + m * m;   // mean square plus the worst point
    };
    std::array<V, 6> s{};
    s[0] = {start.b0, start.b1, start.b2, start.a1, start.a2};
    for (int k = 1; k < 6; ++k) {
        s[static_cast<std::size_t>(k)] = s[0];
        double& v = s[static_cast<std::size_t>(k)][static_cast<std::size_t>(k - 1)];
        v += 0.02 * (std::fabs(v) + 0.05);
    }
    std::array<double, 6> f{};
    for (int k = 0; k < 6; ++k) f[static_cast<std::size_t>(k)] = cost(s[static_cast<std::size_t>(k)]);
    for (int it = 0; it < 1500; ++it) {
        std::array<int, 6> o{0, 1, 2, 3, 4, 5};
        std::sort(o.begin(), o.end(), [&](int a, int b) { return f[static_cast<std::size_t>(a)] < f[static_cast<std::size_t>(b)]; });
        const auto bi = static_cast<std::size_t>(o[0]), wi = static_cast<std::size_t>(o[5]);
        V c{};
        for (int k = 0; k < 5; ++k) for (int d = 0; d < 5; ++d) c[static_cast<std::size_t>(d)] += s[static_cast<std::size_t>(o[static_cast<std::size_t>(k)])][static_cast<std::size_t>(d)] / 5.0;
        auto at = [&](double t) { V r{}; for (int d = 0; d < 5; ++d) r[static_cast<std::size_t>(d)] = c[static_cast<std::size_t>(d)] + t * (s[wi][static_cast<std::size_t>(d)] - c[static_cast<std::size_t>(d)]); return r; };
        const V xr = at(-1.0); const double fr = cost(xr);
        if (fr < f[bi]) {
            const V xe = at(-2.0); const double fe = cost(xe);
            if (fe < fr) { s[wi] = xe; f[wi] = fe; } else { s[wi] = xr; f[wi] = fr; }
        } else if (fr < f[static_cast<std::size_t>(o[4])]) {
            s[wi] = xr; f[wi] = fr;
        } else {
            const V xc = at(0.5); const double fc = cost(xc);
            if (fc < f[wi]) { s[wi] = xc; f[wi] = fc; }
            else {
                for (int k = 1; k < 6; ++k) {
                    auto& v = s[static_cast<std::size_t>(o[static_cast<std::size_t>(k)])];
                    for (int d = 0; d < 5; ++d) v[static_cast<std::size_t>(d)] = s[bi][static_cast<std::size_t>(d)] + 0.5 * (v[static_cast<std::size_t>(d)] - s[bi][static_cast<std::size_t>(d)]);
                    f[static_cast<std::size_t>(o[static_cast<std::size_t>(k)])] = cost(v);
                }
            }
        }
        if (it % 100 == 99 && f[static_cast<std::size_t>(o[5])] - f[bi] < 1e-12) break;
    }
    std::size_t bi = 0;
    for (std::size_t k = 1; k < 6; ++k) if (f[k] < f[bi]) bi = k;
    return toC(s[bi]);
}

StepEqSeDesign design(double fs, const Proto& pIn) {
    StepEqSeDesign d;
    if (std::fabs(pIn.gainDb) < 1e-9) return d;   // 0 dB: a straight wire
    // design the boost; a cut is its exact inverse (full reciprocity, EQAE-OM §1.4)
    Proto p = pIn;
    p.gainDb = std::fabs(pIn.gainDb);
    const Grid g = gridFor(fs, p);
    fx::BiquadCoefs c;
    if (p.type == 0) {
        c = p.f0 < 0.45 * fs ? matchedBell(fs, p.f0, p.q, p.gainDb)
                             : BiquadCoefs::make(BqType::HighShelf, fs, 0.35 * fs, 0.6, std::max(1.5 * g.target[Grid::kN - 1], 0.3));
    } else {
        c = BiquadCoefs::make(p.type == 1 ? BqType::LowShelf : BqType::HighShelf, fs, p.f0, p.q, p.gainDb);
    }
    double e = maxErr(c, g);
    if (e > S::kFitToleranceDb) {   // near / above half the sample rate: fit the five coefficients to the analogue curve
        fx::BiquadCoefs cf = c;
        for (int restart = 0; restart < 4; ++restart) cf = fit(cf, g);   // a fresh simplex around the last best each time
        const double ef = maxErr(cf, g);
        if (ef < e) { c = cf; e = ef; d.fitted = true; }
    }
    d.c = pIn.gainDb < 0 ? invert(c) : c;
    d.maxErrDb = e;
    return d;
}

class StepEqSeFx final : public fx::Effect {
public:
    StepEqSeFx() { setDefaults(); prepare(48000.0, kMaxBlock); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }

    void prepare(double fs, std::uint32_t) override {
        if (fs != fs_ || table_.empty()) {
            fs_ = fs;
            table_.assign(static_cast<std::size_t>(kBands * kFreqs * kGains * 2), fx::BiquadCoefs{});
            for (int b = 0; b < kBands; ++b)
                for (int f = 0; f < kFreqs; ++f)
                    for (int gi = S::kGainZero; gi < kGains; ++gi)
                        for (int sh = 0; sh < 2; ++sh) {
                            if (sh && b != 0 && b != 3) continue;
                            const auto c = design(fs, protoOf(b, f, gi, sh != 0)).c;   // the boost ...
                            table_[slot(b, f, gi, sh)] = c;
                            table_[slot(b, f, 2 * S::kGainZero - gi, sh)] = invert(c);   // ... and its mirror-image cut
                        }
        }
        reset();
    }
    void reset() noexcept override {
        for (auto& ch : bands_) for (auto& b : ch) { b.clear(); b.c = {}; }
        for (auto& h : hpf_) { h.clear(); h.c = {}; }
        for (auto& ch : last_) ch.fill(-1);
        hpfLast_.fill(-1.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        if (p(Bypass) >= 0.5f) return;   // hard bypass: output is the input
        const bool ms = p(MidSide) >= 0.5f;
        for (std::uint32_t done = 0; done < n;) {
            const std::uint32_t k = std::min(n - done, kMaxBlock);
            float* l = L + done;
            float* r = R + done;
            float* x[2] = {l, r};
            if (ms) {
                for (std::uint32_t i = 0; i < k; ++i) { mid_[i] = S::kMsEncode * (l[i] + r[i]); side_[i] = S::kMsEncode * (l[i] - r[i]); }
                x[0] = mid_.data(); x[1] = side_.data();
            }
            for (int c = 0; c < 2; ++c) {
                if (p(id(c, In)) < 0.5f) continue;   // IN out: no filter, no EQ on this channel
                update(c);
                if (p(id(c, HpfOn)) >= 0.5f) hpf_[static_cast<std::size_t>(c)].run(x[c], k);
                for (auto& b : bands_[static_cast<std::size_t>(c)]) b.run(x[c], k);
            }
            if (ms) for (std::uint32_t i = 0; i < k; ++i) { l[i] = mid_[i] + side_[i]; r[i] = mid_[i] - side_[i]; }
            done += k;
        }
    }

private:
    static std::size_t slot(int b, int f, int g, int sh) noexcept { return static_cast<std::size_t>(((b * kFreqs + f) * kGains + g) * 2 + sh); }
    void update(int c) noexcept {
        const int fp[4] = {LoFreq, LoMidFreq, HiMidFreq, HiFreq}, gp[4] = {LoGain, LoMidGain, HiMidGain, HiGain};
        for (int b = 0; b < 4; ++b) {
            const int f = std::clamp(static_cast<int>(p(id(c, fp[b])) + 0.5f), 0, kFreqs - 1);
            const int g = std::clamp(static_cast<int>(p(id(c, gp[b])) + 0.5f), 0, kGains - 1);
            const int sh = (b == 0 && p(id(c, LoShelf)) >= 0.5f) || (b == 3 && p(id(c, HiShelf)) >= 0.5f) ? 1 : 0;
            const int key = static_cast<int>(slot(b, f, g, sh));
            auto& lk = last_[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)];
            if (lk == key) continue;
            lk = key;
            bands_[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)].c = table_[static_cast<std::size_t>(key)];
        }
        const float hz = std::clamp(p(id(c, HpfHz)), S::kHpfMinHz, S::kHpfMaxHz);
        if (hz != hpfLast_[static_cast<std::size_t>(c)]) {
            hpfLast_[static_cast<std::size_t>(c)] = hz;
            hpf_[static_cast<std::size_t>(c)].c = BiquadCoefs::make(BqType::HighPass, fs_, hz, S::kHpfQ);
        }
    }

    double fs_ = 0.0;
    std::vector<fx::BiquadCoefs> table_;
    std::array<std::array<Biquad, 4>, 2> bands_{};
    std::array<Biquad, 2> hpf_{};
    std::array<std::array<int, 4>, 2> last_{};
    std::array<float, 2> hpfLast_{};
    std::array<float, kMaxBlock> mid_{}, side_{};
};

} // namespace

StepEqSeDesign stepEqSeDesign(double fs, int band, int fi, int gi, bool shelf) { return design(fs, protoOf(band, fi, gi, shelf)); }
double stepEqSeAnalogDb(int band, int fi, int gi, bool shelf, double hz) { return analogDb(protoOf(band, fi, gi, shelf), hz); }

std::unique_ptr<fx::Effect> makeStepEqSe() { return std::make_unique<StepEqSeFx>(); }

} // namespace hc::console
