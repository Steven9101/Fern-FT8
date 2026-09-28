// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A complex FFT for power-of-two sizes, in place, unnormalised.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fern::ft8 {

using cf = std::complex<float>;

class Fft {
public:
    // n must be a power of two, at least 2.
    explicit Fft(size_t n);
    size_t size() const { return n_; }
    // X[k] = sum x[j] e^{-2 pi i jk/n}
    void forward(cf* data) const { transform(data, false); }
    // x[j] = sum X[k] e^{+2 pi i jk/n}, without the 1/n.
    void inverse(cf* data) const { transform(data, true); }

private:
    void transform(cf* data, bool inverse) const;

    size_t n_;
    int log2n_;
    std::vector<uint32_t> bitrev_;
    // e^{-2 pi i k/n} for k < n/2
    std::vector<cf> twiddle_;
};

// One shared plan per size; plans are immutable, so threads may share them.
const Fft& fft_plan(size_t n);

}  // namespace fern::ft8
