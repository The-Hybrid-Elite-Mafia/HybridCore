#pragma once
// =============================================================================
//  Small radix-2 complex FFT used by the Hiss Tamer (engine uses float, the
//  offline scan and the tests use double). prepare() allocates; forward() and
//  inverse() never do. inverse() includes the 1/n scale, so inverse(forward(x)) == x.
//  The butterflies use plain real arithmetic (no std::complex multiply) so the
//  speed does not depend on the compiler's complex-number fix-up code.
// =============================================================================
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace hc::hiss {

template <class T>
class FftT {
public:
    using Cx = std::complex<T>;

    void prepare(int n) {
        n_ = n;
        tw_.assign(static_cast<std::size_t>(n / 2), Cx{});
        for (int i = 0; i < n / 2; ++i) {
            const double a = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
            tw_[static_cast<std::size_t>(i)] = Cx(static_cast<T>(std::cos(a)), static_cast<T>(std::sin(a)));
        }
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev_.assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[static_cast<std::size_t>(i)] = r;
        }
    }
    [[nodiscard]] int size() const noexcept { return n_; }

    void forward(Cx* x) const noexcept {
        for (int i = 0; i < n_; ++i) {
            const int r = rev_[static_cast<std::size_t>(i)];
            if (r > i) std::swap(x[i], x[r]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    const Cx w = tw_[static_cast<std::size_t>(k * step)];
                    Cx& a = x[i + k];
                    Cx& b = x[i + k + half];
                    const T tr = b.real() * w.real() - b.imag() * w.imag();
                    const T ti = b.real() * w.imag() + b.imag() * w.real();
                    b = Cx(a.real() - tr, a.imag() - ti);
                    a = Cx(a.real() + tr, a.imag() + ti);
                }
        }
    }
    void inverse(Cx* x) const noexcept {
        for (int i = 0; i < n_; ++i) x[i] = std::conj(x[i]);
        forward(x);
        const T s = static_cast<T>(1) / static_cast<T>(n_);
        for (int i = 0; i < n_; ++i) x[i] = Cx(x[i].real() * s, -x[i].imag() * s);
    }

private:
    int n_ = 0;
    std::vector<Cx> tw_;
    std::vector<int> rev_;
};

using Fft  = FftT<float>;
using FftD = FftT<double>;

} // namespace hc::hiss
