// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// CRC, LDPC code and tone mapping against the protocol's own examples and
// against WSJT-X's ft8code.
#include <random>
#include <string>

#include "protocol.h"
#include "test.h"
#include "vectors.h"

using namespace fern::ft8;

namespace {

Payload payload_from_bits(const std::string& bits) {
    Payload p{};
    for (size_t i = 0; i < bits.size(); ++i)
        set_payload_bit(p, int(i), bits[i] == '1');
    return p;
}

std::string crc_bits(uint16_t crc) {
    std::string s;
    for (int i = 13; i >= 0; --i)
        s += char('0' + ((crc >> i) & 1));
    return s;
}

}  // namespace

TEST(crc14_matches_the_protocol_example) {
    // protocol/gen_crc14.f90's usage example, output of that program.
    const Payload p = payload_from_bits("00000000000000000000000000100000010011011111110011011100100010100001010000001");
    CHECK_EQ(crc14(p), uint16_t(5497));
}

TEST(crc14_matches_ft8code) {
    const auto vectors = test::ft8code_vectors();
    REQUIRE(vectors.size() > 50);
    for (const auto& v : vectors)
        CHECK_EQ(crc_bits(crc14(v.payload())), v.crc);
}

TEST(ldpc_parity_and_tones_match_ft8code) {
    for (const auto& v : test::ft8code_vectors()) {
        const Codeword cw = encode_codeword(v.payload());
        std::string parity, tones;
        for (int i = 0; i < kLdpcM; ++i)
            parity += char('0' + cw[kLdpcK + i]);
        for (uint8_t t : tones_of(cw))
            tones += char('0' + t);
        CHECK_EQ(parity, v.parity);
        CHECK_EQ(tones, v.tones);
        CHECK(parity_ok(cw));
        CHECK(crc_ok(cw));
    }
}

TEST(every_codeword_satisfies_the_parity_checks) {
    // generator.dat and parity.dat describe the same code: every encoded
    // word passes all 83 checks, and flipping any one bit fails some.
    std::mt19937 rng(1);
    for (int trial = 0; trial < 200; ++trial) {
        Payload p{};
        for (int i = 0; i < kPayloadBits; ++i)
            set_payload_bit(p, i, int(rng() & 1));
        Codeword cw = encode_codeword(p);
        CHECK(parity_ok(cw));
        const int bit = int(rng() % kLdpcN);
        cw[size_t(bit)] ^= 1;
        CHECK_EQ(parity_failures(cw), 3);  // column weight 3
    }
}

TEST(ldpc_graph_has_the_published_degrees) {
    const LdpcGraph& g = ldpc_graph();
    int six = 0, seven = 0;
    for (int c = 0; c < kLdpcM; ++c) {
        six += g.check_degree[c] == 6;
        seven += g.check_degree[c] == 7;
    }
    CHECK_EQ(six + seven, kLdpcM);
    CHECK_EQ(6 * six + 7 * seven, 3 * kLdpcN);
}

TEST(crc_detects_corruption) {
    std::mt19937 rng(2);
    for (int trial = 0; trial < 100; ++trial) {
        Payload p{};
        for (int i = 0; i < kPayloadBits; ++i)
            set_payload_bit(p, i, int(rng() & 1));
        Codeword cw = encode_codeword(p);
        CHECK(crc_ok(cw));
        cw[size_t(rng() % kLdpcK)] ^= 1;
        CHECK(!crc_ok(cw));
    }
}

TEST(tone_map_is_the_gray_code_of_table_3) {
    for (int bits = 0; bits < 8; ++bits)
        CHECK_EQ(int(kToneToBits[kBitsToTone[bits]]), bits);
    // Adjacent tones differ in one bit.
    for (int t = 0; t < 7; ++t)
        CHECK_EQ(__builtin_popcount(kToneToBits[t] ^ kToneToBits[t + 1]), 1);
}
