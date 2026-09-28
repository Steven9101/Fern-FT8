# The FT8 protocol files

These are the public-domain files that define parts of the FT4 and FT8
protocols, unchanged: reference [14] of S. Franke K9AN, B. Somerville G4WJS
and J. Taylor K1JT, "The FT4 and FT8 Communication Protocols", QEX
July/August 2020, pp. 7-17. The paper places the protocol description and
these files in the public domain, on the conditions in its section 9.

Source: `ft4_ft8_protocols.tgz` from
<https://www.arrl.org/files/file/QEX%20Binaries/2020/ft4_ft8_protocols.tgz>,
fetched 2026-09-28, SHA-256
`19d680a2676490a829245b90d386b72c8f2ea857fcaff3c872a7bcd9ba784f84`, the
directory `ft4_ft8_public/`. The files keep their CRLF line ends.

`tools/gen_tables.py` builds `src/protocol_tables.cpp` from `generator.dat`,
`parity.dat`, `arrl_rac_sections.txt` and `states_provinces.txt`;
`make check-tables` fails if the two ever differ. The Fortran programs are
the definitions the C++ code follows for CRC-14, callsign hashes and the
field encodings; the tests use their outputs as known vectors.
