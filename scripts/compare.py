#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Fern-FT8's decodes of real recordings with WSJT-X's jt9.

    scripts/compare.py WAVDIR [--depth 2] [--jt9-depth 2] [--rate-test R] [--json OUT]

Decodes every 12 kHz WAV under WAVDIR (for example ft8_lib's test/wav) with
build/fern-ft8 and with `jt9 -8 -d N`, and prints per-file and total counts:
how many messages each found, how many both found, and CPU seconds. jt9's
a-priori decodes (tagged a1..a7) are counted separately, since Fern-FT8
uses no a-priori information. Messages are compared as text with runs of
spaces folded.
"""
import argparse
import glob
import json
import os
import re
import resource
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FERN = os.path.join(ROOT, "build", "fern-ft8")


def parse(lines):
    msgs, ap = set(), set()
    for ln in lines:
        if "~" not in ln:
            continue
        rest = ln.split("~", 1)[1].rstrip()
        parts = re.split(r"\s{2,}", rest.strip())
        m = " ".join(parts[0].split())
        msgs.add(m)
        if len(parts) > 1 and re.match(r"^(a\d|\?)", parts[1]):
            ap.add(m)
    return msgs, ap


def cpu_of(cmd, cwd=None):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return out.stdout.splitlines(), cpu


def run_one(args):
    wav, depth, jt9_depth, rate_test = args
    cmd = [FERN, "decode", wav, "--depth", str(depth)]
    if rate_test:
        cmd += ["--rate-test", str(rate_test)]
    lines, cpu = cpu_of(cmd)
    fern, _ = parse(lines)
    res = {"wav": wav, "fern": sorted(fern), "fern_cpu": cpu}
    if jt9_depth:
        with tempfile.TemporaryDirectory() as td:
            lines, cpu = cpu_of(["jt9", "-8", "-d", str(jt9_depth), "-p", "15", "-a", td, "-t", td, wav], cwd=td)
        jt9, ap = parse(lines)
        res.update({"jt9": sorted(jt9), "jt9_ap": sorted(ap), "jt9_cpu": cpu})
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wavdir")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--jt9-depth", type=int, default=2)
    ap.add_argument("--rate-test", type=float, default=0)
    ap.add_argument("--json")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    wavs = []
    for w in sorted(glob.glob(os.path.join(a.wavdir, "**", "*.wav"), recursive=True)):
        with wave.open(w) as f:
            if f.getframerate() == 12000:
                wavs.append(w)
    have_jt9 = subprocess.run(["which", "jt9"], capture_output=True).returncode == 0
    jd = a.jt9_depth if have_jt9 else 0
    if not have_jt9:
        print("jt9 not found: Fern-FT8 counts only", file=sys.stderr)
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(run_one, [(w, a.depth, jd, a.rate_test) for w in wavs]))
    tot = {"files": len(results), "fern": 0, "fern_cpu": 0.0, "jt9": 0, "jt9_ap": 0, "jt9_cpu": 0.0, "both": 0,
           "only_fern": 0, "only_jt9": 0, "only_jt9_nonap": 0}
    for r in results:
        f = set(r["fern"])
        tot["fern"] += len(f)
        tot["fern_cpu"] += r["fern_cpu"]
        line = "%-40s fern %3d" % (os.path.relpath(r["wav"], a.wavdir), len(f))
        if jd:
            j = set(r["jt9"])
            japs = set(r["jt9_ap"])
            tot["jt9"] += len(j)
            tot["jt9_ap"] += len(japs)
            tot["jt9_cpu"] += r["jt9_cpu"]
            tot["both"] += len(f & j)
            tot["only_fern"] += len(f - j)
            tot["only_jt9"] += len(j - f)
            tot["only_jt9_nonap"] += len((j - f) - japs)
            line += "  jt9 %3d (ap %d)  both %3d  only-fern %2d  only-jt9 %2d" % (
                len(j), len(japs), len(f & j), len(f - j), len(j - f))
            if not a.quiet:
                for m in sorted(f - j):
                    line += "\n      + " + m
                for m in sorted(j - f):
                    line += "\n      - " + m + ("   (a-priori)" if m in japs else "")
        print(line)
    print()
    print("files %d, Fern-FT8 depth %d: %d decodes, %.2f s CPU (%.3f s per file)" % (
        tot["files"], a.depth, tot["fern"], tot["fern_cpu"], tot["fern_cpu"] / max(1, tot["files"])))
    if jd:
        print("jt9 depth %d: %d decodes (%d a-priori), %.2f s CPU (%.3f s per file)" % (
            jd, tot["jt9"], tot["jt9_ap"], tot["jt9_cpu"], tot["jt9_cpu"] / max(1, tot["files"])))
        print("both %d = %.1f%% of jt9's; only Fern-FT8 %d; only jt9 %d (%d of them without a-priori)" % (
            tot["both"], 100.0 * tot["both"] / max(1, tot["jt9"]), tot["only_fern"], tot["only_jt9"],
            tot["only_jt9_nonap"]))
        print("Fern-FT8 count = %.1f%% of jt9's" % (100.0 * tot["fern"] / max(1, tot["jt9"])))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"totals": tot, "files": results}, f, indent=1)


if __name__ == "__main__":
    main()
