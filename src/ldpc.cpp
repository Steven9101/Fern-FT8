// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ldpc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace fern::ft8 {

namespace {

// ---- belief propagation ----

struct Edges {
    // For check c and its k-th bit, which of that bit's three checks c is.
    uint8_t slot[kLdpcM][7];
};

constexpr float kMinSumScale = 0.75f;

const Edges& edges() {
    static const Edges e = [] {
        Edges x{};
        const LdpcGraph& g = ldpc_graph();
        for (int c = 0; c < kLdpcM; ++c)
            for (int k = 0; k < g.check_degree[c]; ++k) {
                const int bit = g.check_bits[c][k];
                for (int s = 0; s < 3; ++s)
                    if (g.bit_checks[bit][s] == c)
                        x.slot[c][k] = uint8_t(s);
            }
        return x;
    }();
    return e;
}

// ---- bit-packed rows for OSD ----

struct Row {
    uint64_t w[3];
    void flip(int j) { w[j >> 6] ^= uint64_t(1) << (j & 63); }
    int get(int j) const { return int((w[j >> 6] >> (j & 63)) & 1); }
    void operator^=(const Row& o) {
        w[0] ^= o.w[0];
        w[1] ^= o.w[1];
        w[2] ^= o.w[2];
    }
};

// Column c of the systematic generator [I | P]: which of the 91 rows have a
// one there, as a 91-bit mask.
struct GeneratorColumns {
    uint64_t mask[kLdpcN][2];
};

const GeneratorColumns& generator_columns() {
    static const GeneratorColumns gc = [] {
        GeneratorColumns x{};
        for (int i = 0; i < kLdpcK; ++i)
            x.mask[i][i >> 6] |= uint64_t(1) << (i & 63);
        for (int p = 0; p < kLdpcM; ++p)
            for (int i = 0; i < kLdpcK; ++i)
                if ((kLdpcGenerator[p][i >> 3] >> (7 - (i & 7))) & 1)
                    x.mask[kLdpcK + p][i >> 6] |= uint64_t(1) << (i & 63);
        return x;
    }();
    return gc;
}

float weighted_distance(const Row& diff, const float* weight) {
    float d = 0.0f;
    for (int w = 0; w < 3; ++w) {
        uint64_t bits = diff.w[w];
        while (bits) {
            const int b = __builtin_ctzll(bits);
            d += weight[w * 64 + b];
            bits &= bits - 1;
        }
    }
    return d;
}

}  // namespace

namespace {

// Four independent min-sum decoders in the lanes of a 4 x 32-bit vector:
// SSE2 on x86-64 and NEON on aarch64 are part of the baseline, so GCC's
// vector extensions compile to them with no run-time dispatch, and to plain
// scalar code elsewhere. Every lane does exactly the arithmetic of a single
// decoder (the same operations in the same order, no fused multiply-add),
// so its results are bit for bit those of bp_decode() on that lane's input.
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

constexpr int kLanes = 4;

inline v4f vabs(v4f x) {
    v4i bits;
    std::memcpy(&bits, &x, sizeof bits);
    bits &= 0x7fffffff;
    v4f out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

inline v4i sign_bits(v4f x) {
    v4i bits;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits >> 31) & 1;  // 1 where the sign bit is set, as for x < 0 but also -0
}

inline v4f with_sign(v4f mag, v4i sign) {
    v4i bits;
    std::memcpy(&bits, &mag, sizeof bits);
    bits |= sign << 31;
    v4f out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

// One run of up to max_iterations over the four inputs.
void bp_lanes(const Llrs* const* inputs, int max_iterations, BpResult* results) {
    constexpr int lanes = kLanes;
    const LdpcGraph& g = ldpc_graph();
    const Edges& e = edges();
    v4f llr[kLdpcN];
    for (int b = 0; b < kLdpcN; ++b)
        for (int l = 0; l < kLanes; ++l)
            llr[b][l] = (*inputs[l])[size_t(b)];
    v4f q[kLdpcM][7];
    v4f r[kLdpcN][3];
    std::memset(r, 0, sizeof r);
    for (int c = 0; c < kLdpcM; ++c)
        for (int k = 0; k < g.check_degree[c]; ++k)
            q[c][k] = llr[g.check_bits[c][k]];
    bool active[kLanes];
    int best_unsatisfied[kLanes], since_best[kLanes];
    for (int l = 0; l < kLanes; ++l) {
        active[l] = l < lanes;
        best_unsatisfied[l] = kLdpcM + 1;
        since_best[l] = 0;
    }
    int remaining = lanes;
    const v4f scale = {kMinSumScale, kMinSumScale, kMinSumScale, kMinSumScale};
    v4f total[kLdpcN];
    for (int it = 1; it <= max_iterations && remaining > 0; ++it) {
        // Check update, normalised min-sum: each output has the sign of the
        // product of the other inputs and 0.75 times the least of their
        // magnitudes. On FT8's code it decodes as well as the exact
        // sum-product rule (measured: 70 % against 68 % of 400 slots at
        // -20.75 dB) and needs no tanh or log. The least and second least
        // are tracked without branches; when two inputs tie for the least,
        // both outputs get that value, as they should.
        for (int c = 0; c < kLdpcM; ++c) {
            const int deg = g.check_degree[c];
            v4f m1 = {1e30f, 1e30f, 1e30f, 1e30f}, m2 = m1;
            v4i sign = {0, 0, 0, 0};
            for (int k = 0; k < deg; ++k) {
                const v4f a = vabs(q[c][k]);
                sign ^= sign_bits(q[c][k]);
                const v4f hi = a > m1 ? a : m1;
                m2 = hi < m2 ? hi : m2;
                m1 = a < m1 ? a : m1;
            }
            for (int k = 0; k < deg; ++k) {
                const v4f a = vabs(q[c][k]);
                const v4f least = a == m1 ? m2 : m1;
                r[g.check_bits[c][k]][e.slot[c][k]] = with_sign(scale * least, sign ^ sign_bits(q[c][k]));
            }
        }
        for (int b = 0; b < kLdpcN; ++b)
            total[b] = llr[b] + r[b][0] + r[b][1] + r[b][2];
        for (int c = 0; c < kLdpcM; ++c)
            for (int k = 0; k < g.check_degree[c]; ++k) {
                const int b = g.check_bits[c][k];
                q[c][k] = total[b] - r[b][e.slot[c][k]];
            }
        // Hard decisions are the sign bits; a check fails when its bits'
        // decisions have odd parity.
        v4i failing = {0, 0, 0, 0};
        for (int c = 0; c < kLdpcM; ++c) {
            v4i parity = {0, 0, 0, 0};
            for (int k = 0; k < g.check_degree[c]; ++k)
                parity ^= (total[g.check_bits[c][k]] < 0.0f) & 1;
            failing += parity;
        }
        for (int l = 0; l < lanes; ++l) {
            if (!active[l])
                continue;
            const int unsatisfied = failing[l];
            BpResult& res = results[l];
            res.iterations = it;
            res.unsatisfied = unsatisfied;
            bool stop = false;
            if (unsatisfied == 0) {
                res.converged = true;
                stop = true;
            } else if (unsatisfied < best_unsatisfied[l]) {
                // Stop early when the count of failing checks stops falling:
                // such runs almost never converge later, and noise makes
                // most of them.
                best_unsatisfied[l] = unsatisfied;
                since_best[l] = 0;
            } else if (++since_best[l] >= 6 && it >= 8) {
                stop = true;
            }
            if (stop || it == max_iterations) {
                for (int b = 0; b < kLdpcN; ++b)
                    res.codeword[size_t(b)] = total[b][l] < 0.0f ? 1 : 0;
                active[l] = false;
                --remaining;
            }
        }
    }
}

}  // namespace

// The plain scalar decoder, one input at a time.
BpResult bp_decode(const Llrs& llr, int max_iterations, Llrs* beliefs) {
    const LdpcGraph& g = ldpc_graph();
    const Edges& e = edges();
    // Messages from bits to checks (q) and from checks to bits (r), the
    // latter stored per bit for the bit update.
    float q[kLdpcM][7];
    float r[kLdpcN][3] = {};
    for (int c = 0; c < kLdpcM; ++c)
        for (int k = 0; k < g.check_degree[c]; ++k)
            q[c][k] = llr[size_t(g.check_bits[c][k])];
    BpResult result;
    Codeword hard{};
    int best_unsatisfied = kLdpcM + 1;
    int since_best = 0;
    for (int it = 1; it <= max_iterations; ++it) {
        // Check update, normalised min-sum, as in bp_lanes().
        for (int c = 0; c < kLdpcM; ++c) {
            const int deg = g.check_degree[c];
            float m1 = 1e30f, m2 = 1e30f;
            int at = 0;
            uint32_t sign = 0;
            for (int k = 0; k < deg; ++k) {
                const float v = q[c][k];
                const float a = std::fabs(v);
                sign ^= v < 0 ? 1u : 0u;
                if (a < m1) {
                    m2 = m1;
                    m1 = a;
                    at = k;
                } else if (a < m2) {
                    m2 = a;
                }
            }
            for (int k = 0; k < deg; ++k) {
                const float mag = kMinSumScale * (k == at ? m2 : m1);
                const uint32_t sk = sign ^ (q[c][k] < 0 ? 1u : 0u);
                r[g.check_bits[c][k]][e.slot[c][k]] = sk ? -mag : mag;
            }
        }
        // Bit update and hard decisions.
        Llrs total;
        for (int b = 0; b < kLdpcN; ++b) {
            total[size_t(b)] = llr[size_t(b)] + r[b][0] + r[b][1] + r[b][2];
            hard[size_t(b)] = total[size_t(b)] < 0.0f ? 1 : 0;
        }
        for (int c = 0; c < kLdpcM; ++c)
            for (int k = 0; k < g.check_degree[c]; ++k) {
                const int b = g.check_bits[c][k];
                q[c][k] = total[size_t(b)] - r[b][e.slot[c][k]];
            }
        const int unsatisfied = parity_failures(hard);
        result.iterations = it;
        result.unsatisfied = unsatisfied;
        if (beliefs)
            *beliefs = total;
        if (unsatisfied == 0) {
            result.converged = true;
            result.codeword = hard;
            return result;
        }
        // Stop early when the count of failing checks stops falling: such
        // runs almost never converge later, and noise makes most of them.
        if (unsatisfied < best_unsatisfied) {
            best_unsatisfied = unsatisfied;
            since_best = 0;
        } else if (++since_best >= 6 && it >= 8) {
            break;
        }
    }
    result.codeword = hard;
    return result;
}

std::array<BpResult, 4> bp_decode4(const std::array<const Llrs*, 4>& llrs, int max_iterations) {
    std::array<BpResult, 4> results;
    bp_lanes(llrs.data(), max_iterations, results.data());
    return results;
}

int hard_disagreements(const Codeword& cw, const Llrs& llr) {
    int n = 0;
    for (int i = 0; i < kLdpcN; ++i)
        n += cw[size_t(i)] != (llr[size_t(i)] < 0.0f ? 1 : 0);
    return n;
}

OsdResult osd_decode(const Llrs& llr, const OsdOptions& options) {
    // Bits in order of falling reliability.
    std::array<int, kLdpcN> perm;
    std::iota(perm.begin(), perm.end(), 0);
    std::stable_sort(perm.begin(), perm.end(),
                     [&](int a, int b) { return std::fabs(llr[size_t(a)]) > std::fabs(llr[size_t(b)]); });
    float weight[3 * 64] = {};
    Row hard{};
    for (int j = 0; j < kLdpcN; ++j) {
        weight[j] = std::fabs(llr[size_t(perm[size_t(j)])]);
        if (llr[size_t(perm[size_t(j)])] < 0.0f)
            hard.flip(j);
    }
    // The generator with its columns in that order.
    const GeneratorColumns& gc = generator_columns();
    Row rows[kLdpcK] = {};
    for (int j = 0; j < kLdpcN; ++j) {
        const uint64_t* m = gc.mask[perm[size_t(j)]];
        for (int w = 0; w < 2; ++w) {
            uint64_t bits = m[w];
            while (bits) {
                const int i = w * 64 + __builtin_ctzll(bits);
                rows[i].flip(j);
                bits &= bits - 1;
            }
        }
    }
    // Gauss-Jordan elimination, taking pivots in order of reliability: the
    // pivot columns are the most reliable basis.
    int pivot[kLdpcK];
    int rank = 0;
    for (int j = 0; j < kLdpcN && rank < kLdpcK; ++j) {
        int found = -1;
        for (int i = rank; i < kLdpcK; ++i)
            if (rows[i].get(j)) {
                found = i;
                break;
            }
        if (found < 0)
            continue;
        std::swap(rows[rank], rows[found]);
        for (int i = 0; i < kLdpcK; ++i)
            if (i != rank && rows[i].get(j))
                rows[i] ^= rows[rank];
        pivot[rank++] = j;
    }
    OsdResult result;
    if (rank < kLdpcK)
        return result;  // cannot happen for a full-rank generator
    // Re-encode the hard decisions of the basis bits.
    Row c0{};
    for (int i = 0; i < kLdpcK; ++i)
        if (hard.get(pivot[i]))
            c0 ^= rows[i];
    // The nearest codeword tried whose CRC is right. The CRC is checked only
    // for codewords nearer than the best one so far, which is few of them.
    float best_d = 1e30f;
    Row best{};
    bool have = false;
    auto consider = [&](const Row& c) {
        Row d = c;
        d ^= hard;
        const float dist = weighted_distance(d, weight);
        if (dist >= best_d)
            return;
        Codeword cw{};
        for (int j = 0; j < kLdpcN; ++j)
            cw[size_t(perm[size_t(j)])] = uint8_t(c.get(j));
        if (!crc_ok(cw))
            return;
        best_d = dist;
        best = c;
        have = true;
    };
    consider(c0);
    // Order 1: every basis bit flipped once.
    for (int i = 0; i < kLdpcK; ++i) {
        Row c = c0;
        c ^= rows[i];
        consider(c);
    }
    // Order 2 among the least reliable basis bits, where a second error is
    // most likely.
    if (options.order >= 2) {
        const int first = std::max(0, kLdpcK - options.pair_span);
        for (int i = first; i < kLdpcK; ++i) {
            Row ci = c0;
            ci ^= rows[i];
            for (int k = i + 1; k < kLdpcK; ++k) {
                Row c = ci;
                c ^= rows[k];
                consider(c);
            }
        }
    }
    if (!have)
        return result;
    for (int j = 0; j < kLdpcN; ++j)
        result.codeword[size_t(perm[size_t(j)])] = uint8_t(best.get(j));
    Row d = best;
    d ^= hard;
    result.distance = best_d;
    result.disagreements = __builtin_popcountll(d.w[0]) + __builtin_popcountll(d.w[1]) + __builtin_popcountll(d.w[2]);
    result.found = true;
    return result;
}

}  // namespace fern::ft8
