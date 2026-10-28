"""Shared, read-only data-root resolution for every Python tool.

The native shared paths module performs migration at application startup. Tools
never migrate the owner's data; they keep using the existing root/cache until then.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Mapping

LEGACY_DATA_ENV = "QUESTPORT_DATA_DIR"  # rebrand: legacy branch interoperability
LEGACY_WINDOWS_DIR = "QuestPort"  # rebrand: legacy data directory
LEGACY_POSIX_DIR = "questport"  # rebrand: legacy data directory
LEGACY_POLICY_ENV = {
    "VENTURE_GPU_DEADLINE": "QUESTPORT_GPU_DEADLINE",  # rebrand: legacy main policy
    "VENTURE_GPU_POLICY_ADMITTED": "QUESTPORT_GPU_POLICY_ADMITTED",  # rebrand: legacy main policy
    "VENTURE_STOP_EVENT": "QP_STOP_EVENT",  # rebrand: legacy main policy
    "VENTURE_ANGLE_DIR": "QP_ANGLE_DIR",  # rebrand: legacy main policy
}


# Default runtime of the platform's release build, relative to the checkout.
RUNTIME = ("build/windows-release/bin/venture-runtime.exe" if os.name == "nt"
           else "build/linux-release/bin/venture-runtime")


POLICY_ENV = ("VENTURE_GPU_POLICY_ADMITTED", "VENTURE_GPU_DEADLINE", "VENTURE_STOP_EVENT", "VENTURE_GPU_PRIORITY",
              "VENTURE_GPU_QUIET_METRICS", "VENTURE_GPU_QUIET_OWNER",
              "VENTURE_GPU_QUIET_CONTROL", "VENTURE_GPU_QUIET_REQUEST",
              "VENTURE_QUIET_WINDOW_ID", "VENTURE_QUIET_WINDOW_REPORT",
              "VENTURE_GPU_MEASUREMENT_CONTROL")


def policy_path(root: Path) -> Path:
    """Prefer the shared checkout's policy; bare repositories have no such checkout."""
    root = root.resolve()
    try:
        common = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            cwd=root, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        common = ""
    if common:
        shared = Path(common).parent / "tools/gpu_policy.py"
        if shared.is_file():
            return shared.resolve()
    local = root / "tools/gpu_policy.py"
    if not local.is_file():
        raise FileNotFoundError(f"No admission policy in checkout {root}")
    return local


def isolated_environment(env: Mapping[str, str], prefixes: tuple[str, ...]) -> dict[str, str]:
    """Drop workload tuning flags but keep the outer policy's admission, deadline, stop event and lane.

    The rebrand moved those policy names under the VENTURE_ prefix that run tools strip.
    """
    return {k: v for k, v in env.items() if k in POLICY_ENV or not k.startswith(prefixes)}


def policy_value(env: Mapping[str, str], key: str, default=None):
    """Bridge the current main policy while worktrees transition independently."""
    return env.get(key, env.get(LEGACY_POLICY_ENV.get(key, ""), default))


def compatibility_environment(env: Mapping[str, str]) -> dict[str, str]:
    """Keep main's admission/shutdown protocol usable by unmerged binaries."""
    result = dict(env)
    for current, legacy in {**LEGACY_POLICY_ENV, "VENTURE_DATA_DIR": LEGACY_DATA_ENV}.items():
        value = env.get(current) or env.get(legacy)
        if value:
            result[current] = result[legacy] = value
    return result


def compatible_status(status: dict) -> dict:
    """Read old main's CLI and saved measurement status without changing it.

    Temporary for live CLI cross-use; retained for historical measurement reads.
    """
    result = dict(status)
    for old, current in ((f"{LEGACY_POSIX_DIR}_runs", "venture_runs"),
                         (f"other_{LEGACY_POSIX_DIR}_runs", "other_venture_runs"),
                         (f"{LEGACY_POSIX_DIR}_runtime_workers", "venture_runtime_workers")):
        for prefix in ("", "segment_"):
            for suffix in ("", "_start", "_end"):
                old_key, new_key = prefix + old + suffix, prefix + current + suffix
                if new_key not in result and old_key in result:
                    result[new_key] = result[old_key]
    return result


def data_dir(env: Mapping[str, str] | None = None, *, windows: bool | None = None,
             home: Path | None = None) -> Path:
    env = os.environ if env is None else env
    for key in ("VENTURE_DATA_DIR", LEGACY_DATA_ENV):
        if env.get(key):
            return Path(env[key])
    windows = os.name == "nt" if windows is None else windows
    home = Path.home() if home is None else home
    if windows:
        base = Path(env.get("LOCALAPPDATA") or home / "AppData/Local")
        current, legacy = base / "Venture", base / LEGACY_WINDOWS_DIR
    else:
        xdg = Path(env.get("XDG_DATA_HOME", ""))
        base = xdg if xdg.is_absolute() else Path(env.get("HOME") or home) / ".local/share"
        current, legacy = base / "venture", base / LEGACY_POSIX_DIR
    # Tool provisioning follows the transition precedence even before either
    # default exists; native startup alone owns the atomic migration.
    return current if current.exists() else legacy


if __name__ == "__main__":
    # CMake consumes this CLI's bytes as UTF-8, including non-ANSI overrides.
    sys.stdout.reconfigure(encoding="utf-8")
    print(data_dir())
