#!/usr/bin/env python3
"""Generate Steam HLE ABI data from pinned JSON and clang's actual target ABIs."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from emit import Model, emit_layouts, emit_methods

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
INCLUDES = '\n'.join(f'#include "{h}.h"' for h in (
    'steam_api', 'steam_gameserver', 'isteamnetworkingsockets', 'isteamnetworkingutils',
    'steamnetworkingfakeip', 'isteamgamecoordinator', 'isteamdualsense'))


def vtables(dump):
    result = {}
    for name, body in re.findall(r"VFTable indices for '([^']+)'[^\n]*\n(.*?)(?=\n\n|\Z)", dump, re.S):
        result[name] = [(int(i), sig) for i, sig in re.findall(r'^\s*(\d+) \| (.*)$', body, re.M)]
    return result


def layouts(dump):
    result = {}
    for section in dump.split('*** Dumping AST Record Layout')[1:]:
        lines = section.strip().splitlines()
        top = re.match(r'\s*0 \| (?:struct|class|union) (.+)', lines[0])
        if not top:
            continue
        tail = re.search(r'\[sizeof=(\d+),.*?align=(\d+)', section, re.S)
        if not tail:
            continue
        fields = []
        for line in lines[1:]:
            m = re.match(r'\s*(\d+) \|( +)(.*)', line)
            if m:
                fields.append(dict(offset=int(m[1]), indent=len(m[2]), text=m[3]))
        result[top[1].removesuffix(' (empty)')] = dict(size=int(tail[1]), align=int(tail[2]), fields=fields)
    return result


def normalize(sig):
    return re.sub(r'ISteam\w+::(E\w+)', r'\1', re.sub(r'\s+', '', sig).replace('class', '').replace('struct', ''))


def slot(vt, classname, method):
    # Clang includes typedef names in the dump; JSON preserves the same names.
    suffix = f"{classname}::{method['methodname']}(" + ','.join(p['paramtype'] for p in method.get('params', [])) + ')'
    candidates = [i for i, sig in vt[classname] if normalize(suffix) in normalize(sig)]
    if not candidates and not any(f"::{method['methodname']}(" in sig for _, sig in vt[classname]):
        return None  # SDK inline helper, not a host virtual method.
    if not candidates:
        # Flat JSON lowers size_t to uint32; retain the header's native signature
        # separately. Only an unambiguous name may use this fallback.
        candidates = [i for i, sig in vt[classname] if f"::{method['methodname']}(" in sig]
    if len(candidates) != 1:
        raise ValueError(f'no unique MSVC slot: {suffix}: {candidates}')
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, default=ROOT / '.deps/llvm-mingw/bin/clang++.exe')
    parser.add_argument('--cache', required=True, type=Path)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    pins = json.loads((ROOT / 'third_party/steamworks/pins.json').read_text())
    out = ROOT / 'src/runtime/hle/steam/generated'
    out.mkdir(parents=True, exist_ok=True)
    for version, pin in pins.items():
        sdk = ROOT / 'third_party/steamworks' / f'sdk{version}'
        for name, digest in pin['files'].items():
            if hashlib.sha256((sdk / name).read_bytes()).hexdigest() != digest:
                raise ValueError(f'pinned Steam SDK {version} input changed: {name}')
        api = json.loads((sdk / 'steam_api.json').read_text())
        records = [r['struct'] for r in api['callback_structs'] + api['structs'] if r['struct'] not in
                   ('SteamDatagramHostedAddress', 'SteamDatagramGameCoordinatorServerLogin')] + ['CallbackMsg_t', 'ValvePackingSentinel_t']
        probe = [INCLUDES] + [f'char record_{i}[sizeof({r})];' for i, r in enumerate(records)]
        for i, face in enumerate(api['interfaces']):
            method = face['methods'][0]
            params = [f"{p['paramtype']} p{j}" for j,p in enumerate(method.get('params', []))]
            probe.append(f"void probe_{i}({face['classname']}* p" + (', ' if params else '') + ', '.join(params) +
                         f") {{ p->{method['methodname']}(" + ', '.join(f'p{j}' for j in range(len(params))) + '); }')
        source = args.cache / f'probe{version}.cpp'
        source.write_text('\n'.join(probe))
        dumps = {}
        for target, label in [('aarch64-linux-android30','android'), ('x86_64-pc-windows-msvc','windows')]:
            command = [str(args.clang), '--target=' + target, '-std=c++17', '-DSTEAM_API_NODLL',
                       '-I'+str(HERE/'stub'), '-I'+str(sdk), '-Xclang', '-fdump-record-layouts',
                       '-Xclang', '-fdump-vtable-layouts', '-c', str(source), '-o', str(args.cache / f'{version}-{label}.obj')]
            run = subprocess.run(command, capture_output=True, text=True)
            (args.cache / f'{version}-{label}.txt').write_text(run.stdout)
            (args.cache / f'{version}-{label}.err').write_text(run.stderr)
            if run.returncode:
                raise RuntimeError(run.stderr[:4000])
            dumps[label] = run.stdout
        vt = vtables(dumps['windows'])
        methods = []
        for face in api['interfaces']:
            for method in face['methods']:
                index = slot(vt,face['classname'],method)
                methods.append(dict(interface=face['classname'], symbol=method['methodname_flat'], slot=index,
                                    host_signature=dict(vt[face['classname']]).get(index)))
        run_frame = [(i,sig) for i,sig in vt['ISteamUtils'] if '::RunFrame()' in sig]
        if len(run_frame) != 1:
            raise ValueError('SteamUtils private RunFrame slot missing')
        methods.append(dict(interface='ISteamUtils',symbol='SteamAPI_ISteamUtils_RunFrame',slot=run_frame[0][0],host_signature=run_frame[0][1]))
        metadata = dict(sdk=version, android_sha256=pin['android_sha256'], methods=methods,
                        android=layouts(dumps['android']), windows=layouts(dumps['windows']))
        (args.cache / f'abi{version}.json').write_text(json.dumps(metadata, indent=2)+'\n')
        model = Model(api, metadata)
        lines = emit_layouts(model, version, INCLUDES.replace('#include "', f'#include "steamworks/sdk{version}/'))
        lines.insert(1, '#include "hle/steam/bridge.h"')
        methods_cpp, skipped = emit_methods(model,version)
        lines += methods_cpp
        (out / f'unsupported{version}.json').write_text(json.dumps(skipped,indent=2)+'\n', newline='\n')
        lines += ['}', f'namespace venture::steam {{ const Sdk& sdk{version}() {{',
                  f'  static const Sdk value{{{version}, "{pin["android_sha256"]}", v{version}::layouts, v{version}::methods, v{version}::interfaces, v{version}::exports}};',
                  '  return value;', '} }']
        (out / f'sdk{version}.cpp').write_text('\n'.join(lines)+'\n', newline='\n')
        print(f'SDK {version}: {len(methods)} MSVC slots, {len(metadata["android"])} Android records')


if __name__ == '__main__':
    main()
