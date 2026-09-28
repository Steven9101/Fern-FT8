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
    // Frequency at time tau from the start, in hertz; the tone before the
    // first and after the last is taken to continue, so that the edges
    // are steady under the ramps.
    auto freq = [&](double tau) {
        const int m = int(std::floor(tau / T));
        double dev = 0.0;
        for (int s = m - 1; s <= m + 1; ++s) {
            const int idx = s < 0 ? 0 : s >= kSymbolCount ? kSymbolCount - 1 : s;
            dev += tones[size_t(idx)] * pulse_lookup(tau / T - (s + 0.5));
        }
        return f0 + kToneSpacingHz * dev;
    };
    const double first = std::ceil(start * rate);
    size_t i0 = first < 0.0 ? 0 : size_t(first);
    if (i0 >= n)
        return;
    double tau = double(i0) * dt - start;
    // Phase at the first sample: integrate from the start to it.
    double phase = 2.0 * M_PI * freq(0.5 * tau) * tau;
    for (size_t i = i0; i < n; ++i) {
        tau = double(i) * dt - start;
        if (tau >= duration)
            break;
        double env = 1.0;
        const double ramp = T / 8.0;
        if (tau < ramp)
            env = 0.5 * (1.0 - std::cos(M_PI * tau / ramp));
        else if (tau > duration - ramp)
            env = 0.5 * (1.0 - std::cos(M_PI * (duration - tau) / ramp));
        out[i] += std::complex<float>(float(amplitude * env * std::cos(phase)), float(amplitude * env * std::sin(phase)));
        phase += 2.0 * M_PI * freq(tau + 0.5 * dt) * dt;
        if (phase > M_PI * 64)
            phase = std::fmod(phase, 2.0 * M_PI);
        else if (phase < -M_PI * 64)
            phase = std::fmod(phase, 2.0 * M_PI);
    }
}

}  // namespace fern::ft8
