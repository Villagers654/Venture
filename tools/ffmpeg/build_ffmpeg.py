#!/usr/bin/env python3
"""Fetch, authenticate and build the approved LGPL MOV/AAC dependency."""
from pathlib import Path
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import re
import argparse

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import bootstrap

LOCK_FILE = Path(__file__).with_name('ffmpeg.lock.json')
LOCK_HASH = hashlib.sha256(LOCK_FILE.read_bytes()).hexdigest()
LOCK = json.loads(LOCK_FILE.read_text())
COMPONENTS = {'CONFIG_AAC_DECODER', 'CONFIG_H264_DECODER', 'CONFIG_HEVC_DECODER',
              'CONFIG_VP8_DECODER', 'CONFIG_VP9_DECODER', 'CONFIG_MOV_DEMUXER',
              'CONFIG_MATROSKA_DEMUXER', 'CONFIG_VP9_PARSER', 'CONFIG_VP9_SUPERFRAME_SPLIT_BSF'}
if os.name == 'nt':
    COMPONENTS |= {'CONFIG_'+codec+'_'+entry+'_HWACCEL' for codec in ('H264','HEVC','VP9')
                   for entry in ('D3D11VA','D3D11VA2')}
BASE = bootstrap.TOOLCHAINS / ('ffmpeg-' + sys.platform + '-x64-' + LOCK_HASH)


def validate_cache(base):
    """Verify published bytes without invoking configure, make or a compiler."""
    install = base / ('install' if os.name == 'nt' else 'install-posix')
    manifest = json.loads((install / 'venture-ffmpeg.json').read_text())
    if (manifest.get('lock_sha256') != LOCK_HASH or
        manifest['recipe']['signature_fingerprint'] != LOCK['fingerprint'] or
        manifest['recipe']['sha256'] != LOCK['sha256'] or
        manifest['recipe']['version'] != LOCK['version'] or
        manifest['recipe']['flags'][:len(LOCK['flags'])] != LOCK['flags'] or
        (os.name == 'nt' and manifest['recipe']['flags'][len(LOCK['flags'])+1:len(LOCK['flags'])+1+len(LOCK['windows_flags'])] != LOCK['windows_flags']) or
        manifest['source_patches'] != [] or
        set(manifest['components']) != COMPONENTS):
        raise RuntimeError('FFmpeg cache recipe does not match the pinned lock')
    expected = dict(manifest['artifacts'])
    names = {'bin/avutil-60.dll', 'bin/avcodec-62.dll', 'bin/avformat-62.dll', 'bin/swscale-9.dll'} if os.name == 'nt' else {
        'lib/libavutil.so.60', 'lib/libavcodec.so.62', 'lib/libavformat.so.62', 'lib/libswscale.so.9'}
    if set(expected) != names or not manifest['headers']:
        raise RuntimeError('Incomplete FFmpeg cache manifest')
    expected.update(manifest['headers'])
    for name, digest in expected.items():
        path = install / name
        if not path.resolve().is_relative_to(install.resolve()) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('FFmpeg cache file changed: ' + name)
    for name, digest in [('archive/' + LOCK['url'].rsplit('/', 1)[-1], LOCK['sha256']),
                         ('signature/release.asc', LOCK['signature_sha256']),
                         ('key/release-key.asc', LOCK['key_sha256'])]:
        if hashlib.sha256((base / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('FFmpeg authenticated input changed: ' + name)
    return install


def provision(rebuild=False):
    """One serialized build per lock; each worktree gets only a junction."""
    bootstrap.TOOLCHAINS.mkdir(parents=True, exist_ok=True)
    with bootstrap.cache_lock(BASE.with_suffix('.lock')):
        manifest = BASE / ('install' if os.name == 'nt' else 'install-posix') / 'venture-ffmpeg.json'
        if rebuild or not manifest.exists():
            print('[ffmpeg] building shared pinned dependency', flush=True)
            build_dependency()
        try:
            validate_cache(BASE)
        except (OSError, ValueError, KeyError, RuntimeError) as error:
            raise RuntimeError('Invalid FFmpeg cache; run python tools/bootstrap.py --ffmpeg: ' + str(error)) from error
        link = bootstrap.DEPS / 'ffmpeg'
        bootstrap.DEPS.mkdir(parents=True, exist_ok=True)
        if os.path.lexists(link):
            if link.is_symlink() or link.is_junction():
                if link.resolve() == BASE.resolve():
                    print('[ffmpeg] reused verified shared cache', flush=True)
                    return link
                if link.is_symlink():
                    link.unlink()
                else:
                    os.rmdir(link)  # Remove the junction itself, never its target.
            else:
                # Old local builds can contain acceptance evidence. Preserve all
                # bytes; never migrate them into the shared dependency cache.
                saved = link.with_name('ffmpeg-local-before-shared')
                if saved.exists() or link.resolve() != bootstrap.DEPS.resolve() / 'ffmpeg':
                    raise RuntimeError('Refusing to replace an unexpected local FFmpeg directory')
                link.rename(saved)
                print('[ffmpeg] preserved previous local directory', flush=True)
        bootstrap.junction(link, BASE)
        print('[ffmpeg] linked verified shared cache', flush=True)
        return link


def shell_path(path):
    value = str(Path(path).absolute()).replace('\\', '/')
    return '/' + value[0].lower() + value[2:] if os.name == 'nt' else value


def fetch_file(name, url, sha, filename):
    dest = BASE / name
    if not dest.exists():
        bootstrap.download(name, dict(url=url, sha256=sha, file=filename), dest)
    result = dest / filename
    if hashlib.sha256(result.read_bytes()).hexdigest() != sha:
        raise RuntimeError(f'{name}: cached SHA-256 mismatch')
    return result


def verify_source(archive, source):
    expected = {}
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            if not member.isfile():
                continue
            relative = Path(member.name).relative_to(LOCK['strip'])
            target = source / relative
            if target.is_symlink() or target.is_junction() or not target.resolve().is_relative_to(source.resolve()):
                raise RuntimeError('Linked or escaping FFmpeg source file')
            digest = hashlib.sha256(bundle.extractfile(member).read()).hexdigest()
            with target.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
                    raise RuntimeError('Pinned FFmpeg source changed: '+relative.as_posix())
            expected[relative.as_posix()] = digest
    actual = set()
    pending = [source]
    while pending:
        directory = pending.pop()
        if directory.is_symlink() or directory.is_junction():
            raise RuntimeError('Linked FFmpeg source directory')
        for entry in directory.iterdir():
            if entry.is_dir():
                pending.append(entry)
            else:
                actual.add(entry.relative_to(source).as_posix())
    if actual != set(expected):
        raise RuntimeError('Unexpected files in pinned FFmpeg source tree')
    return hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()


def git_bash():
    """Git for Windows' bash. A bare 'bash' can resolve to System32's WSL launcher (Windows searches System32
    before PATH), which has no distribution on CI runners."""
    git = shutil.which('git')
    if git:
        for root in list(Path(git).resolve().parents)[:3]:
            if (root/'usr/bin/bash.exe').exists():
                return root/'usr/bin/bash.exe'
    found = shutil.which('bash')
    if found and 'system32' not in found.lower():
        return Path(found)
    raise RuntimeError('FFmpeg needs Git for Windows bash')


def find_gnupg(windows=None):
    """Use an installed pair, including Git's tools outside the normal PATH."""
    windows = os.name == 'nt' if windows is None else windows
    gpg, gpgv = shutil.which('gpg'), shutil.which('gpgv')
    if gpg and gpgv:
        return gpg, gpgv
    candidates = [Path(tool).parent for tool in (gpg, gpgv) if tool]
    if windows:
        git = shutil.which('git')
        if git:
            candidates += [root / 'usr/bin' for root in list(Path(git).resolve().parents)[:3]]
        for variable in ('ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA'):
            directory = os.environ.get(variable)
            if directory:
                candidates.append(Path(directory) / 'GnuPG/bin')
    suffix = '.exe' if windows else ''
    for directory in candidates:
        pair = directory / ('gpg' + suffix), directory / ('gpgv' + suffix)
        if all(tool.is_file() for tool in pair):
            return tuple(str(tool) for tool in pair)
    raise RuntimeError('GnuPG (gpg and gpgv) is required to verify the pinned FFmpeg release. '
                       'Use a complete Git for Windows installation (including usr/bin/gpg.exe and gpgv.exe), '
                       'or install GnuPG and make both tools available.')


def gnupg_path(tool, path):
    # Git's MSYS executables need /c/... paths; native GnuPG needs C:\... .
    msys = (Path(tool).parent / 'msys-2.0.dll').is_file()
    return shell_path(path) if os.name == 'nt' and msys else str(path)


def build_dependency():
    gpg, gpgv = find_gnupg()  # Diagnose prerequisites before downloading sources.
    print('[ffmpeg] signature tools: ' + gpg + ' / ' + gpgv, flush=True)
    BASE.mkdir(parents=True, exist_ok=True)
    # Use the existing downloader inside the shared toolchain cache.
    # Reuse the already authenticated archive rather than refetching it.
    previous_cache = bootstrap.DOWNLOAD_CACHE
    bootstrap.DOWNLOAD_CACHE = BASE / 'downloads'
    bootstrap.DOWNLOAD_CACHE.mkdir(exist_ok=True)
    filename = LOCK['url'].rsplit('/', 1)[-1]
    previous = previous_cache / filename
    cached = bootstrap.DOWNLOAD_CACHE / filename
    if not cached.exists() and previous.exists():
        if hashlib.sha256(previous.read_bytes()).hexdigest() != LOCK['sha256']:
            raise RuntimeError('Previous FFmpeg archive cache changed')
        shutil.copy2(previous, cached)
    archive = fetch_file('archive', LOCK['url'], LOCK['sha256'], filename)
    if not cached.exists():
        shutil.copy2(archive, cached)
    source = BASE / 'source'
    if not source.exists():
        bootstrap.download('ffmpeg', LOCK, source)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != LOCK['sha256']:
        raise RuntimeError('FFmpeg archive SHA-256 mismatch')
    signature = fetch_file('signature', LOCK['url']+'.asc', LOCK['signature_sha256'], 'release.asc')
    key = fetch_file('key', LOCK['key_url'], LOCK['key_sha256'], 'release-key.asc')
    keyring = BASE / 'keyring'; keyring.mkdir(exist_ok=True)
    # gpgv verifies against only the hash-pinned public key. No user keyring,
    # trust database or gpg-agent/socket is needed in the long hash-keyed path.
    public_keyring = keyring / 'release.gpg'
    subprocess.run([gpg, '--batch', '--yes', '--dearmor', '--output', gnupg_path(gpg, public_keyring), gnupg_path(gpg, key)],
                   check=True, capture_output=True)
    verified = subprocess.run([gpgv, '--homedir', gnupg_path(gpgv, keyring), '--keyring', gnupg_path(gpgv, public_keyring),
                               '--status-fd', '1', gnupg_path(gpgv, signature), gnupg_path(gpgv, archive)],
                              check=True, capture_output=True, text=True)
    if '[GNUPG:] VALIDSIG '+LOCK['fingerprint']+' ' not in verified.stdout:
        raise RuntimeError('FFmpeg signature fingerprint mismatch')
    source_tree_hash = verify_source(archive, source)
    build = BASE / ('build-windows' if os.name == 'nt' else 'build-posix'); build.mkdir(exist_ok=True)
    source_link = build / 'src'
    if not source_link.exists():
        bootstrap.junction(source_link, source)
    if source_link.resolve() != source.resolve():
        raise RuntimeError('FFmpeg source junction points outside the pinned source')
    install = BASE / ('install' if os.name == 'nt' else 'install-posix')
    # Native mingw32-make needs Windows/relative paths, not MSYS /c/... paths.
    native_path = lambda p: str(Path(p).absolute()).replace('\\', '/')
    # Configure embeds these arguments in each DLL. Relative paths preserve
    # reproducibility without publishing a developer's workspace identity.
    flags = list(LOCK['flags']) + ['--prefix=../'+install.name]
    env = dict(os.environ)
    make_flags = []
    if os.name == 'nt':
        flags += LOCK['windows_flags']
        tools = ROOT / '.deps/llvm-mingw/bin'
        compiler_link = BASE / 'llvm-mingw'
        if not compiler_link.exists():
            bootstrap.junction(compiler_link, tools.parent.resolve())
        if compiler_link.resolve() != tools.parent.resolve():
            raise RuntimeError('FFmpeg compiler junction differs from the pinned toolchain')
        flags += ['--target-os=mingw32', '--arch=x86_64', '--enable-cross-compile']
        for flag, tool in [('cc','clang'), ('cxx','clang++'), ('ld','clang'),
                           ('host-cc','clang'), ('windres','llvm-windres'), ('ar','llvm-ar'),
                           ('nm','llvm-nm'), ('ranlib','llvm-ranlib'), ('strip','llvm-strip')]:
            flags += ['--'+flag+'=../llvm-mingw/bin/'+tool+'.exe']
        bash = git_bash()
        shell = bash.parent
        env['PATH'] = str(tools)+os.pathsep+str(shell)+os.pathsep+env['PATH']
        import ctypes
        buffer = ctypes.create_unicode_buffer(32768)
        if not ctypes.windll.kernel32.GetShortPathNameW(str(shell/'sh.exe'), buffer, len(buffer)):
            raise RuntimeError('Cannot resolve the configure shell for GNU Make')
        make_flags = ['SHELL='+buffer.value.replace('\\', '/')]
        make = str(tools/'mingw32-make.exe')
    else:
        flags += ['--cc=clang', '--cxx=clang++', '--host-cc=clang']
        make = 'make'
        bash = 'bash'
    recipe = dict(version=LOCK['version'], sha256=LOCK['sha256'], flags=flags,
                  signature_fingerprint=LOCK['fingerprint'])
    marker = build / 'venture-build-recipe.json'
    if marker.exists() and json.loads(marker.read_text())!=recipe:
        raise RuntimeError('FFmpeg recipe changed; use a fresh owned build directory')
    command = ' '.join(shlex.quote(x) for x in ['src/configure', *flags])
    if not (build/'config.h').exists():
        subprocess.run([str(bash), '-c', command], cwd=build, env=env, check=True)
        marker.write_text(json.dumps(recipe, indent=2)+'\n')
    subprocess.run([make, *make_flags, '-j4'], cwd=build, env=env, check=True)
    subprocess.run([make, *make_flags, 'install'], cwd=build, env=env, check=True)
    config = (build/'config.h').read_text()
    for name in ['GPL', 'NONFREE', 'VERSION3']:
        if '#define CONFIG_'+name+' 0' not in config:
            raise RuntimeError('Unapproved FFmpeg license configuration: '+name)
    if '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' not in config:
        raise RuntimeError('Unexpected generated FFmpeg license')
    components = (build/'config_components.h').read_text().splitlines()
    enabled = [line.split()[1] for line in components if line.endswith(' 1')]
    restricted = [name for name in enabled if name.endswith(('_DECODER','_DEMUXER','_PARSER',
                  '_ENCODER','_MUXER','_PROTOCOL','_BSF','_FILTER','_HWACCEL'))]
    if set(restricted) != COMPONENTS:
        raise RuntimeError('Unapproved FFmpeg components: '+repr(restricted))
    names = ['bin/avutil-60.dll','bin/avcodec-62.dll','bin/avformat-62.dll', 'bin/swscale-9.dll'] if os.name=='nt' else [
        'lib/libavutil.so.60','lib/libavcodec.so.62','lib/libavformat.so.62', 'lib/libswscale.so.9']
    artifacts = {name:hashlib.sha256((install/name).read_bytes()).hexdigest() for name in names}
    imports = {}
    if os.name == 'nt':
        for name in names:
            report = subprocess.check_output([str(tools/'llvm-readobj.exe'), '--coff-imports',
                str(install/name)], text=True)
            dependencies = re.findall(r'^  Name: (.+)$', report, re.MULTILINE)
            allowed = {'avutil-60.dll','avcodec-62.dll','avformat-62.dll','kernel32.dll','bcrypt.dll'}
            if any(d.lower() not in allowed and not d.lower().startswith('api-ms-win-crt-') for d in dependencies):
                raise RuntimeError('Unexpected FFmpeg DLL dependency')
            imports[name] = dependencies
    headers = {p.relative_to(install).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in (install/'include').rglob('*.h')}
    published = install/'venture-ffmpeg.json'
    pending = published.with_suffix('.json.pending')
    pending.write_text(json.dumps(dict(lock_sha256=LOCK_HASH,
        recipe=recipe, components=restricted, headers=headers,
        artifacts=artifacts, imports=imports, source_url=LOCK['url'],
        source_tree_sha256=source_tree_hash, source_patches=[]), indent=2)+'\n')
    pending.replace(published)
    print('Verified LGPL FFmpeg MOV/AAC build:', install, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild', action='store_true')
    provision(parser.parse_args().rebuild)
