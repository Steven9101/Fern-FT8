// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// `fern-ft8 --fernsdr-module 2`: the decoder session of FernSDR's decoder
// contract (FernSDR's docs/MODULES.md, "Decoders", API 2). Commands on fd 0,
// sample frames on fd 4, events on fd 3, log lines on fd 2.
#pragma once

namespace fern::ft8 {

// Runs a session on the standard descriptors and returns the exit status:
// 0 when FernSDR stopped it or closed its input, 2 for a wrong invocation,
// 3 for a session FernSDR opened wrongly.
int run_module(int argc, char** argv);

// `fern-ft8 --describe`: what the module is and the settings it takes, as
// one JSON object on stdout; the package's manifest is built from it.
int describe_module();

}  // namespace fern::ft8
