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
#include "resampler.h"
#include "wav.h"

using namespace fern::ft8;

namespace {

void usage() {
    std::fprintf(stderr,
                 "Fern-FT8 %s, an FT8 decoder (receive only).\n"
                 "\n"
                 "usage:\n"
                 "  fern-ft8 decode FILE.wav... [--depth 1|2|3] [--rate-test R] [--verbose]\n"
                 "      Decode 15 s slots of real audio, printed as jt9 prints them:\n"
                 "      HHMMSS SNR DT FREQ ~ MESSAGE. The audio goes through the same\n"
                 "      complex baseband path as live channels; --rate-test resamples\n"
                 "      it to R complex samples a second first. The slot time comes\n"
                 "      from a YYMMDD_HHMMSS file name, else 000000.\n"
                 "  fern-ft8 encode \"MESSAGE\" [--freq HZ] [--snr DB] [--dt S] [--rate HZ]\n"
                 "                  [--seed N] -o OUT.wav\n"
                 "      Write a 15 s slot with one transmission of MESSAGE (default\n"
                 "      1500 Hz, DT 0, 12000 Hz), with white noise at SNR DB in 2500 Hz\n"
                 "      when --snr is given.\n"
                 "  fern-ft8 bench [--depth N] [FILE.wav...]\n"
                 "      CPU time per busy and per quiet slot on one thread.\n",
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

struct Options {
    int depth = 2;
    double rate_test = 0;
    bool verbose = false;
};

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
    cfg.rate = rate;
    cfg.offset_hz = offset;
    cfg.width_hz = 4400;
    cfg.min_freq_hz = 200;
    cfg.max_freq_hz = 4000;
    cfg.depth = o.depth;
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
    std::printf("%06d %3d %4.1f %4d ~  %s", hms, d.snr_db, std::fabs(d.dt) < 0.05 ? 0.0 : d.dt, int(std::lround(d.freq_hz)), d.message.text.c_str());
    if (verbose)
        std::printf("   [%d.%d %s pass %d it %d hard %d costas %d]", d.message.i3, d.message.n3, d.quality(), d.pass,
                    d.ldpc_iterations, d.hard_errors, d.costas_hits);
    std::printf("\n");
}

int cmd_decode(int argc, char** argv) {
    Options o;
    std::vector<std::string> files;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--depth" && i + 1 < argc)
            o.depth = std::atoi(argv[++i]);
        else if (a == "--rate-test" && i + 1 < argc)
            o.rate_test = std::atof(argv[++i]);
        else if (a == "--verbose")
            o.verbose = true;
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
        for (const SlotResult& r : results)
            for (const Decode& d : r.decodes)
                print_decode(d, results.size() == 1 ? hhmmss : -1, o.verbose);
        if (o.verbose)
            std::fprintf(stderr, "%s: %.3f s CPU\n", f.c_str(), cpu);
    }
    return 0;
}

int cmd_encode(int argc, char** argv) {
    std::string text, out;
    double freq = 1500, dt = 0, rate = 12000;
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
        else if (text.empty() && !a.empty() && a[0] != '-')
            text = a;
        else {
            usage();
            return 2;
        }
    }
    if (text.empty() || out.empty() || rate < 2 * (freq + 60) || freq < 0) {
        usage();
        return 2;
    }
    const auto payload = pack_message(text);
    if (!payload) {
        std::fprintf(stderr, "fern-ft8: no FT8 message type carries \"%s\"\n", text.c_str());
        return 1;
    }
    const Tones tones = tones_of(encode_codeword(*payload));
    Audio audio;
    audio.rate = rate;
    const size_t n = size_t(std::llround(15.0 * rate));
    std::vector<std::complex<float>> wave(n);
    // Real noise of standard deviation sigma has one-sided density
    // 2 sigma^2 / rate; a sine of amplitude A has power A^2 / 2, so
    // SNR = A^2 rate / (4 * 2500 * sigma^2).
    const double sigma = 0.03;
    double amp = 0.3;
    if (!std::isnan(snr))
        amp = std::sqrt(std::pow(10.0, snr / 10.0) * 4.0 * 2500.0 * sigma * sigma / rate);
    add_ft8_waveform(tones, rate, freq, kStartSeconds + dt, 1.0f, wave.data(), n);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    audio.samples.resize(n);
    for (size_t i = 0; i < n; ++i)
        audio.samples[i] = float(amp * wave[i].real() + (std::isnan(snr) ? 0.0 : g(rng)));
    write_wav(out, audio);
    return 0;
}

// A busy slot: 30 transmissions across 200 to 3000 Hz at SNRs from -20 to
// +10 dB and DT from -0.5 to 1.5 s, in white noise.
Audio synthetic_busy_slot(unsigned seed, int signals) {
    std::mt19937 rng(seed);
    const double rate = 12000, sigma = 0.02;
    const size_t n = size_t(15 * rate);
    std::vector<std::complex<float>> sum(n);
    const char* first[] = {"CQ", "K1ABC", "W9XYZ", "DL1ABC", "JA1XYZ", "G4ABC", "VK2ABC", "PY2XYZ"};
    const char* seconds[] = {"EA3ABC", "OH2XYZ", "N0AB", "SP9LKP", "HB9CUZ", "R1CBP", "IK4LZH", "ON6UF"};
    const char* tails[] = {"FN42", "-12", "R-07", "RR73", "73", "JO31", "+03", "RRR"};
    for (int s = 0; s < signals; ++s) {
        const std::string text = std::string(first[rng() % 8]) + " " + seconds[rng() % 8] + std::to_string(s % 10) +
                                  (s % 2 ? "A" : "") + " " + tails[rng() % 8];
        auto p = pack_message(text);
        if (!p)
            continue;
        const double f = 200 + 2800.0 * (rng() % 1000) / 1000.0;
        const double dt = -0.5 + 2.0 * (rng() % 1000) / 1000.0;
        const double snr = -20 + 30.0 * (rng() % 1000) / 1000.0;
        const double amp = std::sqrt(std::pow(10.0, snr / 10.0) * 4.0 * 2500.0 * sigma * sigma / rate);
        std::vector<std::complex<float>> w(n);
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
#if defined(__x86_64__) || defined(__i386__)
    s = "x86-64";
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2"))
        s += ", AVX2 available";
    s += ", scalar code (compiled for the baseline ISA)";
#elif defined(__aarch64__)
    s = "aarch64, scalar code";
#else
    s = "scalar code";
#endif
    return s;
}

int cmd_bench(int argc, char** argv) {
    int depth = 0;
    std::vector<std::string> files;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--depth" && i + 1 < argc)
            depth = std::atoi(argv[++i]);
        else
            files.push_back(a);
    }
    std::printf("Fern-FT8 %s bench, one thread, %s\n", FERN_FT8_VERSION, isa_description().c_str());
    std::vector<int> depths = depth ? std::vector<int>{depth} : std::vector<int>{1, 2, 3};
    struct Case {
        std::string name;
        Audio audio;
    };
    std::vector<Case> cases;
    if (files.empty()) {
        cases.push_back({"synthetic busy slot (30 signals)", synthetic_busy_slot(11, 30)});
        Audio quiet = synthetic_busy_slot(12, 0);
        cases.push_back({"quiet slot (noise only)", quiet});
    } else {
        for (const auto& f : files)
            cases.push_back({f, read_wav(f)});
    }
    for (int d : depths) {
        for (const Case& c : cases) {
            Options o;
            o.depth = d;
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
            std::printf("depth %d  %-40s %6.3f s CPU  %3zu decodes\n", d, c.name.c_str(), best, decodes);
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
    try {
        if (cmd == "decode")
            return cmd_decode(argc - 2, argv + 2);
        if (cmd == "encode")
            return cmd_encode(argc - 2, argv + 2);
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
