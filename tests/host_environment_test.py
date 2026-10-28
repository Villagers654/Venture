"""Exercise the Java and native host diagnostics in isolated subprocesses."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

JAVA_HOME = Path(sys.argv.pop(1))
NATIVE_PROBE = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]


class HostEnvironmentTest(unittest.TestCase):
    def test_current_empty_and_unset_host_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="venture-host-env-") as tmp:
            root = Path(tmp)
            probe = root / "Probe.java"
            probe.write_text('''import com.venture.runtime.HostEnvironment;
public class Probe {
  public static void main(String[] args) {
    String value = HostEnvironment.getenv("VENTURE_TRACE_LOOPER");
    System.out.print(value == null ? "unset" : value);
  }
}
''')
            exe = ".exe" if os.name == "nt" else ""
            subprocess.run([str(JAVA_HOME / ("bin/javac" + exe)), "-d", tmp,
                            str(ROOT / "src/runtime/java/framework/com/venture/runtime/HostEnvironment.java"), str(probe)], check=True, capture_output=True)
            legacy = "QP_TRACE_LOOPER"  # rebrand: legacy fixture
            env = {**os.environ, "VENTURE_DATA_DIR": str(root / "data"), legacy: "legacy-value"}
            env.pop("VENTURE_TRACE_LOOPER", None)
            for expected in ("unset", "current-value", ""):
                if expected == "": env["VENTURE_TRACE_LOOPER"] = ""
                if expected == "current-value": env["VENTURE_TRACE_LOOPER"] = expected
                elif expected == "unset": env.pop("VENTURE_TRACE_LOOPER", None)
                result = subprocess.run([str(JAVA_HOME / ("bin/java" + exe)), "-cp", tmp, "Probe"], env=env, check=True, capture_output=True)
                self.assertEqual(result.stdout.decode(), expected)


    def test_native_without_legacy_settings_is_silent(self):
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith("QP_")}
        env.pop("VENTURE_TRACE_LOOPER", None)
        result = subprocess.run([str(NATIVE_PROBE)],env=env,check=True,capture_output=True)
        self.assertEqual(result.stdout.decode().splitlines(), ["unset"]*3)
        self.assertEqual(result.stderr,b"")

    def test_native_legacy_warning_once_without_fallback(self):
        legacy = "QP_TRACE_LOOPER"  # rebrand: ignored legacy fixture
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith("QP_")}
        env[legacy] = "legacy-value"
        env.pop("VENTURE_TRACE_LOOPER", None)
        for current, expected in ((None,"unset"),("current-value","current-value"),("","empty")):
            if current is None: env.pop("VENTURE_TRACE_LOOPER",None)
            else: env["VENTURE_TRACE_LOOPER"] = current
            result = subprocess.run([str(NATIVE_PROBE)],env=env,check=True,capture_output=True)
            self.assertEqual(result.stdout.decode().splitlines(), [expected]*3)
            self.assertEqual(result.stderr.decode().count("Legacy QP_* settings are ignored"),1)

if __name__ == "__main__":
    unittest.main()
