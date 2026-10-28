#!/usr/bin/env python3
"""Convert/verify APK bytecode. Exit 2 means the cache is NOT launchable; see report.json.

python tools/bootstrap.py --jdk
python tools/dex/convert_apk.py base.apk --package com.example --cache CACHE --framework framework.jar
No APK is modified, no application static initializer is run, and no missing class is synthesized.

Build/development entry only: it compiles the tools from this checkout and runs
the Java driver (com.venture.dex.ApkCache) that launches use without Python.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from cache_lock import exclusive_file, filesystem_path

ROOT = Path(__file__).absolute().parents[2]
DEPS = ROOT / ".deps"


def java_tool(name):
    return java_path(DEPS / "jdk/bin" / (name + (".exe" if os.name == "nt" else "")))


def java_path(path):
    # HotSpot/JDK file APIs support long ordinary paths, but the launcher
    # cannot derive java.home from a device-prefixed executable pathname.
    # Keep the Win32 prefix on Python's file operations only.
    path = str(path)
    if os.name == "nt" and path.startswith("\\\\?\\"):
        return "\\\\" + path[8:] if path[4:8].upper() == "UNC\\" else path[4:]
    return path


def java_environment():
    # Verification/compiler options come from this pipeline, not launcher-wide
    # Java option injection (which could change the verifier or its class path).
    return {k: v for k, v in os.environ.items()
            if k.upper() not in ("JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS", "CLASSPATH")}


def build_tools():
    with exclusive_file(DEPS / "dex-tools/.build.lock"):
        return _build_tools()


def _build_tools():
    sources = sorted(filesystem_path(ROOT / "tools/dex/rewriter").glob("*.java"))
    sources += sorted(filesystem_path(ROOT / "src/runtime/java/bridge").rglob("*.java"))
    digest = hashlib.sha256(b"".join(p.read_bytes() for p in sources)).hexdigest()
    output = filesystem_path(DEPS / "dex-tools" / digest)
    if (output / "ready").exists():
        return output
    output.mkdir(parents=True, exist_ok=True)
    compile_cp = os.pathsep.join([java_path(DEPS / "asm/asm.jar"), java_path(DEPS / "dex2jar/lib/*")])
    subprocess.run([java_tool("javac"), "-encoding", "UTF-8", "--release", "21", "-cp", compile_cp, "-d", java_path(output), *map(java_path, sources)], check=True, env=java_environment())
    subprocess.run([java_tool("jar"), "--create", "--file", java_path(output / "venture-java-bridge.jar"), "-C", java_path(output), "com/venture/runtime"], check=True, env=java_environment())
    (output / "ready").write_text(digest)
    return output


def driver_command(tool_dir, arguments):
    """The launch-path driver; its JDK/ASM/dex2jar root is two levels above tool_dir."""
    return [java_tool("java"), "-cp", java_path(tool_dir), "com.venture.dex.ApkCache", *arguments]


def convert(apk, package, cache, framework=(), force=False, tool_dir=None, hierarchy=()):
    """Returns (generation directory, report) like the runtime's subprocess entry."""
    arguments = [java_path(filesystem_path(apk)), "--package", package, "--cache", java_path(filesystem_path(cache))]
    for path in framework:
        arguments += ["--framework", java_path(filesystem_path(path))]
    for path in hierarchy:
        arguments += ["--hierarchy", java_path(filesystem_path(path))]
    if force:
        arguments.append("--force")
    with tempfile.TemporaryDirectory(prefix="venture-convert-") as temp:
        result_file = Path(temp) / "result.json"
        process = subprocess.run(driver_command(tool_dir or build_tools(), [*arguments, "--result-file", str(result_file)]),
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", env=java_environment())
        if process.returncode not in (0, 2) or not result_file.exists():
            raise RuntimeError(f"APK conversion failed ({process.returncode}): {process.stderr.strip() or process.stdout.strip()}")
        result = json.loads(result_file.read_text(encoding="utf-8"))
    dest = filesystem_path(result["jar"]).parent
    return dest, json.loads((dest / "report.json").read_text(encoding="utf-8"))


def main():
    return subprocess.run(driver_command(build_tools(), sys.argv[1:]), env=java_environment()).returncode


if __name__ == "__main__":
    sys.exit(main())
