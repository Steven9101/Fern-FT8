#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode probability against SNR on white Gaussian noise.

    scripts/threshold.py [--mode ft8|ft4] [--trials 100] [--from -23] [--to -17]
                         [--step 0.5] [--depth 3] [--generator fern|ft8sim|ft4code]
                         [--fading HZ,MS] [--jt9 3] [--tune SPEC]

For each SNR (dB in 2500 Hz), makes `trials` slots (15 s, or 7.5 s for FT4)
with one transmission and counts how often Fern-FT8 decodes that exact
message; with --jt9 N, jt9 -8 -d N (jt9 -5 for FT4) decodes the same files.
`fern` makes the slots with `fern-ft8 encode` at random frequencies
(300..2700 Hz), start times (DT -0.5..1.0 s, for FT4 -0.5..0.8 s) and
messages; `ft8sim` uses WSJT-X's simulator ("K1ABC W9XYZ EN37", 1500 Hz,
DT 0), an independent transmitter. `ft4code` takes the tones of random
messages from WSJT-X's ft4code and sends them with `fern-ft8 encode
--tones`, at random frequencies and start times as for `fern`. --fading
sends them over two fading paths (`fern-ft8 encode --fading`). Prints the
table and the SNR of 50 % decode probability, interpolated linearly.
FERN_FT8 in the environment names another build of fern-ft8 to use.
"""
import argparse
import glob
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FERN = os.environ.get("FERN_FT8", os.path.join(ROOT, "build", "fern-ft8"))
CALLS = ["K1ABC", "W9XYZ", "DL1ABC", "JA1XYZ", "G4ABC", "VK2ABC", "PY2XYZ", "EA3ABC", "OH2XYZ", "N0AB", "SP9LKP"]
GRIDS = ["FN42", "EN37", "JO31", "IO91", "QF56", "GG66", "KP20", "JN58"]


# A decode as jt9 and fern-ft8 print it: ~ marks FT8, + FT4.
DECODE = re.compile(r"^\d{6}\s+-?\d+\s+-?[\d.]+\s+\d+\s+[~+]\s+(.*)$")


def decoded(lines, msg):
    want = " ".join(msg.split())
    for ln in lines:
        m = DECODE.match(ln)
        if m and " ".join(m.group(1).split()).startswith(want):
            return True
    return False


def trial(args):
    snr, i, depth, generator, jt9, seed, tune, mode, fading = args
    rng = random.Random(seed * 100003 + i * 7919 + int(snr * 100))
    with tempfile.TemporaryDirectory() as td:
        if generator == "ft8sim":
            msg = "K1ABC W9XYZ EN37"
            subprocess.run(["ft8sim", msg, "1500", "0.0", "0", "0", "1", "%.1f" % snr], cwd=td, capture_output=True,
                           check=True)
            wav = glob.glob(os.path.join(td, "*.wav"))[0]
        else:
            a, b = rng.sample(CALLS, 2)
            msg = "%s %s %s" % (a, b, rng.choice(GRIDS + ["-12", "R-07", "RR73", "73"]))
            wav = os.path.join(td, "t.wav")
            what = [msg]
            if generator == "ft4code":
                tones = subprocess.run(["ft4code", msg], capture_output=True, text=True, check=True).stdout
                what = ["--tones", tones.strip().splitlines()[-1]]
            subprocess.run([FERN, "encode"] + what + ["--mode", mode, "--freq", "%.1f" % rng.uniform(300, 2700), "--dt",
                            "%.2f" % rng.uniform(-0.5, 0.8 if mode == "ft4" else 1.0), "--snr", "%.2f" % snr,
                            "--seed", str(rng.randrange(1 << 30)), "-o", wav] +
                           (["--fading", fading] if fading else []), check=True)
        cmd = [FERN, "decode", wav, "--mode", mode, "--depth", str(depth)] + (["--tune", tune] if tune else [])
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        ok = decoded(out.splitlines(), msg)
        ok_jt9 = None
        if jt9:
            flags = ["-5", "-p", "7.5"] if mode == "ft4" else ["-8", "-p", "15"]
            out = subprocess.run(["jt9"] + flags + ["-d", str(jt9), "-a", td, "-t", td, wav], cwd=td,
                                 capture_output=True, text=True).stdout
            ok_jt9 = decoded(out.splitlines(), msg)
    return snr, ok, ok_jt9


def threshold(points):
    pts = sorted(points)
    for (s0, p0), (s1, p1) in zip(pts, pts[1:]):
        if p0 < 0.5 <= p1:
            return s0 + (0.5 - p0) * (s1 - s0) / (p1 - p0)
    return float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["ft8", "ft4"], default="ft8")
    ap.add_argument("--trials", type=int, default=100)
    ap.add_argument("--from", dest="lo", type=float, default=-23.0)
    ap.add_argument("--to", dest="hi", type=float, default=-17.0)
    ap.add_argument("--step", type=float, default=0.5)
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--generator", choices=["fern", "ft8sim", "ft4code"], default="fern")
    ap.add_argument("--fading", default="", help="Doppler spread in Hz and delay in ms, as HZ,MS")
    ap.add_argument("--jt9", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--tune", default="", help="decoder parameters, as fern-ft8 --tune takes them")
    a = ap.parse_args()
    if a.generator != "fern" and not shutil.which(a.generator):
        sys.exit(a.generator + " not found")
    if (a.generator == "ft8sim" and a.mode != "ft8") or (a.generator == "ft4code" and a.mode != "ft4"):
        sys.exit("--generator %s makes no %s signals" % (a.generator, a.mode.upper()))
    snrs = []
    s = a.lo
    while s <= a.hi + 1e-9:
        snrs.append(round(s, 2))
        s += a.step
    if a.fading and a.generator == "ft8sim":
        sys.exit("--fading needs --generator fern or ft4code")
    jobs = [(snr, i, a.depth, a.generator, a.jt9, a.seed, a.tune, a.mode, a.fading)
            for snr in snrs for i in range(a.trials)]
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(trial, jobs, chunksize=4))
    print("%s, generator %s%s, %d trials per SNR, Fern-FT8 depth %d%s" % (
        a.mode.upper(), a.generator, (", fading " + a.fading) if a.fading else "", a.trials, a.depth,
        (", jt9 depth %d" % a.jt9) if a.jt9 else ""))
    fern_pts, jt9_pts = [], []
    for snr in snrs:
        r = [x for x in results if x[0] == snr]
        pf = sum(x[1] for x in r) / len(r)
        fern_pts.append((snr, pf))
        line = "SNR %6.1f dB  fern %.2f" % (snr, pf)
        if a.jt9:
            pj = sum(x[2] for x in r) / len(r)
            jt9_pts.append((snr, pj))
            line += "  jt9 %.2f" % pj
        print(line, flush=True)
    print("50%% threshold: fern %.2f dB" % threshold(fern_pts) +
          ("  jt9 %.2f dB" % threshold(jt9_pts) if a.jt9 else ""))


if __name__ == "__main__":
    main()
