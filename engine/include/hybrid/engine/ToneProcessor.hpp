#pragma once
// Diagnostic sine source for stream validation. Writes the same tone to every output.
#include "hybrid/engine/AudioProcessor.hpp"

namespace hc::engine {

class ToneProcessor final : public AudioProcessor {
public:
    explicit ToneProcessor(double frequencyHz = 440.0, double levelDbfs = -18.0) noexcept;
    void prepare(const io::StreamConfig& config) override;
    void process(const ProcessContext& ctx) noexcept override;

private:
    double frequency_;
    double amplitude_;
    double cosW_ = 1.0;
    double sinW_ = 0.0;
    double x_    = 1.0;   // rotating phasor (x, y); output = y
    double y_    = 0.0;
};

} // namespace hc::engine
