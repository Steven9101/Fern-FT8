// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The decoder session as FernSDR runs it: build/fern-ft8 started with the
// commands on fd 0, events on fd 3 and frames on fd 4, fed a real recording
// as a channel, and made to answer what it must refuse.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <string>
#include <vector>

#include "json.h"
#include "test.h"
#include "wav.h"

using namespace fern::ft8;

TEST(json_reads_commands_and_refuses_what_is_not_json) {
    Json v;
    REQUIRE(Json::parse(R"({"type":"open","channels":[{"id":"a\"b","rate":7812.5,"ok":true}],"s":{"depth":2}})", v));
    CHECK(v["type"].string() == "open");
    CHECK(v["channels"].elements()[0]["id"].string() == "a\"b");
    CHECK(v["channels"].elements()[0]["rate"].number() == 7812.5);
    CHECK(v["channels"].elements()[0]["ok"].boolean());
    CHECK(v["s"]["depth"].number() == 2);
    CHECK(v["missing"]["deeper"].string().empty());
    CHECK(Json::parse(R"("café")", v) && v.string() == "caf\xc3\xa9");
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1}x", "\"\x01\"", "01x", "nul", "{\"a\" 1}"}) {
        CHECK(!Json::parse(bad, v));
    }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!Json::parse(deep, v));
    CHECK(json_quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
}

namespace {

// build/fern-ft8 --fernsdr-module 2 with its descriptors on pipes.
struct Session {
    pid_t pid = -1;
    int commands = -1;  // our end of its fd 0
    int events = -1;    // our end of its fd 3
    int frames = -1;    // our end of its fd 4
    std::string pending;

    Session() {
        int c[2], e[2], f[2];
        if (pipe(c) != 0 || pipe(e) != 0 || pipe(f) != 0) return;
        pid = fork();
        if (pid == 0) {
            const int null = open("/dev/null", O_RDWR);
            dup2(c[0], 0);
            dup2(null, 1);
            dup2(null, 2);
            dup2(e[1], 3);
            dup2(f[0], 4);
            for (int fd = 5; fd < 64; fd++) close(fd);
            execl("build/fern-ft8", "fern-ft8", "--fernsdr-module", "2", static_cast<char*>(nullptr));
            _exit(127);
        }
        close(c[0]);
        close(e[1]);
        close(f[0]);
        commands = c[1];
        events = e[0];
        frames = f[1];
        signal(SIGPIPE, SIG_IGN);
    }

    ~Session() {
        if (commands >= 0) close(commands);
        if (frames >= 0) close(frames);
        if (events >= 0) close(events);
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }

    void send(const std::string& line) {
        const std::string text = line + "\n";
        CHECK(write(commands, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }

    bool write_all(int fd, const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            const ssize_t n = write(fd, p, size);
            if (n <= 0) return false;
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }

    bool frame(uint16_t channel, uint16_t flags, uint64_t index, int64_t utc_us,
               const std::vector<std::complex<float>>& samples, const char* magic = "FDR1") {
        unsigned char header[32] = {};
        std::memcpy(header, magic, 4);
        const auto put = [&](int at, uint64_t value, int bytes) {
            for (int i = 0; i < bytes; i++) header[at + i] = static_cast<unsigned char>(value >> (8 * i));
        };
        put(4, channel, 2);
        put(6, flags, 2);
        put(8, samples.size(), 4);
        put(16, index, 8);
        put(24, static_cast<uint64_t>(utc_us), 8);
        return write_all(frames, header, sizeof header) &&
               write_all(frames, samples.data(), samples.size() * sizeof(samples[0]));
    }

    // The next event, or null after `ms` without one.
    Json next(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (true) {
            const size_t newline = pending.find('\n');
            if (newline != std::string::npos) {
                Json event;
                Json::parse(pending.substr(0, newline), event);
                pending.erase(0, newline + 1);
                return event;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0) return Json();
            pollfd p{events, POLLIN, 0};
            if (poll(&p, 1, static_cast<int>(left.count())) <= 0) return Json();
            char buf[4096];
            const ssize_t n = read(events, buf, sizeof buf);
            if (n <= 0) return Json();
            pending.append(buf, static_cast<size_t>(n));
        }
    }

    int wait_exit(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            usleep(10000);
        }
        return -2;
    }
};

const char* kOpen =
    R"({"type":"open","channels":[{"id":"20m-ft8-14074","band":"20m","mode":"ft8","dial":14074000,)"
    R"("rate":12000,"offset":2000,"width":4000,"format":"cf32"}],"settings":{"depth":2}})";

}  // namespace

TEST(module_decodes_a_recording_fed_as_frames_and_stops_when_told) {
    Session s;
    REQUIRE(s.pid > 0);
    const Json hello = s.next(5000);
    CHECK(hello["type"].string() == "hello");
    CHECK(hello["api"].number() == 2);
    CHECK(hello["kind"].string() == "decoder");
    s.send(kOpen);
    CHECK(s.next(15000)["type"].string() == "ready");

    // The recording starts on a slot; baseband 0 Hz is audio 2000 Hz, so the
    // real audio shifted down by 2000 Hz. Its mirror lands below -2000 Hz,
    // outside the channel.
    const Audio audio = read_wav("tests/data/191111_110130.wav");
    REQUIRE(audio.rate == 12000);
    std::vector<std::complex<float>> channel(audio.samples.size() + 3 * 12000, 0.0f);
    for (size_t i = 0; i < audio.samples.size(); i++) {
        const double phase = -2.0 * M_PI * 2000.0 * static_cast<double>(i) / 12000.0;
        channel[i] = std::polar(audio.samples[i], static_cast<float>(phase));
    }
    const int64_t slot_ms = 1790604000000;
    const size_t per_frame = 3000;
    for (size_t at = 0; at < channel.size(); at += per_frame) {
        const std::vector<std::complex<float>> part(channel.begin() + at,
                                                    channel.begin() + std::min(channel.size(), at + per_frame));
        REQUIRE(s.frame(0, at == 0 ? 2 : 0, at, slot_ms * 1000 + static_cast<int64_t>(at) * 1000000 / 12000, part));
    }
    int decodes = 0;
    bool stats = false;
    for (int i = 0; i < 200 && decodes < 5; i++) {
        const Json event = s.next(10000);
        if (event.kind() == Json::Kind::Null) break;
        if (event["type"].string() == "stats") stats = true;
        if (event["type"].string() != "decode") continue;
        decodes++;
        CHECK(event["channel"].string() == "20m-ft8-14074");
        CHECK(event["time"].number() == slot_ms);
        CHECK(event["freq"].number() > 0 && event["freq"].number() < 4000);
        CHECK(std::fabs(event["dt"].number()) < 2.5);
        CHECK(!event["message"].string().empty());
        const std::string q = event["quality"].string();
        CHECK(q == "bp" || q == "osd" || q == "low");
    }
    CHECK(decodes == 5);
    CHECK(stats || s.next(1500)["type"].string() == "stats");
    s.send(R"({"type":"stop"})");
    CHECK(s.wait_exit(5000) == 0);
}

TEST(module_refuses_a_bad_open_and_a_bad_frame) {
    {
        Session s;
        REQUIRE(s.pid > 0);
        s.next(5000);
        s.send(R"({"type":"open","channels":[{"id":"x","rate":12000,"format":"cs16"}]})");
        const Json error = s.next(5000);
        CHECK(error["type"].string() == "error");
        CHECK(error["fatal"].boolean());
        CHECK(s.wait_exit(5000) == 3);
    }
    {
        Session s;
        REQUIRE(s.pid > 0);
        s.next(5000);
        s.send(R"({"type":"open","channels":[{"id":"x","rate":12000,"format":"cf32"}],"settings":{"depth":7}})");
        CHECK(s.next(5000)["type"].string() == "error");
        CHECK(s.wait_exit(5000) == 3);
    }
    {
        Session s;
        REQUIRE(s.pid > 0);
        s.next(5000);
        s.send(kOpen);
        CHECK(s.next(15000)["type"].string() == "ready");
        s.frame(0, 0, 0, 0, std::vector<std::complex<float>>(100), "XXXX");
        Json event;
        do event = s.next(5000);
        while (event["type"].string() == "stats");
        CHECK(event["type"].string() == "error");
        CHECK(s.wait_exit(5000) == 3);
    }
    {
        // Closing its input is a stop too.
        Session s;
        REQUIRE(s.pid > 0);
        s.next(5000);
        s.send(kOpen);
        CHECK(s.next(15000)["type"].string() == "ready");
        close(s.frames);
        s.frames = -1;
        CHECK(s.wait_exit(5000) == 0);
    }
}
