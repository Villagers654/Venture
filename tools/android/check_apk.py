#!/usr/bin/env python3
"""Check a built Venture APK without a device: the entries the app needs, how
they are stored, page alignment of the native libraries, and the APK signature
block. With --build-tools it also runs Google's own checks (aapt2 badging,
zipalign -c, apksigner verify).

  check_apk.py FILE.apk [--build-tools DIR] [--java-home DIR]

Exit code 0 when every check passes; each failure is printed.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

PAGE = 16 * 1024
REQUIRED = {
    "AndroidManifest.xml": None,
    "classes.dex": None,
    "resources.arsc": zipfile.ZIP_STORED,  # Android 11+ refuses a compressed table (targetSdk 30+)
    "lib/arm64-v8a/libventure.so": zipfile.ZIP_STORED,
    # The Khronos loader: the headset check uses it, and games whose loader Venture supplies get it.
    "lib/arm64-v8a/libopenxr_loader.so": zipfile.ZIP_STORED,
    "assets/quest_layer/libventure_quest_layer.so": None,
    "assets/quest_layer/venture_quest_layer.json": None,
    # What the app adds to the games it installs (Platform loader and its classes, VrApi).
    "assets/game/classes.dex": None,
    "assets/game/lib/arm64-v8a/libovrplatformloader.so": None,
    "assets/game/lib/arm64-v8a/libvrapi.so": None,
    "assets/game/platform_alias.so": None,
    "assets/game/meta/com.oculus.os.dex": None,
    # The same for ARM32 games on CPUs that still run AArch32: one APK, both ABIs.
    "assets/game/lib/armeabi-v7a/libovrplatformloader.so": None,
    "assets/game/lib/armeabi-v7a/libvrapi.so": None,
    "assets/game/lib/armeabi-v7a/platform_alias.so": None,
    "assets/game/lib/armeabi-v7a/libopenxr_loader.so": None,
    "assets/game/lib/armeabi-v7a/libventure_quest_layer.so": None,
}
# ELF class and machine of the game libraries by ABI folder (EM_AARCH64, EM_ARM).
GAME_ABIS = {"arm64-v8a": (2, 183), "armeabi-v7a": (1, 40)}
SIGNING_BLOCK_MAGIC = b"APK Sig Block 42"


def data_offset(apk: Path, info: zipfile.ZipInfo) -> int:
    """Where an entry's data starts: after its local header, name and extra field."""
    with apk.open("rb") as f:
        f.seek(info.header_offset)
        header = f.read(30)
    name_length, extra_length = struct.unpack("<HH", header[26:30])
    return info.header_offset + 30 + name_length + extra_length


def has_signing_block(apk: Path) -> bool:
    """APK Signature Scheme v2+ puts a block ending in its magic right before the central directory."""
    with zipfile.ZipFile(apk) as z:
        start = min((i.header_offset for i in z.infolist()), default=0)
    data = apk.read_bytes()
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0:
        return False
    central = struct.unpack("<I", data[eocd + 16:eocd + 20])[0]
    return central >= 24 + start and data[central - 16:central] == SIGNING_BLOCK_MAGIC


def check(apk: Path) -> list[str]:
    problems = []
    with zipfile.ZipFile(apk) as z:
        entries = {i.filename: i for i in z.infolist()}
        for name, stored in REQUIRED.items():
            info = entries.get(name)
            if not info:
                problems.append(f"missing {name}")
            elif stored is not None and info.compress_type != stored:
                problems.append(f"{name} must be stored uncompressed")
        for name, info in entries.items():
            if name.startswith("lib/") and name.endswith(".so"):
                if not name.startswith("lib/arm64-v8a/"):
                    problems.append(f"{name}: the APK carries arm64-v8a only")
                if info.compress_type != zipfile.ZIP_STORED:
                    problems.append(f"{name} must be stored uncompressed (extractNativeLibs=false)")
                elif data_offset(apk, info) % PAGE:
                    problems.append(f"{name} is not aligned to 16 KiB")
        for name in entries:
            parts = name.split("/")
            if len(parts) == 5 and parts[:3] == ["assets", "game", "lib"] and name.endswith(".so"):
                expected = GAME_ABIS.get(parts[3])
                header = z.read(name)[:20]
                found = (header[4], struct.unpack("<H", header[18:20])[0]) if len(header) == 20 else None
                if not expected:
                    problems.append(f"{name}: no game libraries for {parts[3]}")
                elif header[:4] != b"\x7fELF" or found != expected:
                    problems.append(f"{name} is not an {parts[3]} library")
        bad = z.testzip()
        if bad:
            problems.append(f"corrupt entry {bad}")
    if not has_signing_block(apk):
        problems.append("no APK signature block (v2/v3)")
    return problems


def google_checks(apk: Path, tools: Path, java_home: Path | None) -> list[str]:
    problems = []
    suffix = ".exe" if os.name == "nt" else ""
    java = (java_home / "bin" / f"java{suffix}") if java_home else Path("java")
    for name, command in (
            ("aapt2 badging", [tools / f"aapt2{suffix}", "dump", "badging", apk]),
            ("zipalign", [tools / f"zipalign{suffix}", "-c", "-P", "16", "4", apk]),
            ("apksigner", [java, "-jar", tools / "lib" / "apksigner.jar", "verify", apk])):
        result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
        if result.returncode:
            problems.append(f"{name} failed: {(result.stderr or result.stdout).strip()[:500]}")
        elif name == "aapt2 badging" and "package: name='org.venture_emu.venture'" not in result.stdout:
            problems.append("aapt2: the package is not org.venture_emu.venture")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--build-tools", type=Path)
    parser.add_argument("--java-home", type=Path)
    args = parser.parse_args()
    problems = check(args.apk)
    if args.build_tools:
        problems += google_checks(args.apk, args.build_tools, args.java_home)
    for problem in problems:
        print(f"check_apk: {problem}")
    if not problems:
        print(f"check_apk: {args.apk.name} passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
