// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Iterative decimation in time: bit reversal, one radix-2 stage when the
// number of stages is odd, then radix-4 stages, each doing the work of two
// radix-2 stages with one pass over the data. Twiddles are computed in
// double and stored per stage in the order the butterflies read them, so
// that the AVX2 kernel can load four at a time. The AVX2 kernel does the
// same multiplications and additions in the same order as the scalar one,
// and its results are identical to the bit (tests/test_dsp.cpp).
#include "fft.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "simd.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

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
    std::vector<cf> twiddle(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * M_PI * double(k) / double(n);
        twiddle[k] = cf(float(std::cos(a)), float(std::sin(a)));
    }
    // Radix-4 stage with half-length h on blocks of 4h, p0..p3 at k, k+h,
    // k+2h, k+3h: the first radix-2 level uses W_{2h}^k = wb^2 (wa), the
    // second W_{4h}^k = wb for p0/p2 and W_{4h}^{k+h} = wb * (-i) for p1/p3.
    for (size_t h = (log2n_ & 1) ? 2 : 1; 4 * h <= n; h *= 4) {
        Stage st;
        st.h = h;
        const size_t stride = n / (4 * h);
        for (int dir = 0; dir < 2; ++dir) {
            const cf rot = dir ? cf(0, 1) : cf(0, -1);
            auto& wa = st.wa[dir];
            auto& wb = st.wb[dir];
            auto& wbr = st.wbr[dir];
            wa.resize(h);
            wb.resize(h);
            wbr.resize(h);
            for (size_t k = 0; k < h; ++k) {
                cf b = twiddle[k * stride];
                cf a = twiddle[2 * k * stride];
                if (dir) {
                    b = std::conj(b);
                    a = std::conj(a);
                }
                wa[k] = a;
                wb[k] = b;
                wbr[k] = b * rot;
            }
        }
        stages_.push_back(std::move(st));
    }
}

namespace {

inline void butterflies_scalar(cf* x, size_t n, size_t h, const cf* wa, const cf* wb, const cf* wbr) {
    for (size_t base = 0; base < n; base += 4 * h) {
        for (size_t k = 0; k < h; ++k) {
            cf* p = x + base + k;
            const cf t1 = wa[k] * p[h];
            const cf t3 = wa[k] * p[3 * h];
            const cf a0 = p[0] + t1, a1 = p[0] - t1;
            const cf b0 = p[2 * h] + t3, b1 = p[2 * h] - t3;
            const cf u0 = wb[k] * b0;
            const cf u1 = wbr[k] * b1;
            p[0] = a0 + u0;
            p[2 * h] = a0 - u0;
            p[h] = a1 + u1;
            p[3 * h] = a1 - u1;
        }
    }
}

#if defined(__x86_64__)
// w * a for four complex numbers, as std::complex computes it with
// -fcx-limited-range: re = w.re a.re - w.im a.im, im = w.re a.im + w.im a.re.
__attribute__((target("avx2"))) inline __m256 cmul4(__m256 w, __m256 a) {
    const __m256 wre = _mm256_moveldup_ps(w);
    const __m256 wim = _mm256_movehdup_ps(w);
    const __m256 t1 = _mm256_mul_ps(wre, a);                               // w.re a.re, w.re a.im
    const __m256 t2 = _mm256_mul_ps(wim, _mm256_permute_ps(a, 0xB1));     // w.im a.im, w.im a.re
    return _mm256_addsub_ps(t1, t2);
}

__attribute__((target("avx2"))) void butterflies_avx2(cf* x, size_t n, size_t h, const cf* wa, const cf* wb,
                                                       const cf* wbr) {
    for (size_t base = 0; base < n; base += 4 * h) {
        for (size_t k = 0; k < h; k += 4) {
            float* p = reinterpret_cast<float*>(x + base + k);
            const __m256 w_a = _mm256_loadu_ps(reinterpret_cast<const float*>(wa + k));
            const __m256 w_b = _mm256_loadu_ps(reinterpret_cast<const float*>(wb + k));
            const __m256 w_br = _mm256_loadu_ps(reinterpret_cast<const float*>(wbr + k));
            const __m256 p0 = _mm256_loadu_ps(p);
            const __m256 p1 = _mm256_loadu_ps(p + 2 * h);
            const __m256 p2 = _mm256_loadu_ps(p + 4 * h);
            const __m256 p3 = _mm256_loadu_ps(p + 6 * h);
            const __m256 t1 = cmul4(w_a, p1);
            const __m256 t3 = cmul4(w_a, p3);
            const __m256 a0 = _mm256_add_ps(p0, t1), a1 = _mm256_sub_ps(p0, t1);
            const __m256 b0 = _mm256_add_ps(p2, t3), b1 = _mm256_sub_ps(p2, t3);
            const __m256 u0 = cmul4(w_b, b0);
            const __m256 u1 = cmul4(w_br, b1);
            _mm256_storeu_ps(p, _mm256_add_ps(a0, u0));
            _mm256_storeu_ps(p + 4 * h, _mm256_sub_ps(a0, u0));
            _mm256_storeu_ps(p + 2 * h, _mm256_add_ps(a1, u1));
            _mm256_storeu_ps(p + 6 * h, _mm256_sub_ps(a1, u1));
        }
    }
}
#endif

}  // namespace

void Fft::transform(cf* x, bool inverse) const {
    for (size_t i = 0; i < n_; ++i) {
        const size_t j = bitrev_[i];
        if (j > i)
            std::swap(x[i], x[j]);
    }
    if (log2n_ & 1) {
        for (size_t i = 0; i < n_; i += 2) {
            const cf a = x[i], b = x[i + 1];
            x[i] = a + b;
            x[i + 1] = a - b;
        }
    }
    const int dir = inverse ? 1 : 0;
#if defined(__x86_64__)
    const bool avx2 = simd_level() >= SimdLevel::Avx2;
#endif
    for (const Stage& st : stages_) {
        const cf* wa = st.wa[dir].data();
        const cf* wb = st.wb[dir].data();
        const cf* wbr = st.wbr[dir].data();
#if defined(__x86_64__)
        if (avx2 && st.h >= 4) {
            butterflies_avx2(x, n_, st.h, wa, wb, wbr);
            continue;
        }
#endif
        butterflies_scalar(x, n_, st.h, wa, wb, wbr);
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
