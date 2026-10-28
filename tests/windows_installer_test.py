#!/usr/bin/env python3
"""Windows zip/installer packaging from a synthetic release layout.

The zip, manifest checks and NSIS file lists run on any host. The installer
itself is built when makensis is found (argument 1, or on PATH).
"""
import contextlib
import hashlib
import io
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_installer", ROOT / "tools/packaging/windows/build_installer.py")
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)

FILES = {
    "venture.exe": b"MZ launcher",
    "venture-runtime.exe": b"MZ runtime",
    "avcodec-62.dll": b"MZ codec",
    "LICENSE": b"MIT\n",
    "java/jdk/bin/server/jvm.dll": b"MZ jvm",
    "java/jdk/lib/modules": b"\0" * 4096,
    "licenses/a$b/notice.txt": b"dollar in a directory name\n",
}


def make_layout(directory):
    for name, data in FILES.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    manifest = {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, data in FILES.items()}
    (directory / "release-files.json").write_text(json.dumps({"files": manifest}))


def expect_error(function, text):
    try:
        function()
    except RuntimeError as error:
        assert text in str(error), error
    else:
        raise AssertionError("expected failure: " + text)


with tempfile.TemporaryDirectory(prefix="windows-installer-test-") as temporary:
    work = Path(temporary)
    layout = work / "Venture-win64"
    make_layout(layout)
    names = packaging.load_layout(layout)
    assert names == sorted(FILES) + ["release-files.json"], names

    # The zip extracts to Venture/ with exactly the layout's bytes.
    output = work / "out"
    packaging.main(["--layout", str(layout), "--output-dir", str(output), "--no-installer"])
    with zipfile.ZipFile(output / "Venture-windows64.zip") as archive:
        assert archive.testzip() is None
        entries = {info.filename: archive.read(info) for info in archive.infolist()}
    assert set(entries) == {"Venture/" + n for n in names}, sorted(entries)
    for name, data in FILES.items():
        assert entries["Venture/" + name] == data, name
    first = (output / "Venture-windows64.zip").read_bytes()
    packaging.main(["--layout", str(layout), "--output-dir", str(output), "--no-installer"])
    assert (output / "Venture-windows64.zip").read_bytes() == first, "zip is not reproducible"

    # NSIS lists: every file is installed and removed; directories are removed
    # deepest first and never recursively; $ is escaped for NSIS.
    lists = work / "lists"
    lists.mkdir()
    install, removal = packaging.nsis_lists(layout, names, lists)
    script = install.read_text(encoding="utf-8-sig")
    assert 'SetOutPath "$INSTDIR\\java\\jdk\\bin\\server"' in script
    assert 'SetOutPath "$INSTDIR\\licenses\\a$$b"' in script
    assert script.count("File ") == len(names)
    lines = removal.read_bytes().decode("ascii").split("\r\n")[:-1]
    assert lines[:len(names)] == [n.replace("/", "\\") for n in names]
    directories = lines[len(names):]
    assert directories == ["java\\jdk\\bin\\server\\", "java\\jdk\\bin\\", "java\\jdk\\lib\\", "java\\jdk\\",
                           "licenses\\a$b\\", "java\\", "licenses\\"], directories

    # The manifest guards what ships.
    (layout / "stray.dll").write_bytes(b"MZ")
    expect_error(lambda: packaging.load_layout(layout), "differs from its manifest")
    (layout / "stray.dll").unlink()
    (layout / "LICENSE").write_bytes(b"changed\n")
    expect_error(lambda: packaging.load_layout(layout), "changed after packaging")
    (layout / "LICENSE").write_bytes(FILES["LICENSE"])
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            packaging.main(["--layout", str(layout), "--name", "Venture-win64", "--no-installer"])
    except SystemExit as exit:
        assert exit.code == 2
    else:
        raise AssertionError("a name without 'windows' must be rejected")

    makensis = next((Path(c) for c in sys.argv[1:] + [shutil.which("makensis") or ""] if c and Path(c).is_file()), None)
    if makensis is None:
        print("makensis not found; installer build skipped")
    else:
        packaging.main(["--layout", str(layout), "--output-dir", str(output), "--no-zip",
                        "--makensis", str(makensis), "--version", "1.2.3"])
        setup = (output / "Venture-windows64-setup.exe").read_bytes()
        assert setup[:2] == b"MZ" and b"Nullsoft" in setup, "not an NSIS installer"
        pe = int.from_bytes(setup[0x3c:0x40], "little")
        # NSIS ships only x86 stubs; the script itself refuses 32-bit Windows.
        assert setup[pe:pe + 6] == b"PE\0\0\x4c\x01", "installer is not on NSIS's x86 stub"
print("windows installer: ok")
