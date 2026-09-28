// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Iterative radix-2 decimation in time, with radix-4 passes where the size
// allows: a radix-4 pass does the work of two radix-2 passes with one read
// and write of the data instead of two. Twiddles are computed in double.
#include "fft.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace fern::ft8 {

Fft::Fft(size_t n) : n_(n), log2n_(0) {
    if (n < 2 || (n & (n - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two");
    while ((size_t(1) << log2n_) < n)
        ++log2n_;
    bitrev_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < log2n_; ++b)
            if (i & (size_t(1) << b))
                r |= uint32_t(1) << (log2n_ - 1 - b);
        bitrev_[i] = r;
    }
    twiddle_.resize(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * M_PI * double(k) / double(n);
        twiddle_[k] = cf(float(std::cos(a)), float(std::sin(a)));
    }
}

void Fft::transform(cf* x, bool inverse) const {
    for (size_t i = 0; i < n_; ++i) {
        const size_t j = bitrev_[i];
        if (j > i)
            std::swap(x[i], x[j]);
    }
    size_t h = 1;
    if (log2n_ & 1) {
        for (size_t i = 0; i < n_; i += 2) {
            const cf a = x[i], b = x[i + 1];
            x[i] = a + b;
            x[i + 1] = a - b;
        }
        h = 2;
    }
    // Two radix-2 stages at once: the stage of half-length h on blocks of 2h
    // and the stage of half-length 2h on blocks of 4h. With p0..p3 at k,
    // k+h, k+2h, k+3h: the first uses W_{2h}^k = wb^2, the second W_{4h}^k =
    // wb for p0/p2 and W_{4h}^{k+h} = wb * (-i) for p1/p3 (conjugated for
    // the inverse).
    const cf rot = inverse ? cf(0, 1) : cf(0, -1);
    for (; 4 * h <= n_; h *= 4) {
        const size_t stride = n_ / (4 * h);
        for (size_t base = 0; base < n_; base += 4 * h) {
            for (size_t k = 0; k < h; ++k) {
                cf wb = twiddle_[k * stride];
                cf wa = twiddle_[2 * k * stride];
                if (inverse) {
                    wb = std::conj(wb);
                    wa = std::conj(wa);
                }
                cf* p = x + base + k;
                const cf t1 = wa * p[h];
                const cf t3 = wa * p[3 * h];
                const cf a0 = p[0] + t1, a1 = p[0] - t1;
                const cf b0 = p[2 * h] + t3, b1 = p[2 * h] - t3;
                const cf u0 = wb * b0;
                const cf u1 = wb * rot * b1;
                p[0] = a0 + u0;
                p[2 * h] = a0 - u0;
                p[h] = a1 + u1;
                p[3 * h] = a1 - u1;
            }
        }
    }
}

const Fft& fft_plan(size_t n) {
    static std::mutex mutex;
    static std::map<size_t, std::unique_ptr<Fft>> plans;
    std::lock_guard<std::mutex> lock(mutex);
    auto& p = plans[n];
    if (!p)
        p = std::make_unique<Fft>(n);
    return *p;
}

}  // namespace fern::ft8
