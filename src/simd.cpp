// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "simd.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace fern::ft8 {

namespace {

std::atomic<int> g_level{-1};

SimdLevel detect() {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2"))
        return SimdLevel::Avx2;
    return SimdLevel::Baseline;
#elif defined(__aarch64__)
    return SimdLevel::Baseline;
#else
    return SimdLevel::Scalar;
#endif
}

}  // namespace

SimdLevel simd_supported() {
    static const SimdLevel level = detect();
    return level;
}

void set_simd_level(SimdLevel level) {
    const int cap = int(simd_supported());
    g_level.store(int(level) < cap ? int(level) : cap);
}

SimdLevel simd_level() {
    int level = g_level.load();
    if (level < 0) {
        SimdLevel want = simd_supported();
        if (const char* env = std::getenv("FERN_FT8_SIMD")) {
            if (std::strcmp(env, "scalar") == 0)
                want = SimdLevel::Scalar;
            else if (std::strcmp(env, "baseline") == 0)
                want = SimdLevel::Baseline;
        }
        set_simd_level(want);
        level = g_level.load();
    }
    return SimdLevel(level);
}

const char* simd_name(SimdLevel level) {
    switch (level) {
    case SimdLevel::Scalar:
        return "scalar";
    case SimdLevel::Baseline:
#if defined(__aarch64__)
        return "NEON";
#else
        return "SSE2";
#endif
    case SimdLevel::Avx2:
        return "AVX2";
    }
    return "?";
}

}  // namespace fern::ft8
