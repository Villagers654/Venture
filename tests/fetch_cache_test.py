"""Shared dependency source cache (cmake/FetchCache.cmake): one extraction per archive
machine-wide, none with sources present, concurrent configures, per-tree builds."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from remove_worktree import remove_tree
from venture_paths import policy_path
CMAKE = ROOT / '.deps/cmake/bin/cmake.exe' if os.name == 'nt' else Path(shutil.which('cmake') or 'cmake')


def archive_bytes(name):
    archive = io.BytesIO()
    members = ((name + '-1.0/CMakeLists.txt',
                'add_custom_target(' + name + '_target)\n'
                'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/built-here" "")\n'),
               (name + '-1.0/include/' + name + '.h', '#define DEP 1\n'))
    with tarfile.open(fileobj=archive, mode='w:gz') as tar:
        for member, text in members:
            data = text.encode()
            info = tarfile.TarInfo(member)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return archive.getvalue()


class FetchCacheTests(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix='venture-fetch-test-'))
        self.digests = {}
        declarations = ''
        for name in ('dep', 'dep2'):
            data = archive_bytes(name)
            path = self.base / (name + '-1.0.tar.gz')
            path.write_bytes(data)
            self.digests[name] = hashlib.sha256(data).hexdigest()
            declarations += ('FetchContent_Declare(' + name + ' URL "' + path.as_posix() + '" URL_HASH SHA256='
                             + self.digests[name] + ')\n')
        self.source = self.base / 'src'
        self.source.mkdir()
        # Several names in one call: each must come from the shared cache.
        (self.source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.28)\nproject(fixture NONE)\ninclude(FetchContent)\n' + declarations +
            'FetchContent_MakeAvailable(dep dep2)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/dep-source.txt" "${dep_SOURCE_DIR}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/dep2-source.txt" "${dep2_SOURCE_DIR}")\n')
        self.cache = self.base / 'shared'

    def tearDown(self):
        assert self.base.name.startswith('venture-fetch-test-')
        remove_tree(('\\\\?\\' if os.name == 'nt' else '') + str(self.base))

    def shared(self, name):
        return self.cache / (name + '-' + self.digests[name][:16])

    def command(self, name, *flags, cache=True):
        args = [str(CMAKE), '-G', 'Ninja', '-S', str(self.source), '-B', str(self.base / name), *flags]
        if cache:
            args += ['-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=' + (ROOT / 'cmake/FetchCache.cmake').as_posix(),
                     '-DVENTURE_FETCH_CACHE_DIR=' + self.cache.as_posix()]
        if os.name == 'nt':
            args += ['-DCMAKE_MAKE_PROGRAM=' + str(ROOT / '.deps/ninja/ninja.exe')]
        return [sys.executable, str(policy_path(ROOT)), '--build', '--', *args]

    def configure(self, name, *flags, cache=True):
        result = subprocess.run(self.command(name, *flags, cache=cache), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_shared_immutable_sources(self):
        stale = self.cache / ('.ex-dep-' + self.digests['dep'][:16] + '-interrupted')
        (stale / 'dep-1.0').mkdir(parents=True)
        first = self.configure('first')
        self.assertIn('Extracting dep ', first.stdout)
        self.assertIn('Extracting dep2 ', first.stdout)
        self.assertFalse(stale.exists(), 'an interrupted extraction of the same archive is cleared')
        for name in ('dep', 'dep2'):
            self.assertTrue((self.shared(name) / 'include' / (name + '.h')).is_file())
            self.assertTrue(self.shared(name).with_name(self.shared(name).name + '.ok').is_file())
            self.assertEqual((self.base / 'first' / (name + '-source.txt')).read_text(), self.shared(name).as_posix())
            # Dependencies build in their own tree, never in the shared cache.
            self.assertTrue((self.base / 'first/_deps' / (name + '-build') / 'built-here').is_file())
        self.assertFalse(any(p.name.endswith('-build') for p in self.cache.iterdir()))
        cache_text = (self.base / 'first/CMakeCache.txt').read_text()
        self.assertIn('FETCHCONTENT_SOURCE_DIR_DEP:PATH=\n', cache_text)
        self.assertIn('VENTURE_FETCH_SOURCE_DIR_DEP:INTERNAL=' + self.shared('dep').as_posix(), cache_text)

        marker = self.shared('dep') / 'include/dep.h'
        before = marker.stat().st_mtime_ns
        second = self.configure('second')  # a fresh tree: no extraction at all
        self.assertNotIn('Extracting', second.stdout)
        self.assertEqual((self.base / 'second/dep-source.txt').read_text(), self.shared('dep').as_posix())
        fresh = self.configure('first', '--fresh')
        self.assertNotIn('Extracting', fresh.stdout)
        self.assertEqual(marker.stat().st_mtime_ns, before)

    def test_concurrent_fresh_configures_extract_once(self):
        processes = [subprocess.Popen(self.command('tree%d' % i), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True) for i in range(2)]
        outputs = [process.communicate()[0] for process in processes]
        for process, output in zip(processes, outputs):
            self.assertEqual(process.returncode, 0, output)
        for name in ('dep', 'dep2'):
            self.assertEqual(sum(output.count('Extracting %s ' % name) for output in outputs), 1, '\n'.join(outputs))
        self.assertEqual(sorted(p.name for p in self.cache.iterdir() if p.is_dir()),
                         [self.shared('dep').name, self.shared('dep2').name])

    def test_without_cache_keeps_stock_fetchcontent(self):
        self.configure('stock', cache=False)
        self.assertTrue((self.base / 'stock/_deps/dep-src/include/dep.h').is_file())
        self.assertFalse(self.cache.exists())


if __name__ == '__main__':
    unittest.main()
