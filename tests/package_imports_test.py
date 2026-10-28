#!/usr/bin/env python3
"""Windows release import audit: only guaranteed OS DLLs may stay external.

Runs on any host. Synthetic PE32+ files carry regular and delay-load imports;
llvm-readobj (argument 1, or found on PATH) parses them as for a real layout.
"""
import importlib.util
from pathlib import Path
import shutil
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def find_readobj():
    candidates = sys.argv[1:] + ["llvm-readobj", "llvm-readobj.exe"]
    for candidate in candidates:
        found = shutil.which(candidate) or (candidate if Path(candidate).is_file() else None)
        if found:
            return found
    return None


def pe(imports=(), delayed=()):
    """A minimal x86-64 PE image whose single section holds its import tables."""
    rva = 0x1000
    body = bytearray()

    def put(data, align=8):
        while len(body) % align:
            body.append(0)
        offset = len(body)
        body.extend(data)
        return rva + offset

    def thunks(name):
        hint = put(struct.pack("<H", 0) + b"Entry\0", 2)
        return put(struct.pack("<QQ", hint, 0))

    descriptors = []
    for name in imports:
        dll = put(name.encode() + b"\0", 2)
        lookup, address = thunks(name), thunks(name)
        descriptors.append(struct.pack("<IIIII", lookup, 0, 0, dll, address))
    import_table = put(b"".join(descriptors) + b"\0" * 20) if imports else 0
    delay_descriptors = []
    for name in delayed:
        dll = put(name.encode() + b"\0", 2)
        handle = put(b"\0" * 8)
        lookup, address = thunks(name), thunks(name)
        delay_descriptors.append(struct.pack("<8I", 1, dll, handle, address, lookup, 0, 0, 0))
    delay_table = put(b"".join(delay_descriptors) + b"\0" * 32) if delayed else 0
    while len(body) % 0x200:
        body.append(0)

    directories = [(0, 0)] * 16
    if imports:
        directories[1] = (import_table, 20 * (len(imports) + 1))
    if delayed:
        directories[13] = (delay_table, 32 * (len(delayed) + 1))
    image_size = rva + ((len(body) + 0xfff) & ~0xfff)
    optional = struct.pack("<HBBIIIII", 0x20b, 14, 0, len(body), 0, 0, 0, rva)
    optional += struct.pack("<QIIHHHHHHIIIIHHQQQQII", 0x140000000, 0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0,
                            image_size, 0x200, 0, 3, 0x8160, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    optional += b"".join(struct.pack("<II", *d) for d in directories)
    dos = b"MZ" + b"\0" * 58 + struct.pack("<I", 64)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(optional), 0x22)
    section = struct.pack("<8sIIIIIIHHI", b".idata", len(body), rva, len(body), 0x200, 0, 0, 0, 0, 0xc0000040)
    headers = dos + coff + optional + section
    return headers + b"\0" * (0x200 - len(headers)) + bytes(body)


def layout(files):
    directory = Path(tempfile.mkdtemp(prefix="package-imports-"))
    for name, data in files.items():
        target = directory / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return directory


def check(files, expect_missing):
    directory = layout(files)
    try:
        report = release.import_report(directory, readobj)
        missing = {(module, name) for module, (_, names) in report.items() for name in names}
        assert missing == expect_missing, missing
        try:
            release.check_imports(directory, readobj)
            assert not expect_missing
        except RuntimeError as error:
            assert expect_missing, error
            for module, name in expect_missing:
                assert name + " in " + module in str(error), error
        return report
    finally:
        shutil.rmtree(directory)


readobj = find_readobj()
if readobj is None:
    print("llvm-readobj not found; skipping")
    sys.exit(77)

# OS-provided names, in any case, plus API sets and the driver's Vulkan loader.
os_imports = ["KERNEL32.dll", "user32.dll", "api-ms-win-crt-runtime-l1-1-0.dll",
              "ext-ms-win-ntuser-window-l1-1-0.dll", "vulkan-1.dll", "D3D11.dll", "dxgi.dll", "WINSPOOL.DRV"]
report = check({"venture.exe": pe(os_imports, ["dwmapi.dll"])}, set())
assert report["venture.exe"][0] == os_imports + ["dwmapi.dll"], report

# Dev-box-only DLLs are rejected even when present on the build machine.
for name in ["VCRUNTIME140.dll", "MSVCP140.dll", "d3dcompiler_47.dll", "ucrtbased.dll",
             "libc++.dll", "libunwind.dll", "mfplat.dll", "WebView2Loader.dll", "openxr_loader.dll"]:
    check({"venture.exe": pe(["kernel32.dll", name])}, {("venture.exe", name)})

# Delay-loaded imports are audited too.
check({"venture-runtime.exe": pe(["kernel32.dll"], ["SDL3.dll"])}, {("venture-runtime.exe", "SDL3.dll")})

# Shipped DLLs resolve (case-insensitively) beside the module, at the layout
# root and in the JRE's bin directories, and are themselves audited.
check({"venture-runtime.exe": pe(["avcodec-62.dll", "LIBEGL.dll"]), "avcodec-62.dll": pe(["avutil-60.dll", "bcrypt.dll"]),
       "avutil-60.dll": pe(["kernel32.dll"]), "libEGL.dll": pe(["kernel32.dll"])}, set())
check({"java/venture-java.exe": pe(["jli.dll", "vcruntime140.dll"]), "java/jdk/bin/jli.dll": pe(["kernel32.dll"]),
       "java/jdk/bin/vcruntime140.dll": pe(["kernel32.dll"]),
       "java/jdk/bin/server/jvm.dll": pe(["vcruntime140.dll", "msvcp140.dll"])},
      {("java/jdk/bin/server/jvm.dll", "msvcp140.dll")})
# A DLL in an unrelated subdirectory does not satisfy another module.
check({"venture.exe": pe(["SDL3.dll"]), "licenses/SDL3.dll": pe(["kernel32.dll"])}, {("venture.exe", "SDL3.dll")})

# Every allowlisted name is a plain lowercase DLL stem; no MSVC runtime crept in.
for name in release.WINDOWS_SYSTEM_DLLS | release.GPU_DRIVER_DLLS:
    assert name == name.lower() and not name.endswith(".dll"), name
    assert not name.startswith(("vcruntime", "msvcp", "concrt", "vccorlib", "d3dcompiler", "ucrtbased", "mf")), name
print("package imports: ok")
