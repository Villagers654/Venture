"""Generate authored APK/OBB fixtures and independent ZIP read expectations.

python tests/runtime/android_make_fixtures.py build/android-fixtures
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import struct
import zipfile
import zlib


def sparse(file):
    if os.name != "nt":
        return
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    ioctl = kernel.DeviceIoControl
    ioctl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                      ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    result = ctypes.c_uint32()
    if not ioctl(msvcrt.get_osfhandle(file.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(result), None):
        raise OSError(ctypes.get_last_error(), "Cannot mark ZIP64 fixture sparse")


def zip64(path):
    # All ZIP64 size/offset sentinels, a real local-header offset above 4 GiB.
    name, data = b"assets/large-offset.bin", b"ZIP64 beyond four GiB"
    local_offset = (1 << 32) + 123
    crc = zlib.crc32(data)
    with path.open("wb") as f:
        sparse(f)
        f.seek(local_offset)
        extra = struct.pack("<HHQQ", 1, 16, len(data), len(data))
        f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0, 0, 0, 0, crc, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra)))
        f.write(name + extra + data)
        cd_offset = f.tell()
        extra = struct.pack("<HHQQQ", 1, 24, len(data), len(data), local_offset)
        f.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0, 0, 0, 0, crc,
                            0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra), 0, 0, 0, 0, 0xFFFFFFFF))
        f.write(name + extra)
        cd_size = f.tell() - cd_offset
        zip64_offset = f.tell()
        f.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 1, 1, cd_size, cd_offset))
        f.write(struct.pack("<IIQI", 0x07064B50, 0, zip64_offset, 1))
        f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0))
    with zipfile.ZipFile(path) as z:
        assert z.read(name.decode()) == data


def oracle(path):
    with zipfile.ZipFile(path) as z:
        entries = [i for i in z.infolist() if not i.is_dir()]
        result = {"path": str(path.resolve()), "entries": [
            {"name": i.filename, "size": i.file_size, "compressed": i.compress_type != 0}
            for i in entries]}
        selected = entries if path.name == "test.apk" else sorted(
            (i for i in entries if i.compress_type == 0 or i.file_size < 1024 * 1024),
            key=lambda i: i.file_size, reverse=True)[:4]
        samples = []
        for entry in selected:
            with z.open(entry) as stream:
                # Deliberately seek backwards too. zipfile is an independent oracle.
                for pos in [0, max(0, entry.file_size - 127), entry.file_size // 2, 11, 0]:
                    pos = min(pos, entry.file_size)
                    stream.seek(pos)
                    data = stream.read(127)
                    samples.append({"name": entry.filename, "offset": pos, "hex": data.hex()})
        result["samples"] = samples
        return result


def unflagged_utf8(path):
    # Android APKs in the wild use UTF-8 asset names even with bit 11 clear.
    # zipfile's default CP437 decoding is not an oracle for those name bytes.
    name, data = "assets/norwegianbokm\u00e5l.txt", b"unflagged UTF-8 asset"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(name, data)
    raw = bytearray(path.read_bytes())
    central = raw.index(b"PK\x01\x02")
    for offset in (6, central + 8):
        assert struct.unpack_from("<H", raw, offset)[0] == 0x800
        struct.pack_into("<H", raw, offset, 0)
    path.write_bytes(raw)
    return {"path": str(path.resolve()),
            "entries": [{"name": name, "size": len(data), "compressed": False}],
            "samples": [{"name": name, "offset": 0, "hex": data.hex()}]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1234)
    with zipfile.ZipFile(out / "test.apk", "w") as z:
        for name, data, method in [
            ("assets/plain.bin", bytes(range(256)) * 1024, 0),
            ("assets/deflated.bin", b"Hello Venture!\x00" * 10000, 8),
            ("assets/random.bin", rng.randbytes(262143), 8),
            ("assets/empty.bin", b"", 8),
            ("assets/sub/nested.txt", b"nested", 0),
            ("assets/empty-directory/", b"", 0),
            ("assets/日本語.txt", b"utf8", 8),
            ("res/raw/test.txt", b"resource", 0),
        ]:
            z.writestr(name, data, compress_type=method)
    with zipfile.ZipFile(out / "main.test.obb", "w") as z:
        z.writestr("assets/obb.txt", b"expansion")
        z.writestr("assets/plain.bin", b"must not override APK")
    with zipfile.ZipFile(out / "unity-extraction.apk", "w") as z:
        z.writestr("assets/bin/Data/Managed/Metadata/global-metadata.dat", b"synthetic metadata" * 8192)
        z.writestr("assets/bin/Data/Managed/Resources/test.dll-resources.dat", b"synthetic resources")
    zip64(out / "zip64.apk")
    broken = bytearray((out / "test.apk").read_bytes())
    with zipfile.ZipFile(out / "test.apk") as z:
        i = z.getinfo("assets/deflated.bin")
        # CRC corruption is caught when the stream reaches its end.
        start = broken.find(b"PK\x01\x02")
        while broken[start + 46:start + 46 + len(i.filename)] != i.filename.encode():
            start = broken.find(b"PK\x01\x02", start + 4)
        broken[start + 16] ^= 1
    (out / "bad-crc.apk").write_bytes(broken)
    (out / "truncated.apk").write_bytes(broken[:-7])
    synthetic = [oracle(out / "test.apk"), oracle(out / "zip64.apk"),
                 unflagged_utf8(out / "unflagged-utf8.apk")]
    (out / "synthetic.json").write_text(json.dumps(synthetic), encoding="utf-8")
    print("Generated authored ZIP, unflagged UTF-8 and ZIP64 expectations")


if __name__ == "__main__":
    main()
