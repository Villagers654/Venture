"""Materialize SDK headers/JSON from the reviewed SteamworksSDK mirror commits.

Input is an explicit local clone, never the Steam client or credential stores.
Only public headers and steam_api.json enter the worktree; no redistributables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

VERSIONS = {'163': '494c2d680b9e47bbc369496b57568f44ef2f6796',
            '164': 'e60a229cd69811462f467b43826e71b199901c32',
            '165': 'df2baabf574a738ef1ea90a7e89339107fc0a279'}
ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repository', type=Path)
    args = parser.parse_args()
    pins = {}
    def git(*cmd):
        return subprocess.check_output(['git', '-C', str(args.repository), *cmd])
    for version, commit in VERSIONS.items():
        files = git('ls-tree', '-r', '--name-only', commit, 'public/steam').decode().splitlines()
        out = ROOT / 'third_party/steamworks' / f'sdk{version}'
        out.mkdir(parents=True, exist_ok=True)
        hashes = {}
        for path in files:
            if not (path.endswith('.h') or path.endswith('/steam_api.json')):
                continue
            content = git('show', f'{commit}:{path}')
            (out / Path(path).name).write_bytes(content)
            hashes[Path(path).name] = hashlib.sha256(content).hexdigest()
        binary = git('show', f'{commit}:redistributable_bin/androidarm64/libsteam_api.so')
        pins[version] = dict(commit=commit, files=hashes, android_sha256=hashlib.sha256(binary).hexdigest())
    (ROOT / 'third_party/steamworks/pins.json').write_text(json.dumps(pins, indent=2) + '\n', newline='\n')


if __name__ == '__main__':
    main()
