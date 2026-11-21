"""The Flatpak's permissions and entry point (tools/packaging/linux).

Checks that the manifest's finish-args, as build_packages.sh reads them, grant
what Venture needs from Steam and SteamVR, and runs venture-flatpak.sh against
fake sandbox layouts: SteamVR installed with the system (/opt/steamvr, as on
SteamOS for the Steam Frame), SteamVR through Steam's ~/.steam links, a
distribution runtime only visible under /run/host, and a runtime the sandbox
cannot see. On Windows the script runs in Git for Windows' bash, whose
"lnk" symbolic links need no privilege.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGING = ROOT / "tools/packaging/linux"
MANIFEST = PACKAGING / "org.venture_emu.Venture.yml"
WRAPPER = PACKAGING / "venture-flatpak.sh"


def finish_args() -> list[str]:
    # build_packages.sh: sed -n 's/^  - \(--[a-z-]*=.*\|--[a-z-]*\)$/\1/p'
    pattern = re.compile(r"^  - (--[a-z-]*=.*|--[a-z-]*)$")
    return [m.group(1) for line in MANIFEST.read_text(encoding="utf-8").splitlines() if (m := pattern.match(line))]


def check_manifest() -> list[str]:
    args = finish_args()
    errors = []
    wanted = [
        # SteamVR's runtime manifest and client library, wherever it is installed.
        "--filesystem=xdg-data/Steam/steamapps/common/SteamVR:ro",
        "--filesystem=/opt/steamvr:ro",
        # OpenVR's path registry and the client/server shared memory.
        "--filesystem=xdg-config/openvr:ro",
        "--filesystem=xdg-config/openvr/logs:create",
        "--device=shm",
        # The host's IPC namespace: SteamVR's sandboxed client hung vrserver without it.
        "--share=ipc",
        # WiVRn installed as a Flatpak.
        "--filesystem=xdg-data/flatpak/app/io.github.wivrn.wivrn:ro",
        "--filesystem=/var/lib/flatpak/app/io.github.wivrn.wivrn:ro",
        # SteamVR's logs (debug reports) and the client's own log.
        "--filesystem=xdg-data/Steam/logs",
        # Add games to Steam and whether Steam runs.
        "--filesystem=xdg-data/Steam/userdata",
        "--filesystem=xdg-data/Steam/config/loginusers.vdf:ro",
        "--filesystem=~/.steam/steam.pipe",
        # The OpenXR active runtime.
        "--filesystem=xdg-config/openxr:ro",
    ]
    for arg in wanted:
        if arg not in args:
            errors.append(f"manifest lacks {arg}")
    for arg in args:
        if re.fullmatch(r"--filesystem=(home|host|~|xdg-data/Steam|/opt)(:r[ow])?", arg):
            errors.append(f"manifest grants more than it needs: {arg}")
    return errors


SCRIPT = r'''
set -u
fail=0
check() {  # NAME ACTUAL EXPECTED
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2', expected '$3'"; fail=1; fi
}
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/bin"
# The entry point with the host mount moved into the fixture, ending in a dump of what it exported.
sed -e "s#^host=/run/host\$#host=$root/runhost#" \
    -e 's#^exec /app/venture/venture "\$@"$#echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-}"; echo "VR_PATHREG_OVERRIDE=${VR_PATHREG_OVERRIDE:-}"; echo "VR_LOG_PATH=${VR_LOG_PATH:-}"#' \
    "$WRAPPER" > "$root/bin/wrapper.sh"
grep -q "^host=$root/runhost\$" "$root/bin/wrapper.sh" || { echo "FAIL the entry point has no host=/run/host line"; exit 1; }
run() {  # HOME: the entry point's XR_RUNTIME_JSON and VR_PATHREG_OVERRIDE
  env -u XR_RUNTIME_JSON -u VR_PATHREG_OVERRIDE -u VR_LOG_PATH HOME="$1" XDG_RUNTIME_DIR="$1/run" FLATPAK_ID=org.venture_emu.Venture \
    VK_DRIVER_FILES=none sh "$root/bin/wrapper.sh"
}
value() { sed -n "s/^$1=//p"; }

# 1. SteamVR installed with the system (/opt/steamvr on the Steam Frame), the
#    user's manifest a link to it, OpenVR's registry in ~/.config/openvr.
h=$root/frame; steamvr=$root/opt/steamvr
mkdir -p "$h/.config/openxr/1" "$h/.config/openvr" "$h/run" "$steamvr/bin/linuxarm64"
echo '{"runtime":{"name":"SteamVR","library_path":"bin/linuxarm64/vrclient.so"}}' > "$steamvr/steamxr_linuxarm64.json"
: > "$steamvr/bin/linuxarm64/vrclient.so"
mkdir -p "$h/.local/share/Steam/logs"
printf '{\n  "config" : [ "%s" ],\n  "log" : [\n    "%s"\n  ],\n  "runtime" : [ "%s" ]\n}\n' \
  "$h/.local/share/Steam/config" "$h/.local/share/Steam/logs" "$steamvr" > "$h/.config/openvr/openvrpaths.vrpath"
ln -s "$steamvr/steamxr_linuxarm64.json" "$h/.config/openxr/1/active_runtime.json"
out=$(run "$h")
check "system SteamVR manifest" "$(echo "$out" | value XR_RUNTIME_JSON)" "$(readlink -f "$steamvr/steamxr_linuxarm64.json")"
check "OpenVR registry" "$(echo "$out" | value VR_PATHREG_OVERRIDE)" "$h/.config/openvr/openvrpaths.vrpath"
check "client logs where the registry says" "$(echo "$out" | value VR_LOG_PATH)" ""

# 1b. The registry's log folder is not in the sandbox (another Steam layout):
#     the client logs to ~/.config/openvr/logs.
h2=$root/frame-logs
mkdir -p "$h2/.config/openvr" "$h2/run"
printf '{ "log" : [ "/nonexistent-venture-test/.steam/debian-installation/logs" ], "runtime" : [ "%s" ] }\n' \
  "$steamvr" > "$h2/.config/openvr/openvrpaths.vrpath"
out=$(run "$h2")
check "client log moved to ~/.config/openvr/logs" "$(echo "$out" | value VR_LOG_PATH)" "$h2/.config/openvr/logs"
[ -d "$h2/.config/openvr/logs" ] || { echo "FAIL ~/.config/openvr/logs was not created"; fail=1; }

# 2. SteamVR in the Steam folder through Steam's ~/.steam/steam link, which the sandbox lacks.
h=$root/steam; common=$h/.local/share/Steam/steamapps/common/SteamVR
mkdir -p "$h/.config/openxr/1" "$h/run" "$common"
echo '{"runtime":{"name":"SteamVR","library_path":"bin/linux64/vrclient.so"}}' > "$common/steamxr_linux64.json"
ln -s "$h/.steam/steam/steamapps/common/SteamVR/steamxr_linux64.json" "$h/.config/openxr/1/active_runtime.json"
out=$(run "$h")
check "SteamVR through ~/.steam/steam" "$(echo "$out" | value XR_RUNTIME_JSON)" "$(readlink -f "$common/steamxr_linux64.json")"
check "no OpenVR registry, none set" "$(echo "$out" | value VR_PATHREG_OVERRIDE)" ""

# 3. A distribution runtime: the manifest links into the host's /usr/share and
#    names an absolute library, both only under /run/host.
h=$root/distro; name=openxr_venture_wrapper_test_$$
mkdir -p "$h/.config/openxr/1" "$h/run" "$root/runhost/usr/share/openxr/1" "$root/runhost/usr/lib"
echo "{\"runtime\":{\"name\":\"Test\",\"library_path\":\"/usr/lib/lib$name.so\"}}" > "$root/runhost/usr/share/openxr/1/$name.json"
: > "$root/runhost/usr/lib/lib$name.so"
ln -s "/usr/share/openxr/1/$name.json" "$h/.config/openxr/1/active_runtime.json"
out=$(run "$h")
json=$(echo "$out" | value XR_RUNTIME_JSON)
check "host runtime rewritten into the runtime dir" "$json" "$h/run/app/org.venture_emu.Venture/active_runtime.json"
check "host runtime library under /run/host" "$(sed -n 's/.*"library_path": "\([^"]*\)".*/\1/p' "$json" 2>/dev/null)" "$root/runhost/usr/lib/lib$name.so"

# 4. A runtime the sandbox cannot see: nothing is set, and the launcher warns.
h=$root/hidden
mkdir -p "$h/.config/openxr/1" "$h/run"
ln -s "/nonexistent-venture-test/steamvr/steamxr_linux64.json" "$h/.config/openxr/1/active_runtime.json"
out=$(run "$h")
check "invisible runtime" "$(echo "$out" | value XR_RUNTIME_JSON)" ""

# 5. The user's own choices stay.
out=$(XR_RUNTIME_JSON=/x/runtime.json VR_PATHREG_OVERRIDE=/x/openvrpaths.vrpath HOME="$root/frame" \
      XDG_RUNTIME_DIR="$root/frame/run" VK_DRIVER_FILES=none sh "$root/bin/wrapper.sh")
check "XR_RUNTIME_JSON kept" "$(echo "$out" | value XR_RUNTIME_JSON)" "/x/runtime.json"
check "VR_PATHREG_OVERRIDE kept" "$(echo "$out" | value VR_PATHREG_OVERRIDE)" "/x/openvrpaths.vrpath"
exit $fail
'''


def shell() -> Path | None:
    if os.name != "nt":
        return Path(shutil.which("bash") or "/bin/sh")
    # A bare "bash" can resolve to System32's WSL launcher.
    git = shutil.which("git")
    if git:
        for root in list(Path(git).resolve().parents)[:3]:
            if (root / "usr/bin/bash.exe").exists():
                return root / "usr/bin/bash.exe"
    found = shutil.which("bash")
    return Path(found) if found and "system32" not in found.lower() else None


def check_wrapper() -> list[str]:
    bash = shell()
    if not bash:
        print("skip: no POSIX shell for venture-flatpak.sh")
        return []
    env = dict(os.environ, WRAPPER=WRAPPER.as_posix(), MSYS="winsymlinks:lnk")
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "wrapper_test.sh"
        script.write_bytes(SCRIPT.encode())
        result = subprocess.run([str(bash), script.as_posix()], env=env, capture_output=True, text=True, timeout=120)
    print(result.stdout, end="")
    if result.returncode:
        print(result.stderr, end="", file=sys.stderr)
        return ["venture-flatpak.sh: see the FAIL lines above"]
    return []


def main() -> int:
    errors = check_manifest() + check_wrapper()
    for e in errors:
        print("FAIL", e)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
