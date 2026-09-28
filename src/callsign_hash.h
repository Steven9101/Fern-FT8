// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Callsign hashes. Some messages carry a call only as a 10, 12 or 22-bit hash
// ([QEX] section 2 and Appendix A); a receiver shows the call behind a hash
// when it has heard that call in full before, and <...> otherwise.
//
// One table serves every channel and slot of a decoder: the hash depends
// only on the call, and a station heard on 40 m is often worked on 20 m.
// Entries age out (an hour by default) and the table holds a bounded number
// of calls, so a busy multi-band skimmer neither grows without end nor keeps
// showing a call for a hash that some other station now uses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fern::ft8 {

// protocol/hashcodes.f90: the call, left-justified in 11 characters of
// " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/", read as a base-38 number,
// multiplied by 47055833459 modulo 2^64; the hash is the top `bits` bits.
// Returns nothing when the call has a character outside that alphabet or is
// longer than 11.
std::optional<uint32_t> callsign_hash(std::string_view call, int bits);

class CallsignHashTable {
public:
    struct Options {
        size_t capacity = 20000;
        int64_t max_age_seconds = 3600;
    };

    CallsignHashTable();
    explicit CallsignHashTable(Options options);
    CallsignHashTable(const CallsignHashTable&) = delete;
    CallsignHashTable& operator=(const CallsignHashTable&) = delete;

    // Records a call heard in full at `now` (seconds on any clock the caller
    // uses consistently, UTC in practice). A later call with the same hash
    // replaces an earlier one, as a receiver shows the most recent.
    void remember(std::string_view call, int64_t now);
    // The call behind a 10, 12 or 22-bit hash, if one was heard within
    // max_age_seconds before `now`.
    std::optional<std::string> lookup(int bits, uint32_t hash, int64_t now) const;
    size_t size() const;

private:
    struct Entry {
        std::string call;
        int64_t seen = 0;
        uint32_t h10 = 0, h12 = 0, h22 = 0;
    };
    using Lru = std::list<Entry>;

    void forget_locked(Lru::iterator it);

    Options options_;
    mutable std::mutex mutex_;
    Lru lru_;  // most recent first
    std::unordered_map<std::string, Lru::iterator> by_call_;
    std::unordered_map<uint32_t, Lru::iterator> by_h10_, by_h12_, by_h22_;
};

}  // namespace fern::ft8
