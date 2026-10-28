"""Run actual guest media HLE in a process whose executable has no FFmpeg DLLs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

workspace = Path(__file__).resolve().parents[1]
root = workspace / 'build/media-tests/missing-library-tests'
root.mkdir(parents=True, exist_ok=True)
if not root.resolve().is_relative_to(workspace.resolve()):
    raise RuntimeError('Missing-library test root escaped the workspace')
directory = Path(tempfile.mkdtemp(prefix='probe-', dir=root)).resolve()
try:
    executable = Path(sys.argv[1]).resolve()
    target = directory / executable.name
    shutil.copy2(executable, target)
    result = subprocess.run([str(target), '--no-skip=true',
        '--test-case=NDK missing libraries returns logged unsupported'], cwd=directory)
finally:
    if directory.is_symlink() or directory.is_junction() or not directory.is_relative_to(root.resolve()):
        raise RuntimeError('Refusing unsafe missing-library test cleanup')
    shutil.rmtree(directory)
sys.exit(result.returncode)
