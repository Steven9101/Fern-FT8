// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The FT4 protocol of [QEX] (see protocol.h). FT4 sends the same 77-bit
// payload, CRC-14 and LDPC (174,91) codeword as FT8, with three differences
// before the waveform:
//  - the payload is XORed with a fixed pseudo-random vector before the CRC
//    and parity are computed, so that a CQ does not send long runs of tone 0
//    ([QEX] Appendix A); a receiver XORs again;
//  - the 174 bits become 87 symbols of 2 bits each, Gray coded onto 4 tones
//    ([QEX] section 4, Table 3);
//  - four different 4x4 Costas arrays frame three groups of 29 data
//    symbols: 103 channel symbols, sent between two ramp symbols of tone 0
//    that soften the start and end ([QEX] section 4).
// The tones are 1/T = 20.833 Hz apart and last T = 48 ms, smoothed by a
// Gaussian filter of BT = 1 ([QEX] section 5, Table 4): 105 symbols, 5.04 s.
#pragma once

#include <array>
#include <cstdint>

#include "protocol.h"

namespace fern::ft8::ft4 {

constexpr int kSymbolCount = 103;      // Costas and data, without the ramps
constexpr int kWaveSymbolCount = 105;  // with the ramp symbol at each end
constexpr int kDataSymbolCount = 87;
constexpr int kToneCount = 4;
constexpr double kSymbolSeconds = 0.048;
constexpr double kToneSpacingHz = 1.0 / kSymbolSeconds;
// [QEX] calls FT4 twice as fast as FT8; WSJT-X runs it on 7.5 s sequences.
constexpr double kSlotSeconds = 7.5;
// The first ramp symbol starts this long after the slot does, and DT is
// measured from it: what jt9 reports as DT 0.0 (docs/PROTOCOL-SOURCES.md).
constexpr double kStartSeconds = 0.5;

// [QEX] section 4: the four Costas arrays, at channel symbols 0, 33, 66 and
// 99 (after the first ramp symbol).
constexpr int kCostas[4][4] = {{0, 1, 3, 2}, {1, 0, 2, 3}, {2, 3, 1, 0}, {3, 2, 0, 1}};
constexpr int kCostasStart[4] = {0, 33, 66, 99};

// [QEX] Table 3, columns 1 and 3: two bits, most significant first, to a
// tone and back. The map is its own inverse.
constexpr uint8_t kBitsToTone[4] = {0, 1, 3, 2};
constexpr uint8_t kToneToBits[4] = {0, 1, 3, 2};

using Tones = std::array<uint8_t, kSymbolCount>;
// The 105 tones the waveform is built from: ramp, the 103, ramp.
using WaveTones = std::array<uint8_t, kWaveSymbolCount>;

// [QEX] Appendix A: the 77 bits XORed with the payload, most significant
// first, as Payload's layout.
extern const Payload kScrambleVector;

// payload XOR the scrambling vector; applying it twice gives the payload.
Payload scramble(const Payload& payload);

// The codeword FT4 sends for a payload: the scrambled payload, its CRC and
// parity.
Codeword encode_codeword(const Payload& payload);
// The payload a received codeword carries.
Payload payload_of(const Codeword& cw);

// S1, 29 data symbols, S2, 29, S3, 29, S4.
Tones tones_of(const Codeword& cw);
WaveTones wave_tones(const Tones& tones);
// Index of data symbol d (0..86) among the 103.
constexpr int data_symbol_index(int d) { return 4 + d + 4 * (d / 29); }

}  // namespace fern::ft8::ft4
