"""Exercise native APK extraction and the launch converter under Unicode paths."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PROBE = Path(os.environ.get("VENTURE_CONVERSION_PROBE", ROOT / "build/windows-release/bin/venture_java_conversion_probe.exe"))


class UnicodePathsTest(unittest.TestCase):
    def test_launch_paths(self):
        with tempfile.TemporaryDirectory(prefix="venture-unicode-") as temp:
            ascii_root = Path(temp)
            root = ascii_root / "Jos\u00e9-\u6d4b\u8bd5-\u03a9"
            root.mkdir()
            install = root / "games"
            install.mkdir()
            # Reuse the conversion fixture's javac/dx route before relocating it.
            java = Path(os.environ.get("VENTURE_UNICODE_JAVA_SOURCE", PROBE.parent / "java"))
            exe = ".exe" if os.name == "nt" else ""
            tool = lambda name: str(java / "jdk/bin" / (name + exe))
            source = ascii_root / "Fixture.java"
            source.write_text("public class Fixture { public static void main(String[] a) {} }", encoding="utf-8")
            subprocess.run([tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(ascii_root), str(source)], check=True)
            with zipfile.ZipFile(ascii_root / "classes.jar", "w") as jar:
                jar.write(ascii_root / "Fixture.class", "Fixture.class")
            subprocess.run([tool("java"), "-cp", str(java / "dex2jar/lib/*"), "com.android.dx.command.Main",
                            "--dex", "--output=" + str(ascii_root / "classes.dex"), str(ascii_root / "classes.jar")], check=True)
            apk = install / "base.apk"
            elf = bytearray(64)
            elf[:7] = b"\x7fELF\x02\x01\x01"
            struct.pack_into("<H", elf, 18, 183)
            with zipfile.ZipFile(apk, "w") as archive:
                archive.write(ascii_root / "classes.dex", "classes.dex")
                archive.writestr("AndroidManifest.xml", struct.pack("<II", 0x00080003, 8))
                archive.writestr("lib/arm64-v8a/libfixture.so", elf)
            (install / "plan.json").write_text(json.dumps({"package": "com.venture.fixture", "versionCode": 1,
                                                         "files": [{"kind": "apk", "name": "base.apk"}]}))
            (root / "temp").mkdir()
            env = dict(os.environ, VENTURE_DATA_DIR=str(root / "data"),
                       TMP=str(root / "temp"), TEMP=str(root / "temp"), TMPDIR=str(root / "temp"))
            native = subprocess.run([str(PROBE), "--configure", str(install)], env=env, capture_output=True)
            with self.subTest(stage="APK ZIP open and extraction"):
                self.assertEqual(native.returncode, 0, native.stderr.decode("utf-8", "replace"))
            shutil.copytree(java, root / "java")
            copied = root / "java"
            framework = root / "framework.jar"
            with zipfile.ZipFile(framework, "w") as jar:
                jar.writestr("fixture-resource", "Unicode dependency path")
            hierarchy = root / "hierarchy.tsv"
            hierarchy.write_text("", encoding="utf-8")
            command = [str(PROBE), str(copied / "jdk/bin" / ("java" + exe)), str(copied / "dex/classes"),
                       str(apk), str(root / "cache"), str(framework), str(hierarchy)]
            lock = root / "cache/.locks" / hashlib.sha256(b"com.venture.fixture").hexdigest()[:32]
            lock.parent.mkdir(parents=True)
            with lock.open("a+b") as held:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(held.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB, 1)
                drivers = [subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
                try:
                    time.sleep(3)
                    waiting = [driver.poll() is None for driver in drivers]
                finally:
                    held.seek(0)
                    if os.name == "nt":
                        msvcrt.locking(held.fileno(), msvcrt.LK_UNLCK, 1)
                    else:
                        fcntl.lockf(held, fcntl.LOCK_UN, 1)
            outputs = [driver.communicate(timeout=120) for driver in drivers]
            with self.subTest(stage="Unicode conversion and cache lock"):
                self.assertEqual(waiting, [True, True], outputs)
                self.assertEqual([driver.returncode for driver in drivers], [0, 0], outputs)
                jars = {json.loads(out.splitlines()[-1])["app_jar"] for out, _ in outputs}
                self.assertEqual(len(jars), 1)
                self.assertTrue(Path(next(iter(jars))).is_file())
                warm = subprocess.run(command, env=env, capture_output=True)
                self.assertEqual(warm.returncode, 0, warm.stderr)
                self.assertEqual(json.loads(warm.stdout.splitlines()[-1])["app_jar"], next(iter(jars)))


if __name__ == "__main__":
    unittest.main()
