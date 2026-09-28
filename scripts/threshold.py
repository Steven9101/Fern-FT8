#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode probability against SNR on white Gaussian noise.

    scripts/threshold.py [--trials 100] [--from -23] [--to -17] [--step 0.5]
                         [--depth 3] [--generator fern|ft8sim] [--jt9 3]

For each SNR (dB in 2500 Hz), makes `trials` 15 s slots with one
transmission and counts how often Fern-FT8 decodes that exact message; with
--jt9 N, jt9 -8 -d N decodes the same files. `fern` makes the slots with
`fern-ft8 encode` at random frequencies (300..2700 Hz), start times (DT
-0.5..1.0 s) and messages; `ft8sim` uses WSJT-X's simulator ("K1ABC W9XYZ
EN37", 1500 Hz, DT 0), an independent transmitter. Prints the table and
the SNR of 50 % decode probability, interpolated linearly.
"""
import argparse
import glob
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FERN = os.path.join(ROOT, "build", "fern-ft8")
CALLS = ["K1ABC", "W9XYZ", "DL1ABC", "JA1XYZ", "G4ABC", "VK2ABC", "PY2XYZ", "EA3ABC", "OH2XYZ", "N0AB", "SP9LKP"]
GRIDS = ["FN42", "EN37", "JO31", "IO91", "QF56", "GG66", "KP20", "JN58"]


def decoded(lines, msg):
    want = " ".join(msg.split())
    return any("~" in ln and " ".join(ln.split("~", 1)[1].split()).startswith(want) for ln in lines)


def trial(args):
    snr, i, depth, generator, jt9, seed = args
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
            subprocess.run([FERN, "encode", msg, "--freq", "%.1f" % rng.uniform(300, 2700), "--dt",
                            "%.2f" % rng.uniform(-0.5, 1.0), "--snr", "%.2f" % snr, "--seed", str(rng.randrange(1 << 30)),
                            "-o", wav], check=True)
        out = subprocess.run([FERN, "decode", wav, "--depth", str(depth)], capture_output=True, text=True).stdout
        ok = decoded(out.splitlines(), msg)
        ok_jt9 = None
        if jt9:
            out = subprocess.run(["jt9", "-8", "-d", str(jt9), "-p", "15", "-a", td, "-t", td, wav], cwd=td,
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
    ap.add_argument("--trials", type=int, default=100)
    ap.add_argument("--from", dest="lo", type=float, default=-23.0)
    ap.add_argument("--to", dest="hi", type=float, default=-17.0)
    ap.add_argument("--step", type=float, default=0.5)
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--generator", choices=["fern", "ft8sim"], default="fern")
    ap.add_argument("--jt9", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    if a.generator == "ft8sim" and not shutil.which("ft8sim"):
        sys.exit("ft8sim not found")
    snrs = []
    s = a.lo
    while s <= a.hi + 1e-9:
        snrs.append(round(s, 2))
        s += a.step
    jobs = [(snr, i, a.depth, a.generator, a.jt9, a.seed) for snr in snrs for i in range(a.trials)]
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(trial, jobs, chunksize=4))
    print("generator %s, %d trials per SNR, Fern-FT8 depth %d%s" % (
        a.generator, a.trials, a.depth, (", jt9 depth %d" % a.jt9) if a.jt9 else ""))
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
