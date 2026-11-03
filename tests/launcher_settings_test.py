"""Exercise the real settings command in fresh processes, without accounts/devices."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

binary = str(Path(sys.argv[1]).resolve())
probe = Path(binary).with_name("venture_passthrough_launch_probe" + Path(binary).suffix)
with tempfile.TemporaryDirectory(prefix="venture-settings-cli-") as temporary:
    root = Path(temporary)
    (root / "settings.json").write_text(json.dumps({"checkForUpdates": False, "addGamesToSteam": False}))
    env = dict(os.environ, VENTURE_DATA_DIR=temporary)
    def command(*args):
        result = subprocess.run([binary, "--offline", "--json", *args], env=env,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads(result.stdout)
    def run(*args):
        return command("settings", *args)
    assert run()["passthrough"] is False
    install = root / "games" / "org.venture.passthrough" / "1"
    install.mkdir(parents=True)
    with zipfile.ZipFile(install / "fixture.apk", "w") as apk:
        apk.writestr("AndroidManifest.xml", bytes.fromhex("0300080008000000"))
    (install / "plan.json").write_text(json.dumps({
        "source": "import", "package": "org.venture.passthrough", "versionCode": 1,
        "files": [{"name": "fixture.apk", "kind": "apk", "size": (install / "fixture.apk").stat().st_size}],
    }))
    run("set", "games-folder", str(root / "games"))
    run("set", "runtime", str(probe))
    for value in ("on", "off", "on", "off"):
        run("set", "passthrough", value)
        assert run()["passthrough"] == (value == "on")
        saved = json.loads((root / "settings.json").read_text())
        assert saved["passthrough"] == (value == "on")
        assert saved["checkForUpdates"] is False and saved["addGamesToSteam"] is False
        # Contradictory inherited values must lose to the explicit launch setting.
        env["VENTURE_PASSTHROUGH"] = "0" if value == "on" else "1"
        result = command("play", "org.venture.passthrough", "--as", "local")
        assert result["exitCode"] == 0, result
        assert json.loads((install / "launch-options.json").read_text())["passthrough"] == ("1" if value == "on" else "0")
    run("set", "steam-frame-passthrough", "on") # legacy command remains usable
    assert run()["passthrough"] is True
print("PASS saved passthrough preference and actual launcher-to-child environment")
