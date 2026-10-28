#!/usr/bin/env python3
"""Build Venture's pinned ANGLE Vulkan DLLs/shared libraries (Windows/Linux x64).

Windows provisions portable Microsoft Build Tools/SDK files and accepts their
license; compilation uses ANGLE's pinned clang-cl, never cl.exe. Nothing is
installed system-wide. See docs/BUILDING_ANGLE.md.
"""
from __future__ import annotations
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.venture_paths import data_dir

import argparse
import contextlib
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LOCK = json.loads((HERE / "toolchain.lock.json").read_text())
PIN = "d2e5e488da0c6215f8276e5b8d992a4a6ec20c32"
PATCHES = sorted((ROOT / "third_party/angle-patches").rglob("*.patch"))
ANGLE_URL = "https://chromium.googlesource.com/angle/angle.git"
DEPOT_URL = "https://chromium.googlesource.com/chromium/tools/depot_tools.git"


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(argv, *, cwd: Path, env=None, capture=False):
    argv = [str(a) for a in argv]
    print("+ " + subprocess.list2cmdline(argv), flush=True)
    return subprocess.run(argv, cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE if capture else None,
                          text=True).stdout


@contextlib.contextmanager
def cache_lock(path: Path):
    # OS locks are released even if a build crashes; no stale lock-file deletion.
    with path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        print(f"Waiting for exclusive ANGLE cache access: {path}", flush=True)
        if os.name == "nt":
            import msvcrt
            while True:
                try:
                    stream.seek(0)
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def checkout(path: Path, url: str, revision: str, env):
    if not (path / ".git").exists():
        path.mkdir(parents=True, exist_ok=True)
        run(["git", "init", path], cwd=path, env=env)
        run(["git", "remote", "add", "origin", url], cwd=path, env=env)
        run(["git", "fetch", "--depth", "1", "origin", revision], cwd=path, env=env)
        run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=path, env=env)
    actual = run(["git", "rev-parse", "HEAD"], cwd=path, env=env, capture=True).strip()
    if actual != revision:
        raise RuntimeError(f"{path}: expected {revision}, found {actual}; use a separate --cache")


def prepare(cache: Path, swiftshader: bool, env):
    depot = cache / "depot_tools"
    checkout(depot, DEPOT_URL, LOCK["depot_tools_commit"], env)
    link_install(ROOT / ".deps/angle-tools" / ("depot_tools-" + LOCK["depot_tools_commit"]), depot)
    shims = cache / "shims"
    shims.mkdir(exist_ok=True)
    if os.name == "nt":
        # Recent depot_tools assumes git.bat, but user-level Git provides git.exe.
        (shims / "git.bat").write_text('@git.exe %*\n', encoding="ascii")
    env["PATH"] = os.pathsep.join([str(shims), str(depot), env["PATH"]])
    env["DEPOT_TOOLS_UPDATE"] = "0"
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    # gclient's mirror mode fetches 10,000 commits even with --shallow. The
    # checkout itself is already in the shared cache, so no second mirror is needed.
    env.pop("GIT_CACHE_PATH", None)
    source = cache / "source"
    checkout(source, ANGLE_URL, PIN, env)
    custom_deps = {"third_party/VK-GL-CTS/src": None, "third_party/catapult": None}
    if not swiftshader:
        custom_deps["third_party/SwiftShader"] = None
    config = "solutions = " + repr([{
        "name": ".", "url": ANGLE_URL, "managed": False, "deps_file": "DEPS",
        "custom_deps": custom_deps,
        "custom_vars": {"checkout_angle_cl_deps": False, "checkout_angle_dawn_deps": False},
    }]) + "\n"
    (source / ".gclient").write_text(config)
    sync_key = hashlib.sha256((PIN + config).encode()).hexdigest()
    stamp = cache / "sync.json"
    if not stamp.exists() or json.loads(stamp.read_text()).get("key") != sync_key:
        gclient = depot / ("gclient.bat" if os.name == "nt" else "gclient")
        run([gclient, "sync", "--nohooks", "--shallow", "-j", "4"], cwd=source, env=env)
        stamp.write_text(json.dumps({"key": sync_key}) + "\n")

    if os.name == "nt":
        msvc_cache = cache / f"msvc-{LOCK['msvc_version']}-{LOCK['sdk_version']}"
        run([sys.executable, "-u", HERE / "provision_msvc.py", "--cache", msvc_cache],
            cwd=ROOT, env=env)
        msvc = msvc_cache / "msvc"
        link_install(ROOT / ".deps/angle-tools" / msvc_cache.name, msvc)
        env["GYP_MSVS_OVERRIDE_PATH"] = str(msvc)
        env["GYP_MSVS_VERSION"] = "2022"
        env["WINDOWSSDKDIR"] = str(msvc / "Windows Kits/10")
        # Only the rc hook is needed: DEPS already fetched clang, GN and Ninja.
        run([sys.executable, depot / "download_from_google_storage.py", "--no_resume",
             "--bucket", "chromium-browser-clang/rc", "-s",
             "build/toolchain/win/rc/win/rc.exe.sha1"], cwd=source, env=env)
    else:
        msvc = None
    run([sys.executable, "build/util/lastchange.py", "-o", "build/util/LASTCHANGE"],
        cwd=source, env=env)
    # Later patches may touch earlier hunks. Individual reverse checks then
    # fail even though a whole prefix is already applied. Match the tracked
    # tree against each exact prefix in a temporary index, and apply only the
    # remaining suffix. Refuse unrelated edits rather than resetting them.
    with tempfile.TemporaryDirectory(prefix="patch-prefix-", dir=cache) as temp:
        for repo in (source, source / "build"):
            series = [p for p in PATCHES if (p.parent.name == "chromium-build") == (repo == source / "build")]
            index = Path(temp) / ("angle.index" if repo == source else "build.index")
            verify_env = dict(env, GIT_INDEX_FILE=str(index))
            run(["git", "read-tree", "HEAD"], cwd=repo, env=verify_env)
            matched = None
            for count in range(len(series) + 1):
                if subprocess.run(["git", "-c", "core.safecrlf=false", "diff", "--quiet", "--ignore-submodules=all", "--"],
                                  cwd=repo, env=verify_env).returncode == 0:
                    matched = count
                if count < len(series):
                    run(["git", "apply", "--cached", series[count]], cwd=repo, env=verify_env)
            if matched is None:
                raise RuntimeError(f"{repo}: source is not an exact prefix of the pinned patches")
            for patch in series[matched:]:
                run(["git", "apply", "--check", patch], cwd=repo, env=env)
                run(["git", "apply", patch], cwd=repo, env=env)
    # Compare tracked source files with a pristine index plus exactly our
    # patches. Never publish unrelated local edits under the same cache key.
    with tempfile.TemporaryDirectory(prefix="verify-", dir=cache) as temp:
        for repo in (source, source / "build"):
            index = Path(temp) / ("angle.index" if repo == source else "build.index")
            verify_env = dict(env, GIT_INDEX_FILE=str(index))
            run(["git", "read-tree", "HEAD"], cwd=repo, env=verify_env)
            for patch in PATCHES:
                if (patch.parent.name == "chromium-build") == (repo == source / "build"):
                    run(["git", "apply", "--cached", patch], cwd=repo, env=verify_env)
            run(["git", "-c", "core.safecrlf=false", "diff", "--exit-code", "--ignore-submodules=all", "--"],
                cwd=repo, env=verify_env)
    return source, msvc


def gn_args(msvc: Path | None, swiftshader: bool) -> str:
    args = {
        "is_debug": False, "is_component_build": False, "symbol_level": 1,
        "target_cpu": "x64", "is_clang": True, "use_siso": False,
        "angle_enable_vulkan": True, "angle_enable_d3d11": False,
        "angle_use_custom_libvulkan": False,
        "angle_enable_gl": False, "angle_enable_null": False,
        "angle_enable_swiftshader": swiftshader, "angle_build_tests": False,
        "angle_enable_vulkan_validation_layers": False,
        "angle_enable_vulkan_api_dump_layer": False,
        "build_angle_deqp_tests": False, "enable_rust": False, "enable_rust_cxx": False,
    }
    if msvc:
        sdk = msvc / "Windows Kits/10"
        args.update(visual_studio_path=msvc.as_posix(), visual_studio_version="2022",
                    windows_sdk_path=sdk.as_posix(), windows_sdk_version=LOCK["sdk_full_version"],
                    wdk_path=sdk.as_posix())
    else:
        # Native Linux compiler toolchain with a headless Vulkan display. No
        # install-build-deps.sh or root-level package installation is performed.
        # Venture selects GPUs through Vulkan/OpenXR, so ANGLE needs no libpci.
        args.update(use_sysroot=False, use_libpci=False, angle_use_x11=False, angle_use_wayland=False,
                    angle_use_vulkan_display=True, angle_vulkan_display_mode="headless")
    return "".join(f"{key} = {json.dumps(value)}\n" for key, value in args.items())


def valid_artifact(path: Path, key: str) -> bool:
    manifest = path / "manifest.json"
    if not manifest.exists():
        return False
    try:
        data = json.loads(manifest.read_text())
    except (OSError, ValueError):
        return False
    suffix = ".dll" if os.name == "nt" else ".so"
    required = {"bin/libEGL" + suffix, "bin/libGLESv2" + suffix,
                "include/EGL/eglext_angle_venture.h", "LICENSE"}
    return data.get("key") == key and required <= data.get("files", {}).keys() and all(
        (path / name).is_file() and sha(path / name) == digest
        for name, digest in data["files"].items())


def reuse_legacy_artifact(cache: Path, swiftshader: bool) -> Path | None:
    """Verify the exact equivalent pre-rebrand patch stack, then stage locally.

    DLL ABI/layout and numeric EGL tokens did not change. The runtime accepts
    the legacy advertised extension; no shared cache artifact is edited.
    """
    legacy_lower = "questport"  # rebrand: legacy ANGLE patch/header names
    legacy_title = "QuestPort"  # rebrand: legacy ANGLE patch contents
    legacy_upper = "QUESTPORT"  # rebrand: legacy ANGLE patch contents
    def legacy_bytes(data):
        return data.replace(b"venture", legacy_lower.encode()).replace(b"Venture", legacy_title.encode()).replace(b"VENTURE", legacy_upper.encode())
    expected = {p.name.replace("venture", legacy_lower): hashlib.sha256(legacy_bytes(p.read_bytes())).hexdigest() for p in PATCHES}
    legacy_header = f"include/EGL/eglext_angle_{legacy_lower}.h"
    for candidate in sorted((cache / "prebuilt").glob(f"{PIN}-*-{sys.platform}-x64")):
        try:
            manifest = json.loads((candidate / "manifest.json").read_text())
            if manifest.get("angle_commit") != PIN or manifest.get("patches") != expected or manifest.get("toolchain") != LOCK:
                continue
            files = manifest["files"]
            suffix = ".dll" if os.name == "nt" else ".so"
            required = {"bin/libEGL" + suffix, "bin/libGLESv2" + suffix, legacy_header, "LICENSE"}
            if not required <= files.keys() or (swiftshader and not any("vk_swiftshader" in name for name in files)):
                continue
            if not all((candidate / name).is_file() and sha(candidate / name) == digest for name, digest in files.items()):
                continue
        except (OSError, ValueError, KeyError):
            continue
        # Header adaptation belongs to this worktree; preserve the verified DLLs
        # and legal notices through junctions/symlinks without copying large data.
        staged = ROOT / ".deps/angle-venture" / candidate.name
        if not staged.exists():
            staged.mkdir(parents=True)
            shutil.copytree(candidate / "include", staged / "include")
            header = staged / legacy_header
            updated = header.read_bytes().replace(legacy_upper.encode(), b"VENTURE").replace(legacy_lower.encode(), b"venture").replace(legacy_title.encode(), b"Venture")
            header.unlink()
            (staged / "include/EGL/eglext_angle_venture.h").write_bytes(updated)
            for name in ("bin", "lib", "licenses"):
                if (candidate / name).exists():
                    link_install(staged / name, candidate / name)
            shutil.copy2(candidate / "LICENSE", staged / "LICENSE")
            (staged / "legacy-artifact.json").write_text(json.dumps({"source": str(candidate), "key": manifest["key"]}) + "\n")
        adapted = {}
        for name, digest in files.items():
            new_name = name.replace(legacy_lower, "venture").replace(legacy_upper, "VENTURE")
            if name.startswith("include/EGL/") or name.startswith("EGL_"):
                original = (candidate / name).read_bytes()
                updated = original.replace(legacy_upper.encode(), b"VENTURE").replace(legacy_lower.encode(), b"venture").replace(legacy_title.encode(), b"Venture")
                destination = staged / new_name
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not destination.exists() or destination.read_bytes() != updated:
                    destination.write_bytes(updated)
                if new_name != name and (staged / name).exists(): (staged / name).unlink()
                adapted[new_name] = hashlib.sha256(updated).hexdigest()
            else:
                if "/" not in name and not (staged / name).exists(): shutil.copy2(candidate / name, staged / name)
                adapted[new_name] = digest
        adapted_manifest = dict(manifest, key="venture-legacy-" + manifest["key"],
            source_key=manifest["key"], legacy_abi=True,
            patches={p.name: sha(p) for p in PATCHES}, files=adapted)
        (staged / "manifest.json").write_text(json.dumps(adapted_manifest, indent=2) + "\n")
        # A default-directory migration moves the shared artifact. Refresh even
        # existing adapters so their absolute junction targets keep resolving.
        for name in ("bin", "lib", "licenses"):
            if (candidate / name).exists(): link_install(staged / name, candidate / name)
        print("Reusing SHA-256-verified pre-rebrand ANGLE ABI; adapted headers in .deps/angle-venture", flush=True)
        return staged
    return None


def install(source: Path, output: Path, destination: Path, key: str, elapsed: float):
    destination.mkdir(parents=True)
    for subdir in ("bin", "lib", "licenses"):
        (destination / subdir).mkdir()
    shutil.copytree(source / "include", destination / "include")
    shutil.copytree(source / "third_party/vulkan-headers/src/include/vulkan",
                    destination / "include/vulkan", dirs_exist_ok=True)
    shutil.copytree(source / "third_party/vulkan-headers/src/include/vk_video",
                    destination / "include/vk_video", dirs_exist_ok=True)
    patterns = ("libEGL.dll*", "libEGL.lib", "libEGL.pdb", "libGLESv2.dll*",
                "libGLESv2.lib", "libGLESv2.pdb", "vk_swiftshader*") if os.name == "nt" else (
                "libEGL.so*", "libGLESv2.so*", "libvk_swiftshader.so*", "vk_swiftshader*")
    for pattern in patterns:
        for file in output.glob(pattern):
            subdir = "lib" if file.suffix == ".lib" else "bin"
            shutil.copy2(file, destination / subdir / file.name)
    shutil.copy2(source / "LICENSE", destination / "LICENSE")
    shutil.copy2(source / "extensions/EGL_VENTURE_vulkan_creation_callbacks.txt", destination)
    # Preserve notices verbatim, indexed by source path. Flat filenames avoid
    # MAX_PATH failures in deeply nested third-party trees and downstream ZIPs.
    source_long = Path("\\\\?\\" + str(source)) if os.name == "nt" else source
    notices = {}
    for subtree in ("third_party", "src/third_party", "buildtools/third_party"):
        for current, dirs, files in os.walk(source_long / subtree):
            dirs[:] = [d for d in dirs if d not in (".git", ".cipd", "__pycache__", "node_modules")]
            if Path(current) == source_long / "third_party":
                dirs[:] = [d for d in dirs if d not in ("catapult", "VK-GL-CTS")]
            for name in files:
                if name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")):
                    file = Path(current) / name
                    if file.is_symlink():
                        continue
                    relative = file.relative_to(source_long).as_posix()
                    dest_name = hashlib.sha256(relative.encode()).hexdigest()[:20] + ".txt"
                    notices[relative] = dest_name
                    shutil.copy2(file, destination / "licenses" / dest_name)
    (destination / "licenses/INDEX.json").write_text(json.dumps(notices, indent=2, sort_keys=True) + "\n")
    shutil.copy2(output / "args.gn", destination / "args.gn")
    files = {p.relative_to(destination).as_posix(): sha(p)
             for p in sorted(destination.rglob("*")) if p.is_file()}
    manifest = {"key": key, "angle_commit": PIN,
                "patches": {p.name: sha(p) for p in PATCHES},
                "toolchain": LOCK, "build_seconds": elapsed, "files": files}
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def link_install(destination: Path, artifact: Path):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(destination):
        if destination.resolve() == artifact.resolve():
            return
        if destination.is_symlink():
            destination.unlink()
        elif os.name == "nt" and destination.lstat().st_reparse_tag == stat.IO_REPARSE_TAG_MOUNT_POINT:
            os.rmdir(destination)  # Removes only the junction, never its target.
        else:
            raise RuntimeError(f"{destination} is a real directory; move it aside first")
    if os.name == "nt":
        run(["cmd", "/c", "mklink", "/J", destination, artifact], cwd=ROOT)
    else:
        destination.symlink_to(artifact, target_is_directory=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_cache = data_dir() / "dev-cache/angle"
    parser.add_argument("--cache", type=Path, default=default_cache)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 4, 16))
    parser.add_argument("--swiftshader", action="store_true")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if platform.machine().lower() not in ("amd64", "x86_64") or sys.platform not in ("win32", "linux"):
        parser.error("Only Windows x64 and Linux x64 hosts are supported")
    if not PATCHES:
        raise RuntimeError("Missing Venture ANGLE patch")
    recipe = {"pin": PIN, "patches": [sha(p) for p in PATCHES], "lock": LOCK,
              "script": sha(Path(__file__)), "provision": sha(HERE / "provision_msvc.py"),
              "platform": sys.platform, "swiftshader": args.swiftshader}
    digest = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
    patch_hash = hashlib.sha256(b"".join(p.read_bytes() for p in PATCHES)).hexdigest()
    key = f"{PIN}-{patch_hash[:16]}-{digest[:16]}-{sys.platform}-x64"
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    artifact = cache / "prebuilt" / key
    with cache_lock(cache / "build.lock"):
        valid = valid_artifact(artifact, key)
        if not valid and not args.prepare_only:
            compatible = reuse_legacy_artifact(cache, args.swiftshader)
            if compatible:
                link_install(ROOT / ".deps/angle" / PIN, compatible)
                return
        if args.prepare_only or not valid:
            if artifact.exists() and not valid:
                quarantine = artifact.with_name(key + f".invalid-{time.time_ns()}")
                if not artifact.resolve().is_relative_to(cache) or not quarantine.resolve().is_relative_to(cache):
                    raise RuntimeError("Artifact path escaped the build cache")
                artifact.rename(quarantine)
                print(f"Preserved invalid cache entry at {quarantine}", flush=True)
            env = dict(os.environ)
            # Per-process Git configuration, never the user's global settings.
            count = int(env.get("GIT_CONFIG_COUNT", "0"))
            env[f"GIT_CONFIG_KEY_{count}"] = "core.longpaths"
            env[f"GIT_CONFIG_VALUE_{count}"] = "true"
            env["GIT_CONFIG_COUNT"] = str(count + 1)
            start = time.monotonic()
            source, msvc = prepare(cache, args.swiftshader, env)
            if args.prepare_only:
                return
            config = gn_args(msvc, args.swiftshader)
            config_hash = hashlib.sha256((PIN + config).encode()).hexdigest()[:16]
            output = source / "out" / ("Venture-" + config_hash)
            output.mkdir(parents=True, exist_ok=True)
            (output / "args.gn").write_text(config)
            gn = source / "buildtools" / ("win/gn.exe" if os.name == "nt" else "linux64/gn")
            ninja = source / "third_party/ninja" / ("ninja.exe" if os.name == "nt" else "ninja")
            run([gn, "gen", output, "--fail-on-unused-args"], cwd=source, env=env)
            run([ninja, "-C", output, "-j", args.jobs, "libEGL", "libGLESv2"], cwd=source, env=env)
            elapsed = time.monotonic() - start
            staged = artifact.with_name(key + f".staging-{os.getpid()}")
            install(source, output, staged, key, elapsed)
            staged.rename(artifact)
            print(f"ANGLE built in {elapsed:.1f}s; artifacts: {artifact}", flush=True)
        else:
            print(f"Reusing SHA-256-verified ANGLE artifact: {artifact}", flush=True)
        link_install(ROOT / ".deps/angle" / PIN, artifact)


if __name__ == "__main__":
    main()
