// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reference vectors recorded from WSJT-X's ft8code and ft4code
// (tools/make_vectors.py).
#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.h"

namespace test {

struct Ft8codeVector {
    std::string message;  // as given to ft8code
    std::string decoded;  // as ft8code prints it back
    int i3 = 0, n3 = 0;
    std::string bits, crc, parity, tones;

    fern::ft8::Payload payload() const {
        fern::ft8::Payload p{};
        for (int i = 0; i < 77; ++i)
            fern::ft8::set_payload_bit(p, i, bits[size_t(i)] == '1');
        return p;
    }
};

inline std::vector<Ft8codeVector> ft8code_vectors() {
    std::vector<Ft8codeVector> out;
    std::ifstream in("tests/vectors/ft8code.txt");
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '|'))
            f.push_back(item);
        if (f.size() != 7)
            continue;
        Ft8codeVector v;
        v.message = f[0];
        v.decoded = f[1];
        const size_t dot = f[2].find('.');
        v.i3 = std::stoi(f[2].substr(0, dot));
        v.n3 = std::stoi(f[2].substr(dot + 1));
        v.bits = f[3];
        v.crc = f[4];
        v.parity = f[5];
        v.tones = f[6];
        out.push_back(v);
    }
    return out;
}

struct Ft4codeVector {
    std::string message;  // as given to ft4code
    std::string decoded;  // as ft4code prints it back
    int i3 = 0, n3 = 0;
    // The payload before and after scrambling, the CRC of the scrambled
    // payload, the parity bits and the 105 tones with both ramp symbols.
    std::string bits, scrambled, crc, parity, tones;

    static fern::ft8::Payload payload_from(const std::string& bits) {
        fern::ft8::Payload p{};
        for (int i = 0; i < 77; ++i)
            fern::ft8::set_payload_bit(p, i, bits[size_t(i)] == '1');
        return p;
    }
    fern::ft8::Payload payload() const { return payload_from(bits); }
};

inline std::vector<Ft4codeVector> ft4code_vectors() {
    std::vector<Ft4codeVector> out;
    std::ifstream in("tests/vectors/ft4code.txt");
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '|'))
            f.push_back(item);
        if (f.size() != 8)
            continue;
        Ft4codeVector v;
        v.message = f[0];
        v.decoded = f[1];
        const size_t dot = f[2].find('.');
        v.i3 = std::stoi(f[2].substr(0, dot));
        v.n3 = std::stoi(f[2].substr(dot + 1));
        v.bits = f[3];
        v.scrambled = f[4];
        v.crc = f[5];
        v.parity = f[6];
        v.tones = f[7];
        out.push_back(v);
    }
    return out;
}

}  // namespace test
