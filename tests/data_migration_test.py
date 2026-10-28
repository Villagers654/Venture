"""Production data migration exercised by isolated native processes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from fixture_wait import HANG_SECONDS, wait_for
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.bootstrap import junction
from tools.remove_worktree import remove_tree

CHILD = sys.argv.pop(1)


class DataMigrationTest(unittest.TestCase):
    def run_children(self, linked=False, count=8, current_env=False):
        root = Path(tempfile.mkdtemp(prefix="venture-process-migrate-"))
        children = []
        try:
            (root / ("current" if linked else "legacy")).mkdir()
            (root / ("current" if linked else "legacy") / "save").write_text("unchanged save")
            if linked:
                junction(root / "legacy", root / "current")
            env = {**os.environ, "VENTURE_DATA_DIR": str(root / "current"), "LOCALAPPDATA": str(root),
                   "QP_MOCK_INPUT": "legacy-route"}  # rebrand: legacy fixture
            env.pop("VENTURE_MOCK_INPUT", None)
            expected = "unset"
            if current_env:
                env["VENTURE_MOCK_INPUT"] = expected = "current-route"
            for i in range(count):
                children.append(subprocess.Popen([CHILD, str(root), f"ready-{i}", "20", expected],
                                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT))
            wait_for(lambda: all((root / f"ready-{i}").exists() for i in range(count)),
                     "children reached startup barrier")
            (root / "go").write_text("go")
            for child in children:
                output = child.communicate(timeout=HANG_SECONDS)[0]
                self.assertEqual(child.returncode, 0, output.decode(errors="replace"))
                self.assertNotIn(b"migration failed", output)
                self.assertEqual(output.count(b"Legacy QP_* settings are ignored"),1)
            self.assertEqual((root / "current/save").read_text(), "unchanged save")
            if linked:
                self.assertTrue(os.path.lexists(root / "legacy"))
                self.assertTrue((root / "legacy").is_junction() if os.name == "nt" else (root / "legacy").is_symlink())
                self.assertEqual((root / "legacy/save").read_text(), "unchanged save")
            else:
                self.assertFalse(os.path.lexists(root / "legacy"))
        finally:
            # These are exact Popen handles owned by this test, never name matches.
            for child in children:
                if child.poll() is None:
                    child.terminate(); child.communicate(timeout=10)
            remove_tree(("\\\\?\\" if os.name == "nt" else "") + str(root))

    def test_simultaneous_process_startup_migrates_once(self):
        self.run_children()

    def test_legacy_junction_to_current_is_retained_without_warnings(self):
        self.run_children(linked=True)

    def test_new_host_environment_overrides_legacy(self):
        self.run_children(count=2, current_env=True)


if __name__ == "__main__":
    unittest.main()
