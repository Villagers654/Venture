#!/usr/bin/env python3
"""Play on an Android headset, end to end on the local emulator
(docs/design/native-headsets.md, section 8): Venture imports a small
Quest-style test game, adapts it for the device, signs it with the game key,
installs it through PackageInstaller and starts it. The game then loads
Venture's Platform loader and VrApi and calls into them.

  headset_play_test.py [--build-dir build/windows-android-arm64] [--abi ABI ...]

Needs the emulator running (tools/android/local_emulator.py start) and the
APK tree built with the test games:

  py -3 tools/worktree_build.py --build-dir build/windows-android-arm64 build venture_test_game_apks

The test runs the arm64-v8a game, and the armeabi-v7a game when the device
lists armeabi-v7a among its 32-bit ABIs (ro.product.cpu.abilist32). Without
that ABI, playing the 32-bit game must fail with the message for a CPU with
no 32-bit mode.

Venture takes its arguments from /data/local/tmp/venture-args, one per line,
when adb's shell user owns the file. The test games and the runtime
description go to /data/local/tmp/venture: from Android 11 on, the shell
cannot use Venture's external files folder.

The emulator has no OpenXR runtime, so --smoke-headset makes the probe report
an OpenXR 1.0 runtime with OpenGL ES. The game's vrapi_Initialize then fails
cleanly, because no runtime answers. The test clears Venture's data first, so
it runs only on an emulator.

--direct is for an emulator image that runs a 32-bit game but not the Venture
app: the API 30 image (local_emulator.py --api 30), whose ARM translation is
the only one Google ships with armeabi-v7a, cannot run the arm64 launcher. Each
game is adapted on the PC instead, by venture_adapt_game (tests/android,
built in the PC tree) with the code Play runs, for the device's ABIs, signed
with the development key, installed with adb and started; the game checks are
the same, without the Venture app's part or the debug report.

For each game the test drives the launcher's smoke options: --smoke-import
then --smoke-play. It taps Install in the system's confirmation, then checks:
- the game is installed for the expected ABI;
- the game's log lines show Venture's libraries answering;
- the game's own Venture logs are in Android/obb/<package>/venture;
- a debug report (--smoke-debug-report log, written to logcat) shows the
  headset check, the game's plan and install state, and both logs.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.android.local_emulator import adb as adb_run  # noqa: E402

VENTURE = "org.venture_emu.venture"
FILES = "/data/local/tmp/venture"
ARGS = "/data/local/tmp/venture-args"
SUFFIX = {"arm64-v8a": "a64", "armeabi-v7a": "a32"}
NO_A32 = "this headset's CPU has no 32-bit mode"
# The emulator has no OpenXR runtime; the probe reports this one instead
# (--smoke-headset), an OpenXR 1.0 runtime with OpenGL ES, as many headsets'.
RUNTIME = ('{"runtime": "Emulated OpenXR runtime 1.0.0", "api_minor": 0, '
           '"extensions": {"XR_KHR_opengl_es_enable": 10, "XR_KHR_android_create_instance": 3}}')


def adb(*args: str, check: bool = True) -> str:
    result = adb_run(*args)
    if check and result.returncode:
        sys.exit(f"headset_play_test: adb {' '.join(args)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def shell(command: str, check: bool = True) -> str:
    return adb("shell", command, check=check)


def logcat() -> str:
    return adb("logcat", "-d", "-v", "brief")


def wait_for(what: str, test, seconds: float, step: float = 2.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = test()
        if value:
            return value
        time.sleep(step)
    sys.exit(f"headset_play_test: timed out waiting for {what}")


def run_venture(arguments: list[str]) -> None:
    shell(f"am force-stop {VENTURE}")
    lines = "\\n".join(arguments)
    shell(f"printf '{lines}\\n' > {ARGS} && chmod 644 {ARGS}")
    adb("logcat", "-c")
    shell(f"am start -n {VENTURE}/.VentureActivity")


def tap_install() -> bool:
    """Taps Install (or Update) in the system's install confirmation, if shown."""
    dump = shell("uiautomator dump /sdcard/venture-ui.xml >/dev/null && cat /sdcard/venture-ui.xml", check=False)
    for node in re.finditer(r"<node [^>]*>", dump):
        text = node.group(0)
        label = re.search(r' text="([^"]*)"', text)
        if not label or label.group(1).upper() not in ("INSTALL", "UPDATE"):
            continue
        bounds = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', text)
        if not bounds or 'clickable="true"' not in text:
            continue
        x1, y1, x2, y2 = map(int, bounds.groups())
        shell(f"input tap {(x1 + x2) // 2} {(y1 + y2) // 2}")
        return True
    return False


def game_checks(abi: str, package: str, log: str) -> list[str]:
    """The installed game's ABI, its log lines and its Venture logs."""
    problems: list[str] = []
    game = [line for line in log.splitlines() if "VentureTestGame" in line]
    installed_abi = re.search(r"primaryCpuAbi=(\S+)", shell(f"dumpsys package {package}", check=False))
    if not installed_abi:
        return [f"{abi}: not installed; the log ends with:\n" + "\n".join(log.splitlines()[-30:])]
    if installed_abi.group(1) != abi:
        problems.append(f"{abi}: installed for {installed_abi.group(1)}")
    if not any(f"done {abi}" in line for line in game):
        problems.append(f"{abi}: the game did not finish its calls; its log:\n" + "\n".join(game))
    for needed in ("platform: loaded, ovr_PlatformInitializeAndroid found", "platform: ovr_PlatformInitializeAndroid returned",
                   "vrapi: loaded, version", "vrapi: vrapi_Initialize returned"):
        if not any(needed in line for line in game):
            problems.append(f"{abi}: no '{needed}' in the game's log")
    # From Android 11 on the shell may not list the OBB folder; the debug
    # report shows the logs either way.
    listing = shell(f"ls /sdcard/Android/obb/{package}/venture 2>&1", check=False)
    if "Permission denied" not in listing:
        for name in ("platform.log", "vrapi.log"):
            if name not in listing.split():
                problems.append(f"{abi}: no {name} in the game's OBB folder (found: {listing.strip() or 'nothing'})")
    for line in game:
        print(f"  {line}")
    return problems


def sign(unsigned: Path, signed: Path, build_dir: Path) -> None:
    """Signs a game adapted on the PC with the development key, as the APK
    build signs (tools/android/build_apk.py)."""
    from tools.android.build_apk import exe, signing_key
    tools = json.loads((build_dir / "test_game" / "tools.json").read_text())
    java_bin = Path(tools["java_home"]) / "bin"
    store, password, alias = signing_key(argparse.Namespace(keystore=None), java_bin)
    subprocess.run([str(exe(java_bin, "java")), "-jar", str(Path(tools["build_tools"]) / "lib" / "apksigner.jar"),
                    "sign", "--ks", str(store), "--ks-pass", "env:VENTURE_APK_SIGN_PASS", "--ks-key-alias", alias,
                    "--min-sdk-version", "29", "--out", str(signed), str(unsigned)],
                   env={**os.environ, "VENTURE_APK_SIGN_PASS": password}, check=True)


def direct(abi: str, apk: Path, venture_apk: Path, args) -> list[str]:
    """--direct: the game adapted on the PC by venture_adapt_game (the code Play
    runs on the headset) for this device, installed with adb and started."""
    package = f"org.venture_emu.testgame.{SUFFIX[abi]}"
    tool = args.host_build_dir / "bin" / ("venture_adapt_game.exe" if os.name == "nt" else "venture_adapt_game")
    if not tool.exists():
        sys.exit(f"headset_play_test: {tool} is missing: build venture_adapt_game in the PC tree")
    work = args.build_dir / "test_game" / "direct"
    work.mkdir(parents=True, exist_ok=True)
    unsigned, signed = work / f"{abi}-unsigned.apk", work / f"{abi}.apk"
    abis = shell("getprop ro.product.cpu.abilist").strip()
    api = shell("getprop ro.build.version.sdk").strip()
    print(f"headset_play_test: {abi}: adapting on the PC for ABIs {abis}, API {api}")
    adapted = subprocess.run([str(tool), str(apk), str(venture_apk), str(unsigned), "--abis", abis, "--api", api,
                              "--xr-minor", "0", "--ext", "XR_KHR_opengl_es_enable:10",
                              "--ext", "XR_KHR_android_create_instance:3"], capture_output=True, text=True)
    if adapted.returncode:
        return [f"{abi}: adapting failed: {adapted.stderr.strip()}"]
    if f"abi={abi}" not in adapted.stdout:
        return [f"{abi}: the plan picked {adapted.stdout.strip()}"]
    sign(unsigned, signed, args.build_dir)
    shell(f"pm uninstall {package}", check=False)
    adb("install", str(signed))
    adb("logcat", "-c")
    print(f"headset_play_test: {abi}: starting the game")
    shell(f"monkey -p {package} -c android.intent.category.LAUNCHER 1")
    wait_for("the game", lambda: re.search(rf"done {abi}|Fatal signal|FATAL EXCEPTION", logcat()), 120)
    return game_checks(abi, package, logcat())


def play(abi: str, apk: Path, a32: bool) -> list[str]:
    package = f"org.venture_emu.testgame.{SUFFIX[abi]}"
    remote = f"{FILES}/{apk.name}"
    problems: list[str] = []
    shell(f"pm uninstall {package}", check=False)
    shell(f"rm -rf /sdcard/Android/obb/{package}", check=False)
    adb("push", str(apk), remote)
    shell(f"chmod 644 {remote}")

    print(f"headset_play_test: {abi}: importing")
    run_venture(["--smoke-import", remote, "--smoke-import-start"])
    imported = wait_for("the import", lambda: re.search(r"Smoke import (completed|failed.*)", logcat()), 180)
    if imported.group(1) != "completed":
        return [f"{abi}: {imported.group(0)}"]

    print(f"headset_play_test: {abi}: playing")
    run_venture(["--smoke-headset", f"{FILES}/runtime.json", "--smoke-play", "--smoke-select", package,
                 "--smoke-seconds", "600"])
    expect_run = abi != "armeabi-v7a" or a32
    end = time.monotonic() + 300
    tapped = False
    while time.monotonic() < end:
        log = logcat()
        if f"done {abi}" in log:
            break
        if NO_A32 in log:
            break
        if re.search(r"Venture's installer failed|The install failed", log):
            break
        if not tapped and tap_install():
            tapped = True
            print(f"headset_play_test: {abi}: confirmed the install")
        time.sleep(2)
    log = logcat()
    if not expect_run:
        if NO_A32 not in log:
            problems.append(f"{abi}: the device has no 32-bit ABI, and Play did not say so")
        if shell(f"pm path {package}", check=False).strip():
            problems.append(f"{abi}: installed although the device cannot run it")
        return problems

    problems += game_checks(abi, package, log)
    if any("not installed" in problem for problem in problems):
        return problems

    # The debug report's headset check, with the game's Venture logs.
    print(f"headset_play_test: {abi}: debug report")
    run_venture(["--smoke-headset", f"{FILES}/runtime.json", "--smoke-select", package,
                 "--smoke-debug-report", "log"])
    done = wait_for("the debug report", lambda: re.search(r"Smoke debug report (written|failed.*)", logcat()), 300)
    if done.group(1) != "written":
        return problems + [f"{abi}: {done.group(0)}"]
    marker = "[report] | "
    report = "\n".join(line.split(marker, 1)[1] for line in logcat().splitlines() if marker in line)
    for needed in ("=== Headset ===", "OpenXR: Emulated OpenXR runtime", f"({package})",
                   f"Runs: natively, {abi}", "Replaced: libvrapi.so with Venture's VrApi",
                   "adapted by this Venture: yes", "=== platform.log", "=== vrapi.log",
                   "Venture's Platform loader is attached", f"vrapi for {abi}"):
        if needed not in report:
            problems.append(f"{abi}: the debug report lacks '{needed}'")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=Path("build/windows-android-arm64"))
    parser.add_argument("--abi", action="append", choices=sorted(SUFFIX), help="default: both")
    parser.add_argument("--direct", action="store_true",
                        help="adapt each game on the PC and install it with adb, without the Venture app")
    parser.add_argument("--host-build-dir", type=Path, default=Path("build/windows-agent"),
                        help="the PC tree with venture_adapt_game (--direct)")
    args = parser.parse_args()

    apks = sorted((args.build_dir / "apk").glob("venture-*-android-arm64.apk"))
    if not apks:
        sys.exit(f"headset_play_test: no Venture APK in {args.build_dir / 'apk'}")
    games = {abi: args.build_dir / "test_game" / f"testgame-{abi}.apk" for abi in SUFFIX}
    for abi, apk in games.items():
        if not apk.exists():
            sys.exit(f"headset_play_test: {apk} is missing: build venture_test_game_apks")
    if "device" not in adb("get-state", check=False):
        sys.exit("headset_play_test: no emulator: run tools/android/local_emulator.py start")

    abilist32 = shell("getprop ro.product.cpu.abilist32").strip().split(",")
    a32 = "armeabi-v7a" in abilist32
    print(f"headset_play_test: device ABIs {shell('getprop ro.product.cpu.abilist').strip()}, "
          f"32-bit {','.join(abilist32) or 'none'}")

    problems: list[str] = []
    if args.direct:
        for abi in args.abi or ["arm64-v8a", "armeabi-v7a"]:
            problems += direct(abi, games[abi], apks[-1], args)
        for problem in problems:
            print(f"headset_play_test: FAIL {problem}")
        if not problems:
            print("headset_play_test: passed")
        return 1 if problems else 0

    adb("install", "-r", str(apks[-1]))
    # A fresh Venture: its data is cleared, so this runs only on an emulator.
    if "1" not in (shell("getprop ro.boot.qemu", check=False) + shell("getprop ro.kernel.qemu", check=False)):
        sys.exit("headset_play_test: the device is not an emulator; this test clears Venture's data")
    shell(f"pm clear {VENTURE}")
    shell(f"appops set {VENTURE} REQUEST_INSTALL_PACKAGES allow")
    shell(f"rm -rf {FILES} && mkdir -p {FILES} && chmod 755 {FILES}")
    shell(f"echo '{RUNTIME}' > {FILES}/runtime.json && chmod 644 {FILES}/runtime.json")
    try:
        for abi in args.abi or ["arm64-v8a", "armeabi-v7a"]:
            problems += play(abi, games[abi], a32)
    finally:
        shell(f"rm -rf {ARGS} {FILES}", check=False)
        shell(f"am force-stop {VENTURE}", check=False)
    for problem in problems:
        print(f"headset_play_test: FAIL {problem}")
    if not problems:
        print("headset_play_test: passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
