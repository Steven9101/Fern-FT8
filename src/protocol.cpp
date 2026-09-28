// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol.h"

namespace fern::ft8 {

namespace {

LdpcGraph build_graph() {
    LdpcGraph g{};
    for (auto& row : g.check_bits)
        for (auto& b : row)
            b = -1;
    for (int bit = 0; bit < kLdpcN; ++bit) {
        for (int k = 0; k < 3; ++k) {
            const int check = kLdpcColumnChecks[bit][k];
            g.bit_checks[bit][k] = uint8_t(check);
            g.check_bits[check][g.check_degree[check]++] = int16_t(bit);
        }
    }
    return g;
}

int generator_bit(int row, int col) { return (kLdpcGenerator[row][col >> 3] >> (7 - (col & 7))) & 1; }

}  // namespace

const LdpcGraph& ldpc_graph() {
    static const LdpcGraph graph = build_graph();
    return graph;
}

uint16_t crc14(const Payload& payload) {
    // Long division of the 82-bit word by x^14 + 0x2757 (0x6757 with its
    // leading term), most significant bit first; the 14-bit remainder is the
    // CRC. Feeding each bit into the top of the register is the same division
    // as protocol/gen_crc14.f90's with the 14 augmenting zeros folded in.
    uint32_t reg = 0;
    for (int i = 0; i < 82; ++i) {
        const uint32_t in = i < kPayloadBits ? uint32_t(payload_bit(payload, i)) : 0u;
        const uint32_t top = ((reg >> 13) & 1u) ^ in;
        reg = (reg << 1) & 0x3fffu;
        if (top)
            reg ^= 0x2757u;
    }
    return uint16_t(reg);
}

Codeword encode_codeword(const Payload& payload) {
    Codeword cw{};
    for (int i = 0; i < kPayloadBits; ++i)
        cw[i] = uint8_t(payload_bit(payload, i));
    const uint16_t crc = crc14(payload);
    for (int i = 0; i < kCrcBits; ++i)
        cw[kPayloadBits + i] = uint8_t((crc >> (kCrcBits - 1 - i)) & 1);
    for (int row = 0; row < kLdpcM; ++row) {
        int sum = 0;
        for (int col = 0; col < kLdpcK; ++col)
            sum ^= generator_bit(row, col) & cw[col];
        cw[kLdpcK + row] = uint8_t(sum);
    }
    return cw;
}

int parity_failures(const Codeword& cw) {
    const LdpcGraph& g = ldpc_graph();
    int bad = 0;
    for (int check = 0; check < kLdpcM; ++check) {
        int sum = 0;
        for (int k = 0; k < g.check_degree[check]; ++k)
            sum ^= cw[g.check_bits[check][k]];
        bad += sum;
    }
    return bad;
}

bool parity_ok(const Codeword& cw) { return parity_failures(cw) == 0; }

Payload payload_of(const Codeword& cw) {
    Payload p{};
    for (int i = 0; i < kPayloadBits; ++i)
        set_payload_bit(p, i, cw[i]);
    return p;
}

bool crc_ok(const Codeword& cw) {
    const uint16_t crc = crc14(payload_of(cw));
    for (int i = 0; i < kCrcBits; ++i)
        if (cw[kPayloadBits + i] != ((crc >> (kCrcBits - 1 - i)) & 1))
            return false;
    return true;
}

Tones tones_of(const Codeword& cw) {
    Tones t{};
    for (int block = 0; block < 3; ++block)
        for (int k = 0; k < 7; ++k)
            t[kCostasStart[block] + k] = uint8_t(kCostas[k]);
    for (int d = 0; d < kDataSymbolCount; ++d) {
        const int bits = (cw[3 * d] << 2) | (cw[3 * d + 1] << 1) | cw[3 * d + 2];
        t[data_symbol_index(d)] = kBitsToTone[bits];
    }
    return t;
}

}  // namespace fern::ft8
