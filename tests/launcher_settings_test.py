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
    assert run()["passthrough"] is True  # on by default
    assert run()["mirror"] == "cropped" and run()["mirror-eye"] == "left"
    run("set", "mirror-eye", "right")
    run("set", "mirror", "full")
    assert run()["mirror"] == "full" and run()["mirror-eye"] == "right"
    saved = json.loads((root / "settings.json").read_text())["display"]
    assert saved["mirror"] == "full" and saved["mirrorEye"] == "right"
    run("set", "mirror-eye", "left")
    assert json.loads((root / "settings.json").read_text())["display"]["mirrorEye"] == "left"
    # The game's audio on the PC: off by default (older settings have no key).
    assert run()["pc-audio"] == "off"
    run("set", "pc-audio", "on")
    assert run()["pc-audio"] == "on"
    assert json.loads((root / "settings.json").read_text())["display"]["pcAudio"] == "on"
    run("set", "pc-audio", "off")
    assert json.loads((root / "settings.json").read_text())["display"]["pcAudio"] == "off"
    bad = subprocess.run([binary, "--offline", "--json", "settings", "set", "pc-audio", "loud"], env=env,
                         capture_output=True, text=True, timeout=30)
    assert bad.returncode != 0 and run()["pc-audio"] == "off"
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
    for value in ("off", "on", "off"):
        run("set", "passthrough", value)
        assert run()["passthrough"] == (value == "on")
        saved = json.loads((root / "settings.json").read_text())
        assert saved["mixedReality"] == (value == "on") and "passthrough" not in saved
        assert saved["checkForUpdates"] is False and saved["addGamesToSteam"] is False
        # Contradictory inherited values must lose to the explicit launch setting.
        env["VENTURE_PASSTHROUGH"] = "0" if value == "on" else "1"
        result = command("play", "org.venture.passthrough", "--as", "local")
        assert result["exitCode"] == 0, result
        assert json.loads((install / "launch-options.json").read_text())["passthrough"] == ("1" if value == "on" else "0")
    # VENTURE_PC_AUDIO reaches the runtime: the global choice, then a game's own over it.
    def launched_pc_audio():
        env["VENTURE_PC_AUDIO"] = "inherited"  # the launcher's value wins
        result = command("play", "org.venture.passthrough", "--as", "local")
        assert result["exitCode"] == 0, result
        return json.loads((install / "launch-options.json").read_text())["pcAudio"]
    assert launched_pc_audio() == "off"
    run("set", "pc-audio", "on")
    assert launched_pc_audio() == "on"
    run("set", "pc-audio", "off")
    settings = json.loads((root / "settings.json").read_text())
    settings["gameDisplay"] = {"org.venture.passthrough": {"pcAudio": "on"}}
    (root / "settings.json").write_text(json.dumps(settings))
    assert launched_pc_audio() == "on"
    env.pop("VENTURE_PC_AUDIO")
    # VENTURE_TEXTURE_COMPRESSION(_THRESHOLD) reach the runtime: the global choice, then a
    # game's own over it. Always is the runtime's default and sets neither.
    env.pop("VENTURE_TEXTURE_COMPRESSION", None)
    env.pop("VENTURE_TEXTURE_COMPRESSION_THRESHOLD", None)
    def launched_textures(compression, threshold, game):
        settings = json.loads((root / "settings.json").read_text())
        settings["textureCompression"], settings["textureCompressionThreshold"] = compression, threshold
        settings["gameDisplay"] = {"org.venture.passthrough": game}
        (root / "settings.json").write_text(json.dumps(settings))
        result = command("play", "org.venture.passthrough", "--as", "local")
        assert result["exitCode"] == 0, result
        options = json.loads((install / "launch-options.json").read_text())
        return options["textureCompression"], options["textureCompressionThreshold"]
    assert launched_textures(0, 50, {}) == ("missing", "missing")
    assert launched_textures(1, 75, {}) == ("auto", "75")
    assert launched_textures(2, 50, {}) == ("never", "missing")
    assert launched_textures(2, 50, {"textureCompression": "always"}) == ("missing", "missing")
    assert launched_textures(0, 50, {"textureCompression": "never"}) == ("never", "missing")
    assert launched_textures(0, 25, {"textureCompression": "auto"}) == ("auto", "25")
    assert launched_textures(2, 25, {"textureCompression": "auto", "textureCompressionThreshold": 90}) == ("auto", "90")
    # Older settings, without a game's choice, load and launch unchanged.
    assert launched_textures(1, 50, {"pcAudio": "on"}) == ("auto", "50")
    settings = json.loads((root / "settings.json").read_text())
    assert settings["textureCompression"] == 1 and settings["textureCompressionThreshold"] == 50
    run("set", "steam-frame-passthrough", "on")  # legacy command remains usable
    assert run()["passthrough"] is True
print("PASS saved mixed reality, PC audio and texture compression settings and actual launcher-to-child environment")
