// HEM FRONT-END TOOLS — implementation. Design numbers live in ConsoleSpecs.hpp (namespace tools, all OWN).
#include "hybrid/console/FrontTools.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace hc::console {
namespace {

namespace S = spec::tools;
using fx::ParamInfo;
using fx::Taper;

const char* const kOnOff[] = {"OFF", "ON"};
const char* const kSpeedN[] = {"FAST", "MEDIUM", "SLOW"};

// =============================================================================================
//  HEM RESO TAMER
// =============================================================================================
std::array<ParamInfo, reso_ids::kCount> makeResoParams() {
    using namespace reso_ids;
    std::array<ParamInfo, kCount> t{};
    static const char* const ids[2][kPerBand] = {{"on1", "freq1", "width1", "thr1", "depth1", "speed1", "listen1"},
                                                 {"on2", "freq2", "width2", "thr2", "depth2", "speed2", "listen2"}};
    static const char* const names[2][kPerBand] = {
        {"Band 1 On", "Band 1 Freq", "Band 1 Width (Q)", "Band 1 Threshold", "Band 1 Depth", "Band 1 Speed", "Band 1 Listen"},
        {"Band 2 On", "Band 2 Freq", "Band 2 Width (Q)", "Band 2 Threshold", "Band 2 Depth", "Band 2 Speed", "Band 2 Listen"}};
    const float defHz[2] = {300.0f, 3000.0f};   // OWN: one low-mid and one upper-mid starting point
    for (int b = 0; b < 2; ++b) {
        auto P = [&](int p) -> ParamInfo& { return t[static_cast<std::size_t>(id(b, p))]; };
        P(On)        = {ids[b][On], names[b][On], "", 0, 1, 0, Taper::Toggle, kOnOff};
        P(Freq)      = {ids[b][Freq], names[b][Freq], "Hz", 40, 16000, defHz[b], Taper::Log};
        P(Width)     = {ids[b][Width], names[b][Width], "Q", 1, 12, 4, Taper::Log};
        P(Threshold) = {ids[b][Threshold], names[b][Threshold], "dB", 0, 24, 0, Taper::Linear};
        P(Depth)     = {ids[b][Depth], names[b][Depth], "dB", 0, 18, 0, Taper::Linear};
        P(Speed)     = {ids[b][Speed], names[b][Speed], "", 0, 2, 1, Taper::Choice, kSpeedN};
        P(Listen)    = {ids[b][Listen], names[b][Listen], "", 0, 1, 0, Taper::Toggle, kOnOff};
    }
    return t;
}
const std::array<ParamInfo, reso_ids::kCount> kResoParams = makeResoParams();
const fx::MeterInfo kResoMeters[] = {{"BAND 1 GR", "dB", -18, 0}, {"BAND 2 GR", "dB", -18, 0}};

class ResoTamer final : public fx::Effect {
public:
    ResoTamer() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kResoParams; }
    std::span<const fx::MeterInfo> meters() const noexcept override { return kResoMeters; }
    bool paramActive(int i) const noexcept override {
        const int b = i / reso_ids::kPerBand;
        return i % reso_ids::kPerBand == reso_ids::On || p(reso_ids::id(b, reso_ids::On)) > 0.5f;
    }

    void prepare(double fs, std::uint32_t) override { fs_ = fs; reset(); }
    void reset() noexcept override {
        for (Band& b : bands_) {
            b.detN.clear(); b.detW.clear(); b.apL.clear(); b.apR.clear();
            b.pN = b.pW = 0; b.cutDb = 0; b.lastHz = b.lastQ = -1; b.lastSpeed = -1;
        }
        publishMeter(0, 0); publishMeter(1, 0);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        using namespace reso_ids;
        const float powCoef = coefMs(fs_, 10.0);   // detector power smoothing
        bool anyListen = false;
        for (int bi = 0; bi < 2; ++bi) {
            Band& b = bands_[static_cast<std::size_t>(bi)];
            b.on = p(id(bi, On)) > 0.5f;
            b.listen = b.on && p(id(bi, Listen)) > 0.5f;
            anyListen = anyListen || b.listen;
            const float hz = p(id(bi, Freq)), q = p(id(bi, Width));
            if (hz != b.lastHz || q != b.lastQ) {
                const BiquadCoefs nb = BiquadCoefs::make(BqType::BandPass, fs_, hz, q);
                b.detN.c = nb; b.apL.c = nb; b.apR.c = nb;
                b.detW.c = BiquadCoefs::make(BqType::BandPass, fs_, hz, S::kResoWideQ);
                b.baselineDb = 10.0f * std::log10(S::kResoWideQ / q);   // noise share of the narrow band vs the wide one
                b.lastHz = hz; b.lastQ = q;
            }
            const int sp = std::clamp(static_cast<int>(p(id(bi, Speed)) + 0.5f), 0, 2);
            if (sp != b.lastSpeed) {
                b.att = coefMs(fs_, S::kResoAttackMs[sp]); b.rel = coefMs(fs_, S::kResoReleaseMs[sp]); b.lastSpeed = sp;
            }
            b.thr = p(id(bi, Threshold)); b.depth = p(id(bi, Depth));
        }
        const float floorPow = dbToLin(S::kResoDetectFloorDb) * dbToLin(S::kResoDetectFloorDb);
        float maxCut[2] = {0, 0};
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = L[i], r = R[i], m = 0.5f * (l + r);
            float outL = anyListen ? 0.0f : l, outR = anyListen ? 0.0f : r;
            for (int bi = 0; bi < 2; ++bi) {
                Band& b = bands_[static_cast<std::size_t>(bi)];
                if (!b.on) continue;
                const float dn = b.detN.tick(m), dw = b.detW.tick(m);
                b.pN = dn * dn + powCoef * (b.pN - dn * dn);
                b.pW = dw * dw + powCoef * (b.pW - dw * dw);
                float want = 0.0f;
                if (b.pW > floorPow && b.pN > floorPow) {
                    const float excess = 10.0f * std::log10(b.pN / b.pW) - b.baselineDb;
                    want = std::clamp(excess - b.thr, 0.0f, b.depth);
                }
                const float c = want > b.cutDb ? b.att : b.rel;
                b.cutDb = want + c * (b.cutDb - want);
                maxCut[bi] = std::max(maxCut[bi], b.cutDb);
                const float bl = b.apL.tick(l), br = b.apR.tick(r);
                if (anyListen) {
                    if (b.listen) { outL += bl; outR += br; }
                } else {
                    const float g = dbToLin(-b.cutDb) - 1.0f;   // y = x + (g - 1)·bp(x): bell cut of g at the centre
                    outL += g * bl; outR += g * br;
                }
            }
            L[i] = outL; R[i] = outR;
        }
        publishMeter(MeterGr1, -maxCut[0]);
        publishMeter(MeterGr2, -maxCut[1]);
        publishReduction(-std::max(maxCut[0], maxCut[1]));
    }

private:
    struct Band {
        fx::Biquad detN, detW, apL, apR;
        float pN = 0, pW = 0, cutDb = 0, baselineDb = 0, thr = 0, depth = 0, att = 0, rel = 0;
        float lastHz = -1, lastQ = -1;
        int lastSpeed = -1;
        bool on = false, listen = false;
    };
    std::array<Band, 2> bands_{};
    double fs_ = 48000.0;
};

// =============================================================================================
//  HEM SHUFFLE
// =============================================================================================
std::array<ParamInfo, shuffle_ids::kCount> makeShuffleParams() {
    using namespace shuffle_ids;
    std::array<ParamInfo, kCount> t{};
    t[ShuffleFreq] = {"sh_freq", "Shuffle Freq", "Hz", 100, 1000, 300, Taper::Log};
    t[Shuffle]     = {"shuffle", "Shuffle", "dB", 0, 12, 0, Taper::Linear};
    t[MudFreq]     = {"mud_freq", "Mud Freq", "Hz", 100, 800, 250, Taper::Log};
    t[Mud]         = {"mud", "Mud Cut", "dB", 0, 12, 0, Taper::Linear};
    t[Width]       = {"width", "Width", "%", 0, 200, 100, Taper::Linear};
    t[MonoBass]    = {"mono_bass", "Mono Bass", "", 0, 1, 0, Taper::Toggle, kOnOff};
    t[Output]      = {"output", "Output", "dB", -6, 6, 0, Taper::Linear};
    return t;
}
const std::array<ParamInfo, shuffle_ids::kCount> kShuffleParams = makeShuffleParams();
const fx::MeterInfo kShuffleMeters[] = {{"CORRELATION", "corr", -1, 1, true}};

class ShuffleFx final : public fx::Effect {
public:
    ShuffleFx() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kShuffleParams; }
    std::span<const fx::MeterInfo> meters() const noexcept override { return kShuffleMeters; }
    bool paramActive(int i) const noexcept override {
        using namespace shuffle_ids;
        if (i == ShuffleFreq) return p(Shuffle) > 0.0f;
        if (i == MudFreq) return p(Mud) > 0.0f;
        return true;
    }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        monoHp_.c = BiquadCoefs::make(BqType::HighPass, fs_, S::kShuffleMonoBassHz, 0.7071);
        corrK_ = coefMs(fs_, S::kShuffleCorrelationMs);
        width_.prepare(fs_, 20.0, 1.0f);
        out_.prepare(fs_, 20.0, 1.0f);
        reset();
    }
    void reset() noexcept override {
        shelf_.clear(); mud_.clear(); monoHp_.clear();
        lastShF_ = lastSh_ = lastMudF_ = lastMud_ = -1;
        cLR_ = cLL_ = cRR_ = 0;
        publishMeter(0, 0);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        using namespace shuffle_ids;
        const float shF = p(ShuffleFreq), sh = p(Shuffle), mF = p(MudFreq), md = p(Mud);
        if (shF != lastShF_ || sh != lastSh_) { shelf_.setLowShelf(fs_, shF, sh); lastShF_ = shF; lastSh_ = sh; }
        if (mF != lastMudF_ || md != lastMud_) { mud_.c = matchedBell(fs_, mF, S::kShuffleMudQ, -md); lastMudF_ = mF; lastMud_ = md; }
        const bool monoBass = p(MonoBass) > 0.5f;
        const float wT = p(Width) * 0.01f, oT = dbToLin(p(Output));
        const bool shelfOn = sh > 0.0f, mudOn = md > 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]);
            if (mudOn) m = mud_.tick(m);
            if (shelfOn) s = static_cast<float>(shelf_.tick(s));
            if (monoBass) s = monoHp_.tick(s);
            s *= width_.next(wT);
            const float g = out_.next(oT);
            const float l = (m + s) * g, r = (m - s) * g;
            L[i] = l; R[i] = r;
            cLR_ = l * r + corrK_ * (cLR_ - l * r);
            cLL_ = l * l + corrK_ * (cLL_ - l * l);
            cRR_ = r * r + corrK_ * (cRR_ - r * r);
        }
        const double den = std::sqrt(static_cast<double>(cLL_) * cRR_);
        publishMeter(MeterCorrelation, den > 1e-12 ? static_cast<float>(std::clamp(cLR_ / den, -1.0, 1.0)) : 0.0f);
    }

private:
    FirstOrder shelf_;
    fx::Biquad mud_, monoHp_;
    fx::Smoothed width_, out_;
    double fs_ = 48000.0;
    float lastShF_ = -1, lastSh_ = -1, lastMudF_ = -1, lastMud_ = -1;
    float corrK_ = 0, cLR_ = 0, cLL_ = 0, cRR_ = 0;
};

// =============================================================================================
//  HEM TAP ECHO
// =============================================================================================
const char* const kNoteN[] = {"FREE", "1/1", "1/2", "1/2 D", "1/2 T", "1/4", "1/4 D", "1/4 T", "1/8", "1/8 D", "1/8 T", "1/16", "1/16 D", "1/16 T"};
constexpr int kNoteCount = 14;
const char* const kModeN[] = {"STEREO", "PING-PONG", "MONO"};

std::array<ParamInfo, echo_ids::kCount> makeEchoParams() {
    using namespace echo_ids;
    std::array<ParamInfo, kCount> t{};
    t[Tempo]    = {"tempo", "Tempo", "BPM", 40, 240, 120, Taper::Linear};
    t[Note]     = {"note", "Note", "", 0, kNoteCount - 1, 5, Taper::Choice, kNoteN};
    t[Time]     = {"time", "Time", "ms", 1, 4000, 250, Taper::Log};
    t[Feedback] = {"feedback", "Feedback", "%", 0, 95, 0, Taper::Linear};
    t[Mix]      = {"mix", "Mix", "%", 0, 100, 0, Taper::Linear};
    t[LowCut]   = {"low_cut", "Low Cut", "Hz", 20, 2000, 20, Taper::Log};
    t[HighCut]  = {"high_cut", "High Cut", "Hz", 1000, 20000, 20000, Taper::Log};
    t[Mode]     = {"mode", "Mode", "", 0, 2, 0, Taper::Choice, kModeN};
    return t;
}
const std::array<ParamInfo, echo_ids::kCount> kEchoParams = makeEchoParams();
const fx::MeterInfo kEchoMeters[] = {{"DELAY", "ms", 0, 4000}};

class TapEcho final : public fx::Effect {
public:
    TapEcho() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kEchoParams; }
    std::span<const fx::MeterInfo> meters() const noexcept override { return kEchoMeters; }
    bool paramActive(int i) const noexcept override {
        using namespace echo_ids;
        const bool free = static_cast<int>(p(Note) + 0.5f) == NoteFree;
        if (i == Time) return free;
        if (i == Tempo) return !free;
        return true;
    }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        const auto maxD = static_cast<std::uint32_t>(std::ceil(S::kEchoMaxSeconds * fs)) + 4;
        dl_.prepare(maxD); dr_.prepare(maxD);
        glide_ = coefMs(fs, S::kEchoTimeGlideMs);
        mix_.prepare(fs, 20.0, 0.0f); fb_.prepare(fs, 20.0, 0.0f);
        reset();
    }
    void reset() noexcept override {
        dl_.clear(); dr_.clear();
        for (auto* b : {&lcL_, &lcR_, &hcL_, &hcR_}) b->clear();
        delay_ = -1.0f; lastLc_ = lastHc_ = -1;
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        using namespace echo_ids;
        const float lc = p(LowCut), hc = p(HighCut);
        if (lc != lastLc_) { lcL_.c = lcR_.c = BiquadCoefs::make(BqType::HighPass, fs_, lc, 0.7071); lastLc_ = lc; }
        if (hc != lastHc_) { hcL_.c = hcR_.c = BiquadCoefs::make(BqType::LowPass, fs_, std::min<double>(hc, 0.45 * fs_), 0.7071); lastHc_ = hc; }
        const double ms = echoNoteMs(static_cast<int>(p(Note) + 0.5f), p(Tempo), p(Time));
        const float maxSamples = S::kEchoMaxSeconds * static_cast<float>(fs_);
        const float target = std::clamp(static_cast<float>(ms * 0.001 * fs_), 1.0f, maxSamples);
        if (delay_ < 0.0f) delay_ = target;
        const int mode = static_cast<int>(p(Mode) + 0.5f);
        const float mixT = p(Mix) * 0.01f, fbT = p(Feedback) * 0.01f;
        for (std::uint32_t i = 0; i < n; ++i) {
            delay_ = target + glide_ * (delay_ - target);
            const float d = delay_ - 1.0f;   // read before this sample's write
            const float rl = hcL_.tick(lcL_.tick(dl_.read(d)));
            const float rr = hcR_.tick(lcR_.tick(dr_.read(d)));
            const float fb = fb_.next(fbT), mx = mix_.next(mixT);
            const float xl = L[i], xr = R[i];
            float wl, wr;
            if (mode == 1) {        // PING-PONG: the input enters left, every repeat crosses over
                wl = 0.5f * (xl + xr) + fb * rr;
                wr = fb * rl;
            } else if (mode == 2) { // MONO: one echo in the middle
                const float mono = 0.5f * (xl + xr) + fb * 0.5f * (rl + rr);
                wl = wr = mono;
            } else {                // STEREO: each side echoes itself
                wl = xl + fb * rl;
                wr = xr + fb * rr;
            }
            dl_.write(wl); dr_.write(wr);
            L[i] = xl + mx * (rl - xl);
            R[i] = xr + mx * (rr - xr);
        }
        publishMeter(MeterDelayMs, static_cast<float>(delay_ * 1000.0 / fs_));
    }

private:
    fx::DelayLine dl_, dr_;
    fx::Biquad lcL_, lcR_, hcL_, hcR_;
    fx::Smoothed mix_, fb_;
    double fs_ = 48000.0;
    float glide_ = 0, delay_ = -1, lastLc_ = -1, lastHc_ = -1;
};

} // namespace

std::unique_ptr<fx::Effect> makeResoTamer() { return std::make_unique<ResoTamer>(); }
std::unique_ptr<fx::Effect> makeShuffle() { return std::make_unique<ShuffleFx>(); }
std::unique_ptr<fx::Effect> makeTapEcho() { return std::make_unique<TapEcho>(); }

double echoNoteMs(int note, double bpm, double freeMs) noexcept {
    if (note <= echo_ids::NoteFree || note >= kNoteCount) return freeMs;
    const double beat = 60000.0 / std::clamp(bpm, 20.0, 400.0);   // quarter note
    static constexpr double kBase[] = {0, 4, 2, 2, 2, 1, 1, 1, 0.5, 0.5, 0.5, 0.25, 0.25, 0.25};
    const int kind = note == 1 ? 0 : (note - 2) % 3;   // 0 straight, 1 dotted, 2 triplet (1/1 has neither)
    const double mul = kind == 1 ? 1.5 : kind == 2 ? 2.0 / 3.0 : 1.0;
    return beat * kBase[note] * mul;
}

bool TapTempo::tap(double t) noexcept {
    if (t - last_ > S::kTapResetSeconds || t < last_) count_ = 0;
    else {
        for (std::size_t i = intervals_.size() - 1; i > 0; --i) intervals_[i] = intervals_[i - 1];
        intervals_[0] = t - last_;
        count_ = std::min<int>(count_ + 1, static_cast<int>(intervals_.size()));
        const int k = std::min(count_, S::kTapAverage);
        double sum = 0;
        for (int i = 0; i < k; ++i) sum += intervals_[static_cast<std::size_t>(i)];
        if (sum > 0) bpm_ = std::clamp(60.0 * k / sum, 40.0, 240.0);
    }
    last_ = t;
    return count_ > 0;
}

ToolSlot::ToolSlot() : reso_(makeResoTamer()), shuffle_(makeShuffle()), echo_(makeTapEcho()) {}
ToolSlot::~ToolSlot() = default;

void ToolSlot::prepare(double fs, std::uint32_t maxBlock) {
    reso_->prepare(fs, maxBlock);
    shuffle_->prepare(fs, maxBlock);
    echo_->prepare(fs, maxBlock);
    running_ = Off;
}
void ToolSlot::reset() noexcept { reso_->reset(); shuffle_->reset(); echo_->reset(); running_ = Off; }
void ToolSlot::setType(int t) noexcept { type_.store(std::clamp(t, 0, kTypeCount - 1), std::memory_order_relaxed); }

fx::Effect& ToolSlot::tool(int t) noexcept { return t == Shuffle ? *shuffle_ : t == Echo ? *echo_ : *reso_; }
const fx::Effect& ToolSlot::tool(int t) const noexcept { return t == Shuffle ? *shuffle_ : t == Echo ? *echo_ : *reso_; }

const char* ToolSlot::typeName(int t) noexcept {
    static const char* const n[] = {"OFF", "RESO TAMER", "SHUFFLE", "TAP ECHO"};
    return n[std::clamp(t, 0, kTypeCount - 1)];
}
const char* ToolSlot::typeKey(int t) noexcept {
    static const char* const n[] = {"off", "reso", "shuffle", "echo"};
    return n[std::clamp(t, 0, kTypeCount - 1)];
}

void ToolSlot::process(float* L, float* R, std::uint32_t frames) noexcept {
    const int t = type_.load(std::memory_order_relaxed);
    if (t != running_) {   // a newly chosen tool starts clean (no old echoes, no old gain reduction)
        if (t != Off) tool(t).reset();
        running_ = t;
    }
    if (t == Off) return;
    fx::Effect& e = tool(t);
    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t k = std::min(frames - done, fx::Effect::kMaxBlock);
        e.process(L + done, R + done, k);
        done += k;
    }
}

} // namespace hc::console
