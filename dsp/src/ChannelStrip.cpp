#include "hybrid/dsp/ChannelStrip.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::dsp {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr float  kTinyDb = 1e-3f;   // closer than this to the target = arrived

// RBJ "Audio EQ Cookbook" designs, normalised so a0 = 1.
struct Coefs { double b0, b1, b2, a1, a2; };
Coefs norm(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
Coefs highPass(double fs, double f) noexcept {
    const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / std::numbers::sqrt2;   // Q = 1/sqrt(2): alpha = sin(w) / (2Q)
    return norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
Coefs peak(double fs, double f, double q, double db) noexcept {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
Coefs lowShelf(double fs, double f, double db) noexcept {   // shelf slope S = 1
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::numbers::sqrt2, s = 2 * std::sqrt(A) * al;
    return norm(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}
Coefs highShelf(double fs, double f, double db) noexcept {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::numbers::sqrt2, s = 2 * std::sqrt(A) * al;
    return norm(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}

float approach(float cur, float to, float k) noexcept {
    const float d = to - cur;
    return std::fabs(d) < kTinyDb ? to : cur + d * k;
}
float approachLog(float cur, float to, float k) noexcept {   // frequencies / times glide on a log scale
    if (std::fabs(to - cur) < 1e-3f * to) return to;
    return std::exp(std::log(cur) + (std::log(to) - std::log(cur)) * k);
}
} // namespace

void ChannelStrip::prepare(double sampleRate) noexcept {
    fs_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    reset();
}

void ChannelStrip::reset() noexcept {
    cur_ = targets();
    coefValid_ = false;
    hpf_.clear(); low_.clear(); mid_.clear(); high_.clear();
    envDb_ = 0.0f;
    grPub_.store(0.0f, kRelaxed);
}

ChannelStrip::Values ChannelStrip::targets() const noexcept {
    Values v;
    v.hpf    = std::clamp(t_.hpf.load(kRelaxed), kHpfOffHz, 1000.0f);
    v.low    = std::clamp(t_.low.load(kRelaxed), -15.0f, 15.0f);
    v.mid    = std::clamp(t_.mid.load(kRelaxed), -15.0f, 15.0f);
    v.midHz  = std::clamp(t_.midHz.load(kRelaxed), 150.0f, 8000.0f);
    v.high   = std::clamp(t_.high.load(kRelaxed), -15.0f, 15.0f);
    v.thr    = std::clamp(t_.thr.load(kRelaxed), -60.0f, 0.0f);
    v.ratio  = std::clamp(t_.ratio.load(kRelaxed), 1.0f, 20.0f);
    v.att    = std::clamp(t_.att.load(kRelaxed), 0.1f, 100.0f);
    v.rel    = std::clamp(t_.rel.load(kRelaxed), 10.0f, 2000.0f);
    v.makeup = std::clamp(t_.makeup.load(kRelaxed), 0.0f, 24.0f);
    return v;
}

bool ChannelStrip::engaged() const noexcept {
    const Values t = targets();
    const Values& c = cur_;
    const bool eqOn   = c.hpf > kHpfOffHz || c.low != 0.0f || c.mid != 0.0f || c.high != 0.0f ||
                        t.hpf > kHpfOffHz || t.low != 0.0f || t.mid != 0.0f || t.high != 0.0f;
    const bool compOn = c.ratio > 1.0f || c.makeup != 0.0f || t.ratio > 1.0f || t.makeup != 0.0f || envDb_ < -1e-4f;
    return eqOn || compOn;
}

float ChannelStrip::gainComputerDb(float x, float T, float R) noexcept {
    if (R <= 1.0f) return 0.0f;
    const float slope = 1.0f / R - 1.0f;   // < 0
    const float over  = x - T;
    if (2.0f * over < -kKneeDb) return 0.0f;
    if (2.0f * std::fabs(over) <= kKneeDb) {
        const float a = over + kKneeDb * 0.5f;
        return slope * a * a / (2.0f * kKneeDb);
    }
    return slope * over;
}

void ChannelStrip::glide(const Values& to, std::uint32_t frames) noexcept {
    const float k = 1.0f - std::exp(-static_cast<float>(frames) / (kGlideMs * 1e-3f * static_cast<float>(fs_)));
    cur_.hpf    = approachLog(cur_.hpf, to.hpf, k);
    cur_.low    = approach(cur_.low, to.low, k);
    cur_.mid    = approach(cur_.mid, to.mid, k);
    cur_.midHz  = approachLog(cur_.midHz, to.midHz, k);
    cur_.high   = approach(cur_.high, to.high, k);
    cur_.thr    = approach(cur_.thr, to.thr, k);
    cur_.ratio  = approachLog(cur_.ratio, to.ratio, k);
    cur_.att    = to.att;   // times only shape the envelope: no audible step
    cur_.rel    = to.rel;
    cur_.makeup = approach(cur_.makeup, to.makeup, k);
    if (cur_.hpf < kHpfOffHz * 1.001f && to.hpf <= kHpfOffHz) cur_.hpf = kHpfOffHz;
    if (cur_.ratio < 1.001f && to.ratio <= 1.0f) cur_.ratio = 1.0f;
}

void ChannelStrip::updateCoefficients() noexcept {
    const double nyq = fs_ * 0.45;   // keep every design below Nyquist at low rates
    auto set = [](Biquad& q, const Coefs& c) { q.b0 = c.b0; q.b1 = c.b1; q.b2 = c.b2; q.a1 = c.a1; q.a2 = c.a2; };
    if (!coefValid_ || cur_.hpf != coefFor_.hpf) set(hpf_, highPass(fs_, std::min<double>(cur_.hpf, nyq)));
    if (!coefValid_ || cur_.low != coefFor_.low) set(low_, lowShelf(fs_, std::min<double>(kLowHz, nyq), cur_.low));
    if (!coefValid_ || cur_.mid != coefFor_.mid || cur_.midHz != coefFor_.midHz)
        set(mid_, peak(fs_, std::min<double>(cur_.midHz, nyq), kMidQ, cur_.mid));
    if (!coefValid_ || cur_.high != coefFor_.high) set(high_, highShelf(fs_, std::min<double>(kHighHz, nyq), cur_.high));
    coefFor_   = cur_;
    coefValid_ = true;
}

void ChannelStrip::run(Biquad& q, float* x, std::uint32_t n, int ch) noexcept {   // transposed direct form II
    double z1 = q.z1[ch], z2 = q.z2[ch];
    const double b0 = q.b0, b1 = q.b1, b2 = q.b2, a1 = q.a1, a2 = q.a2;
    for (std::uint32_t i = 0; i < n; ++i) {
        const double in = x[i];
        const double y  = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        x[i] = static_cast<float>(y);
    }
    // Flush denormals / tiny tails so silence costs nothing.
    q.z1[ch] = std::fabs(z1) < 1e-25 ? 0.0 : z1;
    q.z2[ch] = std::fabs(z2) < 1e-25 ? 0.0 : z2;
}

void ChannelStrip::process(float* L, float* R, std::uint32_t n) noexcept {
    if (n == 0) return;
    const Values t = targets();
    // Settings of a section that is doing nothing jump straight to their targets (inaudible),
    // so it starts from the right place when it comes in.
    if (cur_.ratio <= 1.0f && envDb_ == 0.0f) { cur_.thr = t.thr; cur_.att = t.att; cur_.rel = t.rel; }
    if (cur_.mid == 0.0f) cur_.midHz = t.midHz;
    const Values  from = cur_;
    glide(t, n);
    updateCoefficients();

    // ---- EQ: a band at neutral is skipped (its memory cleared so it re-enters cleanly).
    auto band = [&](Biquad& q, bool on) {
        if (!on) { q.clear(); return; }
        run(q, L, n, 0);
        if (R != L) run(q, R, n, 1);
    };
    band(hpf_, cur_.hpf > kHpfOffHz);
    band(low_, cur_.low != 0.0f);
    band(mid_, cur_.mid != 0.0f);
    band(high_, cur_.high != 0.0f);

    // ---- Compressor.
    const bool comp = cur_.ratio > 1.0f || from.ratio > 1.0f || envDb_ < -1e-4f;
    float deepest = 0.0f;
    if (comp || cur_.makeup != 0.0f || from.makeup != 0.0f) {
        const float fs = static_cast<float>(fs_);
        const float aA = std::exp(-1.0f / (cur_.att * 1e-3f * fs));
        const float aR = std::exp(-1.0f / (cur_.rel * 1e-3f * fs));
        const float T0 = from.thr, dT = (cur_.thr - from.thr) / static_cast<float>(n);
        const float R0 = from.ratio, dR = (cur_.ratio - from.ratio) / static_cast<float>(n);
        const float M0 = from.makeup, dM = (cur_.makeup - from.makeup) / static_cast<float>(n);
        constexpr float kDbPerLn = 8.685889638f;   // 20 / ln(10)
        float env = envDb_;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float fi = static_cast<float>(i);
            const float pk = std::max(std::fabs(L[i]), std::fabs(R[i]));
            float target = 0.0f;
            if (comp && pk > 1e-6f) target = gainComputerDb(kDbPerLn * std::log(pk), T0 + dT * fi, R0 + dR * fi);
            env = target < env ? aA * env + (1.0f - aA) * target : aR * env + (1.0f - aR) * target;
            deepest = std::min(deepest, env);
            const float g = std::exp((env + M0 + dM * fi) / kDbPerLn);
            L[i] *= g;
            if (R != L) R[i] *= g;
        }
        envDb_ = env > -1e-6f ? 0.0f : env;
    }
    grPub_.store(-deepest, kRelaxed);
}

} // namespace hc::dsp
