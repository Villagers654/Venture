#!/usr/bin/env python3
"""venture-runtime --prepare on a fixture install: a first launch's one-time work
(native libraries, Java conversion, translated code) is done without running the
game, recorded in prepared.json, and a second pass finds nothing left to do.

  first_launch_prepare_test.py --runtime <venture-runtime> --cc <clang>
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zipfile


def records(vpc: Path):
    """Block records of a portable block cache file (cpu/portable_cache.cpp: VPC5)."""
    data = vpc.read_bytes()
    assert data[:4] == b"VPC5", data[:4]
    offset, found = 4, []
    while offset + 16 <= len(data):
        descriptor, size, _ = struct.unpack_from("<QII", data, offset)
        if offset + 16 + size > len(data):
            break
        if descriptor not in (2**64 - 1, 2**64 - 2):  # dictionary and explored marks
            found.append(descriptor)
        offset += 16 + size
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    java = args.runtime.parent / "java"
    exe = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="venture-prepare-") as temp:
        root = Path(temp)
        # Java: one class, converted to classes.dex with the bundled tools.
        source = root / "Fixture.java"
        source.write_text("package com.venture.fixture;\npublic final class Fixture { public static int answer() { return 42; } }\n")
        subprocess.run([str(java / "jdk/bin" / ("javac" + exe)), "--release", "8", "-d", str(root), str(source)], check=True)
        with zipfile.ZipFile(root / "source.jar", "w") as jar:
            jar.write(root / "com/venture/fixture/Fixture.class", "com/venture/fixture/Fixture.class")
        subprocess.run([str(java / "jdk/bin" / ("java" + exe)), "-cp", str(java / "dex2jar/lib/*"), "com.android.dx.command.Main",
                        "--dex", "--output=" + str(root / "classes.dex"), str(root / "source.jar")], check=True)
        # Native: a freestanding ARM64 library with unwind tables, so its functions are known.
        (root / "fixture.c").write_text("""
int leaf(int x) { return x * 3 + 1; }
int __attribute__((noinline)) middle(int x) { return x > 10 ? leaf(x) : leaf(x + 7) - 2; }
int entry(int x) { int total = 0; for (int i = 0; i < x; ++i) total += middle(i); return total; }
""")
        subprocess.run([args.cc, "--target=aarch64-linux-android23", "-fuse-ld=lld", "-shared", "-nostdlib", "-O1",
                        "-fno-stack-protector", "-funwind-tables", "-Wl,--eh-frame-hdr", str(root / "fixture.c"),
                        "-o", str(root / "libfixture.so")], check=True)
        install = root / "games/com.venture.fixture/1"
        install.mkdir(parents=True)
        with zipfile.ZipFile(install / "base.apk", "w") as apk:
            apk.write(root / "classes.dex", "classes.dex")
            apk.writestr("AndroidManifest.xml", struct.pack("<II", 0x00080003, 8))
            apk.write(root / "libfixture.so", "lib/arm64-v8a/libfixture.so")
        (install / "plan.json").write_text(json.dumps({"package": "com.venture.fixture", "versionCode": 1,
                                                       "files": [{"kind": "apk", "name": "base.apk"}]}))
        env = {k: v for k, v in os.environ.items() if not k.startswith("VENTURE_")}
        env.update(VENTURE_DATA_DIR=str(root / "data"), VENTURE_JIT_AOT_WORKERS="2")
        cache = root / "data/cache/com.venture.fixture/1"

        def prepare(name):
            log = root / f"{name}.log"
            run = subprocess.run([str(args.runtime), "--game", str(install), "--package", "com.venture.fixture",
                                  "--account", "local", "--prepare", "--log", str(log)],
                                 env=env, capture_output=True, text=True, timeout=600)
            text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            assert run.returncode == 0, (run.returncode, run.stdout, run.stderr, text)
            return json.loads((cache / "prepared.json").read_text()), text

        stamp, log = prepare("cold")
        assert stamp["complete"] is True, (stamp, log)
        assert stamp["runtime"], stamp
        assert (cache / "lib/arm64-v8a/libfixture.so").read_bytes() == (root / "libfixture.so").read_bytes()
        assert list((cache / "java").rglob("*.jar")), "no converted Java"
        assert not (cache / "prepare.pid").exists(), "the preparation left its process id behind"
        blocks = [r for vpc in (cache / "jit-blocks").glob("*.vpc") for r in records(vpc)]
        # Each function's entry, and the blocks it branches and returns to.
        assert stamp["translated_blocks"] >= 3 and len(blocks) >= stamp["translated_blocks"], (stamp, len(blocks), log)
        print(f"Cold preparation: {stamp['translated_blocks']} blocks, Java and libraries: PASS", flush=True)
        stamp, log = prepare("warm")
        assert stamp["complete"] is True and stamp["translated_blocks"] == 0, (stamp, log)
        print("Second preparation translates nothing again: PASS", flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
