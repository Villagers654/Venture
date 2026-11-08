"""Materialize SDK headers/JSON from the reviewed SteamworksSDK mirror commits.

Input is an explicit local clone, never the Steam client or credential stores.
Only public headers and steam_api.json enter the worktree; no redistributables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

VERSIONS = {'163': '494c2d680b9e47bbc369496b57568f44ef2f6796',
            '164': 'e60a229cd69811462f467b43826e71b199901c32',
            '165': 'df2baabf574a738ef1ea90a7e89339107fc0a279'}
ROOT = Path(__file__).resolve().parents[3]


CPP_INTERFACES = {
    '164': ['ISteamApps', 'ISteamFriends', 'ISteamMatchmaking',
            'ISteamMatchmakingServers', 'ISteamUser', 'ISteamUtils'],
}


def android_identity(binary):
    """Record the build ID and dynamic ABI data from the pinned redistributable."""
    shoff = struct.unpack_from('<Q', binary, 40)[0]
    stride, count, names_index = struct.unpack_from('<HHH', binary, 58)

    def section(i):
        return struct.unpack_from('<IIQQQQIIQQ', binary, shoff + stride * i)

    def payload(header):
        offset, size = header[4:6]
        return binary[offset:offset + size]

    names = payload(section(names_index))
    sections = {}
    for i in range(count):
        header = section(i)
        start = header[0]
        name = names[start:names.index(b'\0', start)]
        sections[name] = payload(header)
    note = sections[b'.note.gnu.build-id']
    name_size, desc_size, kind = struct.unpack_from('<III', note)
    if name_size != 4 or kind != 3 or note[12:16] != b'GNU\0' or len(note) < 16 + desc_size:
        raise ValueError('invalid GNU build ID note')
    hashes = ''.join(hashlib.sha256(sections[name]).hexdigest()
                     for name in (b'.dynsym', b'.dynstr', b'.rela.dyn', b'.rela.plt'))
    return dict(android_build_id=note[16:16 + desc_size].hex(),
                android_abi_sha256=hashlib.sha256(hashes.encode('ascii')).hexdigest())


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
        pin = dict(commit=commit, files=hashes, android_sha256=hashlib.sha256(binary).hexdigest())
        if cpp := CPP_INTERFACES.get(version):
            pin.update(cpp_interfaces=cpp, **android_identity(binary))
        pins[version] = pin
    (ROOT / 'third_party/steamworks/pins.json').write_text(json.dumps(pins, indent=2) + '\n', newline='\n')


if __name__ == '__main__':
    main()
