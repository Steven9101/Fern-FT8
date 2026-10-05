// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfsk.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fern::ft8 {

namespace {

// The pulse is negligible beyond 1.5 symbols from its centre: at u = 1 the
// nearer erf argument is already 5.3 * BT * 0.5, 5.3 for FT8 and 2.7 for
// FT4 (whose pulse there is below 1e-4, and below 1e-13 at u = 1.5).
constexpr int kTableSteps = 4096;  // per symbol
using PulseTable = std::array<double, 3 * kTableSteps + 2>;

PulseTable make_pulse_table(double bt) {
    PulseTable t{};
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = gfsk_pulse(-1.5 + double(i) / kTableSteps, bt);
    return t;
}

double pulse_lookup(const PulseTable& t, double u) {
    const double x = (u + 1.5) * kTableSteps;
    if (x <= 0.0 || x >= 3.0 * kTableSteps)
        return 0.0;
    const size_t i = size_t(x);
    const double f = x - double(i);
    return t[i] + f * (t[i + 1] - t[i]);
}

// What tells one mode's waveform from the other's.
struct Shape {
    const uint8_t* tones;
    int count;
    double symbol_seconds;
    double spacing_hz;
    // Raised-cosine ramps over this long at the start and the end.
    double ramp_seconds;
    const PulseTable& pulses;
};

void add_waveform(const Shape& shape, double rate, double f0, double start, float amplitude,
                  std::complex<float>* out, size_t n);

}  // namespace

double gfsk_pulse(double u, double bt) {
    const double k = M_PI * std::sqrt(2.0 / std::log(2.0));
    return 0.5 * (std::erf(k * bt * (u + 0.5)) - std::erf(k * bt * (u - 0.5)));
}

void add_ft8_waveform(const Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n) {
    static const PulseTable pulses = make_pulse_table(kFt8BT);
    add_waveform({tones.data(), kSymbolCount, kSymbolSeconds, kToneSpacingHz, kSymbolSeconds / 8.0, pulses}, rate,
                 f0, start, amplitude, out, n);
}

void add_ft4_waveform(const ft4::Tones& tones, double rate, double f0, double start, float amplitude,
                      std::complex<float>* out, size_t n) {
    static const PulseTable pulses = make_pulse_table(kFt4BT);
    const ft4::WaveTones wave = ft4::wave_tones(tones);
    // [QEX] section 5: the ramps take the whole of the two ramp symbols.
    add_waveform({wave.data(), ft4::kWaveSymbolCount, ft4::kSymbolSeconds, ft4::kToneSpacingHz,
                  ft4::kSymbolSeconds, pulses},
                 rate, f0, start, amplitude, out, n);
}

namespace {

void add_waveform(const Shape& shape, double rate, double f0, double start, float amplitude,
                  std::complex<float>* out, size_t n) {
    const uint8_t* tones = shape.tones;
    const int count = shape.count;
    const PulseTable& pulses = shape.pulses;
    const double T = shape.symbol_seconds;
    const double duration = count * T;
    const double dt = 1.0 / rate;
    const double ramp = shape.ramp_seconds;
    // Frequency deviation in tones at time tau from the start. The tone
    // before the first and after the last is taken to continue, so that the
    // edges are steady under the ramps; pulses two or more symbols away
    // contribute nothing (erf(10.7) is 1 in double).
    auto deviation = [&](double tau) {
        const double x = tau / T;
        const double fl = std::floor(x);
        const int m = int(fl);
        const double u = x - fl;
        auto tone = [&](int s) { return double(tones[size_t(s < 0 ? 0 : s >= count ? count - 1 : s)]); };
        return tone(m - 1) * pulse_lookup(pulses, u + 0.5) + tone(m) * pulse_lookup(pulses, u - 0.5) +
               tone(m + 1) * pulse_lookup(pulses, u - 1.5);
    };
    const double first = std::ceil(start * rate);
    const size_t i0 = first < 0.0 ? 0 : size_t(first);
    if (i0 >= n)
        return;
    const double tau0 = double(i0) * dt - start;
    // Phase at the first sample, integrated from the start by the midpoint
    // rule; then the phasor turns sample by sample. Its step is the carrier
    // f0, the same for every sample, times the deviation's small turn,
    // below 2 pi 62.5 Hz / rate (FT4's tone 3), whose sine and cosine a few
    // terms of their series give to 1e-9.
    // When a symbol is a whole number of samples (1024 at 6400 Hz, 1920 at
    // 12000 Hz; for FT4 256 at 16000/3 Hz, 576 at 12000 Hz), the midpoint of sample i lies at the same place within its
    // symbol as that of sample i + sps, so the three pulses that shape a
    // symbol are tabulated once for that symbol's samples.
    const double sps_real = rate * T;
    const long sps = std::lround(sps_real);
    const bool whole = std::fabs(sps_real - double(sps)) < 1e-9 && sps <= 8192;
    // Sample i0 + q sps + r has its midpoint at x = M0 + q + u0 + r / sps
    // symbols from the start, M0 and u0 the whole and fractional parts for
    // sample i0: in symbol M0 + q, or the next when u0 + r / sps reaches 1.
    std::vector<double> pa, pb, pc;
    std::vector<uint8_t> next;
    long m_first = 0;
    if (whole) {
        pa.resize(size_t(sps));
        pb.resize(size_t(sps));
        pc.resize(size_t(sps));
        next.resize(size_t(sps));
        const double x_first = (tau0 + 0.5 * dt) / T;
        m_first = long(std::floor(x_first));
        const double u0 = x_first - double(m_first);
        for (long r = 0; r < sps; ++r) {
            double u = u0 + double(r) / double(sps);
            next[size_t(r)] = u >= 1.0;
            if (u >= 1.0)
                u -= 1.0;
            pa[size_t(r)] = pulse_lookup(pulses, u + 0.5);
            pb[size_t(r)] = pulse_lookup(pulses, u - 0.5);
            pc[size_t(r)] = pulse_lookup(pulses, u - 1.5);
        }
    }
    auto tone_at = [&](long s) { return double(tones[size_t(s < 0 ? 0 : s >= count ? count - 1 : s)]); };
    const double phase0 = 2.0 * M_PI * (f0 + shape.spacing_hz * deviation(0.5 * tau0)) * tau0;
    std::complex<double> p = std::polar(1.0, phase0);
    const std::complex<double> carrier = std::polar(1.0, 2.0 * M_PI * f0 * dt);
    const double k = 2.0 * M_PI * shape.spacing_hz * dt;
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
        double dev;
        if (whole) {
            const long j = long(i - i0);
            const size_t r = size_t(j % sps);
            const long m = m_first + j / sps + next[r];
            dev = tone_at(m - 1) * pa[r] + tone_at(m) * pb[r] + tone_at(m + 1) * pc[r];
        } else {
            dev = deviation(tau + 0.5 * dt);
        }
        const double x = k * dev;
        const double x2 = x * x;
        const std::complex<double> turn(1.0 - x2 / 2.0 + x2 * x2 / 24.0, x * (1.0 - x2 / 6.0 + x2 * x2 / 120.0));
        p *= carrier * turn;
        if ((i & 1023) == 0)
            p /= std::abs(p);
    }
}

}  // namespace

}  // namespace fern::ft8
