# Test recordings

Three 15 s FT8 slots recorded off the air, 12000 Hz mono 16-bit, from the
test set of ft8_lib (https://github.com/kgoba/ft8_lib, commit 9fec6ca,
`test/wav/`), copyright (c) 2018 Kārlis Goba, MIT licence (the full text is
in `LICENSE-ft8_lib.txt`):

| file here | ft8_lib file |
|---|---|
| `20m_busy_test_01.wav` | `test/wav/20m_busy/test_01.wav` |
| `191111_110130.wav` | `test/wav/191111_110130.wav` |
| `websdr_test4.wav` | `test/wav/websdr_test4.wav` |

Only the recordings are used; no ft8_lib code is.

`*.reference.txt` list the messages WSJT-X decodes from each file, one per
line, sorted: those of WSJT-X 2.7.0-rc3's `jt9 -8 -d 3` together with the
decodes ft8_lib ships next to each recording (made with WSJT-X). The golden
tests in `tests/test_decoder.cpp` require every confident Fern-FT8 decode of
these files to be in that list, and a minimum number of the list to be
found.
