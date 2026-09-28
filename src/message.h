// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The 77-bit messages of [QEX] Tables 1, 2 and 7 and Appendix A: packing a
// message's text into the payload and unpacking a received payload into the
// text a WSJT-X receiver prints, with the fields a spot report needs.
//
// Unpacking is strict on purpose. A received payload is accepted only when a
// conforming transmitter could have produced it: defined types only (the
// unassigned ones stay reserved, as [QEX] section 9 asks), every field in its
// defined range, and each word in its canonical form, so that packing the
// text again gives the same bits wherever no call is known only by its hash.
// A codeword that passes the CRC by chance, which happens once in 16384
// tries, is then far more likely to be refused than printed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "callsign_hash.h"
#include "protocol.h"

namespace fern::ft8 {

struct MessageFields {
    // The first call as printed: a call, "<CALL>" or "<...>" for a hash, or
    // "CQ", "CQ DX", "QRZ", "DE" and so on. Empty for free text and telemetry.
    std::string to;
    // The transmitting station as printed.
    std::string de;
    // The transmitting station's call when the message carried it in full,
    // A-Z 0-9 and / only; empty when it came as a hash (even a known one)
    // or the message has none.
    std::string de_call;
    // A 4 or 6 character Maidenhead locator, or empty.
    std::string grid;
    // The report word as printed ("-12", "R+05", "RRR", "RR73", "73",
    // "579", "6A"), or empty.
    std::string report;
    // The signal report in dB when the message has one.
    std::optional<int> report_db;
    // "DX", "TEST", "123" and the like after CQ.
    std::string cq_modifier;
    bool cq = false;
};

struct Message {
    int i3 = 0;
    int n3 = 0;
    std::string text;
    MessageFields fields;
    // Calls a receiver learns for its hash table: the transmitting station's
    // call when sent in full, and any nonstandard call.
    std::vector<std::string> learned_calls;
};

struct UnpackOptions {
    // WSJT-X shows the rover suffix /R and the RTTY Roundup "TU;" only in
    // contest mode; outside it such messages are almost always false decodes.
    bool contest_forms = false;
};

// The received payload as text, or nothing when it is not a message a
// conforming transmitter sends. `hashes` (may be null) resolves hashed calls
// as of `now`.
std::optional<Message> unpack_message(const Payload& payload, const CallsignHashTable* hashes, int64_t now,
                                      const UnpackOptions& options = UnpackOptions{});

// Packs a message's text, choosing the type as WSJT-X does for the forms of
// [QEX] Table 1. Calls in angle brackets are sent as hashes and recorded in
// `hashes` if given. Returns nothing for text no type can carry.
std::optional<Payload> pack_message(std::string_view text, CallsignHashTable* hashes = nullptr, int64_t now = 0);

// "Free text", "Standard msg" and so on, for i3.n3.
const char* message_type_name(int i3, int n3);

// A standard call as defined in [QEX] Appendix A: a one or two character
// prefix with at least one letter, a digit, and up to three letters. These
// fit in 28 bits.
bool is_standard_call(std::string_view call);

}  // namespace fern::ft8
