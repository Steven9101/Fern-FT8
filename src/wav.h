// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Mono WAV files for the command line tool and the tests: 16-bit PCM or
// 32-bit float in, 16-bit PCM out.
#pragma once

#include <string>
#include <vector>

namespace fern::ft8 {

struct Audio {
    double rate = 0;
    std::vector<float> samples;  // full scale is 1.0
};

// Reads the first channel. Throws std::runtime_error with the reason when
// the file cannot be read or is not a WAV this understands.
Audio read_wav(const std::string& path);
// Writes 16-bit PCM; samples beyond +-1 are clipped. Throws on failure.
void write_wav(const std::string& path, const Audio& audio);

}  // namespace fern::ft8
