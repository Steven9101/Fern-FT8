// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// fern-ft8: decode WAV files, make test signals, measure speed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "channel.h"
#include "gfsk.h"
#include "message.h"
#include "module.h"
#include "resampler.h"
#include "simd.h"
#include "wav.h"

using namespace fern::ft8;

namespace {

void usage() {
    std::fprintf(stderr,
                 "Fern-FT8 %s, an FT8 and FT4 decoder (receive only).\n"
                 "\n"
                 "usage:\n"
                 "  fern-ft8 decode FILE.wav... [--mode ft8|ft4] [--depth 1|2|3] [--rate-test R]\n"
                 "                  [--verbose]\n"
                 "      Decode 15 s FT8 slots (7.5 s FT4 slots with --mode ft4) of real\n"
                 "      audio, printed as jt9 prints them: HHMMSS SNR DT FREQ ~ MESSAGE\n"
                 "      (+ for FT4). The audio goes through the same complex baseband\n"
                 "      path as live channels; --rate-test resamples it to R complex\n"
                 "      samples a second first. The slot time comes from a YYMMDD_HHMMSS\n"
                 "      file name, else 000000.\n"
                 "  fern-ft8 encode \"MESSAGE\" [--mode ft8|ft4] [--freq HZ] [--snr DB] [--dt S]\n"
                 "                  [--rate HZ] [--seed N] -o OUT.wav\n"
                 "      Write a 15 s FT8 slot (7.5 s for FT4) with one transmission of\n"
                 "      MESSAGE (default 1500 Hz, DT 0, 12000 Hz), with white noise at SNR\n"
                 "      DB in 2500 Hz when --snr is given. --tones DIGITS in place of the\n"
                 "      message sends tones as ft8code or ft4code prints them; --fading\n"
                 "      HZ,MS sends it over two fading paths, HZ of Doppler spread, the\n"
                 "      second MS later.\n"
                 "  fern-ft8 noise [--mode ft8|ft4] [--minutes M] [--depth N] [--seed S] [--rate R]\n"
                 "                 [--silence]\n"
                 "      Feed M minutes (default 60) of white Gaussian noise, or digital\n"
                 "      silence, through a live channel at R complex samples a second\n"
                 "      (default 8000) and print every decode: each one is false.\n"
                 "  fern-ft8 bench [--mode ft8|ft4] [--depth N] [FILE.wav...]\n"
                 "      CPU time per busy and per quiet slot on one thread, for both\n"
                 "      modes unless --mode names one.\n",
                 FERN_FT8_VERSION);
}

double thread_cpu() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

// Seconds of the day and UTC seconds since 1970 of a YYMMDD_HHMMSS name.
bool slot_time_from_name(const std::string& path, int64_t& utc, int& hhmmss) {
    const size_t slash = path.find_last_of('/');
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    for (size_t i = 0; i + 13 <= name.size(); ++i) {
        bool ok = name[i + 6] == '_';
        for (size_t k = 0; k < 13 && ok; ++k)
            if (k != 6 && (name[i + k] < '0' || name[i + k] > '9'))
                ok = false;
        if (!ok)
            continue;
        auto num = [&](size_t at) { return (name[i + at] - '0') * 10 + (name[i + at + 1] - '0'); };
        tm t{};
        t.tm_year = 100 + num(0);
        t.tm_mon = num(2) - 1;
        t.tm_mday = num(4);
        t.tm_hour = num(7);
        t.tm_min = num(9);
        t.tm_sec = num(11);
        utc = int64_t(timegm(&t));
        hhmmss = num(7) * 10000 + num(9) * 100 + num(11);
        return true;
    }
    return false;
}

// "ft8" or "ft4" into mode; false for anything else.
bool parse_mode(const char* text, Mode& mode) {
    if (!text)
        return false;
    const std::string t = text;
    if (t != "ft8" && t != "ft4")
        return false;
    mode = t == "ft4" ? Mode::Ft4 : Mode::Ft8;
    return true;
}

struct Options {
    Mode mode = Mode::Ft8;
    int depth = 2;
    double rate_test = 0;
    bool verbose = false;
    std::string tune;  // "key=value,..." over the depth's defaults
};

// Applies --tune to a channel configuration; exits on a bad spec.
void apply_tune(ChannelConfig& cfg, const std::string& tune) {
    if (tune.empty())
        return;
    cfg.custom_tuning = true;
    cfg.tuning = cfg.mode == Mode::Ft4 ? ft4::tuning_for_depth(cfg.depth) : tuning_for_depth(cfg.depth);
    if (!apply_tuning(cfg.tuning, tune))
        throw std::invalid_argument("bad --tune: " + tune);
}

// Real audio at `rate` to complex baseband with audio 2000 Hz at 0 Hz, as a
// FernSDR channel would deliver it, then optionally resampled to rate_test.
std::vector<SlotResult> decode_audio(const Audio& audio, int64_t utc, const Options& o, CallsignHashTable& hashes,
                                     double* cpu) {
    const double offset = 2000.0;
    std::vector<std::complex<float>> x(audio.samples.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double a = -2.0 * M_PI * offset * double(i) / audio.rate;
        x[i] = std::complex<float>(float(audio.samples[i] * std::cos(a)), float(audio.samples[i] * std::sin(a)));
    }
    double rate = audio.rate;
    if (o.rate_test > 0) {
        x = resample(x, audio.rate, o.rate_test, 2200.0);
        rate = o.rate_test;
    }
    ChannelConfig cfg;
    cfg.mode = o.mode;
    cfg.rate = rate;
    cfg.offset_hz = offset;
    cfg.width_hz = 4400;
    cfg.min_freq_hz = 200;
    cfg.max_freq_hz = 4000;
    cfg.depth = o.depth;
    apply_tune(cfg, o.tune);
    Channel ch(cfg, &hashes);
    const double t0 = thread_cpu();
    std::vector<SlotResult> results;
    const size_t frame = 4096;
    for (size_t i = 0; i < x.size(); i += frame) {
        const size_t n = std::min(frame, x.size() - i);
        const int64_t us = utc * 1000000 + int64_t(std::llround(double(i) * 1e6 / rate));
        ch.push(&x[i], n, i, us, 0);
        for (auto& r : ch.decode_ready())
            results.push_back(std::move(r));
    }
    for (auto& r : ch.finish())
        results.push_back(std::move(r));
    if (cpu)
        *cpu = thread_cpu() - t0;
    return results;
}

void print_decode(const Decode& d, int hhmmss, bool verbose) {
    const int64_t sec_of_day = (d.slot_start_ms / 1000) % 86400;
    const int hms = hhmmss >= 0 ? hhmmss
                                : int(sec_of_day / 3600 * 10000 + sec_of_day % 3600 / 60 * 100 + sec_of_day % 60);
    // jt9 marks FT8 decodes with ~ and FT4 decodes with +.
    std::printf("%06d %3d %4.1f %4d %c  %s", hms, d.snr_db, std::fabs(d.dt) < 0.05 ? 0.0 : d.dt,
                int(std::lround(d.freq_hz)), d.mode == Mode::Ft4 ? '+' : '~', d.message.text.c_str());
    if (verbose)
        std::printf("   [%d.%d %s pass %d span %d it %d hard %d costas %d sync %.2f]", d.message.i3, d.message.n3,
                    d.quality(), d.pass, d.llr_span, d.ldpc_iterations, d.hard_errors, d.costas_hits, d.sync);
    std::printf("\n");
}

int cmd_decode(int argc, char** argv) {
    Options o;
    std::vector<std::string> files;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--depth" && i + 1 < argc)
            o.depth = std::atoi(argv[++i]);
        else if (a == "--mode" && i + 1 < argc && parse_mode(argv[i + 1], o.mode))
            ++i;
        else if (a == "--rate-test" && i + 1 < argc)
            o.rate_test = std::atof(argv[++i]);
        else if (a == "--verbose")
            o.verbose = true;
        else if (a == "--tune" && i + 1 < argc)
            o.tune = argv[++i];
        else if (!a.empty() && a[0] == '-') {
            usage();
            return 2;
        } else
            files.push_back(a);
    }
    if (files.empty() || o.depth < 1 || o.depth > 3) {
        usage();
        return 2;
    }
    CallsignHashTable hashes;
    for (const std::string& f : files) {
        const Audio audio = read_wav(f);
        int64_t utc = 0;
        int hhmmss = 0;
        const bool named = slot_time_from_name(f, utc, hhmmss);
        if (!named)
            hhmmss = 0;
        double cpu = 0;
        const auto results = decode_audio(audio, utc, o, hashes, &cpu);
        for (const SlotResult& r : results) {
            for (const Decode& d : r.decodes)
                print_decode(d, results.size() == 1 ? hhmmss : -1, o.verbose);
            if (o.verbose)
                std::fprintf(stderr, "slot %lld: %d passes, %d candidates, %d LDPC runs, %d OSD runs, %.3f s CPU\n",
                             (long long)(r.slot_start_ms / 1000), r.stats.passes, r.stats.candidates,
                             r.stats.ldpc_runs, r.stats.osd_runs, r.cpu_seconds);
        }
        if (o.verbose)
            std::fprintf(stderr, "%s: %.3f s CPU\n", f.c_str(), cpu);
    }
    return 0;
}

// Tones as WSJT-X's ft8code and ft4code print them, digits with or without
// spaces: FT8's 79, or FT4's 103 with or without the ramp symbol (0) at each
// end. False when they are not that.
bool parse_tones(const std::string& text, Mode mode, std::vector<uint8_t>& tones) {
    tones.clear();
    for (char ch : text) {
        if (ch == ' ')
            continue;
        if (ch < '0' || ch > (mode == Mode::Ft4 ? '3' : '7'))
            return false;
        tones.push_back(uint8_t(ch - '0'));
    }
    if (mode == Mode::Ft4 && tones.size() == size_t(ft4::kWaveSymbolCount) && tones.front() == 0 &&
        tones.back() == 0)
        tones = std::vector<uint8_t>(tones.begin() + 1, tones.end() - 1);
    return tones.size() == size_t(mode == Mode::Ft4 ? ft4::kSymbolCount : kSymbolCount);
}

// The complex gain of one propagation path: Gaussian, with a Gaussian
// Doppler spectrum whose width (two standard deviations) is spread_hz, the
// CCIR's definition of frequency spread that [QEX] section 8 uses; mean power
// one. White Gaussian samples at 200 Hz are smoothed by the matching
// Gaussian in time and read at `rate` by linear interpolation.
std::vector<std::complex<double>> path_gain(size_t n, double rate, double spread_hz, std::mt19937& rng) {
    const double low_rate = 200.0;
    // A Gaussian of standard deviation sigma_t in time has a power spectrum
    // of standard deviation 1 / (2 sqrt(2) pi sigma_t).
    const double sigma_t = low_rate / (2.0 * std::sqrt(2.0) * M_PI * 0.5 * spread_hz);  // in low-rate samples
    const int half = int(std::ceil(4.0 * sigma_t));
    std::vector<double> taps(size_t(2 * half + 1));
    double energy = 0;
    for (int i = -half; i <= half; ++i) {
        taps[size_t(i + half)] = std::exp(-0.5 * i * i / (sigma_t * sigma_t));
        energy += taps[size_t(i + half)] * taps[size_t(i + half)];
    }
    // Unit-power complex white noise through taps of unit energy.
    for (double& t : taps)
        t /= std::sqrt(energy);
    std::normal_distribution<double> g(0.0, std::sqrt(0.5));
    const size_t m = size_t(double(n) / rate * low_rate) + 2;
    std::vector<std::complex<double>> white(m + taps.size());
    for (auto& v : white)
        v = std::complex<double>(g(rng), g(rng));
    std::vector<std::complex<double>> slow(m);
    for (size_t i = 0; i < m; ++i)
        for (size_t j = 0; j < taps.size(); ++j)
            slow[i] += white[i + j] * taps[j];
    std::vector<std::complex<double>> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double p = double(i) / rate * low_rate;
        const size_t q = size_t(p);
        out[i] = slow[q] + (p - double(q)) * (slow[q + 1] - slow[q]);
    }
    return out;
}

int cmd_encode(int argc, char** argv) {
    std::string text, out, tone_text;
    Mode mode = Mode::Ft8;
    double freq = 1500, dt = 0, rate = 12000;
    double spread = 0, delay_ms = 0;
    double snr = std::nan("");
    unsigned seed = 1;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--freq" && (v = next()))
            freq = std::atof(v);
        else if (a == "--snr" && (v = next()))
            snr = std::atof(v);
        else if (a == "--dt" && (v = next()))
            dt = std::atof(v);
        else if (a == "--rate" && (v = next()))
            rate = std::atof(v);
        else if (a == "--seed" && (v = next()))
            seed = unsigned(std::strtoul(v, nullptr, 10));
        else if (a == "-o" && (v = next()))
            out = v;
        else if (a == "--mode" && (v = next()) && parse_mode(v, mode))
            ;
        else if (a == "--tones" && (v = next()))
            tone_text = v;
        else if (a == "--fading" && (v = next()) && std::sscanf(v, "%lf,%lf", &spread, &delay_ms) == 2 &&
                 spread > 0 && spread <= 50 && delay_ms >= 0 && delay_ms <= 10)
            ;
        else if (text.empty() && !a.empty() && a[0] != '-')
            text = a;
        else {
            usage();
            return 2;
        }
    }
    const bool ft4 = mode == Mode::Ft4;
    if (text.empty() == tone_text.empty() || out.empty() || rate < 2 * (freq + (ft4 ? 100 : 60)) || freq < 0) {
        usage();
        return 2;
    }
    std::vector<uint8_t> tones;
    if (!tone_text.empty()) {
        if (!parse_tones(tone_text, mode, tones)) {
            std::fprintf(stderr, "fern-ft8: --tones wants the %s tones as ft%ccode prints them\n",
                         ft4 ? "103 (or 105)" : "79", ft4 ? '4' : '8');
            return 2;
        }
    } else {
        const auto payload = pack_message(text);
        if (!payload) {
            std::fprintf(stderr, "fern-ft8: no FT8 message type carries \"%s\"\n", text.c_str());
            return 1;
        }
        if (ft4) {
            const ft4::Tones t = ft4::tones_of(ft4::encode_codeword(*payload));
            tones.assign(t.begin(), t.end());
        } else {
            const Tones t = tones_of(encode_codeword(*payload));
            tones.assign(t.begin(), t.end());
        }
    }
    Audio audio;
    audio.rate = rate;
    const size_t n = size_t(std::llround((ft4 ? ft4::kSlotSeconds : kSlotSeconds) * rate));
    std::vector<std::complex<float>> wave(n);
    // Real noise of standard deviation sigma has one-sided density
    // 2 sigma^2 / rate; a sine of amplitude A has power A^2 / 2, so
    // SNR = A^2 rate / (4 * 2500 * sigma^2).
    const double sigma = 0.03;
    double amp = 0.3;
    if (!std::isnan(snr))
        amp = std::sqrt(std::pow(10.0, snr / 10.0) * 4.0 * 2500.0 * sigma * sigma / rate);
    auto transmit = [&](double start, std::vector<std::complex<float>>& to) {
        if (ft4) {
            ft4::Tones t{};
            std::copy(tones.begin(), tones.end(), t.begin());
            add_ft4_waveform(t, rate, freq, start, 1.0f, to.data(), n);
        } else {
            Tones t{};
            std::copy(tones.begin(), tones.end(), t.begin());
            add_ft8_waveform(t, rate, freq, start, 1.0f, to.data(), n);
        }
    };
    const double start = (ft4 ? ft4::kStartSeconds : kStartSeconds) + dt;
    transmit(start, wave);
    if (spread > 0) {
        // Two paths of equal mean power, the second delay_ms later, each
        // with its own fading gain: the channels of [QEX] Table 6.
        std::vector<std::complex<float>> late(n);
        transmit(start + delay_ms / 1000.0, late);
        std::mt19937 fade_rng(seed * 7919u + 1u);
        const auto g1 = path_gain(n, rate, spread, fade_rng);
        const auto g2 = path_gain(n, rate, spread, fade_rng);
        for (size_t i = 0; i < n; ++i)
            wave[i] = std::complex<float>((g1[i] * std::complex<double>(wave[i]) +
                                           g2[i] * std::complex<double>(late[i])) * std::sqrt(0.5));
    }
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    audio.samples.resize(n);
    for (size_t i = 0; i < n; ++i)
        audio.samples[i] = float(amp * wave[i].real() + (std::isnan(snr) ? 0.0 : g(rng)));
    write_wav(out, audio);
    return 0;
}

// A busy slot: `signals` transmissions at random frequencies from 200 to
// 3000 Hz, SNRs from -20 to +10 dB and DT from -0.5 to 1.5 s (FT4: SNRs
// from -16 to +10 dB, DT from -0.5 to 0.8 s), in white noise; they overlap as
// on a busy band.
Audio synthetic_busy_slot(unsigned seed, int signals, Mode mode = Mode::Ft8) {
    std::mt19937 rng(seed);
    const bool ft4 = mode == Mode::Ft4;
    const double rate = 12000, sigma = 0.02;
    const size_t n = size_t((ft4 ? ft4::kSlotSeconds : kSlotSeconds) * rate);
    std::vector<std::complex<float>> sum(n);
    auto call = [&]() {
        const char* prefixes[] = {"K", "W", "DL", "JA", "G", "VK", "PY", "EA", "OH", "SP", "HB9", "R"};
        std::string c = prefixes[rng() % 12];
        if (c.size() < 3)
            c += char('0' + rng() % 10);
        for (int i = 0; i < 2 + int(rng() % 2); ++i)
            c += char('A' + rng() % 26);
        return c;
    };
    const char* tails[] = {"FN42", "-12", "R-07", "RR73", "73", "JO31", "+03", "RRR"};
    for (int s = 0; s < signals; ++s) {
        const std::string text = (s % 4 == 0 ? std::string("CQ") : call()) + " " + call() + " " +
                                 (s % 4 == 0 ? std::string("IO91") : std::string(tails[rng() % 8]));
        auto p = pack_message(text);
        if (!p)
            continue;
        const double f = 200 + 2800.0 * (rng() % 1000) / 1000.0;
        const double dt = -0.5 + (ft4 ? 1.3 : 2.0) * (rng() % 1000) / 1000.0;
        const double snr = (ft4 ? -16 : -20) + (ft4 ? 26.0 : 30.0) * (rng() % 1000) / 1000.0;
        const double amp = std::sqrt(std::pow(10.0, snr / 10.0) * 4.0 * 2500.0 * sigma * sigma / rate);
        std::vector<std::complex<float>> w(n);
        if (ft4)
            add_ft4_waveform(ft4::tones_of(ft4::encode_codeword(*p)), rate, f, ft4::kStartSeconds + dt, float(amp),
                             w.data(), n);
        else
            add_ft8_waveform(tones_of(encode_codeword(*p)), rate, f, kStartSeconds + dt, float(amp), w.data(), n);
        for (size_t i = 0; i < n; ++i)
            sum[i] += w[i];
    }
    std::normal_distribution<double> g(0.0, sigma);
    Audio a;
    a.rate = rate;
    a.samples.resize(n);
    for (size_t i = 0; i < n; ++i)
        a.samples[i] = float(sum[i].real() + g(rng));
    return a;
}

std::string isa_description() {
    std::string s;
#if defined(__x86_64__)
    s = "x86-64";
#elif defined(__aarch64__)
    s = "aarch64";
#elif defined(__arm__)
    s = "arm";
#else
    s = "other CPU";
#endif
    return s + ", vector code: " + simd_name(simd_level()) + " (CPU supports " + simd_name(simd_supported()) + ")";
}

int cmd_noise(int argc, char** argv) {
    Mode mode = Mode::Ft8;
    double minutes = 60, rate = 8000;
    int depth = 3;
    unsigned seed = 1;
    bool silence = false;
    std::string tune;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--minutes" && (v = next()))
            minutes = std::atof(v);
        else if (a == "--mode" && (v = next()) && parse_mode(v, mode))
            ;
        else if (a == "--depth" && (v = next()))
            depth = std::atoi(v);
        else if (a == "--seed" && (v = next()))
            seed = unsigned(std::strtoul(v, nullptr, 10));
        else if (a == "--rate" && (v = next()))
            rate = std::atof(v);
        else if (a == "--silence")
            silence = true;
        else if (a == "--tune" && (v = next()))
            tune = v;
        else {
            usage();
            return 2;
        }
    }
    ChannelConfig cfg;
    cfg.mode = mode;
    cfg.rate = rate;
    cfg.depth = depth;
    apply_tune(cfg, tune);
    CallsignHashTable hashes;
    Channel ch(cfg, &hashes);
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> g(0.0f, 0.1f);
    // Start on a slot boundary a day after the epoch, like a live stream.
    const int64_t start_us = 86400LL * 1000000;
    const size_t frame = 4096;
    const uint64_t total = uint64_t(minutes * 60.0 * rate);
    std::vector<std::complex<float>> buf(frame);
    size_t slots = 0, decodes = 0;
    double cpu = 0;
    auto take = [&](std::vector<SlotResult> results) {
        for (const SlotResult& r : results) {
            ++slots;
            cpu += r.cpu_seconds;
            for (const Decode& d : r.decodes) {
                ++decodes;
                print_decode(d, -1, true);
            }
        }
    };
    for (uint64_t i = 0; i < total; i += frame) {
        const size_t n = size_t(std::min<uint64_t>(frame, total - i));
        for (size_t k = 0; k < n; ++k)
            buf[k] = silence ? std::complex<float>(0, 0) : std::complex<float>(g(rng), g(rng));
        ch.push(buf.data(), n, i, start_us + int64_t(std::llround(double(i) * 1e6 / rate)), 0);
        take(ch.decode_ready());
    }
    take(ch.finish());
    std::printf("%zu %s slots (%.1f minutes) of %s at depth %d: %zu decodes, %.3f s CPU per slot\n", slots,
                mode == Mode::Ft4 ? "FT4" : "FT8", double(slots) / (mode == Mode::Ft4 ? 8.0 : 4.0),
                silence ? "silence" : "white noise", depth, decodes, slots ? cpu / double(slots) : 0.0);
    return decodes == 0 ? 0 : 3;
}

int cmd_bench(int argc, char** argv) {
    int depth = 0;
    bool only = false;
    Mode only_mode = Mode::Ft8;
    std::string tune;
    std::vector<std::string> files;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--depth" && i + 1 < argc)
            depth = std::atoi(argv[++i]);
        else if (a == "--mode" && i + 1 < argc && parse_mode(argv[i + 1], only_mode))
            only = ++i > 0;
        else if (a == "--tune" && i + 1 < argc)
            tune = argv[++i];
        else
            files.push_back(a);
    }
    std::printf("Fern-FT8 %s bench, one thread, %s\n", FERN_FT8_VERSION, isa_description().c_str());
    std::vector<int> depths = depth ? std::vector<int>{depth} : std::vector<int>{1, 2, 3};
    struct Case {
        std::string name;
        Mode mode;
        Audio audio;
    };
    std::vector<Case> cases;
    if (files.empty()) {
        if (!only || only_mode == Mode::Ft8) {
            cases.push_back({"FT8 synthetic busy slot (30 signals)", Mode::Ft8, synthetic_busy_slot(11, 30)});
            cases.push_back({"FT8 quiet slot (noise only)", Mode::Ft8, synthetic_busy_slot(12, 0)});
        }
        if (!only || only_mode == Mode::Ft4) {
            cases.push_back(
                {"FT4 synthetic busy slot (30 signals)", Mode::Ft4, synthetic_busy_slot(13, 30, Mode::Ft4)});
            cases.push_back({"FT4 quiet slot (noise only)", Mode::Ft4, synthetic_busy_slot(14, 0, Mode::Ft4)});
        }
    } else {
        for (const auto& f : files)
            cases.push_back({f, only_mode, read_wav(f)});
    }
    for (int d : depths) {
        for (const Case& c : cases) {
            Options o;
            o.mode = c.mode;
            o.depth = d;
            o.tune = tune;
            double best = 1e9;
            size_t decodes = 0;
            // Best of three runs, each with a fresh hash table.
            for (int run = 0; run < 3; ++run) {
                CallsignHashTable hashes;
                double cpu = 0;
                const auto r = decode_audio(c.audio, 0, o, hashes, &cpu);
                decodes = 0;
                for (const auto& s : r)
                    decodes += s.decodes.size();
                best = std::min(best, cpu);
            }
            std::printf("depth %d  %-44s %6.3f s CPU  %3zu decodes\n", d, c.name.c_str(), best, decodes);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    // As FernSDR starts it: a decoder session on its descriptors.
    if (cmd == "--fernsdr-module") return fern::ft8::run_module(argc, argv);
    if (cmd == "--describe") return fern::ft8::describe_module();
    try {
        if (cmd == "decode")
            return cmd_decode(argc - 2, argv + 2);
        if (cmd == "encode")
            return cmd_encode(argc - 2, argv + 2);
        if (cmd == "noise")
            return cmd_noise(argc - 2, argv + 2);
        if (cmd == "bench")
            return cmd_bench(argc - 2, argv + 2);
        if (cmd == "--help" || cmd == "-h" || cmd == "help") {
            usage();
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fern-ft8: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
