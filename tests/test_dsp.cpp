// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// FFT, waveform, resampler and LDPC decoders.
#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "fft.h"
#include "gfsk.h"
#include "ldpc.h"
#include "message.h"
#include "resampler.h"
#include "test.h"

using namespace fern::ft8;

TEST(fft_matches_a_direct_dft) {
    std::mt19937 rng(5);
    std::normal_distribution<float> g;
    for (size_t n : {2u, 4u, 8u, 32u, 128u, 512u, 2048u}) {
        std::vector<cf> x(n), y(n);
        for (auto& v : x)
            v = cf(g(rng), g(rng));
        y = x;
        fft_plan(n).forward(y.data());
        double err = 0, mag = 0;
        for (size_t k = 0; k < n; ++k) {
            std::complex<double> s = 0;
            for (size_t j = 0; j < n; ++j)
                s += std::complex<double>(x[j]) * std::polar(1.0, -2 * M_PI * double(j * k % n) / double(n));
            err = std::max(err, std::abs(s - std::complex<double>(y[k])));
            mag = std::max(mag, std::abs(s));
        }
        CHECK(err < 1e-5 * mag * std::log2(double(n)) + 1e-6);
        fft_plan(n).inverse(y.data());
        double back = 0;
        for (size_t j = 0; j < n; ++j)
            back = std::max(back, double(std::abs(y[j] / float(n) - x[j])));
        CHECK(back < 1e-5);
    }
}

TEST(gfsk_pulse_has_unit_area) {
    double area = 0;
    for (int i = -3000; i < 3000; ++i)
        area += gfsk_pulse((i + 0.5) / 1000.0) / 1000.0;
    CHECK(std::fabs(area - 1.0) < 1e-6);
}

TEST(waveform_holds_each_tone_for_a_symbol) {
    // The instantaneous frequency in the middle of each symbol is the tone's.
    const auto p = pack_message("CQ K1ABC FN42");
    REQUIRE(p.has_value());
    const Tones tones = tones_of(encode_codeword(*p));
    const double rate = 12000;
    std::vector<cf> w(size_t(rate * 13));
    add_ft8_waveform(tones, rate, 1000.0, 0.0, 1.0f, w.data(), w.size());
    for (int s = 0; s < kSymbolCount; ++s) {
        const size_t mid = size_t((s + 0.5) * kSymbolSeconds * rate);
        const double dphi = std::arg(w[mid + 1] * std::conj(w[mid]));
        const double f = dphi * rate / (2 * M_PI);
        CHECK(std::fabs(f - (1000.0 + 6.25 * tones[size_t(s)])) < 0.5);
    }
    // Constant envelope between the ramps, silence after the end.
    CHECK(std::fabs(std::abs(w[size_t(rate)]) - 1.0f) < 1e-4f);
    CHECK(std::abs(w[size_t(rate * 12.7)]) == 0.0f);
    CHECK(std::abs(w[0]) < 1e-3f);
}

TEST(resampler_keeps_a_tone_at_odd_rates) {
    for (double out_rate : {6400.0, 7031.25, 4000.0, 12345.678}) {
        const double in_rate = 12000;
        std::vector<cf> x(24000);
        const double f = 1234.5;
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = std::polar(1.0f, float(2 * M_PI * f * double(i) / in_rate));
        const auto y = resample(x, in_rate, out_rate, 1800.0);
        // Away from the edges: unit amplitude and the right phase.
        double worst = 0;
        for (size_t k = y.size() / 4; k < 3 * y.size() / 4; ++k) {
            const cf want = std::polar(1.0f, float(2 * M_PI * f * double(k) / out_rate));
            worst = std::max(worst, double(std::abs(y[k] - want)));
        }
        CHECK(worst < 2e-3);
    }
}

TEST(resampler_rejects_aliases) {
    // 5000 Hz at 12 kHz would land at -1400 Hz when read at 6400.
    const double in_rate = 12000;
    std::vector<cf> x(24000);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = std::polar(1.0f, float(2 * M_PI * 5000.0 * double(i) / in_rate));
    const auto y = resample(x, in_rate, 6400, 2100.0);
    double power = 0;
    for (size_t k = y.size() / 4; k < 3 * y.size() / 4; ++k)
        power += std::norm(y[k]);
    power /= double(y.size() / 2);
    CHECK(10 * std::log10(power + 1e-30) < -60);
}

namespace {

// Codeword bits sent as +-1 with Gaussian noise of standard deviation sigma.
Llrs noisy(const Codeword& cw, double sigma, std::mt19937& rng) {
    std::normal_distribution<double> g(0.0, sigma);
    Llrs llr;
    for (int i = 0; i < kLdpcN; ++i) {
        const double y = (cw[size_t(i)] ? -1.0 : 1.0) + g(rng);
        llr[size_t(i)] = float(2.0 * y / (sigma * sigma));
    }
    return llr;
}

Codeword random_codeword(std::mt19937& rng) {
    Payload p{};
    for (int i = 0; i < kPayloadBits; ++i)
        set_payload_bit(p, i, int(rng() & 1));
    return encode_codeword(p);
}

}  // namespace

TEST(bp_decodes_a_moderately_noisy_codeword) {
    std::mt19937 rng(6);
    int ok = 0;
    const int trials = 200;
    for (int t = 0; t < trials; ++t) {
        const Codeword cw = random_codeword(rng);
        // Eb/N0 3 dB for this rate 0.52 code, where BP should fail rarely.
        const BpResult r = bp_decode(noisy(cw, 0.69, rng), 30);
        ok += r.converged && r.codeword == cw;
    }
    CHECK(ok >= trials * 93 / 100);
}

TEST(osd_decodes_what_bp_misses) {
    std::mt19937 rng(7);
    int bp = 0, osd = 0, wrong = 0;
    const int trials = test::quick() ? 60 : 300;
    for (int t = 0; t < trials; ++t) {
        const Codeword cw = random_codeword(rng);
        const Llrs llr = noisy(cw, 0.95, rng);
        const BpResult r = bp_decode(llr, 30);
        if (r.converged && r.codeword == cw) {
            ++bp;
            continue;
        }
        OsdOptions o;
        o.order = 2;
        const OsdResult d = osd_decode(llr, o);
        if (d.found && d.codeword == cw)
            ++osd;
        else if (d.found)
            ++wrong;
    }
    std::fprintf(stderr, "    %d trials: bp %d, then osd %d, wrong %d\n", trials, bp, osd, wrong);
    CHECK(osd > 0);
    CHECK(bp + osd > bp * 105 / 100);
    // Choosing by the CRC among many trials finds a wrong codeword now and
    // then at this noise level, 0 dB Eb/N0; the decoder's acceptance rules
    // screen those out, not OSD.
    CHECK(wrong <= trials * 4 / 100);
}

TEST(osd_finds_the_codeword_from_clean_bits) {
    std::mt19937 rng(8);
    const Codeword cw = random_codeword(rng);
    Llrs llr;
    for (int i = 0; i < kLdpcN; ++i)
        llr[size_t(i)] = cw[size_t(i)] ? -1.0f - float(i % 7) : 1.0f + float(i % 5);
    const OsdResult d = osd_decode(llr, OsdOptions{});
    CHECK(d.found);
    CHECK(d.codeword == cw);
    CHECK_EQ(d.disagreements, 0);
}
