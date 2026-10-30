"""Run actual java.io bytecode through the production APK rewriter."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/dex"))
from convert_apk import build_tools, java_path, java_tool

framework = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="venture-files-rewrite-") as directory:
    work = Path(directory)
    original = work / "classes"
    original.mkdir()
    snapshots = work / "snapshots"
    snapshots.mkdir()
    asm = java_path(ROOT / ".deps/asm/asm.jar")
    subprocess.run([java_tool("java"), "-cp", asm, str(ROOT / "tests/java/InterfaceCallFixture.java"), str(original)], check=True)
    subprocess.run([java_tool("javac"), "--release", "21", "-cp", os.pathsep.join([framework, str(original)]), "-d", str(original),
                    str(ROOT / "tests/java/VirtualFilesFixture.java")], check=True)
    source, rewritten = work / "fixture.jar", work / "rewritten.jar"
    subprocess.run([java_tool("jar"), "--create", "--file", str(source), "-C", str(original), "."], check=True)
    tool_path = os.pathsep.join([java_path(build_tools()), str(ROOT / ".deps/asm/*")])
    subprocess.run([java_tool("java"), "-cp", tool_path, "com.venture.dex.Rewrite", str(source),
                    str(rewritten), str(work / "census.tsv"), framework], check=True)
    subprocess.run([java_tool("java"), "-Xverify:all", "-Djava.io.tmpdir=" + str(snapshots),
                    "-cp", os.pathsep.join([framework, str(rewritten)]), "VirtualFilesFixture", str(work)], check=True)
