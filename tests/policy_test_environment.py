"""Remove inherited policy state before a test installs its own policy inputs."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from venture_paths import POLICY_ENV, LEGACY_POLICY_ENV


def clear_policy_environment(environment):
    for key in POLICY_ENV:
        environment.pop(key, None)
        legacy = LEGACY_POLICY_ENV.get(key)
        if legacy:
            environment.pop(legacy, None)
