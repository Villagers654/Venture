#!/usr/bin/env python3
"""Package the real build and test shipping boundaries and scanner rejection."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
# Keep each layout private and remove it on success or a Python exception.
# Resolve the parent and verify the created target before recursive cleanup.
layout_parent = Path(sys.argv[sys.argv.index("--build") + 1]).resolve()
layout_temp = tempfile.TemporaryDirectory(prefix="release-layout-test-", dir=layout_parent)
layout_target = (Path(layout_temp.name) / "layout").resolve()
assert layout_target != layout_parent and layout_target.is_relative_to(layout_parent)
sys.argv += ["--output", str(layout_target)]
release.main()
layout = Path(sys.argv[-1])
files = set(json.loads((layout / "release-files.json").read_text())["files"])
actual = {p.relative_to(layout).as_posix() for p in layout.rglob("*") if p.is_file()}
assert actual == files | {"release-files.json"}
# jlink links legal notices on POSIX; the shipped layout must contain no links.
assert not any(p.is_symlink() for p in layout.rglob("*"))
assert "java/jdk/legal/java.logging/LICENSE" in files or os.name == "nt"
expected = {"venture.exe", "venture-runtime.exe", "venture-passthrough.cmd", "libEGL.dll", "libGLESv2.dll", "WebView2Loader.dll"} if os.name == "nt" else {"venture", "venture-runtime", "libEGL.so", "libGLESv2.so"}
expected |= {"venture-framework.jar", "venture-java-bridge.jar", "LICENSE", "release-files.json"}
expected |= {"avutil-60.dll", "avcodec-62.dll", "avformat-62.dll", "swscale-9.dll"} if os.name == "nt" else {
    "libavutil.so.60", "libavcodec.so.62", "libavformat.so.62", "libswscale.so.9", "libsqlite3.so.0"}
assert {p for p in actual if "/" not in p} == expected
directories = {"java", "licenses"}
assert {p.split("/")[0] for p in actual if "/" in p} == directories
assert not any(Path(p).suffix in (".pdb", ".cpp", ".py", ".h", ".a", ".lib") for p in files)
assert not any("mock" in p.lower() or "test" in p.lower() for p in files if not p.startswith("java/jdk/legal/"))
assert "java/jdk/lib/modules" in files
assert "java/dex/classes/ready" in files
assert ("java/agent/venture-jvmti-agent.dll" if os.name == "nt" else
        "java/agent/venture-jvmti-agent.so") in files
assert "java/jdk/bin/javac.exe" not in files and "java/jdk/bin/javac" not in files
assert (layout / "LICENSE").read_bytes() == (ROOT / "LICENSE").read_bytes()
assert (layout / "licenses/venture/MIT.txt").read_bytes() == (ROOT / "LICENSES/MIT.txt").read_bytes()
assert (layout / "licenses/ffmpeg/LGPL-2.1.txt").read_bytes() == (ROOT / "LICENSES/LGPL-2.1.txt").read_bytes()
media_recipe=json.loads((layout / "licenses/ffmpeg/venture-ffmpeg.json").read_text())
from tools.ffmpeg.build_ffmpeg import COMPONENTS
assert set(media_recipe["components"]) == COMPONENTS
assert hashlib.sha256((layout / "licenses/ffmpeg/ffmpeg-8.1.3.tar.xz").read_bytes()).hexdigest() == media_recipe["recipe"]["sha256"]
assert (layout / "licenses/ffmpeg/ffmpeg-8.1.3.tar.xz.asc").is_file()
assert (layout / "licenses/venture/0BSD.txt").read_bytes() == (ROOT / "LICENSES/0BSD.txt").read_bytes()
assert (layout / "licenses/xbyak/COPYRIGHT").read_bytes() == (ROOT / "src/jit/xbyak/COPYRIGHT").read_bytes()
for component in ("fmt", "mcl", "robin_map"):
    assert (layout / "licenses" / component / "LICENSE").is_file()
probes = Path(tempfile.mkdtemp(prefix="release-scan-", dir=layout.parent))
probe = probes / "scan-probe"
try:
    for value, encoding in ((str(ROOT), "utf-8"), (str(ROOT), "utf-16le"),
                            ("D:/build/foreign/source.cpp", "utf-8"),
                            ("D:/build/foreign/source.cpp", "utf-16le")):
        probe.write_bytes(value.encode(encoding))
        try:
            release.scan(probes, [str(ROOT)])
        except RuntimeError:
            pass
        else:
            raise AssertionError("Path scanner accepted a build-tree path")
finally:
    probe.unlink(missing_ok=True)

archive_probe = probes / "scan-probe.jar"
try:
    with zipfile.ZipFile(archive_probe, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("payload", str(ROOT))
    try:
        release.scan(probes, [str(ROOT)])
    except RuntimeError:
        pass
    else:
        raise AssertionError("Path scanner accepted a compressed build-tree path")
finally:
    archive_probe.unlink(missing_ok=True)

if os.name == "nt":
    # Remapping must preserve the image size, imports/exports, and all offsets;
    # a suffix-pooled pointer still sees the identical source basename.
    import struct
    image = bytearray(512)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3c, 64)
    struct.pack_into("<H", image, 70, 1)
    struct.pack_into("<H", image, 84, 0)
    image[88:96] = b".rdata\0\0"
    struct.pack_into("<II", image, 104, 256, 128)
    source = b"/home/first/llvm/libcxxabi/src/private_typeinfo.cpp\0"
    image[160:160 + len(source)] = source
    probe = probes / "remap-probe.exe"
    try:
        probe.write_bytes(image)
        release.remap_pe_source_names(probe)
        after = probe.read_bytes()
        prefix = source.rfind(b"/") + 1
        assert len(after) == len(image)
        assert after[:160] == image[:160]
        assert after[160 + prefix:] == image[160 + prefix:]
        assert after[160:160 + prefix].startswith(b"source/")
        release.scan(probes, [])
    finally:
        probe.unlink(missing_ok=True)

probe = probes / "scan-identity-probe"
try:
    # Host names are whole tokens; account names are whole tokens in path/login
    # context. Third-party words containing or equal to them are not identity.
    for encoding in ("utf-8", "utf-16le"):
        probe.write_bytes("roses\0Primrose\0Rose garden\0orions\0".encode(encoding))
        release.scan(probes, [], ["rose"], ["orion"])
        for value in ("/srv/rose/x", "rose@host", "CORP\\ROSE\0", "built on Orion."):
            probe.write_bytes(value.encode(encoding))
            try:
                release.scan(probes, [], ["rose"], ["orion"])
            except RuntimeError:
                pass
            else:
                raise AssertionError("Identity scanner accepted " + value)
finally:
    probe.unlink(missing_ok=True)

probe = probes / "scan-uri-probe"
try:
    for encoding in ("utf-8", "utf-16le"):
        probe.write_bytes("https://example.org/source/file.cpp\0file:/%s\0".encode(encoding))
        release.scan(probes, [])
finally:
    probe.unlink(missing_ok=True)

if os.name == "nt":
    image = bytearray(1024)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3c, 64)
    struct.pack_into("<H", image, 70, 1)
    struct.pack_into("<H", image, 84, 240)
    struct.pack_into("<H", image, 88, 0x20b)
    struct.pack_into("<II", image, 248, 0x2000, 56)
    image[328:336] = b".rdata\0\0"
    struct.pack_into("<IIII", image, 336, 512, 0x2000, 512, 512)
    codeview = b"RSDS" + bytes(20) + b"D:/build/component/debug.pdb\0"
    struct.pack_into("<IIII", image, 524, 2, len(codeview), 0x2058, 600)
    image[600:600 + len(codeview)] = codeview
    struct.pack_into("<IIII", image, 552, 20, 4, 0x20a8, 680)
    image[680:684] = b"\x01\x00\x00\x00"
    probe = probes / "debug-probe.exe"
    try:
        probe.write_bytes(image)
        release.remap_pe_source_names(probe)
        after = probe.read_bytes()
        assert len(after) == len(image)
        assert after[:512] == image[:512]
        assert after[512:540] == bytes(28)
        assert after[540:568] == image[540:568]
        assert after[600:600 + len(codeview)] == bytes(len(codeview))
        assert after[680:684] == image[680:684]
        release.scan(probes, [])
    finally:
        probe.unlink(missing_ok=True)

probes.rmdir()
print("Shipping boundaries, ASCII/UTF-16/JAR scanner and PE path remapping passed")
