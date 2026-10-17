#include "hybrid/analysis/Fft.hpp"

#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace hc::analysis {

Fft::Fft(std::size_t size) : n_(size) {
    if (size < 2 || size > 65536 || !std::has_single_bit(size)) throw std::invalid_argument("Fft: size must be a power of two, 2..65536");
    twiddle_.resize(n_ / 2);
    for (std::size_t k = 0; k < n_ / 2; ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
        twiddle_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    const int bits = std::countr_zero(n_);
    reverse_.resize(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
        reverse_[i] = r;
    }
}

void Fft::transform(std::complex<float>* x) const noexcept {
    for (std::size_t i = 0; i < n_; ++i)
        if (i < reverse_[i]) std::swap(x[i], x[reverse_[i]]);
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2, step = n_ / len;
        for (std::size_t s = 0; s < n_; s += len)
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<float> t = twiddle_[k * step] * x[s + k + half];
                x[s + k + half] = x[s + k] - t;
                x[s + k] += t;
            }
    }
}

} // namespace hc::analysis
