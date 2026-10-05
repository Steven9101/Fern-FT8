#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record reference vectors from WSJT-X's ft8code for tests/vectors/ft8code.txt,
or with --ft4 from its ft4code for tests/vectors/ft4code.txt.

Each line: message|text ft8code decodes|i3.n3|77 payload bits|14 CRC bits|
83 parity bits|79 tones. With --ft4: message|text ft4code decodes|i3.n3|
77 payload bits|the 77 bits after scrambling|14 CRC bits|83 parity bits|
105 tones (with the two ramp symbols). Only the program's output is
recorded; the tests compare Fern-FT8's own encoder and decoder with it.
Needs ft8code and ft4code on PATH (the Debian/Ubuntu wsjtx package); the
vectors in the repository came from WSJT-X 2.7.0-rc3.
"""
import subprocess
import sys

EXTRA = [
    "CQ 123 K1ABC FN42", "CQ DX K1ABC FN42", "CQ A K1ABC FN42", "CQ ZZZZ K1ABC FN42", "QRZ K1ABC FN42",
    "DE K1ABC FN42", "K1ABC W9XYZ", "CQ K1ABC", "K1ABC W9XYZ -50", "K1ABC W9XYZ +49", "K1ABC W9XYZ -31",
    "K1ABC W9XYZ R+05", "K1ABC W9XYZ R EN37", "W1AW K1ABC RR73", "2E0ABC 9A1AA AA00", "KH1ABC K1AB RR99",
    "12AB", "ABC", "7FFFFFFFFFFFFFFFFF", "W9XYZ K1ABC 17A EMA", "K1ABC W9XYZ 32F DX", "K1ABC W9XYZ R 1B GH",
    "K1ABC W9XYZ 16F NB", "TU; K1ABC W9XYZ 599 X99", "K1ABC W9XYZ 529 7999", "K1ABC W9XYZ 579 PEI",
    "HELLO WORLD", "A+B-C./?", "K1ABC RR73; W9XYZ <KH1/KH7Z> +32", "K1ABC RR73; W9XYZ <KH1/KH7Z> -30",
    "<W9XYZ> YW18FIFA RR73", "CQ KH1/KH7Z", "W9XYZ/P K1ABC/P R JO22", "CQ RU K1ABC FN42",
]


def tests_messages(program, flag):
    out = subprocess.run([program, flag], capture_output=True, text=True).stdout.splitlines()
    msgs = []
    for ln in out:
        if len(ln) > 4 and ln[:3].strip().rstrip(".").isdigit():
            msgs.append(ln[4:41].strip())
    return msgs


def vector(msg):
    out = subprocess.run(["ft8code", msg], capture_output=True, text=True).stdout.splitlines()
    head = out[2]
    decoded = head[42:79].strip()
    itype = head[80:].split()
    itype = [t for t in itype if t != "*"][0]
    bits = crc = parity = None
    tones = ""
    for i, ln in enumerate(out):
        if ln.startswith("Source-encoded"):
            bits = out[i + 1].strip()
        if ln.startswith("14-bit CRC"):
            crc = out[i + 1].strip()
        if ln.startswith("83 Parity"):
            parity = out[i + 1].strip()
        if ln.startswith("Channel symbols"):
            tones = "".join(out[i + 2].split())
    if itype.endswith("."):
        itype += "0"
    return "|".join([msg, decoded, itype, bits, crc, parity, tones])


def vector_ft4(msg):
    out = subprocess.run(["ft4code", msg], capture_output=True, text=True).stdout.splitlines()
    head = out[2]
    decoded = head[42:79].strip()
    itype = [t for t in head[80:].split() if t != "*"][0]
    found = {}
    for i, ln in enumerate(out):
        for key in ("Source-encoded message before", "Source-encoded message after", "14-bit CRC", "83 Parity"):
            if ln.startswith(key):
                found[key] = out[i + 1].strip()
        if ln.startswith("Channel symbols"):
            found["tones"] = "".join(out[i + 2].split())
    if itype.endswith("."):
        itype += "0"
    return "|".join([msg, decoded, itype, found["Source-encoded message before"],
                     found["Source-encoded message after"], found["14-bit CRC"], found["83 Parity"], found["tones"]])


def main():
    if sys.argv[1:] == ["--ft4"]:
        lines = [vector_ft4(m) for m in tests_messages("ft4code", "-t") + EXTRA]
    else:
        lines = [vector(m) for m in tests_messages("ft8code", "-T") + EXTRA]
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
