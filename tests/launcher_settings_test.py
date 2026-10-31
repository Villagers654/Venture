"""Exercise the real settings command in fresh processes, without accounts/devices."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="venture-settings-cli-") as temporary:
    root = Path(temporary)
    (root / "settings.json").write_text(json.dumps({"checkForUpdates": False, "addGamesToSteam": False}))
    env = dict(os.environ, VENTURE_DATA_DIR=temporary)
    def run(*args):
        result = subprocess.run([binary, "--offline", "--json", "settings", *args], env=env,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads(result.stdout)
    assert run()["steam-frame-passthrough"] is False
    for value in ("on", "off", "on", "off"):
        run("set", "steam-frame-passthrough", value)
        assert run()["steam-frame-passthrough"] == (value == "on")
        saved = json.loads((root / "settings.json").read_text())
        assert saved["steamFramePassthrough"] == (value == "on")
        assert saved["checkForUpdates"] is False and saved["addGamesToSteam"] is False
print("PASS saved Steam Frame preference across fresh launcher processes")
