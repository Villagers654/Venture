#!/usr/bin/env python3
"""Run the actual launcher offline against private synthetic game libraries."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def profile(root):
    root.mkdir(parents=True)
    (root / "settings.json").write_text(json.dumps({
        "addGamesToSteam": False, "checkForUpdates": False, "logLevel": 2}))
    for i in range(136):
        package = f"com.fixture.game{i}"
        folder = root / "games" / package / "1"
        folder.mkdir(parents=True)
        (folder / "fixture.apk").write_bytes(b"fixture")
        (folder / "plan.json").write_text(json.dumps({
            "appId": str(100000 + i), "binaryId": "1", "package": package,
            "version": "1", "versionCode": "1", "installedAddons": [],
            "files": [{"id": "1", "name": "fixture.apk", "kind": "apk", "size": 7}]}))


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="venture-launcher-idle-") as temporary:
        for mode in ("focus", "hidden"):
            root = Path(temporary) / mode
            profile(root)
            env = dict(os.environ, VENTURE_DATA_DIR=str(root),
                       SDL_VIDEO_DRIVER="dummy", SDL_RENDER_DRIVER="software")
            report = root / "report.json"
            result = subprocess.run([binary, mode, "--offline", "--smoke-seconds", "6",
                                     "--smoke-report", str(report)], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            print(result.stdout, end="")
            if result.returncode:
                raise RuntimeError(f"Launcher {mode} probe failed: exit {result.returncode}")
            state = json.loads(report.read_text())
            if state["installed"] != 136 or state["exit"]["reason"] != "timeout":
                raise RuntimeError(f"Launcher {mode} did not complete its offline UI test")
            if not report.with_suffix(".bmp").is_file():
                raise RuntimeError("The final smoke screenshot was not produced")
    print("PASS focused idle search, subsequent text input, hidden workers and window restore")


if __name__ == "__main__":
    main()
