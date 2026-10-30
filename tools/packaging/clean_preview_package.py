#!/usr/bin/env python3
"""Remove the installer's generated release layout, including incomplete runs."""
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).absolute().parents[2]
sys.path.insert(0, str(ROOT))
from tools.packaging.install_preview import reject_links


def clean(source_root: Path) -> None:
    # This is a fixed build artifact, never the installation or a game library.
    # The packager requires a manifest to reuse output; interrupted packaging
    # can leave this directory populated before that manifest is written.
    layout = source_root / "dist" / "Venture-win64"
    reject_links(layout, tree=True)
    if layout.exists():
        if not layout.is_dir():
            raise RuntimeError("Expected a generated release directory: " + str(layout))
        shutil.rmtree(layout)
    print("Cleaned generated release package: " + str(layout))


if __name__ == "__main__":
    clean(ROOT)
