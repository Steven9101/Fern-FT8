// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "callsign_hash.h"

namespace fern::ft8 {

namespace {

int alphabet38(char c) {
    if (c == ' ')
        return 0;
    if (c >= '0' && c <= '9')
        return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z')
        return 11 + (c - 'A');
    if (c == '/')
        return 37;
    return -1;
}

}  // namespace

std::optional<uint32_t> callsign_hash(std::string_view call, int bits) {
    if (call.size() > 11 || (bits != 10 && bits != 12 && bits != 22))
        return std::nullopt;
    uint64_t n = 0;
    for (size_t i = 0; i < 11; ++i) {
        const int v = alphabet38(i < call.size() ? call[i] : ' ');
        if (v < 0)
            return std::nullopt;
        n = 38 * n + uint64_t(v);
    }
    // Unsigned arithmetic wraps modulo 2^64, as the definition asks.
    const uint64_t product = n * 47055833459ull;
    return uint32_t(product >> (64 - bits));
}

CallsignHashTable::CallsignHashTable() : CallsignHashTable(Options{}) {}

CallsignHashTable::CallsignHashTable(Options options) : options_(options) {
    if (options_.capacity == 0)
        options_.capacity = 1;
}

void CallsignHashTable::forget_locked(Lru::iterator it) {
    auto drop = [&](std::unordered_map<uint32_t, Lru::iterator>& index, uint32_t h) {
        auto found = index.find(h);
        if (found != index.end() && found->second == it)
            index.erase(found);
    };
    drop(by_h10_, it->h10);
    drop(by_h12_, it->h12);
    drop(by_h22_, it->h22);
    by_call_.erase(it->call);
    lru_.erase(it);
}

void CallsignHashTable::remember(std::string_view call, int64_t now) {
    const auto h10 = callsign_hash(call, 10);
    const auto h12 = callsign_hash(call, 12);
    const auto h22 = callsign_hash(call, 22);
    if (!h10 || !h12 || !h22 || call.size() < 3)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(call);
    auto existing = by_call_.find(key);
    if (existing != by_call_.end())
        forget_locked(existing->second);
    lru_.push_front(Entry{key, now, *h10, *h12, *h22});
    const auto it = lru_.begin();
    by_call_[key] = it;
    by_h10_[*h10] = it;
    by_h12_[*h12] = it;
    by_h22_[*h22] = it;
    // The list is in order of hearing, so the expired entries are at its end
    // unless a caller's clock went backwards; those go when capacity demands.
    while (lru_.size() > options_.capacity || now - lru_.back().seen > options_.max_age_seconds)
        forget_locked(std::prev(lru_.end()));
}

std::optional<std::string> CallsignHashTable::lookup(int bits, uint32_t hash, int64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::unordered_map<uint32_t, Lru::iterator>* index = nullptr;
    if (bits == 10)
        index = &by_h10_;
    else if (bits == 12)
        index = &by_h12_;
    else if (bits == 22)
        index = &by_h22_;
    else
        return std::nullopt;
    const auto found = index->find(hash);
    if (found == index->end())
        return std::nullopt;
    const Entry& e = *found->second;
    if (now - e.seen > options_.max_age_seconds || e.seen - now > options_.max_age_seconds)
        return std::nullopt;
    return e.call;
}

size_t CallsignHashTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_.size();
}

}  // namespace fern::ft8
