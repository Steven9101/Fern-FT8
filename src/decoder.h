// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decoding one 15 s slot of one channel. The input is complex baseband at
// 6400 samples a second (1024 per symbol) from 1.5 s before the slot starts
// to 16.5 s after, so that transmissions started early or late are whole.
// docs/DESIGN.md describes the pipeline and why it is built this way.
#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "callsign_hash.h"
#include "message.h"
#include "protocol.h"

namespace fern::ft8 {

constexpr double kInternalRate = 6400.0;
constexpr int kInternalSamplesPerSymbol = 1024;
constexpr double kSlotLeadSeconds = 1.5;
constexpr int kSlotSamples = 115200;  // 18 s

enum class DecodeMethod { Bp, Osd };

struct Decode {
    // UTC start of the slot, milliseconds since 1970.
    int64_t slot_start_ms = 0;
    // Audio frequency of tone 0 above the dial, hertz.
    double freq_hz = 0;
    // Start of the transmission after slot start + 0.5 s, seconds.
    double dt = 0;
    // WSJT-X's convention: signal power over noise power in 2500 Hz.
    int snr_db = 0;
    Message message;
    Payload payload{};
    // How the codeword was found. A priori information is never used.
    DecodeMethod method = DecodeMethod::Bp;
    int osd_order = 0;
    bool a_priori = false;
    int ldpc_iterations = 0;
    // Codeword bits that disagree with the received hard decisions.
    int hard_errors = 0;
    // Costas tones received as the strongest of their symbol, of 21.
    int costas_hits = 0;
    // The candidate's Costas power ratio in the spectrogram.
    float sync = 0;
    // Which soft-bit set decoded: blocks of 1, 2 or 3 symbols.
    int llr_span = 0;
    // An OSD decode that is far enough from the received bits that it may be
    // wrong; such decodes must not be reported to others.
    bool low_confidence = false;
    int pass = 0;

    // "bp", "osd" or "low", as the decoder contract names them.
    const char* quality() const;
};

// The decoder's parameters for one depth. tuning_for_depth() gives the
// defaults; the fields are public so that experiments (fern-ft8 --tune) can
// vary them.
struct DecoderTuning {
    int passes = 3;
    float sync_min = 1.8f;      // candidate threshold, Costas power ratio
    int max_candidates = 150;   // per pass
    int min_costas_hits = 6;    // of 21, before LDPC
    // Costas tones strongest among the 8 tone bins in the spectrogram, of
    // 21, before a candidate is looked at closely.
    int spec_min_hits = 6;
    int bp_max_hard = 36;       // hard errors a BP decode may have
    int osd_order = 1;          // 0: no OSD
    int osd_pair_span = 30;     // order 2 flips pairs among this many bits
    // An OSD codeword is accepted with at most osd_max_hard hard errors when
    // at least osd_min_costas of the 21 Costas tones were received right,
    // or with at most osd_weak_max_hard when osd_weak_min_costas were.
    int osd_min_costas = 11;
    int osd_max_hard = 36;
    int osd_weak_min_costas = 9;
    int osd_weak_max_hard = 24;
    // OSD runs only on candidates whose Costas power ratio reaches this:
    // on white noise 99 % of candidates stay below 2.82.
    float osd_min_sync = 2.8f;
    // Above osd_low_hard hard errors or below osd_min_costas hits an OSD
    // decode is low confidence.
    int osd_low_hard = 30;
    float llr_scale = 2.8f;
};
DecoderTuning tuning_for_depth(int depth);
// Applies "key=value,key=value" to t; returns false on an unknown key.
bool apply_tuning(DecoderTuning& t, const std::string& spec);

struct DecodeSettings {
    // 1: fast, BP only; 2: BP and OSD order 1, three passes; 3: more
    // candidates and OSD order 2.
    int depth = 2;
    // Range of the tone 0 frequency searched, in baseband hertz.
    double min_hz = -1800;
    double max_hz = 1750;
    UnpackOptions unpack;
    // Overrides tuning_for_depth(depth) when set.
    bool custom_tuning = false;
    DecoderTuning tuning;
};

struct SlotStats {
    int candidates = 0;
    int ldpc_runs = 0;
    int osd_runs = 0;
    int passes = 0;
};

class SlotDecoder {
public:
    SlotDecoder();
    ~SlotDecoder();
    SlotDecoder(const SlotDecoder&) = delete;
    SlotDecoder& operator=(const SlotDecoder&) = delete;

    // `samples` holds kSlotSamples at kInternalRate, sample 0 at the slot's
    // start less 1.5 s; decoded signals are subtracted from it. Samples
    // outside [valid_begin, valid_end) are zeros standing for missing ones.
    // offset_hz is the audio frequency of baseband 0 Hz. Hashed calls
    // resolve through `hashes` (may be null), which also learns the calls
    // of confident decodes.
    std::vector<Decode> decode(std::vector<std::complex<float>>& samples, size_t valid_begin, size_t valid_end,
                               double offset_hz, int64_t slot_start_ms, const DecodeSettings& settings,
                               CallsignHashTable* hashes, SlotStats* stats = nullptr);

    // Scratch space, opaque outside decoder.cpp.
    struct Work;

private:
    std::unique_ptr<Work> work_;
};

}  // namespace fern::ft8
