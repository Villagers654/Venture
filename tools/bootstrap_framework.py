"""Pinned Java tools, shared download cache; invoked by bootstrap.py --jdk.

The bridge may use provision_jdk independently. No machine-wide installation.
"""

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.venture_paths import data_dir
import hashlib
import os
import urllib.request
import zipfile
import io

JDK_URL = 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.8%2B9/OpenJDK21U-jdk_x64_windows_hotspot_21.0.8_9.zip'
JDK_SHA256 = '5dbb74a4edda94c2aae97a1e30f448db0ad9023a9466fe31a8c68377d9dfbac4'

def provision_resources(root):
    """Only AOSP compiled resources; SDK throwing classes never enter our jar."""
    cache = data_dir() / 'dev-cache'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / 'platform-32_r01.zip'
    if not archive.exists():
        urllib.request.urlretrieve('https://dl.google.com/android/repository/platform-32_r01.zip', archive)
    digest = '01d8da1c900e70fcf5da39767d5444e39928935b1a5927055ce749fc348ca7ae'
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
            raise RuntimeError('Android 32 resource archive SHA-256 mismatch')
    target = root / '.deps/framework-res/root/venture/android-32'
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as sdk:
        (target / 'NOTICE.txt').write_bytes(sdk.read('android-12/data/NOTICE.txt'))
        with zipfile.ZipFile(io.BytesIO(sdk.read('android-12/android.jar'))) as jar:
            from dex.class_hierarchy import declaration
            headers = [declaration(jar.read(name)) for name in sorted(jar.namelist())
                       if name.startswith('android/') and name.endswith('.class')]
            # Compiler ancestry, kept outside the resources/runtime JAR. The
            # verifier and application classpath never receive SDK stub classes.
            (root / '.deps/framework-res/android-32.hierarchy.tsv').write_text(
                '# Android API 32, platform archive SHA-256 ' + digest + '\n'
                + '\n'.join(headers) + '\n', encoding='utf-8')
            for name in jar.namelist():
                if name == 'resources.arsc' or name.startswith('res/'):
                    path = target / name
                    if not path.resolve().is_relative_to(target.resolve()):
                        raise RuntimeError('Resource archive path escapes destination')
                    if name.endswith('/'):
                        path.mkdir(parents=True, exist_ok=True)
                    else:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(jar.read(name))
    return target

def provision_jdk(root):
    target = root / '.deps' / 'jdk'
    if target.exists():
        return target
    if os.name != 'nt':
        raise SystemExit('Set up .deps/jdk with a JDK 21 on this platform')
    cache = data_dir() / 'dev-cache'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.8_9.zip'
    if not archive.exists():
        temporary = archive.with_suffix('.framework.download')
        urllib.request.urlretrieve(JDK_URL, temporary)
        with temporary.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != JDK_SHA256:
                raise RuntimeError('Temurin download SHA-256 mismatch')
        temporary.replace(archive)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != JDK_SHA256:
            raise RuntimeError('Temurin cache SHA-256 mismatch')
    staging = root / '.deps' / 'jdk-framework-staging'
    staging.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(staging)
    (staging / 'jdk-21.0.8+9').rename(target)
    staging.rmdir()
    return target
