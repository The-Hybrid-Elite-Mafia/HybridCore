#include "hybrid/forge/Remake.hpp"

#include "hybrid/analysis/Fft.hpp"
#include "hybrid/forge/Composer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <numbers>
#include <numeric>
#include <vector>

namespace hc::forge {

namespace {

constexpr int    kOnN   = 2048;   // onset STFT
constexpr int    kOnHop = 512;
constexpr int    kPitchN = 8192;  // per-step pitch / chroma FFT
constexpr double kBpmLo = 70.0, kBpmHi = 160.0;

double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0.0f;
    const auto k = static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

std::vector<float> hann(int n) {
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n));
    return w;
}

// A mono signal, with access past the ends returning silence.
struct Mono {
    std::vector<float> x;
    [[nodiscard]] float at(std::ptrdiff_t i) const { return i >= 0 && static_cast<std::size_t>(i) < x.size() ? x[static_cast<std::size_t>(i)] : 0.0f; }
    [[nodiscard]] std::size_t size() const { return x.size(); }
};

Mono toMono(const float* a, const float* b, std::size_t n) {
    Mono m;
    m.x.resize(n);
    for (std::size_t i = 0; i < n; ++i) m.x[i] = b && b != a ? 0.5f * (a[i] + b[i]) : a[i];
    return m;
}

// Spectral-flux onset envelopes per band (frame rate = sr / kOnHop).
struct Onsets {
    double fr = 93.75;
    std::vector<float> full, kick, snareBody, snareNoise, hats, low, mids;
    [[nodiscard]] double frameTime(double f) const { return (f * kOnHop + kOnN * 0.5) / (fr * kOnHop); }
};

Onsets onsets(const Mono& m, double sr) {
    Onsets o;
    o.fr = sr / kOnHop;
    const analysis::Fft fft(kOnN);
    const auto w = hann(kOnN);
    std::vector<std::complex<float>> buf(kOnN);
    const std::size_t frames = m.size() > static_cast<std::size_t>(kOnN) ? (m.size() - kOnN) / kOnHop + 1 : 1;
    for (auto* v : {&o.full, &o.kick, &o.snareBody, &o.snareNoise, &o.hats, &o.low, &o.mids}) v->assign(frames, 0.0f);
    std::vector<float> prev(kOnN / 2 + 1, 0.0f), cur(kOnN / 2 + 1, 0.0f);
    const double binHz = sr / kOnN;
    auto band = [&](double lo, double hi) { return std::pair<int, int>{std::max(1, static_cast<int>(lo / binHz)), std::min(kOnN / 2, static_cast<int>(std::ceil(hi / binHz)))}; };
    const auto bK = band(35, 110), bSB = band(150, 350), bSN = band(1500, 5000), bH = band(7000, 16000), bL = band(35, 220), bM = band(300, 2500),
               bF = band(30, 16000);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t s = f * kOnHop;
        for (int i = 0; i < kOnN; ++i) buf[static_cast<std::size_t>(i)] = {m.at(static_cast<std::ptrdiff_t>(s) + i) * w[static_cast<std::size_t>(i)], 0.0f};
        fft.transform(buf.data());
        for (int k = 0; k <= kOnN / 2; ++k) cur[static_cast<std::size_t>(k)] = std::log1p(100.0f * std::abs(buf[static_cast<std::size_t>(k)]));
        auto flux = [&](std::pair<int, int> b) {
            float sum = 0.0f;
            for (int k = b.first; k <= b.second; ++k) sum += std::max(0.0f, cur[static_cast<std::size_t>(k)] - prev[static_cast<std::size_t>(k)]);
            return sum / static_cast<float>(b.second - b.first + 1);
        };
        if (f > 0) {
            o.full[f] = flux(bF);
            o.kick[f] = flux(bK);
            o.snareBody[f] = flux(bSB);
            o.snareNoise[f] = flux(bSN);
            o.hats[f] = flux(bH);
            o.low[f] = flux(bL);
            o.mids[f] = flux(bM);
        }
        std::swap(prev, cur);
    }
    return o;
}

float sampleEnv(const std::vector<float>& e, double frame) {
    if (e.empty() || frame < 0) return 0.0f;
    const auto i = static_cast<std::size_t>(frame);
    if (i + 1 >= e.size()) return i < e.size() ? e[i] : 0.0f;
    const float t = static_cast<float>(frame - static_cast<double>(i));
    return e[i] * (1.0f - t) + e[i + 1] * t;
}

// Peak of an envelope within +-half of a window (frames) around `frame`.
float peakNear(const std::vector<float>& e, double frame, double half) {
    float best = 0.0f;
    const auto a = static_cast<std::ptrdiff_t>(std::floor(frame - half)), b = static_cast<std::ptrdiff_t>(std::ceil(frame + half));
    for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(0, a); i <= b && static_cast<std::size_t>(i) < e.size(); ++i) best = std::max(best, e[static_cast<std::size_t>(i)]);
    return best;
}

struct TempoFit { double bpm = 120, strength = 0; };

// How well the onsets land on the beat grid of a tempo (beats count most, then 8ths, then
// 16ths), with the best phase; `phase` returns that phase in frames.
double gridFit(const std::vector<float>& full, const std::vector<float>& kick, double fr, double bpm, double& phase, double phaseStep) {
    const double beat = fr * 60.0 / bpm, step = beat / 4.0;
    const double n = static_cast<double>(full.size());
    double best = -1;
    for (double p = 0; p < beat; p += phaseStep) {
        double s = 0;
        int k = 0;
        for (double f = p; f < n; f += step, ++k) {
            const int pos = k & 3;
            const double w = pos == 0 ? 1.0 : pos == 2 ? 0.45 : 0.2;
            s += w * (sampleEnv(full, f) + 1.5 * sampleEnv(kick, f));
        }
        s /= std::max(1, k);
        if (s > best) { best = s; phase = p; }
    }
    return best;
}

TempoFit findTempo(const std::vector<float>& env, double fr) {
    TempoFit fit;
    const std::size_t n = env.size();
    if (n < 64) return fit;
    // Remove the slow trend so the autocorrelation sees beats, not loudness.
    std::vector<float> o(n);
    const int win = static_cast<int>(fr * 0.5);
    double run = 0;
    std::vector<double> pre(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + env[i];
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t a = i >= static_cast<std::size_t>(win) ? i - win : 0, b = std::min(n, i + win + 1);
        run = (pre[b] - pre[a]) / static_cast<double>(b - a);
        o[i] = std::max(0.0f, env[i] - static_cast<float>(run));
    }
    const int maxLag = static_cast<int>(fr * 60.0 / kBpmLo * 4.0) + 4;
    std::vector<double> R(static_cast<std::size_t>(maxLag + 1), 0.0);
    for (int L = 1; L <= maxLag && static_cast<std::size_t>(L) < n; ++L) {
        double s = 0;
        for (std::size_t i = static_cast<std::size_t>(L); i < n; ++i) s += static_cast<double>(o[i]) * o[i - L];
        R[static_cast<std::size_t>(L)] = s / static_cast<double>(n - L);
    }
    double r0 = 0;
    for (float v : o) r0 += static_cast<double>(v) * v;
    r0 /= static_cast<double>(n);
    auto Ri = [&](double lag) {
        if (lag < 1 || lag >= maxLag) return 0.0;
        const auto i = static_cast<std::size_t>(lag);
        const double t = lag - static_cast<double>(i);
        return R[i] * (1 - t) + R[i + 1] * t;
    };
    double best = -1;
    for (double bpm = kBpmLo; bpm <= kBpmHi; bpm += 0.05) {
        const double beat = fr * 60.0 / bpm;
        double s = Ri(beat) + Ri(2 * beat) + Ri(3 * beat) + Ri(4 * beat) + 0.5 * Ri(beat / 2) + 0.25 * Ri(beat / 4);
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 125.0) / 0.55, 2.0));
        s *= 0.75 + 0.25 * prior;
        if (s > best) { best = s; fit.bpm = bpm; }
    }
    fit.strength = r0 > 0 ? std::clamp(best / (4.5 * r0), 0.0, 1.0) : 0.0;
    return fit;
}

// Krumhansl–Kessler key profiles.
constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double pearson(const std::array<double, 12>& a, const std::array<double, 12>& b, int rot) {
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; ++i) { ma += a[static_cast<std::size_t>(i)]; mb += b[static_cast<std::size_t>(i)]; }
    ma /= 12; mb /= 12;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; ++i) {
        const double x = a[static_cast<std::size_t>((i + rot) % 12)] - ma, y = b[static_cast<std::size_t>(i)] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    return da > 0 && db > 0 ? num / std::sqrt(da * db) : 0.0;
}

const char* kNoteNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

int snapToScale(int midi, int key, const std::vector<int>& steps) {
    int best = midi, bestD = 99;
    for (int d = -2; d <= 2; ++d) {
        const int pc = ((midi + d - key) % 12 + 12) % 12;
        if (std::find(steps.begin(), steps.end(), pc) != steps.end() && std::abs(d) < bestD) { bestD = std::abs(d); best = midi + d; }
    }
    return best;
}

// Per-step spectrum features from one long FFT.
struct StepFeatures {
    std::array<double, 12> chroma{};
    double bassEnergy = 0, midEnergy = 0, total = 0;
    int bassMidi = -1;  float bassStrength = 0;
    int leadMidi = -1;  float leadStrength = 0;
};

class PitchProbe {
public:
    explicit PitchProbe(double sr) : sr_(sr), fft_(kPitchN), w_(hann(kPitchN)), buf_(kPitchN), mag_(kPitchN / 2 + 1) {}

    // Spectrum of the window starting at sample `start`.
    void at(const Mono& m, std::ptrdiff_t start) {
        for (int i = 0; i < kPitchN; ++i) buf_[static_cast<std::size_t>(i)] = {m.at(start + i) * w_[static_cast<std::size_t>(i)], 0.0f};
        fft_.transform(buf_.data());
        for (int k = 0; k <= kPitchN / 2; ++k) mag_[static_cast<std::size_t>(k)] = std::abs(buf_[static_cast<std::size_t>(k)]);
    }
    [[nodiscard]] float magAt(double hz) const {
        const double b = hz * kPitchN / sr_;
        if (b < 1 || b >= kPitchN / 2 - 1) return 0.0f;
        // Peak within +-1 bin (a note between bins still reads its level).
        const auto i = static_cast<std::size_t>(std::lround(b));
        return std::max({mag_[i - 1], mag_[i], mag_[i + 1]});
    }
    [[nodiscard]] double bandEnergy(double lo, double hi) const {
        const auto a = static_cast<std::size_t>(std::max(1.0, lo * kPitchN / sr_)), b = static_cast<std::size_t>(std::min<double>(kPitchN / 2, hi * kPitchN / sr_));
        double s = 0;
        for (std::size_t k = a; k <= b; ++k) s += static_cast<double>(mag_[k]) * mag_[k];
        return s;
    }
    void chroma(std::array<double, 12>& c, double lo, double hi) const {
        const auto a = static_cast<std::size_t>(std::max(1.0, lo * kPitchN / sr_)), b = static_cast<std::size_t>(std::min<double>(kPitchN / 2, hi * kPitchN / sr_));
        for (std::size_t k = a; k <= b; ++k) {
            const double hz = static_cast<double>(k) * sr_ / kPitchN;
            const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
            const int pc = ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12;
            c[static_cast<std::size_t>(pc)] += std::log1p(static_cast<double>(mag_[k]));
        }
    }
    // Strongest fundamental in a MIDI range by harmonic sum; returns strength.
    float pitch(int lo, int hi, int& midi, int harmonics) const {
        static constexpr float wts[6] = {1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.2f};
        float best = 0.0f;
        midi = -1;
        for (int mm = lo; mm <= hi; ++mm) {
            const double f = midiHz(mm);
            float s = 0.0f;
            for (int h = 0; h < harmonics; ++h) s += wts[h] * magAt(f * (h + 1));
            // Penalise a "fundamental" whose own partial is weak (octave-below mistakes).
            const float own = magAt(f);
            s *= 0.4f + 0.6f * std::min(1.0f, own / (magAt(f * 2) + 1e-9f));
            if (s > best) { best = s; midi = mm; }
        }
        return best;
    }

private:
    double sr_;
    analysis::Fft fft_;
    std::vector<float> w_;
    std::vector<std::complex<float>> buf_;
    std::vector<float> mag_;
};

struct BandHits {
    std::vector<float> v;      // strength per step
    float thr = 0;
    [[nodiscard]] bool hit(std::size_t j) const { return j < v.size() && v[j] > thr; }
};

BandHits sampleBand(const std::vector<float>& env, const Onsets& o, double t0, double stepSec, std::size_t steps, double halfFrames, float factor,
                    float floorFrac) {
    BandHits b;
    b.v.resize(steps);
    for (std::size_t j = 0; j < steps; ++j) {
        const double t = t0 + static_cast<double>(j) * stepSec;
        const double frame = (t * o.fr * kOnHop - kOnN * 0.5) / kOnHop;
        b.v[j] = peakNear(env, frame, halfFrames);
    }
    // Between the quiet floor and the hits: works whether the part plays on few steps or almost all.
    (void)factor;
    const float p10 = percentile(b.v, 0.10), p95 = percentile(b.v, 0.95);
    b.thr = p10 + std::max(0.30f, floorFrac) * (p95 - p10);
    if (b.thr <= 0.0f || p95 <= p10 * 1.05f) b.thr = 1e9f;
    return b;
}

std::string chordName(int root, int quality) {   // 0 major, 1 minor, 2 diminished
    return std::string(kNoteNames[root]) + (quality == 1 ? "m" : quality == 2 ? "dim" : "");
}

} // namespace

RemakeResult remake(const RemakeInput& in, const std::function<bool(float)>& progress) {
    RemakeResult r;
    auto report = [&](float f) { return !progress || progress(std::clamp(f, 0.0f, 1.0f)); };
    const double sr = in.sampleRate > 1000 ? in.sampleRate : 48000.0;

    // ---- sources: the mix, or the stems added together -------------------------------------
    Mono mix;
    if (in.mixL && in.mixFrames > 0) {
        mix = toMono(in.mixL, in.mixR, in.mixFrames);
    } else {
        std::size_t n = std::max({in.drumFrames, in.bassFrames, in.musicFrames});
        mix.x.assign(n, 0.0f);
        for (auto [p, len] : {std::pair{in.drums, in.drumFrames}, std::pair{in.bass, in.bassFrames}, std::pair{in.music, in.musicFrames}})
            if (p)
                for (std::size_t i = 0; i < len; ++i) mix.x[i] += p[i];
    }
    if (mix.size() < static_cast<std::size_t>(sr * 4)) { r.error = "the beat is shorter than 4 seconds"; return r; }
    {
        float peak = 0.0f;
        for (float v : mix.x) peak = std::max(peak, std::fabs(v));
        if (peak < 1e-4f) { r.error = "the file is silent"; return r; }   // below -80 dBFS everywhere
    }
    const Mono drums = in.drums ? toMono(in.drums, nullptr, in.drumFrames) : Mono{};
    const Mono bassStem = in.bass ? toMono(in.bass, nullptr, in.bassFrames) : Mono{};
    const Mono music = in.music ? toMono(in.music, nullptr, in.musicFrames) : Mono{};
    const Mono& drumSrc = drums.size() ? drums : mix;
    const Mono& bassSrc = bassStem.size() ? bassStem : mix;
    const Mono& musicSrc = music.size() ? music : mix;

    // ---- onsets, tempo, grid ----------------------------------------------------------------
    const Onsets od = onsets(drumSrc, sr);
    if (!report(0.15f)) { r.error = "cancelled"; return r; }
    const Onsets om = (&musicSrc == &drumSrc) ? od : onsets(musicSrc, sr);
    const Onsets ob = (&bassSrc == &drumSrc) ? od : (&bassSrc == &musicSrc ? om : onsets(bassSrc, sr));
    if (!report(0.3f)) { r.error = "cancelled"; return r; }

    // Tempo: the autocorrelation's pick and its usual mistakes (half, double, 3/4, 4/3, 2/3, 3/2)
    // are all tried against the whole song's onsets on a beat / 8th / 16th grid.
    const TempoFit tf = findTempo(od.full, od.fr);
    r.tempoConfidence = tf.strength;
    double bpm = tf.bpm, bestFit = -1, phaseTmp = 0;
    for (double mul : {1.0, 2.0, 0.5, 0.75, 4.0 / 3.0, 2.0 / 3.0, 1.5}) {
        const double cand = tf.bpm * mul;
        if (cand < kBpmLo || cand > kBpmHi) continue;
        const double prior = std::exp(-0.5 * std::pow(std::log2(cand / 125.0) / 0.6, 2.0));
        const double fit = gridFit(od.full, od.kick, od.fr, cand, phaseTmp, 1.0) * (0.8 + 0.2 * prior);
        if (fit > bestFit) { bestFit = fit; bpm = cand; }
    }
    // Half or double? Hats and other fast parts tell: at a tempo that is too slow they land
    // between that tempo's 16ths (on its 32nds).
    {
        auto offRatio = [&](double b) {
            double ph = 0;
            gridFit(od.full, od.kick, od.fr, b, ph, 0.5);
            const double step = od.fr * 60.0 / b / 4.0;
            double on = 0, off = 0;
            for (double f = ph; f < static_cast<double>(od.hats.size()); f += step) {
                on += sampleEnv(od.hats, f) + 0.5 * sampleEnv(od.snareNoise, f);
                off += sampleEnv(od.hats, f + step * 0.5) + 0.5 * sampleEnv(od.snareNoise, f + step * 0.5);
            }
            return on > 0 ? off / on : 0.0;
        };
        const double slow = bpm * 2.0 <= kBpmHi ? bpm : bpm / 2.0, fast = slow * 2.0;
        if (slow >= kBpmLo && fast <= kBpmHi) {
            const double ratio = offRatio(slow);
            if (ratio > 0.5) bpm = fast;
            else if (ratio < 0.35) bpm = slow;
        }
    }
    // Fine tempo: the beat must stay on the grid for the whole song (0.01 BPM steps).
    {
        double fine = bpm, fineFit = -1;
        for (double b = bpm - 0.6; b <= bpm + 0.6; b += 0.02) {
            const double fit = gridFit(od.full, od.kick, od.fr, b, phaseTmp, 1.0);
            if (fit > fineFit) { fineFit = fit; fine = b; }
        }
        for (double b = fine - 0.03; b <= fine + 0.03; b += 0.005) {
            const double fit = gridFit(od.full, od.kick, od.fr, b, phaseTmp, 0.5);
            if (fit > fineFit) { fineFit = fit; fine = b; }
        }
        // Songs are almost always made at a whole-number tempo.
        const double whole = std::round(fine);
        if (std::fabs(whole - fine) < 0.2 && gridFit(od.full, od.kick, od.fr, whole, phaseTmp, 0.5) >= 0.985 * fineFit) fine = whole;
        bpm = fine;
    }
    r.bpm = std::round(bpm * 100.0) / 100.0;
    const double beatSec = 60.0 / r.bpm, stepSec = beatSec / 4.0, barSec = beatSec * 4.0;
    const double beatFrames = beatSec * od.fr;

    // Beat phase: where the onsets (kicks weighted up) line up with the beat grid.
    double bestPhase = 0;
    gridFit(od.full, od.kick, od.fr, r.bpm, bestPhase, 0.25);
    // Downbeat: which of the 4 beats carries the most kick at bar starts.
    int bestBeat = 0;
    double bestBar = -1;
    for (int b = 0; b < 4; ++b) {
        double s = 0;
        for (double f = bestPhase + b * beatFrames; f < static_cast<double>(od.kick.size()); f += 4 * beatFrames) s += sampleEnv(od.kick, f);
        if (s > bestBar) { bestBar = s; bestBeat = b; }
    }
    double t0 = od.frameTime(bestPhase + bestBeat * beatFrames);
    t0 = std::fmod(t0, barSec);
    if (t0 < 0) t0 += barSec;
    r.firstDownbeat = t0;
    const double dur = static_cast<double>(mix.size()) / sr;
    const int songBars = std::clamp(static_cast<int>((dur - t0) / barSec), 1, kMaxSongBars);
    const std::size_t steps = static_cast<std::size_t>(songBars) * kStepsPerBar;
    const double halfStepFrames = stepSec * od.fr * 0.45;

    // ---- drums on the step grid (hats also on the 32nd grid) -------------------------------
    BandHits kickH = sampleBand(od.kick, od, t0, stepSec, steps, halfStepFrames, 2.5f, 0.30f);
    BandHits bodyH = sampleBand(od.snareBody, od, t0, stepSec, steps, halfStepFrames, 1.0f, 0.0f);
    BandHits noiseH = sampleBand(od.snareNoise, od, t0, stepSec, steps, halfStepFrames, 1.0f, 0.0f);
    BandHits snareH;
    snareH.v.resize(steps);
    {
        const float nRef = std::max(1e-9f, percentile(noiseH.v, 0.95)), bRef = std::max(1e-9f, percentile(bodyH.v, 0.95));
        for (std::size_t j = 0; j < steps; ++j) snareH.v[j] = noiseH.v[j] / nRef + 0.5f * bodyH.v[j] / bRef;
        const float p10 = percentile(snareH.v, 0.10), p95 = percentile(snareH.v, 0.95);
        snareH.thr = p10 + 0.45f * (p95 - p10);
    }
    BandHits hatH = sampleBand(od.hats, od, t0, stepSec / 2.0, steps * 2, halfStepFrames / 2.0, 2.0f, 0.25f);
    // A kick's click also lights the snare band: a snare on a kick step must stand out more.
    for (std::size_t j = 0; j < steps; ++j)
        if (kickH.hit(j) && snareH.v[j] < snareH.thr * 1.6f) snareH.v[j] = 0.0f;
    if (!report(0.45f)) { r.error = "cancelled"; return r; }

    // ---- per-step pitch / chroma -------------------------------------------------------------
    PitchProbe bassProbe(sr), musicProbe(sr);
    std::vector<StepFeatures> feat(steps);
    const auto offs = static_cast<std::ptrdiff_t>(sr * 0.04);
    for (std::size_t j = 0; j < steps; ++j) {
        const auto start = static_cast<std::ptrdiff_t>((t0 + static_cast<double>(j) * stepSec) * sr) + offs;
        StepFeatures& F = feat[j];
        bassProbe.at(bassSrc, start);
        F.bassEnergy = bassProbe.bandEnergy(35, 220);
        F.bassStrength = bassProbe.pitch(26, 55, F.bassMidi, 4);
        if (&musicSrc != &bassSrc) musicProbe.at(musicSrc, start);
        const PitchProbe& mp = (&musicSrc != &bassSrc) ? musicProbe : bassProbe;
        mp.chroma(F.chroma, 110, 2000);
        F.midEnergy = mp.bandEnergy(250, 3000);
        F.leadStrength = mp.pitch(60, 88, F.leadMidi, 3);
        if ((j & 63) == 0 && !report(0.45f + 0.4f * static_cast<float>(j) / static_cast<float>(steps))) { r.error = "cancelled"; return r; }
    }

    // ---- key ---------------------------------------------------------------------------------
    std::array<double, 12> total{};
    for (const auto& F : feat)
        for (int i = 0; i < 12; ++i) total[static_cast<std::size_t>(i)] += F.chroma[static_cast<std::size_t>(i)];
    // The bass on the first beat of each bar usually sits on the home note: it breaks the tie
    // between a key and its relative major / minor.
    std::array<double, 12> bassHome{};
    double bassSum = 0;
    for (std::size_t j = 0; j < steps; ++j) {
        const auto& F = feat[j];
        if (F.bassMidi < 0) continue;
        const double w = F.bassEnergy * (j % kStepsPerBar == 0 ? 3.0 : 1.0);   // bar starts count more
        bassHome[static_cast<std::size_t>(F.bassMidi % 12)] += w;
        bassSum += w;
    }
    double bestKey = -2;
    for (int k = 0; k < 12; ++k) {
        const double home = bassSum > 0 ? bassHome[static_cast<std::size_t>(k)] / bassSum : 0.0;
        const double maj = pearson(total, kMajorProfile, k), mnr = pearson(total, kMinorProfile, k);
        const double sMaj = maj + 0.5 * home, sMin = mnr + 0.5 * home;
        if (sMaj > bestKey) { bestKey = sMaj; r.key = k; r.scale = 0; r.keyConfidence = maj; }
        if (sMin > bestKey) { bestKey = sMin; r.key = k; r.scale = 1; r.keyConfidence = mnr; }
    }
    r.keyConfidence = std::clamp(r.keyConfidence, 0.0, 1.0);
    const std::vector<int>& scaleSteps = Composer::scaleSteps(r.scale);

    // ---- chords per bar (diatonic triads of the key) ----------------------------------------
    std::vector<int> barRoot(static_cast<std::size_t>(songBars)), barQual(static_cast<std::size_t>(songBars));
    std::vector<double> barMid(static_cast<std::size_t>(songBars), 0.0), barBass(static_cast<std::size_t>(songBars), 0.0);
    auto triadOf = [&](int degree, int& root, int& quality) {
        const std::vector<int> major{0, 2, 4, 5, 7, 9, 11}, minor{0, 2, 3, 5, 7, 8, 10};
        const auto& sc = r.scale == 0 ? major : minor;
        const int a = sc[static_cast<std::size_t>(degree % 7)], b = sc[static_cast<std::size_t>((degree + 2) % 7)] + ((degree + 2) >= 7 ? 12 : 0),
                  c = sc[static_cast<std::size_t>((degree + 4) % 7)] + ((degree + 4) >= 7 ? 12 : 0);
        root = (r.key + a) % 12;
        const int third = b - a, fifth = c - a;
        quality = third == 4 ? 0 : (fifth == 6 ? 2 : 1);
    };
    for (int bar = 0; bar < songBars; ++bar) {
        std::array<double, 12> c{};
        for (int s = 0; s < kStepsPerBar; ++s) {
            const auto& F = feat[static_cast<std::size_t>(bar * kStepsPerBar + s)];
            for (int i = 0; i < 12; ++i) c[static_cast<std::size_t>(i)] += F.chroma[static_cast<std::size_t>(i)];
            barMid[static_cast<std::size_t>(bar)] += F.midEnergy;
            barBass[static_cast<std::size_t>(bar)] += F.bassEnergy;
        }
        double best = -1e9;
        for (int d = 0; d < 7; ++d) {
            int root = 0, q = 0;
            triadOf(d, root, q);
            const int third = (root + (q == 0 ? 4 : 3)) % 12, fifth = (root + (q == 2 ? 6 : 7)) % 12;
            double mean = 0;
            for (double v : c) mean += v;
            mean /= 12;
            const double s = 1.0 * (c[static_cast<std::size_t>(root)] - mean) + 0.8 * (c[static_cast<std::size_t>(third)] - mean) + 0.7 * (c[static_cast<std::size_t>(fifth)] - mean);
            if (s > best) { best = s; barRoot[static_cast<std::size_t>(bar)] = root; barQual[static_cast<std::size_t>(bar)] = q; }
        }
    }

    // ---- which parts play in each bar ----------------------------------------------------------
    std::vector<std::array<bool, kTracks>> active(static_cast<std::size_t>(songBars));
    const double midRef = percentile(std::vector<float>(barMid.begin(), barMid.end()), 0.8), bassRef = percentile(std::vector<float>(barBass.begin(), barBass.end()), 0.8);
    std::vector<int> bassStep(steps, -1);   // MIDI note sounding (start) at each step, -1 none
    const float bassThr = static_cast<float>(bassRef / kStepsPerBar * 0.08);
    for (std::size_t j = 0; j < steps; ++j) {
        const auto& F = feat[j];
        if (F.bassEnergy > bassThr && F.bassMidi > 0) bassStep[j] = snapToScale(F.bassMidi, r.key, scaleSteps);
    }
    for (int bar = 0; bar < songBars; ++bar) {
        auto& a = active[static_cast<std::size_t>(bar)];
        int k = 0, s = 0, h = 0, b = 0;
        for (int st = 0; st < kStepsPerBar; ++st) {
            const auto j = static_cast<std::size_t>(bar * kStepsPerBar + st);
            k += kickH.hit(j);
            s += snareH.hit(j);
            h += hatH.hit(2 * j) + hatH.hit(2 * j + 1);
            b += bassStep[j] >= 0;
        }
        a[kick] = k > 0;
        a[snare] = s > 0;
        a[hats] = h >= 3;
        a[perc] = false;
        a[bass] = b >= 3;
        const bool music2 = barMid[static_cast<std::size_t>(bar)] > midRef * 0.15;
        a[keys] = music2;
        a[pad] = music2;
        a[lead] = false;   // set below from the melody
    }

    // ---- the repeating cycle: 1, 2, 4 or 8 bars, from the chords ----------------------------
    int cycle = 4;
    {
        double bestAgree = -1;
        for (int P : {1, 2, 4, 8}) {
            int same = 0, n = 0;
            for (int bar = P; bar < songBars; ++bar) {
                if (!active[static_cast<std::size_t>(bar)][pad]) continue;
                ++n;
                same += barRoot[static_cast<std::size_t>(bar)] == barRoot[static_cast<std::size_t>(bar - P)];
            }
            const double agree = n > 0 ? static_cast<double>(same) / n : 0.0;
            if (agree >= 0.8) { cycle = P; break; }
            if (agree > bestAgree + 0.05) { bestAgree = agree; cycle = P; }
        }
        cycle = std::clamp(cycle, 1, kMaxBars);
        if (cycle < 2) cycle = 2;   // a 2-bar minimum keeps room for drum variation
    }
    const int cycleSteps = cycle * kStepsPerBar;

    // ---- majority groove over every cycle where the part plays --------------------------------
    Pattern& pat = r.song.pattern;
    pat = Pattern{};
    pat.bars = cycle;
    auto vote = [&](int track, int sub, const BandHits& h, int pitch) {   // sub = sub-steps per 16th
        const int slots = cycleSteps * sub;
        std::vector<int> hits(static_cast<std::size_t>(slots), 0), seen(static_cast<std::size_t>(slots), 0);
        std::vector<float> vel(static_cast<std::size_t>(slots), 0.0f);
        const float ref = std::max(1e-9f, percentile(h.v, 0.9));
        for (int bar = 0; bar < songBars; ++bar) {
            if (!active[static_cast<std::size_t>(bar)][static_cast<std::size_t>(track)]) continue;
            for (int st = 0; st < kStepsPerBar * sub; ++st) {
                const auto j = static_cast<std::size_t>(bar * kStepsPerBar * sub + st);
                const int slot = ((bar % cycle) * kStepsPerBar * sub + st);
                ++seen[static_cast<std::size_t>(slot)];
                if (h.hit(j)) { ++hits[static_cast<std::size_t>(slot)]; vel[static_cast<std::size_t>(slot)] += h.v[j] / ref; }
            }
        }
        for (int s2 = 0; s2 < slots; ++s2) {
            const int n = seen[static_cast<std::size_t>(s2)], k = hits[static_cast<std::size_t>(s2)];
            if (n == 0 || static_cast<double>(k) / n < 0.45) continue;
            Note note;
            note.start = static_cast<float>(s2) / static_cast<float>(sub);
            note.length = 1.0f / static_cast<float>(sub);
            note.pitch = static_cast<std::uint8_t>(pitch);
            note.velocity = static_cast<std::uint8_t>(std::clamp(60.0f + 55.0f * vel[static_cast<std::size_t>(s2)] / static_cast<float>(k), 50.0f, 127.0f));
            pat.tracks[static_cast<std::size_t>(track)].add(note);
        }
    };
    vote(kick, 1, kickH, drum::kick);
    vote(snare, 1, snareH, drum::snare);
    vote(hats, 2, hatH, drum::hatClosed);

    // Bass line: starts where the bass begins or changes note; the most common note per slot.
    {
        std::vector<std::map<int, int>> votes(static_cast<std::size_t>(cycleSteps));
        std::vector<int> seen(static_cast<std::size_t>(cycleSteps), 0);
        std::vector<std::vector<int>> lens(static_cast<std::size_t>(cycleSteps));
        BandHits lowOn = sampleBand(ob.low, ob, t0, stepSec, steps, halfStepFrames, 2.0f, 0.2f);
        for (int bar = 0; bar < songBars; ++bar) {
            if (!active[static_cast<std::size_t>(bar)][bass]) continue;
            for (int st = 0; st < kStepsPerBar; ++st) {
                const auto j = static_cast<std::size_t>(bar * kStepsPerBar + st);
                const int slot = (bar % cycle) * kStepsPerBar + st;
                ++seen[static_cast<std::size_t>(slot)];
                const int p = bassStep[j];
                if (p < 0) continue;
                const bool starts = j == 0 || bassStep[j - 1] != p || lowOn.hit(j);
                if (!starts) continue;
                int len = 1;
                while (j + static_cast<std::size_t>(len) < steps && len < 16 && bassStep[j + static_cast<std::size_t>(len)] == p && !lowOn.hit(j + static_cast<std::size_t>(len))) ++len;
                ++votes[static_cast<std::size_t>(slot)][p];
                lens[static_cast<std::size_t>(slot)].push_back(len);
            }
        }
        for (int s2 = 0; s2 < cycleSteps; ++s2) {
            int total2 = 0, bestN = 0, bestP = -1;
            for (auto [p, n] : votes[static_cast<std::size_t>(s2)]) { total2 += n; if (n > bestN) { bestN = n; bestP = p; } }
            const int n = seen[static_cast<std::size_t>(s2)];
            if (n == 0 || static_cast<double>(total2) / n < 0.45 || bestP < 0) continue;
            auto& L = lens[static_cast<std::size_t>(s2)];
            std::nth_element(L.begin(), L.begin() + static_cast<std::ptrdiff_t>(L.size() / 2), L.end());
            Note note;
            note.start = static_cast<float>(s2);
            note.length = static_cast<float>(std::max(1, L[L.size() / 2]));
            note.pitch = static_cast<std::uint8_t>(std::clamp(bestP, 24, 60));
            note.velocity = 105;
            pat.tracks[bass].add(note);
        }
        // Notes must not overlap the next one.
        auto& T = pat.tracks[bass];
        T.sort();
        for (int i = 0; i + 1 < T.count; ++i)
            T.notes[static_cast<std::size_t>(i)].length = std::min(T.notes[static_cast<std::size_t>(i)].length, T.notes[static_cast<std::size_t>(i + 1)].start - T.notes[static_cast<std::size_t>(i)].start);
    }

    // Chords of the cycle: the most common chord at each bar of the cycle.
    std::vector<int> cycRoot(static_cast<std::size_t>(cycle), r.key), cycQual(static_cast<std::size_t>(cycle), r.scale == 0 ? 0 : 1);
    for (int b = 0; b < cycle; ++b) {
        std::map<int, int> votes;
        for (int bar = b; bar < songBars; bar += cycle)
            if (active[static_cast<std::size_t>(bar)][pad]) ++votes[barRoot[static_cast<std::size_t>(bar)] * 4 + barQual[static_cast<std::size_t>(bar)]];
        int bestN = 0;
        for (auto [code, n] : votes) if (n > bestN) { bestN = n; cycRoot[static_cast<std::size_t>(b)] = code / 4; cycQual[static_cast<std::size_t>(b)] = code % 4; }
    }
    // Pad: the chord held for the bar. Keys: the chord where the mids hit (majority), else beats 1 and 3.
    BandHits midOn = sampleBand(om.mids, om, t0, stepSec, steps, halfStepFrames, 2.5f, 0.3f);
    for (int b = 0; b < cycle; ++b) {
        const int root = cycRoot[static_cast<std::size_t>(b)], q = cycQual[static_cast<std::size_t>(b)];
        const int third = q == 0 ? 4 : 3, fifth = q == 2 ? 6 : 7;
        int base = 48 + root;   // C3..B3
        if (base < 52) base += 12;
        for (int iv : {0, third, fifth, 12}) {
            Note n;
            n.start = static_cast<float>(b * kStepsPerBar);
            n.length = static_cast<float>(kStepsPerBar);
            n.pitch = static_cast<std::uint8_t>(base + iv);
            n.velocity = 80;
            pat.tracks[pad].add(n);
        }
        std::vector<int> slots;
        for (int st = 0; st < kStepsPerBar; ++st) {
            int seen2 = 0, hit2 = 0;
            for (int bar = b; bar < songBars; bar += cycle) {
                if (!active[static_cast<std::size_t>(bar)][keys]) continue;
                ++seen2;
                const auto j = static_cast<std::size_t>(bar * kStepsPerBar + st);
                hit2 += midOn.hit(j) && !kickH.hit(j) && !snareH.hit(j);
            }
            if (seen2 > 0 && static_cast<double>(hit2) / seen2 >= 0.45) slots.push_back(st);
        }
        if (slots.empty()) slots = {0, 8};
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const int st = slots[i], next = i + 1 < slots.size() ? slots[i + 1] : kStepsPerBar;
            for (int iv : {0, third, fifth}) {
                Note n;
                n.start = static_cast<float>(b * kStepsPerBar + st);
                n.length = static_cast<float>(std::max(1, std::min(next - st, 8)));
                n.pitch = static_cast<std::uint8_t>(base + 12 + iv);
                n.velocity = 90;
                pat.tracks[keys].add(n);
            }
        }
    }

    // Lead melody (rough): a clear high pitch where the mids have an onset, most common per slot.
    {
        std::vector<float> strengths;
        for (const auto& F : feat) strengths.push_back(F.leadStrength);
        const float leadThr = percentile(strengths, 0.6);
        std::vector<std::map<int, int>> votes(static_cast<std::size_t>(cycleSteps));
        std::vector<int> seen(static_cast<std::size_t>(cycleSteps), 0);
        std::vector<int> leadBars(static_cast<std::size_t>(songBars), 0);
        for (int bar = 0; bar < songBars; ++bar) {
            if (!active[static_cast<std::size_t>(bar)][keys]) continue;
            for (int st = 0; st < kStepsPerBar; ++st) {
                const auto j = static_cast<std::size_t>(bar * kStepsPerBar + st);
                const int slot = (bar % cycle) * kStepsPerBar + st;
                ++seen[static_cast<std::size_t>(slot)];
                const auto& F = feat[j];
                if (F.leadMidi < 0 || F.leadStrength < leadThr || !midOn.hit(j)) continue;
                const int pc = ((F.leadMidi - r.key) % 12 + 12) % 12;
                if (std::find(scaleSteps.begin(), scaleSteps.end(), pc) == scaleSteps.end()) continue;
                ++votes[static_cast<std::size_t>(slot)][F.leadMidi];
                ++leadBars[static_cast<std::size_t>(bar)];
            }
        }
        for (int s2 = 0; s2 < cycleSteps; ++s2) {
            int bestN = 0, bestP = -1;
            for (auto [p, n] : votes[static_cast<std::size_t>(s2)]) if (n > bestN) { bestN = n; bestP = p; }
            const int n = seen[static_cast<std::size_t>(s2)];
            if (n == 0 || bestP < 0 || static_cast<double>(bestN) / n < 0.35) continue;
            Note note;
            note.start = static_cast<float>(s2);
            note.length = 2.0f;
            note.pitch = static_cast<std::uint8_t>(bestP);
            note.velocity = 100;
            pat.tracks[lead].add(note);
        }
        auto& T = pat.tracks[lead];
        T.sort();
        for (int i = 0; i + 1 < T.count; ++i)
            T.notes[static_cast<std::size_t>(i)].length = std::min(T.notes[static_cast<std::size_t>(i)].length, T.notes[static_cast<std::size_t>(i + 1)].start - T.notes[static_cast<std::size_t>(i)].start);
        const bool hasLead = T.count > 0;
        for (int bar = 0; bar < songBars; ++bar) active[static_cast<std::size_t>(bar)][lead] = hasLead && leadBars[static_cast<std::size_t>(bar)] > 0;
    }
    for (auto& t : pat.tracks) t.sort();
    if (!report(0.9f)) { r.error = "cancelled"; return r; }

    // ---- song form: masks per bar, sections by energy -----------------------------------------
    Arrangement& arr = r.song.arrangement;
    arr = Arrangement{};
    arr.bars = songBars;
    std::vector<float> barDb(static_cast<std::size_t>(songBars));
    for (int bar = 0; bar < songBars; ++bar) {
        double e = 0;
        const auto a = static_cast<std::size_t>((t0 + bar * barSec) * sr), b = std::min(mix.size(), static_cast<std::size_t>((t0 + (bar + 1) * barSec) * sr));
        for (std::size_t i = a; i < b; ++i) e += static_cast<double>(mix.x[i]) * mix.x[i];
        barDb[static_cast<std::size_t>(bar)] = static_cast<float>(10.0 * std::log10(e / std::max<std::size_t>(1, b - a) + 1e-12));
        std::uint8_t m = 0;
        for (int t = 0; t < kTracks; ++t)
            if (active[static_cast<std::size_t>(bar)][static_cast<std::size_t>(t)] && pat.tracks[static_cast<std::size_t>(t)].count > 0) m |= static_cast<std::uint8_t>(1u << t);
        arr.mask[static_cast<std::size_t>(bar)] = m;
    }
    int firstKick = songBars, lastKick = -1;
    for (int bar = 0; bar < songBars; ++bar)
        if (active[static_cast<std::size_t>(bar)][kick]) { firstKick = std::min(firstKick, bar); lastKick = bar; }
    const float hookDb = percentile(barDb, 0.7) - 1.0f;
    for (int bar = 0; bar < songBars; ++bar) {
        Section s = kVerse;
        if (bar < firstKick) s = kIntro;
        else if (bar > lastKick) s = kOutro;
        else if (barDb[static_cast<std::size_t>(bar)] >= hookDb) s = kHook;
        arr.section[static_cast<std::size_t>(bar)] = s;
    }
    // Merge runs shorter than 4 bars into the section before them.
    for (int bar = 1; bar < songBars; ++bar) {
        int run = 1;
        while (bar + run < songBars && arr.section[static_cast<std::size_t>(bar + run)] == arr.section[static_cast<std::size_t>(bar)]) ++run;
        if (run < 4 && arr.section[static_cast<std::size_t>(bar)] != arr.section[static_cast<std::size_t>(bar - 1)] &&
            arr.section[static_cast<std::size_t>(bar)] != kIntro && arr.section[static_cast<std::size_t>(bar)] != kOutro)
            for (int i = 0; i < run; ++i) arr.section[static_cast<std::size_t>(bar + i)] = arr.section[static_cast<std::size_t>(bar - 1)];
        bar += run - 1;
    }

    // ---- names --------------------------------------------------------------------------------
    for (int b = 0; b < cycle; ++b) r.chords += (b ? " | " : "") + chordName(cycRoot[static_cast<std::size_t>(b)], cycQual[static_cast<std::size_t>(b)]);
    char buf[256];
    std::snprintf(buf, sizeof buf, "REMAKE  \xC2\xB7  %.1f BPM  \xC2\xB7  %s %s  \xC2\xB7  %s  \xC2\xB7  %d-bar groove, %d bars", r.bpm, kNoteNames[r.key],
                  r.scale == 0 ? "major" : "minor", r.chords.c_str(), cycle, songBars);
    r.summary = buf;
    r.ok = !pat.isEmpty();
    if (!r.ok) r.error = "no notes found (is the file silent?)";
    report(1.0f);
    return r;
}

} // namespace hc::forge
