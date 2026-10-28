"""The redistributed JRE (tools/dex/make_jre.py) must contain every JDK module the framework imports.

Dev runs use the full JDK, so a missing module only fails in the release layout (NoClassDefFoundError)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRAMEWORK = ROOT / "src/runtime/java/framework"
# Package prefix -> owning JDK module, for the JDK packages outside java.base the framework can reach.
OWNERS = {"java.awt": "java.desktop", "javax.imageio": "java.desktop", "javax.swing": "java.desktop",
          "java.beans": "java.desktop", "javax.sound": "java.desktop", "java.util.logging": "java.logging",
          "javax.xml": "java.xml", "org.w3c.dom": "java.xml", "org.xml.sax": "java.xml", "java.sql": "java.sql",
          "java.net.http": "java.net.http", "sun.misc": "jdk.unsupported"}


def modules():
    text = (ROOT / "tools/dex/make_jre.py").read_text(encoding="utf-8")
    return set(re.search(r'^MODULES = "([^"]+)"', text, re.M).group(1).split(","))


class JreModulesTest(unittest.TestCase):
    def test_framework_jdk_packages_are_in_the_jre(self):
        shipped, missing = modules(), {}
        for source in FRAMEWORK.rglob("*.java"):
            text = source.read_text(encoding="utf-8", errors="replace")
            for prefix, module in OWNERS.items():
                if re.search(r"\b" + re.escape(prefix) + r"\.", text) and module not in shipped:
                    missing.setdefault(module, []).append(str(source.relative_to(ROOT)))
        self.assertEqual(missing, {}, "make_jre.py MODULES lacks modules the framework uses")


if __name__ == "__main__":
    sys.exit(unittest.main())
