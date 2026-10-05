// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// FT4: scrambling, CRC, LDPC and tones against [QEX] and WSJT-X's ft4code,
// the waveform, and whole decodes through a Channel.
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "ft4.h"
#include "gfsk.h"
#include "message.h"
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
