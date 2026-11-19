"""The Venture APK's packaging helpers (tools/android): the launcher icon drawn
from the logo's SVG, the version code, and check_apk's device-free checks on
synthetic APKs (missing entries, compressed or misaligned native libraries, no
signature block). The real APK is built by the android-arm64 preset and checked
by the same script in CI."""
from pathlib import Path
import struct
import sys
import tempfile
import re
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'android'))
sys.path.insert(0, str(ROOT))
import build_apk  # noqa: E402
import check_apk  # noqa: E402
import platform_exports  # noqa: E402

ANDROID = '{http://schemas.android.com/apk/res/android}'


def write_apk(path, entries, sign=True):
    """A zip with page-aligned stored libraries (as zipalign -P 16 leaves them)
    and, with sign, a stand-in v2 signing block before the central directory."""
    with zipfile.ZipFile(path, 'w') as z:
        for name, (data, method) in entries.items():
            info = zipfile.ZipInfo(name)
            info.compress_type = method
            if method == zipfile.ZIP_STORED and name.endswith('.so'):
                # Pad the extra field so the data starts on a 16 KiB boundary.
                offset = z.fp.tell() + 30 + len(name.encode())
                pad = (-offset - 4) % check_apk.PAGE
                info.extra = struct.pack('<HH', 0xD935, pad) + b'\0' * pad
            z.writestr(info, data)
    if not sign:
        return
    data = bytearray(path.read_bytes())
    eocd = data.rfind(b'PK\x05\x06')
    central = struct.unpack('<I', data[eocd + 16:eocd + 20])[0]
    block = struct.pack('<Q', 24) + struct.pack('<Q', 24) + check_apk.SIGNING_BLOCK_MAGIC
    data[central:central] = block
    struct.pack_into('<I', data, eocd + len(block) + 16, central + len(block))
    path.write_bytes(bytes(data))


def elf(machine):
    """An ELF header with the class and machine check_apk reads."""
    return b'\x7fELF' + bytes([1 if machine == 40 else 2]) + b'\0' * 13 + struct.pack('<H', machine)


def complete_entries():
    stored, deflated = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
    a64, a32 = elf(183), elf(40)
    return {
        'AndroidManifest.xml': (b'manifest', deflated),
        'resources.arsc': (b'table', stored),
        'classes.dex': (b'dex\n035', deflated),
        'lib/arm64-v8a/libventure.so': (b'\x7fELF' + b'\0' * 100, stored),
        'lib/arm64-v8a/libopenxr_loader.so': (b'\x7fELF' + b'\0' * 100, stored),
        'assets/quest_layer/libventure_quest_layer.so': (b'\x7fELF', deflated),
        'assets/quest_layer/venture_quest_layer.json': (b'{}', deflated),
        'assets/game/classes.dex': (b'dex\n035', deflated),
        'assets/game/lib/arm64-v8a/libovrplatformloader.so': (a64, deflated),
        'assets/game/lib/arm64-v8a/libvrapi.so': (a64, deflated),
        'assets/game/platform_alias.so': (a64, deflated),
        'assets/game/meta/com.oculus.os.dex': (b'dex\n035', deflated),
        'assets/game/lib/armeabi-v7a/libovrplatformloader.so': (a32, deflated),
        'assets/game/lib/armeabi-v7a/libvrapi.so': (a32, deflated),
        'assets/game/lib/armeabi-v7a/platform_alias.so': (a32, deflated),
        'assets/game/lib/armeabi-v7a/libopenxr_loader.so': (a32, deflated),
        'assets/game/lib/armeabi-v7a/libventure_quest_layer.so': (a32, deflated),
    }


class BuildApkHelpers(unittest.TestCase):
    def test_version_code(self):
        self.assertEqual(build_apk.version_code('0.2.5'), 2005)
        self.assertEqual(build_apk.version_code('1.12.3'), 1012003)
        self.assertEqual(build_apk.version_code('0.3'), 3000)

    def test_icon_from_logo(self):
        xml = build_apk.vector_drawable(ROOT / 'src/launcher/assets/venture.svg')
        root = ET.fromstring(xml)
        self.assertEqual(root.tag, 'vector')
        self.assertEqual(root.get(ANDROID + 'viewportWidth'), '200')
        group = root.find('group')
        self.assertEqual(group.get(ANDROID + 'translateX'), '-28')
        self.assertEqual(group.get(ANDROID + 'translateY'), '-29')
        paths = group.findall('path')
        self.assertEqual(len(paths), 4)
        self.assertEqual(paths[0].get(ANDROID + 'fillColor'), '#0A4FA3')
        self.assertTrue(paths[0].get(ANDROID + 'pathData').startswith('M40 103'))


class CheckApk(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='venture-apk-test-')
        self.apk = Path(self.dir.name) / 'venture.apk'

    def tearDown(self):
        self.dir.cleanup()

    def test_complete_apk_passes(self):
        write_apk(self.apk, complete_entries())
        self.assertEqual(check_apk.check(self.apk), [])

    def test_missing_entry(self):
        entries = complete_entries()
        del entries['classes.dex']
        write_apk(self.apk, entries)
        self.assertIn('missing classes.dex', check_apk.check(self.apk))

    def test_compressed_library(self):
        entries = complete_entries()
        entries['lib/arm64-v8a/libventure.so'] = (b'\x7fELF' * 100, zipfile.ZIP_DEFLATED)
        write_apk(self.apk, entries)
        problems = check_apk.check(self.apk)
        self.assertIn('lib/arm64-v8a/libventure.so must be stored uncompressed', problems)

    def test_other_abi_refused(self):
        entries = complete_entries()
        entries['lib/x86_64/libventure.so'] = (b'\x7fELF', zipfile.ZIP_STORED)
        write_apk(self.apk, entries)
        self.assertIn('lib/x86_64/libventure.so: the APK carries arm64-v8a only', check_apk.check(self.apk))

    def test_misaligned_library(self):
        entries = complete_entries()
        with zipfile.ZipFile(self.apk, 'w') as z:
            for name, (data, method) in entries.items():
                z.writestr(zipfile.ZipInfo(name), data, compress_type=method)
        self.assertIn('lib/arm64-v8a/libventure.so is not aligned to 16 KiB', check_apk.check(self.apk))

    def test_unsigned(self):
        write_apk(self.apk, complete_entries(), sign=False)
        self.assertIn('no APK signature block (v2/v3)', check_apk.check(self.apk))

    def test_game_library_of_the_wrong_abi(self):
        entries = complete_entries()
        entries['assets/game/lib/armeabi-v7a/libvrapi.so'] = (elf(183), zipfile.ZIP_DEFLATED)
        entries['assets/game/lib/x86/libvrapi.so'] = (elf(3), zipfile.ZIP_DEFLATED)
        write_apk(self.apk, entries)
        problems = check_apk.check(self.apk)
        self.assertIn('assets/game/lib/armeabi-v7a/libvrapi.so is not an armeabi-v7a library', problems)
        self.assertIn('assets/game/lib/x86/libvrapi.so: no game libraries for x86', problems)



class PlatformExports(unittest.TestCase):
    def test_every_loader_export_and_handler(self):
        names = platform_exports.export_names(ROOT)
        self.assertEqual(names, sorted(set(names)))
        coverage = [line.split('\t', 1)[0] for line in
                    (ROOT / 'tests/runtime/ovrplatform_coverage.txt').read_text(encoding='utf-8').splitlines()
                    if line and not line.startswith('#') and not line.startswith('name\t')]
        self.assertTrue(set(coverage) <= set(names))
        for name in ('ovr_PlatformInitializeAndroid', 'ovr_IsPlatformInitialized', 'ovrID_ToString'):
            self.assertIn(name, names)
        self.assertTrue(all(n.startswith('ovr') for n in names))  # tools match src/android_game exports.map

    def test_32_bit_signatures(self):
        # The armeabi-v7a library converts each call by these (android_game/platform/a32_call.h).
        table = platform_exports.signatures(ROOT)
        self.assertEqual(table['ovr_PopMessage'], 'w')
        self.assertEqual(table['ovr_Message_GetType'], 'wu')
        self.assertEqual(table['ovr_GetLoggedInUserID'], 'I')
        self.assertEqual(table['ovrKeyValuePair_makeString'], 'Cuu')
        self.assertEqual(table['ovr_PlatformInitializeAndroidAsynchronous'], 'Iuuu')
        self.assertEqual(table['ovr_PlatformInitializeAndroidAsynchronousWithOptions'], 'IuuuKu')
        self.assertEqual(table['ovr_Log_NewEvent'], 'vuPu')
        self.assertEqual(table['ovr_Matchmaking_Browse'], 'IuQ')
        self.assertEqual(table['ovr_MatchmakingOptions_SetEnqueueDataSettingsDouble'], 'vuud')
        names = set(platform_exports.export_names(ROOT))
        self.assertTrue(set(table) <= names)
        self.assertGreater(len(table), 900)
        # Records the conversion does not rebuild leave the export without one.
        self.assertNotIn('ovr_Platform_InitializeStandaloneOculus', table)
        for signature in table.values():
            self.assertRegex(signature, r'^[vwBIfdC][iuIfdPKQ]*$')


class VrApiExports(unittest.TestCase):
    def test_native_library_exports_what_the_pc_runtime_serves(self):
        # libvrapi.so on a headset and the PC runtime's libvrapi.so serve the same entry points.
        register = (ROOT / 'src/runtime/hle/vrapi/register.cpp').read_text(encoding='utf-8')
        served = set(re.findall(r'VENTURE_VRAPI\((vrapi_\w+)\)', register))
        served |= set(re.findall(r'manual\("(vrapi_\w+)"', register))
        served |= set(re.findall(r'"(vrapi_\w+)"', register[register.index('for (auto name : {"vrapi_CreateAndroidSurface'):]))
        served |= set(re.findall(r'"(_Z\d+vrapi_\w+)"', register))
        served.add('vrapi_SetTrackingTransform')
        exported = (ROOT / 'src/android_game/vrapi/exports.map').read_text(encoding='utf-8')
        exported = set(re.findall(r'^\s+(\w+);$', exported, re.M))
        self.assertEqual(exported, served)
        source = (ROOT / 'src/android_game/vrapi/exports.cpp').read_text(encoding='utf-8')
        for name in exported:
            self.assertIn(name + '(', source)
        # The 64-bit-only forms (records with pointers) have ILP32 ones for the
        # armeabi-v7a library, and nothing else does.
        lp64 = ''.join(re.findall(r'#if defined\(__LP64__\)(.*?)#endif', source, re.S))
        guarded = set(re.findall(r'VENTURE_VRAPI_EXPORT [^(]*?\b(vrapi_\w+)\(', lp64))
        ilp32 = (ROOT / 'src/android_game/vrapi/exports_ilp32.cpp').read_text(encoding='utf-8')
        self.assertEqual(set(re.findall(r'VENTURE_VRAPI_EXPORT [^(]*?\b(vrapi_\w+)\(', ilp32)), guarded)
        self.assertIn('vrapi_SubmitFrame2', guarded)


if __name__ == '__main__':
    unittest.main()
