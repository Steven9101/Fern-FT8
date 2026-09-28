// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decoding the (174,91) LDPC code of [QEX] section 3 from soft bits.
//
// Belief propagation (the sum-product algorithm, Lin and Costello, "Error
// Control Coding", 2nd ed., 2004, chapter 17) is fast and finds the codeword
// for most signals. When it does not converge, ordered statistics decoding
// (OSD; Fossorier and Lin 1995, as in Lin and Costello chapter 10) takes the
// 91 most reliable independent bits as right, re-encodes, and tries flipping
// one or two of the least reliable of them, keeping the codeword closest to
// the received soft bits. [QEX] section 6 describes this hybrid for FT8.
//
// Soft bits (LLRs) here are log(P(bit = 0) / P(bit = 1)): positive favours 0.
#pragma once

#include <array>
#include <cstdint>

#include "protocol.h"

namespace fern::ft8 {

using Llrs = std::array<float, kLdpcN>;

struct BpResult {
    bool converged = false;  // all parity checks satisfied
    int iterations = 0;
    int unsatisfied = kLdpcM;  // parity checks failing at the end
    Codeword codeword{};
};

// Sum-product belief propagation, at most max_iterations. `beliefs`, if not
// null, receives the bits' total LLRs after the last iteration.
BpResult bp_decode(const Llrs& llr, int max_iterations, Llrs* beliefs = nullptr);

struct OsdResult {
    bool found = false;
    Codeword codeword{};
    // Sum of |LLR| over the bits where the codeword disagrees with the hard
    // decisions, and the number of those bits.
    float distance = 0;
    int disagreements = 0;
};

struct OsdOptions {
    // 1: flip each of the 91 basis bits; 2: also pairs among the least
    // reliable `pair_span` of them.
    int order = 1;
    int pair_span = 30;
};

// The codeword nearest to the soft bits among those OSD tries, provided its
// CRC is right; found is false otherwise.
OsdResult osd_decode(const Llrs& llr, const OsdOptions& options);

// Bits where cw disagrees with the hard decisions of llr.
int hard_disagreements(const Codeword& cw, const Llrs& llr);

}  // namespace fern::ft8
