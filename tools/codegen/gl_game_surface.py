#!/usr/bin/env python3
"""Audit measured APK graphics names against generated and resolved guest thunks.

The r3 graphics extraction is a measured inventory, not an active-path claim.
--verify-apks reopens the owned APK members and checks the measured ELF hashes.
No APK/OBB is changed. Optional procedure candidates may legitimately be absent.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory', type=Path, required=True)
    p.add_argument('--generated', type=Path, required=True, help='CMake generated directory')
    p.add_argument('--resolved', type=Path, help='VENTURE_GFX_PROCS_OUT dump from the HLE smoke test')
    p.add_argument('--verify-apks', action='store_true')
    a = p.parse_args()
    generated = set(re.findall(r'"((?:gl|egl)\w+)"',
        (a.generated/'gl_dispatch.cpp').read_text() + (a.generated/'egl_dispatch.inc').read_text()))
    resolved = set(a.resolved.read_text().splitlines()) if a.resolved else generated
    for game in json.loads(a.inventory.read_text())['games']:
        imports, procedures = set(), set()
        archive = zipfile.ZipFile(game['apk']) if a.verify_apks else None
        try:
            for lib in game['libs']:
                imports.update(n for n in lib['imports'] if n.startswith(('gl', 'egl')))
                procedures.update(n for n in lib['proc_name_strings'] if n.startswith(('gl', 'egl')))
                if archive and (lib['imports'] or lib['proc_name_strings']):
                    digest = hashlib.sha256()
                    with archive.open(lib['member']) as f:
                        while block := f.read(1024*1024):
                            digest.update(block)
                    if digest.hexdigest() != lib['sha256']:
                        raise ValueError(f"APK changed: {game['package']} {lib['member']}")
        finally:
            if archive:
                archive.close()
        print(json.dumps({'package': game['package'], 'abi': game['abi'],
            'imports': len(imports), 'missing_imports': sorted(imports-resolved),
            'procedure_candidates': len(procedures), 'resolved_candidates': len(procedures & resolved),
            'absent_candidates': sorted(procedures-resolved), 'apk_hashes_verified': a.verify_apks}))


if __name__ == '__main__':
    main()
