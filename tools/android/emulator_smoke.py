#!/usr/bin/env python3
"""Install the Venture APK on a running emulator or device (adb), start it and
check that the launcher comes up: SDL runs Venture's main, the process stays
alive, and nothing of Venture's crashes. Leaves logcat and a screenshot in
--out for the CI artifact.

  emulator_smoke.py FILE.apk [--out DIR] [--seconds N]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

PACKAGE = "org.venture_emu.venture"
ACTIVITY = f"{PACKAGE}/.VentureActivity"


def adb(*args: str, check: bool = True, binary: bool = False):
    result = subprocess.run(["adb", *args], capture_output=True, text=not binary)
    if check and result.returncode:
        sys.exit(f"emulator_smoke: adb {' '.join(args)} failed: {result.stderr}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--out", type=Path, default=Path("smoke"))
    parser.add_argument("--seconds", type=int, default=45)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    adb("wait-for-device")
    adb("install", "-r", "-g", str(args.apk))
    adb("logcat", "-c")
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    deadline = time.monotonic() + args.seconds
    started = alive = False
    while time.monotonic() < deadline:
        time.sleep(3)
        log = adb("logcat", "-d", "-v", "threadtime", check=False)
        started = started or "Running main function SDL_main from library libventure.so" in log
        alive = bool(adb("shell", "pidof", PACKAGE, check=False).strip())
        if not alive or (started and time.monotonic() > deadline - args.seconds / 2):
            break
    log = adb("logcat", "-d", "-v", "threadtime", check=False)
    (args.out / "logcat.txt").write_text(log, encoding="utf-8")
    (args.out / "screen.png").write_bytes(adb("exec-out", "screencap", "-p", check=False, binary=True))

    problems = []
    if not started:
        problems.append("SDL never ran Venture's main")
    if not alive:
        problems.append("the Venture process is gone")
    crash_lines = [line for line in log.splitlines()
                   if ("FATAL EXCEPTION" in line or "Fatal signal" in line or "UnsatisfiedLinkError" in line)]
    if any(PACKAGE in line or "venture" in line.lower() for line in crash_lines) or \
            f">>> {PACKAGE} <<<" in log:
        problems.append("Venture crashed (see logcat.txt)")
    for problem in problems:
        print(f"emulator_smoke: {problem}")
    if not problems:
        print("emulator_smoke: the launcher started and kept running")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
