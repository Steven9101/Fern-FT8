// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>
#include <thread>
#include <vector>

#include "callsign_hash.h"
#include "test.h"

using namespace fern::ft8;

TEST(hashes_match_the_protocol_program) {
    // Output of protocol/hashcodes.f90 for these calls.
    struct {
        const char* call;
        uint32_t h10, h12, h22;
    } const cases[] = {
        {"PJ4/K1ABC", 346, 1387, 1420834},
        {"YW18FIFA", 188, 753, 771524},
        {"W9XYZ", 972, 3889, 3982604},
    };
    for (const auto& c : cases) {
        CHECK_EQ(*callsign_hash(c.call, 10), c.h10);
        CHECK_EQ(*callsign_hash(c.call, 12), c.h12);
        CHECK_EQ(*callsign_hash(c.call, 22), c.h22);
    }
    CHECK(!callsign_hash("K1ABC*", 22).has_value());
    CHECK(!callsign_hash("ABCDEFGHIJKL", 22).has_value());
    CHECK(!callsign_hash("K1ABC", 16).has_value());
}

TEST(table_resolves_all_three_lengths) {
    CallsignHashTable t;
    t.remember("PJ4/K1ABC", 1000);
    CHECK_EQ(*t.lookup(10, 346, 1000), std::string("PJ4/K1ABC"));
    CHECK_EQ(*t.lookup(12, 1387, 1000), std::string("PJ4/K1ABC"));
    CHECK_EQ(*t.lookup(22, 1420834, 1000), std::string("PJ4/K1ABC"));
    CHECK(!t.lookup(22, 1420835, 1000).has_value());
}

TEST(table_entries_expire) {
    CallsignHashTable::Options o;
    o.max_age_seconds = 60;
    CallsignHashTable t(o);
    t.remember("W9XYZ", 0);
    CHECK(t.lookup(22, 3982604, 59).has_value());
    CHECK(!t.lookup(22, 3982604, 61).has_value());
    t.remember("K1ABC", 200);  // prunes the expired entry
    CHECK_EQ(t.size(), size_t(1));
}

TEST(table_is_bounded_and_keeps_the_most_recent) {
    CallsignHashTable::Options o;
    o.capacity = 100;
    CallsignHashTable t(o);
    for (int i = 0; i < 1000; ++i)
        t.remember("K" + std::to_string(i % 10) + "A" + std::string(1, char('A' + i % 26)) +
                       std::string(1, char('A' + (i / 26) % 26)),
                   i);
    CHECK(t.size() <= 100);
    // The latest call to use a hash wins.
    t.remember("W9XYZ", 2000);
    CHECK_EQ(*t.lookup(22, 3982604, 2000), std::string("W9XYZ"));
}

TEST(table_is_safe_to_share_between_threads) {
    CallsignHashTable t;
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; ++k) {
        threads.emplace_back([&t, k] {
            for (int i = 0; i < 5000; ++i) {
                const std::string call = "K" + std::to_string(k) + "A" + std::string(1, char('A' + i % 26)) +
                                         std::string(1, char('A' + (i / 26) % 26));
                t.remember(call, i);
                (void)t.lookup(22, *callsign_hash(call, 22), i);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(t.lookup(22, *callsign_hash("K0AAA", 22), 5000).has_value());
}
