// HEM TWIN BUS — implementation. Every number comes from hybrid/console/ConsoleSpecs.hpp.
#include "hybrid/console/TwinBus.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace hc::console {
namespace {

namespace S = spec::twinbus;
using namespace twinbus_ids;
using fx::ParamInfo;
using fx::Taper;

template <std::size_t N>
struct Names {
    std::array<std::string, N> s;
    std::array<const char*, N> p{};
    template <typename F> explicit Names(F f) { for (std::size_t i = 0; i < N; ++i) { s[i] = f(i); p[i] = s[i].c_str(); } }
};
std::string fmt(const char* f, double v) { char b[32]; std::snprintf(b, sizeof b, f, v); return b; }
std::string hzName(float hz) { return hz >= 1000.0f ? fmt(hz == std::floor(hz / 1000.0f) * 1000.0f ? "%.0f kHz" : "%.1f kHz", hz / 1000.0) : fmt("%.0f Hz", hz); }

const char* const kOnOff[] = {"OFF", "ON"};
const char* const kModes[] = {"CLASSIC STEREO", "SUM S/C STEREO", "DUAL MONO", "MID-SIDE"};
const char* const kExtSc[] = {"OFF", "COMP", "D-EQ", "BOTH"};
const char* const kExtBands[] = {"LF + HF", "HF ONLY", "LF ONLY"};
const char* const kMixModes[] = {"CLASSIC", "PARALLEL"};
const char* const kTrims[] = {"0 dB", "+10 dB", "-5 dB"};
const char* const kSoloModes[] = {"IN PHASE", "DECODED"};
const char* const kTimes[] = {"NOMINAL", "FAST", "AUTO"};
const Names<31> kThrN([](std::size_t i) { return fmt("%+.1f dB", twinBusThresholdDb(static_cast<int>(i))); });
const Names<11> kAttN([](std::size_t i) { return fmt(S::kAttackMs[i] < 1 ? "%.1f ms" : "%.0f ms", S::kAttackMs[i]); });
const Names<11> kRatioN([](std::size_t i) { return fmt(S::kRatio[i] == std::floor(S::kRatio[i]) ? "%.0f:1" : "%.1f:1", S::kRatio[i]); });
const Names<11> kRelN([](std::size_t i) { return i < 9 ? fmt("%.2f s", S::kReleaseS[i]) : std::string(i == 9 ? "AUTO" : "AUTO 2"); });
const Names<31> kMkN([](std::size_t i) { return fmt("%+.0f dB", twinBusMakeupDb(static_cast<int>(i), false)); });
const Names<31> kScN([](std::size_t i) { return i == 0 ? std::string("OUT") : fmt("%.0f Hz", 10.0 * static_cast<double>(i)); });
const Names<31> kMixN([](std::size_t i) { return i == 0 ? std::string("DRY") : i == 30 ? std::string("WET") : fmt("%.0f %%", 100.0 * static_cast<double>(i) / 30.0); });
const Names<9> kGritN([](std::size_t i) { return fmt("%.0f", static_cast<double>(i + 1)); });
const Names<31> kCeN([](std::size_t i) { const int v = static_cast<int>(i) - 15; return v == 0 ? std::string("0") : (v < 0 ? "COMP " : "EXP ") + std::to_string(std::abs(v)); });
const Names<16> kLfN([](std::size_t i) { return hzName(S::kLfHz[i]); });
const Names<16> kHfN([](std::size_t i) { return hzName(S::kHfShelfHz[i]) + " / " + hzName(S::kHfBellHz[i]); });
const Names<23> kRangeN([](std::size_t i) { return fmt("%.1f dB", twinBusRangeDb(static_cast<int>(i))); });
const Names<31> kSgN([](std::size_t i) { return fmt("%+.1f dB", twinBusStaticGainDb(static_cast<int>(i))); });

std::array<ParamInfo, kCount> makeParams() {
    std::array<ParamInfo, kCount> t{};
    t[Mode]         = {"mode", "Mode", "", 0, 3, 0, Taper::Choice, kModes};
    t[ExtSc]        = {"ext_sc", "Ext S/C", "", 0, 3, 0, Taper::Choice, kExtSc};
    t[ExtScBands]   = {"ext_sc_bands", "Ext S/C D-EQ Bands", "", 0, 2, 0, Taper::Choice, kExtBands};
    t[MixMode]      = {"mix_mode", "Mix Mode", "", 0, 1, 0, Taper::Choice, kMixModes};
    t[InputTrim]    = {"in_trim", "Input Trim", "", 0, 2, 0, Taper::Choice, kTrims};
    t[Lpf]          = {"lpf", "Input LPF 26 kHz", "", 0, 1, 0, Taper::Toggle, kOnOff};
    t[SideSoloMode] = {"side_solo_mode", "Side Solo Mode", "", 0, 1, 0, Taper::Choice, kSoloModes};
    t[MidSolo]      = {"mid_solo", "Mid Solo", "", 0, 1, 0, Taper::Toggle, kOnOff};
    t[SideSolo]     = {"side_solo", "Side Solo", "", 0, 1, 0, Taper::Toggle, kOnOff};
    t[RightMeter]   = {"right_meter", "Right Meter In Stereo", "", 0, 1, 0, Taper::Toggle, kOnOff};
    static std::array<std::array<std::string, kSideStride * 2>, 2> idStore, nameStore;
    struct Def { const char* id; const char* name; ParamInfo info; };
    const Def defs[kSideStride] = {
        {"in", "In", {nullptr, nullptr, "", 0, 1, 1, Taper::Toggle, kOnOff}},
        {"thr", "Threshold", {nullptr, nullptr, "", 0, 30, 15, Taper::Choice, kThrN.p.data()}},
        {"att", "Attack", {nullptr, nullptr, "", 0, 10, 4, Taper::Choice, kAttN.p.data()}},
        {"ratio", "Ratio", {nullptr, nullptr, "", 0, 10, 4, Taper::Choice, kRatioN.p.data()}},
        {"rel", "Release", {nullptr, nullptr, "", 0, 10, 9, Taper::Choice, kRelN.p.data()}},
        {"makeup", "Make-Up", {nullptr, nullptr, "", 0, 30, 10, Taper::Choice, kMkN.p.data()}},
        {"fine", "Make-Up Fine", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"post_deq", "Post D-EQ", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"sc_hpf", "S/C HPF", {nullptr, nullptr, "", 0, 30, 0, Taper::Choice, kScN.p.data()}},
        {"mix", "Mix", {nullptr, nullptr, "", 0, 30, 30, Taper::Choice, kMixN.p.data()}},
        {"low_thd", "Low THD", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"fb", "F/B", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"grit", "Grit", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"grit_lvl", "Grit Level", {nullptr, nullptr, "", 0, 8, S::kGritDefault - 1, Taper::Choice, kGritN.p.data()}},
        {"lf_on", "D-EQ LF On", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"lf_ce", "D-EQ LF Comp/Exp", {nullptr, nullptr, "", 0, 30, 15, Taper::Choice, kCeN.p.data()}},
        {"lf_freq", "D-EQ LF Freq", {nullptr, nullptr, "", 0, 15, S::kLfDefault, Taper::Choice, kLfN.p.data()}},
        {"lf_range", "D-EQ LF Range", {nullptr, nullptr, "", 0, 22, 22, Taper::Choice, kRangeN.p.data()}},
        {"lf_gain", "LF Gain", {nullptr, nullptr, "", 0, 30, 15, Taper::Choice, kSgN.p.data()}},
        {"reso", "LF Reso", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"lf_time", "LF Fast", {nullptr, nullptr, "", 0, 2, 0, Taper::Choice, kTimes}},
        {"hf_on", "D-EQ HF On", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"hf_ce", "D-EQ HF Comp/Exp", {nullptr, nullptr, "", 0, 30, 15, Taper::Choice, kCeN.p.data()}},
        {"hf_freq", "D-EQ HF Freq", {nullptr, nullptr, "", 0, 15, S::kHfShelfDefault, Taper::Choice, kHfN.p.data()}},
        {"hf_range", "D-EQ HF Range", {nullptr, nullptr, "", 0, 22, 22, Taper::Choice, kRangeN.p.data()}},
        {"hf_bell", "HF Bell", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
        {"hf_time", "HF Fast", {nullptr, nullptr, "", 0, 2, 0, Taper::Choice, kTimes}},
        {"tex", "Transient Expander", {nullptr, nullptr, "", 0, 1, 0, Taper::Toggle, kOnOff}},
    };
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < kSideStride; ++k) {
            auto& idS = idStore[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)];
            auto& nmS = nameStore[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)];
            idS = std::string(defs[k].id) + (s ? "2" : "1");
            nmS = std::string(s ? "R " : "L ") + defs[k].name;
            ParamInfo pi = defs[k].info;
            pi.id = idS.c_str();
            pi.name = nmS.c_str();
            t[static_cast<std::size_t>(kGlobalCount + s * kSideStride + k)] = pi;
        }
    return t;
}
const std::array<ParamInfo, kCount> kParams = makeParams();
const fx::MeterInfo kMeters[] = {
    {"GR L", "dB", -20, 0}, {"GR R", "dB", -20, 0},
    {"D-EQ LF L", "dB", -15, 15, true}, {"D-EQ HF L", "dB", -15, 15, true},
    {"D-EQ LF R", "dB", -15, 15, true}, {"D-EQ HF R", "dB", -15, 15, true},
    {"OVER L", "", 0, 1}, {"OVER R", "", 0, 1},
};

[[nodiscard]] inline int ix(float v) noexcept { return static_cast<int>(v + 0.5f); }

// ---- one processing side: compressor + D-EQ for 1 (dual / M-S) or 2 (stereo modes) channels -----
struct Side {
    double fs = 48000;
    // compressor
    Biquad scHpfL, scHpfR;
    int lastScPos = -1;
    DualRate gr;
    int lastRel = -1, lastAtt = -1;
    float hold = 0; int holdLeft = 0;
    float prevWet[2]{0, 0};
    fx::Oversampler4 osUp[2], osDn[2];
    std::vector<float> up[2];
    fx::DelayLine plain[2], dry[2];
    FirstOrder dc[2];
    std::vector<float> wet[2], dryBuf[2];
    float grMax = 0;
    // D-EQ
    struct Band {
        FirstOrder f1[2], k1[2];          // 1st-order shelf band (signal / key)
        Biquad f2[2], k2[2];              // 2nd-order band (RESO low-pass or HF bell band-pass)
        DualRate env;                      // dynamic amount (dB magnitude)
        int lastKey = -1, lastTime = -1;
        float bellGainDb = 1e9f;
        float act = 0;
        void clear() noexcept { for (int c = 0; c < 2; ++c) { f1[c].clear(); k1[c].clear(); f2[c].clear(); k2[c].clear(); } env.clear(); act = 0; }
    } lf, hf;
    float teFast = 0, teSlow = 0, teFa = 0, teFr = 0, teSa = 0, teSr = 0;

    void prepare(double rate, std::uint32_t maxBlock) {
        fs = rate;
        for (int c = 0; c < 2; ++c) {
            up[c].assign(static_cast<std::size_t>(maxBlock) * fx::Oversampler4::kFactor, 0.0f);
            plain[c].prepare(fx::Oversampler4::latency());
            dry[c].prepare(fx::Oversampler4::latency());
            dc[c].set(FirstOrder::Type::HighPass, fs, 5.0);
            wet[c].assign(maxBlock, 0.0f);
            dryBuf[c].assign(maxBlock, 0.0f);
        }
        teFa = coefMs(fs, S::kTeFastAttackMs); teFr = coefMs(fs, S::kTeFastReleaseMs);
        teSa = coefMs(fs, S::kTeSlowAttackMs); teSr = coefMs(fs, S::kTeSlowReleaseMs);
        clear();
    }
    void clear() noexcept {
        scHpfL.clear(); scHpfR.clear(); lastScPos = -1;
        gr.clear(); lastRel = lastAtt = -1;
        hold = 0; holdLeft = 0; prevWet[0] = prevWet[1] = 0;
        for (int c = 0; c < 2; ++c) { osUp[c].clear(); osDn[c].clear(); plain[c].clear(); dry[c].clear(); dc[c].clear(); }
        lf.clear(); hf.clear(); lf.lastKey = hf.lastKey = -1; lf.lastTime = hf.lastTime = -1; hf.bellGainDb = 1e9f;
        teFast = teSlow = 0;
    }
};

struct SideParams {   // one side's controls, read once per block
    bool in, fine, postDeq, lowThd, fb, grit, lfOn, reso, hfOn, hfBell, tex;
    int thr, att, ratio, rel, makeup, scHpf, mix, gritLvl, lfCe, lfFreq, lfRange, lfGain, lfTime, hfCe, hfFreq, hfRange, hfTime;
};

class TwinBusFx final : public fx::Effect {
public:
    TwinBusFx() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const fx::MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return fx::Oversampler4::latency(); }

    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        maxBlock_ = std::max<std::uint32_t>(1, std::min(maxBlock, kMaxBlock));
        for (auto& s : side_) s.prepare(fs, maxBlock_);
        for (auto* v : {&a_, &b_, &ka_, &kb_}) v->assign(maxBlock_, 0.0f);
        for (int c = 0; c < 2; ++c) { lpf1_[c].set(FirstOrder::Type::LowPass, fs, S::kLpfHz); }
        lpf2_ = BiquadCoefs::make(BqType::LowPass, fs, S::kLpfHz, 1.0);   // with the 1st order = 3rd-order Butterworth
        reset();
    }
    void reset() noexcept override {
        for (auto& s : side_) s.clear();
        for (int c = 0; c < 2; ++c) { lpf1_[c].clear(); lpfB_[c].clear(); lpfB_[c].c = lpf2_; }
        for (int i = 0; i < 8; ++i) publishMeter(i, 0.0f);
        publishReduction(0.0f);
    }
    void setKey(const float* l, const float* r) noexcept { keyL_ = l; keyR_ = r; }

    void process(float* L, float* R, std::uint32_t frames) noexcept override {
        std::uint32_t done = 0;
        while (done < frames) {
            const std::uint32_t n = std::min(frames - done, maxBlock_);
            block(L + done, R + done, keyL_ ? keyL_ + done : nullptr, keyR_ ? keyR_ + done : nullptr, n);
            done += n;
        }
        keyL_ = keyR_ = nullptr;   // a key is valid for one call
    }

private:
    SideParams read(int s) const noexcept {
        auto P = [&](int q) { return p(id(s, q)); };
        auto B = [&](int q) { return P(q) >= 0.5f; };
        auto I = [&](int q) { return ix(P(q)); };
        return {B(In), B(Fine), B(PostDeq), B(LowThd), B(Feedback), B(Grit), B(LfOn), B(Reso), B(HfOn), B(HfBell), B(TransientExp),
                I(Threshold), I(Attack), I(Ratio), I(Release), I(Makeup), I(ScHpf), I(Mix), I(GritLevel),
                I(LfCompExp), I(LfFreq), I(LfRange), I(LfGain), I(LfTime), I(HfCompExp), I(HfFreq), I(HfRange), I(HfTime)};
    }

    void block(float* L, float* R, const float* kL, const float* kR, std::uint32_t n) noexcept {
        const int mode = ix(p(Mode));
        const bool stereo = mode <= 1;
        // ---- input stage: trim, optional 26 kHz LPF (only meaningful when it sits below Nyquist) ----
        const float trim = dbToLin(S::kInputTrimDb[static_cast<std::size_t>(std::clamp(ix(p(InputTrim)), 0, 2))]);
        const bool lpf = p(Lpf) >= 0.5f && S::kLpfHz < 0.45 * fs_;
        float* io[2] = {L, R};
        for (int c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < n; ++i) {
                float x = io[c][i] * trim;
                if (lpf) x = lpfB_[c].tick(static_cast<float>(lpf1_[c].tick(x)));
                io[c][i] = x;
            }
        // ---- mode encode ------------------------------------------------------------------------
        float* A = a_.data();
        float* Bv = b_.data();
        if (mode == 3) for (std::uint32_t i = 0; i < n; ++i) { A[i] = (L[i] + R[i]) * 0.5f; Bv[i] = (L[i] - R[i]) * 0.5f; }
        else { std::copy_n(L, n, A); std::copy_n(R, n, Bv); }
        const float* KA = nullptr; const float* KB = nullptr;
        if (kL) {
            const float* kr = kR ? kR : kL;
            if (mode == 3) { for (std::uint32_t i = 0; i < n; ++i) { ka_[i] = (kL[i] + kr[i]) * 0.5f; kb_[i] = (kL[i] - kr[i]) * 0.5f; } }
            else { std::copy_n(kL, n, ka_.data()); std::copy_n(kr, n, kb_.data()); }
            KA = ka_.data(); KB = kb_.data();
        }

        const SideParams s1 = read(0), s2 = read(1);
        const float hfStaticStereo = twinBusStaticGainDb(s2.lfGain);   // side 2's LF/HF GAIN = HF gain in stereo modes
        float over[2] = {0, 0};
        if (stereo) {
            if (s1.in) runSide(side_[0], s1, A, Bv, KA, KB, n, mode, hfStaticStereo);
            else side_[0].grMax = 0;
            side_[1].grMax = 0; side_[1].lf.act = side_[1].hf.act = 0;
        } else {
            if (s1.in) runSide(side_[0], s1, A, nullptr, KA, nullptr, n, mode, 0.0f); else side_[0].grMax = 0;
            if (s2.in) runSide(side_[1], s2, Bv, nullptr, KB, nullptr, n, mode, 0.0f); else side_[1].grMax = 0;
        }
        over[0] = linToDb(peakOf(A, n)) > S::kOverloadDbfs ? 1.0f : 0.0f;
        over[1] = linToDb(peakOf(Bv, n)) > S::kOverloadDbfs ? 1.0f : 0.0f;

        // ---- decode / solo ----------------------------------------------------------------------
        if (mode == 3) {
            const bool midSolo = p(MidSolo) >= 0.5f, sideSolo = p(SideSolo) >= 0.5f, decoded = p(SideSoloMode) >= 0.5f;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float m = A[i], sd = Bv[i];
                if (midSolo && !sideSolo) { L[i] = R[i] = m; }
                else if (sideSolo && !midSolo) { L[i] = sd; R[i] = decoded ? -sd : sd; }
                else { L[i] = m + sd; R[i] = m - sd; }
            }
        } else { std::copy_n(A, n, L); std::copy_n(Bv, n, R); }

        // ---- meters -----------------------------------------------------------------------------
        publishMeter(MeterGr1, -side_[0].grMax);
        publishMeter(MeterGr2, stereo ? -side_[0].grMax : -side_[1].grMax);
        publishMeter(MeterLf1, side_[0].lf.act); publishMeter(MeterHf1, side_[0].hf.act);
        publishMeter(MeterLf2, stereo ? side_[0].lf.act : side_[1].lf.act);
        publishMeter(MeterHf2, stereo ? side_[0].hf.act : side_[1].hf.act);
        publishMeter(MeterOver1, over[0]); publishMeter(MeterOver2, stereo ? std::max(over[0], over[1]) : over[1]);
        publishReduction(-std::max(side_[0].grMax, side_[1].grMax));
    }

    // One side over 1 or 2 channels (b == nullptr: 1 channel).
    void runSide(Side& sd, const SideParams& sp, float* a, float* b, const float* ka, const float* kb,
                 std::uint32_t n, int mode, float hfStatic) noexcept {
        const int ext = ix(p(ExtSc));
        const bool compKey = (ext == 1 || ext == 3) && ka;
        const bool deqKey = (ext == 2 || ext == 3) && ka;
        const int extBands = ix(p(ExtScBands));
        if (!sp.postDeq) { comp(sd, sp, a, b, compKey ? ka : nullptr, compKey ? kb : nullptr, n, mode); deq(sd, sp, a, b, deqKey ? ka : nullptr, deqKey ? kb : nullptr, extBands, n, hfStatic); }
        else             { deq(sd, sp, a, b, deqKey ? ka : nullptr, deqKey ? kb : nullptr, extBands, n, hfStatic); comp(sd, sp, a, b, compKey ? ka : nullptr, compKey ? kb : nullptr, n, mode); }
    }

    // ---- compressor -------------------------------------------------------------------------------
    void comp(Side& sd, const SideParams& sp, float* a, float* b, const float* ka, const float* kb, std::uint32_t n, int mode) noexcept {
        const int nch = b ? 2 : 1;
        float* x[2] = {a, b};
        // time constants
        if (sp.att != sd.lastAtt || sp.rel != sd.lastRel) {
            const double att = S::kAttackMs[static_cast<std::size_t>(std::clamp(sp.att, 0, 10))];
            if (sp.rel <= 8) sd.gr.set(fs_, att, S::kReleaseS[static_cast<std::size_t>(sp.rel)] * 1000.0, false, S::kAutoChargeMs, S::kAuto1SlowMs);
            else if (sp.rel == 9) sd.gr.set(fs_, att, S::kAuto1FastMs, true, S::kAutoChargeMs, S::kAuto1SlowMs);
            else sd.gr.set(fs_, att, S::kAuto2FastMs, true, S::kAutoChargeMs, S::kAuto2SlowMs);
            sd.lastAtt = sp.att; sd.lastRel = sp.rel;
        }
        if (sp.scHpf != sd.lastScPos) {
            if (sp.scHpf > 0) {
                const auto c = BiquadCoefs::make(BqType::HighPass, fs_, 10.0 * sp.scHpf, S::kScHpfQ);
                sd.scHpfL.c = c; sd.scHpfR.c = c;
            }
            sd.scHpfL.clear(); sd.scHpfR.clear();
            sd.lastScPos = sp.scHpf;
        }
        const float thr = twinBusThresholdDb(sp.thr) - spec::kBusDbuAtFullScale;
        const float ratio = S::kRatio[static_cast<std::size_t>(std::clamp(sp.ratio, 0, 10))];
        const float makeup = dbToLin(twinBusMakeupDb(sp.makeup, sp.fine));
        const int holdN = static_cast<int>(S::kLowThdHoldMs * 0.001 * fs_);
        const bool sumSc = mode == 1 && nch == 2;
        const bool grit = sp.grit;
        const float k2 = S::kGrit2ndPerLevel * static_cast<float>(sp.gritLvl + 1);
        const float k3 = S::kGrit3rdPerLevel * static_cast<float>(sp.gritLvl + 1);
        float grMax = 0.0f;

        for (std::uint32_t i = 0; i < n; ++i) {
            // side-chain source: key, feedback (previous VCA output) or the input
            float sa, sb;
            if (ka) { sa = ka[i]; sb = kb ? kb[i] : ka[i]; }
            else if (sp.fb) { sa = sd.prevWet[0]; sb = sd.prevWet[1]; }
            else { sa = a[i]; sb = b ? b[i] : a[i]; }
            if (sp.scHpf > 0) { sa = sd.scHpfL.tick(sa); sb = sd.scHpfR.tick(sb); }
            float det = nch == 2 ? (sumSc ? std::fabs(sa + sb) * S::kSumScGain : std::max(std::fabs(sa), std::fabs(sb))) : std::fabs(sa);
            if (sp.lowThd) {   // hold the detector over a low-frequency cycle (ASSUMED model of LOW THD)
                if (det >= sd.hold) { sd.hold = det; sd.holdLeft = holdN; }
                else if (sd.holdLeft > 0) --sd.holdLeft;
                else sd.hold = det;
                det = sd.hold;
            }
            const float target = -compGainDb(linToDb(det), thr, ratio, S::kKneeDb);
            const float g = sd.gr.tick(target);
            grMax = std::max(grMax, g);
            const float vca = dbToLin(-g);
            for (int c = 0; c < nch; ++c) {
                const float w = x[c][i] * vca;
                sd.prevWet[c] = w;
                sd.wet[c][i] = w;
                sd.dryBuf[c][i] = x[c][i];
            }
        }
        sd.grMax = grMax;
        // GRIT (4x oversampled VCA colour) or the matching delay, so the latency never changes
        for (int c = 0; c < nch; ++c) {
            float* w = sd.wet[c].data();
            if (grit) {
                sd.osUp[c].up(w, sd.up[c].data(), n);
                float* u = sd.up[c].data();
                for (std::uint32_t j = 0; j < n * fx::Oversampler4::kFactor; ++j) { const float v = u[j]; u[j] = v + k2 * v * v + k3 * v * v * v; }
                sd.osDn[c].down(u, w, n);
                for (std::uint32_t i = 0; i < n; ++i) w[i] = static_cast<float>(sd.dc[c].tick(w[i]));
            } else {
                for (std::uint32_t i = 0; i < n; ++i) w[i] = sd.plain[c].process(w[i], fx::Oversampler4::latency());
                sd.osUp[c].clear(); sd.osDn[c].clear(); sd.dc[c].clear();
            }
            // dry path delayed to match; MIX (classic cross-fade or parallel) then make-up on the wet
            const float m = static_cast<float>(sp.mix) / 30.0f;
            const bool parallel = p(MixMode) >= 0.5f;
            const float gw = m, gd = parallel ? (sp.mix >= 30 ? 0.0f : 1.0f) : 1.0f - m;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float dryS = sd.dry[c].process(sd.dryBuf[c][i], fx::Oversampler4::latency());
                x[c][i] = dryS * gd + w[i] * makeup * gw;
            }
        }
    }

    // ---- dynamic EQ -------------------------------------------------------------------------------
    void setTimes(Side::Band& bd, int time, bool lfBand) noexcept {
        if (time == bd.lastTime) return;
        bd.lastTime = time;
        if (time == 2) bd.env.set(fs_, S::kAutoAttackMs, S::kAuto1FastMs, true, S::kAutoChargeMs, S::kAuto1SlowMs);
        else if (time == 1) bd.env.set(fs_, lfBand ? S::kLfFastAttackMs : S::kHfFastAttackMs, lfBand ? S::kLfFastReleaseMs : S::kHfFastReleaseMs, false, 1, 1);
        else bd.env.set(fs_, lfBand ? S::kLfAttackMs : S::kHfAttackMs, lfBand ? S::kLfReleaseMs : S::kHfReleaseMs, false, 1, 1);
    }

    // Dynamic gain change (dB, signed) for a band level x (dBFS): COMP cuts, EXP lifts, both limited by RANGE.
    static float dynTarget(float x, int ce, int range) noexcept {
        const int v = ce - 15;
        if (v == 0) return 0.0f;
        const float thr = S::kDeqThreshTopDbu - S::kDeqThreshTravelDb * static_cast<float>(std::abs(v) - 1) / 14.0f - spec::kBusDbuAtFullScale;
        const float amt = std::min(std::max(0.0f, x - thr) * S::kDeqSlope, twinBusRangeDb(range));
        return amt;   // magnitude; COMP/EXP sign is applied by the caller
    }

    void deq(Side& sd, const SideParams& sp, float* a, float* b, const float* ka, const float* kb, int extBands,
             std::uint32_t n, float hfStatic) noexcept {
        const int nch = b ? 2 : 1;
        float* x[2] = {a, b};
        const float* k[2] = {ka, kb ? kb : ka};
        // ---- LF band (1st-order shelf, or 2nd-order "RESO") ----
        if (sp.lfOn) {
            auto& bd = sd.lf;
            setTimes(bd, sp.lfTime, true);
            const int key = sp.lfFreq * 2 + (sp.reso ? 1 : 0);
            if (key != bd.lastKey) {
                const float hz = S::kLfHz[static_cast<std::size_t>(std::clamp(sp.lfFreq, 0, 15))];
                for (int c = 0; c < 2; ++c) {
                    bd.f1[c].set(FirstOrder::Type::LowPass, fs_, hz); bd.k1[c].set(FirstOrder::Type::LowPass, fs_, hz);
                    const auto q = BiquadCoefs::make(BqType::LowPass, fs_, hz, S::kResoQ);
                    bd.f2[c].c = q; bd.k2[c].c = q;
                }
                bd.lastKey = key;
            }
            const bool useKey = ka && extBands != 1;   // "HF ONLY" leaves LF on its own signal
            const float stat = twinBusStaticGainDb(sp.lfGain);
            const float sign = sp.lfCe < 15 ? -1.0f : 1.0f;
            float actMax = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                float band[2] = {0, 0}, det = 0;
                for (int c = 0; c < nch; ++c) {
                    band[c] = sp.reso ? bd.f2[c].tick(x[c][i]) : static_cast<float>(bd.f1[c].tick(x[c][i]));
                    const float kv = useKey ? (sp.reso ? bd.k2[c].tick(k[c][i]) : static_cast<float>(bd.k1[c].tick(k[c][i]))) : band[c];
                    det = std::max(det, std::fabs(kv));
                }
                const float dyn = bd.env.tick(dynTarget(linToDb(det), sp.lfCe, sp.lfRange)) * sign;
                actMax = std::fabs(dyn) > std::fabs(actMax) ? dyn : actMax;
                const float g = dbToLin(stat + dyn) - 1.0f;
                for (int c = 0; c < nch; ++c) x[c][i] += g * band[c];
            }
            bd.act = actMax;
        } else { sd.lf.act = 0.0f; sd.lf.env.clear(); }

        // ---- HF band (1st-order shelf or proportional-Q bell) — or the transient expander --------
        if (sp.tex) {
            const float amt = static_cast<float>(std::max(0, sp.hfCe - 15)) / 15.0f;   // 0 (12 o'clock) .. EXP
            float boostMax = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                float lv = 0;
                for (int c = 0; c < nch; ++c) lv = std::max(lv, std::fabs(x[c][i]));
                sd.teFast = lv + (lv > sd.teFast ? sd.teFa : sd.teFr) * (sd.teFast - lv);
                sd.teSlow = lv + (lv > sd.teSlow ? sd.teSa : sd.teSr) * (sd.teSlow - lv);
                const float boost = amt * std::clamp(linToDb(sd.teFast) - linToDb(sd.teSlow), 0.0f, S::kTeMaxBoostDb);
                boostMax = std::max(boostMax, boost);
                const float g = dbToLin(boost);
                for (int c = 0; c < nch; ++c) x[c][i] *= g;
            }
            sd.hf.act = boostMax;
            return;
        }
        if (!sp.hfOn) { sd.hf.act = 0.0f; sd.hf.env.clear(); return; }
        auto& bd = sd.hf;
        setTimes(bd, sp.hfTime, false);
        const bool bell = sp.hfBell;
        const int fi = std::clamp(sp.hfFreq, 0, 15);
        const float hz = bell ? S::kHfBellHz[static_cast<std::size_t>(fi)] : S::kHfShelfHz[static_cast<std::size_t>(fi)];
        const int key = fi * 2 + (bell ? 1 : 0);
        if (key != bd.lastKey) {
            for (int c = 0; c < 2; ++c) { bd.f1[c].set(FirstOrder::Type::HighPass, fs_, hz); bd.k1[c].set(FirstOrder::Type::HighPass, fs_, hz); }
            bd.lastKey = key;
            bd.bellGainDb = 1e9f;
        }
        const bool useKey = ka && extBands != 2;   // "LF ONLY" leaves HF on its own signal
        const float sign = sp.hfCe < 15 ? -1.0f : 1.0f;
        float actMax = 0.0f;
        constexpr std::uint32_t kCtl = 16;   // bell Q follows the gain at control rate
        for (std::uint32_t i = 0; i < n; ++i) {
            if (bell && (i % kCtl) == 0) {
                const float gdb = hfStatic + bd.act;
                if (std::fabs(gdb - bd.bellGainDb) > 0.05f) {
                    const auto q = BiquadCoefs::make(BqType::BandPass, fs_, hz, twinBusBellQ(gdb));
                    for (int c = 0; c < 2; ++c) { bd.f2[c].c = q; bd.k2[c].c = q; }
                    bd.bellGainDb = gdb;
                }
            }
            float band[2] = {0, 0}, det = 0;
            for (int c = 0; c < nch; ++c) {
                band[c] = bell ? bd.f2[c].tick(x[c][i]) : static_cast<float>(bd.f1[c].tick(x[c][i]));
                const float kv = useKey ? (bell ? bd.k2[c].tick(k[c][i]) : static_cast<float>(bd.k1[c].tick(k[c][i]))) : band[c];
                det = std::max(det, std::fabs(kv));
            }
            const float dyn = bd.env.tick(dynTarget(linToDb(det), sp.hfCe, sp.hfRange)) * sign;
            bd.act = dyn;
            actMax = std::fabs(dyn) > std::fabs(actMax) ? dyn : actMax;
            const float g = dbToLin(hfStatic + dyn) - 1.0f;
            for (int c = 0; c < nch; ++c) x[c][i] += g * band[c];
        }
        bd.act = actMax;
    }

    double fs_ = 48000.0;
    std::uint32_t maxBlock_ = 512;
    std::array<Side, 2> side_;
    std::vector<float> a_, b_, ka_, kb_;
    FirstOrder lpf1_[2];
    Biquad lpfB_[2];
    BiquadCoefs lpf2_;
    const float* keyL_ = nullptr;
    const float* keyR_ = nullptr;
};

} // namespace

void twinBusSetKey(fx::Effect& e, const float* l, const float* r) noexcept {
    if (auto* t = dynamic_cast<TwinBusFx*>(&e)) t->setKey(l, r);
}

float twinBusThresholdDb(int i) noexcept {
    i = std::clamp(i, 0, S::kThreshSteps - 1);
    return S::kThreshMinDb + (S::kThreshMaxDb - S::kThreshMinDb) * static_cast<float>(i) / static_cast<float>(S::kThreshSteps - 1);
}
float twinBusMakeupDb(int i, bool fine) noexcept {
    i = std::clamp(i, 0, 30);
    return fine ? S::kMakeupFineMinDb + 0.5f * static_cast<float>(i) : S::kMakeupMinDb + static_cast<float>(i);
}
float twinBusStaticGainDb(int i) noexcept {
    i = std::clamp(i, 0, 30);
    const int v = i - 15;
    const float a = static_cast<float>(std::abs(v));
    float db = 0.0f;
    for (std::size_t k = 1; k < S::kStaticGainMarkSteps.size(); ++k)
        if (a <= S::kStaticGainMarkSteps[k]) {
            const float t = (a - S::kStaticGainMarkSteps[k - 1]) / (S::kStaticGainMarkSteps[k] - S::kStaticGainMarkSteps[k - 1]);
            db = S::kStaticGainMarkDb[k - 1] + t * (S::kStaticGainMarkDb[k] - S::kStaticGainMarkDb[k - 1]);
            break;
        }
    return v < 0 ? -db : db;
}
float twinBusRangeDb(int i) noexcept {
    i = std::clamp(i, 0, S::kRangeSteps - 1);
    return i < 16 ? 0.5f * static_cast<float>(i + 1) : 8.0f + static_cast<float>(i - 15);
}
float twinBusBellQ(float gainDb) noexcept {
    const float g = std::fabs(gainDb);
    const float q = S::kBellQAt5 + (g - 5.0f) * (S::kBellQAt10 - S::kBellQAt5) / 5.0f;
    return std::max(q, S::kBellQMin);
}

std::unique_ptr<fx::Effect> makeTwinBus() { return std::make_unique<TwinBusFx>(); }

} // namespace hc::console
