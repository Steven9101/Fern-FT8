// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// FT4 slots, by the pipeline of decoder.cpp scaled to FT4's symbols: 48 ms
// instead of 160 ms, 4 tones 20.833 Hz apart instead of 8 at 6.25 Hz, four
// Costas arrays of 4 instead of three of 7. Per pass:
//  1. Spectrogram: 256-sample (one symbol) windows zero-padded to 512, so
//     bins are half a tone (10.42 Hz), every quarter symbol (12 ms).
//  2. Candidates: for each tone-0 bin and start frame, the power in the 16
//     Costas tones against the other tones of the same symbols; local
//     maxima above a threshold, strongest first.
//  3. Per candidate: cut the 260 Hz around it out of one FFT of the slot
//     and transform back: complex baseband at 333.3 samples a second, 16 per
//     symbol. Refine time and frequency on the Costas symbols, then on the
//     phases of neighbouring symbols, then coherently over the whole
//     transmission; cut the baseband again so that symbols start on its
//     samples, and take the 4 tone amplitudes of the 103.
//  4. Soft bits from blocks of 6, 4 and 1 symbols (noncoherent block
//     detection, [QEX] section 6, with longer blocks than its 1, 2 and 4),
//     and the interference-tolerant single-symbol set, tried with belief
//     propagation, then OSD.
//  5. A decode that passes the CRC and unpacks is rebuilt, its gain along
//     the transmission estimated, and subtracted.
// docs/DESIGN.md has the measurements behind the choices.
#include <algorithm>
#include <cmath>
#include <utility>

#include "decoder.h"
#include "fft.h"
#include "gfsk.h"
#include "ldpc.h"
#include "simd.h"
#include "slot_dsp.h"

namespace fern::ft8::ft4 {

namespace {

constexpr double kRate = kInternalRate;
constexpr int kSps = kInternalSamplesPerSymbol;
constexpr int kFrameStep = 64;
constexpr int kFrameLen = 256;
constexpr int kSpecFft = 512;
constexpr double kBinHz = kRate / kSpecFft;  // 10.417, half a tone
constexpr int kFrames = (kSlotSamples - kFrameLen) / kFrameStep + 1;
constexpr int kFramesPerSymbol = kSps / kFrameStep;
constexpr int kSlotFft = 65536;
constexpr double kSlotBinHz = kRate / kSlotFft;
constexpr int kDecimation = 16;
constexpr double kBbRate = kRate / kDecimation;  // 333.3
constexpr int kBbLen = kSlotFft / kDecimation;   // 12.3 s
constexpr int kBbSps = kSps / kDecimation;       // 16
// Where a transmission sent on time starts in the buffer: its first ramp
// symbol, 0.5 s into the slot; the first Costas array follows it.
constexpr double kNominalStart = kSlotLeadSeconds + kStartSeconds;
// DT from -1.0 to +1.0 s, the range jt9 searches (docs/PROTOCOL-SOURCES.md).
constexpr double kMaxDt = 1.0;
constexpr int kFirstSyncFrame = int((kNominalStart + kSymbolSeconds - kMaxDt) * kRate / kFrameStep);
constexpr int kLastSyncFrame = int((kNominalStart + kSymbolSeconds + kMaxDt) * kRate / kFrameStep) + 1;
constexpr double kCentreHz = 1.5 * kToneSpacingHz;
// Delays tried when refining the start from phases: +-8 ms in 0.125 ms.
constexpr int kTauSteps = 129;
constexpr double kTauStep = 0.000125;
// The block soft-bit sets: blocks of `span` symbols, the first of each
// group of 29 `first_len` long when that is not 0.
struct BlockSet {
    int span;
    int first_len;
};
constexpr BlockSet kBlockSets[3] = {{6, 0}, {4, 2}, {1, 0}};

// A candidate's baseband: the bins within 130 Hz of its centre, tapered
// from 90 Hz out.
constexpr int kTaperBins = int(130.0 / kSlotBinHz);
// The gain of a decoded signal is smoothed over 4 symbols (192 ms), twice.
constexpr int kGainSmooth = 4 * kSps;
// The noise estimate's windows: two symbols, 10.42 Hz bins.
constexpr int kNoiseWindow = 512;

static_assert(kSlotSamples <= kSlotFft, "the slot fits its FFT");
static_assert(kLastSyncFrame + kFramesPerSymbol * (kSymbolCount - 1) < kFrames, "the latest start is searched whole");

struct Candidate {
    float score;
    int bin;    // tone-0 bin in the spectrogram's range
    int frame;  // frame of the first Costas symbol
    int hits;   // Costas tones strongest in their symbol's spectrum
};

struct Found {
    Decode decode;
    Tones tones{};
    double f0 = 0;  // baseband hertz of tone 0
    double t0 = 0;  // start of the first ramp symbol, seconds after buffer sample 0
};

}  // namespace

// The FT8 depths' structure with FT4's counts: 16 Costas tones instead of
// 21, and thresholds set by measurement on white noise and on synthetic
// signals (docs/DESIGN.md). A candidate needs 7 of 16 Costas tones (6 at
// depth 3), against 4 by chance: fewer cost CPU and found nothing more. The
// Costas power ratio says less than FT8's with 16 tones (99 % of noise
// candidates stay below 3.5), so OSD's results are screened above all by
// their hard errors against all four soft-bit sets: of OSD codewords that
// unpacked from white noise none had 145 or fewer, of true ones at -18 to
// -16.5 dB 91 % did.
DecoderTuning tuning_for_depth(int depth) {
    DecoderTuning t;
    t.sync_min = 1.8f;
    t.max_candidates = 150;
    t.spec_min_hits = 5;
    t.min_costas_hits = 7;
    t.osd_min_costas = 11;
    t.osd_max_hard = 31;
    t.osd_weak_min_costas = 8;
    t.osd_weak_max_hard = 29;
    t.osd_low_hard = 28;
    t.osd_max_total = 145;
    t.osd_min_sync = 2.8f;
    if (depth <= 1) {
        t.passes = 2;
        t.sync_min = 2.0f;
        t.max_candidates = 100;
        t.spec_min_hits = 6;
        t.osd_order = 0;
    } else if (depth >= 3) {
        t.sync_min = 1.6f;
        t.max_candidates = 250;
        t.spec_min_hits = 4;
        t.min_costas_hits = 6;
        t.osd_order = 2;
    }
    return t;
}

struct SlotDecoder::Work {
    // Spectrogram power, kFrames rows of nbins, bins from bin_lo (signed).
    std::vector<float> power;
    int bin_lo = 0;
    int nbins = 0;
    std::vector<float> noise;  // noise power per sample, per kNoiseWindow bin from noise_bin_lo
    int noise_bin_lo = 0;
    std::vector<cf> slot;  // FFT of the whole slot
    std::vector<cf> bb;    // one candidate's baseband
    std::vector<cf> fft_buf;
    std::vector<cf> ref;
    std::vector<cf> gain_z, gain_e;
    // The taper of the bins cut out for a candidate, divided by the size of
    // the slot's FFT.
    std::vector<float> taper;
    // Tone correlators for 16-sample symbols: e^{-2 pi i (k - 1.5) n / 16}.
    cf tone_w[kToneCount][kBbSps];
    // e^{2 pi i 20.833 Hz d tau} for tone steps d = -3..3 and the delays tried.
    std::complex<double> tau_rot[kTauSteps][7];
    SlotStats stats;
};

SlotDecoder::SlotDecoder() : work_(std::make_unique<Work>()) {
    for (int k = -kTaperBins; k <= kTaperBins; ++k) {
        const double fr = std::fabs(k * kSlotBinHz);
        const double t = fr <= 90.0 ? 1.0 : 0.5 * (1.0 + std::cos(M_PI * (fr - 90.0) / 40.0));
        work_->taper.push_back(float(t / kSlotFft));
    }
    for (int k = 0; k < kToneCount; ++k)
        for (int n = 0; n < kBbSps; ++n) {
            const double a = -2.0 * M_PI * (k - 1.5) * n / kBbSps;
            work_->tone_w[k][n] = cf(float(std::cos(a)), float(std::sin(a)));
        }
    for (int q = 0; q < kTauSteps; ++q)
        for (int d = 0; d < 7; ++d)
            work_->tau_rot[q][d] =
                std::polar(1.0, 2.0 * M_PI * kToneSpacingHz * (d - 3) * (q - kTauSteps / 2) * kTauStep);
}

SlotDecoder::~SlotDecoder() = default;

namespace {

void compute_spectrogram(const std::vector<cf>& x, double min_hz, double max_hz, std::vector<float>& power,
                         int& bin_lo, int& nbins, std::vector<cf>& buf) {
    const Fft& fft = fft_plan(kSpecFft);
    bin_lo = int(std::floor(min_hz / kBinHz)) - 1;
    const int bin_hi = int(std::ceil((max_hz + 3 * kToneSpacingHz) / kBinHz)) + 2;
    nbins = bin_hi - bin_lo;
    power.assign(size_t(kFrames) * size_t(nbins), 0.0f);
    buf.resize(kSpecFft);
    for (int f = 0; f < kFrames; ++f) {
        const cf* src = &x[size_t(f) * kFrameStep];
        std::copy(src, src + kFrameLen, buf.begin());
        std::fill(buf.begin() + kFrameLen, buf.end(), cf(0, 0));
        fft.forward(buf.data());
        float* row = &power[size_t(f) * size_t(nbins)];
        for (int b = 0; b < nbins; ++b) {
            const int k = (bin_lo + b) & (kSpecFft - 1);
            row[b] = std::norm(buf[size_t(k)]);
        }
    }
}

std::vector<Candidate> find_candidates(const SlotDecoder::Work& w, const DecoderTuning& tu, double min_hz,
                                       double max_hz) {
    const int nb = w.nbins;
    // Per frame and bin, the power summed over the 4 tones from that bin.
    std::vector<float> tone_sum(size_t(kFrames) * size_t(nb), 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        const float* row = &w.power[size_t(f) * size_t(nb)];
        float* out = &tone_sum[size_t(f) * size_t(nb)];
        for (int b = 0; b + 6 < nb; ++b)
            out[b] = row[b] + row[b + 2] + row[b + 4] + row[b + 6];
    }
    const int b_first = std::max(0, int(std::ceil(min_hz / kBinHz)) - w.bin_lo);
    const int b_last = std::min(nb - 7, int(std::floor(max_hz / kBinHz)) - w.bin_lo);
    const int frames = kLastSyncFrame - kFirstSyncFrame + 1;
    std::vector<float> score(size_t(std::max(0, b_last - b_first + 1)) * size_t(frames), 0.0f);
    auto at = [&](int b, int t) -> float& {
        return score[size_t(b - b_first) * size_t(frames) + size_t(t - kFirstSyncFrame)];
    };
    for (int b = b_first; b <= b_last; ++b) {
        for (int t = kFirstSyncFrame; t <= kLastSyncFrame; ++t) {
            float s[4] = {0, 0, 0, 0}, n[4] = {0, 0, 0, 0};
            for (int blk = 0; blk < 4; ++blk) {
                for (int j = 0; j < 4; ++j) {
                    const int f = t + kFramesPerSymbol * (kCostasStart[blk] + j);
                    const float sig = w.power[size_t(f) * size_t(nb) + size_t(b + 2 * kCostas[blk][j])];
                    s[blk] += sig;
                    n[blk] += tone_sum[size_t(f) * size_t(nb) + size_t(b)] - sig;
                }
            }
            // All four Costas arrays, or three when samples are missing at
            // either end of the slot.
            auto ratio = [](float sig, float other) { return other > 0 ? 3.0f * sig / other : 0.0f; };
            float r = ratio(s[0] + s[1] + s[2] + s[3], n[0] + n[1] + n[2] + n[3]);
            r = std::max(r, ratio(s[0] + s[1] + s[2], n[0] + n[1] + n[2]));
            r = std::max(r, ratio(s[1] + s[2] + s[3], n[1] + n[2] + n[3]));
            at(b, t) = r;
        }
    }
    std::vector<Candidate> all;
    for (int b = b_first; b <= b_last; ++b) {
        for (int t = kFirstSyncFrame; t <= kLastSyncFrame; ++t) {
            const float v = at(b, t);
            if (v < tu.sync_min)
                continue;
            bool peak = true;
            for (int db = -1; db <= 1 && peak; ++db)
                for (int dt = -1; dt <= 1; ++dt) {
                    if (!db && !dt)
                        continue;
                    const int bb = b + db, tt = t + dt;
                    if (bb < b_first || bb > b_last || tt < kFirstSyncFrame || tt > kLastSyncFrame)
                        continue;
                    if (at(bb, tt) > v) {
                        peak = false;
                        break;
                    }
                }
            if (peak)
                all.push_back(Candidate{v, b, t, 0});
        }
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<Candidate> kept;
    for (const Candidate& c : all) {
        bool near = false;
        for (const Candidate& k : kept)
            if (std::abs(k.bin - c.bin) <= 1 && std::abs(k.frame - c.frame) <= 3) {
                near = true;
                break;
            }
        if (near)
            continue;
        Candidate k = c;
        for (int blk = 0; blk < 4; ++blk)
            for (int j = 0; j < 4; ++j) {
                const int f = c.frame + kFramesPerSymbol * (kCostasStart[blk] + j);
                const float* row = &w.power[size_t(f) * size_t(nb) + size_t(c.bin)];
                int arg = 0;
                for (int t = 1; t < kToneCount; ++t)
                    if (row[2 * t] > row[2 * arg])
                        arg = t;
                k.hits += arg == kCostas[blk][j];
            }
        if (k.hits < tu.spec_min_hits)
            continue;
        kept.push_back(k);
        if (int(kept.size()) >= tu.max_candidates)
            break;
    }
    return kept;
}

// Costas power at baseband start n0 (the first Costas symbol) with tone 0
// at 20.833 * (-1.5) + df hertz: each symbol correlated on its own.
float costas_power(const std::vector<cf>& bb, int n0, double df, const cf (*tone_w)[kBbSps]) {
    cf rot[kBbSps];
    for (int n = 0; n < kBbSps; ++n) {
        const double a = -2.0 * M_PI * df * n / kBbRate;
        rot[n] = cf(float(std::cos(a)), float(std::sin(a)));
    }
    float total = 0;
    for (int blk = 0; blk < 4; ++blk) {
        for (int j = 0; j < 4; ++j) {
            const int start = n0 + kBbSps * (kCostasStart[blk] + j);
            if (start < 0 || start + kBbSps > kBbLen)
                continue;
            const cf* w = tone_w[kCostas[blk][j]];
            cf acc(0, 0);
            for (int n = 0; n < kBbSps; ++n)
                acc += cmul(bb[size_t(start + n)], cmul(w[n], rot[n]));
            total += std::norm(acc);
        }
    }
    return total;
}

// The same, coherent over the 4 symbols of each Costas array: each
// symbol's correlation referred to one time origin (its phase alternates
// by pi from one symbol to the next, as in demodulate()), the four summed
// before their power is taken. 192 ms of signal instead of 48 ms make this
// four times as sharp in frequency.
float costas_power_coherent(const std::vector<cf>& bb, int n0, double df, const cf (*tone_w)[kBbSps]) {
    cf rot[kBbSps];
    for (int n = 0; n < kBbSps; ++n) {
        const double a = -2.0 * M_PI * df * n / kBbRate;
        rot[n] = cf(float(std::cos(a)), float(std::sin(a)));
    }
    float total = 0;
    for (int blk = 0; blk < 4; ++blk) {
        cf sum(0, 0);
        for (int j = 0; j < 4; ++j) {
            const int s = kCostasStart[blk] + j;
            const int start = n0 + kBbSps * s;
            if (start < 0 || start + kBbSps > kBbLen)
                continue;
            const cf* w = tone_w[kCostas[blk][j]];
            cf acc(0, 0);
            for (int n = 0; n < kBbSps; ++n)
                acc += cmul(bb[size_t(start + n)], cmul(w[n], rot[n]));
            const double a = -2.0 * M_PI * df * kBbSps * j / kBbRate + M_PI * j;
            sum += cmul(acc, cf(float(std::cos(a)), float(std::sin(a))));
        }
        total += std::norm(sum);
    }
    return total;
}

// Frequency and start refined coherently over the whole transmission. The
// phases of neighbouring symbols measure them over 48 ms, so noisily (a
// median error of 0.2 Hz and 0.7 ms at -17 dB); with the tones decided,
// each symbol's correlation c[s][tone] turns by 2 pi delta T from one symbol
// to the next for a frequency error delta, and by 2 pi 20.833 Hz (k - 1.5)
// tau for a start error tau, so the delta and tau that line up all 103 best
// are found to 0.01 Hz and 0.06 ms. On a fading channel the sum is weaker
// but its peak stays put; the search is kept within +-0.5 Hz and +-1 ms of
// the phase estimate. Returns delta and tau.
std::pair<double, double> coherent_fine(const cf (*c)[kToneCount]) {
    int tone[kSymbolCount];
    for (int s = 0; s < kSymbolCount; ++s) {
        int arg = 0;
        for (int k = 1; k < kToneCount; ++k)
            if (std::norm(c[s][k]) > std::norm(c[s][arg]))
                arg = k;
        tone[s] = arg;
    }
    for (int blk = 0; blk < 4; ++blk)
        for (int j = 0; j < 4; ++j)
            tone[kCostasStart[blk] + j] = kCostas[blk][j];
    // The sum over symbols, per tone, depends on delta alone; tau only
    // turns the four partial sums.
    auto by_tone = [&](double delta, std::complex<double>* out) {
        for (int k = 0; k < kToneCount; ++k)
            out[k] = 0;
        const std::complex<double> stp = std::polar(1.0, -2.0 * M_PI * delta * kSymbolSeconds);
        std::complex<double> rot = 1.0;
        for (int s = 0; s < kSymbolCount; ++s) {
            out[tone[s]] += std::complex<double>(c[s][tone[s]]) * rot;
            rot *= stp;
        }
    };
    auto power = [&](const std::complex<double>* parts, double tau) {
        std::complex<double> sum = 0;
        for (int k = 0; k < kToneCount; ++k)
            sum += parts[k] * std::polar(1.0, 2.0 * M_PI * kToneSpacingHz * (k - 1.5) * tau);
        return std::norm(sum);
    };
    double best = -1, delta = 0, tau = 0;
    auto search = [&](double d_centre, double d_step, int d_steps, double t_centre, double t_step, int t_steps) {
        std::complex<double> parts[kToneCount];
        for (int p = -d_steps; p <= d_steps; ++p) {
            const double d = d_centre + p * d_step;
            by_tone(d, parts);
            for (int q = -t_steps; q <= t_steps; ++q) {
                const double t = t_centre + q * t_step;
                const double m = power(parts, t);
                if (m > best) {
                    best = m;
                    delta = d;
                    tau = t;
                }
            }
        }
    };
    // +-0.5 Hz by 0.05 Hz and +-1 ms by 0.25 ms, then around the best by
    // 0.01 Hz and 0.0625 ms.
    search(0.0, 0.05, 10, 0.0, 0.00025, 4);
    search(delta, 0.01, 5, tau, 0.0000625, 2);
    return {delta, tau};
}

// Soft bits from blocks of `span` data symbols within each group of 29,
// the first block of a group `first_len` long (0: span) and the last
// shorter when the rest does not divide: for each of the 4^len tone
// sequences of a block the magnitude of the coherent sum; a bit's soft
// value is the largest magnitude among sequences with that bit 0 less the
// largest with it 1 ([QEX] section 6). A block is split into a head and a
// tail of at most 3 symbols, whose 4^3 partial sums make every sequence's
// sum in one addition; the largest power for each head is what a head bit
// needs, the largest for each tail what a tail bit needs, and the square
// root is taken of those maxima only. Single symbols keep the powers
// themselves: on a channel that fades within a symbol the received phase
// means nothing and the power is the better measure (square-law detection),
// which decoded 76 of 100 slots at -9 dB on the 10 Hz channel of [QEX]
// Table 6 against 69 with magnitudes, and the same elsewhere.
void block_llrs(const cf (*c)[kToneCount], int span, int first_len, Llrs& llr) {
    cf head[64], tail[64];
    float head_max[64], tail_max[64];
    for (int group = 0; group < 3; ++group) {
        for (int first = 0; first < 29;) {
            const int len = std::min(first == 0 && first_len ? first_len : span, 29 - first);
            const int d0 = group * 29 + first;
            const int h_len = len / 2, t_len = len - h_len;
            const int nh = 1 << (2 * h_len), nt = 1 << (2 * t_len);
            // Sequences numbered by the bits they carry, first symbol most
            // significant.
            auto partial = [&](int d, int count, cf* out) {
                const int n = 1 << (2 * count);
                for (int j = 0; j < n; ++j) {
                    cf sum(0, 0);
                    for (int i = 0; i < count; ++i)
                        sum += c[data_symbol_index(d + i)][kBitsToTone[(j >> (2 * (count - 1 - i))) & 3]];
                    out[j] = sum;
                }
            };
            partial(d0, h_len, head);
            partial(d0 + h_len, t_len, tail);
            std::fill(head_max, head_max + nh, 0.0f);
            std::fill(tail_max, tail_max + nt, 0.0f);
            for (int a = 0; a < nh; ++a)
                for (int b = 0; b < nt; ++b) {
                    const float p = std::norm(head[a] + tail[b]);
                    head_max[a] = std::max(head_max[a], p);
                    tail_max[b] = std::max(tail_max[b], p);
                }
            for (int i = 0; i < 2 * len; ++i) {
                const bool in_head = i < 2 * h_len;
                const float* m = in_head ? head_max : tail_max;
                const int n = in_head ? nh : nt;
                const int bits = in_head ? 2 * h_len : 2 * t_len;
                const int bit = in_head ? i : i - 2 * h_len;
                const int run = 1 << (bits - 1 - bit);  // the bit is 0 in the first run of each 2*run
                float best0 = 0, best1 = 0;
                for (int base = 0; base < n; base += 2 * run)
                    for (int j = 0; j < run; ++j) {
                        best0 = std::max(best0, m[base + j]);
                        best1 = std::max(best1, m[base + run + j]);
                    }
                llr[size_t(2 * d0 + i)] = len == 1 ? best0 - best1 : std::sqrt(best0) - std::sqrt(best1);
            }
            first += len;
        }
    }
}

// Single-symbol soft bits divided by the symbol's strongest tone, as in
// decoder.cpp: a symbol another signal hits weighs no more than a clean one.
void relative_llrs(const cf (*c)[kToneCount], Llrs& llr) {
    for (int d = 0; d < kDataSymbolCount; ++d) {
        const cf* row = c[data_symbol_index(d)];
        float mag[kToneCount];
        float top = 1e-30f;
        for (int k = 0; k < kToneCount; ++k) {
            mag[k] = std::sqrt(std::norm(row[k]));
            top = std::max(top, mag[k]);
        }
        for (int i = 0; i < 2; ++i) {
            float b0 = 0, b1 = 0;
            for (int k = 0; k < kToneCount; ++k) {
                if ((kToneToBits[k] >> (1 - i)) & 1)
                    b1 = std::max(b1, mag[k]);
                else
                    b0 = std::max(b0, mag[k]);
            }
            llr[size_t(2 * d + i)] = (b0 - b1) / top;
        }
    }
}

bool all_zero(const Codeword& cw) {
    for (uint8_t b : cw)
        if (b)
            return false;
    return true;
}

}  // namespace

std::vector<Decode> SlotDecoder::decode(std::vector<cf>& x, size_t valid_begin, size_t valid_end, double offset_hz,
                                        int64_t slot_start_ms, const DecodeSettings& settings,
                                        CallsignHashTable* hashes, SlotStats* stats_out) {
    Work& w = *work_;
    w.stats = SlotStats{};
    x.resize(kSlotSamples);
    valid_end = std::min(valid_end, size_t(kSlotSamples));
    if (valid_end < valid_begin + size_t(kFrameLen))
        return {};
    const DecoderTuning tu = settings.custom_tuning ? settings.tuning : tuning_for_depth(settings.depth);
    const int64_t now_s = slot_start_ms / 1000;
    std::vector<Found> found;
    const Fft& slot_fft = fft_plan(kSlotFft);
    const Fft& bb_fft = fft_plan(kBbLen);
    w.slot.resize(kSlotFft);
    w.bb.resize(kBbLen);

    for (int pass = 1; pass <= tu.passes; ++pass) {
        w.stats.passes = pass;
        compute_spectrogram(x, settings.min_hz, settings.max_hz, w.power, w.bin_lo, w.nbins, w.fft_buf);
        if (pass == 1)
            estimate_noise(x, int(valid_begin), int(valid_end), kRate, kNoiseWindow, 14, settings.min_hz,
                           settings.max_hz, 3 * kToneSpacingHz, w.noise, w.noise_bin_lo, w.fft_buf);
        const std::vector<Candidate> cands = find_candidates(w, tu, settings.min_hz, settings.max_hz);
        w.stats.candidates += int(cands.size());
        std::copy(x.begin(), x.end(), w.slot.begin());
        std::fill(w.slot.begin() + kSlotSamples, w.slot.end(), cf(0, 0));
        slot_fft.forward(w.slot.data());
        int new_decodes = 0;

        for (const Candidate& cand : cands) {
            const double f_coarse = (w.bin_lo + cand.bin) * kBinHz;
            const double t_coarse = cand.frame * double(kFrameStep) / kRate;
            bool seen = false;
            for (const Found& f : found)
                if (std::fabs(f.f0 - f_coarse) < 6.0 && std::fabs(f.t0 + kSymbolSeconds - t_coarse) < 0.025)
                    seen = true;
            if (seen)
                continue;

            // The signal's centre +-130 Hz: FT4's tones lie within 31 Hz of
            // it, and BT = 1 keeps their spectrum narrow.
            const long i0 = std::lround((f_coarse + kCentreHz) / kSlotBinHz);
            const double f_ext = double(i0) * kSlotBinHz;
            // Baseband sample m is the signal at m / kBbRate + advance
            // seconds: a phase ramp across the bins moves it by a fraction
            // of a sample.
            auto extract = [&](double advance) {
                std::fill(w.bb.begin(), w.bb.end(), cf(0, 0));
                const std::complex<double> step = std::polar(1.0, 2.0 * M_PI * kSlotBinHz * advance);
                std::complex<double> ramp = std::polar(1.0, -2.0 * M_PI * kTaperBins * kSlotBinHz * advance);
                for (int k = -kTaperBins; k <= kTaperBins; ++k) {
                    const long src = ((i0 + k) % kSlotFft + kSlotFft) % kSlotFft;
                    w.bb[size_t((k + kBbLen) % kBbLen)] =
                        cmul(w.slot[size_t(src)], cf(float(ramp.real()), float(ramp.imag()))) *
                        w.taper[size_t(k + kTaperBins)];
                    ramp *= step;
                }
                bb_fft.inverse(w.bb.data());
            };
            extract(0.0);

            // Fine sync on the Costas symbols: time, frequency, time again.
            // The spectrogram places the start to 12 ms (4 baseband
            // samples) and tone 0 to 5 Hz.
            const double d0 = f_coarse + kCentreHz - f_ext;
            int n0 = int(std::lround(t_coarse * kBbRate));
            double df = d0;
            float best = -1;
            int best_n = n0;
            for (int dn = -6; dn <= 6; ++dn) {
                const float p = costas_power(w.bb, n0 + dn, df, w.tone_w);
                if (p > best) {
                    best = p;
                    best_n = n0 + dn;
                }
            }
            n0 = best_n;
            double best_f = df;
            best = -1;
            for (int k = -12; k <= 12; ++k) {
                const double f = d0 + 0.5 * k;
                const float p = costas_power_coherent(w.bb, n0, f, w.tone_w);
                if (p > best) {
                    best = p;
                    best_f = f;
                }
            }
            df = best_f;
            best = -1;
            for (int dn = -3; dn <= 3; ++dn) {
                const float p = costas_power_coherent(w.bb, n0 + dn, df, w.tone_w);
                if (p > best) {
                    best = p;
                    best_n = n0 + dn;
                }
            }
            n0 = best_n;

            // Tone amplitudes of the 103 symbols, referred to one time
            // origin: with tones at half-integer multiples of the symbol
            // rate from the centre, a per-symbol correlator's phase
            // alternates by pi per symbol.
            cf c[kSymbolCount][kToneCount];
            auto demodulate = [&](double f) {
                cf rot[kBbSps];
                for (int n = 0; n < kBbSps; ++n) {
                    const double a = -2.0 * M_PI * f * n / kBbRate;
                    rot[n] = cf(float(std::cos(a)), float(std::sin(a)));
                }
                cf w_rot[kToneCount][kBbSps];
                for (int k = 0; k < kToneCount; ++k)
                    for (int n = 0; n < kBbSps; ++n)
                        w_rot[k][n] = cmul(w.tone_w[k][n], rot[n]);
                for (int s = 0; s < kSymbolCount; ++s) {
                    const int start = n0 + kBbSps * s;
                    const double a = -2.0 * M_PI * f * kBbSps * s / kBbRate + M_PI * s;
                    const cf sym_rot(float(std::cos(a)), float(std::sin(a)));
                    for (int k = 0; k < kToneCount; ++k) {
                        cf acc(0, 0);
                        if (start >= 0 && start + kBbSps <= kBbLen)
                            for (int n = 0; n < kBbSps; ++n)
                                acc += cmul(w.bb[size_t(start + n)], w_rot[k][n]);
                        c[s][k] = cmul(acc, sym_rot);
                    }
                }
            };
            // Frequency and time from the phases of neighbouring symbols, as
            // decoder.cpp explains: from one symbol to the next the phase
            // turns by 2 pi df T, less 2 pi 20.833 Hz (k' - k) tau when the
            // tone changes from k to k' and the signal is tau late.
            double tau_rest = 0;
            auto correct_phases = [&]() {
                for (int k = 0; k < kToneCount; ++k) {
                    const double a = 2.0 * M_PI * kToneSpacingHz * (k - 1.5) * tau_rest;
                    const cf r(float(std::cos(a)), float(std::sin(a)));
                    for (int s = 0; s < kSymbolCount; ++s)
                        c[s][k] = cmul(c[s][k], r);
                }
            };
            demodulate(df);
            for (int round = 0; round < 3; ++round) {
                int tone[kSymbolCount];
                for (int s = 0; s < kSymbolCount; ++s) {
                    int arg = 0;
                    for (int k = 1; k < kToneCount; ++k)
                        if (std::norm(c[s][k]) > std::norm(c[s][arg]))
                            arg = k;
                    tone[s] = arg;
                }
                for (int blk = 0; blk < 4; ++blk)
                    for (int j = 0; j < 4; ++j)
                        tone[kCostasStart[blk] + j] = kCostas[blk][j];
                std::complex<double> by_dk[7] = {};
                for (int s = 0; s + 1 < kSymbolCount; ++s)
                    by_dk[tone[s + 1] - tone[s] + 3] +=
                        std::complex<double>(cmul_conj(c[s + 1][tone[s + 1]], c[s][tone[s]]));
                double best_tau = 0, best_m = -1;
                std::complex<double> best_sum = 0;
                for (int q = 0; q < kTauSteps; ++q) {
                    std::complex<double> sum = 0;
                    for (int d = 0; d < 7; ++d)
                        sum += by_dk[d] * w.tau_rot[q][d];
                    const double m = std::norm(sum);
                    if (m > best_m) {
                        best_m = m;
                        best_tau = (q - kTauSteps / 2) * kTauStep;
                        best_sum = sum;
                    }
                }
                const double step = std::arg(best_sum) / (2.0 * M_PI * kSymbolSeconds);
                if (std::fabs(step) > 8.0)
                    break;
                df += step;
                const double tau = tau_rest + best_tau;
                const int whole = int(std::lround(tau * kBbRate));
                n0 += whole;
                tau_rest = tau - whole / kBbRate;
                demodulate(df);
                correct_phases();
                if (std::fabs(step) < 0.07 && std::fabs(best_tau) < 0.00015)
                    break;
            }
            int hits = 0;
            for (int blk = 0; blk < 4; ++blk)
                for (int j = 0; j < 4; ++j) {
                    const cf* row = c[kCostasStart[blk] + j];
                    int arg = 0;
                    for (int k = 1; k < kToneCount; ++k)
                        if (std::norm(row[k]) > std::norm(row[arg]))
                            arg = k;
                    hits += arg == kCostas[blk][j];
                }
            if (hits < tu.min_costas_hits)
                continue;
            const auto [delta, tau] = coherent_fine(c);
            df += delta;
            tau_rest += tau;
            // The symbol windows start on whole baseband samples, 3 ms
            // apart; the phase correction above leaves them up to 1.5 ms
            // (3 % of a symbol) off the symbols, which costs about 0.1 dB.
            // Cut the baseband again, moved by what is left.
            extract(tau_rest);
            demodulate(df);

            // Four soft-bit sets, one per vector lane: blocks of 6 symbols,
            // of 4 starting 2 symbols into each group, single symbols, and
            // the relative single-symbol set. [QEX] section 6 has blocks of
            // 1, 2 and 4 for FT4; with the start and frequency found as
            // precisely as above, longer blocks gain more where the channel
            // holds its phase (0.5 dB on white noise, 0.6 and 0.3 dB with
            // 0.5 and 1 Hz of Doppler spread), and blocks that start where
            // the others do not add more than blocks of 2 did. Single
            // symbols keep fast fading decodable (docs/DESIGN.md).
            Llrs sets[4];
            for (int i = 0; i < 3; ++i) {
                block_llrs(c, kBlockSets[i].span, kBlockSets[i].first_len, sets[i]);
                normalise(sets[i]);
            }
            relative_llrs(c, sets[3]);
            normalise(sets[3]);
            std::optional<Codeword> cw;
            DecodeMethod method = DecodeMethod::Bp;
            int iterations = 0;
            int hard_errors = 0;
            int span_used = 0;
            w.stats.ldpc_runs += 4;
            std::array<BpResult, 4> bp;
            if (simd_level() >= SimdLevel::Baseline) {
                bp = bp_decode4({&sets[0], &sets[1], &sets[2], &sets[3]}, 30);
            } else {
                for (int s = 0; s < 4; ++s)
                    bp[size_t(s)] = bp_decode(sets[s], 30);
            }
            // The all-zero codeword passes every check and the CRC, and is
            // what a steady carrier on tone 0 demodulates to; a transmitter
            // sends it only for the one payload equal to the scrambling
            // vector, so it is never taken.
            for (int s = 0; s < 4 && !cw; ++s) {
                const BpResult& r = bp[size_t(s)];
                if (r.converged && crc_ok(r.codeword) && !all_zero(r.codeword)) {
                    const int he = hard_disagreements(r.codeword, sets[s]);
                    if (he <= tu.bp_max_hard) {
                        cw = r.codeword;
                        span_used = s < 3 ? kBlockSets[s].span : 1;
                        iterations = r.iterations;
                        hard_errors = he;
                    }
                }
            }
            const bool osd_sync =
                hits >= std::min(tu.osd_min_costas, tu.osd_weak_min_costas) && cand.score >= tu.osd_min_sync;
            const int osd_hard = hits >= tu.osd_min_costas ? tu.osd_max_hard : tu.osd_weak_max_hard;
            if (!cw && tu.osd_order > 0 && osd_sync) {
                OsdOptions o;
                o.order = tu.osd_order;
                o.pair_span = tu.osd_pair_span;
                // OSD on the three block sets: the single-symbol one finds
                // what fast fading leaves (at -10 dB on the 10 Hz channel 71
                // of 100 slots against 54 without it), the others what
                // steadier channels do.
                for (int s : {0, 1, 2}) {
                    ++w.stats.osd_runs;
                    const OsdResult r = osd_decode(sets[s], o);
                    int total = 0;
                    if (r.found)
                        for (const Llrs& set : sets)
                            total += hard_disagreements(r.codeword, set);
                    if (r.found && r.disagreements <= osd_hard && total <= tu.osd_max_total && !all_zero(r.codeword)) {
                        cw = r.codeword;
                        span_used = kBlockSets[s].span;
                        method = DecodeMethod::Osd;
                        hard_errors = r.disagreements;
                        break;
                    }
                }
            }
            if (!cw)
                continue;
            const Payload payload = payload_of(*cw);
            bool dup = false;
            for (const Found& f : found)
                if (f.decode.payload == payload)
                    dup = true;
            if (dup)
                continue;
            auto msg = unpack_message(payload, hashes, now_s, settings.unpack);
            if (!msg)
                continue;

            Found fd;
            fd.tones = tones_of(*cw);
            fd.f0 = f_ext - kCentreHz + df;
            fd.t0 = n0 / kBbRate + tau_rest - kSymbolSeconds;
            Decode& d = fd.decode;
            d.mode = Mode::Ft4;
            d.slot_start_ms = slot_start_ms;
            d.message = std::move(*msg);
            d.payload = payload;
            d.method = method;
            d.osd_order = method == DecodeMethod::Osd ? tu.osd_order : 0;
            d.ldpc_iterations = iterations;
            d.hard_errors = hard_errors;
            d.costas_hits = hits;
            d.sync = cand.score;
            d.llr_span = span_used;
            d.low_confidence =
                method == DecodeMethod::Osd && (hard_errors > tu.osd_low_hard || hits < tu.osd_min_costas);
            d.pass = pass;

            // Rebuild, refine the start to a sample, estimate the gain along
            // the transmission and subtract, as decoder.cpp does.
            const int margin = 64;
            const long n_start = long(std::floor(fd.t0 * kRate)) - margin;
            const int len = kWaveSymbolCount * kSps + 2 * margin;
            auto build_ref = [&](double t0) {
                w.ref.assign(size_t(len), cf(0, 0));
                add_ft4_waveform(fd.tones, kRate, fd.f0, t0 - double(n_start) / kRate, 1.0f, w.ref.data(),
                                 w.ref.size());
            };
            auto sample = [&](long i) { return (i >= 0 && i < kSlotSamples) ? x[size_t(i)] : cf(0, 0); };
            build_ref(fd.t0);
            // Coherent over blocks of 24 symbols (1.15 s, about as long as
            // FT8's 8), every second sample.
            auto corr = [&](int shift) {
                double total = 0;
                for (int blk = 0; blk < kWaveSymbolCount; blk += 24) {
                    cf acc(0, 0);
                    const int end = std::min(kWaveSymbolCount, blk + 24) * kSps;
                    for (int n = blk * kSps; n < end; n += 2)
                        acc += cmul_conj(sample(n_start + margin + n + shift), w.ref[size_t(margin + n)]);
                    total += std::norm(acc);
                }
                return total;
            };
            int shift = 0;
            double cbest = -1;
            for (int sft = -8; sft <= 8; sft += 4) {
                const double v = corr(sft);
                if (v > cbest) {
                    cbest = v;
                    shift = sft;
                }
            }
            for (int step : {2, 1}) {
                const int centre = shift;
                for (int dir : {-1, 1}) {
                    const double v = corr(centre + dir * step);
                    if (v > cbest) {
                        cbest = v;
                        shift = centre + dir * step;
                    }
                }
            }
            double frac = 0;
            {
                const double a = corr(shift - 1), b = cbest, cc = corr(shift + 1);
                const double den = a - 2 * b + cc;
                if (den < 0)
                    frac = std::clamp(0.5 * (a - cc) / den, -0.5, 0.5);
            }
            fd.t0 += (shift + frac) / kRate;
            build_ref(fd.t0);
            // What is left of the frequency error, from the phase step
            // between neighbouring symbols' gains; twice.
            for (int iter = 0; iter < 2; ++iter) {
                cf prev(0, 0);
                std::complex<double> steps = 0;
                for (int s = 0; s < kWaveSymbolCount; ++s) {
                    cf acc(0, 0);
                    const int base = margin + s * kSps;
                    for (int n = 0; n < kSps; n += 2)
                        acc += cmul_conj(sample(n_start + base + n), w.ref[size_t(base + n)]);
                    if (s > 0)
                        steps += std::complex<double>(cmul_conj(acc, prev));
                    prev = acc;
                }
                const double dfix = std::arg(steps) / (2.0 * M_PI * kSymbolSeconds);
                if (std::fabs(dfix) > 1.5)
                    break;
                fd.f0 += dfix;
                const double t_rel = fd.t0 - double(n_start) / kRate;
                std::complex<double> ph = std::polar(1.0, -2.0 * M_PI * dfix * t_rel);
                const std::complex<double> stp = std::polar(1.0, 2.0 * M_PI * dfix / kRate);
                for (int n = 0; n < len; ++n) {
                    w.ref[size_t(n)] = cmul(w.ref[size_t(n)], cf(float(ph.real()), float(ph.imag())));
                    ph *= stp;
                }
            }
            w.gain_z.assign(size_t(len), cf(0, 0));
            w.gain_e.assign(size_t(len), cf(0, 0));
            for (int n = 0; n < len; ++n) {
                w.gain_z[size_t(n)] = cmul_conj(sample(n_start + n), w.ref[size_t(n)]);
                w.gain_e[size_t(n)] = cf(std::norm(w.ref[size_t(n)]), 0);
            }
            smooth(w.gain_z, kGainSmooth);
            smooth(w.gain_e, kGainSmooth);
            double sig = 0;
            int sig_n = 0;
            for (int n = 0; n < len; ++n) {
                const long i = n_start + n;
                const float e = w.gain_e[size_t(n)].real();
                if (e < 1e-3f)
                    continue;
                const cf g = w.gain_z[size_t(n)] / e;
                if (i >= 0 && i < kSlotSamples)
                    x[size_t(i)] -= cmul(g, w.ref[size_t(n)]);
                if (e > 0.99f) {
                    sig += std::norm(g);
                    ++sig_n;
                }
            }
            // SNR: the gain's power, less the noise its smoothing lets
            // through (a triangle of twice kGainSmooth averages about 4/3 of
            // that many samples), over the noise in 2500 Hz.
            double sigma2 = 0;
            {
                const int nb = int(w.noise.size());
                const int b = int(std::lround((fd.f0 + kCentreHz) / (kRate / kNoiseWindow))) - w.noise_bin_lo;
                if (b >= 0 && b < nb)
                    sigma2 = w.noise[size_t(b)];
            }
            double ps = sig_n ? sig / sig_n - sigma2 / (kGainSmooth * 4.0 / 3.0) : 0.0;
            double snr = -30;
            if (ps > 0 && sigma2 > 0)
                snr = 10.0 * std::log10(ps * kRate / (sigma2 * 2500.0));
            d.snr_db = int(std::lround(std::clamp(snr, -30.0, 49.0)));
            d.freq_hz = offset_hz + fd.f0;
            d.dt = fd.t0 - kNominalStart;
            if (hashes && !d.low_confidence)
                for (const std::string& call : d.message.learned_calls)
                    hashes->remember(call, now_s);
            found.push_back(std::move(fd));
            ++new_decodes;
        }
        if (new_decodes == 0)
            break;
    }
    std::vector<Decode> out;
    out.reserve(found.size());
    for (Found& f : found)
        out.push_back(std::move(f.decode));
    if (stats_out)
        *stats_out = w.stats;
    return out;
}

}  // namespace fern::ft8::ft4
