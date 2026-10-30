#!/usr/bin/env python3
"""Package deployment tests; no compiler, account or Windows installation."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("install_preview", ROOT / "tools/packaging/install_preview.py")
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


class PreviewInstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="venture-preview-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.layout = self.base / "package"
        self.destination = self.base / "preview"
        self.package(b"first")

    def manifest(self):
        files = {p.relative_to(self.layout).as_posix():
                 {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.layout.rglob("*") if p.is_file() and p.name != "release-files.json"}
        (self.layout / "release-files.json").write_text(json.dumps({"files": files}))

    def package(self, payload):
        self.layout.mkdir(exist_ok=True)
        for name in ("venture.exe", "venture-runtime.exe", "venture-passthrough.cmd"):
            (self.layout / name).write_bytes(payload + name.encode())
        self.manifest()

    def legacy_version(self):
        version = hashlib.sha256((self.layout / "release-files.json").read_bytes()).hexdigest()[:24]
        directory = self.destination / "versions" / version
        shutil.copytree(self.layout, directory)
        preview.write_json(self.destination / preview.MARKER, {"owner": preview.OWNER, "schema": 1})
        preview.write_json(self.destination / "current.json",
                           {"owner": preview.OWNER, "version": version, "directory": str(directory)})
        return directory

    def test_install_reuse_and_update_overwrite_one_fixed_directory(self):
        first = preview.install(self.layout, self.destination, "revision-one")
        self.assertEqual(Path(first["directory"]), self.destination)
        self.assertEqual(preview.install(self.layout, self.destination, "revision-one"), first)
        self.package(b"second")
        second = preview.install(self.layout, self.destination, "revision-two")
        self.assertEqual(second["directory"], first["directory"])
        self.assertNotIn("previous_version", second)
        self.assertEqual((self.destination / "venture.exe").read_bytes(), b"secondventure.exe")
        self.assertEqual(json.loads((self.destination / "current.json").read_text()), second)
        self.assertEqual({p.name for p in self.destination.iterdir()},
                         {p.name for p in self.layout.iterdir()} | {preview.MARKER, "current.json"})

    def test_invalid_package_never_creates_an_installation(self):
        (self.layout / "venture.exe").write_bytes(b"changed")
        with self.assertRaises(RuntimeError):
            preview.install(self.layout, self.destination, "invalid")
        self.assertFalse(self.destination.exists())

    def test_copy_failure_does_not_truncate_target_and_retry_repairs_installation(self):
        preview.install(self.layout, self.destination, "first")
        first_bytes = (self.destination / "venture.exe").read_bytes()
        self.package(b"second")
        with patch.object(preview.shutil, "copy2", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                preview.install(self.layout, self.destination, "second")
        self.assertEqual((self.destination / "venture.exe").read_bytes(), first_bytes)
        self.assertFalse((self.destination / "current.json").exists())
        self.assertFalse(list(self.destination.glob("*.tmp")))
        receipt = preview.install(self.layout, self.destination, "second")
        self.assertEqual(receipt["revision"], "second")
        self.assertEqual((self.destination / "venture.exe").read_bytes(), b"secondventure.exe")

    def test_changed_copy_is_not_published(self):
        preview.install(self.layout, self.destination, "first")
        first_bytes = (self.destination / "venture.exe").read_bytes()
        self.package(b"second")
        copy = preview.shutil.copy2
        def corrupt(source, target):
            copy(source, target)
            Path(target).write_bytes(b"incomplete transfer")
        with patch.object(preview.shutil, "copy2", side_effect=corrupt):
            with self.assertRaises(RuntimeError):
                preview.install(self.layout, self.destination, "second")
        self.assertEqual((self.destination / "venture.exe").read_bytes(), first_bytes)
        self.assertFalse((self.destination / "current.json").exists())

    def test_unowned_directory_is_not_modified(self):
        self.destination.mkdir()
        (self.destination / "personal.txt").write_text("keep")
        with self.assertRaises(RuntimeError):
            preview.install(self.layout, self.destination, "first")
        self.assertEqual(list(self.destination.iterdir()), [self.destination / "personal.txt"])

    def test_modified_installed_files_are_replaced(self):
        preview.install(self.layout, self.destination, "first")
        executable = self.destination / "venture.exe"
        executable.write_bytes(b"old or incomplete file")
        preview.install(self.layout, self.destination, "repaired")
        self.assertEqual(executable.read_bytes(), (self.layout / "venture.exe").read_bytes())

    def test_obsolete_packaged_files_removed_but_user_data_preserved(self):
        (self.layout / "lib").mkdir()
        (self.layout / "lib/old.dll").write_bytes(b"obsolete")
        (self.layout / "old-assets").mkdir()
        (self.layout / "old-assets/old.dat").write_bytes(b"obsolete")
        self.manifest()
        preview.install(self.layout, self.destination, "first")
        (self.destination / "lib/personal.txt").write_text("keep")
        shutil.rmtree(self.layout / "lib")
        shutil.rmtree(self.layout / "old-assets")
        self.package(b"second")
        preview.install(self.layout, self.destination, "second")
        self.assertFalse((self.destination / "lib/old.dll").exists())
        self.assertFalse((self.destination / "old-assets").exists())
        self.assertEqual((self.destination / "lib/personal.txt").read_text(), "keep")

    def test_legacy_versions_are_automatically_removed(self):
        first = self.legacy_version()
        self.package(b"second")
        second = self.legacy_version()
        self.package(b"third")
        copy = preview.shutil.copy2
        def copy_after_cleanup(source, target):
            self.assertFalse((self.destination / "versions").exists(),
                             "release old disk space before copying the replacement")
            return copy(source, target)
        with patch.object(preview.shutil, "copy2", side_effect=copy_after_cleanup):
            receipt = preview.install(self.layout, self.destination, "third")
        self.assertEqual(Path(receipt["directory"]), self.destination)
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())
        self.assertFalse((self.destination / "versions").exists())
        self.assertEqual((self.destination / "venture.exe").read_bytes(), b"thirdventure.exe")

    def test_unknown_legacy_content_is_preserved(self):
        known = self.legacy_version()
        personal = known.parent / "personal.txt"
        personal.write_text("keep")
        modified = known.parent / ("a" * 24)
        shutil.copytree(self.layout, modified)
        (modified / "notes.txt").write_text("keep")
        preview.install(self.layout, self.destination, "first")
        self.assertFalse(known.exists())
        self.assertEqual(personal.read_text(), "keep")
        self.assertTrue((modified / "notes.txt").exists())

    def test_linked_destination_is_rejected(self):
        real = self.base / "real"
        real.mkdir()
        try:
            self.destination.symlink_to(real, target_is_directory=True)
        except OSError:
            self.skipTest("Directory links are unavailable")
        with self.assertRaises(RuntimeError):
            preview.install(self.layout, self.destination, "first")
        self.assertEqual(list(real.iterdir()), [])

    def test_linked_legacy_version_is_not_followed(self):
        known = self.legacy_version()
        outside = self.base / "outside"
        known.rename(outside)
        try:
            known.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("Directory links are unavailable")
        with self.assertRaises(RuntimeError):
            preview.install(self.layout, self.destination, "first")
        self.assertEqual((outside / "venture.exe").read_bytes(), b"firstventure.exe")

    def test_old_manifest_cannot_remove_files_outside_installation(self):
        preview.install(self.layout, self.destination, "first")
        personal = self.base / "personal.txt"
        personal.write_text("keep")
        (self.destination / "release-files.json").write_text(json.dumps({"files": {"../personal.txt": {}}}))
        with self.assertRaises(RuntimeError):
            preview.install(self.layout, self.destination, "first")
        self.assertEqual(personal.read_text(), "keep")

    @unittest.skipUnless(os.name == "nt", "Windows file sharing semantics")
    def test_windows_locked_target_is_not_overwritten(self):
        preview.install(self.layout, self.destination, "first")
        self.package(b"second")
        executable = self.destination / "venture.exe"
        with executable.open("rb") as running:
            with self.assertRaises(OSError):
                preview.install(self.layout, self.destination, "second")
            self.assertEqual(running.read(), b"firstventure.exe")
        self.assertEqual(preview.install(self.layout, self.destination, "second")["revision"], "second")


if __name__ == "__main__":
    unittest.main()
