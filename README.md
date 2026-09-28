# Fern-FT8

Fern-FT8 decodes FT8, the weak-signal digital mode of WSJT-X, for
[FernSDR](https://github.com/Steven9101/WebSDR). It will run as a FernSDR
decoder module: FernSDR cuts narrow channels out of its bands and streams
them to the module, which reports what it decodes (FernSDR's
`docs/MODULES.md`, "Decoders"). This repository holds the decoding engine, a
library with a command line tool and tests; the module around it is built
separately.

It only receives. It follows the FT8 protocol as its authors define it
(S. Franke K9AN, B. Somerville G4WJS, J. Taylor K1JT, "The FT4 and FT8
Communication Protocols", QEX July/August 2020) and is an independent
implementation: no WSJT-X or ft8_lib code is in it. `docs/PROTOCOL-SOURCES.md`
lists what comes from where; `docs/DESIGN.md` explains how it decodes.

## Build

A C++17 compiler (g++) and make; nothing else, and the result needs nothing
but the C++ runtime.

```
make              # build/libfernft8.a and build/fern-ft8
make test         # 49 tests: protocol vectors, messages, DSP, whole decodes
make test-asan    # the same under AddressSanitizer and UBSan
make test-cross CROSS_ARCH=aarch64   # or armhf: cross build, run under qemu-user
make bench        # CPU per busy and per quiet slot, one thread
```

## Command line

```
fern-ft8 decode FILE.wav... [--depth 1|2|3] [--rate-test R] [--verbose]
```

Decodes 15 s slots of real audio (a WAV starting at the slot, as WSJT-X
saves them) and prints one line per decode as `jt9` does:
`HHMMSS SNR DT FREQ ~  MESSAGE`. The audio is turned into complex baseband
around 2000 Hz and goes through the same channel code as live input;
`--rate-test R` resamples it to R complex samples a second first, to try
rates such as 7031.25. `--verbose` adds, per decode, the type, `bp`, `osd` or
`low` (an OSD decode that may be wrong), the pass, hard errors and sync.
Depth 2 is the default.

```
fern-ft8 encode "MESSAGE" [--freq HZ] [--snr DB] [--dt S] [--rate HZ] [--seed N] -o OUT.wav
```

Writes a 15 s slot with one transmission of any FT8 message type, with white
noise at the given SNR in 2500 Hz: test signals, not for the air.

```
fern-ft8 noise [--minutes M] [--depth N] [--seed S] [--rate R] [--silence]
fern-ft8 bench [--depth N] [FILE.wav...]
```

`noise` feeds white Gaussian noise (or silence) through a live channel and
prints every decode, each of them false; `bench` measures CPU time.

## Engine interface

`src/fern_ft8.h` is the interface for the module. A `Channel` takes the
decoder contract's sample frames as they come (complex float samples, the
running sample index, the UTC time of the first sample in microseconds, the
lost-samples and clock-set flags); it resamples onto a UTC-aligned grid and
decodes each slot once 1.5 s before to 16.5 s after its start have arrived.
Each `Decode` holds what a decode event needs: slot time, audio frequency
above the dial, SNR, DT, the message as WSJT-X prints it, its type, the
transmitting station's call when it was sent in full (never a call known
only by hash), grid, report, and whether BP or OSD found it and whether it
is low confidence. A priori information is never used. One
`CallsignHashTable` serves all channels on all threads, with expiry and a
size limit.

## Measured results

All on an AMD Ryzen 9 9950X (6 vCPU virtual machine), one thread per
decode, AVX2, commit of this README.

**Sensitivity**, white Gaussian noise, 100 slots per 0.5 dB step, SNR at
which half the slots decode:

| test signals | Fern-FT8 depth 1 | depth 2 | depth 3 | jt9 depth 2 | jt9 depth 3 |
|---|---|---|---|---|---|
| WSJT-X `ft8sim`, "K1ABC W9XYZ EN37", 1500 Hz, DT 0 | | -21.04 dB | -21.21 dB | -21.13 dB | -21.19 dB |
| `fern-ft8 encode`, random message, 300 to 2700 Hz, DT -0.5 to 1.0 s | -20.91 dB | -21.10 dB | -21.14 dB | | |

```
scripts/threshold.py --generator ft8sim --depth 3 --jt9 3 --from -23 --to -18.5
scripts/threshold.py --generator ft8sim --depth 2 --jt9 2 --from -23 --to -18.5
scripts/threshold.py --generator fern --depth D --from -23 --to -17
```

Decode probability at depth 3 on the `ft8sim` slots, with jt9 depth 3 on
the same files:

| SNR dB | -23 | -22.5 | -22 | -21.5 | -21 | -20.5 | -20 | -19.5 | -19 |
|---|---|---|---|---|---|---|---|---|---|
| Fern-FT8 | 0.00 | 0.01 | 0.13 | 0.35 | 0.61 | 0.86 | 0.97 | 0.99 | 1.00 |
| jt9 | 0.01 | 0.03 | 0.14 | 0.34 | 0.60 | 0.77 | 0.94 | 0.98 | 1.00 |

**Real recordings**: the 62 slots at 12 kHz in ft8_lib's `test/wav` (38 of
them busy 20 m slots), decoded by Fern-FT8 and by WSJT-X 2.7.0-rc3's `jt9`
(which does use a priori information at depths 2 and 3):

| | decodes | found by both | CPU per file |
|---|---|---|---|
| jt9 depth 2 | 1424 (15 a priori) | | 1.56 s |
| jt9 depth 3 | 1462 (20 a priori) | | 2.23 s |
| Fern-FT8 depth 1 | 1324 | 1283 = 90.1 % of jt9 d2's | 0.11 s |
| Fern-FT8 depth 2 | 1418 = 99.6 % of jt9 d2's | 1344 = 94.4 % of jt9 d2's | 0.15 s |
| Fern-FT8 depth 3 | 1436 = 98.2 % of jt9 d3's | 1370 = 93.7 % of jt9 d3's | 0.20 s |

```
scripts/compare.py path/to/ft8_lib/test/wav --depth 2 --jt9-depth 2
```

The same recordings through odd channel rates at depth 2 give 1416
(7031.25 Hz), 1415 (5859.375 Hz) and 1415 (4000 Hz) decodes
(`--rate-test`).

**False decodes**: none in 24 hours of white noise at depth 3, 12 hours at
depth 2, 12 hours at depth 1, and an hour of digital silence:

```
fern-ft8 noise --minutes 240 --depth 3 --seed 101    # and seeds 102..106
fern-ft8 noise --minutes 240 --depth 2 --seed 201    # and 202, 203
fern-ft8 noise --minutes 240 --depth 1 --seed 301    # and 302, 303
fern-ft8 noise --minutes 60 --silence
```

**CPU per slot**, one thread, over the 38 busy 20 m recordings (about 27
decodes each):

| depth | mean | largest |
|---|---|---|
| 1 | 0.094 s | 0.114 s |
| 2 | 0.137 s | 0.157 s |
| 3 | 0.184 s | 0.212 s |

A quiet slot (noise only, `make bench`) takes 0.021, 0.027 and 0.040 s. With
vector code off (`FERN_FT8_SIMD=scalar`) a busy slot takes about a third
longer (0.185 against 0.137 s at depth 2 on the synthetic busy slot of
`make bench`); decodes are identical at every vector level.

## Licence

GPL-2.0-or-later, see `LICENSE`. The FT8 protocol description and the files
in `protocol/` are public domain by their authors' statement; the test
recordings in `tests/data/` come from ft8_lib under the MIT licence.
