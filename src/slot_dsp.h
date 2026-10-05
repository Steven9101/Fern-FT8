// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pieces the FT8 and FT4 slot decoders share: complex products written out
// (std::complex's operator* checks for infinities and costs a call), soft
// bit scaling, the smoothing of a decoded signal's gain, and the noise
// estimate behind SNR.
#pragma once

#include <complex>
#include <vector>

#include "fft.h"
#include "ldpc.h"

namespace fern::ft8 {

inline cf cmul_conj(cf a, cf b) {  // a * conj(b)
    return cf(a.real() * b.real() + a.imag() * b.imag(), a.imag() * b.real() - a.real() * b.imag());
}

inline cf cmul(cf a, cf b) {
    return cf(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real());
}

// Unit root-mean-square: min-sum and OSD do not depend on the scale, this
// only keeps the numbers in a comfortable range.
void normalise(Llrs& llr);

// Moving average of length len, centred, applied twice (a triangle).
void smooth(std::vector<cf>& v, int len);

// Noise power per sample (sigma^2 of the complex baseband) as a function of
// frequency, from Hann-windowed spectra of `window` samples every half
// window of x (at `rate`) between first_sample and last_sample: the
// rectangular windows of the sync spectrograms leak a strong signal's power
// far into its neighbourhood, a Hann window does not. Per bin the lower
// quartile over time, scaled to the mean of an exponential distribution
// (its lower quartile is -ln(0.75) = 0.2877 of the mean), is noise where
// signals come and go; a transmission fills its own bins all the time, so
// the value kept is the lower quartile of those over +-`half` bins, mostly
// bins between signals. Bins cover min_hz to max_hz + signal_hz with 30 to
// spare on each side; noise[b] is the bin noise_bin_lo + b.
void estimate_noise(const std::vector<cf>& x, int first_sample, int last_sample, double rate, int window,
                    int half, double min_hz, double max_hz, double signal_hz, std::vector<float>& noise,
                    int& noise_bin_lo, std::vector<cf>& buf);

}  // namespace fern::ft8
