#!/usr/bin/env python3
"""Recover real incomplete package output without rebuilding or touching installs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.packaging.clean_preview_package import clean
from tools import package_release as release


class PreviewPackageTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="venture-preview-package-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "checkout"
        self.layout = self.root / "dist/Venture-win64"
        self.layout.mkdir(parents=True)
        self.keep = Path(temporary.name) / "installed preview"
        self.keep.mkdir()
        (self.keep / "settings.json").write_text("keep installed settings")

    def test_incomplete_package_is_recreated_and_can_be_retried_after_another_failure(self):
        (self.layout / "old.dll").write_bytes(b"stale")
        build = self.root / "build/windows-release"
        binary = build / "bin"
        binary.mkdir(parents=True)
        suffix = ".exe" if os.name == "nt" else ""
        for name in ("venture", "venture-runtime"):
            (binary / (name + suffix)).write_bytes(b"compiled")
        def package():
            release.package(build, self.layout, self.root / "angle", "unused-strip", "unused-readobj")
        with self.assertRaisesRegex(RuntimeError, "Output exists without a release manifest"):
            package()
        for _ in range(2):
            clean(self.root)
            # Exercise the real packager: it now reaches copying build output.
            # Deliberately omit the JAR to interrupt packaging before a manifest.
            with self.assertRaisesRegex(RuntimeError, "Missing shipping file:.*venture-framework.jar"):
                package()
            self.assertEqual((self.layout / ("venture" + suffix)).read_bytes(), b"compiled")
            self.assertFalse((self.layout / "old.dll").exists())
            self.assertFalse((self.layout / "release-files.json").exists())
        clean(self.root)
        self.assertFalse(self.layout.exists())
        self.assertTrue((binary / ("venture" + suffix)).is_file())
        self.assertEqual((self.keep / "settings.json").read_text(), "keep installed settings")

    def test_completed_package_and_empty_output_can_be_cleaned_repeatedly(self):
        (self.layout / "release-files.json").write_text('{"files": {}}')
        clean(self.root)
        clean(self.root)
        self.assertFalse(self.layout.exists())

    def test_directory_links_are_rejected_before_deletion(self):
        for top_level in (False, True):
            link = self.layout if top_level else self.layout / "java"
            if top_level:
                self.layout.rmdir()
            if os.name == "nt":
                subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(self.keep)],
                               check=True, capture_output=True)
            else:
                link.symlink_to(self.keep, target_is_directory=True)
            try:
                with self.assertRaisesRegex(RuntimeError, "links"):
                    clean(self.root)
                self.assertTrue((self.keep / "settings.json").is_file())
            finally:
                if os.name == "nt":
                    link.rmdir()
                else:
                    link.unlink()


if __name__ == "__main__":
    unittest.main()
