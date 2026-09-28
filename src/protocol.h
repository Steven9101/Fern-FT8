// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The FT8 protocol as its authors define it in "The FT4 and FT8 Communication
// Protocols" (S. Franke K9AN, B. Somerville G4WJS, J. Taylor K1JT, QEX
// July/August 2020, pp. 7-17), cited below as [QEX], and in the public-domain
// files of its reference [14], ft4_ft8_protocols.tgz, kept in protocol/.
//
// A transmission carries 77 payload bits. A 14-bit CRC makes 91 bits, an
// LDPC (174,91) code adds 83 parity bits, and the 174 bits become 58 tones of
// 3 bits each, Gray coded. Three 7x7 Costas arrays frame them: 79 tones of
// 0.16 s at 6.25 Hz spacing, 12.64 s in all, sent 0.5 s into a 15 s slot.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fern::ft8 {

constexpr int kPayloadBits = 77;
constexpr int kCrcBits = 14;
constexpr int kLdpcK = 91;   // payload and CRC
constexpr int kLdpcM = 83;   // parity bits
constexpr int kLdpcN = 174;  // codeword
constexpr int kSymbolCount = 79;
constexpr int kDataSymbolCount = 58;
constexpr int kToneCount = 8;
constexpr double kSymbolSeconds = 0.16;
constexpr double kToneSpacingHz = 6.25;
constexpr double kSlotSeconds = 15.0;
// A transmission starts this long after its slot does; DT is measured from it.
constexpr double kStartSeconds = 0.5;

// [QEX] section 4: the Costas array, sent at symbols 0, 36 and 72.
constexpr int kCostas[7] = {3, 1, 4, 0, 6, 5, 2};
constexpr int kCostasStart[3] = {0, 36, 72};

// [QEX] Table 3: three bits, most significant first, to a tone and back.
constexpr uint8_t kBitsToTone[8] = {0, 1, 3, 2, 5, 6, 4, 7};
constexpr uint8_t kToneToBits[8] = {0, 1, 3, 2, 6, 4, 5, 7};

// The 77 payload bits, most significant first; the low 3 bits of byte 9 are
// zero. This is also the layout PSK Reporter's messageBits field takes.
using Payload = std::array<uint8_t, 10>;

// Codeword bits, one per byte, 0 or 1: payload, CRC, parity.
using Codeword = std::array<uint8_t, kLdpcN>;
using Tones = std::array<uint8_t, kSymbolCount>;

// Generated from protocol/ by tools/gen_tables.py.
extern const uint8_t kLdpcGenerator[kLdpcM][12];
extern const uint8_t kLdpcColumnChecks[kLdpcN][3];
constexpr int kArrlSectionCount = 86;
constexpr int kStateCount = 171;
extern const char* const kArrlSections[kArrlSectionCount];
extern const char* const kStatesProvinces[kStateCount];

// The parity-check matrix as lists: which bits each check sums, and which
// checks each bit is in. Built once from kLdpcColumnChecks.
struct LdpcGraph {
    // Rows have 6 or 7 bits; unused entries are -1.
    int16_t check_bits[kLdpcM][7];
    uint8_t check_degree[kLdpcM];
    uint8_t bit_checks[kLdpcN][3];
};
const LdpcGraph& ldpc_graph();

inline int payload_bit(const Payload& p, int i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
inline void set_payload_bit(Payload& p, int i, int v) {
    const uint8_t mask = uint8_t(0x80u >> (i & 7));
    if (v)
        p[i >> 3] |= mask;
    else
        p[i >> 3] &= uint8_t(~mask);
}

// [QEX] section 3: CRC-14, polynomial 0x6757, initial value zero, over the
// payload extended with 5 zero bits to 82 (protocol/gen_crc14.f90).
uint16_t crc14(const Payload& payload);

// Payload + CRC + parity, per protocol/generator.dat.
Codeword encode_codeword(const Payload& payload);
// True when all 83 parity checks of protocol/parity.dat are satisfied.
bool parity_ok(const Codeword& cw);
// Number of unsatisfied parity checks.
int parity_failures(const Codeword& cw);
// True when bits 77..90 are the CRC of bits 0..76.
bool crc_ok(const Codeword& cw);
Payload payload_of(const Codeword& cw);

// The 79 tones: Costas, 29 data tones, Costas, 29 data tones, Costas.
Tones tones_of(const Codeword& cw);
// Index of data symbol d (0..57) among the 79.
constexpr int data_symbol_index(int d) { return d < 29 ? 7 + d : 43 + (d - 29); }

}  // namespace fern::ft8
