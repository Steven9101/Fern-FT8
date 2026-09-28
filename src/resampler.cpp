// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "resampler.h"

#include <algorithm>
#include <cmath>

namespace fern::ft8 {

namespace {

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

}  // namespace

Interpolator::Interpolator(double in_rate, double out_rate, double passband_hz) : phases_(256) {
    // Whatever lies above stop_hz may land in the passband after reading at
    // out_rate (down) or is an image of the input (up); in between nothing
    // matters, so the filter can use all of it as transition band.
    const double pass = std::min(passband_hz, 0.45 * in_rate);
    double stop = std::min(in_rate, out_rate) - pass;
    stop = std::max(stop, pass + 0.05 * in_rate);
    const double cutoff = 0.5 * (pass + stop);
    // Kaiser window, beta 7: about 70 dB of rejection; its length follows
    // the transition width.
    const double beta = 7.0;
    half_ = int(std::ceil(2.2 * in_rate / (stop - pass))) + 1;
    const double scale = 2.0 * cutoff / in_rate;
    const double i0b = bessel_i0(beta);
    const int row = 2 * half_;
    taps_.resize(size_t(phases_ + 1) * size_t(row));
    for (int q = 0; q <= phases_; ++q) {
        const double frac = double(q) / phases_;
        for (int t = 0; t < row; ++t) {
            const double d = double(t - half_ + 1) - frac;  // input offset from the read position
            const double u = d / half_;
            double w = 0.0;
            if (std::fabs(u) < 1.0)
                w = bessel_i0(beta * std::sqrt(1.0 - u * u)) / i0b;
            const double a = M_PI * scale * d;
            const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
            taps_[size_t(q) * size_t(row) + size_t(t)] = float(scale * sinc * w);
        }
    }
}

std::complex<float> Interpolator::at(const std::complex<float>* x, double frac) const {
    const double pos = frac * phases_;
    int q = int(pos);
    if (q >= phases_)
        q = phases_ - 1;
    const float f = float(pos - q);
    const int row = 2 * half_;
    const float* a = &taps_[size_t(q) * size_t(row)];
    const float* b = a + row;
    float re = 0.0f, im = 0.0f;
    for (int t = 0; t < row; ++t) {
        const float h = a[t] + f * (b[t] - a[t]);
        re += h * x[t].real();
        im += h * x[t].imag();
    }
    return {re, im};
}

std::vector<std::complex<float>> resample(const std::vector<std::complex<float>>& in, double in_rate,
                                          double out_rate, double passband_hz, double start) {
    const Interpolator interp(in_rate, out_rate, passband_hz);
    const int half = interp.half_width();
    // Pad with zeros so every read has its taps.
    std::vector<std::complex<float>> padded(in.size() + size_t(4 * half) + 2);
    std::copy(in.begin(), in.end(), padded.begin() + 2 * half);
    const double last = double(in.size());
    const size_t n = size_t(std::max(0.0, std::floor((last - start) * out_rate / in_rate)));
    std::vector<std::complex<float>> out(n);
    for (size_t k = 0; k < n; ++k) {
        const double p = start + double(k) * in_rate / out_rate;
        const double fl = std::floor(p);
        const long i = long(fl);
        const long first = i - half + 1 + 2 * half;
        if (first < 0 || size_t(first) + size_t(2 * half) > padded.size())
            continue;
        out[k] = interp.at(&padded[size_t(first)], p - fl);
    }
    return out;
}

}  // namespace fern::ft8
