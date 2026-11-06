#!/usr/bin/env python3
"""The translation checks CI runs: the committed source catalog matches the
marked strings (i18n_extract.py --check) and every translation is valid
(i18n_compile.py --check)."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import i18n_compile  # noqa: E402
import i18n_extract  # noqa: E402

if __name__ == "__main__":
    extracted = i18n_extract.main(["--check"])
    compiled = i18n_compile.main(["--check"])
    sys.exit(1 if extracted or compiled else 0)
