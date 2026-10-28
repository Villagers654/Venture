#!/usr/bin/env python3
"""Provision the pinned build toolchain into <repo>/.deps (user-level, no admin).

  python tools/bootstrap.py            # provision everything that is missing
  python tools/bootstrap.py --print-env  # print PATH additions for the toolchain
  python tools/bootstrap.py --ndk        # also provision the Android guest toolchain

Toolchains are downloaded once from their pinned URLs, verified by SHA-256 and
extracted into a Venture-owned shared cache (%LOCALAPPDATA%/Venture/dev-cache
/toolchains); each worktree's .deps/<name> is a directory junction to it.
NEVER delete a worktree or .deps with a tool that follows junctions (robocopy
/MIR, PowerShell 5.1 Remove-Item -Recurse, git worktree remove on Windows): use
tools/remove_worktree.py, which unlinks junctions first.
The optional NDK uses Google's published SHA-1 and a shared, verified zip cache.
"""
from __future__ import annotations
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.venture_paths import data_dir


import argparse
import contextlib
import hashlib
import io
import os
import shutil
import subprocess
import time
import tempfile
import urllib.request
import zipfile
import tarfile
import json

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / ".deps"
DOWNLOAD_CACHE = data_dir() / "dev-cache"
TOOLCHAINS = DOWNLOAD_CACHE / "toolchains"

# Temurin binaries include GPLv2 + Classpath Exception and third-party notices.
# Keep legal/ and NOTICE when redistributing, including jlink images.
JDK_VERSION = "21.0.12.1+1"
JAVA_DEPENDENCIES = {
    "jdk": {
        "url": "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip",
        "sha256": "f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e",
        "strip": "jdk-21.0.12.1+1", "bin": "bin",
    },
    "dex2jar": {
        "url": "https://github.com/pxb1988/dex2jar/releases/download/v2.4/dex-tools-v2.4.zip",
        "sha256": "ee7c45eb3c1d2474a6145d8d447e651a736a22d9664b6d3d3be5a5a817dda23a",
        "strip": "dex-tools-v2.4", "bin": "",
    },
    "asm": {
        "url": "https://repo.maven.apache.org/maven2/org/ow2/asm/asm/9.8/asm-9.8.jar",
        "sha256": "876eab6a83daecad5ca67eb9fcabb063c97b5aeb8cf1fca7a989ecde17522051",
        "file": "asm.jar", "bin": "",
    },
}
if os.name != "nt":
    JAVA_DEPENDENCIES["jdk"].update(
        url="https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz",
        sha256="ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94")

DEPENDENCIES = {
    "llvm-mingw": {
        "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip",
        "sha256": "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666",
        "strip": "llvm-mingw-20260922-ucrt-x86_64",
        "shared": "llvm-mingw-20260922-ucrt-x86_64",
        "bin": "bin",
    },
    "cmake": {
        "url": "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip",
        "sha256": "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab",
        "strip": "cmake-4.4.3-windows-x86_64",
        "shared": "cmake-4.4.3-windows-x86_64",
        "bin": "bin",
    },
    "ninja": {
        "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
        "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65",
        "strip": "",
        "shared": "ninja-1.13.2",
        "bin": "",
    },
}
if os.name != "nt":
    # Linux uses the host's native Clang (docs/HOST_COMPILER.md); CMake and
    # Ninja stay pinned. SHA-256 from Kitware's signed list / GitHub release API.
    del DEPENDENCIES["llvm-mingw"]
    DEPENDENCIES["cmake"].update(
        url="https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-linux-x86_64.tar.gz",
        sha256="d6c83076c575bc00b823522ac974bda66d0af05d6ddc30e739c12385cf32c6cc",
        strip="cmake-4.4.3-linux-x86_64", shared="cmake-4.4.3-linux-x86_64")
    DEPENDENCIES["ninja"].update(
        url="https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip",
        sha256="5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6",
        shared="ninja-1.13.2-linux")

# Official ccache 4.14.1 release assets; SHA-256 from the upstream release API.
# https://github.com/ccache/ccache/releases/tag/v4.14.1
CCACHE = {
    "url": "https://github.com/ccache/ccache/releases/download/v4.14.1/ccache-4.14.1-windows-x86_64.zip",
    "sha256": "6219f3865ca59aec41ee4b678df171d5d35855ecb2b6dbbbd20690b3a68af7b4",
    "strip": "ccache-4.14.1-windows-x86_64", "bin": "",
    "shared": "ccache-4.14.1-windows-x86_64",
}
if os.name != "nt":
    CCACHE.update(
        url="https://github.com/ccache/ccache/releases/download/v4.14.1/ccache-4.14.1-linux-x86_64-musl-static.tar.gz",
        sha256="8f684cf82a7acd2f1daddbefcda505c499afe8245cfbfe78f751ce389cd6df52",
        strip="ccache-4.14.1-linux-x86_64-musl-static", bin="bin",
        shared="ccache-4.14.1-linux-x86_64-musl-static")

# Latest stable LTS, verified against https://developer.android.com/ndk/downloads
# on 2026-10-08. Google publishes SHA-1 for this archive; verify every cache hit.
NDK = {
    "url": "https://dl.google.com/android/repository/android-ndk-r30-windows.zip",
    "sha1": "9bf167a1985fa7d4a036186b78f702eab9179408",
    "size": 727980788,
    "strip": "android-ndk-r30",
    "revision": "30.0.16248370",
    "bin": "toolchains/llvm/prebuilt/windows-x86_64/bin",
}
if os.name != "nt":
    NDK.update(url="https://dl.google.com/android/repository/android-ndk-r30-linux.zip",
               sha1="5107f898313790e449e87eee2183d9a20602dee9", size=738633529,
               bin="toolchains/llvm/prebuilt/linux-x86_64/bin")
NDK_CACHE = data_dir() / 'dev-cache'


@contextlib.contextmanager
def cache_lock(path: Path):
    """Serialize downloads across worktrees; OS releases the lock after a crash."""
    with path.open("a+b") as lock:
        lock.seek(0)
        lock.write(b"0")
        lock.flush()
        if os.name == "nt":
            import msvcrt
            deadline = time.monotonic() + 1800
            while True:
                try:
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"waiting for dependency cache lock: {path}")
                    time.sleep(1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)


def extract_zip(archive: zipfile.ZipFile, destination: Path) -> None:
    """zipfile drops Unix modes and symlinks; restore both on POSIX."""
    if os.name == "nt":
        archive.extractall(destination)
        return
    for info in archive.infolist():
        mode = info.external_attr >> 16
        target = destination / info.filename
        if (mode & 0o170000) == 0o120000:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(archive.read(info).decode())
            continue
        archive.extract(info, destination)
        if mode & 0o111 and not info.is_dir():
            target.chmod(mode & 0o777)


def verify_ndk_zip(path: Path) -> None:
    with path.open("rb") as archive:
        digest = hashlib.file_digest(archive, "sha1").hexdigest()
    if path.stat().st_size != NDK["size"] or digest != NDK["sha1"]:
        raise ValueError(f"[android-ndk] archive verification failed: {path} (SHA-1 {digest})")


def provision_ndk() -> Path:
    destination = DEPS / "android-ndk"
    if destination.exists():
        properties = (destination / "source.properties").read_text(encoding="utf-8")
        if f"Pkg.Revision = {NDK['revision']}" not in properties:
            raise ValueError(f"{destination} is not pinned NDK {NDK['revision']}")
        return destination
    unlink_dangling_dependency(destination)
    NDK_CACHE.mkdir(parents=True, exist_ok=True)
    archive_path = NDK_CACHE / NDK["url"].rsplit("/", 1)[1]
    with cache_lock(archive_path.with_suffix(".lock")):
        if not archive_path.exists():
            partial = archive_path.with_suffix(".zip.partial")
            print(f"[android-ndk] downloading {NDK['url']}", flush=True)
            try:
                with urllib.request.urlopen(NDK["url"], timeout=600) as response, partial.open("wb") as output:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
                verify_ndk_zip(partial)
                partial.replace(archive_path)
            finally:
                partial.unlink(missing_ok=True)
        else:
            verify_ndk_zip(archive_path)
            print(f"[android-ndk] verified cached {archive_path}", flush=True)
    staging = destination.with_name("android-ndk.staging")
    # Never follow a pre-existing staging junction outside this worktree.
    if not DEPS.resolve().is_relative_to(ROOT.resolve()) or not staging.resolve().is_relative_to(DEPS.resolve()):
        raise ValueError(f"NDK staging directory escapes the worktree: {staging}")
    shutil.rmtree(staging, ignore_errors=True)
    print(f"[android-ndk] extracting to {destination}", flush=True)
    try:
        with zipfile.ZipFile(archive_path) as archive:
            extract_zip(archive, staging)
        (staging / NDK["strip"]).rename(destination)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return destination


def provision_platform_sdk() -> None:
    """Optional, hash-pinned inputs for the checked-in Platform ABI generator."""
    source_dir = ROOT / "src/runtime/hle/ovrplatform"
    pins = json.loads((source_dir / "sdk_sources.json").read_text())
    cache = data_dir() / 'dev-cache'
    cache.mkdir(parents=True, exist_ok=True)
    for package in pins["packages"]:
        version = package["version"]
        name = f"com.meta.xr.sdk.platform-{version}.tgz"
        archive = cache / name
        if not archive.exists():
            url = f"https://npm.developer.oculus.com/com.meta.xr.sdk.platform/-/{name}"
            with urllib.request.urlopen(url, timeout=120) as response:
                archive.write_bytes(response.read())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != package["sha256"]:
            sys.exit(f"Platform SDK {version}: SHA-256 mismatch")
        destination = DEPS / f"ovrplatform-{version}"
        with tarfile.open(archive, "r:gz") as bundle:
            members = [m for m in bundle.getmembers() if m.isfile() and m.name.endswith((".cs", ".h", ".json"))]
            bundle.extractall(destination, members=members, filter="data")
    for source in pins["inputs"]:
        destination = DEPS / source["path"]
        if not destination.exists() and source["path"].startswith("ovrplatform-old/"):
            destination.parent.mkdir(parents=True, exist_ok=True)
            url = pins["native_repository"].replace("github.com", "raw.githubusercontent.com")
            url += "/" + pins["native_revision"] + "/Include/" + destination.name
            with urllib.request.urlopen(url, timeout=30) as response:
                blob = response.read()
            if hashlib.sha256(blob).hexdigest() != source["sha256"]:
                sys.exit(f"Platform SDK {destination.name}: SHA-256 mismatch")
            destination.write_bytes(blob)
        if hashlib.sha256(destination.read_bytes()).hexdigest() != source["sha256"]:
            sys.exit(f"Platform SDK {destination.name}: SHA-256 mismatch")
    print("Platform SDK declarations verified")


def unlink_dangling_dependency(destination: Path) -> None:
    if destination.exists() or not os.path.lexists(destination):
        return
    # A migrated cache can leave a dangling junction. Remove the point only;
    # never traverse its target or replace a real directory/file.
    if destination.is_symlink():
        destination.unlink()
    elif os.name == "nt" and destination.is_junction():
        os.rmdir(destination)
    else:
        raise RuntimeError(f"Refusing to replace non-link dependency: {destination.name}")


def junction(link: Path, target: Path) -> None:
    unlink_dangling_dependency(link)
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True,
                       stdout=subprocess.DEVNULL)
    else:
        link.symlink_to(target, target_is_directory=True)


def download(name: str, spec: dict, destination: Path) -> None:
    DOWNLOAD_CACHE.mkdir(parents=True, exist_ok=True)
    cached = DOWNLOAD_CACHE / spec["url"].rsplit("/", 1)[-1]
    if not cached.exists():
        print(f"[{name}] downloading {spec['url']}", flush=True)
        with tempfile.NamedTemporaryFile(dir=DOWNLOAD_CACHE, delete=False) as out:
            temporary = Path(out.name)
            try:
                with urllib.request.urlopen(spec["url"], timeout=600) as response:
                    shutil.copyfileobj(response, out)
            except BaseException:
                out.close()
                temporary.unlink(missing_ok=True)
                raise
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != spec["sha256"]:
            temporary.unlink()
            raise ValueError(f"[{name}] downloaded SHA-256 mismatch")
        os.replace(temporary, cached)
    blob = cached.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    if digest != spec["sha256"]:
        sys.exit(f"[{name}] SHA-256 mismatch: {digest}")
    staging = destination.with_name(destination.name + ".staging")
    # Check the final absolute deletion target, including parent junctions.
    allowed_roots = (DEPS.resolve(), TOOLCHAINS.resolve())
    if (staging.is_symlink() or staging.is_junction() or
        not any(staging.resolve().is_relative_to(root) for root in allowed_roots)):
        raise RuntimeError(f"[{name}] refusing staging cleanup outside dependency roots")
    shutil.rmtree(staging, ignore_errors=True)
    if "file" in spec:
        staging.mkdir(parents=True)
        (staging / spec["file"]).write_bytes(blob)
    elif spec["url"].endswith((".tar.gz", ".tar.xz")):
        with tarfile.open(fileobj=io.BytesIO(blob)) as archive:
            archive.extractall(staging, filter="data")
    else:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            extract_zip(archive, staging)
    source = staging / spec["strip"] if spec.get("strip") else staging
    source.rename(destination)
    shutil.rmtree(staging, ignore_errors=True)


def provision(name: str, spec: dict) -> Path:
    destination = DEPS / name
    if destination.exists():
        return destination
    unlink_dangling_dependency(destination)
    if "shared" in spec:
        shared = TOOLCHAINS / spec["shared"]
        if not shared.exists():
            TOOLCHAINS.mkdir(parents=True, exist_ok=True)
            download(name, spec, shared)
        print(f"[{name}] linking {shared}")
        junction(destination, shared)
        return destination
    download(name, spec, destination)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ccache", action="store_true", help="provision pinned optional agent compiler cache")
    parser.add_argument("--print-env", action="store_true")
    parser.add_argument("--ndk", action="store_true", help="also provision the pinned Android NDK")
    parser.add_argument("--platform-sdk", action="store_true", help="provision pinned Platform ABI generator inputs")
    parser.add_argument("--angle", action="store_true", help="build/reuse pinned ANGLE")
    parser.add_argument("--ffmpeg", action="store_true", help="explicitly rebuild pinned shared LGPL audio-only FFmpeg")
    parser.add_argument("--jdk", action="store_true", help="provision pinned JDK 21, dex2jar and ASM (Windows/Linux x64)")
    args = parser.parse_args()
    DEPS.mkdir(exist_ok=True)
    # The shared native CMake check cache is automatic for every configure,
    # including build directories created without a preset.
    probe_root = (Path(os.environ['LOCALAPPDATA']) / 'Venture/dev-cache/probes'
                  if os.name == 'nt' else
                  Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'Venture/dev-cache/probes')
    probe_root.mkdir(parents=True, exist_ok=True)
    dependencies = {**DEPENDENCIES, **(JAVA_DEPENDENCIES if args.jdk else {})}
    if args.ccache:
        dependencies["ccache"] = CCACHE
    paths = [provision(name, spec) / spec["bin"] for name, spec in dependencies.items()]
    if args.ndk:
        paths.append(provision_ndk() / NDK["bin"])
    if args.platform_sdk:
        provision_platform_sdk()
    if args.angle:
        subprocess.run([sys.executable, str(ROOT / "tools/angle/build_angle.py")], check=True)
    subprocess.run([sys.executable, str(ROOT / "tools/ffmpeg/build_ffmpeg.py"),
                    *(["--rebuild"] if args.ffmpeg else [])], check=True)
    if args.jdk:
        from bootstrap_framework import provision_resources
        provision_resources(ROOT)
    if args.print_env:
        print(os.pathsep.join(str(p) for p in paths))
    else:
        print("toolchain ready:", ", ".join([*dependencies, *(["android-ndk"] if args.ndk else [])]))



if __name__ == "__main__":
    main()
