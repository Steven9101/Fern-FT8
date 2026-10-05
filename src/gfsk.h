// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The FT8 and FT4 waveforms of [QEX] section 5: continuous-phase FSK with
// h = 1, the frequency deviation smoothed by a Gaussian filter (equation 3)
// of BT = 2 for FT8's 8 tones and BT = 1 for FT4's 4, and raised-cosine
// ramps: over the first and last T/8 of FT8's 79 symbols, over the whole of
// the ramp symbols around FT4's 103. The encoder uses them to make test
// signals and the decoders to rebuild a decoded signal before subtracting
// it.
#pragma once

#include <complex>
#include <cstddef>

#include "ft4.h"
#include "protocol.h"

namespace fern::ft8 {

constexpr double kFt8BT = 2.0;
constexpr double kFt4BT = 1.0;

// The smoothed frequency pulse of [QEX] equation 3 times T, at u = t/T from
// the pulse centre: 0.5 (erf(k BT (u + 0.5)) - erf(k BT (u - 0.5))) with
// k = pi sqrt(2 / ln 2). Its integral over u is 1.
double gfsk_pulse(double u, double bt = kFt8BT);

// Adds amplitude * e^{i phase(t)} to out[0..n) at `rate` samples a second:
// the transmission of `tones` with tone 0 at f0 hertz (may be negative, for
// complex baseband), starting `start` seconds after out[0]. Samples outside
// the 12.64 s of the transmission are left alone. Phase is 0 at the start.
void add_ft8_waveform(const Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n);

// The same for FT4: `start` is the start of the first ramp symbol, and the
// transmission lasts 105 symbols, 5.04 s.
void add_ft4_waveform(const ft4::Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n);

}  // namespace fern::ft8
