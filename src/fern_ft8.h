// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The engine's public interface, for the decoder module that connects it to
// FernSDR (docs/MODULES.md, "Decoders", API 2):
//
//   CallsignHashTable hashes;                  // one for all channels
//   ChannelConfig cfg;                          // per channel in "open"
//   cfg.rate = rate; cfg.offset_hz = offset; cfg.width_hz = width;
//   cfg.depth = settings depth;
//   Channel ch(cfg, &hashes);
//   ch.push(samples, count, index, utc_us, flags);   // per sample frame
//   for (const SlotResult& r : ch.decode_ready())     // after each push
//       for (const Decode& d : r.decodes) ...          // one "decode" event
//
// A frame's header maps onto push() as it is: its sample index, its UTC time
// in microseconds and its flags (kFrameSamplesLost, kFrameClockSet). A Decode
// carries what a "decode" event needs: slot_start_ms (time), freq_hz (freq),
// snr_db (snr), dt, message.text (message, at most 40 printable characters),
// message.fields.de_call (call, empty when the sender was sent as a hash),
// message.fields.grid (grid, 4 or 6 characters or empty),
// message.fields.report and report_db (report) and quality() ("bp", "osd" or
// "low"). SlotResult has the CPU time of the slot for "stats".
//
// Each Channel is used by one thread at a time; channels may run on
// different threads, sharing the CallsignHashTable, which locks.
#pragma once

#include "callsign_hash.h"
#include "channel.h"
#include "decoder.h"
#include "message.h"
#include "protocol.h"
#include "simd.h"
