#!/usr/bin/env python3
"""Clone Venture (or update an existing clone) and build a release.

Usage:  python tools/clone_and_build.py [workdir] [--branch NAME]
Works on Windows and Linux; all paths are relative to the work directory.
"""
import argparse
import platform
import subprocess
import sys
from pathlib import Path

REPO = "https://github.com/DeliciousMeatPop/Venture-UnknownSourceEdition"


def run(cmd, cwd=None):
    print("+", " ".join(map(str, cmd)), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir", nargs="?", default="venture-build")
    ap.add_argument("--branch", default=None)
    args = ap.parse_args()

    src = Path(args.workdir) / "src-tree"
    if (src / ".git").is_dir():
        run(["git", "fetch", "origin"], cwd=src)
        run(["git", "pull", "--ff-only"], cwd=src)
    else:
        src.parent.mkdir(parents=True, exist_ok=True)
        cmd = ["git", "clone", "--recurse-submodules"]
        if args.branch:
            cmd += ["--branch", args.branch]
        run(cmd + [REPO, str(src)])

    windows = platform.system() == "Windows"
    preset = "windows-release" if windows else "linux-release"
    boot = ["--jdk", "--angle"] if windows else ["--jdk"]
    py = sys.executable
    cmake = str(Path(".deps") / "cmake" / "bin" / ("cmake.exe" if windows else "cmake"))

    run([py, str(Path("tools") / "bootstrap.py"), *boot], cwd=src)
    run([cmake, "--preset", preset], cwd=src)
    run([cmake, "--build", "--preset", preset], cwd=src)
    print("Build finished. Output is under", src / "build")


if __name__ == "__main__":
    main()
