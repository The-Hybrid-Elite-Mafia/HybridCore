#pragma once
// =============================================================================
//  Fft — in-place radix-2 complex FFT for the analyzer (UI thread). Twiddles and the
//  bit-reversal table are built once in the constructor; transform() never allocates.
// =============================================================================
#include <complex>
#include <cstddef>
#include <vector>

namespace hc::analysis {

class Fft {
public:
    explicit Fft(std::size_t size);   // size: power of two, 2 .. 65536 (throws otherwise)
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    void transform(std::complex<float>* data) const noexcept;   // forward, unscaled, in place

private:
    std::size_t n_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<std::size_t>         reverse_;
};

} // namespace hc::analysis
