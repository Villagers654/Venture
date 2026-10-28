# Third-party notices

Venture's JIT (`src/jit`) is GPL-3.0-or-later like the rest of Venture. It
descends from Dynarmic; original Dynarmic portions retain 0BSD (notice in
`LICENSES/0BSD.txt`), and files keep their SPDX copyright lines. xbyak is
kept in `src/jit/xbyak` (BSD-3-Clause, `COPYRIGHT` there) because Venture adds
dual-mapped code buffer support to it. The JIT's other libraries (fmt, mcl,
robin-map and, for tests, Catch2 and oaknut) are unmodified and pinned by commit
and archive SHA256 in `cmake/Dependencies.cmake` like every other fetched
dependency; release packages ship their license files under `licenses/<name>`.
