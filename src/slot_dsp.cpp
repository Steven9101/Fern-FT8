// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slot_dsp.h"

#include <algorithm>
#include <cmath>

namespace fern::ft8 {

void normalise(Llrs& llr) {
    double s2 = 0;
    for (float v : llr)
        s2 += double(v) * v;
    const double rms = std::sqrt(s2 / kLdpcN);
    const float k = rms > 0 ? float(1.0 / rms) : 0.0f;
    for (float& v : llr)
        v *= k;
}

void smooth(std::vector<cf>& v, int len) {
    std::vector<cf> tmp(v.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<cf>& in = pass == 0 ? v : tmp;
        std::vector<cf>& out = pass == 0 ? tmp : v;
        const int n = int(in.size());
        const int h = len / 2;
        std::complex<double> acc = 0;
        int count = 0;
        // Window [i - h, i + h), clipped at the ends.
        int lo = 0, hi = 0;
        for (int i = 0; i < n; ++i) {
            while (hi < std::min(n, i + h)) {
                acc += std::complex<double>(in[size_t(hi)]);
                ++hi;
                ++count;
            }
            while (lo < i - h) {
                acc -= std::complex<double>(in[size_t(lo)]);
                ++lo;
                --count;
            }
            out[size_t(i)] = count ? cf(acc / double(count)) : cf(0, 0);
        }
    }
}

void estimate_noise(const std::vector<cf>& x, int first_sample, int last_sample, double rate, int window,
                    int half, double min_hz, double max_hz, double signal_hz, std::vector<float>& noise,
                    int& noise_bin_lo, std::vector<cf>& buf) {
    const int n = window;
    const Fft& fft = fft_plan(size_t(n));
    const double bin_hz = rate / n;
    noise_bin_lo = int(std::floor(min_hz / bin_hz)) - 30;
    const int bin_hi = int(std::ceil((max_hz + signal_hz) / bin_hz)) + 30;
    const int nb = bin_hi - noise_bin_lo;
    std::vector<float> hann(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        hann[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / n));
    std::vector<std::vector<float>> cols{size_t(nb)};
    buf.resize(size_t(n));
    for (int start = first_sample; start + n <= last_sample; start += n / 2) {
        for (int i = 0; i < n; ++i)
            buf[size_t(i)] = x[size_t(start + i)] * hann[size_t(i)];
        fft.forward(buf.data());
        for (int b = 0; b < nb; ++b)
            cols[size_t(b)].push_back(std::norm(buf[size_t((noise_bin_lo + b) & (n - 1))]));
    }
    std::vector<float> raw(size_t(nb), 0.0f);
    for (int b = 0; b < nb; ++b) {
        auto& col = cols[size_t(b)];
        if (col.empty())
            continue;
        const size_t q = col.size() / 4;
        std::nth_element(col.begin(), col.begin() + long(q), col.end());
        // sum of the Hann window squared is 0.375 n
        raw[size_t(b)] = col[q] / 0.2877f / (0.375f * float(n));
    }
    noise.assign(size_t(nb), 0.0f);
    std::vector<float> win;
    for (int b = 0; b < nb; ++b) {
        win.clear();
        for (int j = std::max(0, b - half); j <= std::min(nb - 1, b + half); ++j)
            win.push_back(raw[size_t(j)]);
        const size_t q = win.size() / 4;
        std::nth_element(win.begin(), win.begin() + long(q), win.end());
        noise[size_t(b)] = win[q];
    }
}

}  // namespace fern::ft8
