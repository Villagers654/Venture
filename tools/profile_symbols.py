#!/usr/bin/env python3
"""Build a compact ASLR-independent host symbol index for VENTURE_PROFILE."""
import argparse
import struct
import subprocess
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("nm")
    p.add_argument("executable", type=Path)
    args = p.parse_args()
    data = args.executable.read_bytes()
    if data.startswith(b"\x7fELF"):
        # ELF64: index relative to the lowest PT_LOAD address (0 for PIE).
        phoff, = struct.unpack_from("<Q", data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        loads = [struct.unpack_from("<Q", data, phoff + i * phentsize + 0x10)[0] for i in range(phnum)
                 if struct.unpack_from("<I", data, phoff + i * phentsize)[0] == 1]
        base = min(loads) & ~0xfff
    else:
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    lines = subprocess.check_output([args.nm, "--numeric-sort", "--demangle", "--print-size", str(args.executable)],
                                    encoding="utf-8").splitlines()
    # Weak code symbols (inline and template functions, e.g. the x64 emitter's) are indexed too, with sizes, so a
    # sample past a function's end is not credited to the symbol before it. Lines: address [size] type name.
    symbols = {}
    for line in lines:
        parts = line.split(" ", 2)
        if len(parts) == 3 and len(parts[1]) > 1:
            parts = [parts[0], parts[1]] + parts[2].split(" ", 1)
        else:  # nm prints no size for some symbols
            parts.insert(1, "0")
        if len(parts) == 4 and parts[2] in ("t", "T", "w", "W"):
            address = int(parts[0], 16)
            if address >= base and not symbols.get(address - base, (0,))[0]:
                symbols[address - base] = (int(parts[1], 16), parts[3])
    target = args.executable.with_suffix(".profile-symbols.tsv")
    target.write_text("".join(f"{addr:x}\t{size:x}\t{name}\n" for addr, (size, name) in sorted(symbols.items())),
                      encoding="utf-8")


if __name__ == "__main__":
    main()
