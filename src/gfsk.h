// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The FT8 waveform of [QEX] section 5: continuous-phase 8-FSK with h = 1,
// the frequency deviation smoothed by a Gaussian filter of BT = 2
// (equation 3), and raised-cosine ramps over the first and last T/8.
// The encoder uses it to make test signals and the decoder to rebuild a
// decoded signal before subtracting it.
#pragma once

#include <complex>
#include <cstddef>

#include "protocol.h"

namespace fern::ft8 {

// The smoothed frequency pulse of [QEX] equation 3 times T, at u = t/T from
// the pulse centre: 0.5 (erf(k BT (u + 0.5)) - erf(k BT (u - 0.5))) with
// k = pi sqrt(2 / ln 2) and BT = 2. Its integral over u is 1.
double gfsk_pulse(double u);

// Adds amplitude * e^{i phase(t)} to out[0..n) at `rate` samples a second:
// the transmission of `tones` with tone 0 at f0 hertz (may be negative, for
// complex baseband), starting `start` seconds after out[0]. Samples outside
// the 12.64 s of the transmission are left alone. Phase is 0 at the start.
void add_ft8_waveform(const Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n);

}  // namespace fern::ft8
