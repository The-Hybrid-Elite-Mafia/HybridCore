#pragma once
// =============================================================================
//  Radix-2 complex FFT for the spectral HEM effects (HUSH, SWIRL KILLER).
//  prepare(n) allocates (message thread); forward / inverse never allocate.
//  inverse() includes the 1/n scale, so inverse(forward(x)) == x.
// =============================================================================
#include <complex>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

namespace hc::fx {

class Fft {
public:
    void prepare(int n) {
        n_ = n;
        tw_.resize(static_cast<std::size_t>(n / 2));
        for (int i = 0; i < n / 2; ++i) tw_[static_cast<std::size_t>(i)] = std::polar(1.0f, static_cast<float>(-2.0 * std::numbers::pi * i / n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[static_cast<std::size_t>(i)] = r;
        }
    }
    [[nodiscard]] int size() const noexcept { return n_; }

    void forward(std::complex<float>* x) const noexcept {
        for (int i = 0; i < n_; ++i) {
            const int r = rev_[static_cast<std::size_t>(i)];
            if (r > i) std::swap(x[i], x[r]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    const std::complex<float> t = x[i + k + half] * tw_[static_cast<std::size_t>(k * step)];
                    x[i + k + half] = x[i + k] - t;
                    x[i + k] += t;
                }
        }
    }
    void inverse(std::complex<float>* x) const noexcept {
        for (int i = 0; i < n_; ++i) x[i] = std::conj(x[i]);
        forward(x);
        const float s = 1.0f / static_cast<float>(n_);
        for (int i = 0; i < n_; ++i) x[i] = std::conj(x[i]) * s;
    }

private:
    int n_ = 0;
    std::vector<std::complex<float>> tw_;
    std::vector<int> rev_;
};

} // namespace hc::fx
