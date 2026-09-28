// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pipeline, per pass:
//  1. Spectrogram: 1024-sample (one symbol) windows zero-padded to 2048, so
//     bins are half a tone (3.125 Hz), every quarter symbol (40 ms).
//  2. Candidates: for each tone-0 bin and start frame, the power in the 21
//     Costas tones against the other tones of the same symbols; local maxima
//     above a threshold, strongest first.
//  3. Per candidate: cut 200 Hz around it out of one FFT of the whole slot
//     and transform back: complex baseband at 200 samples a second, 32 per
//     symbol. Refine time and frequency on the Costas symbols, then take
//     the 8 tone amplitudes of all 79 symbols.
//  4. Soft bits from blocks of 1, 2 and 3 symbols (noncoherent block
//     detection, Simon and Divsalar, IEEE Trans. Commun. 41(1), 1993; [QEX]
//     section 6), each tried with belief propagation, then OSD.
//  5. A decode that passes the CRC and unpacks is rebuilt, its time-varying
//     complex gain estimated, and subtracted from the slot ([QEX] section 6),
//     so that the next pass can find what it covered.
#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "fft.h"
#include "gfsk.h"
#include "ldpc.h"

namespace fern::ft8 {

namespace {

constexpr int kFrameStep = 256;
constexpr int kFrameLen = 1024;
constexpr int kSpecFft = 2048;
constexpr double kBinHz = kInternalRate / kSpecFft;  // 3.125
constexpr int kFrames = (kSlotSamples - kFrameLen) / kFrameStep + 1;
constexpr int kSlotFft = 131072;
constexpr double kSlotBinHz = kInternalRate / kSlotFft;
constexpr double kBbRate = 200.0;
constexpr int kBbLen = 4096;  // 20.48 s at 200 Hz
constexpr int kBbSps = 32;
// Frames per symbol, and the frame where a transmission starting on time
// (0.5 s into the slot, 2.0 s into the buffer) begins.
constexpr int kFramesPerSymbol = 4;
constexpr double kNominalStart = kSlotLeadSeconds + kStartSeconds;
// Latest start searched: DT +2.5 s, as WSJT-X.
constexpr int kLastStartFrame = int((kNominalStart + 2.5) * kInternalRate / kFrameStep);
constexpr double kCentreHz = 3.5 * kToneSpacingHz;
// Delays tried when refining the start from phases: +-12 ms in 0.25 ms.
constexpr int kTauSteps = 97;
constexpr double kTauStep = 0.00025;

struct Candidate {
    float score;
    int bin;    // tone-0 bin in the spectrogram's range
    int frame;  // start frame
};

struct Found {
    Decode decode;
    Tones tones{};
    double f0 = 0;  // baseband hertz of tone 0
    double t0 = 0;  // start, seconds after buffer sample 0
};

inline cf cmul_conj(cf a, cf b) {  // a * conj(b)
    return cf(a.real() * b.real() + a.imag() * b.imag(), a.imag() * b.real() - a.real() * b.imag());
}

inline cf cmul(cf a, cf b) {
    return cf(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real());
}

}  // namespace

DecoderTuning tuning_for_depth(int depth) {
    DecoderTuning t;
    if (depth <= 1) {
        t.passes = 2;
        t.sync_min = 2.0f;
        t.max_candidates = 200;
        t.min_costas_hits = 7;
        t.osd_order = 0;
    } else if (depth >= 3) {
        t.sync_min = 1.6f;
        t.max_candidates = 500;
        t.min_costas_hits = 5;
        t.osd_order = 2;
    }
    return t;
}

bool apply_tuning(DecoderTuning& t, const std::string& spec) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos)
            end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string key = item.substr(0, eq);
        const double v = std::atof(item.c_str() + eq + 1);
        if (key == "passes")
            t.passes = int(v);
        else if (key == "sync_min")
            t.sync_min = float(v);
        else if (key == "max_candidates")
            t.max_candidates = int(v);
        else if (key == "min_costas_hits")
            t.min_costas_hits = int(v);
        else if (key == "bp_max_hard")
            t.bp_max_hard = int(v);
        else if (key == "osd_order")
            t.osd_order = int(v);
        else if (key == "osd_pair_span")
            t.osd_pair_span = int(v);
        else if (key == "osd_min_costas")
            t.osd_min_costas = int(v);
        else if (key == "osd_max_hard")
            t.osd_max_hard = int(v);
        else if (key == "osd_weak_min_costas")
            t.osd_weak_min_costas = int(v);
        else if (key == "osd_weak_max_hard")
            t.osd_weak_max_hard = int(v);
        else if (key == "osd_min_sync")
            t.osd_min_sync = float(v);
        else if (key == "osd_low_hard")
            t.osd_low_hard = int(v);
        else if (key == "llr_scale")
            t.llr_scale = float(v);
        else
            return false;
    }
    return true;
}

const char* Decode::quality() const {
    if (low_confidence)
        return "low";
    return method == DecodeMethod::Osd ? "osd" : "bp";
}

struct SlotDecoder::Work {
    // Spectrogram power, kFrames rows of nbins, bins from bin_lo (signed).
    std::vector<float> power;
    int bin_lo = 0;
    int nbins = 0;
    int frame_first = 0;  // frames wholly inside the valid samples
    int frame_last = 0;
    std::vector<float> noise;  // noise power per sample, per 6.25 Hz bin from noise_bin_lo
    int noise_bin_lo = 0;
    std::vector<cf> slot;      // FFT of the whole slot
    std::vector<cf> bb;        // one candidate's 200 Hz baseband
    std::vector<cf> fft_buf;
    std::vector<cf> ref;
    std::vector<cf> gain_z, gain_e;
    // Tone correlators for 32-sample symbols: e^{-2 pi i (k - 3.5) n / 32}.
    cf tone_w[kToneCount][kBbSps];
    // e^{2 pi i 6.25 Hz d tau} for tone steps d = -7..7 and the delays tried.
    std::complex<double> tau_rot[kTauSteps][15];
    SlotStats stats;
};

SlotDecoder::SlotDecoder() : work_(std::make_unique<Work>()) {
    for (int k = 0; k < kToneCount; ++k)
        for (int n = 0; n < kBbSps; ++n) {
            const double a = -2.0 * M_PI * (k - 3.5) * n / kBbSps;
            work_->tone_w[k][n] = cf(float(std::cos(a)), float(std::sin(a)));
        }
    for (int q = 0; q < kTauSteps; ++q)
        for (int d = 0; d < 15; ++d)
            work_->tau_rot[q][d] =
                std::polar(1.0, 2.0 * M_PI * kToneSpacingHz * (d - 7) * (q - kTauSteps / 2) * kTauStep);
}

SlotDecoder::~SlotDecoder() = default;

namespace {

void compute_spectrogram(const std::vector<cf>& x, double min_hz, double max_hz, std::vector<float>& power,
                         int& bin_lo, int& nbins, std::vector<cf>& buf) {
    const Fft& fft = fft_plan(kSpecFft);
    bin_lo = int(std::floor(min_hz / kBinHz)) - 1;
    const int bin_hi = int(std::ceil((max_hz + 7 * kToneSpacingHz) / kBinHz)) + 2;
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

// Noise power per sample (sigma^2 of the complex baseband) as a function of
// frequency, from Hann-windowed spectra of one symbol every half symbol: the
// rectangular windows of the sync spectrogram leak a strong signal's power
// far into its neighbourhood, a Hann window does not. Per 6.25 Hz bin the
// lower quartile over time, scaled to the mean of an exponential
// distribution (its lower quartile is -ln(0.75) = 0.2877 of the mean), is
// noise where signals come and go; a transmission fills its own bins all the
// time, so the value used is the lower quartile of those over +-150 Hz,
// mostly bins between signals.
void estimate_noise(const std::vector<cf>& x, int first_sample, int last_sample, double min_hz, double max_hz,
                    std::vector<float>& noise, int& noise_bin_lo, std::vector<cf>& buf) {
    const int n = kFrameLen;
    const Fft& fft = fft_plan(size_t(n));
    const double bin_hz = kInternalRate / n;
    noise_bin_lo = int(std::floor(min_hz / bin_hz)) - 30;
    const int bin_hi = int(std::ceil((max_hz + 50.0) / bin_hz)) + 30;
    const int nb = bin_hi - noise_bin_lo;
    static const std::vector<float> hann = [] {
        std::vector<float> h(kFrameLen);
        for (int i = 0; i < kFrameLen; ++i)
            h[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / kFrameLen));
        return h;
    }();
    std::vector<std::vector<float>> cols{size_t(nb)};
    buf.resize(size_t(n));
    for (int start = first_sample; start + n <= last_sample; start += n / 2) {
        for (int i = 0; i < n; ++i)
            buf[size_t(i)] = x[size_t(start + i)] * hann[size_t(i)];
        fft.forward(buf.data());
        for (int b = 0; b < nb; ++b)
            cols[size_t(b)].push_back(std::norm(buf[size_t((noise_bin_lo + b) & (n - 1))]));
    }
    std::vector<float> raw(size_t(nb), 0.0f);
    for (int b = 0; b < nb; ++b) {
        auto& col = cols[size_t(b)];
        if (col.empty())
            continue;
        const size_t q = col.size() / 4;
        std::nth_element(col.begin(), col.begin() + long(q), col.end());
        // sum of the Hann window squared is 0.375 n
        raw[size_t(b)] = col[q] / 0.2877f / (0.375f * float(n));
    }
    noise.assign(size_t(nb), 0.0f);
    const int half = 24;
    std::vector<float> win;
    for (int b = 0; b < nb; ++b) {
        win.clear();
        for (int j = std::max(0, b - half); j <= std::min(nb - 1, b + half); ++j)
            win.push_back(raw[size_t(j)]);
        const size_t q = win.size() / 4;
        std::nth_element(win.begin(), win.begin() + long(q), win.end());
        noise[size_t(b)] = win[q];
    }
}

std::vector<Candidate> find_candidates(const SlotDecoder::Work& w, const DecoderTuning& tu, double min_hz, double max_hz) {
    const int nb = w.nbins;
    // Per frame and bin, the power summed over the 8 tones from that bin.
    std::vector<float> tone_sum(size_t(kFrames) * size_t(nb), 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        const float* row = &w.power[size_t(f) * size_t(nb)];
        float* out = &tone_sum[size_t(f) * size_t(nb)];
        for (int b = 0; b + 14 < nb; ++b) {
            float s = 0;
            for (int k = 0; k < kToneCount; ++k)
                s += row[b + 2 * k];
            out[b] = s;
        }
    }
    const int b_first = std::max(0, int(std::ceil(min_hz / kBinHz)) - w.bin_lo);
    const int b_last = std::min(nb - 15, int(std::floor(max_hz / kBinHz)) - w.bin_lo);
    const int frames = kLastStartFrame + 1;
    std::vector<float> score(size_t(std::max(0, b_last - b_first + 1)) * size_t(frames), 0.0f);
    auto at = [&](int b, int t) -> float& { return score[size_t(b - b_first) * size_t(frames) + size_t(t)]; };
    for (int b = b_first; b <= b_last; ++b) {
        for (int t = 0; t < frames; ++t) {
            float s[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            for (int blk = 0; blk < 3; ++blk) {
                for (int j = 0; j < 7; ++j) {
                    const int f = t + kFramesPerSymbol * (kCostasStart[blk] + j);
                    if (f >= kFrames)
                        break;
                    const float sig = w.power[size_t(f) * size_t(nb) + size_t(b + 2 * kCostas[j])];
                    s[blk] += sig;
                    n[blk] += tone_sum[size_t(f) * size_t(nb) + size_t(b)] - sig;
                }
            }
            // All three Costas arrays, or two when the transmission began
            // before the buffer or ran past its end.
            auto ratio = [](float sig, float other) { return other > 0 ? 7.0f * sig / other : 0.0f; };
            float r = ratio(s[0] + s[1] + s[2], n[0] + n[1] + n[2]);
            r = std::max(r, ratio(s[0] + s[1], n[0] + n[1]));
            r = std::max(r, ratio(s[1] + s[2], n[1] + n[2]));
            at(b, t) = r;
        }
    }
    std::vector<Candidate> all;
    for (int b = b_first; b <= b_last; ++b) {
        for (int t = 0; t < frames; ++t) {
            const float v = at(b, t);
            if (v < tu.sync_min)
                continue;
            bool peak = true;
            for (int db = -1; db <= 1 && peak; ++db)
                for (int dt = -1; dt <= 1; ++dt) {
                    if (!db && !dt)
                        continue;
                    const int bb = b + db, tt = t + dt;
                    if (bb < b_first || bb > b_last || tt < 0 || tt >= frames)
                        continue;
                    if (at(bb, tt) > v) {
                        peak = false;
                        break;
                    }
                }
            if (peak)
                all.push_back(Candidate{v, b, t});
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
        if (!near)
            kept.push_back(c);
        if (int(kept.size()) >= tu.max_candidates)
            break;
    }
    return kept;
}

// Costas power at 200 Hz baseband start n0 with tone 0 at 6.25*(-3.5) + df
// hertz: each symbol correlated on its own (noncoherent).
float costas_power(const std::vector<cf>& bb, int n0, double df, const cf (*tone_w)[kBbSps]) {
    cf rot[kBbSps];
    for (int n = 0; n < kBbSps; ++n) {
        const double a = -2.0 * M_PI * df * n / kBbRate;
        rot[n] = cf(float(std::cos(a)), float(std::sin(a)));
    }
    float total = 0;
    for (int blk = 0; blk < 3; ++blk) {
        for (int j = 0; j < 7; ++j) {
            const int start = n0 + kBbSps * (kCostasStart[blk] + j);
            if (start < 0 || start + kBbSps > kBbLen)
                continue;
            const cf* w = tone_w[kCostas[j]];
            cf acc(0, 0);
            for (int n = 0; n < kBbSps; ++n)
                acc += cmul(bb[size_t(start + n)], cmul(w[n], rot[n]));
            total += std::norm(acc);
        }
    }
    return total;
}

// Soft bits from blocks of `span` data symbols, per half of the message,
// the last block of a half shorter when 29 does not divide.
void block_llrs(const cf (*c)[kToneCount], int span, Llrs& llr) {
    for (int half = 0; half < 2; ++half) {
        for (int first = 0; first < 29; first += span) {
            const int len = std::min(span, 29 - first);
            const int d0 = half * 29 + first;
            const int combos = 1 << (3 * len);
            float best0[9], best1[9];
            for (int i = 0; i < 3 * len; ++i)
                best0[i] = best1[i] = -1.0f;
            for (int combo = 0; combo < combos; ++combo) {
                cf sum(0, 0);
                int bits = 0;
                for (int s = 0; s < len; ++s) {
                    const int tone = (combo >> (3 * (len - 1 - s))) & 7;
                    sum += c[data_symbol_index(d0 + s)][tone];
                    bits = (bits << 3) | kToneToBits[tone];
                }
                const float m = std::sqrt(std::norm(sum));
                for (int i = 0; i < 3 * len; ++i) {
                    const int bit = (bits >> (3 * len - 1 - i)) & 1;
                    float& slot = bit ? best1[i] : best0[i];
                    if (m > slot)
                        slot = m;
                }
            }
            for (int i = 0; i < 3 * len; ++i)
                llr[size_t(3 * d0 + i)] = best0[i] - best1[i];
        }
    }
}

// Single-symbol soft bits divided by the symbol's strongest tone, so that a
// symbol hit by an interfering signal carries no more weight than a clean
// one: its bits are decided by ratios, not by absolute power.
void relative_llrs(const cf (*c)[kToneCount], Llrs& llr) {
    for (int d = 0; d < kDataSymbolCount; ++d) {
        const cf* row = c[data_symbol_index(d)];
        float mag[kToneCount];
        float top = 1e-30f;
        for (int k = 0; k < kToneCount; ++k) {
            mag[k] = std::sqrt(std::norm(row[k]));
            top = std::max(top, mag[k]);
        }
        for (int i = 0; i < 3; ++i) {
            float b0 = 0, b1 = 0;
            for (int k = 0; k < kToneCount; ++k) {
                if ((kToneToBits[k] >> (2 - i)) & 1)
                    b1 = std::max(b1, mag[k]);
                else
                    b0 = std::max(b0, mag[k]);
            }
            llr[size_t(3 * d + i)] = (b0 - b1) / top;
        }
    }
}

void normalise(Llrs& llr, float scale) {
    double s2 = 0;
    for (float v : llr)
        s2 += double(v) * v;
    const double rms = std::sqrt(s2 / kLdpcN);
    const float k = rms > 0 ? float(scale / rms) : 0.0f;
    for (float& v : llr)
        v *= k;
}

// Moving average of length len, centred, applied twice (a triangle).
void smooth(std::vector<cf>& v, int len) {
    std::vector<cf> tmp(v.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<cf>& in = pass == 0 ? v : tmp;
        std::vector<cf>& out = pass == 0 ? tmp : v;
        const int n = int(in.size());
        const int h = len / 2;
        std::complex<double> acc = 0;
        int count = 0;
        // Window [i - h, i + h), clipped at the ends.
        int lo = 0, hi = 0;
        for (int i = 0; i < n; ++i) {
            while (hi < std::min(n, i + h)) {
                acc += std::complex<double>(in[size_t(hi)]);
                ++hi;
                ++count;
            }
            while (lo < i - h) {
                acc -= std::complex<double>(in[size_t(lo)]);
                ++lo;
                --count;
            }
            out[size_t(i)] = count ? cf(acc / double(count)) : cf(0, 0);
        }
    }
}

}  // namespace

std::vector<Decode> SlotDecoder::decode(std::vector<cf>& x, size_t valid_begin, size_t valid_end, double offset_hz,
                                        int64_t slot_start_ms, const DecodeSettings& settings,
                                        CallsignHashTable* hashes, SlotStats* stats_out) {
    Work& w = *work_;
    w.stats = SlotStats{};
    x.resize(kSlotSamples);
    valid_end = std::min(valid_end, size_t(kSlotSamples));
    const DecoderTuning tu = settings.custom_tuning ? settings.tuning : tuning_for_depth(settings.depth);
    const int64_t now_s = slot_start_ms / 1000;
    std::vector<Found> found;
    w.frame_first = int((valid_begin + kFrameStep - 1) / kFrameStep);
    w.frame_last = valid_end >= size_t(kFrameLen) ? int((valid_end - kFrameLen) / kFrameStep) : -1;
    w.frame_last = std::min(w.frame_last, kFrames - 1);
    if (w.frame_last < w.frame_first)
        return {};
    const Fft& slot_fft = fft_plan(kSlotFft);
    const Fft& bb_fft = fft_plan(kBbLen);
    w.slot.resize(kSlotFft);
    w.bb.resize(kBbLen);

    for (int pass = 1; pass <= tu.passes; ++pass) {
        w.stats.passes = pass;
        compute_spectrogram(x, settings.min_hz, settings.max_hz, w.power, w.bin_lo, w.nbins, w.fft_buf);
        if (pass == 1)
            estimate_noise(x, int(valid_begin), int(valid_end), settings.min_hz, settings.max_hz, w.noise,
                           w.noise_bin_lo, w.fft_buf);
        const std::vector<Candidate> cands = find_candidates(w, tu, settings.min_hz, settings.max_hz);
        w.stats.candidates += int(cands.size());
        std::copy(x.begin(), x.end(), w.slot.begin());
        std::fill(w.slot.begin() + kSlotSamples, w.slot.end(), cf(0, 0));
        slot_fft.forward(w.slot.data());
        int new_decodes = 0;

        for (const Candidate& cand : cands) {
            const double f_coarse = (w.bin_lo + cand.bin) * kBinHz;
            const double t_coarse = cand.frame * double(kFrameStep) / kInternalRate;
            bool seen = false;
            for (const Found& f : found)
                if (std::fabs(f.f0 - f_coarse) < 2.0 && std::fabs(f.t0 - t_coarse) < 0.08)
                    seen = true;
            if (seen)
                continue;

            // 200 Hz around the signal's centre, tapered from 60 to 90 Hz out.
            const long i0 = std::lround((f_coarse + kCentreHz) / kSlotBinHz);
            const double f_ext = double(i0) * kSlotBinHz;
            for (int k = -kBbLen / 2; k < kBbLen / 2; ++k) {
                const double fr = std::fabs(k * kSlotBinHz);
                float taper = 1.0f;
                if (fr >= 90.0)
                    taper = 0.0f;
                else if (fr > 60.0)
                    taper = float(0.5 * (1.0 + std::cos(M_PI * (fr - 60.0) / 30.0)));
                const long src = ((i0 + k) % kSlotFft + kSlotFft) % kSlotFft;
                w.bb[size_t((k + kBbLen) % kBbLen)] = w.slot[size_t(src)] * (taper / float(kSlotFft));
            }
            bb_fft.inverse(w.bb.data());

            // Fine sync: time, frequency, time again, then finer frequency.
            const double d0 = f_coarse + kCentreHz - f_ext;
            int n0 = int(std::lround(t_coarse * kBbRate));
            double df = d0;
            float best = -1;
            int best_n = n0;
            for (int dn = -12; dn <= 12; ++dn) {
                const float p = costas_power(w.bb, n0 + dn, df, w.tone_w);
                if (p > best) {
                    best = p;
                    best_n = n0 + dn;
                }
            }
            n0 = best_n;
            double best_f = df;
            best = -1;
            for (int k = -5; k <= 5; ++k) {
                const double f = d0 + 0.5 * k;
                const float p = costas_power(w.bb, n0, f, w.tone_w);
                if (p > best) {
                    best = p;
                    best_f = f;
                }
            }
            df = best_f;
            best = -1;
            for (int dn = -4; dn <= 4; ++dn) {
                const float p = costas_power(w.bb, n0 + dn, df, w.tone_w);
                if (p > best) {
                    best = p;
                    best_n = n0 + dn;
                }
            }
            n0 = best_n;

            // Tone amplitudes of all 79 symbols, referred to one time origin
            // so that neighbouring symbols can be added coherently: with tones
            // at half-integer multiples of the symbol rate from the centre, a
            // per-symbol correlator's phase alternates by pi per symbol.
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
            // Frequency and time from the phases. The Costas power hardly
            // changes with a frequency error of half a hertz or a time error
            // of 10 ms, but the phases do: from one symbol to the next the
            // phase turns by 2 pi df T, less 2 pi 6.25 Hz (k' - k) tau when the
            // tone changes from k to k' and the signal is tau late. The
            // products c[s+1] conj(c[s]), at the Costas tones and at the
            // strongest tone elsewhere (a wrong decision only adds noise),
            // give tau as the delay that lines up their phases best and df as
            // their common turn. A whole number of samples of tau moves the
            // start; the rest is taken off each tone's phase.
            double tau_rest = 0;
            auto correct_phases = [&]() {
                for (int k = 0; k < kToneCount; ++k) {
                    const double a = 2.0 * M_PI * kToneSpacingHz * (k - 3.5) * tau_rest;
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
                for (int blk = 0; blk < 3; ++blk)
                    for (int j = 0; j < 7; ++j)
                        tone[kCostasStart[blk] + j] = kCostas[j];
                std::complex<double> z[kSymbolCount - 1];
                int dk[kSymbolCount - 1];
                for (int s = 0; s + 1 < kSymbolCount; ++s) {
                    z[s] = std::complex<double>(cmul_conj(c[s + 1][tone[s + 1]], c[s][tone[s]]));
                    dk[s] = tone[s + 1] - tone[s];
                }
                // Delays within +-12 ms, in steps of a quarter millisecond. The
                // pairs with the same tone step are summed first.
                std::complex<double> by_dk[15] = {};
                for (int s = 0; s + 1 < kSymbolCount; ++s)
                    by_dk[dk[s] + 7] += z[s];
                double best_tau = 0, best_m = -1;
                std::complex<double> best_sum = 0;
                for (int q = 0; q < kTauSteps; ++q) {
                    std::complex<double> sum = 0;
                    for (int d = 0; d < 15; ++d)
                        sum += by_dk[d] * w.tau_rot[q][d];
                    const double m = std::norm(sum);
                    if (m > best_m) {
                        best_m = m;
                        best_tau = (q - kTauSteps / 2) * kTauStep;
                        best_sum = sum;
                    }
                }
                const double step = std::arg(best_sum) / (2.0 * M_PI * kSymbolSeconds);
                if (std::fabs(step) > 1.0)
                    break;
                df += step;
                const double tau = tau_rest + best_tau;
                const int whole = int(std::lround(tau * kBbRate));
                n0 += whole;
                tau_rest = tau - whole / kBbRate;
                demodulate(df);
                correct_phases();
                if (std::fabs(step) < 0.02 && std::fabs(best_tau) < 0.0005)
                    break;
            }
            int hits = 0;
            for (int blk = 0; blk < 3; ++blk)
                for (int j = 0; j < 7; ++j) {
                    const cf* row = c[kCostasStart[blk] + j];
                    int arg = 0;
                    for (int k = 1; k < kToneCount; ++k)
                        if (std::norm(row[k]) > std::norm(row[arg]))
                            arg = k;
                    hits += arg == kCostas[j];
                }
            if (hits < tu.min_costas_hits)
                continue;

            Llrs sets[4];
            for (int span = 1; span <= 3; ++span) {
                block_llrs(c, span, sets[span - 1]);
                normalise(sets[span - 1], tu.llr_scale);
            }
            relative_llrs(c, sets[3]);
            normalise(sets[3], tu.llr_scale);
            std::optional<Codeword> cw;
            DecodeMethod method = DecodeMethod::Bp;
            int iterations = 0;
            int hard_errors = 0;
            int span_used = 0;
            for (int s = 0; s < 4 && !cw; ++s) {
                ++w.stats.ldpc_runs;
                const BpResult r = bp_decode(sets[s], 30);
                if (r.converged && crc_ok(r.codeword)) {
                    const int he = hard_disagreements(r.codeword, sets[s]);
                    if (he <= tu.bp_max_hard) {
                        cw = r.codeword;
                        span_used = s + 1;
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
                for (int s : {2, 0}) {
                    ++w.stats.osd_runs;
                    const OsdResult r = osd_decode(sets[s], o);
                    if (r.found && r.disagreements <= osd_hard) {
                        cw = r.codeword;
                        span_used = s + 1;
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
            fd.t0 = n0 / kBbRate + tau_rest;
            Decode& d = fd.decode;
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

            // Rebuild, refine the start to a sample at 6400 Hz, estimate the
            // gain along the transmission and subtract.
            const int margin = 64;
            const long n_start = long(std::floor(fd.t0 * kInternalRate)) - margin;
            const int len = kSymbolCount * kInternalSamplesPerSymbol + 2 * margin;
            auto build_ref = [&](double t0) {
                w.ref.assign(size_t(len), cf(0, 0));
                add_ft8_waveform(fd.tones, kInternalRate, fd.f0, t0 - double(n_start) / kInternalRate, 1.0f,
                                 w.ref.data(), w.ref.size());
            };
            auto sample = [&](long i) { return (i >= 0 && i < kSlotSamples) ? x[size_t(i)] : cf(0, 0); };
            build_ref(fd.t0);
            // Timing metric: coherent over blocks of 8 symbols (1.28 s, over
            // which the channel's phase holds), so that a time error shows as
            // phase differences between tones; one symbol at a time it would
            // hardly show.
            auto corr = [&](int shift) {
                double total = 0;
                for (int blk = 0; blk < kSymbolCount; blk += 8) {
                    cf acc(0, 0);
                    const int end = std::min(kSymbolCount, blk + 8) * kInternalSamplesPerSymbol;
                    // Every fourth sample: the signal is 50 Hz wide, far
                    // below the 1600 Hz this still resolves.
                    for (int n = blk * kInternalSamplesPerSymbol; n < end; n += 2)
                        acc += cmul_conj(sample(n_start + margin + n + shift), w.ref[size_t(margin + n)]);
                    total += std::norm(acc);
                }
                return total;
            };
            // The start from the 200 Hz stage is good to a few samples at
            // 6400 Hz; search +-8 of them, then refine.
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
            fd.t0 += (shift + frac) / kInternalRate;
            build_ref(fd.t0);
            // The frequency from the sync search is good to about 0.05 Hz,
            // which turns the phase by more than half a turn over 12.64 s.
            // The phase step between neighbouring symbols' gains measures
            // what is left; twice, as the first step may be off by a bit.
            for (int iter = 0; iter < 2; ++iter) {
                cf prev(0, 0);
                std::complex<double> steps = 0;
                for (int s = 0; s < kSymbolCount; ++s) {
                    cf acc(0, 0);
                    const int base = margin + s * kInternalSamplesPerSymbol;
                    for (int n = 0; n < kInternalSamplesPerSymbol; n += 2)
                        acc += cmul_conj(sample(n_start + base + n), w.ref[size_t(base + n)]);
                    if (s > 0)
                        steps += std::complex<double>(cmul_conj(acc, prev));
                    prev = acc;
                }
                const double dfix = std::arg(steps) / (2.0 * M_PI * kSymbolSeconds);
                if (std::fabs(dfix) > 0.5)
                    break;
                fd.f0 += dfix;
                // Shifting the rebuilt signal in frequency is a phase ramp
                // from its start.
                const double t_rel = fd.t0 - double(n_start) / kInternalRate;
                std::complex<double> ph = std::polar(1.0, -2.0 * M_PI * dfix * t_rel);
                const std::complex<double> stp = std::polar(1.0, 2.0 * M_PI * dfix / kInternalRate);
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
            smooth(w.gain_z, kInternalSamplesPerSymbol);
            smooth(w.gain_e, kInternalSamplesPerSymbol);
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
            // SNR: the gain's power over the noise in 2500 Hz. The gain
            // estimate averages about 1365 samples; their noise is taken off.
            double sigma2 = 0;
            {
                const int nb = int(w.noise.size());
                const int b = int(std::lround((fd.f0 + kCentreHz) / (kInternalRate / kFrameLen))) - w.noise_bin_lo;
                if (b >= 0 && b < nb)
                    sigma2 = w.noise[size_t(b)];
            }
            double ps = sig_n ? sig / sig_n - sigma2 / 1365.0 : 0.0;
            double snr = -30;
            if (ps > 0 && sigma2 > 0)
                snr = 10.0 * std::log10(ps * kInternalRate / (sigma2 * 2500.0));
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

}  // namespace fern::ft8
