"""Superclass constructors on a new-instance (R8 constructor inlining, ART-only) through the
production DEX converter and rewriter: assemble tests/java/super_init/*.smali into two DEX files,
convert, link, and run SuperInitCheck under -Xverify:all."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/dex"))
from convert_apk import build_tools, java_path, java_tool

FIXTURE = ROOT / "tests/java/super_init"
DEX2JAR = java_path(ROOT / ".deps/dex2jar/lib/*")

with tempfile.TemporaryDirectory(prefix="venture-super-init-") as directory:
    work = Path(directory)
    # The callers and their target classes are in different DEX files (multidex).
    dexes = {"classes.dex": ["callers.smali"],
             "classes2.dex": ["targets.smali", "plain.smali", "middle.smali", "child.smali", "colliding.smali", "busy.smali"]}
    for dex, sources in dexes.items():
        subprocess.run([java_tool("java"), "-cp", DEX2JAR, "com.googlecode.d2j.smali.SmaliCmd", "-o", str(work / dex),
                        *(str(FIXTURE / s) for s in sources)], check=True, stdout=subprocess.DEVNULL)
    tools = java_path(build_tools())
    tool_path = os.pathsep.join([tools, java_path(ROOT / ".deps/asm/asm.jar"), DEX2JAR])
    command = [java_tool("java"), "-cp", tool_path, "com.venture.dex.Convert"]
    for dex in dexes:
        command += [str(work / dex), str(work / (dex + ".jar")), str(work / (dex + ".errors.tsv"))]
    log = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    print(log, end="")
    assert "superclass constructor calls retargeted=6 synthetic constructors=7 unresolved=0" in log, log
    for dex in dexes:
        assert (work / (dex + ".errors.tsv")).read_text() == "", dex
    merged = work / "app.jar"
    with zipfile.ZipFile(merged, "w") as out:
        for dex in dexes:
            with zipfile.ZipFile(work / (dex + ".jar")) as jar:
                for name in jar.namelist():
                    out.writestr(name, jar.read(name))
    linked = work / "linked.jar"
    subprocess.run([java_tool("java"), "-cp", tool_path, "com.venture.dex.Rewrite", str(merged), str(linked),
                    str(work / "census.tsv"), ""], check=True, stdout=subprocess.DEVNULL)
    frames = [line for line in (work / "census.tsv").read_text().splitlines() if line.startswith("F\t")]
    assert not frames, frames
    checks = work / "check"
    subprocess.run([java_tool("javac"), "--release", "21", "-d", str(checks), str(FIXTURE / "SuperInitCheck.java")], check=True)
    subprocess.run([java_tool("java"), "-Xverify:all", "-cp", os.pathsep.join([str(linked), str(checks)]), "SuperInitCheck"], check=True)
