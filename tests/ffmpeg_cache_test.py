"""Shared provisioning and cleanup must never rebuild a valid cache or follow links."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import shutil
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import bootstrap
from tools.ffmpeg import build_ffmpeg as ffmpeg
from tools.remove_worktree import remove_tree


class CacheTest(unittest.TestCase):



    def test_cache_corruption_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            install = base / ('install' if os.name == 'nt' else 'install-posix')
            install.mkdir()
            names = ['bin/avutil-60.dll', 'bin/avcodec-62.dll', 'bin/avformat-62.dll', 'bin/swscale-9.dll'] if os.name == 'nt' else [
                'lib/libavutil.so.60', 'lib/libavcodec.so.62', 'lib/libavformat.so.62', 'lib/libswscale.so.9']
            hashes = {}
            for name in names + ['include/libavcodec/avcodec.h']:
                path = install / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'fixture')
                hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            manifest = dict(lock_sha256=ffmpeg.LOCK_HASH, source_patches=[], recipe=dict(sha256=ffmpeg.LOCK['sha256'],
                version=ffmpeg.LOCK['version'], flags=ffmpeg.LOCK['flags']+['--prefix=../install']+(ffmpeg.LOCK['windows_flags'] if os.name=='nt' else []),
                signature_fingerprint=ffmpeg.LOCK['fingerprint']), components=sorted(ffmpeg.COMPONENTS),
                artifacts={n: hashes[n] for n in names}, headers={'include/libavcodec/avcodec.h': hashes['include/libavcodec/avcodec.h']})
            (install / 'venture-ffmpeg.json').write_text(json.dumps(manifest))
            (install / names[0]).write_bytes(b'corrupt')
            with self.assertRaisesRegex(RuntimeError, 'cache file changed'):
                ffmpeg.validate_cache(base)


if __name__ == '__main__':
    unittest.main()
