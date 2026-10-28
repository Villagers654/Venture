#!/usr/bin/env python3
"""Build and run NDK ARM/Thumb GL callers. Launch through tools/gpu_policy.py."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, default=ROOT / 'build/windows-release/bin/venture-runtime.exe')
    parser.add_argument('--ndk', type=Path, default=ROOT / '.deps/android-ndk')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary_dir = next((args.ndk / 'toolchains/llvm/prebuilt').iterdir()) / 'bin'
    compiler = binary_dir / ('clang.exe' if (binary_dir / 'clang.exe').exists() else 'clang')
    for mode in ('arm', 'thumb'):
        library = args.output.resolve() / f'liba32_gles_{mode}.so'
        subprocess.run([str(compiler), '--target=armv7a-linux-androideabi23', '-m' + mode,
                        '-mfloat-abi=softfp', '-mfpu=neon', '-O2', '-fPIC', '-shared', '-nostdlib',
                        '-Wl,--hash-style=sysv', '-Wl,--pack-dyn-relocs=none',
                        str(ROOT / 'tests/guest/a32_gles.c'), '-lEGL', '-lGLESv3', '-o', str(library)], check=True)
        with (args.output / f'{mode}.log').open('wb') as log:
            process = subprocess.run([str(args.runtime.resolve()), '--run-guest-test', str(library),
                                      '--test-dir', str(args.output.resolve() / mode)],
                                     stdout=log, stderr=subprocess.STDOUT, timeout=60)
        print(f'{mode}: exit {process.returncode}', flush=True)
        if process.returncode:
            raise SystemExit(process.returncode)

if __name__ == '__main__':
    main()
