# How Fern-FT8 decodes

This is the engine: a library (`build/libfernft8.a`, header
`src/fern_ft8.h`), the `fern-ft8` command line tool and the tests. The
FernSDR module around it (pipes, the decoder contract's JSON) is separate
work. [QEX] below is the protocol paper, see `docs/PROTOCOL-SOURCES.md`.

## From frames to slots

FernSDR's decoder contract (API 2 in its `docs/MODULES.md`) delivers each
channel as complex baseband at a rate that need not be whole, centred
`offset` hertz above the dial, in frames with a running sample index and
the UTC time of their first sample. `Channel::push()` takes exactly that.

`Channel` resamples onto one grid for all channels: complex samples at
6400 Hz, sample g at g/6400 s after 1970. 6400 Hz gives 1024 samples a
symbol, a power of two, and covers 5400 Hz of channel width; 15 s is 96000
grid samples, so slots start on grid samples. The resampler reads the input
at any fractional position with a Kaiser-windowed sinc (beta 7, about 70
dB) whose transition band is everything that cannot alias into the channel,
so it is short: 18 taps at 8000 Hz. Positions come from the anchor (the
index and time of the frame that started the stream), computed in double
from exact integers, so a stream can run for years without drift.

A frame whose index or time does not follow on (more than 20 ms off), or
that is flagged as lost samples or a new clock, starts a new anchor; the
samples that never came stay missing, and a clock set back drops what was
buffered. A ring of 64 s holds the grid. A slot is decoded once the grid
holds 1.5 s before its start to 16.5 s after: transmissions start 0.5 s into
the slot and last 12.64 s, and DT from -2.0 to +2.5 s is searched, as WSJT-X
does. `finish()` decodes what is left when a stream ends, if it reaches at
least 2 s into a slot.

## One slot

`SlotDecoder::decode()` takes the 18 s (115200 samples) and runs up to three
passes; each pass decodes what it can and subtracts it, and the next pass
looks again. Depth 1 runs two passes and no OSD.

1. **Spectrogram.** One-symbol windows (1024 samples), zero-padded to 2048,
   every quarter symbol: half-tone bins (3.125 Hz) every 40 ms. The window
   is rectangular: it matches a symbol, which maximises the power a tone
   puts in its bin.

2. **Candidates.** For every tone-0 bin and start frame, the power at the
   21 Costas tones against the other 7 tones of the same symbols, over all
   three Costas arrays, or over two when the transmission began before the
   buffer or runs past its end. Local maxima above a threshold (2.0, 1.8,
   1.6 by depth), strongest first; of peaks within one bin and three frames
   (120 ms) of each other only the strongest is kept.

3. **Pre-check.** In the same spectrogram, the number of Costas symbols
   whose Costas tone is the strongest of the 8. Candidates below 7, 6 or 5
   (by depth) are dropped, and at most 100, 150 or 250 are kept per pass.
   This check costs nothing and rejects what would fail later: on the 62
   recordings it gave more decodes for 18 % less CPU than a larger list
   without it (1418 against 1401 decodes at depth 2).

4. **Baseband.** One 131072-point FFT of the slot per pass; for each
   candidate the 200 Hz around it is taken out, tapered from 60 to 90 Hz,
   and transformed back: 4096 samples at 200 Hz, 32 per symbol.

5. **Time and frequency.** A search over the Costas symbols (noncoherent:
   each symbol's correlation on its own) finds the start to 5 ms and the
   frequency to 0.5 Hz. That metric hardly changes for smaller errors, and
   the errors that remain (0.3 to 0.6 Hz, up to 12 ms at -21 dB) ruin the
   coherent block detection that follows. The phases do change: with each
   tone's correlation referred to one time origin, the phase turns by
   2 pi df T from one symbol to the next, and by 2 pi 6.25 Hz (k' - k) tau
   more when the tone changes from k to k' and the signal is tau late.
   Summing c[s+1] conj(c[s]) over all 78 neighbouring pairs, at the Costas
   tones and at the strongest tone elsewhere (a wrong decision only adds
   noise), gives tau as the delay that lines those phases up best (a search
   over +-12 ms by 0.25 ms) and df as their common turn; three rounds. At
   -20.75 dB this raised the decode rate from 27 % to 64 %.

6. **Soft bits.** The 8 tone amplitudes of each symbol, then four sets of
   soft bits: blocks of 1, 2 and 3 symbols (noncoherent block detection,
   Simon and Divsalar 1993, as in [QEX] section 6: for each bit the largest
   coherent-sum magnitude among tone sequences with the bit 0 less the
   largest with it 1), and single symbols divided by their strongest tone.
   The last set carries interference: a symbol that another signal hits
   has one very strong wrong tone, which the other sets turn into confident
   wrong bits (2 % more decodes on the recordings when it was added).

7. **LDPC.** Normalised min-sum belief propagation (factor 0.75), up to 30
   iterations, stopped when the count of failing checks has not fallen for
   6 iterations. It decoded as well as the exact sum-product rule (70 %
   against 68 % of 400 slots at -20.75 dB) with no tanh or log. The four
   sets run at once in the four lanes of a vector; the first in order that
   converges, passes the CRC and has at most 36 bits against the received
   hard decisions is taken. When none does, depth 2 and 3 run ordered
   statistics decoding (OSD, Lin and Costello chapter 10) on the 3-symbol
   and 1-symbol sets: Gauss-Jordan elimination of the generator in order of
   reliability (bit-packed, three 64-bit words a row), then every one of the
   91 most reliable independent bits flipped (order 1), and at depth 3 every
   pair among the 30 least reliable of them (order 2). The nearest of those
   codewords that passes the CRC is the result. Taking the nearest codeword
   and then checking its CRC instead lost decodes: a nearer wrong codeword
   hides the right one (1366 against 1327 decodes at depth 3).

8. **Acceptance.** A codeword must pass the CRC, and its payload must be a
   message a conforming transmitter sends (`src/message.h`): a defined type,
   every field in range and canonical, so that packing the text again gives
   the same bits. Of random payloads about a third pass, which divides the
   false decodes that get through the CRC by three. OSD, which picks among
   thousands of codewords, needs more: it runs only for candidates with a
   sync ratio of 2.8 or more (99 % of candidates on white noise stay below
   2.82), and its result is kept with at most 36 hard errors when 11 or more
   Costas tones were received right, or 24 with 9 or 10. On 3 hours of
   white noise with these limits taken off, OSD results had 5 to 13 Costas
   hits and 24 to 57 hard errors; true OSD decodes at -21.5 to -19 dB had
   11 or more hits, and on the recordings those WSJT-X confirms had a sync
   ratio of 3.17 or more. An OSD decode with more than 30 hard errors or
   fewer than 11 hits is marked low confidence (`quality() == "low"`), to be
   shown but not reported to others. A priori information is never used.

9. **Subtraction.** A decode's waveform is rebuilt at 6400 Hz from its
   tones. Its start is refined against the samples to a fraction of a sample
   with a correlation that is coherent over 8 symbols (per symbol it would
   not see a time error), the frequency from the phase step of the
   per-symbol gains, and the complex gain along the transmission is the
   received signal times the conjugate reference, smoothed twice over one
   symbol (a triangle of 0.32 s), divided by the smoothed reference power.
   Gain times reference is subtracted ([QEX] section 6). On a synthetic
   signal the residual is 57.6 dB below it at +30 dB SNR (with per-symbol
   timing it was 25.7 dB, which left strong signals' remains over weak
   ones).

10. **SNR.** The power of the estimated gain, less the noise the smoothing
    lets through, over the noise power in 2500 Hz, WSJT-X's convention. The
    noise comes from Hann-windowed spectra (a rectangular window leaks a
    strong signal's power into its neighbourhood and capped SNRs at about
    +20 dB): per 6.25 Hz bin the lower quartile over time, scaled to the
    mean, then the lower quartile of those over +-150 Hz. On synthetic
    signals (10 per step, `fern-ft8 encode` at 12000 Hz) the estimate is
    within 1 dB of the truth from -20 to +20 dB.

## Callsign hashes

One `CallsignHashTable` serves all channels and slots: a call heard on 40 m
resolves its hash on 20 m. It learns the transmitting station's call from
confident decodes that carry it in full, as a receiver should, and the
nonstandard call of type 4 messages. Entries expire after an hour and the
table holds at most 20000 calls (least recently heard go first); a later
call with the same hash replaces an earlier one. A mutex guards it.

## Threads

A `Channel` (and its `SlotDecoder`) is used by one thread at a time;
channels can decode on different threads at once. Shared state is the hash
table (locked), FFT plans (built once under a lock, then read only) and
static tables (initialised once, thread-safe by C++11). The tests run two
channels on two threads with one table, also under ThreadSanitizer.

## Vector code

Every vector path does exactly the scalar arithmetic: the same operations in
the same order, and the build uses `-ffp-contract=off` so that no multiply
and add fuse. Decodes are therefore identical at every level, which the
tests check on real recordings (`decodes_do_not_depend_on_the_simd_level`),
and each kernel is checked against its scalar form bit for bit.

- Belief propagation runs the four soft-bit sets in the four 32-bit lanes of
  GCC's vector extensions: SSE2 on x86-64 and NEON on aarch64, both always
  present, so no dispatch is needed (and plain scalar code on 32-bit ARM
  without NEON). It saved 16 % of a busy slot's CPU at depth 2.
- The FFT's radix-4 butterflies have an AVX2 kernel, chosen at run time
  when the CPU has AVX2: 5 % of a busy slot, 16 % of a quiet one.

`simd_level()` reports the level; `FERN_FT8_SIMD=scalar` or `baseline`
lowers it. `make test-cross CROSS_ARCH=aarch64` (or `armhf`) builds the
tests with a cross compiler and runs them under qemu-user.

## What is not done

- A priori decoding (the CQ hypothesis and repeat-caller lists of [QEX]
  section 6) is not implemented; the decoder never uses a priori
  information.
- FT4, WSPR and SuperFox are not decoded.
- Candidates are processed one at a time; the belief propagation is vector
  code only across one candidate's four soft-bit sets, not across
  candidates.
- Adaptive depth under a CPU budget, and early decodes before the slot
  ends, are left to the module around the engine.
- The spectrogram and the slot FFT are computed once the slot is complete,
  not while samples arrive.
