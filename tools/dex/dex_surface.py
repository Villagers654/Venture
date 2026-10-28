"""Strict DEX declaration census, ported from the r2-java research parser.

Format: https://source.android.com/docs/core/runtime/dex-format
No code is executed. DEX 041 containers and compact DEX require another reader.
"""
import hashlib
import re
import struct
import zlib


class Dex:
    def __init__(self, data: bytes, name: str, *, verify_checksum=True):
        self.data, self.name = data, name
        if data[:8] not in [b"dex\n" + v + b"\0" for v in (b"035", b"037", b"038", b"039", b"040")]:
            raise ValueError(f"{name}: unsupported DEX magic {data[:8]!r}")
        if len(data) < 112 or self.u32(32) != len(data) or self.u32(36) != 112 or self.u32(40) != 0x12345678:
            raise ValueError(f"{name}: invalid DEX header")
        # ART validates Adler-32, not the SHA-1 identification field. The owned
        # RE4 APK has a stale SHA-1 but a valid Adler-32 (including that field).
        # AOSP android12L-release, libdexfile/dex/dex_file_verifier.cc: CheckHeader
        # https://android.googlesource.com/platform/art/+/android12L-release/libdexfile/dex/dex_file_verifier.cc
        self.checksum_valid = zlib.adler32(data[12:]) & 0xffffffff == self.u32(8)
        if verify_checksum and not self.checksum_valid:
            raise ValueError(f"{name}: DEX checksum mismatch")
        strings = self.strings = [self.string(self.u32(self.u32(60) + 4 * i)) for i in range(self.u32(56))]
        self.types = [strings[self.u32(self.u32(68) + 4 * i)] for i in range(self.u32(64))]
        protos = []
        for i in range(self.u32(72)):
            _, ret, params = struct.unpack_from("<III", data, self.u32(76) + 12 * i)
            protos.append("(" + "".join(self.typelist(params)) + ")" + self.types[ret])
        methods = self.methods = []
        for i in range(self.u32(88)):
            cls, proto, nm = struct.unpack_from("<HHI", data, self.u32(92) + 8 * i)
            methods.append((self.types[cls][1:-1], strings[nm], protos[proto]))
        self.fields = []
        for i in range(self.u32(80)):
            cls, typ, nm = struct.unpack_from("<HHI", data, self.u32(84) + 8 * i)
            self.fields.append((self.types[cls][1:-1], strings[nm], self.types[typ]))
        self.classes, self.natives = set(), set()
        self.parents, self.defined_methods, self.defined_fields = {}, {}, set()
        for i in range(self.u32(96)):
            cls, _, parent, interfaces, _, _, p, _ = struct.unpack_from("<8I", data, self.u32(100) + 32 * i)
            owner = self.types[cls][1:-1]
            if owner in self.classes:
                raise ValueError(f"{name}: duplicate class {owner}")
            self.classes.add(owner)
            self.parents[owner] = ([self.types[parent][1:-1]] if parent != 0xffffffff else []) + [
                t[1:-1] for t in self.typelist(interfaces)]
            if not p:
                continue
            sizes = []
            for _ in range(4):
                size, p = self.uleb(p)
                sizes.append(size)
            for count in sizes[:2]:
                idx = 0
                for _ in range(count):
                    delta, p = self.uleb(p)
                    idx += delta
                    _, p = self.uleb(p)
                    self.defined_fields.add(self.fields[idx])
            for count in sizes[2:]:
                idx = 0
                for _ in range(count):
                    delta, p = self.uleb(p)
                    idx += delta
                    flags, p = self.uleb(p)
                    code, p = self.uleb(p)
                    if methods[idx][0] != owner:
                        raise ValueError(f"{name}: method owner mismatch")
                    self.defined_methods[methods[idx]] = (flags, code)
                    if flags & 0x100:
                        if code:
                            raise ValueError(f"{name}: native method contains code")
                        self.natives.add((*methods[idx], bool(flags & 8)))

    def references(self, code):
        """Yield (kind, value, opcode) in a code_item, without executing code.

        AOSP dalvik-bytecode instruction widths, including embedded payloads.
        Branches are deliberately not pruned: reachability is conservative.
        """
        if not code:
            return
        size = self.u32(code + 12)
        words = struct.unpack_from(f"<{size}H", self.data, code + 16)
        p = 0
        while p < size:
            op = words[p] & 255
            width = _WIDTHS[op]
            if op == 0 and words[p]:
                kind = words[p] >> 8
                if kind == 1:
                    width = 4 + 2 * words[p + 1]
                elif kind == 2:
                    width = 2 + 4 * words[p + 1]
                elif kind == 3:
                    count = words[p + 2] | words[p + 3] << 16
                    width = 4 + (words[p + 1] * count + 1) // 2
                else:
                    raise ValueError(f"{self.name}: unknown DEX payload {kind}")
            if not width or p + width > size:
                raise ValueError(f"{self.name}: invalid opcode/width {op:02x} at {code:x}+{p}")
            if op in (0x1c, 0x1f, 0x20, 0x22, 0x23, 0x24, 0x25):
                yield "class", self.types[words[p + 1]], op
            elif 0x52 <= op <= 0x6d:
                yield "field", self.fields[words[p + 1]], op
            elif 0x6e <= op <= 0x72 or 0x74 <= op <= 0x78 or op in (0xfa, 0xfb):
                yield "method", self.methods[words[p + 1]], op
            elif op in (0x1a, 0x1b):
                idx = words[p + 1] | (words[p + 2] << 16 if op == 0x1b else 0)
                yield "string", self.strings[idx], op
            p += width


    def u32(self, p):
        return struct.unpack_from("<I", self.data, p)[0]

    def uleb(self, p):
        value = 0
        for shift in range(0, 35, 7):
            b = self.data[p]
            p += 1
            value |= (b & 127) << shift
            if not b & 128:
                return value, p
        raise ValueError(f"{self.name}: overlong ULEB128")

    def string(self, p):
        size, p = self.uleb(p)
        end = self.data.index(0, p)
        value = self.data[p:end].replace(b"\xc0\x80", b"\0").decode("utf-8", "surrogatepass")
        if len(value.encode("utf-16-le", "surrogatepass")) // 2 != size:
            raise ValueError(f"{self.name}: invalid modified UTF-8 string")
        return value

    def typelist(self, p):
        return [self.types[struct.unpack_from("<H", self.data, p + 4 + 2 * i)[0]] for i in range(self.u32(p))] if p else []

# https://source.android.com/docs/core/runtime/dalvik-bytecode
_WIDTHS = [0] * 256
for _ops, _width in (
    (range(0x00, 0x13), 1), (range(0x13, 0x17), 2), (range(0x1a, 0x1d), 2),
    (range(0x1d, 0x1f), 1), (range(0x1f, 0x24), 2), (range(0x24, 0x27), 3),
    (range(0x27, 0x29), 1), (range(0x2d, 0x3e), 2), (range(0x44, 0x6e), 2),
    (range(0x6e, 0x73), 3), (range(0x74, 0x79), 3), (range(0x7b, 0x90), 1),
    (range(0x90, 0xb0), 2), (range(0xb0, 0xd0), 1), (range(0xd0, 0xe3), 2),
    ((0xfa, 0xfb), 4), ((0xfc, 0xfd), 3), ((0xfe, 0xff), 2)):
    for _op in _ops:
        _WIDTHS[_op] = _width
for _op, _width in {2: 2, 3: 3, 5: 2, 6: 3, 8: 2, 9: 3, 0x14: 3, 0x17: 3,
                   0x18: 5, 0x19: 2, 0x1b: 3, 0x21: 1, 0x29: 2, 0x2a: 3,
                   0x2b: 3, 0x2c: 3}.items():
    _WIDTHS[_op] = _width


def read_apk(archive):
    names = sorted(n for n in archive.namelist() if re.fullmatch(r"classes(?:[0-9]+)?\.dex", n))
    if not names:
        raise ValueError("APK contains no root classes*.dex")
    dexes = [(name, archive.read(name)) for name in names]
    classes, natives = set(), set()
    digest = hashlib.sha256()
    for name, data in dexes:
        dex = Dex(data, name)
        if classes & dex.classes:
            raise ValueError("APK contains duplicate class definitions")
        classes |= dex.classes
        natives |= dex.natives
        digest.update(name.encode() + b"\0" + hashlib.sha256(data).digest())
    dexsha = hashlib.sha256(dexes[0][1]).hexdigest() if len(dexes) == 1 else digest.hexdigest()
    return dexes, classes, natives, dexsha
