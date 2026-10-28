#!/usr/bin/env python3
"""Stage Java support beside the runtime: two JARs and java/{jdk,agent,dex/classes,asm,dex2jar}."""
import argparse
import filecmp
from pathlib import Path
import shutil
import os
import subprocess

from convert_apk import DEPS, build_tools, java_environment


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or not filecmp.cmp(source, target, shallow=False):
        shutil.copy2(source, target)


def copy_tree(source, target):
    # Removed JDK/converter files must not survive an update in bin/.
    if target.is_dir():
        for path in target.rglob("*"):
            if path.is_file() and not (source / path.relative_to(target)).is_file():
                path.unlink()
    for path in source.rglob("*"):
        if path.is_file():
            copy_file(path, target / path.relative_to(source))


def remove_python_driver(dex):
    # Launches run the Java driver in dex/classes; earlier builds staged a
    # Python driver here. Remove exactly those files, never a whole tree.
    for name in ("convert_apk.py", "dex_surface.py", "cache_lock.py"):
        (dex / name).unlink(missing_ok=True)
    cache = dex / "__pycache__"
    if cache.is_dir() and not cache.is_symlink():
        for path in cache.glob("*.pyc"):
            if path.stem.split(".")[0] in ("convert_apk", "dex_surface", "cache_lock"):
                path.unlink()
        if not any(cache.iterdir()):
            cache.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--jdk", required=True, type=Path)
    parser.add_argument("--bridge", required=True, type=Path)
    parser.add_argument("--agent", required=True, type=Path)
    args = parser.parse_args()
    java = args.bin / "java"
    copy_file(args.bridge, args.bin / "venture-java-bridge.jar")
    copy_tree(args.jdk, java / "jdk")
    copy_file(args.agent, java / "agent" / args.agent.name)
    copy_tree(build_tools(), java / "dex/classes")
    remove_python_driver(java / "dex")
    copy_file(DEPS / "asm/asm.jar", java / "asm/asm.jar")
    copy_tree(DEPS / "dex2jar/lib", java / "dex2jar/lib")
    for name in ("LICENSE.txt", "NOTICE.txt"):
        copy_file(DEPS / "dex2jar" / name, java / "dex2jar" / name)
    copy_file(DEPS / "framework-res/android-32.hierarchy.tsv", java / "android-32.hierarchy.tsv")
    executable = java / "jdk/bin" / ("java.exe" if os.name == "nt" else "java")
    if executable.is_file():
        # Dependency bytes are immutable in the distributed bundle. Compute
        # their identity during staging, rather than rereading the JDK modules
        # and every converter JAR in a launch-time JVM.
        result = subprocess.run([str(executable), "-cp", str(java / "dex/classes"),
                                 "com.venture.dex.ApkCache", "--translation-identity"],
                                check=True, capture_output=True, env=java_environment())
        target = java / "dex/classes/translation-identity.json"
        temporary = target.with_suffix(".tmp")
        temporary.write_bytes(result.stdout)
        os.replace(temporary, target)


if __name__ == "__main__":
    main()
