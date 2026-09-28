// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reference vectors recorded from WSJT-X's ft8code (tools/make_vectors.py).
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

}  // namespace test
