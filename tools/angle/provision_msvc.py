#!/usr/bin/env python3
"""Extract a pinned portable Windows toolchain; no VS install or registry changes.

Running this accepts Microsoft's Build Tools license:
https://go.microsoft.com/fwlink/?LinkId=2327714
The upstream extractor uses msiexec /a (administrative extraction, not installation).
Its package hashes come from our SHA-256-pinned official Microsoft manifest.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import sys
import urllib.request

LOCK = Path(__file__).with_name("toolchain.lock.json")


def fetch(spec: dict, destination: Path) -> bytes:
    if destination.exists():
        data = destination.read_bytes()
    else:
        with urllib.request.urlopen(spec["url"], timeout=600) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != spec["sha256"]:
        raise RuntimeError(f"SHA-256 mismatch: {destination}")
    destination.write_bytes(data)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text())
    args.cache.mkdir(parents=True, exist_ok=True)
    cache = args.cache.resolve()
    marker = cache / "ready.json"
    lock_hash = hashlib.sha256(json.dumps(lock, sort_keys=True).encode()).hexdigest()
    recipe_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    expected = {"lock_sha256": lock_hash, "recipe_sha256": recipe_hash}
    if marker.exists() and json.loads(marker.read_text()) == expected:
        return
    if marker.exists() and json.loads(marker.read_text()).get("lock_sha256") != lock_hash:
        raise RuntimeError("Toolchain pin changed: use a new --cache directory")
    script = cache / "portable-msvc.py"
    fetch(lock["portable_msvc"], script)
    channel = fetch(lock["vs_channel"], cache / "channel.json")
    manifest = fetch(lock["vs_manifest"], cache / "manifest.json")
    # Redirect only the upstream moving discovery URL and its manifest to our
    # verified snapshot. Package payload URLs and hashes remain Microsoft's.
    urlopen = urllib.request.urlopen

    def pinned_urlopen(url, *a, **kw):
        if url == "https://aka.ms/vs/18/stable/channel":
            return io.BytesIO(channel)
        if url == lock["vs_manifest"]["url"]:
            return io.BytesIO(manifest)
        return urlopen(url, *a, **kw)

    urllib.request.urlopen = pinned_urlopen
    os.chdir(cache)
    sys.argv = [str(script), "--vs", lock["vs_channel_version"], "--msvc-version", lock["msvc_version"],
                "--sdk-version", lock["sdk_version"], "--target", "x64,x86", "--accept-license"]
    print(__doc__, flush=True)
    runpy.run_path(str(script), run_name="__main__")
    # GN invokes vcvarsall; the portable extractor only emits setup_x64.bat.
    # This shim is local to the extracted toolchain and changes no global env.
    (cache / "msvc/VC/Auxiliary/Build/vcvarsall.bat").write_text(
        '@echo off\n'
        'if "%1"=="amd64" (call "%~dp0../../../setup_x64.bat" & exit /b 0)\n'
        'if "%1"=="amd64_x86" (call "%~dp0../../../setup_x86.bat" & exit /b 0)\n'
        'exit /b 1\n', encoding="ascii")
    marker.write_text(json.dumps(expected, indent=2) + "\n")


if __name__ == "__main__":
    main()
