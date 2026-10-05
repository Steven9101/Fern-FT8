// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ft4.h"

namespace fern::ft8::ft4 {

// [QEX] Appendix A prints the vector as 0100101001011110100010
// 0110110100101100001000 1010011110010101010110 11111000101; here packed
// eight bits a byte, the last three bits zero.
const Payload kScrambleVector = {0x4a, 0x5e, 0x89, 0xb4, 0xb0, 0x8a, 0x79, 0x55, 0xbe, 0x28};

Payload scramble(const Payload& payload) {
    Payload out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = uint8_t(payload[i] ^ kScrambleVector[i]);
    return out;
}

Codeword encode_codeword(const Payload& payload) { return ft8::encode_codeword(scramble(payload)); }

Payload payload_of(const Codeword& cw) { return scramble(ft8::payload_of(cw)); }

Tones tones_of(const Codeword& cw) {
    Tones t{};
    for (int block = 0; block < 4; ++block)
        for (int k = 0; k < 4; ++k)
            t[size_t(kCostasStart[block] + k)] = uint8_t(kCostas[block][k]);
    for (int d = 0; d < kDataSymbolCount; ++d)
        t[size_t(data_symbol_index(d))] = kBitsToTone[(cw[size_t(2 * d)] << 1) | cw[size_t(2 * d + 1)]];
    return t;
}

WaveTones wave_tones(const Tones& tones) {
    WaveTones w{};
    // The ramp symbols are tone 0 ([QEX] section 4).
    for (int s = 0; s < kSymbolCount; ++s)
        w[size_t(s + 1)] = tones[size_t(s)];
    return w;
}

}  // namespace fern::ft8::ft4
