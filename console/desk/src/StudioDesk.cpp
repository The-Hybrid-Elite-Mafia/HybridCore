// HEM STUDIO DESK — implementation. Numbers come from hybrid/console/ConsoleSpecs.hpp only.
#include "hybrid/console/StudioDesk.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::console {
namespace {

namespace S = spec::desk;
using fx::ParamInfo;
using fx::Taper;

const char* const kOnOff[] = {"OFF", "ON"};

const ParamInfo kCh[StudioDesk::kChParamCount] = {
    {"trim",      "Trim",            "dB", S::kTrimMinDb, S::kTrimMaxDb, 0.0f},
    {"mono",      "Mono",            "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"polarity",  "Polarity (Ø)",    "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"hpf",       "HPF 75 Hz",       "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"eq_in",     "EQ In",           "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"hf",        "HF",              "dB", -S::kEqKnobMaxDb, S::kEqKnobMaxDb, 0.0f},
    {"hf_bell",   "HF Bell",         "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mf",        "MF",              "dB", -S::kEqKnobMaxDb, S::kEqKnobMaxDb, 0.0f},
    {"lf",        "LF",              "dB", -S::kEqKnobMaxDb, S::kEqKnobMaxDb, 0.0f},
    {"lf_bell",   "LF Bell",         "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"comp_in",   "Comp In",         "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"comp_thr",  "Comp Threshold",  "dBu", S::kChThreshMinDbu, S::kChThreshMaxDbu, S::kChThreshMaxDbu},
    {"insert",    "Insert",          "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue1_on",   "St Cue 1 On",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue1_lvl",  "St Cue 1 Level",  "dB", S::kOffDb, S::kCueSendMaxDb, S::kOffDb},
    {"cue1_pan",  "St Cue 1 Pan",    "",   -1, 1, 0},
    {"cue1_post", "St Cue 1 Post",   "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue2_on",   "St Cue 2 On",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue2_lvl",  "St Cue 2 Level",  "dB", S::kOffDb, S::kCueSendMaxDb, S::kOffDb},
    {"cue2_pan",  "St Cue 2 Pan",    "",   -1, 1, 0},
    {"cue2_post", "St Cue 2 Post",   "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"pan",       "Pan / Balance",   "",   -1, 1, 0},
    {"direct_post","Direct Out Post Fader", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"mute_busb", "Mute / Bus B",    "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"pfl",       "PFL",             "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"fader",     "Fader",           "dB", S::kOffDb, S::kFaderMaxDb, 0.0f},
};

const ParamInfo kMs[StudioDesk::kMasterParamCount] = {
    {"ext1_lvl",   "Ext 1 Level",          "dB", S::kOffDb, S::kExtLevelMaxDb, 0.0f},
    {"ext2_lvl",   "Ext 2 Level",          "dB", S::kOffDb, S::kExtLevelMaxDb, 0.0f},
    {"ext1_main",  "Ext 1 to Main",        "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"ext2_main",  "Ext 2 to Main",        "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"main_ins",   "Main Insert",          "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue1_comp",  "St Cue 1 to Bus Comp & Sum to Main", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"bc_in",      "Bus Comp In",          "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"bc_thr",     "Bus Comp Threshold",   "dB", S::kBusThreshMinDb, S::kBusThreshMaxDb, S::kBusThreshMaxDb},
    {"bc_auto",    "Bus Comp Auto Release","",   0, 1, 0, Taper::Toggle, kOnOff},
    {"bc_makeup",  "Bus Comp Make Up",     "dB", 0.0f, S::kBusMakeupMaxDb, 0.0f},
    {"main_fader", "Main Fader",           "dB", S::kOffDb, S::kFaderMaxDb, 0.0f},
    {"busb_lvl",   "Bus B Level",          "dB", S::kOffDb, S::kBusBMasterMaxDb, 0.0f},
    {"busb_mute",  "Bus B Mute",           "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue1_master","St Cue 1 Master",      "dB", S::kOffDb, S::kCueMasterMaxDb, 0.0f},
    {"cue1_ext1",  "St Cue 1 + Ext 1",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue1_ext2",  "St Cue 1 + Ext 2",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue2_master","St Cue 2 Master",      "dB", S::kOffDb, S::kCueMasterMaxDb, 0.0f},
    {"cue2_ext1",  "St Cue 2 + Ext 1",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cue2_ext2",  "St Cue 2 + Ext 2",     "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mon_main",   "Monitor Source Main",  "",   0, 1, 1, Taper::Toggle, kOnOff},
    {"mon_busb",   "Monitor Source Bus B", "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mon_ext1",   "Monitor Source Ext 1", "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mon_ext2",   "Monitor Source Ext 2", "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"dim_lvl",    "Dim Level",            "dB", S::kDimMinDb, S::kDimMaxDb, -20.0f},
    {"dim",        "Dim",                  "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"cut",        "Cut",                  "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mon_mono",   "Mono",                 "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"alt",        "Alt",                  "",   0, 1, 0, Taper::Toggle, kOnOff},
    {"mon_lvl",    "Monitor Level",        "",   0.0f, S::kMonitorKnobMax, S::kMonitorKnobMax},
};

constexpr float kSmoothMs = 20.0f;   // level glides (no zipper noise)
constexpr float kSwitchMs = 5.0f;    // routing switches fade instead of clicking

[[nodiscard]] inline float levelGain(float db) noexcept { return db <= S::kOffDb + 0.01f ? 0.0f : dbToLin(db); }
[[nodiscard]] inline bool on(float v) noexcept { return v >= 0.5f; }

struct Ramp {   // per-sample glide toward a target gain
    float cur = 0, coef = 0;
    void prepare(double fs, float ms, float v) noexcept { coef = coefMs(fs, ms); cur = v; }
    [[nodiscard]] float next(float target) noexcept { cur = target + coef * (cur - target); return cur; }
    void snap(float v) noexcept { cur = v; }
};

} // namespace

// ---- pan laws -------------------------------------------------------------------------------
void StudioDesk::panGains(float p, float& gl, float& gr) noexcept {
    // Equal-shape law with a -4.5 dB centre (VERIFIED centre and end-stop unity): g = cos^k / sin^k.
    static const float k = std::log(dbToLin(S::kPanCentreDb)) / std::log(std::sqrt(0.5f));
    const float th = (std::clamp(p, -1.0f, 1.0f) + 1.0f) * 0.25f * std::numbers::pi_v<float>;
    gl = std::pow(std::max(std::cos(th), 0.0f), k);
    gr = std::pow(std::max(std::sin(th), 0.0f), k);
}
void StudioDesk::balanceGains(float p, float& gl, float& gr) noexcept {
    float l = 0, r = 0, c = 0, unused = 0;
    panGains(p, l, r);
    panGains(0.0f, c, unused);
    gl = std::min(1.0f, l / c);
    gr = std::min(1.0f, r / c);
}
float StudioDesk::monitorKnobGain(float v) noexcept {
    v = std::clamp(v, 0.0f, S::kMonitorKnobMax);
    if (v <= 0.0f) return 0.0f;
    return dbToLin(S::kMonitorTaperDb * std::log10(v / S::kMonitorKnobMax));
}
int StudioDesk::channelMeterLeds(float dbu) noexcept { return ledsLit(dbu, S::kChannelMeterDbu); }
int StudioDesk::mainMeterLeds(float dbu) noexcept { return ledsLit(dbu, S::kMainMeterDbu); }
int StudioDesk::channelCompLeds(float grDb) noexcept { int n = 0; for (float m : S::kChGrLedDb) if (grDb >= m) ++n; return n; }
int StudioDesk::busCompLeds(float grDb) noexcept { int n = 0; for (float m : S::kBusGrLedDb) if (grDb >= m) ++n; return n; }

std::span<const fx::ParamInfo> StudioDesk::channelParams() noexcept { return kCh; }
std::span<const fx::ParamInfo> StudioDesk::masterParams() noexcept { return kMs; }

// ---- state ----------------------------------------------------------------------------------
struct StudioDesk::Impl {
    struct Channel {
        Biquad hpfL, hpfR, hfL, hfR, mfL, mfR, lfL, lfR;
        float lastHf = 1e9f, lastMf = 1e9f, lastLf = 1e9f;
        int lastHfBell = -1, lastLfBell = -1;
        float gr = 0;                                   // channel comp gain reduction, dB
        std::atomic<float> grOut{0};
        Ramp trim, fader, mainSend, busBSend, panL, panR, cue1L, cue1R, cue2L, cue2R, pfl;
        PeakMeter meterL, meterR;
        std::atomic<InsertSlot*> insert{nullptr};
        std::array<std::atomic<float>, kChParamCount> v{};
    };

    double fs = 48000.0;
    std::uint32_t maxBlock = 0;
    std::array<Channel, kChannels> ch;
    std::array<std::atomic<float>, kMasterParamCount> mv{};

    // scratch (allocated in prepare)
    std::vector<float> xL, xR, mainL, mainR, busBL, busBR, c1L, c1R, c2L, c2R, pflL, pflR, e1L, e1R, e2L, e2R, tL, tR, monL, monR;

    // master state
    Ramp ext1, ext2, ext1Main, ext2Main, mainFader, busB, cue1M, cue2M, c1e1, c1e2, c2e1, c2e2;
    Ramp srcMain, srcBusB, srcExt1, srcExt2, dimG, cutG, monLvl, toMon, toAlt, makeup, cue1Sum;
    FirstOrder scL, scR;
    DualRate busGr;
    bool lastAuto = false;
    std::atomic<float> busGrOut{0};
    std::atomic<InsertSlot*> mainInsert{nullptr};
    PeakMeter mainMeterL, mainMeterR;
    std::atomic<bool> pfl{false};

    // channel comp coefficients
    float chAttFast = 0, chAttSlow = 0, chRel = 0;

    float c(int k, int p) const noexcept { return ch[static_cast<std::size_t>(k)].v[static_cast<std::size_t>(p)].load(std::memory_order_relaxed); }
    float m(int p) const noexcept { return mv[static_cast<std::size_t>(p)].load(std::memory_order_relaxed); }

    void updateEq(Channel& k, float hf, bool hfBell, float mf, float lf, bool lfBell) noexcept {
        if (hf != k.lastHf || static_cast<int>(hfBell) != k.lastHfBell) {
            const auto& b = hfBell ? S::kHfBell : S::kHfShelf;
            const auto t = hfBell ? BqType::Bell : BqType::HighShelf;
            const double gdb = hf / S::kEqKnobMaxDb * b.gainAtFullKnobDb;
            const auto cf = hfBell ? matchedBell(fs, b.hz, b.q, gdb) : BiquadCoefs::make(t, fs, b.hz, b.q, gdb);
            k.hfL.c = cf; k.hfR.c = cf; k.lastHf = hf; k.lastHfBell = hfBell;
        }
        if (mf != k.lastMf) {
            const auto cf = matchedBell(fs, S::kMfBell.hz, S::kMfBell.q, mf / S::kEqKnobMaxDb * S::kMfBell.gainAtFullKnobDb);
            k.mfL.c = cf; k.mfR.c = cf; k.lastMf = mf;
        }
        if (lf != k.lastLf || static_cast<int>(lfBell) != k.lastLfBell) {
            const auto& b = lfBell ? S::kLfBell : S::kLfShelf;
            const auto t = lfBell ? BqType::Bell : BqType::LowShelf;
            const double gdb = lf / S::kEqKnobMaxDb * b.gainAtFullKnobDb;
            const auto cf = lfBell ? matchedBell(fs, b.hz, b.q, gdb) : BiquadCoefs::make(t, fs, b.hz, b.q, gdb);
            k.lfL.c = cf; k.lfR.c = cf; k.lastLf = lf; k.lastLfBell = lfBell;
        }
    }
};

StudioDesk::StudioDesk() : d_(std::make_unique<Impl>()) { setDefaults(); prepare(48000.0, 512); }
StudioDesk::~StudioDesk() = default;

void StudioDesk::setDefaults() noexcept {
    for (int k = 0; k < static_cast<int>(kChannels); ++k)
        for (int p = 0; p < kChParamCount; ++p) setChannel(k, p, kCh[p].def);
    for (int p = 0; p < kMasterParamCount; ++p) setMaster(p, kMs[p].def);
}

void StudioDesk::setChannel(int k, int p, float v) noexcept {
    if (k < 0 || k >= static_cast<int>(kChannels) || p < 0 || p >= kChParamCount) return;
    d_->ch[static_cast<std::size_t>(k)].v[static_cast<std::size_t>(p)].store(fx::clampPlain(kCh[p], v), std::memory_order_relaxed);
}
float StudioDesk::channel(int k, int p) const noexcept {
    if (k < 0 || k >= static_cast<int>(kChannels) || p < 0 || p >= kChParamCount) return 0.0f;
    return d_->c(k, p);
}
void StudioDesk::setMaster(int p, float v) noexcept {
    if (p < 0 || p >= kMasterParamCount) return;
    d_->mv[static_cast<std::size_t>(p)].store(fx::clampPlain(kMs[p], v), std::memory_order_relaxed);
}
float StudioDesk::master(int p) const noexcept { return p >= 0 && p < kMasterParamCount ? d_->m(p) : 0.0f; }

void StudioDesk::setChannelInsert(int k, InsertSlot* s) noexcept {
    if (k >= 0 && k < static_cast<int>(kChannels)) d_->ch[static_cast<std::size_t>(k)].insert.store(s, std::memory_order_release);
}
void StudioDesk::setMainInsert(InsertSlot* s) noexcept { d_->mainInsert.store(s, std::memory_order_release); }

float StudioDesk::channelPeakDbu(int k, int side) const noexcept {
    if (k < 0 || k >= static_cast<int>(kChannels)) return -120.0f;
    const auto& c = d_->ch[static_cast<std::size_t>(k)];
    return (side ? c.meterR.db() : c.meterL.db()) + spec::kDbuAtFullScale;
}
float StudioDesk::channelCompGrDb(int k) const noexcept {
    return k >= 0 && k < static_cast<int>(kChannels) ? d_->ch[static_cast<std::size_t>(k)].grOut.load(std::memory_order_relaxed) : 0.0f;
}
float StudioDesk::mainMeterDbu(int side) const noexcept { return (side ? d_->mainMeterR.db() : d_->mainMeterL.db()) + spec::kDbuAtFullScale; }
float StudioDesk::busCompGrDb() const noexcept { return d_->busGrOut.load(std::memory_order_relaxed); }
bool StudioDesk::pflActive() const noexcept { return d_->pfl.load(std::memory_order_relaxed); }

void StudioDesk::prepare(double fs, std::uint32_t maxBlock) {
    auto& d = *d_;
    d.fs = fs;
    d.maxBlock = std::max<std::uint32_t>(1, maxBlock);
    for (auto* v : {&d.xL, &d.xR, &d.mainL, &d.mainR, &d.busBL, &d.busBR, &d.c1L, &d.c1R, &d.c2L, &d.c2R,
                    &d.pflL, &d.pflR, &d.e1L, &d.e1R, &d.e2L, &d.e2R, &d.tL, &d.tR, &d.monL, &d.monR})
        v->assign(d.maxBlock, 0.0f);
    d.chAttFast = coefMs(fs, S::kChAttackFastMs);
    d.chAttSlow = coefMs(fs, S::kChAttackSlowMs);
    d.chRel = coefMs(fs, S::kChReleaseMs);
    d.scL.set(FirstOrder::Type::HighPass, fs, S::kBusScHpfHz);
    d.scR.set(FirstOrder::Type::HighPass, fs, S::kBusScHpfHz);
    reset();
}

void StudioDesk::reset() noexcept {
    auto& d = *d_;
    const auto hp = BiquadCoefs::make(BqType::HighPass, d.fs, S::kHpfHz, S::kHpfQ);
    for (auto& k : d.ch) {
        for (Biquad* b : {&k.hpfL, &k.hpfR, &k.hfL, &k.hfR, &k.mfL, &k.mfR, &k.lfL, &k.lfR}) b->clear();
        k.hpfL.c = hp; k.hpfR.c = hp;
        k.lastHf = k.lastMf = k.lastLf = 1e9f; k.lastHfBell = k.lastLfBell = -1;
        k.gr = 0; k.grOut.store(0);
        for (Ramp* r : {&k.trim, &k.fader}) r->prepare(d.fs, kSmoothMs, 0.0f);
        for (Ramp* r : {&k.panL, &k.panR, &k.cue1L, &k.cue1R, &k.cue2L, &k.cue2R}) r->prepare(d.fs, kSmoothMs, 0.0f);
        for (Ramp* r : {&k.mainSend, &k.busBSend, &k.pfl}) r->prepare(d.fs, kSwitchMs, 0.0f);
        k.meterL.prepare(d.fs, S::kMeterReleaseDbPerSec);
        k.meterR.prepare(d.fs, S::kMeterReleaseDbPerSec);
    }
    for (Ramp* r : {&d.ext1, &d.ext2, &d.mainFader, &d.busB, &d.cue1M, &d.cue2M, &d.monLvl, &d.makeup, &d.dimG})
        r->prepare(d.fs, kSmoothMs, 0.0f);
    for (Ramp* r : {&d.ext1Main, &d.ext2Main, &d.c1e1, &d.c1e2, &d.c2e1, &d.c2e2, &d.srcMain, &d.srcBusB, &d.srcExt1,
                    &d.srcExt2, &d.cutG, &d.toMon, &d.toAlt, &d.cue1Sum})
        r->prepare(d.fs, kSwitchMs, 0.0f);
    d.scL.clear(); d.scR.clear();
    d.busGr.clear();
    d.lastAuto = !on(d.m(BusCompAutoRelease));   // force set() on the first block
    d.busGrOut.store(0);
    d.mainMeterL.prepare(d.fs, S::kMeterReleaseDbPerSec);
    d.mainMeterR.prepare(d.fs, S::kMeterReleaseDbPerSec);
    // Start every glide at its target so the first block has no fade-in.
    for (int k = 0; k < static_cast<int>(kChannels); ++k) {
        auto& c = d.ch[static_cast<std::size_t>(k)];
        c.trim.snap(dbToLin(d.c(k, Trim)));
        c.fader.snap(levelGain(d.c(k, Fader)));
    }
}

void StudioDesk::process(const Inputs& in, const Outputs& out, std::uint32_t frames) noexcept {
    auto& d = *d_;
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(frames - done, d.maxBlock);
        const auto off = done;
        auto at = [off](const float* p) { return p ? p + off : nullptr; };
        auto atw = [off](float* p) { return p ? p + off : nullptr; };

        // ---- clear the buses --------------------------------------------------------------
        for (auto* v : {&d.mainL, &d.mainR, &d.busBL, &d.busBR, &d.c1L, &d.c1R, &d.c2L, &d.c2R, &d.pflL, &d.pflR})
            std::fill_n(v->begin(), n, 0.0f);

        bool anyPfl = false;
        // ---- channels -----------------------------------------------------------------------
        for (int k = 0; k < static_cast<int>(kChannels); ++k) {
            auto& c = d.ch[static_cast<std::size_t>(k)];
            float* L = d.xL.data();
            float* R = d.xR.data();
            const float* srcL = at(in.left[static_cast<std::size_t>(k)]);
            const float* srcR = at(in.right[static_cast<std::size_t>(k)]);
            if (!srcL) { std::fill_n(L, n, 0.0f); std::fill_n(R, n, 0.0f); }
            else { std::copy_n(srcL, n, L); std::copy_n(srcR ? srcR : srcL, n, R); }

            // TRIM (input gain)
            const float trimT = dbToLin(d.c(k, Trim));
            for (std::uint32_t i = 0; i < n; ++i) { const float g = c.trim.next(trimT); L[i] *= g; R[i] *= g; }
            // Ø then HPF (the stem enters ahead of both, as the USB return does on the reference)
            if (on(d.c(k, Polarity))) for (std::uint32_t i = 0; i < n; ++i) { L[i] = -L[i]; R[i] = -R[i]; }
            if (on(d.c(k, Hpf))) { c.hpfL.run(L, n); c.hpfR.run(R, n); }
            else { c.hpfL.clear(); c.hpfR.clear(); }
            // MONO
            const bool mono = on(d.c(k, Mono));
            if (mono) for (std::uint32_t i = 0; i < n; ++i) { const float s = (L[i] + R[i]) * S::kMonoSumGain; L[i] = R[i] = s; }
            // EQ
            if (on(d.c(k, EqIn))) {
                d.updateEq(c, d.c(k, HfGain), on(d.c(k, HfBell)), d.c(k, MfGain), d.c(k, LfGain), on(d.c(k, LfBell)));
                c.hfL.run(L, n); c.hfR.run(R, n);
                c.mfL.run(L, n); c.mfR.run(R, n);
                c.lfL.run(L, n); c.lfR.run(R, n);
            }
            // COMP (one knob, 2:1, program-dependent attack, auto make-up)
            float grMax = 0.0f;
            if (on(d.c(k, CompIn))) {
                const float thrDbu = d.c(k, CompThreshold);
                const float thr = thrDbu - spec::kDbuAtFullScale;   // dBFS
                const float makeup = (1.0f - 1.0f / S::kChRatio) * std::max(0.0f, S::kChAutoMakeupRefDbu - thrDbu);
                for (std::uint32_t i = 0; i < n; ++i) {
                    const float x = linToDb(std::max(std::fabs(L[i]), std::fabs(R[i])));
                    const float target = -compGainDb(x, thr, S::kChRatio, S::kChKneeDb);
                    const float t = std::clamp((x - thr) / S::kChAttackSpanDb, 0.0f, 1.0f);
                    const float att = d.chAttSlow + (d.chAttFast - d.chAttSlow) * t;
                    c.gr = target + (target > c.gr ? att : d.chRel) * (c.gr - target);
                    const float g = dbToLin(makeup - c.gr);
                    L[i] *= g; R[i] *= g;
                    grMax = std::max(grMax, c.gr);
                }
            } else {
                c.gr = 0.0f;
            }
            c.grOut.store(grMax, std::memory_order_relaxed);
            // INSERT (empty slot passes audio)
            if (on(d.c(k, Insert)))
                if (InsertSlot* s = c.insert.load(std::memory_order_acquire)) s->process(L, R, n);

            // ---- pre-fader taps: meter, PFL, pre cue sends, pre direct out ---------------------
            c.meterL.feedPeak(peakOf(L, n), n);
            c.meterR.feedPeak(peakOf(R, n), n);
            const float pflT = on(d.c(k, Pfl)) ? 1.0f : 0.0f;
            anyPfl = anyPfl || pflT > 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) { const float g = c.pfl.next(pflT); d.pflL[i] += L[i] * g; d.pflR[i] += R[i] * g; }

            // cue send gains: ON × level × pan (pan law on a mono-switched channel, balance on stereo)
            auto cueTargets = [&](int onP, int lvlP, int panP, float& tl, float& tr) {
                const float lv = on(d.c(k, onP)) ? levelGain(d.c(k, lvlP)) : 0.0f;
                float gl = 1, gr = 1;
                if (mono) panGains(d.c(k, panP), gl, gr); else balanceGains(d.c(k, panP), gl, gr);
                tl = lv * gl; tr = lv * gr;
            };
            float c1tl, c1tr, c2tl, c2tr;
            cueTargets(Cue1On, Cue1Level, Cue1Pan, c1tl, c1tr);
            cueTargets(Cue2On, Cue2Level, Cue2Pan, c2tl, c2tr);
            const bool c1Post = on(d.c(k, Cue1Post)), c2Post = on(d.c(k, Cue2Post));
            const bool directPost = on(d.c(k, DirectPostFader));
            float* dL = atw(out.directL[static_cast<std::size_t>(k)]);
            float* dR = atw(out.directR[static_cast<std::size_t>(k)]);
            if (!directPost && dL && dR) { std::copy_n(L, n, dL); std::copy_n(R, n, dR); }
            if (!c1Post) for (std::uint32_t i = 0; i < n; ++i) { d.c1L[i] += L[i] * c.cue1L.next(c1tl); d.c1R[i] += R[i] * c.cue1R.next(c1tr); }
            if (!c2Post) for (std::uint32_t i = 0; i < n; ++i) { d.c2L[i] += L[i] * c.cue2L.next(c2tl); d.c2R[i] += R[i] * c.cue2R.next(c2tr); }

            // ---- FADER ---------------------------------------------------------------------------
            const float fT = levelGain(d.c(k, Fader));
            for (std::uint32_t i = 0; i < n; ++i) { const float g = c.fader.next(fT); L[i] *= g; R[i] *= g; }

            // post-fader taps
            if (directPost && dL && dR) { std::copy_n(L, n, dL); std::copy_n(R, n, dR); }
            if (c1Post) for (std::uint32_t i = 0; i < n; ++i) { d.c1L[i] += L[i] * c.cue1L.next(c1tl); d.c1R[i] += R[i] * c.cue1R.next(c1tr); }
            if (c2Post) for (std::uint32_t i = 0; i < n; ++i) { d.c2L[i] += L[i] * c.cue2L.next(c2tl); d.c2R[i] += R[i] * c.cue2R.next(c2tr); }

            // ---- PAN / BALANCE, then MAIN or BUS B (MUTE/BUS B) ----------------------------------
            float pl = 1, pr = 1;
            if (mono) panGains(d.c(k, Pan), pl, pr); else balanceGains(d.c(k, Pan), pl, pr);
            const float toB = on(d.c(k, MuteBusB)) ? 1.0f : 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float l = L[i] * c.panL.next(pl), r = R[i] * c.panR.next(pr);
                const float gm = c.mainSend.next(1.0f - toB), gb = c.busBSend.next(toB);
                d.mainL[i] += l * gm; d.mainR[i] += r * gm;
                d.busBL[i] += l * gb; d.busBR[i] += r * gb;
            }
        }
        d.pfl.store(anyPfl, std::memory_order_relaxed);

        // ---- EXT returns (after their level controls; they feed main, cues and monitor) ---------
        const float e1T = levelGain(d.m(Ext1Level)), e2T = levelGain(d.m(Ext2Level));
        const float* x1L = at(in.ext1L); const float* x1R = at(in.ext1R ? in.ext1R : in.ext1L);
        const float* x2L = at(in.ext2L); const float* x2R = at(in.ext2R ? in.ext2R : in.ext2L);
        for (std::uint32_t i = 0; i < n; ++i) {
            const float g1 = d.ext1.next(e1T), g2 = d.ext2.next(e2T);
            d.e1L[i] = x1L ? x1L[i] * g1 : 0.0f; d.e1R[i] = x1R ? x1R[i] * g1 : 0.0f;
            d.e2L[i] = x2L ? x2L[i] * g2 : 0.0f; d.e2R[i] = x2R ? x2R[i] * g2 : 0.0f;
        }

        // ---- cue masters: bus + EXT sums, then the master level (last control before the output) --
        {
            const float a1 = on(d.m(Cue1Ext1)) ? 1.0f : 0.0f, a2 = on(d.m(Cue1Ext2)) ? 1.0f : 0.0f;
            const float b1 = on(d.m(Cue2Ext1)) ? 1.0f : 0.0f, b2 = on(d.m(Cue2Ext2)) ? 1.0f : 0.0f;
            const float m1 = levelGain(d.m(Cue1Master)), m2 = levelGain(d.m(Cue2Master));
            for (std::uint32_t i = 0; i < n; ++i) {
                const float ga1 = d.c1e1.next(a1), ga2 = d.c1e2.next(a2), gb1 = d.c2e1.next(b1), gb2 = d.c2e2.next(b2);
                const float gm1 = d.cue1M.next(m1), gm2 = d.cue2M.next(m2);
                d.c1L[i] = (d.c1L[i] + d.e1L[i] * ga1 + d.e2L[i] * ga2) * gm1;
                d.c1R[i] = (d.c1R[i] + d.e1R[i] * ga1 + d.e2R[i] * ga2) * gm1;
                d.c2L[i] = (d.c2L[i] + d.e1L[i] * gb1 + d.e2L[i] * gb2) * gm2;
                d.c2R[i] = (d.c2R[i] + d.e1R[i] * gb1 + d.e2R[i] * gb2) * gm2;
            }
        }

        // ---- main bus: + EXT sums -> MAIN INSERT -> BUS COMP -> MAIN FADER ----------------------
        {
            const float t1 = on(d.m(Ext1ToMain)) ? 1.0f : 0.0f, t2 = on(d.m(Ext2ToMain)) ? 1.0f : 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float g1 = d.ext1Main.next(t1), g2 = d.ext2Main.next(t2);
                d.mainL[i] += d.e1L[i] * g1 + d.e2L[i] * g2;
                d.mainR[i] += d.e1R[i] * g1 + d.e2R[i] * g2;
            }
        }
        if (on(d.m(MainInsert)))
            if (InsertSlot* s = d.mainInsert.load(std::memory_order_acquire)) s->process(d.mainL.data(), d.mainR.data(), n);

        const bool cue1Comp = on(d.m(Cue1ToBusComp));
        const bool compIn = on(d.m(BusCompIn));
        // Compressor input: the main bus, or (CUE 1 TO BUS COMP) a copy of the cue 1 mix that is then summed to main.
        float* cL = cue1Comp ? d.tL.data() : d.mainL.data();
        float* cR = cue1Comp ? d.tR.data() : d.mainR.data();
        if (cue1Comp) { std::copy_n(d.c1L.data(), n, cL); std::copy_n(d.c1R.data(), n, cR); }
        float busGrMax = 0.0f;
        if (compIn) {
            const bool autoRel = on(d.m(BusCompAutoRelease));
            if (autoRel != d.lastAuto) {
                if (autoRel) d.busGr.set(d.fs, S::kBusAttackMs, S::kBusAutoFastMs, true, S::kBusAutoChargeMs, S::kBusAutoSlowMs);
                else         d.busGr.set(d.fs, S::kBusAttackMs, S::kBusReleaseMs, false, S::kBusAutoChargeMs, S::kBusAutoSlowMs);
                d.lastAuto = autoRel;
            }
            const float thr = d.m(BusCompThreshold) + S::kBusThreshRefDbu - spec::kDbuAtFullScale;   // dBFS
            const float mkT = dbToLin(d.m(BusCompMakeup));
            for (std::uint32_t i = 0; i < n; ++i) {
                const double sl = d.scL.tick(cL[i]), sr = d.scR.tick(cR[i]);   // 50 Hz 1st-order side-chain HPF
                const float x = linToDb(static_cast<float>(std::max(std::fabs(sl), std::fabs(sr))));
                const float gr = d.busGr.tick(-compGainDb(x, thr, S::kBusRatio, S::kBusKneeDb));
                const float g = dbToLin(-gr) * d.makeup.next(mkT);
                cL[i] *= g; cR[i] *= g;
                busGrMax = std::max(busGrMax, gr);
            }
        } else {
            d.busGr.clear();
        }
        d.busGrOut.store(busGrMax, std::memory_order_relaxed);
        {
            const float sumT = cue1Comp ? 1.0f : 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) { const float g = d.cue1Sum.next(sumT); d.mainL[i] += d.tL[i] * g; d.mainR[i] += d.tR[i] * g; }
            if (!cue1Comp) { std::fill_n(d.tL.begin(), n, 0.0f); std::fill_n(d.tR.begin(), n, 0.0f); }
        }
        const float mfT = levelGain(d.m(MainFader));
        for (std::uint32_t i = 0; i < n; ++i) { const float g = d.mainFader.next(mfT); d.mainL[i] *= g; d.mainR[i] *= g; }

        // ---- BUS B master (level, mute) -------------------------------------------------------
        const float bT = on(d.m(BusBMute)) ? 0.0f : levelGain(d.m(BusBLevel));
        for (std::uint32_t i = 0; i < n; ++i) { const float g = d.busB.next(bT); d.busBL[i] *= g; d.busBR[i] *= g; }

        // ---- MONITOR: sources SUM, PFL interrupts, then MONO, DIM, CUT, level, MAIN/ALT ----------
        {
            const float sM = on(d.m(MonMain)) ? 1.0f : 0.0f, sB = on(d.m(MonBusB)) ? 1.0f : 0.0f;
            const float s1 = on(d.m(MonExt1)) ? 1.0f : 0.0f, s2 = on(d.m(MonExt2)) ? 1.0f : 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float gm = d.srcMain.next(sM), gb = d.srcBusB.next(sB), g1 = d.srcExt1.next(s1), g2 = d.srcExt2.next(s2);
                d.monL[i] = d.mainL[i] * gm + d.busBL[i] * gb + d.e1L[i] * g1 + d.e2L[i] * g2;
                d.monR[i] = d.mainR[i] * gm + d.busBR[i] * gb + d.e1R[i] * g1 + d.e2R[i] * g2;
            }
            if (anyPfl) { std::copy_n(d.pflL.data(), n, d.monL.data()); std::copy_n(d.pflR.data(), n, d.monR.data()); }
            d.mainMeterL.feedPeak(peakOf(d.monL.data(), n), n);   // the main meter follows the monitor source
            d.mainMeterR.feedPeak(peakOf(d.monR.data(), n), n);
            if (on(d.m(MonoMonitor)))
                for (std::uint32_t i = 0; i < n; ++i) { const float s = (d.monL[i] + d.monR[i]) * S::kMonoMonitorGain; d.monL[i] = d.monR[i] = s; }
            const float dimT = on(d.m(Dim)) ? dbToLin(d.m(DimLevel)) : 1.0f;
            const float cutT = on(d.m(Cut)) ? 0.0f : 1.0f;
            const float lvT = monitorKnobGain(d.m(MonitorLevel));
            const float altT = on(d.m(Alt)) ? 1.0f : 0.0f;
            float* oL = atw(out.monL); float* oR = atw(out.monR);
            float* aL = atw(out.altL); float* aR = atw(out.altR);
            for (std::uint32_t i = 0; i < n; ++i) {
                const float g = d.dimG.next(dimT) * d.cutG.next(cutT) * d.monLvl.next(lvT);
                const float ga = d.toAlt.next(altT), gmn = d.toMon.next(1.0f - altT);
                const float l = d.monL[i] * g, r = d.monR[i] * g;
                if (oL) oL[i] = l * gmn;
                if (oR) oR[i] = r * gmn;
                if (aL) aL[i] = l * ga;
                if (aR) aR[i] = r * ga;
            }
        }

        // ---- outputs --------------------------------------------------------------------------
        auto put = [n](float* dst, const std::vector<float>& src) { if (dst) std::copy_n(src.data(), n, dst); };
        put(atw(out.mainL), d.mainL); put(atw(out.mainR), d.mainR);
        put(atw(out.busBL), d.busBL); put(atw(out.busBR), d.busBR);
        put(atw(out.cue1L), d.c1L);   put(atw(out.cue1R), d.c1R);
        put(atw(out.cue2L), d.c2L);   put(atw(out.cue2R), d.c2R);
        done += n;
    }
}

} // namespace hc::console
