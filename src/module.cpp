// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "module.h"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fern_ft8.h"
#include "json.h"

namespace fern::ft8 {

namespace {

constexpr int kCommands = 0;
constexpr int kEvents = 3;
constexpr int kFrames = 4;
constexpr size_t kHeaderBytes = 32;
constexpr uint32_t kMaxFrameSamples = 65536;
constexpr size_t kMaxChannels = 32;
// FernSDR allows this many decodes per channel and slot.
constexpr size_t kMaxDecodesPerSlot = 200;
// How far a channel's worker may fall behind before frames are dropped:
// long enough to ride out a busy slot on a slow machine, short enough that
// what is dropped is a slot rather than the memory of the host.
constexpr double kMaxBacklogSeconds = 20.0;

bool write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n > 0) {
            data += n;
            size -= static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool read_all(int fd, void* out, size_t size) {
    size_t got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, static_cast<char*>(out) + got, size - got);
        if (n > 0) got += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

uint64_t little_endian(const unsigned char* p, int bytes) {
    uint64_t value = 0;
    for (int i = bytes - 1; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

// One JSON object a line on fd 3, from any thread.
class Events {
public:
    void send(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!write_all(kEvents, (line + "\n").data(), line.size() + 1)) broken_.store(true);
    }
    bool broken() const { return broken_.load(); }

private:
    std::mutex mutex_;
    std::atomic<bool> broken_{false};
};

void log_line(const std::string& text) {
    const std::string line = "fern-ft8: " + text + "\n";
    (void)!::write(2, line.data(), line.size());
}

std::string fixed(double value, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, value);
    return buf;
}

struct Frame {
    uint32_t flags = 0;
    uint64_t index = 0;
    int64_t utc_us = 0;
    std::vector<std::complex<float>> samples;
};

// One channel and the thread that decodes it, so that a busy slot never
// holds up reading the frames of the others.
class Worker {
public:
    Worker(std::string id, const ChannelConfig& config, CallsignHashTable* hashes, Events* events)
        : id_(std::move(id)), channel_(config, hashes), events_(events),
          max_backlog_(static_cast<size_t>(config.rate * kMaxBacklogSeconds)) {
        thread_ = std::thread([this] { run(); });
    }

    ~Worker() { stop(); }

    // Queues a frame, or drops it when the decoder is too far behind; the
    // next frame kept then says samples were lost.
    void enqueue(Frame frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (backlog_ + frame.samples.size() > max_backlog_) {
                lost_ = true;
                return;
            }
            if (lost_) {
                frame.flags |= kFrameSamplesLost;
                lost_ = false;
            }
            backlog_ += frame.samples.size();
            queue_.push_back(std::move(frame));
        }
        wake_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // "slots", "decodes", "late" and "cpu_ms" for the stats event.
    std::string stats() const {
        return "{\"id\":" + json_quote(id_) + ",\"slots\":" + std::to_string(slots_.load()) +
               ",\"decodes\":" + std::to_string(decodes_.load()) + ",\"late\":" + std::to_string(late_.load()) +
               ",\"cpu_ms\":" + std::to_string(cpu_ms_.load()) + "}";
    }

private:
    void run() {
        while (true) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                frame = std::move(queue_.front());
                queue_.pop_front();
                backlog_ -= frame.samples.size();
            }
            channel_.push(frame.samples.data(), frame.samples.size(), frame.index, frame.utc_us, frame.flags);
            for (const SlotResult& result : channel_.decode_ready()) report(result);
        }
    }

    void report(const SlotResult& result) {
        slots_++;
        if (result.missing > 0.0) late_++;
        cpu_ms_ += static_cast<uint64_t>(std::llround(result.cpu_seconds * 1000.0));
        size_t sent = 0;
        for (const Decode& d : result.decodes) {
            if (sent == kMaxDecodesPerSlot) break;
            std::string line = "{\"type\":\"decode\",\"channel\":" + json_quote(id_) +
                               ",\"time\":" + std::to_string(d.slot_start_ms) + ",\"freq\":" + fixed(d.freq_hz, 1) +
                               ",\"snr\":" + std::to_string(d.snr_db) + ",\"dt\":" + fixed(d.dt, 2) +
                               ",\"message\":" + json_quote(d.message.text);
            const MessageFields& f = d.message.fields;
            if (!f.de_call.empty()) line += ",\"call\":" + json_quote(f.de_call);
            if (!f.grid.empty()) line += ",\"grid\":" + json_quote(f.grid);
            if (!f.report.empty()) line += ",\"report\":" + json_quote(f.report);
            line += ",\"quality\":" + json_quote(d.quality()) + "}";
            events_->send(line);
            sent++;
        }
        decodes_ += sent;
    }

    std::string id_;
    Channel channel_;
    Events* events_;
    size_t max_backlog_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Frame> queue_;
    size_t backlog_ = 0;
    bool lost_ = false;
    bool stopping_ = false;
    std::atomic<uint64_t> slots_{0}, decodes_{0}, late_{0}, cpu_ms_{0};
    std::thread thread_;
};

void fatal(Events& events, const std::string& code, const std::string& message) {
    log_line(message);
    events.send("{\"type\":\"error\",\"code\":" + json_quote(code) + ",\"message\":" + json_quote(message) +
                ",\"fatal\":true}");
}

// Reads fd 0 a line at a time; false at end of file.
class Lines {
public:
    // A whole line when one has arrived; `more` false once fd 0 has ended.
    bool next(std::string& line) {
        const size_t end = buffer_.find('\n');
        if (end == std::string::npos) return false;
        line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        return true;
    }
    // Reads what is there; false at end of file or on an error.
    bool fill() {
        char chunk[4096];
        while (true) {
            const ssize_t n = ::read(kCommands, chunk, sizeof chunk);
            if (n > 0) {
                buffer_.append(chunk, static_cast<size_t>(n));
                // A command line is short; one that is not is not a command.
                return buffer_.size() <= 1 << 20;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
    }

private:
    std::string buffer_;
};

}  // namespace

int describe_module() {
    std::printf("{\"api\":2,\"id\":\"ft8\",\"name\":\"Fern-FT8\",\"version\":\"" FERN_FT8_VERSION
                "\",\"kind\":\"decoder\",\"modes\":[\"ft8\",\"ft4\"],\"settings\":[{\"key\":\"depth\","
                "\"type\":\"number\",\"label\":\"Depth\",\"min\":1,\"max\":3,\"default\":2,"
                "\"help\":\"How hard each slot is searched: 1 is quickest, 3 finds a few more weak signals "
                "for about a third more CPU in FT8 and twice as much in FT4\"}]}\n");
    return 0;
}

int run_module(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "--fernsdr-module" || std::string(argv[2]) != "2") {
        std::fprintf(stderr, "usage: fern-ft8 --fernsdr-module 2 (started by FernSDR)\n");
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    Events events;
    events.send("{\"type\":\"hello\",\"api\":2,\"id\":\"ft8\",\"version\":\"" FERN_FT8_VERSION
                "\",\"kind\":\"decoder\",\"modes\":[\"ft8\",\"ft4\"]}");

    // The open command, before anything else.
    Lines lines;
    std::string line;
    Json open;
    while (true) {
        if (lines.next(line)) {
            if (!Json::parse(line, open) || !open.is_object()) {
                fatal(events, "invalid", "a command is not a JSON object");
                return 3;
            }
            if (open["type"].string() == "stop") return 0;
            if (open["type"].string() == "open") break;
            continue;
        }
        if (!lines.fill()) return 0;
    }

    const Json& list = open["channels"];
    if (!list.is_array() || list.elements().empty() || list.elements().size() > kMaxChannels) {
        fatal(events, "invalid", "open names no channels, or more than 32");
        return 3;
    }
    int depth = 2;
    if (open["settings"].has("depth")) {
        const double asked = open["settings"]["depth"].number(-1);
        if (asked != 1 && asked != 2 && asked != 3) {
            fatal(events, "invalid", "depth is 1, 2 or 3");
            return 3;
        }
        depth = static_cast<int>(asked);
    }
    CallsignHashTable hashes;
    std::vector<std::unique_ptr<Worker>> workers;
    for (const Json& c : list.elements()) {
        const std::string id = c["id"].string();
        ChannelConfig config;
        config.rate = c["rate"].number(0);
        config.offset_hz = c["offset"].number(2000);
        config.width_hz = c["width"].number(4000);
        config.depth = depth;
        // A channel without a mode is FT8, as this module's id names it.
        const std::string mode = c.has("mode") ? c["mode"].string() : "ft8";
        config.mode = mode == "ft4" ? Mode::Ft4 : Mode::Ft8;
        if (id.empty() || id.size() > 64 || c["format"].string() != "cf32" || (mode != "ft8" && mode != "ft4") ||
            !(config.rate > 0 && config.rate < 1e6) || !std::isfinite(config.offset_hz) ||
            !(config.width_hz > 0 && config.width_hz <= 48000)) {
            fatal(events, "invalid",
                  "channel '" + id + "' is not an FT8 or FT4 channel of cf32 samples this decoder can take");
            return 3;
        }
        try {
            workers.push_back(std::make_unique<Worker>(id, config, &hashes, &events));
        } catch (const std::exception& e) {
            fatal(events, "invalid", "channel '" + id + "': " + e.what());
            return 3;
        }
    }
    events.send("{\"type\":\"ready\"}");
    log_line("decoding " + std::to_string(workers.size()) + " channel(s) at depth " + std::to_string(depth));

    auto next_stats = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int status = 0;
    bool running = true;
    while (running && !events.broken()) {
        pollfd fds[2] = {{kCommands, POLLIN, 0}, {kFrames, POLLIN, 0}};
        const int ready = ::poll(fds, 2, 250);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!lines.fill()) break;
            while (lines.next(line)) {
                Json command;
                if (Json::parse(line, command) && command["type"].string() == "stop") running = false;
            }
        }
        if (running && ready > 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char header[kHeaderBytes];
            if (!read_all(kFrames, header, sizeof header)) break;
            const uint32_t channel = static_cast<uint32_t>(little_endian(header + 4, 2));
            const uint32_t count = static_cast<uint32_t>(little_endian(header + 8, 4));
            if (std::memcmp(header, "FDR1", 4) != 0 || channel >= workers.size() || count > kMaxFrameSamples) {
                fatal(events, "invalid", "a sample frame has a bad header");
                status = 3;
                break;
            }
            Frame frame;
            frame.flags = static_cast<uint32_t>(little_endian(header + 6, 2));
            frame.index = little_endian(header + 16, 8);
            frame.utc_us = static_cast<int64_t>(little_endian(header + 24, 8));
            frame.samples.resize(count);
            // cf32 as little-endian pairs, which is also how std::complex<float>
            // is laid out on every machine this runs on.
            if (!read_all(kFrames, frame.samples.data(), count * sizeof(std::complex<float>))) break;
            workers[channel]->enqueue(std::move(frame));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_stats) {
            std::string stats = "{\"type\":\"stats\",\"channels\":[";
            for (size_t i = 0; i < workers.size(); i++) stats += (i ? "," : "") + workers[i]->stats();
            events.send(stats + "]}");
            next_stats = now + std::chrono::seconds(1);
        }
    }
    for (auto& worker : workers) worker->stop();
    return status;
}

}  // namespace fern::ft8
