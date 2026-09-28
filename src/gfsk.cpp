// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfsk.h"

#include <array>
#include <cmath>

namespace fern::ft8 {

namespace {

constexpr double kBT = 2.0;
// The pulse is negligible beyond 1.5 symbols from its centre: at u = 1 the
// nearer erf argument is already 5.3 * 2 * 0.5 = 5.3.
constexpr int kTableSteps = 4096;  // per symbol

const std::array<double, 3 * kTableSteps + 2>& pulse_table() {
    static const auto table = [] {
        std::array<double, 3 * kTableSteps + 2> t{};
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = gfsk_pulse(-1.5 + double(i) / kTableSteps);
        return t;
    }();
    return table;
}

double pulse_lookup(double u) {
    const double x = (u + 1.5) * kTableSteps;
    if (x <= 0.0 || x >= 3.0 * kTableSteps)
        return 0.0;
    const size_t i = size_t(x);
    const double f = x - double(i);
    const auto& t = pulse_table();
    return t[i] + f * (t[i + 1] - t[i]);
}

}  // namespace

double gfsk_pulse(double u) {
    const double k = M_PI * std::sqrt(2.0 / std::log(2.0));
    return 0.5 * (std::erf(k * kBT * (u + 0.5)) - std::erf(k * kBT * (u - 0.5)));
}

void add_ft8_waveform(const Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n) {
    const double T = kSymbolSeconds;
    const double duration = kSymbolCount * T;
    const double dt = 1.0 / rate;
    const double ramp = T / 8.0;
    // Frequency deviation in tones at time tau from the start. The tone
    // before the first and after the last is taken to continue, so that the
    // edges are steady under the ramps; pulses two or more symbols away
    // contribute nothing (erf(10.7) is 1 in double).
    auto deviation = [&](double tau) {
        const double x = tau / T;
        const double fl = std::floor(x);
        const int m = int(fl);
        const double u = x - fl;
        auto tone = [&](int s) { return double(tones[size_t(s < 0 ? 0 : s >= kSymbolCount ? kSymbolCount - 1 : s)]); };
        return tone(m - 1) * pulse_lookup(u + 0.5) + tone(m) * pulse_lookup(u - 0.5) +
               tone(m + 1) * pulse_lookup(u - 1.5);
    };
    const double first = std::ceil(start * rate);
    const size_t i0 = first < 0.0 ? 0 : size_t(first);
    if (i0 >= n)
        return;
    const double tau0 = double(i0) * dt - start;
    // Phase at the first sample, integrated from the start by the midpoint
    // rule; then the phasor turns sample by sample. Its step is the carrier
    // f0, the same for every sample, times the deviation's small turn,
    // below 2 pi 43.75 Hz / rate, whose sine and cosine a few terms of
    // their series give to 1e-9.
    const double phase0 = 2.0 * M_PI * (f0 + kToneSpacingHz * deviation(0.5 * tau0)) * tau0;
    std::complex<double> p = std::polar(1.0, phase0);
    const std::complex<double> carrier = std::polar(1.0, 2.0 * M_PI * f0 * dt);
    const double k = 2.0 * M_PI * kToneSpacingHz * dt;
    for (size_t i = i0; i < n; ++i) {
        const double tau = double(i) * dt - start;
        if (tau >= duration)
            break;
        double env = 1.0;
        if (tau < ramp)
            env = 0.5 * (1.0 - std::cos(M_PI * tau / ramp));
        else if (tau > duration - ramp)
            env = 0.5 * (1.0 - std::cos(M_PI * (duration - tau) / ramp));
        const double a = amplitude * env;
        out[i] += std::complex<float>(float(a * p.real()), float(a * p.imag()));
        const double x = k * deviation(tau + 0.5 * dt);
        const double x2 = x * x;
        const std::complex<double> turn(1.0 - x2 / 2.0 + x2 * x2 / 24.0, x * (1.0 - x2 / 6.0 + x2 * x2 / 120.0));
        p *= carrier * turn;
        if ((i & 1023) == 0)
            p /= std::abs(p);
    }
}

}  // namespace fern::ft8
