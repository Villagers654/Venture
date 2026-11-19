#!/usr/bin/env python3
"""Build the Venture APK from the native libraries CMake built, with Google's
command-line build tools only (no Gradle, no Android Studio).

  build_apk.py --build-tools DIR --android-jar FILE --java-home DIR
               --manifest FILE --res DIR --java DIR [--java DIR ...]
               --lib FILE [--lib FILE ...] [--asset DEST=FILE ...]
               --min-sdk N --target-sdk N --version NAME --out FILE.apk
               [--keystore FILE] [--work DIR]

Steps: aapt2 compiles and links the resources and manifest (and writes R.java),
javac compiles the Java sources against android.jar, d8 dexes them with the
classes --jar-classes takes from prebuilt jars (apksig from build-tools'
apksigner.jar, which the app signs the games it adapts with), the native
libraries go in uncompressed under lib/<--abi>, zipalign aligns them to
16 KiB pages, and apksigner signs the result (v2 and v3 schemes).

The signing key: --keystore, or VENTURE_APK_KEYSTORE with its password in
VENTURE_APK_KEYSTORE_PASS (release builds); otherwise a development key kept in
Venture's dev-cache, generated once per machine, so successive development
builds install over each other.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.venture_paths import data_dir  # noqa: E402

WINDOWS = os.name == "nt"


def exe(directory: Path, name: str) -> Path:
    path = directory / (name + (".exe" if WINDOWS else ""))
    if not path.exists():
        sys.exit(f"build_apk: missing {path}")
    return path


def run(command: list, **kwargs) -> None:
    result = subprocess.run([str(c) for c in command], **kwargs)
    if result.returncode:
        sys.exit(f"build_apk: {Path(str(command[0])).name} failed ({result.returncode})")


def version_code(version: str) -> int:
    """0.2.5 -> 2005; major * 1,000,000 + minor * 1,000 + patch."""
    parts = [int(p) for p in version.split(".")[:3]] + [0, 0, 0]
    return parts[0] * 1_000_000 + parts[1] * 1_000 + parts[2]


def vector_drawable(svg: Path) -> str:
    """An Android vector drawable of a flat SVG (viewBox plus filled paths)."""
    text = svg.read_text(encoding="utf-8")
    box = re.search(r'viewBox="([-\d.]+) ([-\d.]+) ([\d.]+) ([\d.]+)"', text)
    if not box:
        sys.exit(f"build_apk: {svg} has no viewBox")
    x, y, w, h = box.groups()
    paths = re.findall(r'<path d="([^"]+)" fill="(#[0-9A-Fa-f]{6})"\s*/>', text)
    if not paths:
        sys.exit(f"build_apk: {svg} has no filled paths")
    body = "".join(f'    <path android:pathData="{d}" android:fillColor="{fill}" />\n' for d, fill in paths)
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Generated from the launcher logo by tools/android/build_apk.py. -->\n'
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            f'    android:width="108dp" android:height="108dp"\n'
            f'    android:viewportWidth="{w}" android:viewportHeight="{h}">\n'
            f'  <group android:translateX="{-float(x):g}" android:translateY="{-float(y):g}">\n'
            f'{body}  </group>\n</vector>\n')


def signing_key(args, java_bin: Path) -> tuple[Path, str, str]:
    if args.keystore:
        return Path(args.keystore), os.environ.get("VENTURE_APK_KEYSTORE_PASS", "android"), \
            os.environ.get("VENTURE_APK_KEY_ALIAS", "venture")
    if os.environ.get("VENTURE_APK_KEYSTORE"):
        password = os.environ.get("VENTURE_APK_KEYSTORE_PASS")
        if not password:
            sys.exit("build_apk: VENTURE_APK_KEYSTORE needs VENTURE_APK_KEYSTORE_PASS")
        return Path(os.environ["VENTURE_APK_KEYSTORE"]), password, \
            os.environ.get("VENTURE_APK_KEY_ALIAS", "venture")
    # Development key: generated once per machine. Not a release key.
    store = data_dir() / "dev-cache" / "android-dev.keystore"
    if not store.exists():
        store.parent.mkdir(parents=True, exist_ok=True)
        partial = store.with_suffix(".partial")
        partial.unlink(missing_ok=True)
        run([exe(java_bin, "keytool"), "-genkeypair", "-keystore", partial, "-storetype", "PKCS12",
             "-storepass", "android", "-keypass", "android", "-alias", "venture", "-keyalg", "RSA",
             "-keysize", "3072", "-validity", "10000", "-dname", "CN=Venture development build"],
            stdout=subprocess.DEVNULL)
        partial.replace(store)
    return store, "android", "venture"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-tools", required=True, type=Path)
    parser.add_argument("--android-jar", required=True, type=Path)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--res", required=True, type=Path)
    parser.add_argument("--icon-svg", type=Path, help="flat SVG drawn as drawable/ic_launcher")
    parser.add_argument("--java", action="append", default=[], type=Path)
    parser.add_argument("--lib", action="append", default=[], type=Path)
    parser.add_argument("--abi", default="arm64-v8a", help="the ABI folder --lib libraries go in")
    parser.add_argument("--strip", type=Path, help="llvm-strip: libraries and the layer go in without debug info")
    parser.add_argument("--asset", action="append", default=[], help="DEST=FILE, DEST relative to assets/")
    parser.add_argument("--asset-dex", action="append", default=[],
                        help="DEST=SOURCE[,SOURCE...]: Java sources (folders or files) dexed into assets/DEST")
    parser.add_argument("--asset-dex-sourcepath", action="append", default=[], type=Path,
                        help="Java files asset dex files compile against without carrying them (one dex per package)")
    parser.add_argument("--jar-classes", action="append", default=[],
                        help="JAR=PREFIX: the jar's classes under PREFIX (com/android/apksig/) join the app's dex")
    parser.add_argument("--min-sdk", required=True)
    parser.add_argument("--target-sdk", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--keystore")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()

    tools = args.build_tools
    java_bin = args.java_home / "bin"
    work = args.work or args.out.with_suffix(".work")
    if work.exists():
        shutil.rmtree(work)
    (work / "gen").mkdir(parents=True)
    (work / "dex").mkdir()
    (work / "assets").mkdir()

    # Resources and manifest; the launcher icon is drawn from the logo's SVG.
    res = work / "res"
    shutil.copytree(args.res, res)
    if args.icon_svg:
        (res / "drawable").mkdir(exist_ok=True)
        (res / "drawable" / "ic_launcher.xml").write_text(vector_drawable(args.icon_svg), encoding="utf-8")
    flat = work / "res.zip"
    run([exe(tools, "aapt2"), "compile", "--dir", res, "-o", flat])
    def stripped(source: Path, target: Path) -> Path:
        target.parent.mkdir(parents=True, exist_ok=True)
        if args.strip and source.suffix == ".so":
            run([args.strip, "--strip-unneeded", "-o", target, source])
        else:
            shutil.copyfile(source, target)
        return target

    def java_files(paths: list) -> list:
        found = []
        for path in paths:
            found += [str(path)] if path.is_file() else [str(p) for p in sorted(path.rglob("*.java"))]
        return found

    # Java against the platform's own java.* and android.*, dexed into output.
    # `sourcepath`: other sources the listed ones compile against, left out of the dex.
    def dex(sources: list, name: str, output: Path, jars: list = (), sourcepath: list = ()) -> None:
        if not sources and not jars:
            return  # an APK without code (a NativeActivity)
        classes = work / f"classes-{name}"
        classes.mkdir(parents=True)
        # Prebuilt classes first, so the sources compile against them.
        for spec in jars:
            jar, _, prefix = spec.partition("=")
            with zipfile.ZipFile(jar) as archive:
                members = [m for m in archive.namelist() if m.startswith(prefix) and m.endswith(".class")]
                if not members:
                    sys.exit(f"build_apk: {jar} has no classes under {prefix}")
                archive.extractall(classes, members)
        listing = work / f"sources-{name}.txt"
        listing.write_text("\n".join(f'"{s}"'.replace("\\", "/") for s in sources), encoding="utf-8")
        search = ["-sourcepath", os.pathsep.join(str(p) for p in sourcepath), "-implicit:none"] if sourcepath else []
        run([exe(java_bin, "javac"), "-source", "8", "-target", "8", "-bootclasspath", args.android_jar,
             "-classpath", classes, *search, "-Xlint:-options", "-encoding", "UTF-8", "-nowarn", "-d", classes,
             f"@{listing}"])
        class_list = work / f"classes-{name}.txt"
        class_list.write_text("\n".join(str(p) for p in classes.rglob("*.class")), encoding="utf-8")
        output.mkdir(parents=True, exist_ok=True)
        run([exe(java_bin, "java"), "-cp", tools / "lib" / "d8.jar", "com.android.tools.r8.D8", "--release",
             "--min-api", args.min_sdk, "--lib", args.android_jar, "--output", output, f"@{class_list}"])

    for spec in args.asset:
        dest, _, source = spec.partition("=")
        stripped(Path(source), work / "assets" / dest)
    # Dex files the app carries as assets (the classes it adds to the games it installs).
    # The sources they may compile against, alone in a tree by package, so
    # nothing else of their folders is read.
    sourcepath = work / "asset-sourcepath"
    for source in java_files(args.asset_dex_sourcepath):
        package = re.search(r"^\s*package\s+([\w.]+)\s*;", Path(source).read_text(encoding="utf-8"), re.M)
        target = sourcepath.joinpath(*(package.group(1).split(".") if package else []), Path(source).name)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for i, spec in enumerate(args.asset_dex):
        dest, _, sources = spec.partition("=")
        out = work / f"asset-dex-{i}"
        dex(java_files([Path(s) for s in sources.split(",")]), f"asset{i}", out,
            sourcepath=[sourcepath] if args.asset_dex_sourcepath else [])
        target = work / "assets" / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / "classes.dex", target)
    libs = [stripped(lib, work / "lib" / lib.name) for lib in args.lib]
    linked = work / "linked.apk"
    run([exe(tools, "aapt2"), "link", "-o", linked, "-I", args.android_jar, "--manifest", args.manifest,
         "--java", work / "gen", "--min-sdk-version", args.min_sdk, "--target-sdk-version", args.target_sdk,
         "--version-code", str(version_code(args.version)), "--version-name", args.version,
         "-A", work / "assets", "--auto-add-overlay", flat])

    # The app's classes and R.
    dex([str(p) for p in (work / "gen").rglob("*.java")] + java_files(args.java), "app", work / "dex",
        args.jar_classes)

    # Assemble: native libraries stored (loaded straight from the APK), dex deflated.
    unaligned = work / "unaligned.apk"
    shutil.copyfile(linked, unaligned)
    with zipfile.ZipFile(unaligned, "a") as apk:
        for dex in sorted((work / "dex").glob("*.dex")):
            apk.write(dex, dex.name, compress_type=zipfile.ZIP_DEFLATED)
        for lib in libs:
            apk.write(lib, f"lib/{args.abi}/{lib.name}", compress_type=zipfile.ZIP_STORED)
    aligned = work / "aligned.apk"
    run([exe(tools, "zipalign"), "-f", "-P", "16", "4", unaligned, aligned])

    store, password, alias = signing_key(args, java_bin)
    signed = work / "signed.apk"
    run([exe(java_bin, "java"), "-jar", tools / "lib" / "apksigner.jar", "sign", "--ks", store,
         "--ks-pass", "env:VENTURE_APK_SIGN_PASS", "--ks-key-alias", alias, "--min-sdk-version", args.min_sdk,
         "--v1-signing-enabled", "false", "--v2-signing-enabled", "true", "--v3-signing-enabled", "true",
         "--out", signed, aligned], env={**os.environ, "VENTURE_APK_SIGN_PASS": password})
    run([exe(java_bin, "java"), "-jar", tools / "lib" / "apksigner.jar", "verify", "--min-sdk-version",
         args.min_sdk, signed])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    os.replace(signed, args.out)
    print(f"build_apk: {args.out} ({args.out.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()
