// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// FT4: scrambling, CRC, LDPC and tones against [QEX] and WSJT-X's ft4code,
// the waveform, and whole decodes through a Channel.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fern_ft8.h"
#include "ft4.h"
#include "gfsk.h"
#include "message.h"
#include "simd.h"
#include "test.h"
#include "vectors.h"

using namespace fern::ft8;

namespace {

// Calls ft4code knew by hash when it printed a vector, taken from the text.
void learn_bracketed(const std::string& text, CallsignHashTable& table) {
    for (size_t pos = text.find('<'); pos != std::string::npos; pos = text.find('<', pos + 1)) {
        const size_t end = text.find('>', pos);
        if (end == std::string::npos)
            break;
        if (text.compare(pos + 1, end - pos - 1, "...") != 0)
            table.remember(text.substr(pos + 1, end - pos - 1), 0);
    }
}

std::string bit_string(const Payload& p) {
    std::string s;
    for (int i = 0; i < kPayloadBits; ++i)
        s += char('0' + payload_bit(p, i));
    return s;
}

}  // namespace

TEST(ft4_scrambling_vector_is_the_papers_and_ft4codes) {
    // [QEX] Appendix A, as printed there.
    const std::string paper = "0100101001011110100010"
                              "0110110100101100001000"
                              "1010011110010101010110"
                              "11111000101";
    CHECK_EQ(bit_string(ft4::kScrambleVector), paper);
    CHECK_EQ(int(ft4::kScrambleVector[9] & 7), 0);
    const auto vectors = test::ft4code_vectors();
    REQUIRE(vectors.size() > 50);
    for (const auto& v : vectors) {
        CHECK_EQ(bit_string(ft4::scramble(v.payload())), v.scrambled);
        CHECK(ft4::scramble(ft4::scramble(v.payload())) == v.payload());
    }
}

TEST(ft4_crc_parity_and_tones_match_ft4code) {
    for (const auto& v : test::ft4code_vectors()) {
        // The message bits are FT8's (test_message.cpp checks them all);
        // where ft4code cut the text to fit there is nothing to compare.
        if (v.message == v.decoded) {
            const auto packed = pack_message(v.message);
            REQUIRE(packed.has_value());
            CHECK_EQ(bit_string(*packed), v.bits);
        }
        const Codeword cw = ft4::encode_codeword(v.payload());
        std::string crc, parity, tones;
        for (int i = 0; i < kCrcBits; ++i)
            crc += char('0' + cw[size_t(kPayloadBits + i)]);
        for (int i = 0; i < kLdpcM; ++i)
            parity += char('0' + cw[size_t(kLdpcK + i)]);
        for (uint8_t t : ft4::wave_tones(ft4::tones_of(cw)))
            tones += char('0' + t);
        CHECK_EQ(crc, v.crc);
        CHECK_EQ(parity, v.parity);
        CHECK_EQ(tones, v.tones);
        CHECK(parity_ok(cw));
        CHECK(crc_ok(cw));
        // A receiver unscrambles what it decoded.
        CHECK(ft4::payload_of(cw) == v.payload());
        CallsignHashTable table;
        learn_bracketed(v.decoded, table);
        const auto msg = unpack_message(ft4::payload_of(cw), &table, 0, UnpackOptions{true});
        REQUIRE(msg.has_value());
        CHECK_EQ(msg->text, v.decoded);
        CHECK_EQ(msg->i3, v.i3);
        CHECK_EQ(msg->n3, v.n3);
    }
}

TEST(ft4_tone_map_and_costas_arrays_are_the_papers) {
    // [QEX] Table 3, columns 1 and 3: tone 0..3 carries 00, 01, 11, 10.
    const uint8_t table3[4] = {0b00, 0b01, 0b11, 0b10};
    for (int tone = 0; tone < 4; ++tone) {
        CHECK_EQ(int(ft4::kToneToBits[tone]), int(table3[tone]));
        CHECK_EQ(int(ft4::kBitsToTone[table3[tone]]), tone);
    }
    // Each array is a permutation, and they all differ.
    for (int a = 0; a < 4; ++a) {
        int seen = 0;
        for (int k = 0; k < 4; ++k)
            seen |= 1 << ft4::kCostas[a][k];
        CHECK_EQ(seen, 15);
    }
    // Data symbols fill the 87 places between them.
    std::vector<int> used(ft4::kSymbolCount, 0);
    for (int d = 0; d < ft4::kDataSymbolCount; ++d)
        used[size_t(ft4::data_symbol_index(d))]++;
    for (int b = 0; b < 4; ++b)
        for (int k = 0; k < 4; ++k)
            used[size_t(ft4::kCostasStart[b] + k)] += 10;
    for (int s = 0; s < ft4::kSymbolCount; ++s)
        CHECK(used[size_t(s)] == 1 || used[size_t(s)] == 10);
}

TEST(ft4_waveform_holds_each_tone_for_a_symbol_between_its_ramps) {
    const auto p = pack_message("CQ K1ABC FN42");
    REQUIRE(p.has_value());
    const ft4::Tones tones = ft4::tones_of(ft4::encode_codeword(*p));
    const ft4::WaveTones wave = ft4::wave_tones(tones);
    const double rate = 12000;
    std::vector<std::complex<float>> w(size_t(rate * 5.2));
    add_ft4_waveform(tones, rate, 1000.0, 0.0, 1.0f, w.data(), w.size());
    // 576 samples a symbol at 12 kHz; the middle of each symbol is at its
    // tone, ramps included, with BT = 1 within a hertz.
    for (int s = 0; s < ft4::kWaveSymbolCount; ++s) {
        const size_t mid = size_t((s + 0.5) * ft4::kSymbolSeconds * rate);
        const double dphi = std::arg(w[mid + 1] * std::conj(w[mid]));
        const double f = dphi * rate / (2 * M_PI);
        CHECK(std::fabs(f - (1000.0 + ft4::kToneSpacingHz * wave[size_t(s)])) < 1.0);
    }
    // The ramps take a whole symbol each: half amplitude in the middle of
    // the first and last, full from the second to the 104th, silence after.
    CHECK(std::fabs(std::abs(w[288]) - 0.5f) < 1e-3f);
    CHECK(std::fabs(std::abs(w[size_t(104.5 * 576)]) - 0.5f) < 1e-3f);
    CHECK(std::fabs(std::abs(w[577]) - 1.0f) < 1e-4f);
    CHECK(std::fabs(std::abs(w[size_t(104 * 576 - 1)]) - 1.0f) < 1e-4f);
    CHECK(std::abs(w[0]) < 1e-3f);
    CHECK(std::abs(w[size_t(105 * 576 + 1)]) == 0.0f);
}

TEST(ft4_pulse_has_unit_area) {
    double area = 0;
    for (int i = -3000; i < 3000; ++i)
        area += gfsk_pulse((i + 0.5) / 1000.0, kFt4BT) / 1000.0;
    CHECK(std::fabs(area - 1.0) < 1e-6);
}

namespace {

using cvec = std::vector<std::complex<float>>;

struct Tx {
    std::string text;
    double audio_hz;  // tone 0 above the dial
    double dt;
    double snr_db;
};

// Complex baseband at `rate` with baseband 0 Hz at audio `offset`, covering
// `seconds` from UTC `start_s`, with FT4 transmissions in the slot starting
// at slot_s and white noise of unit power per sample, so that SNR is per
// 2500 Hz.
cvec make_ft4(const std::vector<Tx>& txs, double rate, double offset, double start_s, double seconds, double slot_s,
              unsigned seed, bool noise = true) {
    const size_t n = size_t(std::llround(seconds * rate));
    cvec x(n);
    for (const Tx& t : txs) {
        const auto p = pack_message(t.text);
        if (!p)
            continue;
        const double amp = std::sqrt(std::pow(10.0, t.snr_db / 10.0) * 2500.0 / rate);
        add_ft4_waveform(ft4::tones_of(ft4::encode_codeword(*p)), rate, t.audio_hz - offset,
                         slot_s - start_s + ft4::kStartSeconds + t.dt, float(amp), x.data(), n);
    }
    if (noise) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> g(0.0f, float(std::sqrt(0.5)));
        for (auto& v : x)
            v += std::complex<float>(g(rng), g(rng));
    }
    return x;
}

ChannelConfig ft4_config(double rate, int depth = 2) {
    ChannelConfig c;
    c.mode = Mode::Ft4;
    c.rate = rate;
    c.depth = depth;
    return c;
}

std::vector<SlotResult> run_to_end(Channel& ch, const cvec& x, double rate, double start_s, size_t frame = 3000) {
    std::vector<SlotResult> out;
    for (size_t i = 0; i < x.size(); i += frame) {
        const size_t n = std::min(frame, x.size() - i);
        ch.push(&x[i], n, i, int64_t(std::llround(start_s * 1e6 + double(i) * 1e6 / rate)), 0);
        for (auto& r : ch.decode_ready())
            out.push_back(std::move(r));
    }
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

}  // namespace

TEST(ft4_decodes_transmissions_with_their_frequency_time_and_snr) {
    const double rate = 8000, offset = 2000;
    const double slot = 7.5 * 4001;  // an odd slot: FT4 slots are 7.5 s apart
    const std::vector<Tx> txs = {
        {"CQ K1ABC FN42", 400.0, 0.0, -10},
        {"K1ABC W9XYZ -12", 1234.5, 0.6, -13},
        {"W9XYZ K1ABC R-12", 2600.3, -0.6, -6},
        {"CQ DX DL1ABC JO31", 3500.0, 0.9, 0},
        {"DL1ABC G4ABC RR73", 1800.0, -0.9, 10},
    };
    const cvec x = make_ft4(txs, rate, offset, slot - 1.0, 8.4, slot, 1);
    CallsignHashTable hashes;
    Channel ch(ft4_config(rate), &hashes);
    const auto rs = run_to_end(ch, x, rate, slot - 1.0);
    REQUIRE(rs.size() == 1);
    CHECK_EQ(rs[0].slot_start_ms, int64_t(slot * 1000));
    CHECK_EQ(count_decodes(rs), txs.size());
    for (const Tx& t : txs) {
        const Decode* d = find(rs, t.text);
        REQUIRE(d != nullptr || (std::fprintf(stderr, "    missing %s\n", t.text.c_str()), false));
        CHECK(d->mode == Mode::Ft4);
        CHECK(std::fabs(d->freq_hz - t.audio_hz) < 0.5);
        CHECK(std::fabs(d->dt - t.dt) < 0.01);
        CHECK(std::abs(d->snr_db - int(t.snr_db)) <= 2);
        CHECK_EQ(std::string(d->quality()), std::string("bp"));
    }
    const Decode* cq = find(rs, "CQ DX DL1ABC JO31");
    REQUIRE(cq != nullptr);
    CHECK_EQ(cq->message.fields.de_call, std::string("DL1ABC"));
    CHECK_EQ(cq->message.fields.grid, std::string("JO31"));
}

TEST(ft4_round_trips_every_message_type) {
    // One slot per type, each at -12 dB, through a 12 kHz channel.
    const char* const messages[] = {
        "TNX BOB 73 GL", "K1ABC RR73; W9XYZ <KH1/KH7Z> -08", "K1ABC W9XYZ 6A WI", "W9XYZ K1ABC R 17B EMA",
        "123456789ABCDEF012", "CQ K1ABC FN42", "K1ABC W9XYZ R-09", "CQ G4ABC/P IO91", "K1ABC W9XYZ 579 WI",
        "CQ PJ4/K1ABC", "<W9XYZ> PJ4/K1ABC RRR",
    };
    const double rate = 12000;
    double slot = 7.5 * 20000;
    for (const char* text : messages) {
        CallsignHashTable hashes;
        // The receiver has heard the hashed calls before.
        hashes.remember("KH1/KH7Z", int64_t(slot));
        hashes.remember("W9XYZ", int64_t(slot));
        const cvec x = make_ft4({{text, 1500.0, 0.2, -12}}, rate, 2000, slot - 1.0, 8.4, slot, 2);
        ChannelConfig cfg = ft4_config(rate);
        cfg.unpack.contest_forms = true;
        Channel ch(cfg, &hashes);
        const auto rs = run_to_end(ch, x, rate, slot - 1.0);
        if (!find(rs, text))
            std::fprintf(stderr, "    missing %s\n", text);
        CHECK(find(rs, text) != nullptr);
        CHECK_EQ(count_decodes(rs), size_t(1));
        slot += 7.5;
    }
}

TEST(ft4_decodes_at_odd_and_low_rates_and_offsets) {
    // Rates that are not whole numbers, and the edges of the DT range.
    const double rates[] = {5859.375, 7031.25, 4000.0, 12000.0, 11718.75};
    const double dts[] = {-0.95, -0.3, 0.0, 0.45, 0.95};
    for (int i = 0; i < 5; ++i) {
        const double rate = rates[i];
        const double slot = 7.5 * (3001 + i);
        const std::vector<Tx> txs = {{"CQ PJ4/K1ABC", 1500.0, dts[i], -12}, {"K1ABC W9XYZ RR73", 700.0, -dts[i], -12}};
        const cvec x = make_ft4(txs, rate, 2000, slot - 1.3, 9.0, slot, 3 + unsigned(i));
        Channel ch(ft4_config(rate), nullptr);
        const auto rs = run_to_end(ch, x, rate, slot - 1.3);
        for (const Tx& t : txs) {
            const Decode* d = find(rs, t.text);
            if (!d)
                std::fprintf(stderr, "    rate %.3f DT %.2f: missing %s\n", rate, t.dt, t.text.c_str());
            CHECK(d != nullptr);
            if (d)
                CHECK(std::fabs(d->dt - t.dt) < 0.01);
        }
    }
}

TEST(ft4_slots_follow_utc_across_a_stream) {
    // A minute in one stream, a transmission in each 7.5 s slot.
    const double rate = 8000, start = 900000.0 - 3.3;
    cvec x(size_t(64 * rate));
    for (int k = 0; k < 8; ++k) {
        const double slot = 900000.0 + 7.5 * k;
        const std::string text = "K1ABC W9XYZ " + std::string(k % 2 ? "R" : "") + "-0" + std::to_string(k + 1);
        const cvec s = make_ft4({{text, 1000.0 + 100 * k, 0.1 * k - 0.3, -8}}, rate, 2000, start, 64, slot, 0, false);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += s[i];
    }
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.0f, float(std::sqrt(0.5)));
    for (auto& v : x)
        v += std::complex<float>(g(rng), g(rng));
    Channel ch(ft4_config(rate), nullptr);
    const auto rs = run_to_end(ch, x, rate, start);
    int found = 0;
    for (const auto& r : rs) {
        CHECK_EQ(r.slot_start_ms % 7500, int64_t(0));
        const int k = int((r.slot_start_ms - 900000000) / 7500);
        for (const auto& d : r.decodes) {
            const std::string want = "K1ABC W9XYZ " + std::string(k % 2 ? "R" : "") + "-0" + std::to_string(k + 1);
            CHECK_EQ(d.message.text, want);
            CHECK(std::fabs(d.freq_hz - (1000.0 + 100 * k)) < 0.5);
            ++found;
        }
    }
    CHECK_EQ(found, 8);
}

TEST(ft4_overlapping_signals_decode_after_subtraction) {
    // A weak signal 20 Hz from a strong one and 0.2 s later: found once the
    // strong one is taken away.
    const double rate = 8000, slot = 7.5 * 100;
    const cvec x = make_ft4({{"CQ K1ABC FN42", 1500.0, 0.0, 10}, {"W9XYZ DL1ABC JO31", 1520.0, 0.2, -10}}, rate,
                            2000, slot - 1.0, 9.0, slot, 6);
    Channel ch(ft4_config(rate), nullptr);
    const auto rs = run_to_end(ch, x, rate, slot - 1.0);
    const Decode* strong = find(rs, "CQ K1ABC FN42");
    const Decode* weak = find(rs, "W9XYZ DL1ABC JO31");
    REQUIRE(strong != nullptr);
    REQUIRE(weak != nullptr);
    CHECK(weak->pass > strong->pass);
}

TEST(ft4_subtraction_leaves_little_of_a_strong_signal) {
    // The residual of a +30 dB signal (no noise) after one pass.
    const double rate = ft4::kInternalRate;
    cvec x(size_t(ft4::kSlotSamples));
    const auto p = pack_message("CQ K1ABC FN42");
    REQUIRE(p.has_value());
    add_ft4_waveform(ft4::tones_of(ft4::encode_codeword(*p)), rate, 300.25,
                     ft4::kSlotLeadSeconds + ft4::kStartSeconds + 0.137, 1.0f, x.data(), x.size());
    double before = 0;
    for (const auto& v : x)
        before += std::norm(v);
    ft4::SlotDecoder dec;
    DecodeSettings settings;
    settings.depth = 1;
    const auto ds = dec.decode(x, 0, x.size(), 2000.0, 0, settings, nullptr);
    REQUIRE(ds.size() == 1);
    double after = 0;
    for (const auto& v : x)
        after += std::norm(v);
    const double db = 10 * std::log10(after / before);
    std::fprintf(stderr, "    residual %.1f dB\n", db);
    CHECK(db < -40);
}

TEST(ft4_weak_signals_decode_near_the_threshold) {
    // At -15 dB nearly all decode, at -16.5 dB most do; WSJT-X's own
    // threshold is near -17.5 dB ([QEX] Table 5).
    const int trials = test::quick() ? 6 : 24;
    const double rate = 8000;
    for (const double snr : {-15.0, -16.5}) {
        int ok = 0;
        for (int i = 0; i < trials; ++i) {
            const double slot = 7.5 * (5000 + i);
            const std::string text = std::string(i % 2 ? "K1ABC" : "DL1ABC") + " W9XYZ " + (i % 3 ? "-1" : "R-0") +
                                     std::to_string(i % 10);
            const cvec x = make_ft4({{text, 400.0 + 97.0 * i, -0.4 + 0.05 * i, snr}}, rate, 2000, slot - 1.0, 8.4,
                                    slot, 100 + unsigned(i) + unsigned(snr * -10));
            Channel ch(ft4_config(rate), nullptr);
            ok += find(run_to_end(ch, x, rate, slot - 1.0), text) != nullptr;
        }
        std::fprintf(stderr, "    %.1f dB: %d of %d\n", snr, ok, trials);
        CHECK(ok >= trials * (snr > -16 ? 90 : 60) / 100);
    }
}

TEST(ft4_no_decodes_from_white_noise_or_silence) {
    // The long run is `fern-ft8 noise --mode ft4`; this is a few minutes.
    const double rate = 8000;
    const double minutes = test::quick() ? 1.0 : 4.0;
    for (int depth : {1, 2, 3}) {
        Channel ch(ft4_config(rate, depth), nullptr);
        const cvec x = make_ft4({}, rate, 2000, 86400.0, minutes * 60, 0, 200 + unsigned(depth));
        const auto rs = run_to_end(ch, x, rate, 86400.0);
        CHECK(rs.size() >= size_t(minutes * 8) - 1);
        for (const auto& r : rs)
            for (const auto& d : r.decodes)
                std::fprintf(stderr, "    false decode at depth %d: %s\n", depth, d.message.text.c_str());
        CHECK_EQ(count_decodes(rs), size_t(0));
    }
    Channel ch(ft4_config(rate), nullptr);
    const cvec zero(size_t(20 * rate));
    CHECK_EQ(count_decodes(run_to_end(ch, zero, rate, 86400.0)), size_t(0));
    // A steady carrier demodulates to the all-zero codeword, which passes
    // the CRC; it is not a message.
    cvec carrier = make_ft4({}, rate, 2000, 86400.0, 20.0, 0, 9);
    for (size_t i = 0; i < carrier.size(); ++i)
        carrier[i] += std::polar(3.0f, float(2 * M_PI * -500.0 * double(i) / rate));
    Channel ch2(ft4_config(rate, 3), nullptr);
    CHECK_EQ(count_decodes(run_to_end(ch2, carrier, rate, 86400.0)), size_t(0));
}

TEST(ft4_a_busy_slot_decodes) {
    // 25 transmissions between 200 and 3000 Hz, -14 to +10 dB, overlapping
    // in time and some in frequency.
    const double rate = 12000, slot = 7.5 * 7000;
    std::vector<Tx> txs;
    std::mt19937 rng(17);
    const char* calls[] = {"K1ABC", "W9XYZ", "DL1ABC", "JA1XYZ", "G4ABC", "VK2ABC", "PY2XYZ", "EA3ABC", "OH2XYZ"};
    for (int i = 0; i < 25; ++i) {
        const std::string text = std::string(calls[i % 9]) + " " + calls[(i + 1 + i / 9) % 9] + " " +
                                 (i % 2 ? "R" : "") + "-" + std::to_string(10 + i);
        txs.push_back({text, 200.0 + 2800.0 * double(rng() % 1000) / 1000.0, -0.5 + double(rng() % 1000) / 1000.0,
                       -14.0 + 24.0 * double(rng() % 1000) / 1000.0});
    }
    const cvec x = make_ft4(txs, rate, 2000, slot - 1.0, 8.4, slot, 18);
    ChannelConfig cfg = ft4_config(rate);
    cfg.width_hz = 4400;
    cfg.min_freq_hz = 200;
    cfg.max_freq_hz = 4000;
    Channel ch(cfg, nullptr);
    const auto rs = run_to_end(ch, x, rate, slot - 1.0);
    size_t found = 0;
    for (const Tx& t : txs)
        found += find(rs, t.text) != nullptr;
    std::fprintf(stderr, "    %zu of %zu, %zu decodes\n", found, txs.size(), count_decodes(rs));
    CHECK(found >= txs.size() - 2);
    CHECK_EQ(count_decodes(rs), found);
}

TEST(ft4_decodes_do_not_depend_on_the_simd_level) {
    const double rate = 12000, slot = 7.5 * 7100;
    std::vector<Tx> txs;
    for (int i = 0; i < 12; ++i)
        txs.push_back({std::string(i % 2 ? "K1ABC" : "W9XYZ") + " DL1ABC " + std::to_string(-20 + i), 300.0 + 230.0 * i,
                       -0.4 + 0.1 * i, -17.0 + 2.0 * i});
    const cvec x = make_ft4(txs, rate, 2000, slot - 1.0, 8.4, slot, 19);
    const SimdLevel saved = simd_level();
    std::vector<std::string> runs;
    for (SimdLevel level : {SimdLevel::Scalar, SimdLevel::Baseline, SimdLevel::Avx2}) {
        set_simd_level(level);
        Channel ch(ft4_config(rate, 3), nullptr);
        std::string out;
        char buf[160];
        for (const auto& r : run_to_end(ch, x, rate, slot - 1.0))
            for (const auto& d : r.decodes) {
                std::snprintf(buf, sizeof buf, "%s %.6f %.6f %d %s\n", d.message.text.c_str(), d.freq_hz, d.dt,
                              d.snr_db, d.quality());
                out += buf;
            }
        runs.push_back(out);
    }
    set_simd_level(saved);
    CHECK(!runs[0].empty());
    CHECK(runs[0] == runs[1]);
    CHECK(runs[0] == runs[2]);
}

TEST(ft8_and_ft4_channels_decode_at_once_with_one_hash_table) {
    // An FT4 channel hears PJ4/K1ABC call CQ in full; an FT8 channel, on
    // another thread at the same time, decodes its own slot; then a second
    // FT4 slot answers PJ4/K1ABC by hash, resolved through the shared table.
    const double rate = 8000, slot = 15.0 * 9000;
    CallsignHashTable hashes;
    const cvec a = make_ft4({{"CQ PJ4/K1ABC", 1000.0, 0.0, -5}}, rate, 2000, slot - 1.0, 8.4, slot, 30);
    const cvec b = make_ft4({{"<PJ4/K1ABC> W9XYZ -11", 1500.0, 0.0, -5}}, rate, 2000, slot + 6.5, 8.4, slot + 7.5, 31);
    // The FT8 channel's slot, made the same way as test_decoder.cpp does.
    cvec c(size_t(18.4 * rate));
    {
        const auto p = pack_message("CQ DL1ABC JO31");
        REQUIRE(p.has_value());
        add_ft8_waveform(tones_of(encode_codeword(*p)), rate, 700.0 - 2000.0, 2.0 + kStartSeconds,
                         float(std::sqrt(std::pow(10.0, -0.5) * 2500.0 / rate)), c.data(), c.size());
        std::mt19937 rng(32);
        std::normal_distribution<float> g(0.0f, float(std::sqrt(0.5)));
        for (auto& v : c)
            v += std::complex<float>(g(rng), g(rng));
    }
    std::vector<SlotResult> ra, rb, rc;
    std::thread t8([&] {
        ChannelConfig cfg;
        cfg.rate = rate;
        Channel ch(cfg, &hashes);
        rc = run_to_end(ch, c, rate, slot - 2.0);
    });
    std::thread t4([&] {
        Channel ch(ft4_config(rate), &hashes);
        ra = run_to_end(ch, a, rate, slot - 1.0);
    });
    t4.join();
    t8.join();
    Channel ch(ft4_config(rate), &hashes);
    rb = run_to_end(ch, b, rate, slot + 6.5);
    CHECK(find(ra, "CQ PJ4/K1ABC") != nullptr);
    CHECK(find(rb, "<PJ4/K1ABC> W9XYZ -11") != nullptr);
    CHECK(find(rc, "CQ DL1ABC JO31") != nullptr);
}

namespace {

// Runs build/fern-ft8 with `args` and returns what it printed.
std::string run_cli(const std::string& args) {
    std::string out;
    FILE* p = popen(("build/fern-ft8 " + args + " 2>&1").c_str(), "r");
    if (!p)
        return out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p))
        out += buf;
    pclose(p);
    return out;
}

}  // namespace

TEST(command_line_writes_and_decodes_ft4_slots) {
    const std::string wav = "build/test/ft4-cli.wav";
    // A message, as jt9 prints FT4 decodes.
    CHECK(run_cli("encode \"K1ABC W9XYZ R-07\" --mode ft4 --freq 1234 --dt 0.3 --snr -10 -o " + wav).empty());
    CHECK_HAS(run_cli("decode --mode ft4 " + wav), " 0.3 1234 +  K1ABC W9XYZ R-07");
    // FT4 audio is not FT8.
    CHECK(run_cli("decode " + wav).find("K1ABC") == std::string::npos);
    // ft4code's tones, ramps included, sent as they are.
    const auto vectors = test::ft4code_vectors();
    REQUIRE(!vectors.empty());
    const auto& v = vectors[size_t(6)];
    CHECK(run_cli("encode --mode ft4 --tones " + v.tones + " --snr -10 -o " + wav).empty());
    CHECK_HAS(run_cli("decode --mode ft4 " + wav), "+  " + v.decoded);
    // Over two fading paths at a good SNR.
    CHECK(run_cli("encode \"CQ DL1ABC JO31\" --mode ft4 --fading 1,2 --snr 0 --seed 3 -o " + wav).empty());
    CHECK_HAS(run_cli("decode --mode ft4 " + wav), "+  CQ DL1ABC JO31");
    CHECK_HAS(run_cli("encode --mode ft4 --tones 0123 -o " + wav), "--tones wants");
    std::remove(wav.c_str());
}
