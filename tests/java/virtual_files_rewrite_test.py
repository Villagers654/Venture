"""Run actual java.io bytecode through the production APK rewriter."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/dex"))
from convert_apk import build_tools, java_path, java_tool

framework, bridge = sys.argv[1], sys.argv[2]
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
    missing = [line.split("\t") for line in (work / "census.tsv").read_text().splitlines() if line.startswith("M\t")]
    assert missing == [["M", "MemberCaller", "android/widget/Button.ventureAbsentMember()V", "java.lang.NoSuchMethodError"]], missing
    direct = [line for line in (work / "census.tsv").read_text().splitlines()
              if line.startswith("R\tVirtualFilesFixture\t") and "ByteBuffer.allocateDirect" in line]
    assert len(direct) == 2, direct
    arrays = [line for line in (work / "census.tsv").read_text().splitlines()
              if line.startswith("R\tVirtualFilesFixture\t") and "direct buffer contents" in line]
    assert len(arrays) == 4, arrays
    subprocess.run([java_tool("java"), "-Xverify:all", "-Djava.io.tmpdir=" + str(snapshots),
                    "-cp", os.pathsep.join([framework, bridge, str(rewritten)]), "VirtualFilesFixture", str(work)], check=True)
