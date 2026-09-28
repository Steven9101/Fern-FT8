// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "channel.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace fern::ft8 {

namespace {

constexpr int64_t kGridPerSlot = 96000;  // 15 s at 6400 Hz
constexpr int64_t kLead = 9600;          // 1.5 s
constexpr int64_t kRing = 6400 * 64;
constexpr double kUsPerGrid = 1e6 / kInternalRate;  // 156.25, exact in binary
// Clock differences below this are taken as jitter, not a new start.
constexpr double kMaxSlipUs = 20000;

int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

double thread_cpu_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

double passband(const ChannelConfig& c) { return std::min(0.5 * c.width_hz + 100.0, 0.45 * c.rate); }

}  // namespace

Channel::Channel(const ChannelConfig& config, CallsignHashTable* hashes)
    : config_(config),
      hashes_(hashes),
      interp_((config.rate > 0 && std::isfinite(config.rate)) ? config.rate : 1.0, kInternalRate, passband(config)) {
    if (!(config.rate >= 500.0 && config.rate <= 1e7))
        throw std::invalid_argument("channel rate must be between 500 and 1e7 samples a second");
    if (!(config.width_hz > 100.0 && config.width_hz <= 5400.0))
        throw std::invalid_argument("channel width must be above 100 and at most 5400 Hz");
    if (config.depth < 1 || config.depth > 3)
        throw std::invalid_argument("depth must be 1, 2 or 3");
    // The search stays inside the channel and inside what the resampler
    // passes; tone 7 lies 43.75 Hz above tone 0.
    const double pass = passband(config);
    double lo = std::max(-0.5 * config.width_hz, -pass + 10.0);
    double hi = std::min(0.5 * config.width_hz, pass - 10.0) - 50.0;
    if (config.min_freq_hz > 0)
        lo = std::max(lo, config.min_freq_hz - config.offset_hz);
    if (config.max_freq_hz > 0)
        hi = std::min(hi, config.max_freq_hz - config.offset_hz);
    if (hi <= lo)
        throw std::invalid_argument("the frequency range searched is empty");
    settings_.depth = config.depth;
    settings_.min_hz = lo;
    settings_.max_hz = hi;
    settings_.unpack = config.unpack;
    ring_.assign(size_t(kRing), std::complex<float>(0, 0));
    have_.assign(size_t(kRing), 0);
}

void Channel::anchor(uint64_t index, int64_t utc_us) {
    anchored_ = true;
    anchor_index_ = index;
    anchor_utc_us_ = utc_us;
    hist_.clear();
    hist_start_ = index;
    const int64_t g_first = int64_t(std::ceil(double(utc_us) / kUsPerGrid));
    if (slot_known_ && g_first <= written_) {
        // The clock went back: what is buffered no longer lines up.
        std::fill(have_.begin(), have_.end(), 0);
        slot_known_ = false;
    }
    if (slot_known_) {
        for (int64_t g = written_ + 1; g < g_first && g < written_ + 1 + kRing; ++g)
            have_[size_t(g % kRing)] = 0;
        written_ = g_first - 1;
    } else {
        // The first slot worth decoding is the one whose transmissions could
        // have started 1.5 s before these samples at the earliest.
        next_slot_ = floor_div(g_first - 12800 + kGridPerSlot - 1, kGridPerSlot);
        written_ = g_first - 1;
        slot_known_ = true;
    }
    next_g_ = g_first;
}

void Channel::push(const std::complex<float>* samples, size_t count, uint64_t first_index, int64_t utc_us,
                   uint32_t flags) {
    if (count == 0)
        return;
    bool restart = !anchored_ || (flags & (kFrameSamplesLost | kFrameClockSet)) || first_index != next_index_;
    if (!restart) {
        const double predicted = double(anchor_utc_us_) + double(first_index - anchor_index_) * 1e6 / config_.rate;
        restart = std::fabs(double(utc_us) - predicted) > kMaxSlipUs;
    }
    if (restart)
        anchor(first_index, utc_us);
    hist_.insert(hist_.end(), samples, samples + count);
    next_index_ = first_index + count;
    produce();
}

void Channel::produce() {
    const int half = interp_.half_width();
    const double scale = config_.rate / 1e6;
    std::vector<std::complex<float>> edge(size_t(2 * half));
    for (;;) {
        const double p = double(anchor_index_) + (double(next_g_) * kUsPerGrid - double(anchor_utc_us_)) * scale;
        const double fl = std::floor(p);
        const int64_t i = int64_t(fl);
        if (i + half >= int64_t(next_index_))
            break;
        const int64_t first = i - half + 1;
        std::complex<float> v;
        if (first >= int64_t(hist_start_)) {
            v = interp_.at(&hist_[size_t(first - int64_t(hist_start_))], p - fl);
        } else {
            // Just after a new start: the samples before it read as zero.
            for (int t = 0; t < 2 * half; ++t) {
                const int64_t j = first + t;
                edge[size_t(t)] = j >= int64_t(hist_start_) ? hist_[size_t(j - int64_t(hist_start_))]
                                                            : std::complex<float>(0, 0);
            }
            v = interp_.at(edge.data(), p - fl);
        }
        ring_[size_t(next_g_ % kRing)] = v;
        have_[size_t(next_g_ % kRing)] = 1;
        written_ = next_g_;
        ++next_g_;
    }
    // Drop input no longer needed, in large steps.
    const double p = double(anchor_index_) + (double(next_g_) * kUsPerGrid - double(anchor_utc_us_)) * scale;
    const int64_t keep_from = int64_t(std::floor(p)) - half - 1;
    if (keep_from > int64_t(hist_start_) + 65536) {
        const size_t drop = size_t(keep_from - int64_t(hist_start_));
        hist_.erase(hist_.begin(), hist_.begin() + long(std::min(drop, hist_.size())));
        hist_start_ += drop;
    }
}

SlotResult Channel::decode_slot(int64_t slot) {
    SlotResult r;
    r.slot_start_ms = slot * 15000;
    const int64_t g0 = slot * kGridPerSlot - kLead;
    slot_buf_.assign(size_t(kSlotSamples), std::complex<float>(0, 0));
    size_t valid_begin = kSlotSamples, valid_end = 0, missing = 0;
    for (int64_t n = 0; n < kSlotSamples; ++n) {
        const int64_t g = g0 + n;
        const bool ok = g <= written_ && g > written_ - kRing && have_[size_t(g % kRing)];
        if (ok) {
            slot_buf_[size_t(n)] = ring_[size_t(g % kRing)];
            valid_begin = std::min(valid_begin, size_t(n));
            valid_end = size_t(n) + 1;
        } else {
            ++missing;
        }
    }
    r.missing = double(missing) / kSlotSamples;
    if (valid_end <= valid_begin)
        return r;
    const double t0 = thread_cpu_seconds();
    r.decodes = decoder_.decode(slot_buf_, valid_begin, valid_end, config_.offset_hz, r.slot_start_ms, settings_,
                                hashes_, &r.stats);
    r.cpu_seconds = thread_cpu_seconds() - t0;
    return r;
}

std::vector<SlotResult> Channel::decode_ready() {
    std::vector<SlotResult> out;
    while (slot_known_ && written_ >= next_slot_ * kGridPerSlot - kLead + kSlotSamples - 1) {
        // A slot whose start has left the ring is lost, not decoded.
        if (next_slot_ * kGridPerSlot - kLead > written_ - kRing)
            out.push_back(decode_slot(next_slot_));
        ++next_slot_;
    }
    return out;
}

std::vector<SlotResult> Channel::finish() {
    std::vector<SlotResult> out = decode_ready();
    while (slot_known_ && written_ >= next_slot_ * kGridPerSlot + 12800) {
        out.push_back(decode_slot(next_slot_));
        ++next_slot_;
    }
    return out;
}

}  // namespace fern::ft8
