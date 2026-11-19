#!/usr/bin/env python3
"""Run the Android emulator on this machine, user-level, for the APK smoke
(tools/android/emulator_smoke.py) without a device or the public CI.

Google's emulator, platform tools and an API 34 x86_64 Google APIs image (its
ARM translation runs arm64 apps) come from Google's SDK repository, pinned by
SHA-1 like cmake/AndroidSdk.cmake, into <data dir>/dev-cache/android-sdk. The
AVD lives beside them. Nothing is installed system-wide and no admin is needed;
acceleration is whatever the emulator finds usable (WHPX on Windows, KVM on
Linux). Only the Windows archives are pinned so far; on Linux the script stops
before downloading until their SHA-1s are added.

  local_emulator.py setup          download, check and extract the pins
  local_emulator.py accel          the emulator's acceleration check
  local_emulator.py start          boot headless (4 cores, 4 GB), wait for boot
  local_emulator.py stop           shut the emulator down (adb emu kill)
  local_emulator.py env            print the PATH entry with adb

--api 30 picks the API 30 x86_64 Google APIs image instead (its own AVD),
whose ARM translation also runs armeabi-v7a apps, for ARM32 games.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

REPO = "https://dl.google.com/android/repository/"
WINDOWS = sys.platform == "win32"

# (directory in the SDK, archive, SHA-1, top-level directory inside the archive)
PINS = {
    "emulator": (
        ("emulator", "emulator-windows_x64-16740729.zip", "4d7268aa7787c640ad892844e6c9a5f159e91ad3", "emulator")
        if WINDOWS else
        ("emulator", "emulator-linux_x64-16740729.zip", None, "emulator")),
    "platform-tools": (
        ("platform-tools", "platform-tools_r37.0.1-win.zip", "e03e78b1d80b396f1c3358e31251cb31740e1110", "platform-tools")
        if WINDOWS else
        ("platform-tools", "platform-tools_r37.0.1-linux.zip", None, "platform-tools")),
}
# Google APIs x86_64 images by API level (--api). 34's ARM translation runs
# arm64 apps only (ro.product.cpu.abilist32 is empty); 30's also runs
# armeabi-v7a apps, for ARM32 games (tools/android/headset_play_test.py).
IMAGES = {
    34: ("system-images/android-34/google_apis/x86_64", "sys-img/google_apis/x86_64-34_r14.zip",
         "e0f6c9a0691aa27bd597d0deb1bcfdc943ac8ca7", "x86_64"),
    30: ("system-images/android-30/google_apis/x86_64", "sys-img/google_apis/x86_64-30_r16.zip",
         "6ae21030eaadc041078444d3798e4b399f3e787d", "x86_64"),}
DEFAULT_API = 34


def avd_name(api: int) -> str:
    return "venture-smoke" if api == DEFAULT_API else f"venture-smoke-{api}"


def data_dir() -> Path:
    if os.environ.get("VENTURE_DATA_DIR"):
        return Path(os.environ["VENTURE_DATA_DIR"])
    if WINDOWS:
        return Path(os.environ["LOCALAPPDATA"]) / "Venture"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "venture"


SDK = data_dir() / "dev-cache" / "android-sdk"
EXE = ".exe" if WINDOWS else ""


def sha1(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name: str, api: int = DEFAULT_API) -> None:
    sub, archive, digest, top = IMAGES[api] if name == "system-image" else PINS[name]
    target = SDK / sub
    if (target / ".venture-pin").is_file() and (target / ".venture-pin").read_text() == archive:
        return
    if digest is None:
        sys.exit(f"local_emulator: no pinned SHA-1 for {archive} on this host")
    downloads = SDK / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    zpath = downloads / Path(archive).name
    if not zpath.is_file() or sha1(zpath) != digest:
        print(f"local_emulator: downloading {archive}", flush=True)
        part = zpath.with_suffix(".part")
        with urllib.request.urlopen(REPO + archive) as r, part.open("wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        if sha1(part) != digest:
            part.unlink()
            sys.exit(f"local_emulator: {archive} does not match its pinned SHA-1")
        part.replace(zpath)
    staging = SDK / (".extract-" + name)
    if staging.exists():
        shutil.rmtree(staging)
    print(f"local_emulator: extracting {archive}", flush=True)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(staging)
        if not WINDOWS:
            for info in z.infolist():
                mode = info.external_attr >> 16
                if mode:
                    os.chmod(staging / info.filename, mode & 0o7777)
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    (staging / top).replace(target)
    shutil.rmtree(staging)
    (target / ".venture-pin").write_text(archive)


def env() -> dict[str, str]:
    e = dict(os.environ)
    e["ANDROID_SDK_ROOT"] = e["ANDROID_HOME"] = str(SDK)
    e["ANDROID_AVD_HOME"] = str(SDK / "avd")
    e["ANDROID_EMULATOR_HOME"] = str(SDK / "home")
    e["PATH"] = str(SDK / "platform-tools") + os.pathsep + e.get("PATH", "")
    return e


def write_avd(api: int) -> None:
    name = avd_name(api)
    avd_home = SDK / "avd"
    avd = avd_home / f"{name}.avd"
    avd.mkdir(parents=True, exist_ok=True)
    (SDK / "home").mkdir(exist_ok=True)
    (avd_home / f"{name}.ini").write_text(f"avd.ini.encoding=UTF-8\npath={avd}\ntarget=android-{api}\n")
    (avd / "config.ini").write_text("\n".join([
        "AvdId=" + name, "avd.ini.displayname=" + name,
        "abi.type=x86_64", "hw.cpu.arch=x86_64", "hw.cpu.ncore=4", "hw.ramSize=4096",
        f"image.sysdir.1={IMAGES[api][0]}/",
        "tag.id=google_apis", "tag.display=Google APIs",
        "hw.lcd.width=1920", "hw.lcd.height=1080", "hw.lcd.density=240",
        "hw.gpu.enabled=yes", "hw.gpu.mode=swiftshader_indirect",
        "hw.keyboard=yes", "disk.dataPartition.size=8G", "fastboot.forceColdBoot=yes",
        ""]))


def adb(*args: str, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run([str(SDK / "platform-tools" / f"adb{EXE}"), *args],
                          capture_output=True, text=True, encoding="utf-8", errors="replace", env=env(),
                          check=check)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["setup", "accel", "start", "stop", "env"])
    parser.add_argument("--boot-seconds", type=int, default=600)
    parser.add_argument("--api", type=int, choices=sorted(IMAGES), default=DEFAULT_API,
                        help="the system image's API level (30 runs ARM32 apps too)")
    args = parser.parse_args()
    emulator = SDK / "emulator" / f"emulator{EXE}"

    if args.action == "setup":
        for name in PINS:
            fetch(name)
        fetch("system-image", args.api)
        write_avd(args.api)
        print(f"local_emulator: SDK ready in {SDK}")
        return 0
    if args.action == "env":
        print(SDK / "platform-tools")
        return 0
    if args.action == "accel":
        r = subprocess.run([str(emulator), "-accel-check"], capture_output=True, text=True, env=env())
        print(r.stdout + r.stderr)
        return r.returncode
    if args.action == "stop":
        adb("emu", "kill")
        return 0

    fetch("system-image", args.api)
    write_avd(args.api)
    log = (SDK / "emulator.log").open("w")
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0
    proc = subprocess.Popen([str(emulator), "-avd", avd_name(args.api), "-no-window", "-no-audio", "-no-boot-anim",
                             "-no-snapshot", "-gpu", "swiftshader_indirect", "-cores", "4", "-memory", "4096",
                             "-netdelay", "none", "-netspeed", "full"],
                            stdout=log, stderr=subprocess.STDOUT, env=env(), creationflags=flags)
    print(f"local_emulator: emulator pid {proc.pid}, log {SDK / 'emulator.log'}", flush=True)
    deadline = time.monotonic() + args.boot_seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            sys.exit(f"local_emulator: the emulator exited ({proc.returncode}); see emulator.log")
        if adb("shell", "getprop", "sys.boot_completed").stdout.strip() == "1":
            print("local_emulator: booted", flush=True)
            return 0
        time.sleep(5)
    sys.exit("local_emulator: no boot within the time limit")


if __name__ == "__main__":
    sys.exit(main())
