// HEM ECHO DECK: tape-style echo with styles, groove, feel, saturation, tap tempo and ping-pong.
// Our own design. Every guess it makes is in include/hem/echodeck/EchoDeckConfig.hpp.
//
// The loop has two stages so every other repeat can be moved (GROOVE):
//   repeat 1 = X line (the input, read at a + feel) + A line (the loop return, read at a)  -> stage A
//   repeat 2 = B line (repeat 1, read at b) * feedback                                      -> stage B
//   A line is fed with feedback * repeat 2, so the repeats run 1, fb, fb^2 ... at a, a+b, 2a+b, 2a+2b ...
//   a = T (1 + g), b = T (1 - g): g = 0 gives even spacing, g = +-1/3 gives a triplet shuffle / swing.
// Each stage colours its repeat once: style curve (drive = style + SATURATION) -> style band limits ->
// LOW CUT / HIGH CUT -> diffusion all-passes. So every repeat is darker / dirtier than the last.
// SINGLE: left and right have their own loops. PING-PONG: mono input, repeat 1 left, repeat 2 right ...
// Audio thread: no allocation, no locks, no waiting. Parameters are read once per block.
#include "hem/echodeck/EchoDeckConfig.hpp"
#include "hem/echodeck/HemEchoDeck.hpp"
#include "hybrid/fx/Dsp.hpp"

#include <cmath>
#include <numbers>

namespace hem::echodeck {
namespace {
using namespace hc::fx;

enum P { Input, Output, Mix, TimeMode, Time, Note, Feedback, LowCut, HighCut, TempoSrc, Bpm, Tap,
         Groove, Feel, Saturation, StyleSel, Mode, kCount };

const char* const kTimeModes[] = {"MS", "NOTE", "DOT", "TRIP"};
const char* const kNoteNames[] = {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1"};
const char* const kTempoNames[] = {"HOST", "TAP"};
const char* const kOnOff[] = {"OFF", "ON"};
const char* const kModeNames[] = {"SINGLE", "PING-PONG"};
const char* const kStyleNames[] = {cfg::kStyles[0].name, cfg::kStyles[1].name, cfg::kStyles[2].name, cfg::kStyles[3].name,
                                   cfg::kStyles[4].name, cfg::kStyles[5].name, cfg::kStyles[6].name, cfg::kStyles[7].name};
constexpr int kStyleCount = static_cast<int>(cfg::kStyles.size());
static_assert(std::size(kStyleNames) == cfg::kStyles.size());

const ParamInfo kParams[kCount] = {
    {"input",      "Input",       "dB", cfg::kInOutMinDb, cfg::kInOutMaxDb, 0.0f},
    {"output",     "Output",      "dB", cfg::kInOutMinDb, cfg::kInOutMaxDb, 0.0f},
    {"mix",        "Mix",         "%",  0.0f, 100.0f, 50.0f},
    {"timemode",   "Time Mode",   "",   0.0f, 3.0f, 1.0f, Taper::Choice, kTimeModes},
    {"time",       "Echo Time",   "ms", cfg::kTimeMinMs, cfg::kTimeMaxMs, 375.0f, Taper::Log},
    {"note",       "Note",        "",   0.0f, 6.0f, 4.0f, Taper::Choice, kNoteNames},
    {"feedback",   "Feedback",    "%",  0.0f, 100.0f, 0.0f},
    {"lowcut",     "Low Cut",     "Hz", cfg::kLowCutOffHz, 2000.0f, cfg::kLowCutOffHz, Taper::Log},
    {"highcut",    "High Cut",    "Hz", 1000.0f, cfg::kHighCutOffHz, cfg::kHighCutOffHz, Taper::Log},
    {"temposrc",   "Tempo",       "",   0.0f, 1.0f, 0.0f, Taper::Choice, kTempoNames},
    {"bpm",        "BPM",         "",   cfg::kBpmMin, cfg::kBpmMax, 120.0f},
    {"tap",        "Tap Tempo",   "",   0.0f, 1.0f, 0.0f, Taper::Toggle, kOnOff},
    {"groove",     "Groove",      "%",  -100.0f, 100.0f, 0.0f},
    {"feel",       "Feel",        "%",  -100.0f, 100.0f, 0.0f},
    {"saturation", "Saturation",  "%",  0.0f, 100.0f, 0.0f},
    {"style",      "Style",       "",   0.0f, static_cast<float>(kStyleCount - 1), 0.0f, Taper::Choice, kStyleNames},
    {"mode",       "Mode",        "",   0.0f, 1.0f, 0.0f, Taper::Choice, kModeNames},
};
// 0 ECHO time, 1 BPM in use, 2 IN (echo input level, after INPUT), 3 OUT (echo level, after OUTPUT)
const MeterInfo kMeters[] = {{"ECHO", "ms", 0.0f, 4000.0f}, {"BPM", "", cfg::kBpmMin, cfg::kBpmMax},
                             {"IN", "dB", -54.0f, 0.0f}, {"OUT", "dB", -54.0f, 0.0f}};

constexpr float kLog10Mul = 8.685889638065035f;   // 20 / ln(10)

float shape(cfg::Shape s, float u) noexcept {
    switch (s) {
    case cfg::Shape::Clean: {   // straight up to 1, then a soft limit that never passes 2
        const float a = std::fabs(u);
        return a <= 1.0f ? u : std::copysign(1.0f + std::tanh(a - 1.0f), u);
    }
    case cfg::Shape::Soft: return std::tanh(u);
    case cfg::Shape::Tape: return std::tanh(u + cfg::kTapeEven * u * u);
    case cfg::Shape::Hard: {    // cubic: slope 1 at 0, flat at +-2/3 from |u| = 1
        const float c = std::clamp(u, -1.0f, 1.0f);
        return c - c * c * c * (1.0f / 3.0f);
    }
    }
    return u;
}

// One Schroeder all-pass: y = -g x + z[n-D]; z[n] = x + g y.
struct AllPass {
    std::vector<float> buf;
    std::uint32_t pos = 0;
    void prepare(std::uint32_t d) { buf.assign(std::max<std::uint32_t>(d, 1), 0.0f); pos = 0; }
    void clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    float tick(float x, float g) noexcept {
        const float z = buf[pos];
        const float y = -g * x + z;
        buf[pos] = x + g * y;
        if (++pos == buf.size()) pos = 0;
        return y;
    }
};

// What colours one repeat. Coefficients are shared per block; state is per stage and side.
struct Colour {
    Biquad styleHp, styleLp, userLc, userHc;
    std::array<AllPass, 4> ap;
    void clear() noexcept { styleHp.clear(); styleLp.clear(); userLc.clear(); userHc.clear(); for (auto& a : ap) a.clear(); }
};

class EchoDeck final : public Effect {
public:
    EchoDeck() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    bool paramActive(int i) const noexcept override {
        const bool ms = static_cast<int>(p(TimeMode) + 0.5f) == 0;
        switch (i) {
        case Time: return ms;
        case Note: case TempoSrc: return !ms;
        case Bpm: return !ms && p(TempoSrc) >= 0.5f;
        default: return true;
        }
    }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        const double maxS = cfg::kMaxTimeS * (1.0 + cfg::kGrooveMax) + cfg::kFeelMaxMs * 0.001 + cfg::kModMaxMs * 0.001;
        const auto size = static_cast<std::uint32_t>(std::ceil(maxS * fs)) + 8;
        for (int c = 0; c < 2; ++c) { x_[c].prepare(size); a_[c].prepare(size); b_[c].prepare(size); }
        for (int s = 0; s < 2; ++s)
            for (int c = 0; c < 2; ++c)
                for (std::size_t k = 0; k < 4; ++k)
                    col_[s][c].ap[k].prepare(static_cast<std::uint32_t>(std::lround(cfg::kDiffuseMs[k] * 0.001 * fs * (c == 0 ? 1.0 : 1.07))));
        lastStyle_ = -1; lastLc_ = lastHc_ = -1.0f;
        reset();
    }

    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { x_[c].clear(); a_[c].clear(); b_[c].clear(); }
        for (auto& s : col_) for (auto& c : s) c.clear();
        tCur_ = tFrom_ = tTo_ = tPending_ = -1.0;
        w_ = 1.0;
        phWow_ = phFl_ = 0.0;
        tapPrimed_ = false; haveTap_ = false; tapCount_ = 0; tapNext_ = 0; sinceTap_ = 0;
        lastMode_ = -1;
        in_.prepare(fs_, cfg::kSmoothMs, dbToGain(p(Input)));
        out_.prepare(fs_, cfg::kSmoothMs, dbToGain(p(Output)));
        mix_.prepare(fs_, cfg::kSmoothMs, p(Mix) * 0.01f);
        fb_.prepare(fs_, cfg::kSmoothMs, p(Feedback) * 0.01f * cfg::kFeedbackMax);
        g_.prepare(fs_, cfg::kGlideMs, grooveOf());
        off_.prepare(fs_, cfg::kGlideMs, feelOf());
        drive_.prepare(fs_, cfg::kSmoothMs, driveOf());
        publishMeter(0, 0.0f); publishMeter(1, 120.0f);
        publishMeter(2, cfg::kMeterFloorDb); publishMeter(3, cfg::kMeterFloorDb);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        handleTap();
        // ---- block settings -------------------------------------------------------------
        const int styleIdx = std::clamp(static_cast<int>(p(StyleSel) + 0.5f), 0, kStyleCount - 1);
        const cfg::Style& st = cfg::kStyles[static_cast<std::size_t>(styleIdx)];
        updateFilters(styleIdx, st);
        const int mode = std::clamp(static_cast<int>(p(Mode) + 0.5f), 0, 1);
        if (mode != lastMode_) {   // the other layout's echoes would come back in odd places: start clean
            if (lastMode_ >= 0) { for (int c = 0; c < 2; ++c) { x_[c].clear(); a_[c].clear(); b_[c].clear(); } for (auto& s : col_) for (auto& c : s) c.clear(); }
            lastMode_ = mode;
        }
        const bool msMode = static_cast<int>(p(TimeMode) + 0.5f) == 0;
        const double target = targetSamples();
        if (tCur_ < 0.0) tCur_ = tFrom_ = tTo_ = tPending_ = target;
        tPending_ = target;
        const float glide = onePoleCoef(fs_, cfg::kGlideMs);
        const double fadeStep = 1.0 / std::max(1.0, cfg::kNoteFadeMs * 0.001 * fs_);
        if (msMode) w_ = 1.0;

        const float inT = dbToGain(p(Input)), outT = dbToGain(p(Output)), mixT = p(Mix) * 0.01f;
        const float fbT = p(Feedback) * 0.01f * cfg::kFeedbackMax;
        const float gT = grooveOf(), offT = feelOf();
        const float driveT = driveOf();
        const float apG = cfg::kDiffuseGainMax * st.diffusion;
        const bool diffuse = apG > 0.0f;
        const double wowAmp = st.wowMs * 0.001 * fs_, flAmp = st.flutterMs * 0.001 * fs_;
        const double wowInc = st.wowHz / fs_, flInc = st.flutterHz / fs_;
        const bool mod = wowAmp > 0.0 || flAmp > 0.0;
        const bool lc = lastLc_ > cfg::kLowCutOffHz, hc = lastHc_ < cfg::kHighCutOffHz;
        const bool sHp = st.hpHz > 0.0f, sLp = st.lpHz > 0.0f;
        const double maxT = cfg::kMaxTimeS * fs_;

        float inPk = 0.0f, outPk = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            // ---- time ---------------------------------------------------------------------
            if (msMode) {
                tCur_ = tPending_ + glide * (tCur_ - tPending_);
                tFrom_ = tTo_ = tCur_;
            } else if (w_ >= 1.0) {
                tCur_ = tFrom_ = tTo_;
                if (std::fabs(tPending_ - tTo_) > 0.5) { tTo_ = tPending_; w_ = 0.0; }
            } else {
                w_ = std::min(1.0, w_ + fadeStep);
            }
            const double g = g_.next(gT), off = off_.next(offT);
            double m0 = 0.0, m1 = 0.0;
            if (mod) {
                const double tw = 2.0 * std::numbers::pi * phWow_, tf = 2.0 * std::numbers::pi * phFl_;
                m0 = wowAmp * (1.0 + std::sin(tw)) + flAmp * (1.0 + std::sin(tf));                 // never negative: a delay can't read the future
                m1 = wowAmp * (1.0 + std::cos(tw)) + flAmp * (1.0 + std::cos(tf));                 // right side 90 degrees on
                phWow_ += wowInc; if (phWow_ >= 1.0) phWow_ -= 1.0;
                phFl_ += flInc; if (phFl_ >= 1.0) phFl_ -= 1.0;
            }
            const float wl = static_cast<float>(w_);
            const double tf = std::min(tFrom_, maxT), tt = std::min(tTo_, maxT);
            const Taps from = taps(tf, g, off), to = taps(tt, g, off);

            const float ig = in_.next(inT), og = out_.next(outT), mx = mix_.next(mixT), fb = fb_.next(fbT), dr = drive_.next(driveT);
            const float invDr = 1.0f / dr;
            const float xl = L[i], xr = R[i];
            const float wetG = std::min(1.0f, 2.0f * mx), dryG = std::min(1.0f, 2.0f * (1.0f - mx));

            const auto colour = [&](Colour& c, float v) noexcept {
                v = shape(st.shape, v * dr) * invDr;
                if (sHp) v = c.styleHp.tick(v);
                if (sLp) v = c.styleLp.tick(v);
                if (lc) v = c.userLc.tick(v);
                if (hc) v = c.userHc.tick(v);
                if (diffuse) for (auto& ap : c.ap) v = ap.tick(v, apG);
                return v;
            };
            const auto side = [&](int c, float xin, double m, float& rep1, float& rep2) noexcept {
                const float fm = static_cast<float>(m);
                const auto rd = [&](const DelayLine& line, double dFrom, double dTo) noexcept {
                    const float a = line.read(static_cast<float>(dFrom) + fm - 1.0f);
                    return wl >= 1.0f ? a : a + wl * (line.read(static_cast<float>(dTo) + fm - 1.0f) - a);
                };
                const float r1 = rd(x_[c], from.d1, to.d1) + rd(a_[c], from.a, to.a);
                rep1 = colour(col_[0][c], r1);
                rep2 = fb * colour(col_[1][c], rd(b_[c], from.b, to.b));
                x_[c].write(xin);
                a_[c].write(fb * rep2);
                b_[c].write(rep1);
            };

            float el, er;
            if (mode == 0) {
                const float il = ig * xl, ir = ig * xr;
                inPk = std::max(inPk, std::max(std::fabs(il), std::fabs(ir)));
                float l1, l2, r1, r2;
                side(0, il, m0, l1, l2);
                side(1, ir, m1, r1, r2);
                el = og * (l1 + l2); er = og * (r1 + r2);
            } else {
                const float mono = ig * 0.5f * (xl + xr);
                inPk = std::max(inPk, std::fabs(mono));
                float p1, p2;
                side(0, mono, m0, p1, p2);
                el = og * p1; er = og * p2;
            }
            outPk = std::max(outPk, std::max(std::fabs(el), std::fabs(er)));
            L[i] = dryG * xl + wetG * el;
            R[i] = dryG * xr + wetG * er;
        }
        sinceTap_ += n;
        publishMeter(0, static_cast<float>(tTo_ / fs_ * 1000.0));
        publishMeter(1, static_cast<float>(bpmInUse()));
        publishMeter(2, std::max(cfg::kMeterFloorDb, kLog10Mul * std::log(inPk + 1e-12f)));
        publishMeter(3, std::max(cfg::kMeterFloorDb, kLog10Mul * std::log(outPk + 1e-12f)));
    }

private:
    struct Taps { double a, b, d1; };
    // Delay of each read, in samples, for an echo time t (samples), groove g and feel offset (samples).
    static Taps taps(double t, double g, double off) noexcept {
        const double a = std::max(1.0, t * (1.0 + g)), b = std::max(1.0, t * (1.0 - g));
        return {a, b, std::max(1.0, a + off)};
    }
    const cfg::Style& styleOf() const noexcept {
        return cfg::kStyles[static_cast<std::size_t>(std::clamp(static_cast<int>(p(StyleSel) + 0.5f), 0, kStyleCount - 1))];
    }
    float driveOf() const noexcept { return dbToGain(styleOf().driveDb + p(Saturation) * 0.01f * cfg::kSatMaxDriveDb); }
    float grooveOf() const noexcept { return static_cast<float>(-p(Groove) * 0.01 * cfg::kGrooveMax); }   // - = shuffle = repeat 1, 3 .. later
    float feelOf() const noexcept { return static_cast<float>(-p(Feel) * 0.01 * cfg::kFeelMaxMs * 0.001 * fs_); }   // - = DRAG = later

    double bpmInUse() const noexcept {
        const double bpm = p(TempoSrc) >= 0.5f ? static_cast<double>(p(Bpm)) : static_cast<double>(tempo());
        return std::clamp(bpm, static_cast<double>(cfg::kBpmMin), static_cast<double>(cfg::kBpmMax));
    }
    double targetSamples() const noexcept {
        const int tm = std::clamp(static_cast<int>(p(TimeMode) + 0.5f), 0, 3);
        double s;
        if (tm == 0) s = p(Time) * 0.001;
        else {
            const int ni = std::clamp(static_cast<int>(p(Note) + 0.5f), 0, static_cast<int>(cfg::kNoteBeats.size()) - 1);
            double beats = cfg::kNoteBeats[static_cast<std::size_t>(ni)];
            if (tm == 2) beats *= cfg::kDotMul;
            if (tm == 3) beats *= cfg::kTripMul;
            s = beats * 60.0 / bpmInUse();
        }
        return std::clamp(s, cfg::kTimeMinMs * 0.001, cfg::kMaxTimeS) * fs_;
    }

    // TAP: every flip of the switch is a tap. Gaps are timed in samples (to the block).
    void handleTap() noexcept {
        const float v = p(Tap);
        if (!tapPrimed_) { tapPrimed_ = true; lastTapV_ = v; return; }
        if (v == lastTapV_) return;
        lastTapV_ = v;
        const double gap = static_cast<double>(sinceTap_) / fs_;
        if (!haveTap_ || gap > cfg::kTapMaxGapS) { haveTap_ = true; tapCount_ = 0; tapNext_ = 0; sinceTap_ = 0; return; }
        if (gap < cfg::kTapMinGapS) return;   // too quick: ignored, the series keeps its last tap
        sinceTap_ = 0;
        gaps_[static_cast<std::size_t>(tapNext_)] = gap;
        tapNext_ = (tapNext_ + 1) % cfg::kTapAverage;
        tapCount_ = std::min(tapCount_ + 1, cfg::kTapAverage);
        double sum = 0.0;
        for (int k = 0; k < tapCount_; ++k) sum += gaps_[static_cast<std::size_t>(k)];
        const double bpm = 60.0 / (sum / tapCount_);
        set(Bpm, static_cast<float>(bpm));
        set(TempoSrc, 1.0f);
    }

    void updateFilters(int styleIdx, const cfg::Style& st) noexcept {
        const float lcHz = p(LowCut), hcHz = p(HighCut);
        if (styleIdx != lastStyle_) {
            const auto hp = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, st.hpHz > 0 ? st.hpHz : 20.0, cfg::kFilterQ);
            const auto lp = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, st.lpHz > 0 ? st.lpHz : 20000.0, cfg::kFilterQ);
            for (auto& s : col_) for (auto& c : s) { c.styleHp.c = hp; c.styleLp.c = lp; }
            lastStyle_ = styleIdx;
        }
        if (lcHz != lastLc_) {
            const auto f = BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, lcHz, cfg::kFilterQ);
            for (auto& s : col_) for (auto& c : s) { if (lastLc_ <= cfg::kLowCutOffHz) c.userLc.clear(); c.userLc.c = f; }
            lastLc_ = lcHz;
        }
        if (hcHz != lastHc_) {
            const auto f = BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, hcHz, cfg::kFilterQ);
            for (auto& s : col_) for (auto& c : s) { if (lastHc_ >= cfg::kHighCutOffHz || lastHc_ < 0.0f) c.userHc.clear(); c.userHc.c = f; }
            lastHc_ = hcHz;
        }
    }

    double fs_ = 48000.0;
    DelayLine x_[2], a_[2], b_[2];
    Colour col_[2][2];   // [stage][side]
    double tCur_ = -1, tFrom_ = -1, tTo_ = -1, tPending_ = -1, w_ = 1;
    double phWow_ = 0, phFl_ = 0;
    Smoothed in_, out_, mix_, fb_, g_, off_, drive_;
    int lastStyle_ = -1, lastMode_ = -1;
    float lastLc_ = -1, lastHc_ = -1;
    // tap tempo
    bool tapPrimed_ = false, haveTap_ = false;
    float lastTapV_ = 0;
    int tapCount_ = 0, tapNext_ = 0;
    std::uint64_t sinceTap_ = 0;
    std::array<double, cfg::kTapAverage> gaps_{};
};

} // namespace

std::unique_ptr<hc::fx::Effect> make() { return std::make_unique<EchoDeck>(); }

} // namespace hem::echodeck
