// HEM COLOUR STAGE — implementation. Every number comes from hybrid/console/ConsoleSpecs.hpp.
#include "hybrid/console/ColourStage.hpp"

#include "hybrid/console/ConsoleSpecs.hpp"
#include "hybrid/console/Filters.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace hc::console {
namespace {

namespace S = spec::colour;
using namespace colour_ids;
using fx::ParamInfo;
using fx::Taper;

const char* const kOnOff[] = {"OFF", "ON"};
const char* const kHpfN[] = {"OFF", "30 Hz", "40 Hz", "50 Hz"};
const char* const kLowN[] = {"30 Hz", "50 Hz", "70 Hz", "90 Hz"};
const char* const kHighN[] = {"8 kHz", "12 kHz", "16 kHz", "20 kHz"};

const ParamInfo kParams[kCount] = {
    {"in_trim", "Input Trim", "dB", -S::kTrimDb, S::kTrimDb, 0.0f},
    {"hpf", "HPF", "", 0, 3, 0, Taper::Choice, kHpfN},
    {"vd_in", "Drive In", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"drive", "Drive", "/10", 1.0f, 11.0f, 5.0f},
    {"density", "Density", "/10", 0.0f, 10.0f, 5.0f},
    {"eq_in", "Tone EQ In", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"low_freq", "Low Freq", "", 0, 3, 0, Taper::Choice, kLowN},
    {"low_gain", "Low Gain", "dB", -9.0f, 9.0f, 0.0f},
    {"high_freq", "High Freq", "", 0, 3, 0, Taper::Choice, kHighN},
    {"high_gain", "High Gain", "dB", -9.0f, 9.0f, 0.0f},
    {"hfc_in", "HF Compressor In", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"hfc_crush", "Crush Mode", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"hfc_thr", "HF Threshold", "dB", -10.0f, 10.0f, 10.0f},
    {"hfc_xover", "X-Over / Wet-Dry", "Hz", S::kHfcXoverMinHz, S::kHfcXoverMaxHz, 15000.0f, Taper::Log},
    {"si_in", "Stereo Image In", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"space", "Space", "dB", -S::kSpaceDb, S::kSpaceDb, 0.0f},
    {"width", "Width", "dB", -S::kWidthDb, S::kWidthDb, 0.0f},
    {"transformer", "Transformer", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"insert", "Insert", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"insert_ms", "Insert M/S Mode", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"pre_eq", "Pre EQ", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"out_trim", "Output Trim", "dB", -S::kTrimDb, S::kTrimDb, 0.0f},
    {"bypass", "Bypass", "", 0, 1, 0, Taper::Toggle, kOnOff},
    {"bypass_post", "Bypass Post I/P Trim", "", 0, 1, 0, Taper::Toggle, kOnOff},
};
const fx::MeterInfo kMeters[] = {
    {"O/L L", "", 0, 1}, {"O/L R", "", 0, 1}, {"DRIVE THD", "%", 0, 5}, {"HF COMP GR", "dB", -12, 0},
    {"METER L", "dB", -21, 24}, {"METER R", "dB", -21, 24}, {"HOLD L", "dB", -21, 24}, {"HOLD R", "dB", -21, 24},
};

[[nodiscard]] inline float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] inline float softKnee(float v, float T) noexcept {
    const float a = std::fabs(v);
    if (a <= T) return v;
    const float o = T + (1.0f - T) * std::tanh((a - T) / (1.0f - T));
    return v < 0 ? -o : o;
}

class ColourStageFx final : public fx::Effect {
public:
    ColourStageFx() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const fx::MeterInfo> meters() const noexcept override { return kMeters; }
    std::uint32_t latency() const noexcept override { return fx::Oversampler4::latency(); }
    void setInsert(InsertSlot* s) noexcept { insert_.store(s, std::memory_order_release); }

    void prepare(double fs, std::uint32_t maxBlock) override {
        fs_ = fs;
        maxBlock_ = std::max<std::uint32_t>(1, std::min(maxBlock, kMaxBlock));
        for (int c = 0; c < 2; ++c) {
            up_[c].assign(static_cast<std::size_t>(maxBlock_) * fx::Oversampler4::kFactor, 0.0f);
            vdDelay_[c].prepare(latency());
            bypassDelay_[c].prepare(latency());
            dry_[c].assign(maxBlock_, 0.0f);
            lin_[c].assign(maxBlock_, 0.0f);
            dc_[c].set(FirstOrder::Type::HighPass, fs, 5.0);
            lfBand_[c].set(FirstOrder::Type::LowPass, fs, S::kTxLfBandHz);
            allpass_[c].setAllPass(fs, S::kTxAllpassHz);
            sparkle_[c].setHighShelf(fs, S::kTxSparkleHz, S::kTxSparkleDb);
            txBump_[c].c = BiquadCoefs::make(BqType::Bell, fs, S::kTxBumpHz, S::kTxBumpQ, S::kTxBumpDb);
        }
        holdN_ = static_cast<int>(S::kMeterHoldSeconds * fs);
        teAtt_ = coefMs(fs, 1.0); teRel_ = coefMs(fs, 50.0);
        txEnvCoef_ = coefMs(fs, 50.0);
        reset();
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) {
            hpf1_[c].clear(); hpf2_[c].clear(); osUp_[c].clear(); osDn_[c].clear(); vdDelay_[c].clear(); bypassDelay_[c].clear();
            dc_[c].clear(); low_[c].clear(); high_[c].clear(); hfcBand_[c].clear(); hfcDet_[c].clear(); spaceS_.clear();
            txHp_[c].clear(); txBump_[c].clear(); lfBand_[c].clear(); allpass_[c].clear(); sparkle_[c].clear();
            env_[c] = 0; hold_[c] = -120.0f; holdLeft_[c] = 0;
        }
        hfcGr_.clear(); crushGr_.clear();
        lastHpf_ = lastLow_ = lastHigh_ = -1; lastLowG_ = lastHighG_ = 1e9f; lastXover_ = -1.0f; lastSpace_ = 1e9f;
        lastHfcMode_ = -1; txEnv_ = 0; txHz_ = -1.0f;
        inTrim_.prepare(fs_, 20, dbToLin(p(InputTrim)));
        outTrim_.prepare(fs_, 20, dbToLin(p(OutputTrim)));
        for (int i = 0; i < 8; ++i) publishMeter(i, i >= MeterL ? -120.0f : 0.0f);
    }

    void process(float* L, float* R, std::uint32_t frames) noexcept override {
        std::uint32_t done = 0;
        while (done < frames) {
            const std::uint32_t n = std::min(frames - done, maxBlock_);
            block(L + done, R + done, n);
            done += n;
        }
    }

private:
    void block(float* L, float* R, std::uint32_t n) noexcept {
        float* x[2] = {L, R};
        const std::uint32_t lat = latency();
        // ---- O/L on the raw input ----
        const float olLin = dbToLin(S::kOverloadDbu - S::kDbuAtFullScale);
        publishMeter(MeterOverL, peakOf(L, n) > olLin ? 1.0f : 0.0f);
        publishMeter(MeterOverR, peakOf(R, n) > olLin ? 1.0f : 0.0f);

        // ---- BYPASS (standard = untouched; POST I/P TRIM = input trim only), timing kept the same ----
        if (p(Bypass) >= 0.5f) {
            const bool post = p(BypassPostTrim) >= 0.5f;
            const float tT = post ? dbToLin(p(InputTrim)) : 1.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float g = inTrim_.next(tT);
                for (int c = 0; c < 2; ++c) x[c][i] = bypassDelay_[c].process(x[c][i] * g, lat);
            }
            meter(L, R, n);   // in BYPASS the meter shows the input
            publishMeter(MeterDriveThd, 0.0f);
            publishMeter(MeterHfcGr, 0.0f);
            return;
        }
        for (int c = 0; c < 2; ++c) bypassDelay_[c].clear();

        // ---- INPUT TRIM ----
        const float inT = dbToLin(p(InputTrim));
        for (std::uint32_t i = 0; i < n; ++i) { const float g = inTrim_.next(inT); L[i] *= g; R[i] *= g; }

        // ---- HPF (3rd-order Butterworth: 1st-order + 2nd-order Q 1) ----
        const int hpf = static_cast<int>(p(Hpf) + 0.5f);
        if (hpf != lastHpf_) {
            if (hpf > 0) {
                const double hz = S::kHpfHz[static_cast<std::size_t>(hpf - 1)];
                for (int c = 0; c < 2; ++c) { hpf1_[c].set(FirstOrder::Type::HighPass, fs_, hz); hpf2_[c].c = BiquadCoefs::make(BqType::HighPass, fs_, hz, 1.0); }
            }
            for (int c = 0; c < 2; ++c) { hpf1_[c].clear(); hpf2_[c].clear(); }
            lastHpf_ = hpf;
        }
        if (hpf > 0)
            for (int c = 0; c < 2; ++c)
                for (std::uint32_t i = 0; i < n; ++i) x[c][i] = hpf2_[c].tick(static_cast<float>(hpf1_[c].tick(x[c][i])));

        // ---- DRIVE (4x oversampled) or the matching delay ----
        driveStage(x, n);

        const bool insOn = p(Insert) >= 0.5f, insMs = p(InsertMs) >= 0.5f, preEq = p(PreEq) >= 0.5f;
        InsertSlot* slot = insOn ? insert_.load(std::memory_order_acquire) : nullptr;
        if (slot && !insMs && preEq) slot->process(L, R, n);          // stereo insert before the EQ
        toneEq(x, n);
        if (slot && !insMs && !preEq) slot->process(L, R, n);         // stereo insert after the EQ (default)
        hfCompressor(x, n);

        // ---- M/S section: STEREO IMAGE, with the M/S insert before (PRE EQ) or after it ----
        const bool image = p(ImageIn) >= 0.5f;
        if (image || (slot && insMs)) {
            for (std::uint32_t i = 0; i < n; ++i) { const float m = (L[i] + R[i]) * 0.5f, s = (L[i] - R[i]) * 0.5f; L[i] = m; R[i] = s; }
            if (slot && insMs && preEq) slot->process(L, R, n);
            if (image) stereoImage(R, n);
            if (slot && insMs && !preEq) slot->process(L, R, n);
            for (std::uint32_t i = 0; i < n; ++i) { const float m = L[i], s = R[i]; L[i] = m + s; R[i] = m - s; }
        }

        if (p(Transformer) >= 0.5f) transformer(x, n);
        else for (int c = 0; c < 2; ++c) { txHp_[c].clear(); txBump_[c].clear(); allpass_[c].clear(); sparkle_[c].clear(); lfBand_[c].clear(); }

        const float outT = dbToLin(p(OutputTrim));
        for (std::uint32_t i = 0; i < n; ++i) { const float g = outTrim_.next(outT); L[i] *= g; R[i] *= g; }
        meter(L, R, n);
    }

    void driveStage(float** x, std::uint32_t n) noexcept {
        const std::uint32_t lat = latency();
        if (p(DriveIn) < 0.5f) {
            for (int c = 0; c < 2; ++c) {
                for (std::uint32_t i = 0; i < n; ++i) x[c][i] = vdDelay_[c].process(x[c][i], lat);
                osUp_[c].clear(); osDn_[c].clear(); dc_[c].clear();
            }
            publishMeter(MeterDriveThd, 0.0f);
            return;
        }
        const float drive = p(Drive), dens = p(Density);
        const float gd = dbToLin(S::kDriveDbPerStep * (drive - 1.0f));
        const float t = (dens - S::kDensityLowAt) / (S::kDensityHighAt - S::kDensityLowAt);
        const float T = std::clamp(lerp(S::kKneeLow, S::kKneeHigh, t), S::kKneeMin, S::kKneeMax);
        const float e = std::clamp(lerp(S::kEvenLow, S::kEvenHigh, t), 0.0f, S::kEvenMax);
        const float outGain = dbToLin(S::kDensityLevelDb * dens / 10.0f) / gd;
        const float expand = std::max(0.0f, 1.0f - dens) * S::kMinExpansionDb;   // MIN-density expansion (dB at full scale)
        // hiss: -75 dBu at mid settings, falling as DRIVE and DENSITY rise
        const float hissDb = S::kHissDbu - S::kDbuAtFullScale - S::kHissFallDb * ((drive - 6.0f) / 5.0f + (dens - 5.0f) / 5.0f) * 0.5f;
        const float hiss = dbToLin(hissDb) * std::sqrt(3.0f);   // uniform noise with that RMS
        double errSq = 0, outSq = 0;
        for (int c = 0; c < 2; ++c) {
            float* u = up_[c].data();
            for (std::uint32_t i = 0; i < n; ++i) lin_[c][i] = x[c][i];
            osUp_[c].up(x[c], u, n);
            for (std::uint32_t j = 0; j < n * fx::Oversampler4::kFactor; ++j) {
                float v = u[j] * gd;
                if (expand > 0.0f) {
                    env_[c] = std::fabs(v) + (std::fabs(v) > env_[c] ? teAtt_ : teRel_) * (env_[c] - std::fabs(v));
                    v *= dbToLin(expand * std::min(env_[c], 1.0f));
                }
                const float k = softKnee(v, T);
                u[j] = (k + e * k * k) * outGain;
            }
            osDn_[c].down(u, x[c], n);
            for (std::uint32_t i = 0; i < n; ++i) {
                x[c][i] = static_cast<float>(dc_[c].tick(x[c][i])) + hiss * (2.0f * rnd() - 1.0f);
                // distortion estimate for the LED: output vs the (latency-aligned) clean input at matched level
                const float clean = vdDelay_[c].process(lin_[c][i], latency()) * dbToLin(S::kDensityLevelDb * dens / 10.0f);
                const double d = static_cast<double>(x[c][i]) - clean;
                errSq += d * d; outSq += static_cast<double>(x[c][i]) * x[c][i];
            }
        }
        publishMeter(MeterDriveThd, outSq > 1e-12 ? static_cast<float>(100.0 * std::sqrt(errSq / outSq)) : 0.0f);
    }

    void toneEq(float** x, std::uint32_t n) noexcept {
        if (p(EqIn) < 0.5f) return;
        const int lf = static_cast<int>(p(LowFreq) + 0.5f), hf = static_cast<int>(p(HighFreq) + 0.5f);
        const float lg = p(LowGain), hg = p(HighGain);
        if (lf != lastLow_ || lg != lastLowG_) {
            for (int c = 0; c < 2; ++c) low_[c].setLowShelf(fs_, S::kLowMidHz[static_cast<std::size_t>(lf)], lg / 9.0f * S::kToneDepthAtNineDb);
            lastLow_ = lf; lastLowG_ = lg;
        }
        if (hf != lastHigh_ || hg != lastHighG_) {
            for (int c = 0; c < 2; ++c) high_[c].setHighShelf(fs_, S::kHighMidHz[static_cast<std::size_t>(hf)], hg / 9.0f * S::kToneDepthAtNineDb);
            lastHigh_ = hf; lastHighG_ = hg;
        }
        for (int c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < n; ++i) x[c][i] = static_cast<float>(high_[c].tick(low_[c].tick(x[c][i])));
    }

    void hfCompressor(float** x, std::uint32_t n) noexcept {
        if (p(HfcIn) < 0.5f) { publishMeter(MeterHfcGr, 0.0f); hfcGr_.clear(); crushGr_.clear(); return; }
        const bool crush = p(HfcCrush) >= 0.5f;
        const float thrDbu = p(HfcThreshold) + S::kHfcThresholdRefDbu;
        const float thr = thrDbu - S::kDbuAtFullScale;
        const int mode = crush ? 1 : 0;
        if (mode != lastHfcMode_) {
            hfcGr_.set(fs_, S::kHfcAttackMs, S::kHfcReleaseMs, false, 1, 1);
            crushGr_.set(fs_, S::kCrushAttackMs, S::kCrushReleaseMs, false, 1, 1);
            hfcGr_.clear(); crushGr_.clear();
            lastHfcMode_ = mode;
        }
        float grMax = 0.0f;
        if (!crush) {
            const float xo = p(HfcXover);
            if (xo != lastXover_) {
                const auto c2 = BiquadCoefs::make(BqType::HighPass, fs_, xo, S::kHfcXoverQ);
                for (int c = 0; c < 2; ++c) { hfcBand_[c].c = c2; hfcDet_[c].c = c2; }
                lastXover_ = xo;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                const float bl = hfcBand_[0].tick(x[0][i]), br = hfcBand_[1].tick(x[1][i]);
                const float lvl = linToDb(std::max(std::fabs(bl), std::fabs(br)));
                const float gr = hfcGr_.tick(-compGainDb(lvl, thr, S::kHfcRatio, S::kHfcKneeDb));
                grMax = std::max(grMax, gr);
                const float g = dbToLin(-gr) - 1.0f;   // dynamic high shelf: only the band above X-OVER moves
                x[0][i] += g * bl; x[1][i] += g * br;
            }
        } else {
            const float wet = colourCrushWet(p(HfcXover));
            const float makeup = dbToLin((1.0f - 1.0f / S::kCrushRatio) * std::max(0.0f, S::kCrushMakeupRefDbu - thrDbu));
            for (std::uint32_t i = 0; i < n; ++i) {
                const float lvl = linToDb(std::max(std::fabs(x[0][i]), std::fabs(x[1][i])));
                const float gr = crushGr_.tick(-compGainDb(lvl, thr, S::kCrushRatio, S::kCrushKneeDb));
                grMax = std::max(grMax, gr);
                const float g = (1.0f - wet) + wet * dbToLin(-gr) * makeup;
                x[0][i] *= g; x[1][i] *= g;
            }
        }
        publishMeter(MeterHfcGr, -grMax);
    }

    void stereoImage(float* side, std::uint32_t n) noexcept {
        const float sp = p(Space), w = dbToLin(p(Width));
        if (sp != lastSpace_) { spaceS_.setLowShelf(fs_, S::kSpaceHz, sp); lastSpace_ = sp; }
        for (std::uint32_t i = 0; i < n; ++i) side[i] = static_cast<float>(spaceS_.tick(side[i])) * w;
    }

    void transformer(float** x, std::uint32_t n) noexcept {
        constexpr std::uint32_t kCtl = 32;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (i % kCtl == 0) {
                // the low-frequency roll-off moves up with level (FITTED corner at +16 dBu, ASSUMED law)
                const float lvlDbu = linToDb(std::sqrt(std::max(txEnv_, 1e-12f))) + S::kDbuAtFullScale;
                const float hz = std::clamp(S::kTxHpHzAt16Dbu * std::pow(10.0f, (lvlDbu - 16.0f) / (2.0f * S::kTxHpDbPerDecadeOfLevel)), 3.0f, 60.0f);
                if (std::fabs(hz - txHz_) > 0.05f) {
                    txHp_[0].set(FirstOrder::Type::HighPass, fs_, hz);
                    txHp_[1].set(FirstOrder::Type::HighPass, fs_, hz);
                    txHz_ = hz;
                }
            }
            const float ms = 0.5f * (x[0][i] * x[0][i] + x[1][i] * x[1][i]);
            txEnv_ = ms + txEnvCoef_ * (txEnv_ - ms);
            const float lvl16 = std::sqrt(std::max(txEnv_, 1e-12f)) / dbToLin(16.0f - S::kDbuAtFullScale);
            const float h = S::kTxHarmonicAt16Dbu * std::min(lvl16, 4.0f) * 4.0f;   // LF harmonics grow with level
            for (int c = 0; c < 2; ++c) {
                float v = txBump_[c].tick(static_cast<float>(txHp_[c].tick(x[c][i])));
                const float lf = static_cast<float>(lfBand_[c].tick(v));
                v += h * (lf * lf + lf * lf * lf * 0.5f);   // even + odd low-frequency harmonics
                v = static_cast<float>(allpass_[c].tick(v));   // slight top-end phase shift (flat level)
                x[c][i] = static_cast<float>(sparkle_[c].tick(v));
            }
        }
    }

    void meter(const float* L, const float* R, std::uint32_t n) noexcept {
        const float pk[2] = {peakOf(L, n), peakOf(R, n)};
        for (int c = 0; c < 2; ++c) {
            const float db = linToDb(pk[c]) + S::kDbuAtFullScale;
            if (db >= hold_[c]) { hold_[c] = db; holdLeft_[c] = holdN_; }
            else if ((holdLeft_[c] -= static_cast<int>(n)) <= 0) hold_[c] = db;
            publishMeter(MeterL + c, db);
            publishMeter(MeterHoldL + c, hold_[c]);
        }
    }

    float rnd() noexcept { rng_ = rng_ * 1664525u + 1013904223u; return static_cast<float>(rng_ >> 8) / 16777216.0f; }

    double fs_ = 48000.0;
    std::uint32_t maxBlock_ = 512;
    std::atomic<InsertSlot*> insert_{nullptr};
    FirstOrder hpf1_[2]; Biquad hpf2_[2]; int lastHpf_ = -1;
    fx::Oversampler4 osUp_[2], osDn_[2];
    std::vector<float> up_[2], dry_[2], lin_[2];
    fx::DelayLine vdDelay_[2], bypassDelay_[2];
    FirstOrder dc_[2];
    float env_[2]{0, 0}, teAtt_ = 0, teRel_ = 0;
    FirstOrder low_[2], high_[2]; int lastLow_ = -1, lastHigh_ = -1; float lastLowG_ = 1e9f, lastHighG_ = 1e9f;
    Biquad hfcBand_[2], hfcDet_[2]; float lastXover_ = -1.0f; int lastHfcMode_ = -1;
    DualRate hfcGr_, crushGr_;
    FirstOrder spaceS_; float lastSpace_ = 1e9f;
    FirstOrder txHp_[2]; Biquad txBump_[2]; FirstOrder lfBand_[2], allpass_[2], sparkle_[2]; float txEnv_ = 0, txEnvCoef_ = 0, txHz_ = -1.0f;
    fx::Smoothed inTrim_, outTrim_;
    float hold_[2]{-120, -120}; int holdLeft_[2]{0, 0}, holdN_ = 0;
    std::uint32_t rng_ = 0x12345u;
};

} // namespace

float colourDriveLedColour(float thd) noexcept {
    if (thd > S::kLedRedThd) return 3.0f;
    if (thd > S::kLedOrangeThd) return 2.0f;
    if (thd > S::kLedGreenThd) return 1.0f;
    return 0.0f;
}
float colourCrushWet(float xoverHz) noexcept {
    const float t = std::log(std::clamp(xoverHz, S::kHfcXoverMinHz, S::kHfcXoverMaxHz) / S::kHfcXoverMinHz) / std::log(S::kHfcXoverMaxHz / S::kHfcXoverMinHz);
    return t;   // ASSUMED: the knob's travel maps straight onto 0 % .. 100 % wet
}
void colourStageSetInsert(fx::Effect& e, InsertSlot* slot) noexcept {
    if (auto* c = dynamic_cast<ColourStageFx*>(&e)) c->setInsert(slot);
}
std::unique_ptr<fx::Effect> makeColourStage() { return std::make_unique<ColourStageFx>(); }

} // namespace hc::console
