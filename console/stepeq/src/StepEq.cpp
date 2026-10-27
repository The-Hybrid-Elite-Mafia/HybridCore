// HEM STEP EQ — implementation. Every frequency, step and range comes from ConsoleSpecs.hpp.
#include "hybrid/console/StepEq.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace hc::console {
namespace {

namespace S = spec::stepeq;
using namespace stepeq_ids;
using fx::ParamInfo;
using fx::Taper;

// Choice names built once (static storage; the ParamInfo table points into them).
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
const Names<7> kLoN([](std::size_t i) { return hzName(S::kLoHz[i]); });
const Names<7> kLoMidN([](std::size_t i) { return hzName(S::kLoMidHz[i]); });
const Names<7> kHiMidN([](std::size_t i) { return hzName(S::kHiMidHz[i]); });
const Names<7> kHiN([](std::size_t i) { return hzName(S::kHiHz[i]); });
const Names<11> kGainN([](std::size_t i) { char b[16]; std::snprintf(b, sizeof b, "%+.0f dB", static_cast<double>(S::kGainDb[i])); return std::string(S::kGainDb[i] == 0 ? "0 dB" : b); });
const char* const kRangeN[] = {"x1", "x.5", "x.25"};
const char* const kOnOff[] = {"OFF", "ON"};
constexpr float kGain0 = 5.0f;   // index of 0 dB

std::array<ParamInfo, kCount> makeParams() {
    std::array<ParamInfo, kCount> t{};
    t[Bypass] = {"bypass", "Bypass", "", 0, 1, 0, Taper::Toggle, kOnOff};
    static const char* const ids[2][12] = {
        {"in1", "lo_f1", "lo_g1", "lo_shelf1", "lm_f1", "lm_g1", "hm_f1", "hm_g1", "hi_f1", "hi_g1", "hi_shelf1", "range1"},
        {"in2", "lo_f2", "lo_g2", "lo_shelf2", "lm_f2", "lm_g2", "hm_f2", "hm_g2", "hi_f2", "hi_g2", "hi_shelf2", "range2"}};
    static const char* const names[2][12] = {
        {"Ch 1 In", "Ch 1 Lo Freq", "Ch 1 Lo Gain", "Ch 1 Lo Shelf", "Ch 1 Lo-Mid Freq", "Ch 1 Lo-Mid Gain", "Ch 1 Hi-Mid Freq", "Ch 1 Hi-Mid Gain", "Ch 1 Hi Freq", "Ch 1 Hi Gain", "Ch 1 Hi Shelf", "Ch 1 Range"},
        {"Ch 2 In", "Ch 2 Lo Freq", "Ch 2 Lo Gain", "Ch 2 Lo Shelf", "Ch 2 Lo-Mid Freq", "Ch 2 Lo-Mid Gain", "Ch 2 Hi-Mid Freq", "Ch 2 Hi-Mid Gain", "Ch 2 Hi Freq", "Ch 2 Hi Gain", "Ch 2 Hi Shelf", "Ch 2 Range"}};
    for (int c = 0; c < 2; ++c) {
        auto P = [&](int p) -> ParamInfo& { return t[static_cast<std::size_t>(id(c, p))]; };
        auto I = [&](int p) { return ids[c][p - 1]; };
        auto N = [&](int p) { return names[c][p - 1]; };
        P(In)        = {I(In), N(In), "", 0, 1, 1, Taper::Toggle, kOnOff};
        P(LoFreq)    = {I(LoFreq), N(LoFreq), "", 0, 6, 3, Taper::Choice, kLoN.p.data()};
        P(LoGain)    = {I(LoGain), N(LoGain), "", 0, 10, kGain0, Taper::Choice, kGainN.p.data()};
        P(LoShelf)   = {I(LoShelf), N(LoShelf), "", 0, 1, 0, Taper::Toggle, kOnOff};
        P(LoMidFreq) = {I(LoMidFreq), N(LoMidFreq), "", 0, 6, 4, Taper::Choice, kLoMidN.p.data()};
        P(LoMidGain) = {I(LoMidGain), N(LoMidGain), "", 0, 10, kGain0, Taper::Choice, kGainN.p.data()};
        P(HiMidFreq) = {I(HiMidFreq), N(HiMidFreq), "", 0, 6, 2, Taper::Choice, kHiMidN.p.data()};
        P(HiMidGain) = {I(HiMidGain), N(HiMidGain), "", 0, 10, kGain0, Taper::Choice, kGainN.p.data()};
        P(HiFreq)    = {I(HiFreq), N(HiFreq), "", 0, 6, 3, Taper::Choice, kHiN.p.data()};
        P(HiGain)    = {I(HiGain), N(HiGain), "", 0, 10, kGain0, Taper::Choice, kGainN.p.data()};
        P(HiShelf)   = {I(HiShelf), N(HiShelf), "", 0, 1, 0, Taper::Toggle, kOnOff};
        P(Range)     = {I(Range), N(Range), "", 0, 2, 0, Taper::Choice, kRangeN};
    }
    return t;
}
const std::array<ParamInfo, kCount> kParams = makeParams();

class StepEqFx final : public fx::Effect {
public:
    StepEqFx() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }

    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        for (auto& ch : bands_) for (auto& b : ch) { b.clear(); b.c = {}; }
        for (auto& ch : last_) ch.fill(-1);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        if (p(Bypass) >= 0.5f) return;   // hard bypass: output is the input
        float* x[2] = {L, R};
        for (int c = 0; c < 2; ++c) {
            if (p(id(c, In)) < 0.5f) continue;   // IN out: no EQ
            update(c);
            for (auto& b : bands_[static_cast<std::size_t>(c)]) b.run(x[c], n);
        }
    }

private:
    void update(int c) noexcept {
        const int range = static_cast<int>(p(id(c, Range)));
        struct B { int fp, gp, shelfP; const float* hz; BqType shelf; };
        const B bs[4] = {{LoFreq, LoGain, LoShelf, S::kLoHz.data(), BqType::LowShelf},
                         {LoMidFreq, LoMidGain, -1, S::kLoMidHz.data(), BqType::Bell},
                         {HiMidFreq, HiMidGain, -1, S::kHiMidHz.data(), BqType::Bell},
                         {HiFreq, HiGain, HiShelf, S::kHiHz.data(), BqType::HighShelf}};
        for (int b = 0; b < 4; ++b) {
            const int f = static_cast<int>(p(id(c, bs[b].fp))), g = static_cast<int>(p(id(c, bs[b].gp)));
            const int sh = bs[b].shelfP >= 0 && p(id(c, bs[b].shelfP)) >= 0.5f ? 1 : 0;
            const int key = ((f * 16 + g) * 4 + range) * 2 + sh;
            auto& lk = last_[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)];
            if (lk == key) continue;
            lk = key;
            const float hz = bs[b].hz[f];
            const float eff = stepEqEffectiveGainDb(g, range);
            BiquadCoefs cf;
            if (sh) {
                const float oct = std::pow(2.0f, S::kShelfOctaves);
                const float mid = bs[b].shelf == BqType::LowShelf ? hz * oct : hz / oct;
                cf = BiquadCoefs::make(bs[b].shelf, fs_, mid, S::kShelfQ, eff);
            } else {
                cf = BiquadCoefs::make(BqType::Bell, fs_, hz, stepEqQ(eff), eff);
            }
            bands_[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)].c = cf;
        }
    }

    double fs_ = 48000.0;
    std::array<std::array<Biquad, 4>, 2> bands_{};
    std::array<std::array<int, 4>, 2> last_{};
};

} // namespace

float stepEqEffectiveGainDb(int gainIndex, int rangeIndex) noexcept {
    gainIndex = std::clamp(gainIndex, 0, 10);
    rangeIndex = std::clamp(rangeIndex, 0, 2);
    return S::kGainDb[static_cast<std::size_t>(gainIndex)] * S::kRange[static_cast<std::size_t>(rangeIndex)];
}
float stepEqQ(float effectiveGainDb) noexcept { return S::kQAtZero + S::kQPerDb * std::fabs(effectiveGainDb); }

std::unique_ptr<fx::Effect> makeStepEq() { return std::make_unique<StepEqFx>(); }

} // namespace hc::console
