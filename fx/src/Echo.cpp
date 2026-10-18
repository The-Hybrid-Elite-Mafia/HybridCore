// HEM ECHO — stereo / ping-pong delay with tempo sync, ducking and tape wobble.
//   Time: free (ms) or a note value at the song tempo (the host's, or Beat Forge's).
//   A new time is approached tape-style (1/2000 of the distance per sample), never jumps.
//   Feedback path: low cut + high cut, then a soft curve, so even high feedback stays
//   musical and can't run away. PING-PONG crosses the repeats between the sides.
//   DUCK: the echoes step back while the dry signal is loud (250 ms recovery).
//   WOBBLE: a slow tape-style pitch drift on the repeats (0.8 Hz, up to ±2 ms).
//   Ported from the Rework Playground's Elite Delay and improved: WOBBLE.
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"

namespace hc::fx {
namespace {

enum P { Time, Sync, Note, Feedback, LowCut, HighCut, PingPong, Wobble, Duck, Mix, kCount };
const char* const kOnOff[] = {"OFF", "ON"};
const char* const kNotes[] = {"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"};
constexpr double kBeats[] = {0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0};
const ParamInfo kParams[kCount] = {
    {"time",     "Time",      "ms", 1.0f, 2000.0f, 375.0f, Taper::Log},
    {"sync",     "Tempo Sync", "", 0.0f, 1.0f, 1.0f, Taper::Toggle, kOnOff},
    {"note",     "Note",      "", 0.0f, 11.0f, 8.0f, Taper::Choice, kNotes},
    {"feedback", "Feedback",  "%", 0.0f, 100.0f, 30.0f},
    {"lowcut",   "Low Cut",   "Hz", 20.0f, 2000.0f, 150.0f, Taper::Log},
    {"highcut",  "High Cut",  "Hz", 1000.0f, 20000.0f, 8000.0f, Taper::Log},
    {"pingpong", "Ping-Pong", "%", 0.0f, 100.0f, 0.0f},
    {"wobble",   "Wobble",    "%", 0.0f, 100.0f, 0.0f},
    {"duck",     "Duck",      "%", 0.0f, 100.0f, 0.0f},
    {"mix",      "Mix",       "%", 0.0f, 100.0f, 25.0f},
};
const MeterInfo kMeters[] = {{"ECHO", "ms", 0.0f, 2000.0f}};
constexpr double kMaxSeconds = 4.0, kQ = 0.7071067811865476;
constexpr float kWobbleMs = 2.0f;

class Echo final : public Effect {
public:
    Echo() { setDefaults(); }
    std::span<const ParamInfo> params() const noexcept override { return kParams; }
    std::span<const MeterInfo> meters() const noexcept override { return kMeters; }
    bool paramActive(int i) const noexcept override {   // TIME counts with sync off, NOTE with sync on
        const bool sync = p(Sync) >= 0.5f;
        return i == Time ? !sync : i == Note ? sync : true;
    }

    void prepare(double fs, std::uint32_t) override {
        fs_ = fs;
        const auto size = static_cast<std::uint32_t>(std::ceil((kMaxSeconds + 0.01) * fs)) + 4;
        line_[0].prepare(size); line_[1].prepare(size);
        duckRel_ = onePoleCoef(fs, 250.0);
        lastLow_ = lastHigh_ = -1.0f;
        reset();
    }
    void reset() noexcept override {
        line_[0].clear(); line_[1].clear();
        hp_.clear(); lp_.clear();
        duckEnv_ = 0.0f;
        current_ = -1.0f;
        wobPh_ = 0.0;
        fb_.prepare(fs_, 50, p(Feedback) * 0.0095f);
        mix_.prepare(fs_, 30, p(Mix) * 0.01f);
        ping_.prepare(fs_, 50, p(PingPong) * 0.01f);
        wob_.prepare(fs_, 50, p(Wobble) * 0.01f);
        publishMeter(0, 0.0f);
    }

    void process(float* L, float* R, std::uint32_t n) noexcept override {
        const bool sync = p(Sync) >= 0.5f;
        const double seconds = sync ? kBeats[std::clamp(static_cast<int>(p(Note)), 0, 11)] * 60.0 / std::clamp(static_cast<double>(tempo()), 30.0, 300.0)
                                    : p(Time) * 0.001;
        const float target = static_cast<float>(std::clamp(seconds * fs_, 1.0, kMaxSeconds * fs_));
        if (current_ < 0.0f) current_ = target;
        const float lo = p(LowCut), hi = p(HighCut);
        if (lo != lastLow_ || hi != lastHigh_) {
            hp_.set(BiquadCoefs::make(BiquadCoefs::Type::HighPass, fs_, lo, kQ));
            lp_.set(BiquadCoefs::make(BiquadCoefs::Type::LowPass, fs_, hi, kQ));
            lastLow_ = lo; lastHigh_ = hi;
        }
        const float fbT = p(Feedback) * 0.0095f, mT = p(Mix) * 0.01f, pT = p(PingPong) * 0.01f, wT = p(Wobble) * 0.01f;
        const float duck = p(Duck) * 0.01f;
        const float wobDepth = kWobbleMs * 0.001f * static_cast<float>(fs_);
        for (std::uint32_t i = 0; i < n; ++i) {
            current_ += (target - current_) * 0.0005f;
            const float wob = wob_.next(wT) * wobDepth * static_cast<float>(std::sin(2.0 * std::numbers::pi * wobPh_));
            wobPh_ += 0.8 / fs_; if (wobPh_ >= 1.0) wobPh_ -= 1.0;
            const float d = std::max(1.0f, current_ + wob);
            // read before write: the head sits `d` samples behind the next write
            float wl = line_[0].read(d - 1.0f), wr = line_[1].read(d - 1.0f);
            float fl = std::tanh(lp_.l.tick(hp_.l.tick(wl)));
            float fr = std::tanh(lp_.r.tick(hp_.r.tick(wr)));
            const float xl = L[i], xr = R[i];
            const float pp = ping_.next(pT), fb = fb_.next(fbT);
            const float mono = 0.5f * (xl + xr);
            line_[0].write((1.0f - pp) * xl + pp * mono + fb * ((1.0f - pp) * fl + pp * fr));
            line_[1].write((1.0f - pp) * xr + fb * ((1.0f - pp) * fr + pp * fl));
            const float a = std::max(std::fabs(xl), std::fabs(xr));
            duckEnv_ = a > duckEnv_ ? a : a + duckRel_ * (duckEnv_ - a);
            if (duckEnv_ < 1e-20f) duckEnv_ = 0.0f;
            const float dg = 1.0f - duck * std::min(1.0f, duckEnv_ * 4.0f);
            const float m = mix_.next(mT);
            L[i] = xl + m * (fl * dg - xl);
            R[i] = xr + m * (fr * dg - xr);
        }
        publishMeter(0, static_cast<float>(current_ / fs_ * 1000.0));
    }

private:
    double fs_ = 48000, wobPh_ = 0;
    DelayLine line_[2];
    Biquad2 hp_, lp_;
    float duckEnv_ = 0, duckRel_ = 0, current_ = -1, lastLow_ = -1, lastHigh_ = -1;
    Smoothed fb_, mix_, ping_, wob_;
};

} // namespace

std::unique_ptr<Effect> makeEcho() { return std::make_unique<Echo>(); }

} // namespace hc::fx
