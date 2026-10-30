#!/usr/bin/env python3
"""FFmpeg verifier discovery without changing the user's PATH or keyring."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.ffmpeg import build_ffmpeg as ffmpeg


class GnuPGDiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="venture-gpg-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def pair(self, directory):
        directory.mkdir(parents=True, exist_ok=True)
        result = directory / "gpg.exe", directory / "gpgv.exe"
        for tool in result:
            tool.touch()
        return tuple(map(str, result))

    def test_existing_path_pair_is_reused(self):
        tools = {"gpg": "/existing/gpg", "gpgv": "/existing/gpgv"}
        with patch.object(ffmpeg.shutil, "which", side_effect=tools.get):
            self.assertEqual(ffmpeg.find_gnupg(windows=False), tuple(tools.values()))

    def test_git_cmd_only_path_discovers_bundled_pair(self):
        git = self.root / "Git with spaces/cmd/git.exe"
        git.parent.mkdir(parents=True)
        git.touch()
        expected = self.pair(git.parent.parent / "usr/bin")
        with patch.object(ffmpeg.shutil, "which", side_effect=lambda name: str(git) if name == "git" else None):
            actual = ffmpeg.find_gnupg(windows=True)
            self.assertEqual(tuple(Path(p).resolve() for p in actual), tuple(Path(p).resolve() for p in expected))

    def test_incomplete_pair_cannot_pass(self):
        git = self.root / "Git/cmd/git.exe"
        git.parent.mkdir(parents=True)
        git.touch()
        directory = git.parent.parent / "usr/bin"
        directory.mkdir(parents=True)
        (directory / "gpg.exe").touch()
        with patch.object(ffmpeg.shutil, "which", side_effect=lambda name: str(git) if name == "git" else None), \
             patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "gpg and gpgv"):
                ffmpeg.find_gnupg(windows=True)

    def test_native_install_outside_path_is_discovered(self):
        program_files = self.root / "Program Files"
        expected = self.pair(program_files / "GnuPG/bin")
        with patch.object(ffmpeg.shutil, "which", return_value=None), \
             patch.dict(os.environ, {"ProgramFiles": str(program_files)}, clear=True):
            self.assertEqual(ffmpeg.find_gnupg(windows=True), expected)

    def test_native_verifier_receives_native_paths(self):
        tool = self.root / "native/gpgv.exe"
        value = self.root / "keyring/release.gpg"
        with patch.object(ffmpeg, "shell_path", side_effect=AssertionError("native paths must not be rewritten")):
            self.assertEqual(ffmpeg.gnupg_path(tool, value), str(value))

    @unittest.skipUnless(os.name == "nt", "Windows MSYS path conversion")
    def test_msys_verifier_receives_msys_paths(self):
        tool = self.root / "usr/bin/gpgv.exe"
        tool.parent.mkdir(parents=True)
        (tool.parent / "msys-2.0.dll").touch()
        value = self.root / "signature/release.asc"
        expected = str(value).replace("\\", "/")
        expected = "/" + expected[0].lower() + expected[2:]
        self.assertEqual(ffmpeg.gnupg_path(tool, value), expected)

    @unittest.skipUnless(os.name == "nt", "Real Git for Windows distribution")
    def test_real_git_tools_work_without_usr_bin_on_path(self):
        git = shutil.which("git")
        self.assertTrue(git, "Git for Windows must be installed on this runner")
        with patch.object(ffmpeg.shutil, "which", side_effect=lambda name: git if name == "git" else None):
            tools = ffmpeg.find_gnupg()
        for tool in tools:
            output = subprocess.run([tool, "--version"], check=True, capture_output=True, text=True)
            self.assertIn("GnuPG", output.stdout)


if __name__ == "__main__":
    unittest.main()
