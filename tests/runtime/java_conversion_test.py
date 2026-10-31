"""Real DEX -> dex2jar -> ASM -> HotSpot behavior and strict-cache tests."""
import json
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/dex"))
from dex_surface import Dex, read_apk
from convert_apk import DEPS, build_tools, convert, driver_command, java_environment, java_tool, java_path
from cache_lock import filesystem_path
import stage_runtime




@unittest.skipUnless((DEPS / "jdk/bin").exists(), "run python tools/bootstrap.py --jdk")
class ConversionTest(unittest.TestCase):




    def test_multidex_keeps_cross_dex_linkage_and_native_declarations(self):
        with tempfile.TemporaryDirectory(prefix="java-multidex-", dir=ROOT / "build") as temp:
            root = Path(temp)
            (root / "First.java").write_text('''public class First {
  public static void main(String[] args) { if (Second.value() != 42) throw new AssertionError(); }
}''', encoding="utf-8")
            (root / "Second.java").write_text('''public class Second {
  public native int nativeMethod(long input);
  public static int value() { return 42; }
}''', encoding="utf-8")
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(root),
                            str(root / "First.java"), str(root / "Second.java")], check=True)
            apk = root / "multidex.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                for index, cls in enumerate(("First", "Second")):
                    source = root / (cls + ".jar")
                    with zipfile.ZipFile(source, "w") as jar:
                        jar.write(root / (cls + ".class"), cls + ".class")
                    dex = root / ("classes" + (str(index + 1) if index else "") + ".dex")
                    subprocess.run([java_tool("java"), "-cp", str(DEPS / "dex2jar/lib/*"), "com.android.dx.command.Main",
                                    "--dex", "--output=" + str(dex), str(source)], check=True)
                    archive.write(dex, dex.name)
            dest, report = convert(apk, "com.venture.multidex", root / "cache")
            self.assertTrue(report["ready"], report["failures"])
            # The Java driver's DEX census and identity match the Python parser's.
            with zipfile.ZipFile(apk) as archive:
                _, classes, natives, dexsha = read_apk(archive)
            self.assertEqual((report["dex_sha256"], report["dex_classes"], report["dex_natives"]),
                             (dexsha, len(classes), len(natives)))
            self.assertEqual(report["jar_classes"], 2)
            self.assertEqual(report["jar_natives"], 1)
            self.assertEqual((Path(report["translation_report"]).parent / "conversion.log").read_text().count("phases "), 2)
            subprocess.run([java_tool("java"), "-Xverify:all", "-cp", java_path(dest / "app.jar"), "First"], check=True)

    def test_conversion_verification_rewrites_behavior_and_cache(self):
        with tempfile.TemporaryDirectory(prefix="java-conversion-", dir=ROOT / "build") as temp:
            root = Path(temp)
            source = root / "ConversionFixture.java"
            source.write_text('''
import java.lang.annotation.*;
import java.lang.reflect.*;
@Retention(RetentionPolicy.RUNTIME) @interface Mark { String value(); }
interface DefaultMethod { default int value() { return 41; } }
@Mark("retained") public class ConversionFixture implements DefaultMethod {
  static { if (!"run".equals(System.getProperty("fixture.init"))) throw new AssertionError("initializer executed during verification"); }
  public native long nativeInstance(int x, float y, String z);
  public static native boolean nativeStatic(long x);
  public synchronized int arithmetic(int input) {
    int[] array = {3,4,5};
    try { if (input == 0) throw new IllegalArgumentException(); return array[input] + value(); }
    catch (IllegalArgumentException e) { return 7; }
  }
  public static void loads(String path) {
    System.loadLibrary(path); System.load(path);
    Runtime.getRuntime().loadLibrary(path); Runtime.getRuntime().load(path);
  }
  public static Object reflect(Method method, Object target, Object[] args) throws Exception { return method.invoke(target,args); }
  public static void main(String[] args) throws Exception {
    ConversionFixture fixture = new ConversionFixture();
    if (fixture.arithmetic(0)!=7 || fixture.arithmetic(1)!=45 || fixture.value()!=41) throw new AssertionError("behavior mismatch");
    if (!ConversionFixture.class.getAnnotation(Mark.class).value().equals("retained")) throw new AssertionError("annotation lost");
    Method nativeMethod=ConversionFixture.class.getDeclaredMethod("nativeInstance",int.class,float.class,String.class);
    if (!Modifier.isNative(nativeMethod.getModifiers())) throw new AssertionError("native flag lost");
    if (!reflect(String.class.getMethod("length"), "abc", new Object[0]).equals(3)) throw new AssertionError("reflection rewrite");
  }
}
''', encoding="utf-8")
            classes = root / "classes"
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(classes), str(source)], check=True)
            dex = root / "classes.dex"
            subprocess.run([java_tool("java"), "-cp", str(DEPS / "dex2jar/lib/*"), "com.android.dx.command.Main",
                            "--dex", "--min-sdk-version=26", "--output=" + str(dex), str(classes)], check=True)
            apk = root / "fixture.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.write(dex, "classes.dex")
            tool_dir = build_tools()
            dest, report = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            self.assertTrue(report["ready"], json.dumps(report["failures"], indent=2))
            self.assertEqual(report["jar_classes"], 3)
            self.assertEqual(report["jar_natives"], 2)
            self.assertGreaterEqual(report["rewrites"], 5)
            # Independently generate a ready cache under the released pipeline
            # identity. A Venture conversion must choose a new linked artifact.
            import shutil
            legacy_tools = root / "legacy/dex/classes"
            shutil.copytree(tool_dir, legacy_tools)
            driver = root / "ApkCache.java"
            legacy_pipeline = "dex2jar-2.4/asm-9.8/qp-2"  # rebrand: legacy cache fixture
            legacy_source = (ROOT / "tools/dex/rewriter/ApkCache.java").read_text().replace("dex2jar-2.4/asm-9.8/venture-3", legacy_pipeline)
            legacy_source = legacy_source.replace("deps = tools.getParent().getParent();", 'deps = Path.of(System.getenv("VENTURE_LEGACY_TEST_DEPS"));')
            driver.write_text(legacy_source, encoding="utf-8")
            subprocess.run([java_tool("javac"), "--release", "21", "-cp", java_path(legacy_tools), "-d", java_path(legacy_tools), str(driver)], check=True)
            with patch.dict(os.environ, {"VENTURE_LEGACY_TEST_DEPS": str(DEPS)}):
                old_dest, old_report = convert(apk, "com.venture.fixture", root / "cache", tool_dir=legacy_tools)
            self.assertTrue(old_report["ready"])
            self.assertEqual(old_report["identity"]["pipeline"], legacy_pipeline)
            new_dest, new_report = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            self.assertNotEqual(new_dest, old_dest)
            self.assertEqual(new_report["identity"]["pipeline"], "dex2jar-2.4/asm-9.8/venture-3")
            with zipfile.ZipFile(new_dest / "app.jar") as jar:
                fixture_class = jar.read("ConversionFixture.class")
                self.assertIn(b"com/venture/runtime/NativeLoader", fixture_class)
                legacy_owner = b"com/questport/runtime/NativeLoader"  # rebrand: legacy cache fixture
                self.assertNotIn(legacy_owner, fixture_class)
            raw_report = Path(report["translation_report"])
            raw_stamp = raw_report.stat().st_mtime_ns
            # The verifier would already have failed if it ran <clinit>.
            cp = os.pathsep.join([java_path(dest / "app.jar"), java_path(tool_dir / "venture-java-bridge.jar")])
            subprocess.run([java_tool("java"), "-Xverify:all", "-Dfixture.init=run", "-cp", cp, "ConversionFixture"], check=True)
            stamp = (dest / "app.jar").stat().st_mtime_ns
            _, cached = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            self.assertTrue(cached["ready"])
            self.assertEqual(stamp, (dest / "app.jar").stat().st_mtime_ns)
            # Changed compiler metadata gets its own artifact while an earlier
            # JAR is still open (Windows rejects replacement of an open JAR).
            metadata = root / "empty.hierarchy.tsv"
            metadata.write_text("", encoding="utf-8")
            with (dest / "app.jar").open("rb"):
                other, other_report = convert(apk, "com.venture.fixture", root / "cache",
                                               tool_dir=tool_dir, hierarchy=[metadata])
                self.assertTrue(other_report["ready"])
                self.assertTrue(other_report["translation_cache_hit"])
                self.assertEqual(other_report["translation_report"], str(raw_report))
                self.assertEqual(raw_stamp, raw_report.stat().st_mtime_ns)
                self.assertNotEqual(other, dest)
                self.assertEqual(stamp, (dest / "app.jar").stat().st_mtime_ns)
            again, _ = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            self.assertEqual(again, dest)
            with patch.dict(os.environ, {"JDK_JAVA_OPTIONS": "--venture-invalid-option", "_JAVA_OPTIONS": "-Xverify:none"}):
                isolated, isolated_report = convert(apk, "com.venture.fixture", root / "cache", force=True, tool_dir=tool_dir)
            self.assertTrue(isolated_report["ready"], isolated_report["failures"])
            self.assertNotEqual(isolated, dest)
            # Two forced publications while a real JVM holds the earlier JAR:
            # both succeed, and neither replaces that JVM's immutable generation.
            holder_source = root / "Holder.java"
            holder_source.write_text('''
public class Holder {
  public static void main(String[] args) throws Exception {
    try (var jar = new java.util.jar.JarFile(args[0])) {
      jar.getInputStream(jar.getJarEntry("ConversionFixture.class")).readAllBytes();
      System.out.println("held"); System.out.flush(); System.in.read();
      if (jar.getJarEntry("ConversionFixture.class") == null) throw new AssertionError();
    }
  }
}
''', encoding="utf-8")
            subprocess.run([java_tool("javac"), "-d", str(root / "holder"), str(holder_source)], check=True)
            with subprocess.Popen([java_tool("java"), "-Xverify:all", "-cp", str(root / "holder"),
                                   "Holder", str(dest / "app.jar")], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as holder:
                try:
                    self.assertEqual(holder.stdout.readline().strip(), "held")
                    command = [sys.executable, str(ROOT / "tools/dex/convert_apk.py"), str(apk),
                               "--package", "com.venture.fixture", "--cache", str(root / "cache"), "--force"]
                    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as first, \
                         subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as second:
                        outputs = [first.communicate(timeout=90), second.communicate(timeout=90)]
                        self.assertEqual(first.returncode, 0, outputs[0])
                        self.assertEqual(second.returncode, 0, outputs[1])
                        jars = [Path(json.loads(stdout.strip().splitlines()[-1])["jar"]) for stdout, _ in outputs]
                        self.assertNotEqual(jars[0], jars[1])
                        self.assertNotIn(dest / "app.jar", jars)
                    self.assertEqual(stamp, (dest / "app.jar").stat().st_mtime_ns)
                finally:
                    holder.communicate(input="\n", timeout=30)
                self.assertEqual(holder.returncode, 0)
            dest, _ = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            # A corrupted JAR is never accepted by a cached ready report.
            (dest / "app.jar").write_bytes(b"bad cache")
            repaired_dest, repaired = convert(apk, "com.venture.fixture", root / "cache", tool_dir=tool_dir)
            self.assertTrue(repaired["ready"])
            self.assertNotEqual(repaired_dest, dest)
            self.assertTrue(zipfile.is_zipfile(repaired_dest / "app.jar"))
            dest = repaired_dest
            probe = Path(os.environ.get("VENTURE_CONVERSION_PROBE", ROOT / "build/windows-release/bin/venture_java_conversion_probe.exe"))
            if probe.exists():
                # The runtime's subprocess entry also isolates the driver JVM
                # from launcher-wide Java option injection.
                env = {**os.environ, "JDK_JAVA_OPTIONS": "--venture-invalid-option", "_JAVA_OPTIONS": "-Xverify:none"}
                result = subprocess.run([str(probe), java_tool("java"), java_path(tool_dir), str(apk), str(root / "cache")],
                                        check=True, capture_output=True, text=True, env=env)
                response = json.loads(result.stdout.strip().splitlines()[-1])
                self.assertEqual(filesystem_path(response["app_jar"]), dest / "app.jar")

    def test_app_class_in_a_java_package_is_deferred(self):
        # ART loads app classes in java.* packages (Stremio's libVLC ships java.awt
        # Blu-ray classes); HotSpot refuses them, which defers them like missing linkage.
        with tempfile.TemporaryDirectory(prefix="java-prohibited-", dir=ROOT / "build") as temp:
            root = Path(temp)
            source = root / "java" / "awt" / "BDArea.java"
            source.parent.mkdir(parents=True)
            source.write_text("package java.awt; public class BDArea {}", encoding="utf-8")
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(root / "classes"), str(source)], check=True)
            jar = root / "prohibited.jar"
            with zipfile.ZipFile(jar, "w") as archive:
                archive.write(root / "classes" / "java" / "awt" / "BDArea.class", "java/awt/BDArea.class")
            dex = root / "classes.dex"
            subprocess.run([java_tool("java"), "-cp", str(DEPS / "dex2jar/lib/*"), "com.android.dx.command.Main",
                            "--dex", "--core-library", "--min-sdk-version=26", "--output=" + str(dex), str(jar)], check=True)
            apk = root / "prohibited.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.write(dex, "classes.dex")
            _, report = convert(apk, "com.venture.prohibited", root / "cache")
            self.assertFalse(report["ready"])
            self.assertTrue(report["translation_complete"], report["failures"])
            self.assertTrue(any(f.get("kind") == "java.lang.SecurityException" for f in report["failures"]))

    def test_missing_external_class_never_publishes_ready_cache(self):
        with tempfile.TemporaryDirectory(prefix="java-missing-", dir=ROOT / "build") as temp:
            root = Path(temp)
            (root / "Missing.java").write_text("public class Missing extends External {}", encoding="utf-8")
            (root / "External.java").write_text("public class External {}", encoding="utf-8")
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(root), str(root / "Missing.java"), str(root / "External.java")], check=True)
            dex = root / "classes.dex"
            partial_jar = root / "missing-classes.jar"
            with zipfile.ZipFile(partial_jar, "w") as archive:
                archive.write(root / "Missing.class", "Missing.class")
            subprocess.run([java_tool("java"), "-cp", str(DEPS / "dex2jar/lib/*"), "com.android.dx.command.Main",
                            "--dex", "--min-sdk-version=26", "--output=" + str(dex), str(partial_jar)], check=True)
            apk = root / "missing.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.write(dex, "classes.dex")
            _, report = convert(apk, "com.venture.missing", root / "cache")
            self.assertFalse(report["ready"])
            self.assertTrue(report["translation_complete"])
            _, cached = convert(apk, "com.venture.missing", root / "cache")
            self.assertFalse(cached["ready"])
            self.assertEqual(cached, report)
            self.assertTrue(any(f.get("kind") == "java.lang.NoClassDefFoundError" for f in report["failures"]))
            metadata = root / "sdk.hierarchy.tsv"
            metadata.write_text("External\tjava/lang/Object\t0\t\n", encoding="utf-8")
            ancestry_dest, with_ancestry = convert(apk, "com.venture.missing", root / "cache", hierarchy=[metadata])
            self.assertFalse(with_ancestry["ready"])
            self.assertTrue(any(f.get("kind") == "java.lang.NoClassDefFoundError" for f in with_ancestry["failures"]))
            self.assertNotEqual(with_ancestry["identity"], report["identity"])
            # Compiler metadata cannot masquerade as an executable implementation.
            with zipfile.ZipFile(ancestry_dest / "app.jar") as jar:
                self.assertNotIn("External.class", jar.namelist())
            framework = root / "framework.jar"
            with zipfile.ZipFile(framework, "w") as jar:
                jar.write(root / "External.class", "External.class")
            good, linked = convert(apk, "com.venture.missing", root / "cache", framework=[framework])
            self.assertTrue(linked["ready"], linked["failures"])
            self.assertTrue(linked["translation_cache_hit"])
            # Same framework pathname, different ancestry/access bytes: this
            # must reverify and fail, while keeping the good framework generation.
            (root / "External.java").write_text("public final class External {}", encoding="utf-8")
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(root), str(root / "External.java")], check=True)
            with zipfile.ZipFile(framework, "w") as jar:
                jar.write(root / "External.class", "External.class")
            bad, invalid = convert(apk, "com.venture.missing", root / "cache", framework=[framework])
            self.assertNotEqual(bad, good)
            self.assertTrue(invalid["translation_cache_hit"])
            self.assertFalse(invalid["ready"])
            self.assertFalse(invalid["translation_complete"])
            self.assertTrue(any(f.get("kind") == "java.lang.IncompatibleClassChangeError" for f in invalid["failures"]))
            # The runtime's per-package cache keeps only the generation in use:
            # each Venture update would otherwise leave another copy behind.
            runtime_cache = root / "runtime" / "com.venture.missing" / "7" / "java"
            (runtime_cache / ".conversion-old").mkdir(parents=True)
            first, _ = convert(apk, "com.venture.missing", runtime_cache)
            second, _ = convert(apk, "com.venture.missing", runtime_cache, hierarchy=[metadata])
            self.assertNotEqual(first, second)
            self.assertFalse(first.exists())
            self.assertTrue((second / "app.jar").is_file())
            self.assertFalse((runtime_cache / ".conversion-old").exists())
            self.assertEqual([p.name for p in (runtime_cache / "linked").iterdir()], [second.parent.name])
            # Shared caches (several packages, as the CLI allows) are left alone.
            self.assertTrue((bad / "app.jar").is_file() and (good / "app.jar").is_file())

    def test_unframeable_method_throws_like_art_and_class_still_loads(self):
        # Unreal's ProfilerAccessor merges types an older framework lacks: ART loads
        # the class and only the method throws when it runs; HotSpot needs frames.
        with tempfile.TemporaryDirectory(prefix="java-soft-", dir=ROOT / "build") as temp:
            root = Path(temp)
            (root / "Soft.java").write_text('''
public class Soft {
  public static int ok() { return 5; }
  public static Object pick(boolean c) {
    Object o;
    try { o = c ? new MissingA() : new MissingB(); } catch (RuntimeException e) { o = null; }
    return o;
  }
}
class MissingA {} class MissingB {}
''', encoding="utf-8")
            (root / "Driver.java").write_text('''
public class Driver {
  public static void main(String[] a) {
    if (Soft.ok() != 5) throw new AssertionError("working method");
    try { Soft.pick(true); throw new AssertionError("no throw"); }
    catch (NoClassDefFoundError e) { System.out.println("missing " + e.getMessage()); }
  }
}
''', encoding="utf-8")
            classes = root / "classes"
            subprocess.run([java_tool("javac"), "-Xlint:-options", "--release", "8", "-d", str(classes),
                            str(root / "Soft.java"), str(root / "Driver.java")], check=True)
            partial = root / "soft.jar"
            with zipfile.ZipFile(partial, "w") as archive:
                archive.write(classes / "Soft.class", "Soft.class")
            dex = root / "classes.dex"
            subprocess.run([java_tool("java"), "-cp", str(DEPS / "dex2jar/lib/*"), "com.android.dx.command.Main",
                            "--dex", "--min-sdk-version=26", "--output=" + str(dex), str(partial)], check=True)
            apk = root / "soft.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.write(dex, "classes.dex")
            dest, report = convert(apk, "com.venture.soft", root / "cache")
            self.assertEqual([f["stage"] for f in report["failures"]], ["frames"])
            driver = root / "driver"
            driver.mkdir()
            (driver / "Driver.class").write_bytes((classes / "Driver.class").read_bytes())
            run = subprocess.run([java_tool("java"), "-cp", os.pathsep.join([java_path(dest / "app.jar"), java_path(driver)]), "Driver"],
                                 capture_output=True, text=True, env=java_environment())
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("missing Missing", run.stdout)


if __name__ == "__main__":
    unittest.main()
