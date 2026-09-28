// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One decoder channel as FernSDR's decoder contract (docs/MODULES.md,
// "Decoders", API 2) delivers it: complex baseband at a rate that need not
// be whole, centred `offset` hertz above the dial, in frames that carry a
// running sample index and the UTC time of their first sample. Channel
// resamples onto a grid of 1/6400 s aligned to UTC, keeps the last minute,
// and decodes each 15 s slot once it holds 1.5 s before to 16.5 s after the
// slot's start.
//
// A Channel is used by one thread at a time; different channels may decode
// on different threads at once. The callsign hash table may be shared by
// all of them.
#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "callsign_hash.h"
#include "decoder.h"
#include "resampler.h"

namespace fern::ft8 {

// Frame flags of the decoder contract.
constexpr uint32_t kFrameSamplesLost = 1;
constexpr uint32_t kFrameClockSet = 2;

struct ChannelConfig {
    double rate = 0;          // complex samples a second
    double offset_hz = 2000;  // audio frequency of baseband 0 Hz
    double width_hz = 4000;   // the channel covers offset +- width / 2
    int depth = 2;
    // Tone 0 frequencies searched, audio hertz above the dial. Zero means
    // the channel's own edges, less the 50 Hz a signal occupies.
    double min_freq_hz = 0;
    double max_freq_hz = 0;
    UnpackOptions unpack;
};

struct SlotResult {
    int64_t slot_start_ms = 0;
    std::vector<Decode> decodes;
    SlotStats stats;
    // Share of the slot's 18 s that no frame covered.
    double missing = 0;
    // CPU time of the decoding thread for this slot.
    double cpu_seconds = 0;
};

class Channel {
public:
    // Throws std::invalid_argument for a rate or width it cannot handle.
    Channel(const ChannelConfig& config, CallsignHashTable* hashes);

    // One frame: `count` samples, the first with running index first_index
    // and UTC time utc_us (microseconds since 1970); flags as the contract
    // defines them. A frame whose index or time does not follow on from the
    // previous one is taken as a new start, as if flagged.
    void push(const std::complex<float>* samples, size_t count, uint64_t first_index, int64_t utc_us,
              uint32_t flags = 0);

    // Decodes every slot that is complete, oldest first.
    std::vector<SlotResult> decode_ready();
    // The input has ended: decodes the remaining slots that have samples
    // from their start up to at least 2 s into them.
    std::vector<SlotResult> finish();

    const ChannelConfig& config() const { return config_; }

private:
    void anchor(uint64_t index, int64_t utc_us);
    void produce();
    SlotResult decode_slot(int64_t slot);

    ChannelConfig config_;
    CallsignHashTable* hashes_;
    DecodeSettings settings_;
    Interpolator interp_;
    SlotDecoder decoder_;

    // Input samples from hist_start_ on, by running index.
    std::vector<std::complex<float>> hist_;
    uint64_t hist_start_ = 0;
    uint64_t next_index_ = 0;  // index after the last frame
    bool anchored_ = false;
    uint64_t anchor_index_ = 0;
    int64_t anchor_utc_us_ = 0;

    // Output on the UTC grid: sample g is at g / 6400 s after 1970.
    std::vector<std::complex<float>> ring_;
    std::vector<uint8_t> have_;
    int64_t next_g_ = 0;     // next grid sample to produce
    int64_t written_ = -1;   // grid samples up to here have been handled
    int64_t next_slot_ = 0;  // index of the next slot to decode (slot k starts at 15 k s)
    bool slot_known_ = false;
    std::vector<std::complex<float>> slot_buf_;
};

}  // namespace fern::ft8
