// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Complex resampling between arbitrary rates with a Kaiser-windowed sinc,
// evaluated at any fractional input position. The host's channel rate need
// not be whole (64e6 * L / K, say), so nothing here assumes a rational
// ratio with small terms.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace fern::ft8 {

class Interpolator {
public:
    // A low-pass interpolation kernel for input at in_rate that keeps
    // |f| <= passband_hz and rejects whatever could alias into it when read
    // out at out_rate.
    Interpolator(double in_rate, double out_rate, double passband_hz);
    // Half the kernel length, in input samples: reading at position p needs
    // input samples floor(p) - half_width() + 1 .. floor(p) + half_width().
    int half_width() const { return half_; }
    // The value at fractional input position p (0 <= p - floor(p) < 1) from
    // x, where x[0] is input sample floor(p) - half_width() + 1.
    std::complex<float> at(const std::complex<float>* x, double frac) const;

private:
    int half_;
    int phases_;
    // Row q (0..phases_) holds the kernel for fraction q / phases_ at the
    // 2 * half_ taps; rows next to each other are interpolated.
    std::vector<float> taps_;
};

// Resamples a whole block at once: out[k] = input at position start + k *
// in_rate / out_rate. Samples outside the input read as zero.
std::vector<std::complex<float>> resample(const std::vector<std::complex<float>>& in, double in_rate,
                                          double out_rate, double passband_hz, double start = 0.0);

}  // namespace fern::ft8
