// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "wav.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fern::ft8 {

namespace {

uint32_t le32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }

void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        s += char((v >> (8 * i)) & 0xff);
}
void put16(std::string& s, uint16_t v) {
    s += char(v & 0xff);
    s += char(v >> 8);
}

}  // namespace

Audio read_wav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error(path + " is not a WAV file");
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* pcm = nullptr;
    size_t pcm_bytes = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const uint32_t size = le32(&data[pos + 4]);
        const size_t body = pos + 8;
        if (size > data.size() - body)
            throw std::runtime_error(path + ": truncated chunk");
        if (std::memcmp(&data[pos], "fmt ", 4) == 0 && size >= 16) {
            format = le16(&data[body]);
            channels = le16(&data[body + 2]);
            rate = le32(&data[body + 4]);
            bits = le16(&data[body + 14]);
            if (format == 0xfffe && size >= 26)
                format = le16(&data[body + 24]);  // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (std::memcmp(&data[pos], "data", 4) == 0) {
            pcm = &data[body];
            pcm_bytes = size;
        }
        pos = body + size + (size & 1);
    }
    if (!pcm || channels == 0 || rate == 0)
        throw std::runtime_error(path + ": no format or data chunk");
    Audio a;
    a.rate = rate;
    if (format == 1 && bits == 16) {
        const size_t frames = pcm_bytes / (2u * channels);
        a.samples.resize(frames);
        for (size_t i = 0; i < frames; ++i)
            a.samples[i] = float(int16_t(le16(pcm + 2 * channels * i))) / 32768.0f;
    } else if (format == 3 && bits == 32) {
        const size_t frames = pcm_bytes / (4u * channels);
        a.samples.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            const uint32_t u = le32(pcm + 4 * channels * i);
            float f;
            std::memcpy(&f, &u, 4);
            a.samples[i] = std::isfinite(f) ? f : 0.0f;
        }
    } else {
        throw std::runtime_error(path + ": only 16-bit PCM and 32-bit float WAV files are read");
    }
    return a;
}

void write_wav(const std::string& path, const Audio& audio) {
    const uint32_t rate = uint32_t(std::lround(audio.rate));
    const uint32_t bytes = uint32_t(audio.samples.size() * 2);
    std::string s = "RIFF";
    put32(s, 36 + bytes);
    s += "WAVEfmt ";
    put32(s, 16);
    put16(s, 1);
    put16(s, 1);
    put32(s, rate);
    put32(s, rate * 2);
    put16(s, 2);
    put16(s, 16);
    s += "data";
    put32(s, bytes);
    for (float x : audio.samples) {
        const float c = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
        put16(s, uint16_t(int16_t(std::lround(c * 32767.0f))));
    }
    std::ofstream out(path, std::ios::binary);
    out.write(s.data(), std::streamsize(s.size()));
    if (!out)
        throw std::runtime_error("cannot write " + path);
}

}  // namespace fern::ft8
