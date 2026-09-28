// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Message packing and unpacking: every ft8code vector both ways, round
// trips of every type, fields, and random payloads.
#include <map>
#include <random>
#include <string>

#include "message.h"
#include "test.h"
#include "vectors.h"

using namespace fern::ft8;

namespace {

// Calls ft8code knew by hash when it printed a vector, taken from the text.
void learn_bracketed(const std::string& text, CallsignHashTable& table) {
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos) {
        const size_t end = text.find('>', pos);
        if (end == std::string::npos)
            break;
        const std::string call = text.substr(pos + 1, end - pos - 1);
        if (call != "...")
            table.remember(call, 0);
        pos = end;
    }
}

UnpackOptions contest() {
    UnpackOptions o;
    o.contest_forms = true;
    return o;
}

std::string bits_of(const Payload& p) {
    std::string s;
    for (int i = 0; i < 77; ++i)
        s += char('0' + payload_bit(p, i));
    return s;
}

std::optional<Message> round_trip(const std::string& text, CallsignHashTable* table = nullptr,
                                  const UnpackOptions& options = UnpackOptions{}) {
    auto p = pack_message(text, table, 0);
    if (!p)
        return std::nullopt;
    return unpack_message(*p, table, 0, options);
}

}  // namespace

TEST(unpack_matches_ft8code) {
    for (const auto& v : test::ft8code_vectors()) {
        CallsignHashTable table;
        learn_bracketed(v.decoded, table);
        const auto m = unpack_message(v.payload(), &table, 0, contest());
        REQUIRE(m.has_value() || (std::fprintf(stderr, "    unpack failed: %s\n", v.decoded.c_str()), false));
        CHECK_EQ(m->text, v.decoded);
        CHECK_EQ(m->i3, v.i3);
        CHECK_EQ(m->n3, v.n3);
    }
}

TEST(pack_matches_ft8code) {
    for (const auto& v : test::ft8code_vectors()) {
        if (v.message != v.decoded)
            continue;  // ft8code cut the text to fit; nothing to compare
        const auto p = pack_message(v.message);
        REQUIRE(p.has_value() || (std::fprintf(stderr, "    pack failed: %s\n", v.message.c_str()), false));
        if (bits_of(*p) != v.bits)
            std::fprintf(stderr, "    %s\n", v.message.c_str());
        CHECK_EQ(bits_of(*p), v.bits);
    }
}

TEST(every_type_round_trips) {
    const char* const messages[] = {
        "TNX BOB 73 GL",
        "K1ABC RR73; W9XYZ <KH1/KH7Z> -08",
        "K1ABC W9XYZ 6A WI",
        "W9XYZ K1ABC R 17B EMA",
        "123456789ABCDEF012",
        "CQ K1ABC FN42",
        "CQ DX K1ABC FN42",
        "K1ABC W9XYZ R-09",
        "K1ABC W9XYZ RR73",
        "W9XYZ <PJ4/K1ABC> -11",
        "CQ G4ABC/P IO91",
        "K1ABC W9XYZ 579 WI",
        "KA1ABC G3AAA 529 0013",
        "CQ PJ4/K1ABC",
        "<W9XYZ> PJ4/K1ABC RRR",
        "PJ4/K1ABC <W9XYZ> 73",
        "<PA9XYZ> <G4ABC/P> R 590003 IO91NP",
        "<PA3XYZ> <G4ABC> 522047 AA00AA",
    };
    for (const char* text : messages) {
        CallsignHashTable table;
        const auto m = round_trip(text, &table);
        REQUIRE(m.has_value() || (std::fprintf(stderr, "    %s\n", text), false));
        CHECK_EQ(m->text, std::string(text));
    }
}

TEST(contest_forms_need_the_option) {
    CHECK(!round_trip("K1ABC/R W9XYZ EN37").has_value());
    CHECK(round_trip("K1ABC/R W9XYZ EN37", nullptr, contest()).has_value());
    CHECK(!round_trip("TU; KA0DEF K1ABC R 569 MA").has_value());
    CHECK(round_trip("TU; KA0DEF K1ABC R 569 MA", nullptr, contest()).has_value());
    // /P is the EU VHF form, shown always.
    CHECK(round_trip("G4ABC/P PA9XYZ JO22").has_value());
}

TEST(fields_of_a_cq) {
    const auto m = round_trip("CQ DX K1ABC FN42");
    REQUIRE(m.has_value());
    CHECK(m->fields.cq);
    CHECK_EQ(m->fields.cq_modifier, std::string("DX"));
    CHECK_EQ(m->fields.to, std::string("CQ DX"));
    CHECK_EQ(m->fields.de_call, std::string("K1ABC"));
    CHECK_EQ(m->fields.grid, std::string("FN42"));
    CHECK(!m->fields.report_db.has_value());
    CHECK_EQ(m->i3, 1);
}

TEST(fields_of_reports) {
    auto m = round_trip("K1ABC W9XYZ R-09");
    REQUIRE(m.has_value());
    CHECK(!m->fields.cq);
    CHECK_EQ(m->fields.to, std::string("K1ABC"));
    CHECK_EQ(m->fields.de_call, std::string("W9XYZ"));
    CHECK_EQ(m->fields.report, std::string("R-09"));
    CHECK_EQ(*m->fields.report_db, -9);
    m = round_trip("K1ABC W9XYZ -50");
    REQUIRE(m.has_value());
    CHECK_EQ(*m->fields.report_db, -50);
    m = round_trip("K1ABC W9XYZ +49");
    REQUIRE(m.has_value());
    CHECK_EQ(*m->fields.report_db, 49);
    m = round_trip("K1ABC W9XYZ RR73");
    REQUIRE(m.has_value());
    CHECK_EQ(m->fields.report, std::string("RR73"));
    CHECK(m->fields.grid.empty());
}

TEST(hashed_callers_are_not_reported_as_calls) {
    CallsignHashTable table;
    auto m = round_trip("PJ4/K1ABC <W9XYZ> 73", &table);
    REQUIRE(m.has_value());
    CHECK_EQ(m->fields.de, std::string("<W9XYZ>"));
    CHECK(m->fields.de_call.empty());
    m = round_trip("<W9XYZ> PJ4/K1ABC RRR", &table);
    REQUIRE(m.has_value());
    CHECK_EQ(m->fields.de_call, std::string("PJ4/K1ABC"));
    m = round_trip("K1ABC <PJ4/W9XYZ> -11", &table);
    REQUIRE(m.has_value());
    CHECK_EQ(m->fields.de, std::string("<PJ4/W9XYZ>"));
    CHECK(m->fields.de_call.empty());
    m = round_trip("K1ABC RR73; W9XYZ <KH1/KH7Z> -08", &table);
    REQUIRE(m.has_value());
    CHECK(m->fields.de_call.empty());
}

TEST(unknown_hashes_print_as_dots) {
    CallsignHashTable sender;
    const auto p = pack_message("W9XYZ <PJ4/K1ABC> -11", &sender, 0);
    REQUIRE(p.has_value());
    const auto m = unpack_message(*p, nullptr, 0);
    REQUIRE(m.has_value());
    CHECK_EQ(m->text, std::string("W9XYZ <...> -11"));
}

TEST(receivers_learn_the_sender_call) {
    auto m = round_trip("K1ABC W9XYZ EN37");
    REQUIRE(m.has_value());
    REQUIRE(m->learned_calls.size() == 1);
    CHECK_EQ(m->learned_calls[0], std::string("W9XYZ"));
    m = round_trip("CQ YW18FIFA");
    REQUIRE(m.has_value());
    REQUIRE(m->learned_calls.size() == 1);
    CHECK_EQ(m->learned_calls[0], std::string("YW18FIFA"));
}

TEST(reserved_types_are_refused) {
    std::mt19937 rng(3);
    for (int trial = 0; trial < 2000; ++trial) {
        Payload p{};
        for (int i = 0; i < 71; ++i)
            set_payload_bit(p, i, int(rng() & 1));
        const int kind = trial % 5;
        // i3 6 and 7; i3 0 with n3 2, 6 and 7
        const int i3 = kind == 0 ? 6 : kind == 1 ? 7 : 0;
        const int n3 = kind == 2 ? 2 : kind == 3 ? 6 : kind == 4 ? 7 : int(rng() % 8);
        for (int b = 0; b < 3; ++b) {
            set_payload_bit(p, 71 + b, (n3 >> (2 - b)) & 1);
            set_payload_bit(p, 74 + b, (i3 >> (2 - b)) & 1);
        }
        CHECK(!unpack_message(p, nullptr, 0, contest()).has_value());
    }
}

TEST(rr73_has_two_encodings) {
    // WSJT-X sends RR73 as the locator of that name; the public-domain
    // grid4_to_g15.f90 gives it g15 = 32403. Both read as the word.
    auto p = pack_message("K1ABC W9XYZ RR73");
    REQUIRE(p.has_value());
    Payload q = *p;
    for (int b = 0; b < 15; ++b)
        set_payload_bit(q, 59 + b, (32403 >> (14 - b)) & 1);
    auto m = unpack_message(q, nullptr, 0);
    REQUIRE(m.has_value());
    CHECK_EQ(m->text, std::string("K1ABC W9XYZ RR73"));
    CHECK(m->fields.grid.empty());
    // Never with the R bit.
    set_payload_bit(q, 58, 1);
    CHECK(!unpack_message(q, nullptr, 0).has_value());
    q = *p;
    set_payload_bit(q, 58, 1);
    CHECK(!unpack_message(q, nullptr, 0).has_value());
}

TEST(non_canonical_words_are_refused) {
    const auto cq = pack_message("CQ K1ABC FN42");
    REQUIRE(cq.has_value());
    Payload r = *cq;
    set_payload_bit(r, 58, 1);  // R1 on a CQ
    CHECK(!unpack_message(r, nullptr, 0).has_value());
    // Type 2 without /P.
    const auto std1 = pack_message("K1ABC W9XYZ EN37");
    REQUIRE(std1.has_value());
    Payload t2 = *std1;
    set_payload_bit(t2, 75, 1);
    set_payload_bit(t2, 76, 0);
    CHECK(!unpack_message(t2, nullptr, 0).has_value());
    // A report beyond what is packed.
    Payload big = *std1;
    for (int b = 0; b < 15; ++b)
        set_payload_bit(big, 59 + b, ((32400 + 200) >> (14 - b)) & 1);
    CHECK(!unpack_message(big, nullptr, 0).has_value());
}

TEST(random_payloads_unpack_safely_and_canonically) {
    std::mt19937_64 rng(4);
    const int trials = test::quick() ? 20000 : 300000;
    std::map<std::string, int> accepted;
    int total = 0;
    for (int trial = 0; trial < trials; ++trial) {
        Payload p{};
        const uint64_t a = rng(), b = rng();
        for (int i = 0; i < 64; ++i)
            set_payload_bit(p, i, int((a >> i) & 1));
        for (int i = 64; i < 77; ++i)
            set_payload_bit(p, i, int((b >> (i - 64)) & 1));
        for (const UnpackOptions& o : {UnpackOptions{}, contest()}) {
            const auto m = unpack_message(p, nullptr, 0, o);
            if (!m)
                continue;
            ++total;
            accepted[std::to_string(m->i3) + "." + std::to_string(m->n3)]++;
            CHECK(m->text.size() <= 40);
            for (char c : m->text)
                CHECK(c >= 32 && c < 127);
            for (char c : m->fields.de_call)
                CHECK((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/');
            CHECK(m->fields.de_call.size() <= 12);
            CHECK(m->fields.grid.empty() || m->fields.grid.size() == 4 || m->fields.grid.size() == 6);
            // Accepted means canonical: the text packs to the same bits,
            // wherever no call is shown only as a hash.
            if (m->text.find('<') == std::string::npos) {
                const auto back = pack_message(m->text);
                if (!back)
                    std::fprintf(stderr, "    does not pack: %s\n", m->text.c_str());
                CHECK(back.has_value());
                if (back && *back != p) {
                    // Two words have two encodings in use: RR73, and a
                    // nonstandard call justified left or right.
                    const auto again = unpack_message(*back, nullptr, 0, o);
                    const bool alias = again && again->text == m->text &&
                                       (m->i3 == 4 || m->text.find("RR73") != std::string::npos);
                    if (!alias)
                        std::fprintf(stderr, "    not canonical: %s\n", m->text.c_str());
                    CHECK(alias);
                }
            }
        }
    }
    std::fprintf(stderr, "    %d of %d random payloads accepted:", total, 2 * trials);
    for (const auto& [k, n] : accepted)
        std::fprintf(stderr, " %s=%d", k.c_str(), n);
    std::fprintf(stderr, "\n");
}

TEST(pack_refuses_what_no_type_carries) {
    CHECK(!pack_message("").has_value());
    CHECK(!pack_message("THIS TEXT IS FAR TOO LONG").has_value());
    CHECK(!pack_message("LOWER_CASE").has_value());
    CHECK(!pack_message("K1ABC W9XYZ +50X").has_value());
}

TEST(message_type_names) {
    CHECK_EQ(std::string(message_type_name(0, 0)), std::string("Free text"));
    CHECK_EQ(std::string(message_type_name(1, 0)), std::string("Standard msg"));
    CHECK_EQ(std::string(message_type_name(4, 0)), std::string("Nonstandard call"));
    CHECK_EQ(std::string(message_type_name(7, 0)), std::string("Reserved"));
}
