// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Field encodings follow [QEX] Appendix A and the public-domain programs in
// protocol/ (std_call_to_c28.f90, grid4_to_g15.f90, grid6_to_g25.f90,
// nonstd_to_c58.f90, free_text_to_f71.f90, hashcodes.f90). The printed forms
// are those of [QEX] Table 1, checked word for word against WSJT-X 2.7.0's
// ft8code in tests/test_message.cpp.
#include "message.h"

#include <cstdio>
#include <cstring>

namespace fern::ft8 {

namespace {

constexpr uint32_t kTokens = 2063592;    // c28 values below this are DE, QRZ, CQ...
constexpr uint32_t kMax22 = 4194304;     // then 2^22 hash values, then calls
constexpr uint32_t kStdCalls = 262177560;  // 37*36*10*27*27*27
constexpr uint32_t kMaxGrid4 = 32400;    // 18*18*10*10
constexpr uint32_t kMaxGrid6 = 18662400;  // 18*18*10*10*24*24

constexpr char kA1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char kA2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char kA4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char kA38[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
constexpr char kA42[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_letter(char c) { return c >= 'A' && c <= 'Z'; }

int index_in(const char* alphabet, char c) {
    const char* p = std::strchr(alphabet, c);
    return (p && c != '\0') ? int(p - alphabet) : -1;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ')
        ++a;
    while (b > a && s[b - 1] == ' ')
        --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split_words(std::string_view s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty())
                words.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        words.push_back(cur);
    return words;
}

class BitWriter {
public:
    void put(uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            set_payload_bit(payload_, pos_++, int((value >> i) & 1));
    }
    const Payload& payload() const { return payload_; }
    int position() const { return pos_; }

private:
    Payload payload_{};
    int pos_ = 0;
};

class BitReader {
public:
    explicit BitReader(const Payload& p) : p_(p) {}
    uint64_t get(int bits) {
        uint64_t v = 0;
        for (int i = 0; i < bits; ++i)
            v = (v << 1) | uint64_t(payload_bit(p_, pos_++));
        return v;
    }

private:
    const Payload& p_;
    int pos_ = 0;
};

// ---- standard calls, c28 ----

// The value of a standard call within the call range of c28, per
// protocol/std_call_to_c28.f90, with the call-area digit placed third.
std::optional<uint32_t> std_call_value(std::string_view call) {
    if (call.size() < 3 || call.size() > 6)
        return std::nullopt;
    int area = -1;
    for (int i = int(call.size()) - 1; i >= 1; --i) {
        if (is_digit(call[i])) {
            area = i;
            break;
        }
    }
    if (area != 1 && area != 2)
        return std::nullopt;
    int prefix_letters = 0;
    for (int i = 0; i < area; ++i) {
        if (is_letter(call[i]))
            ++prefix_letters;
        else if (!is_digit(call[i]))
            return std::nullopt;
    }
    if (prefix_letters == 0 || call[0] == 'Q')
        return std::nullopt;
    if (call.size() - size_t(area) - 1 > 3)
        return std::nullopt;
    for (size_t i = size_t(area) + 1; i < call.size(); ++i)
        if (!is_letter(call[i]))
            return std::nullopt;
    char f[6] = {' ', ' ', ' ', ' ', ' ', ' '};
    const int shift = area == 1 ? 1 : 0;
    for (size_t i = 0; i < call.size(); ++i)
        f[i + shift] = call[i];
    const int i1 = index_in(kA1, f[0]), i2 = index_in(kA2, f[1]), i3 = index_in("0123456789", f[2]);
    const int i4 = index_in(kA4, f[3]), i5 = index_in(kA4, f[4]), i6 = index_in(kA4, f[5]);
    if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0 || i6 < 0)
        return std::nullopt;
    return uint32_t(((((i1 * 36 + i2) * 10 + i3) * 27 + i4) * 27 + i5) * 27 + i6);
}

std::optional<std::string> std_call_text(uint32_t n) {
    if (n >= kStdCalls)
        return std::nullopt;
    const uint32_t value = n;
    char f[7];
    f[5] = kA4[n % 27];
    n /= 27;
    f[4] = kA4[n % 27];
    n /= 27;
    f[3] = kA4[n % 27];
    n /= 27;
    f[2] = char('0' + n % 10);
    n /= 10;
    f[1] = kA2[n % 36];
    n /= 36;
    f[0] = kA1[n];
    f[6] = '\0';
    const std::string call = trim(f);
    // Canonical only: the call must pack to the same value again, which rules
    // out spaces inside it and letters after a gap.
    const auto back = std_call_value(call);
    if (!back || *back != value)
        return std::nullopt;
    return call;
}

bool is_cq_modifier(std::string_view w) {
    if (w.size() == 3 && is_digit(w[0]) && is_digit(w[1]) && is_digit(w[2]))
        return true;
    if (w.empty() || w.size() > 4)
        return false;
    for (char c : w)
        if (!is_letter(c))
            return false;
    return true;
}

uint32_t cq_modifier_value(std::string_view w) {
    if (is_digit(w[0]))
        return 3 + uint32_t(std::stoi(std::string(w)));
    uint32_t m = 0;
    for (size_t i = 0; i < 4 - w.size(); ++i)
        m = 27 * m;
    for (char c : w)
        m = 27 * m + uint32_t(c - 'A' + 1);
    return 1003 + m;
}

std::string hashed_text(const CallsignHashTable* hashes, int bits, uint32_t h, int64_t now) {
    if (hashes) {
        if (auto call = hashes->lookup(bits, h, now))
            return "<" + *call + ">";
    }
    return "<...>";
}

struct C28 {
    enum Kind { De, Qrz, Cq, Hash, Call } kind = Call;
    std::string text;      // as printed
    std::string modifier;  // after CQ
};

std::optional<C28> unpack_c28(uint32_t n, const CallsignHashTable* hashes, int64_t now) {
    C28 out;
    if (n < kTokens) {
        if (n == 0) {
            out.kind = C28::De;
            out.text = "DE";
        } else if (n == 1) {
            out.kind = C28::Qrz;
            out.text = "QRZ";
        } else if (n == 2) {
            out.kind = C28::Cq;
            out.text = "CQ";
        } else if (n <= 1002) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%03u", n - 3);
            out.kind = C28::Cq;
            out.modifier = buf;
            out.text = "CQ " + out.modifier;
        } else if (n >= 1004 && n <= 532443) {
            uint32_t m = n - 1003;
            char f[5];
            for (int i = 3; i >= 0; --i) {
                f[i] = kA4[m % 27];
                m /= 27;
            }
            f[4] = '\0';
            const std::string mod = trim(f);
            // Letters must be contiguous and right-aligned, as packed.
            if (mod.find(' ') != std::string::npos || f[3] == ' ')
                return std::nullopt;
            out.kind = C28::Cq;
            out.modifier = mod;
            out.text = "CQ " + mod;
        } else {
            return std::nullopt;  // 1003 and 532444..2063591 are unassigned
        }
        return out;
    }
    if (n - kTokens < kMax22) {
        out.kind = C28::Hash;
        out.text = hashed_text(hashes, 22, n - kTokens, now);
        return out;
    }
    auto call = std_call_text(n - kTokens - kMax22);
    if (!call)
        return std::nullopt;
    out.kind = C28::Call;
    out.text = *call;
    return out;
}

// ---- locators ----

std::optional<uint32_t> grid4_value(std::string_view g) {
    if (g.size() != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !is_digit(g[2]) || !is_digit(g[3]))
        return std::nullopt;
    return uint32_t((g[0] - 'A') * 1800 + (g[1] - 'A') * 100 + (g[2] - '0') * 10 + (g[3] - '0'));
}

std::string grid4_text(uint32_t n) {
    char g[5] = {char('A' + n / 1800), char('A' + (n / 100) % 18), char('0' + (n / 10) % 10), char('0' + n % 10), 0};
    return g;
}

std::optional<uint32_t> grid6_value(std::string_view g) {
    if (g.size() != 6)
        return std::nullopt;
    const auto g4 = grid4_value(g.substr(0, 4));
    if (!g4 || g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X')
        return std::nullopt;
    return *g4 * 576 + uint32_t((g[4] - 'A') * 24 + (g[5] - 'A'));
}

std::string grid6_text(uint32_t n) {
    std::string s = grid4_text(n / 576);
    s += char('A' + (n % 576) / 24);
    s += char('A' + n % 24);
    return s;
}

// ---- 71-bit fields: free text in base 42, telemetry in hex ----

// A 71-bit number in three 32-bit limbs, least significant first.
struct U71 {
    uint32_t w[3] = {0, 0, 0};
    void mul_add(uint32_t m, uint32_t a) {
        uint64_t carry = a;
        for (auto& x : w) {
            const uint64_t t = uint64_t(x) * m + carry;
            x = uint32_t(t);
            carry = t >> 32;
        }
    }
    uint32_t div(uint32_t d) {
        uint64_t rem = 0;
        for (int i = 2; i >= 0; --i) {
            const uint64_t t = (rem << 32) | w[i];
            w[i] = uint32_t(t / d);
            rem = t % d;
        }
        return uint32_t(rem);
    }
    bool fits71() const { return (w[2] >> 7) == 0; }
    bool zero() const { return w[0] == 0 && w[1] == 0 && w[2] == 0; }
    bool operator==(const U71& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2]; }
};

void put71(BitWriter& bw, const U71& v) {
    bw.put(v.w[2] & 0x7f, 7);
    bw.put(v.w[1], 32);
    bw.put(v.w[0], 32);
}

U71 get71(BitReader& br) {
    U71 v;
    v.w[2] = uint32_t(br.get(7));
    v.w[1] = uint32_t(br.get(32));
    v.w[0] = uint32_t(br.get(32));
    return v;
}

// protocol/free_text_to_f71.f90: up to 13 characters, right-justified.
std::optional<U71> free_text_value(std::string_view text) {
    if (text.size() > 13)
        return std::nullopt;
    U71 v;
    for (size_t i = 0; i < 13; ++i) {
        const size_t pad = 13 - text.size();
        const char c = i < pad ? ' ' : text[i - pad];
        const int k = index_in(kA42, c);
        if (k < 0)
            return std::nullopt;
        v.mul_add(42, uint32_t(k));
    }
    return v;
}

std::optional<std::string> free_text_of(U71 v) {
    const U71 original = v;
    char f[14];
    for (int i = 12; i >= 0; --i)
        f[i] = kA42[v.div(42)];
    f[13] = '\0';
    if (!v.zero())
        return std::nullopt;  // 42^13 or more
    const std::string text = trim(f);
    if (text.empty())
        return std::nullopt;
    const auto back = free_text_value(text);
    if (!back || !(*back == original))
        return std::nullopt;  // trailing spaces inside the field: not as packed
    return text;
}

// ---- c58: nonstandard calls ----

bool is_nonstandard_call(std::string_view call) {
    if (call.size() < 3 || call.size() > 11)
        return false;
    bool digit = false, letter = false;
    for (char c : call) {
        if (is_digit(c))
            digit = true;
        else if (is_letter(c))
            letter = true;
        else if (c != '/')
            return false;
    }
    return digit && letter && call.front() != '/' && call.back() != '/';
}

// Right-justified in 11 characters, as WSJT-X sends it.
uint64_t c58_value(std::string_view call) {
    uint64_t n = 0;
    const size_t pad = 11 - call.size();
    for (size_t i = 0; i < 11; ++i)
        n = 38 * n + uint64_t(index_in(kA38, i < pad ? ' ' : call[i - pad]));
    return n;
}

std::optional<std::string> c58_text(uint64_t n) {
    char f[12];
    for (int i = 10; i >= 0; --i) {
        f[i] = kA38[n % 38];
        n /= 38;
    }
    f[11] = '\0';
    if (n != 0)
        return std::nullopt;  // 38^11 or more
    const std::string call = trim(f);
    // WSJT-X right-justifies the call and protocol/nonstd_to_c58.f90 takes
    // it left-justified; either way there are no spaces inside it.
    if ((f[0] == ' ' && f[10] == ' ') || !is_nonstandard_call(call))
        return std::nullopt;
    return call;
}

std::string report_text(int db) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%+03d", db);
    return buf;
}

// Reports in g15 as WSJT-X packs them: -30..+50 directly, -50..-31 folded
// above that.
std::optional<uint32_t> report_irpt(int db) {
    if (db >= -30 && db <= 50)
        return uint32_t(db + 35);
    if (db >= -50 && db <= -31)
        return uint32_t(db + 101 + 35);
    return std::nullopt;
}

std::optional<int> parse_report(std::string_view w) {
    if (w.size() < 2 || w.size() > 3 || (w[0] != '+' && w[0] != '-'))
        return std::nullopt;
    for (size_t i = 1; i < w.size(); ++i)
        if (!is_digit(w[i]))
            return std::nullopt;
    const int v = std::stoi(std::string(w.substr(1)));
    return w[0] == '-' ? -v : v;
}

// ---- unpacking by type ----

std::optional<Message> unpack_standard(const Payload& p, int i3, const CallsignHashTable* hashes, int64_t now,
                                       const UnpackOptions& options) {
    BitReader br(p);
    const uint32_t n28a = uint32_t(br.get(28));
    const int ipa = int(br.get(1));
    const uint32_t n28b = uint32_t(br.get(28));
    const int ipb = int(br.get(1));
    const int ir = int(br.get(1));
    const uint32_t igrid4 = uint32_t(br.get(15));
    auto a = unpack_c28(n28a, hashes, now);
    auto b = unpack_c28(n28b, hashes, now);
    if (!a || !b || (b->kind != C28::Call && b->kind != C28::Hash))
        return std::nullopt;
    // The suffix bits belong to calls sent in full.
    if ((ipa && a->kind != C28::Call) || (ipb && b->kind != C28::Call))
        return std::nullopt;
    if (i3 == 1 && (ipa || ipb) && !options.contest_forms)
        return std::nullopt;
    // Type 2 exists for /P; without one the message goes as type 1.
    if (i3 == 2 && !ipa && !ipb)
        return std::nullopt;
    const char* suffix = i3 == 1 ? "/R" : "/P";
    std::string call1 = a->text + (ipa ? suffix : "");
    std::string call2 = b->text + (ipb ? suffix : "");
    const bool cq = a->kind == C28::Cq;

    Message m;
    m.i3 = i3;
    m.n3 = 0;
    MessageFields& f = m.fields;
    f.to = call1;
    f.de = call2;
    f.cq = cq;
    f.cq_modifier = a->modifier;
    if (b->kind == C28::Call) {
        f.de_call = call2;
        m.learned_calls.push_back(call2);
    }
    std::string text = call1 + " " + call2;
    if (igrid4 < kMaxGrid4) {
        const std::string grid = grid4_text(igrid4);
        if (cq && ir)
            return std::nullopt;
        if (grid == "RR73") {
            // WSJT-X sends RR73 as the locator of that name, while
            // protocol/grid4_to_g15.f90 gives it a value of its own (below);
            // both mean the word, and the square is open sea.
            if (ir)
                return std::nullopt;
            f.report = "RR73";
            text += " RR73";
        } else {
            f.grid = grid;
            text += ir ? " R " + f.grid : " " + f.grid;
            if (ir)
                f.report = "R";
        }
    } else {
        const uint32_t irpt = igrid4 - kMaxGrid4;
        if (irpt == 0 || irpt > 105)
            return std::nullopt;
        if (irpt <= 4) {
            // Blank, RRR, RR73 and 73 never carry the R bit, and after CQ only
            // the blank is sent.
            if (ir || (cq && irpt != 1))
                return std::nullopt;
            static const char* const words[5] = {"", "", "RRR", "RR73", "73"};
            f.report = words[irpt];
            if (irpt > 1)
                text += std::string(" ") + words[irpt];
        } else {
            if (cq)
                return std::nullopt;
            int db = int(irpt) - 35;
            if (db > 50)
                db -= 101;
            f.report = (ir ? "R" : "") + report_text(db);
            f.report_db = db;
            text += " " + f.report;
        }
    }
    m.text = text;
    return m;
}

std::optional<Message> unpack_dxpedition(const Payload& p, const CallsignHashTable* hashes, int64_t now) {
    BitReader br(p);
    const uint32_t n28a = uint32_t(br.get(28));
    const uint32_t n28b = uint32_t(br.get(28));
    const uint32_t n10 = uint32_t(br.get(10));
    const int n5 = int(br.get(5));
    auto a = unpack_c28(n28a, hashes, now);
    auto b = unpack_c28(n28b, hashes, now);
    if (!a || !b || (a->kind != C28::Call && a->kind != C28::Hash) || (b->kind != C28::Call && b->kind != C28::Hash))
        return std::nullopt;
    Message m;
    m.i3 = 0;
    m.n3 = 1;
    const int db = 2 * n5 - 30;
    const std::string fox = hashed_text(hashes, 10, n10, now);
    m.text = a->text + " RR73; " + b->text + " " + fox + " " + report_text(db);
    // The Fox sends this; its call is only a hash.
    m.fields.to = a->text;
    m.fields.de = fox;
    m.fields.report = report_text(db);
    m.fields.report_db = db;
    return m;
}

std::optional<Message> unpack_field_day(const Payload& p, int n3, const CallsignHashTable* hashes, int64_t now) {
    BitReader br(p);
    const uint32_t n28a = uint32_t(br.get(28));
    const uint32_t n28b = uint32_t(br.get(28));
    const int ir = int(br.get(1));
    const int intx = int(br.get(4));
    const int nclass = int(br.get(3));
    const int isec = int(br.get(7));
    if (isec < 1 || isec > kArrlSectionCount || nclass > 5)
        return std::nullopt;
    auto a = unpack_c28(n28a, hashes, now);
    auto b = unpack_c28(n28b, hashes, now);
    if (!a || !b || (a->kind != C28::Call && a->kind != C28::Hash) || (b->kind != C28::Call && b->kind != C28::Hash))
        return std::nullopt;
    const int ntx = intx + 1 + (n3 == 4 ? 16 : 0);
    const std::string exch = std::to_string(ntx) + char('A' + nclass);
    Message m;
    m.i3 = 0;
    m.n3 = n3;
    m.text = a->text + " " + b->text + (ir ? " R " : " ") + exch + " " + kArrlSections[isec - 1];
    m.fields.to = a->text;
    m.fields.de = b->text;
    if (b->kind == C28::Call) {
        m.fields.de_call = b->text;
        m.learned_calls.push_back(b->text);
    }
    m.fields.report = (ir ? "R " : "") + exch;
    return m;
}

std::optional<Message> unpack_telemetry(const Payload& p) {
    BitReader br(p);
    U71 v = get71(br);
    if (v.zero())
        return std::nullopt;
    char hex[19];
    for (int i = 17; i >= 0; --i)
        hex[i] = "0123456789ABCDEF"[v.div(16)];
    hex[18] = '\0';
    const char* s = hex;
    while (*s == '0')
        ++s;
    Message m;
    m.i3 = 0;
    m.n3 = 5;
    m.text = s;
    return m;
}

std::optional<Message> unpack_rtty(const Payload& p, const CallsignHashTable* hashes, int64_t now,
                                   const UnpackOptions& options) {
    BitReader br(p);
    const int itu = int(br.get(1));
    const uint32_t n28a = uint32_t(br.get(28));
    const uint32_t n28b = uint32_t(br.get(28));
    const int ir = int(br.get(1));
    const int irpt = int(br.get(3));
    const int nexch = int(br.get(13));
    if (itu && !options.contest_forms)
        return std::nullopt;
    auto a = unpack_c28(n28a, hashes, now);
    auto b = unpack_c28(n28b, hashes, now);
    if (!a || !b || (a->kind != C28::Call && a->kind != C28::Hash) || (b->kind != C28::Call && b->kind != C28::Hash))
        return std::nullopt;
    std::string exch;
    if (nexch > 8000 && nexch <= 8000 + kStateCount) {
        exch = kStatesProvinces[nexch - 8001];
    } else if (nexch >= 1 && nexch <= 7999) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04d", nexch);
        exch = buf;
    } else {
        return std::nullopt;
    }
    const std::string rst = std::string("5") + char('2' + irpt) + "9";
    Message m;
    m.i3 = 3;
    m.n3 = 0;
    m.text = std::string(itu ? "TU; " : "") + a->text + " " + b->text + (ir ? " R " : " ") + rst + " " + exch;
    m.fields.to = a->text;
    m.fields.de = b->text;
    if (b->kind == C28::Call) {
        m.fields.de_call = b->text;
        m.learned_calls.push_back(b->text);
    }
    m.fields.report = rst;
    return m;
}

std::optional<Message> unpack_nonstandard(const Payload& p, const CallsignHashTable* hashes, int64_t now) {
    BitReader br(p);
    const uint32_t n12 = uint32_t(br.get(12));
    const uint64_t n58 = br.get(58);
    const int iflip = int(br.get(1));
    const int nrpt = int(br.get(2));
    const int icq = int(br.get(1));
    auto call = c58_text(n58);
    if (!call)
        return std::nullopt;
    Message m;
    m.i3 = 4;
    m.n3 = 0;
    m.learned_calls.push_back(*call);
    if (icq) {
        // "CQ <...>" is refused: a CQ always carries the full call, and its
        // hash field holds the hash of that call.
        if (iflip || nrpt || n12 != *callsign_hash(*call, 12))
            return std::nullopt;
        m.text = "CQ " + *call;
        m.fields.to = "CQ";
        m.fields.cq = true;
        m.fields.de = *call;
        m.fields.de_call = *call;
        return m;
    }
    const std::string hashed = hashed_text(hashes, 12, n12, now);
    static const char* const words[4] = {"", "RRR", "RR73", "73"};
    if (iflip == 0) {
        m.fields.to = hashed;
        m.fields.de = *call;
        m.fields.de_call = *call;
    } else {
        m.fields.to = *call;
        m.fields.de = hashed;
    }
    m.fields.report = words[nrpt];
    m.text = m.fields.to + " " + m.fields.de + (nrpt ? std::string(" ") + words[nrpt] : std::string());
    return m;
}

std::optional<Message> unpack_eu_vhf(const Payload& p, const CallsignHashTable* hashes, int64_t now) {
    BitReader br(p);
    const uint32_t n12 = uint32_t(br.get(12));
    const uint32_t n22 = uint32_t(br.get(22));
    const int ir = int(br.get(1));
    const int irpt = int(br.get(3));
    const int serial = int(br.get(11));
    const uint32_t igrid6 = uint32_t(br.get(25));
    if (igrid6 >= kMaxGrid6)
        return std::nullopt;
    Message m;
    m.i3 = 5;
    m.n3 = 0;
    m.fields.to = hashed_text(hashes, 12, n12, now);
    m.fields.de = hashed_text(hashes, 22, n22, now);
    m.fields.grid = grid6_text(igrid6);
    char exch[8];
    std::snprintf(exch, sizeof exch, "%2d%04d", 52 + irpt, serial);
    m.fields.report = exch;
    m.text = m.fields.to + " " + m.fields.de + (ir ? " R " : " ") + exch + " " + m.fields.grid;
    return m;
}

// ---- packing ----

struct CallWord {
    enum Kind { Std, Hash, Nonstd } kind = Std;
    std::string call;  // without brackets or suffix
    char suffix = 0;   // 'R' or 'P'
};

std::optional<CallWord> parse_call(std::string_view w) {
    CallWord c;
    if (w.size() >= 3 && w.front() == '<' && w.back() == '>') {
        c.kind = CallWord::Hash;
        c.call = std::string(w.substr(1, w.size() - 2));
        if (c.call.size() < 3 || !callsign_hash(c.call, 22))
            return std::nullopt;
        return c;
    }
    if (w.size() > 2 && (w.substr(w.size() - 2) == "/R" || w.substr(w.size() - 2) == "/P")) {
        if (std_call_value(w.substr(0, w.size() - 2))) {
            c.kind = CallWord::Std;
            c.call = std::string(w.substr(0, w.size() - 2));
            c.suffix = w.back();
            return c;
        }
    }
    if (std_call_value(w)) {
        c.kind = CallWord::Std;
        c.call = std::string(w);
        return c;
    }
    if (is_nonstandard_call(w)) {
        c.kind = CallWord::Nonstd;
        c.call = std::string(w);
        return c;
    }
    return std::nullopt;
}

// c28 of a call word that fits in it: a standard call or a hash.
std::optional<uint32_t> call_c28(const CallWord& c, CallsignHashTable* hashes, int64_t now) {
    if (c.kind == CallWord::Std)
        return kTokens + kMax22 + *std_call_value(c.call);
    if (c.kind == CallWord::Hash) {
        if (hashes)
            hashes->remember(c.call, now);
        return kTokens + *callsign_hash(c.call, 22);
    }
    return std::nullopt;
}

std::optional<Payload> pack_standard(const std::vector<std::string>& w, CallsignHashTable* hashes, int64_t now) {
    size_t i = 0;
    uint32_t c28a = 0;
    bool cq = false;
    CallWord first;
    bool first_is_call = false;
    if (w.empty())
        return std::nullopt;
    if (w[0] == "CQ") {
        cq = true;
        if (w.size() >= 3 && is_cq_modifier(w[1]) && parse_call(w[2])) {
            c28a = cq_modifier_value(w[1]);
            i = 2;
        } else {
            c28a = 2;
            i = 1;
        }
    } else if (w[0] == "DE" || w[0] == "QRZ") {
        c28a = w[0] == "DE" ? 0 : 1;
        i = 1;
    } else {
        auto c = parse_call(w[0]);
        if (!c || c->kind == CallWord::Nonstd)
            return std::nullopt;
        first = *c;
        first_is_call = true;
        i = 1;
    }
    if (i >= w.size())
        return std::nullopt;
    auto second = parse_call(w[i]);
    if (!second || second->kind == CallWord::Nonstd)
        return std::nullopt;
    ++i;
    char suffix = second->suffix;
    if (first_is_call && first.suffix) {
        if (suffix && suffix != first.suffix)
            return std::nullopt;
        suffix = first.suffix;
    }
    int ir = 0;
    uint32_t g15 = kMaxGrid4 + 1;
    std::vector<std::string> rest(w.begin() + long(i), w.end());
    if (!rest.empty() && rest[0] == "R" && rest.size() == 2) {
        ir = 1;
        rest.erase(rest.begin());
    }
    if (rest.size() > 1)
        return std::nullopt;
    if (rest.size() == 1) {
        const std::string& x = rest[0];
        if (x == "RRR" && !ir)
            g15 = kMaxGrid4 + 2;
        else if (x == "RR73" && !ir)
            g15 = *grid4_value("RR73");  // as WSJT-X sends it
        else if (x == "73" && !ir)
            g15 = kMaxGrid4 + 4;
        else if (auto g = grid4_value(x))
            g15 = *g;
        else {
            std::string_view r = x;
            if (!ir && r.size() > 1 && r[0] == 'R' && (r[1] == '+' || r[1] == '-')) {
                ir = 1;
                r.remove_prefix(1);
            } else if (ir) {
                return std::nullopt;  // "R" before something not a grid
            }
            auto db = parse_report(r);
            if (!db)
                return std::nullopt;
            auto irpt = report_irpt(*db);
            if (!irpt)
                return std::nullopt;
            g15 = kMaxGrid4 + *irpt;
        }
    }
    if (cq && (ir || g15 > kMaxGrid4 + 1))
        return std::nullopt;
    if (first_is_call)
        c28a = *call_c28(first, hashes, now);
    const uint32_t c28b = *call_c28(*second, hashes, now);
    BitWriter bw;
    bw.put(c28a, 28);
    bw.put(first_is_call && first.suffix ? 1 : 0, 1);
    bw.put(c28b, 28);
    bw.put(second->suffix ? 1 : 0, 1);
    bw.put(uint64_t(ir), 1);
    bw.put(g15, 15);
    bw.put(suffix == 'P' ? 2 : 1, 3);
    return bw.payload();
}

std::optional<Payload> pack_nonstandard(const std::vector<std::string>& w, CallsignHashTable* hashes, int64_t now) {
    BitWriter bw;
    if (w.size() == 2 && w[0] == "CQ") {
        auto c = parse_call(w[1]);
        if (!c || c->kind != CallWord::Nonstd)
            return std::nullopt;
        bw.put(*callsign_hash(c->call, 12), 12);
        bw.put(c58_value(c->call), 58);
        bw.put(0, 1);
        bw.put(0, 2);
        bw.put(1, 1);
        bw.put(4, 3);
        return bw.payload();
    }
    if (w.size() != 2 && w.size() != 3)
        return std::nullopt;
    auto a = parse_call(w[0]);
    auto b = parse_call(w[1]);
    if (!a || !b)
        return std::nullopt;
    int iflip;
    const CallWord* hashed;
    const CallWord* full;
    if (a->kind == CallWord::Hash && b->kind == CallWord::Nonstd) {
        iflip = 0;
        hashed = &*a;
        full = &*b;
    } else if (a->kind == CallWord::Nonstd && b->kind == CallWord::Hash) {
        iflip = 1;
        hashed = &*b;
        full = &*a;
    } else {
        return std::nullopt;
    }
    int nrpt = 0;
    if (w.size() == 3) {
        if (w[2] == "RRR")
            nrpt = 1;
        else if (w[2] == "RR73")
            nrpt = 2;
        else if (w[2] == "73")
            nrpt = 3;
        else
            return std::nullopt;
    }
    if (hashes)
        hashes->remember(hashed->call, now);
    bw.put(*callsign_hash(hashed->call, 12), 12);
    bw.put(c58_value(full->call), 58);
    bw.put(uint64_t(iflip), 1);
    bw.put(uint64_t(nrpt), 2);
    bw.put(0, 1);
    bw.put(4, 3);
    return bw.payload();
}

std::optional<Payload> pack_dxpedition(const std::vector<std::string>& w, CallsignHashTable* hashes, int64_t now) {
    if (w.size() != 5 || w[1] != "RR73;")
        return std::nullopt;
    auto a = parse_call(w[0]);
    auto b = parse_call(w[2]);
    auto fox = parse_call(w[3]);
    auto db = parse_report(w[4]);
    if (!a || !b || !fox || !db || a->kind == CallWord::Nonstd || b->kind == CallWord::Nonstd || a->suffix ||
        b->suffix || fox->kind != CallWord::Hash)
        return std::nullopt;
    if (*db < -30 || *db > 32)
        return std::nullopt;
    if (hashes)
        hashes->remember(fox->call, now);
    BitWriter bw;
    bw.put(*call_c28(*a, hashes, now), 28);
    bw.put(*call_c28(*b, hashes, now), 28);
    bw.put(*callsign_hash(fox->call, 10), 10);
    bw.put(uint64_t((*db + 30) / 2), 5);
    bw.put(1, 3);
    bw.put(0, 3);
    return bw.payload();
}

int section_index(std::string_view s) {
    for (int i = 0; i < kArrlSectionCount; ++i)
        if (s == kArrlSections[i])
            return i + 1;
    return -1;
}

std::optional<Payload> pack_field_day(const std::vector<std::string>& w, CallsignHashTable* hashes, int64_t now) {
    if (w.size() != 4 && w.size() != 5)
        return std::nullopt;
    const int ir = w.size() == 5 ? 1 : 0;
    if (ir && w[2] != "R")
        return std::nullopt;
    auto a = parse_call(w[0]);
    auto b = parse_call(w[1]);
    if (!a || !b || a->kind == CallWord::Nonstd || b->kind == CallWord::Nonstd || a->suffix || b->suffix)
        return std::nullopt;
    const std::string& exch = w[2 + size_t(ir)];
    const int sec = section_index(w[3 + size_t(ir)]);
    if (exch.size() < 2 || exch.size() > 3 || sec < 0)
        return std::nullopt;
    const char cls = exch.back();
    if (cls < 'A' || cls > 'F')
        return std::nullopt;
    for (size_t i = 0; i + 1 < exch.size(); ++i)
        if (!is_digit(exch[i]))
            return std::nullopt;
    const int ntx = std::stoi(exch.substr(0, exch.size() - 1));
    if (ntx < 1 || ntx > 32)
        return std::nullopt;
    BitWriter bw;
    bw.put(*call_c28(*a, hashes, now), 28);
    bw.put(*call_c28(*b, hashes, now), 28);
    bw.put(uint64_t(ir), 1);
    bw.put(uint64_t(ntx <= 16 ? ntx - 1 : ntx - 17), 4);
    bw.put(uint64_t(cls - 'A'), 3);
    bw.put(uint64_t(sec), 7);
    bw.put(ntx <= 16 ? 3 : 4, 3);
    bw.put(0, 3);
    return bw.payload();
}

std::optional<Payload> pack_rtty(std::vector<std::string> w, CallsignHashTable* hashes, int64_t now) {
    int itu = 0;
    if (!w.empty() && w[0] == "TU;") {
        itu = 1;
        w.erase(w.begin());
    }
    if (w.size() != 4 && w.size() != 5)
        return std::nullopt;
    const int ir = w.size() == 5 ? 1 : 0;
    if (ir && w[2] != "R")
        return std::nullopt;
    auto a = parse_call(w[0]);
    auto b = parse_call(w[1]);
    if (!a || !b || a->kind == CallWord::Nonstd || b->kind == CallWord::Nonstd || a->suffix || b->suffix)
        return std::nullopt;
    const std::string& rst = w[2 + size_t(ir)];
    const std::string& exch = w[3 + size_t(ir)];
    if (rst.size() != 3 || rst[0] != '5' || rst[2] != '9' || rst[1] < '2' || rst[1] > '9')
        return std::nullopt;
    int nexch = -1;
    for (int i = 0; i < kStateCount; ++i)
        if (exch == kStatesProvinces[i])
            nexch = 8001 + i;
    if (nexch < 0) {
        if (exch.empty() || exch.size() > 4)
            return std::nullopt;
        for (char c : exch)
            if (!is_digit(c))
                return std::nullopt;
        nexch = std::stoi(exch);
        if (nexch < 1 || nexch > 7999)
            return std::nullopt;
    }
    BitWriter bw;
    bw.put(uint64_t(itu), 1);
    bw.put(*call_c28(*a, hashes, now), 28);
    bw.put(*call_c28(*b, hashes, now), 28);
    bw.put(uint64_t(ir), 1);
    bw.put(uint64_t(rst[1] - '2'), 3);
    bw.put(uint64_t(nexch), 13);
    bw.put(3, 3);
    return bw.payload();
}

std::optional<Payload> pack_eu_vhf(const std::vector<std::string>& w, CallsignHashTable* hashes, int64_t now) {
    if (w.size() != 4 && w.size() != 5)
        return std::nullopt;
    const int ir = w.size() == 5 ? 1 : 0;
    if (ir && w[2] != "R")
        return std::nullopt;
    auto a = parse_call(w[0]);
    auto b = parse_call(w[1]);
    if (!a || !b || a->kind != CallWord::Hash || b->kind != CallWord::Hash)
        return std::nullopt;
    const std::string& exch = w[2 + size_t(ir)];
    const auto g = grid6_value(w[3 + size_t(ir)]);
    if (exch.size() != 6 || !g)
        return std::nullopt;
    for (char c : exch)
        if (!is_digit(c))
            return std::nullopt;
    const int rs = std::stoi(exch.substr(0, 2));
    const int serial = std::stoi(exch.substr(2));
    if (rs < 52 || rs > 59 || serial > 2047)
        return std::nullopt;
    if (hashes) {
        hashes->remember(a->call, now);
        hashes->remember(b->call, now);
    }
    BitWriter bw;
    bw.put(*callsign_hash(a->call, 12), 12);
    bw.put(*callsign_hash(b->call, 22), 22);
    bw.put(uint64_t(ir), 1);
    bw.put(uint64_t(rs - 52), 3);
    bw.put(uint64_t(serial), 11);
    bw.put(*g, 25);
    bw.put(5, 3);
    return bw.payload();
}

std::optional<Payload> pack_telemetry(std::string_view text) {
    if (text.empty() || text.size() > 18)
        return std::nullopt;
    U71 v;
    for (char c : text) {
        const int k = index_in("0123456789ABCDEF", c);
        if (k < 0)
            return std::nullopt;
        v.mul_add(16, uint32_t(k));
    }
    if (!v.fits71() || v.zero())
        return std::nullopt;
    BitWriter bw;
    put71(bw, v);
    bw.put(5, 3);
    bw.put(0, 3);
    return bw.payload();
}

std::optional<Payload> pack_free_text(std::string_view text) {
    auto v = free_text_value(text);
    if (!v || text.empty())
        return std::nullopt;
    BitWriter bw;
    put71(bw, *v);
    bw.put(0, 3);
    bw.put(0, 3);
    return bw.payload();
}

}  // namespace

bool is_standard_call(std::string_view call) { return std_call_value(call).has_value(); }

std::optional<Message> unpack_message(const Payload& payload, const CallsignHashTable* hashes, int64_t now,
                                      const UnpackOptions& options) {
    // The three bits after the payload must be zero in this representation.
    if (payload[9] & 0x07)
        return std::nullopt;
    const int n3 = (payload_bit(payload, 71) << 2) | (payload_bit(payload, 72) << 1) | payload_bit(payload, 73);
    const int i3 = (payload_bit(payload, 74) << 2) | (payload_bit(payload, 75) << 1) | payload_bit(payload, 76);
    switch (i3) {
    case 0:
        switch (n3) {
        case 0: {
            BitReader br(payload);
            auto text = free_text_of(get71(br));
            if (!text)
                return std::nullopt;
            Message m;
            m.text = *text;
            return m;
        }
        case 1:
            return unpack_dxpedition(payload, hashes, now);
        case 3:
        case 4:
            return unpack_field_day(payload, n3, hashes, now);
        case 5:
            return unpack_telemetry(payload);
        default:
            // 0.2 and 0.6 and 0.7 are not FT8 messages ([QEX] Table 1).
            return std::nullopt;
        }
    case 1:
    case 2:
        return unpack_standard(payload, i3, hashes, now, options);
    case 3:
        return unpack_rtty(payload, hashes, now, options);
    case 4:
        return unpack_nonstandard(payload, hashes, now);
    case 5:
        return unpack_eu_vhf(payload, hashes, now);
    default:
        return std::nullopt;  // 6 and 7 are reserved
    }
}

std::optional<Payload> pack_message(std::string_view text_in, CallsignHashTable* hashes, int64_t now) {
    std::string text;
    for (char c : text_in)
        text += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
    text = trim(text);
    const std::vector<std::string> w = split_words(text);
    if (w.empty())
        return std::nullopt;
    if (auto p = pack_dxpedition(w, hashes, now))
        return p;
    if (auto p = pack_standard(w, hashes, now))
        return p;
    if (auto p = pack_nonstandard(w, hashes, now))
        return p;
    if (auto p = pack_field_day(w, hashes, now))
        return p;
    if (auto p = pack_rtty(w, hashes, now))
        return p;
    if (auto p = pack_eu_vhf(w, hashes, now))
        return p;
    if (w.size() == 1)
        if (auto p = pack_telemetry(w[0]))
            return p;
    return pack_free_text(text);
}

const char* message_type_name(int i3, int n3) {
    if (i3 == 0) {
        switch (n3) {
        case 0:
            return "Free text";
        case 1:
            return "DXpedition mode";
        case 3:
        case 4:
            return "ARRL Field Day";
        case 5:
            return "Telemetry";
        default:
            return "Reserved";
        }
    }
    switch (i3) {
    case 1:
        return "Standard msg";
    case 2:
        return "EU VHF Contest";
    case 3:
        return "ARRL RTTY Roundup";
    case 4:
        return "Nonstandard call";
    case 5:
        return "EU VHF Contest";
    default:
        return "Reserved";
    }
}

}  // namespace fern::ft8
