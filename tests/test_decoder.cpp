// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The whole receive path: synthetic transmissions through a Channel at
// several rates, gaps and clock changes, white noise and silence, and
// golden decodes of real recordings.
#include <cmath>
#include <complex>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "channel.h"
#include "gfsk.h"
#include "message.h"
#include "simd.h"
#include "test.h"
#include "wav.h"

using namespace fern::ft8;

namespace {

using cvec = std::vector<std::complex<float>>;

struct Tx {
    std::string text;
    double audio_hz;  // tone 0 above the dial
    double dt;
    double snr_db;
};

// Complex baseband at `rate` with baseband 0 Hz at audio `offset`, covering
// `seconds` from UTC `start_s`, with the transmissions of slot `slot_s`
// (a multiple of 15) and white noise of unit power per sample... scaled so
// that SNR is per 2500 Hz: noise density is 1 / rate per hertz.
cvec make_baseband(const std::vector<Tx>& txs, double rate, double offset, double start_s, double seconds,
                   double slot_s, unsigned seed, bool noise = true) {
    const size_t n = size_t(std::llround(seconds * rate));
    cvec x(n);
    for (const Tx& t : txs) {
        const auto p = pack_message(t.text);
        if (!p)
            continue;
        const double amp = std::sqrt(std::pow(10.0, t.snr_db / 10.0) * 2500.0 / rate);
        add_ft8_waveform(tones_of(encode_codeword(*p)), rate, t.audio_hz - offset,
                         slot_s - start_s + kStartSeconds + t.dt, float(amp), x.data(), n);
    }
    if (noise) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> g(0.0f, float(std::sqrt(0.5)));
        for (auto& v : x)
            v += std::complex<float>(g(rng), g(rng));
    }
    return x;
}

std::vector<SlotResult> run_channel(Channel& ch, const cvec& x, double rate, double start_s, uint64_t first_index = 0,
                                    size_t frame = 4000) {
    std::vector<SlotResult> out;
    for (size_t i = 0; i < x.size(); i += frame) {
        const size_t n = std::min(frame, x.size() - i);
        const int64_t us = int64_t(std::llround(start_s * 1e6 + double(i) * 1e6 / rate));
        ch.push(&x[i], n, first_index + i, us, 0);
        for (auto& r : ch.decode_ready())
            out.push_back(std::move(r));
    }
    return out;
}

std::vector<SlotResult> run_to_end(Channel& ch, const cvec& x, double rate, double start_s) {
    auto out = run_channel(ch, x, rate, start_s);
    for (auto& r : ch.finish())
        out.push_back(std::move(r));
    return out;
}

const Decode* find(const std::vector<SlotResult>& rs, const std::string& text) {
    for (const auto& r : rs)
        for (const auto& d : r.decodes)
            if (d.message.text == text)
                return &d;
    return nullptr;
}

size_t count_decodes(const std::vector<SlotResult>& rs) {
    size_t n = 0;
    for (const auto& r : rs)
        n += r.decodes.size();
    return n;
}

ChannelConfig config(double rate) {
    ChannelConfig c;
    c.rate = rate;
    return c;
}

}  // namespace

TEST(decodes_transmissions_with_their_frequency_time_and_snr) {
    const double rate = 8000, offset = 2000;
    const double slot = 15 * 1800;
    const std::vector<Tx> txs = {
        {"CQ K1ABC FN42", 400.0, 0.0, -10},
        {"K1ABC W9XYZ -12", 1234.5, 0.8, -14},
        {"W9XYZ K1ABC R-12", 2600.3, -0.6, -6},
        {"CQ DX DL1ABC JO31", 3500.0, 1.9, 0},
    };
    // Up to 16.4 s into the slot: not all of it, so finish() decodes it,
    // and too little of the next slot to try that one.
    const cvec x = make_baseband(txs, rate, offset, slot - 2.0, 18.4, slot, 1);
    ChannelConfig cfg;
    cfg.rate = rate;
    CallsignHashTable hashes;
    Channel ch(cfg, &hashes);
    const auto rs = run_to_end(ch, x, rate, slot - 2.0);
    REQUIRE(rs.size() == 1);
    CHECK_EQ(rs[0].slot_start_ms, int64_t(slot * 1000));
    CHECK_EQ(count_decodes(rs), txs.size());
    for (const Tx& t : txs) {
        const Decode* d = find(rs, t.text);
        REQUIRE(d != nullptr || (std::fprintf(stderr, "    missing %s\n", t.text.c_str()), false));
        CHECK(std::fabs(d->freq_hz - t.audio_hz) < 0.5);
        CHECK(std::fabs(d->dt - t.dt) < 0.02);
        CHECK(std::abs(d->snr_db - int(t.snr_db)) <= 2);
        CHECK_EQ(std::string(d->quality()), std::string("bp"));
        CHECK(!d->a_priori);
    }
    // The CQ carries the grid and call a spot report needs.
    const Decode* cq = find(rs, "CQ DX DL1ABC JO31");
    REQUIRE(cq != nullptr);
    CHECK_EQ(cq->message.fields.de_call, std::string("DL1ABC"));
    CHECK_EQ(cq->message.fields.grid, std::string("JO31"));
}

TEST(decodes_at_odd_and_low_rates) {
    // 64 MHz * 3 / 32768 and friends: rates that are not whole numbers.
    for (double rate : {5859.375, 7031.25, 4000.0, 12000.0, 11718.75}) {
        const double offset = 2000;
        const double slot = 15 * 4000;
        const std::vector<Tx> txs = {{"CQ PJ4/K1ABC", 1500.0, 0.2, -12}, {"K1ABC W9XYZ RR73", 700.0, 0.4, -12}};
        const cvec x = make_baseband(txs, rate, offset, slot - 1.7, 19.0, slot, 2);
        ChannelConfig cfg;
        cfg.rate = rate;
        Channel ch(cfg, nullptr);
        const auto rs = run_to_end(ch, x, rate, slot - 1.7);
        for (const Tx& t : txs) {
            const Decode* d = find(rs, t.text);
            if (!d)
                std::fprintf(stderr, "    rate %.3f: missing %s\n", rate, t.text.c_str());
            CHECK(d != nullptr);
            if (d)
                CHECK(std::fabs(d->dt - t.dt) < 0.02);
        }
    }
}

TEST(slots_follow_utc_across_a_stream) {
    // Two minutes in one stream: each slot decodes its own transmission.
    const double rate = 8000, offset = 2000, start = 900000.0 - 7.3;
    std::vector<cvec> parts;
    cvec x(size_t(128 * rate));
    for (int k = 0; k < 8; ++k) {
        const double slot = 900000.0 + 15 * k;
        const std::string text = "K1ABC W9XYZ " + std::string(k % 2 ? "R" : "") + "-0" + std::to_string(k + 1);
        const cvec s = make_baseband({{text, 1000.0 + 100 * k, 0.1 * k, -8}}, rate, offset, start, 128, slot, 10 + k,
                                     false);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += s[i];
    }
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.0f, float(std::sqrt(0.5)));
    for (auto& v : x)
        v += std::complex<float>(g(rng), g(rng));
    Channel ch(config(rate), nullptr);
    const auto rs = run_to_end(ch, x, rate, start);
    int found = 0;
    for (const auto& r : rs) {
        const int k = int((r.slot_start_ms / 1000 - 900000) / 15);
        for (const auto& d : r.decodes) {
            const std::string want =
                "K1ABC W9XYZ " + std::string(k % 2 ? "R" : "") + "-0" + std::to_string(k + 1);
            CHECK_EQ(d.message.text, want);
            CHECK(std::fabs(d.freq_hz - (1000.0 + 100 * k)) < 0.5);
            ++found;
        }
    }
    // The stream ends 15.7 s into slot 7: finish() decodes that one.
    CHECK_EQ(found, 8);
}

TEST(gaps_and_clock_changes_start_afresh) {
    const double rate = 6000, offset = 2000, slot = 15 * 7000;
    const cvec x = make_baseband({{"CQ K1ABC FN42", 1500.0, 0.0, -10}}, rate, offset, slot - 2.0, 18.4, slot, 4);
    // A gap of 0.3 s in the first second of the slot, flagged; the index
    // counts over it.
    Channel ch(config(rate), nullptr);
    const size_t cut0 = size_t(2.5 * rate), cut1 = size_t(2.8 * rate);
    std::vector<SlotResult> rs;
    auto push = [&](size_t from, size_t to, uint32_t flags) {
        for (size_t i = from; i < to; i += 3000) {
            const size_t n = std::min<size_t>(3000, to - i);
            ch.push(&x[i], n, i, int64_t(std::llround((slot - 2.0) * 1e6 + double(i) * 1e6 / rate)),
                    i == from ? flags : 0);
            for (auto& r : ch.decode_ready())
                rs.push_back(std::move(r));
        }
    };
    push(0, cut0, 0);
    push(cut1, x.size(), kFrameSamplesLost);
    for (auto& r : ch.finish())
        rs.push_back(std::move(r));
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].missing > 0.01 && rs[0].missing < 0.3);
    CHECK(find(rs, "CQ K1ABC FN42") != nullptr);

    // A clock set back by a minute: the slot in progress is abandoned, and
    // the stream carries on at the new time.
    Channel ch2(config(rate), nullptr);
    const auto a = run_channel(ch2, x, rate, slot - 2.0);
    const cvec y = make_baseband({{"CQ W9XYZ EN37", 900.0, 0.3, -10}}, rate, offset, slot - 62.0, 18.4, slot - 60, 5);
    std::vector<SlotResult> b;
    for (size_t i = 0; i < y.size(); i += 4000) {
        const size_t n = std::min<size_t>(4000, y.size() - i);
        ch2.push(&y[i], n, x.size() + i, int64_t(std::llround((slot - 62.0) * 1e6 + double(i) * 1e6 / rate)),
                 i == 0 ? kFrameClockSet : 0);
        for (auto& r : ch2.decode_ready())
            b.push_back(std::move(r));
    }
    for (auto& r : ch2.finish())
        b.push_back(std::move(r));
    CHECK(find(b, "CQ W9XYZ EN37") != nullptr);
    CHECK(find(b, "CQ K1ABC FN42") == nullptr);
}

TEST(a_stream_may_start_at_time_zero) {
    // Files without a time stamp are decoded as the slot at 0 s UTC; the
    // 1.5 s before it lie before 1970.
    const double rate = 12000;
    const cvec x = make_baseband({{"CQ K1ABC FN42", 1000.0, 0.0, -10}}, rate, 2000, 0.0, 15.0, 0.0, 9);
    Channel ch(config(rate), nullptr);
    const auto rs = run_to_end(ch, x, rate, 0.0);
    REQUIRE(rs.size() == 1);
    CHECK_EQ(rs[0].slot_start_ms, int64_t(0));
    CHECK(rs[0].missing > 0.15 && rs[0].missing < 0.18);
    CHECK(find(rs, "CQ K1ABC FN42") != nullptr);
}

TEST(overlapping_signals_decode_after_subtraction) {
    // A weak signal 6 Hz from a strong one and 0.4 s later: found only once
    // the strong one is taken away.
    const double rate = 8000, offset = 2000, slot = 15 * 100;
    const cvec x = make_baseband({{"CQ K1ABC FN42", 1500.0, 0.0, 10}, {"W9XYZ DL1ABC JO31", 1506.25, 0.4, -12}},
                                 rate, offset, slot - 2.0, 20.0, slot, 6);
    ChannelConfig cfg;
    cfg.rate = rate;
    Channel ch(cfg, nullptr);
    const auto rs = run_to_end(ch, x, rate, slot - 2.0);
    const Decode* strong = find(rs, "CQ K1ABC FN42");
    const Decode* weak = find(rs, "W9XYZ DL1ABC JO31");
    REQUIRE(strong != nullptr);
    REQUIRE(weak != nullptr);
    CHECK(weak->pass > strong->pass);
}

TEST(hashed_calls_resolve_across_slots) {
    const double rate = 8000, offset = 2000, slot = 15 * 200;
    CallsignHashTable hashes;
    ChannelConfig cfg;
    cfg.rate = rate;
    Channel ch(cfg, &hashes);
    // Slot 1: PJ4/K1ABC calls CQ in full; slot 2: W9XYZ answers it by hash.
    cvec x = make_baseband({{"CQ PJ4/K1ABC", 1000.0, 0.0, -5}}, rate, offset, slot - 2.0, 32.0, slot, 7);
    const cvec y = make_baseband({{"<PJ4/K1ABC> W9XYZ -11", 1200.0, 0.0, -5}}, rate, offset, slot - 2.0, 32.0,
                                 slot + 15, 8, false);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += y[i];
    const auto rs = run_to_end(ch, x, rate, slot - 2.0);
    CHECK(find(rs, "CQ PJ4/K1ABC") != nullptr);
    const Decode* d = find(rs, "<PJ4/K1ABC> W9XYZ -11");
    REQUIRE(d != nullptr);
    CHECK_EQ(d->message.fields.de_call, std::string("W9XYZ"));
}

TEST(no_decodes_from_white_noise_or_silence) {
    // The long run of this check is `fern-ft8 noise`, hours at a time; this
    // is a few minutes of it for every build.
    const double rate = 8000;
    const double minutes = test::quick() ? 1.0 : 4.0;
    for (int depth : {1, 2, 3}) {
        ChannelConfig cfg;
        cfg.rate = rate;
        cfg.depth = depth;
        Channel ch(cfg, nullptr);
        const cvec x = make_baseband({}, rate, 2000, 86400.0, minutes * 60, 0, 100 + unsigned(depth));
        const auto rs = run_to_end(ch, x, rate, 86400.0);
        CHECK(rs.size() >= size_t(minutes * 4) - 1);
        for (const auto& r : rs)
            for (const auto& d : r.decodes)
                std::fprintf(stderr, "    false decode at depth %d: %s\n", depth, d.message.text.c_str());
        CHECK_EQ(count_decodes(rs), size_t(0));
    }
    Channel ch(config(rate), nullptr);
    const cvec zero(size_t(40 * rate));
    CHECK_EQ(count_decodes(run_to_end(ch, zero, rate, 86400.0)), size_t(0));
}

TEST(golden_decodes_of_real_recordings) {
    struct Golden {
        const char* name;
        size_t at_least;  // of the reference decodes, at depth 2
    } const files[] = {
        {"20m_busy_test_01", 26},
        {"191111_110130", 4},
        {"websdr_test4", 23},
    };
    for (const Golden& g : files) {
        const std::string base = std::string("tests/data/") + g.name;
        const Audio a = read_wav(base + ".wav");
        std::set<std::string> want;
        std::ifstream in(base + ".reference.txt");
        for (std::string line; std::getline(in, line);)
            if (!line.empty())
                want.insert(line);
        REQUIRE(!want.empty());
        // Real audio to complex baseband around 2000 Hz, as the CLI does.
        cvec x(a.samples.size());
        for (size_t i = 0; i < x.size(); ++i) {
            const double ph = -2.0 * M_PI * 2000.0 * double(i) / a.rate;
            x[i] = std::complex<float>(float(a.samples[i] * std::cos(ph)), float(a.samples[i] * std::sin(ph)));
        }
        ChannelConfig cfg;
        cfg.rate = a.rate;
        cfg.width_hz = 4400;
        cfg.min_freq_hz = 200;
        cfg.max_freq_hz = 4000;
        Channel ch(cfg, nullptr);
        const auto rs = run_to_end(ch, x, a.rate, 15 * 1000.0);
        // Fern-FT8 finds some real signals WSJT-X does not; those cannot be
        // told from false decodes here, so a few are allowed and shown.
        size_t matched = 0, others = 0;
        for (const auto& r : rs)
            for (const auto& d : r.decodes) {
                if (want.count(d.message.text)) {
                    ++matched;
                } else if (d.quality() != std::string("low")) {
                    ++others;
                    std::fprintf(stderr, "    %s: not in WSJT-X's list: %s\n", g.name, d.message.text.c_str());
                }
            }
        CHECK(others <= 2);
        std::fprintf(stderr, "    %s: %zu of the %zu reference decodes\n", g.name, matched, want.size());
        CHECK(matched >= g.at_least);
    }
}

TEST(decodes_do_not_depend_on_the_simd_level) {
    const SimdLevel saved = simd_level();
    for (const char* name : {"20m_busy_test_01", "websdr_test4"}) {
        const Audio a = read_wav(std::string("tests/data/") + name + ".wav");
        cvec x(a.samples.size());
        for (size_t i = 0; i < x.size(); ++i) {
            const double ph = -2.0 * M_PI * 2000.0 * double(i) / a.rate;
            x[i] = std::complex<float>(float(a.samples[i] * std::cos(ph)), float(a.samples[i] * std::sin(ph)));
        }
        std::vector<std::string> runs;
        for (SimdLevel level : {SimdLevel::Scalar, SimdLevel::Baseline, SimdLevel::Avx2}) {
            set_simd_level(level);
            ChannelConfig cfg = config(a.rate);
            cfg.width_hz = 4400;
            cfg.min_freq_hz = 200;
            cfg.max_freq_hz = 4000;
            cfg.depth = 3;
            Channel ch(cfg, nullptr);
            std::string out;
            char buf[160];
            for (const auto& r : run_to_end(ch, x, a.rate, 15 * 1000.0))
                for (const auto& d : r.decodes) {
                    std::snprintf(buf, sizeof buf, "%s %.6f %.6f %d %s\n", d.message.text.c_str(), d.freq_hz, d.dt,
                                  d.snr_db, d.quality());
                    out += buf;
                }
            runs.push_back(out);
        }
        CHECK(!runs[0].empty());
        CHECK(runs[0] == runs[1]);
        CHECK(runs[0] == runs[2]);
    }
    set_simd_level(saved);
}
