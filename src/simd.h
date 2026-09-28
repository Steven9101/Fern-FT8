// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Which vector code the engine uses. Every vector path computes exactly
// what its scalar counterpart does (the same operations in the same order,
// never a fused multiply-add), so decodes do not depend on the level; the
// tests check that. The level can be lowered for tests and measurements.
#pragma once

namespace fern::ft8 {

enum class SimdLevel {
    // Plain scalar code everywhere.
    Scalar = 0,
    // What every CPU of the architecture has: SSE2 on x86-64, NEON on
    // aarch64. Four-lane belief propagation.
    Baseline = 1,
    // x86-64 with AVX2, detected at run time: also the FFT kernel.
    Avx2 = 2,
};

// The level in use: the highest the CPU supports, unless lowered by
// set_simd_level() or the environment variable FERN_FT8_SIMD (scalar,
// baseline or avx2) read at the first call.
SimdLevel simd_level();
// Lowers (or restores) the level; a level the CPU lacks is capped. Not for
// use while other threads decode.
void set_simd_level(SimdLevel level);
// The CPU's highest level.
SimdLevel simd_supported();
const char* simd_name(SimdLevel level);

}  // namespace fern::ft8
