#!/usr/bin/env python3
"""Build a relocatable shipping layout from a completed build (no source at launch)."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import struct
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
# Linked components, not build tools or tests. Nested licenses cover bundled code.
NOTICES = {
    "nlohmann_json": ["LICENSE.MIT"], "curl": ["COPYING"],
    "sdl3": ["LICENSE.txt"], "imgui": ["LICENSE.txt"],
    "freetype": ["docs/FTL.TXT", "LICENSE.TXT"], "webp": ["COPYING", "PATENTS"],
    "stb": ["LICENSE"], "nanosvg": ["LICENSE.txt"], "openxr_sdk": ["LICENSE"],
    "zlib": ["LICENSE"], "boost_headers": ["LICENSE_1_0.txt"],
    "fmt": ["LICENSE"], "mcl": ["LICENSE"], "xbyak": ["COPYRIGHT"], "robin_map": ["LICENSE"],
    "miniz": ["LICENSE"], "mimalloc": ["LICENSE"],
    "venture_astcenc": ["LICENSE.txt"], "utf8proc": ["LICENSE.md"],
    "venture_vulkan_headers": ["LICENSES/Apache-2.0.txt", "LICENSES/MIT.txt"], "venture_xr_headers": ["LICENSE"],
}
# Sources kept in the repository (xbyak carries Venture's dual-mapping change).
IN_TREE = {"xbyak": ROOT / "src/jit/xbyak"}
# ANGLE Vulkan backend: linked dependencies/headers, excluding its test suites,
# source generators, desktop GL and window-system backends, OpenCL and toolchain
# dependencies.
ANGLE_NOTICES = ("third_party/abseil-cpp/", "third_party/astc-encoder/src/LICENSE.txt",
                 "third_party/spirv-tools/src/LICENSE", "third_party/spirv-headers/src/LICENSE",
                 "third_party/vulkan-headers/src/LICENSE.md", "third_party/vulkan_memory_allocator/LICENSE.txt", "third_party/zlib/",
                 "third_party/perfetto/LICENSE", "src/third_party/volk/")

# Intentional product content: its GitHub owner may also be the build account.
PUBLIC_PROJECT_URL = "https://github.com/McFredward/steam-link-native-passthrough"


def copy(source, target):
    if not source.is_file():
        raise RuntimeError("Missing shipping file: " + str(source.relative_to(ROOT) if source.is_relative_to(ROOT) else source.name))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def tree(source, target):
    for path in sorted(source.rglob("*")):
        if path.is_file():
            copy(path, target / path.relative_to(source))


def scan(layout, forbidden, users=(), hosts=()):
    # Check ASCII and UTF-16 strings in native files, and inflated JAR entries.
    # Paths match as substrings. Host names match as whole tokens; account names
    # only as whole tokens in path/login context (dir/user, DOMAIN\user, user@host),
    # since short account names are often ordinary words in third-party data
    # (public-suffix TLDs, translated Android resources).
    needles = []
    for value in forbidden:
        for variant in {value, value.replace("\\", "/"), value.replace("/", "\\")}:
            if variant:
                needles.extend([variant.lower().encode(), variant.lower().encode("utf-16le")])
    drive = rb"(?<![a-zA-Z0-9])[a-z]:[/\\](?![/\\])"
    patterns = [drive + rb"(?:users|build|agent|workspace)[/\\]",
                rb"(?<![:/\\a-zA-Z0-9])/(?:home|Users)/[^/\x00\s]+/",
                drive + rb"[^\x00\r\n]{0,512}[/\\](?:build|src|source|_work|out|\.deps)[/\\]"]
    token = lambda value, inner: rb"(?<![a-z0-9])" + inner(re.escape(value.lower().encode())) + rb"(?![a-z0-9])"
    identities = [token(value, lambda v: v) for value in set(hosts) if value]
    identities += [token(value, lambda v: rb"(?:(?<=[/\\@])" + v + rb"|" + v + rb"(?=[/\\@]))") for value in set(users) if value]
    public_origin, public_owner, _ = PUBLIC_PROJECT_URL.rsplit("/", 2)
    public_account = re.compile(rb"(?<=" + re.escape((public_origin + "/").encode()) + rb")" +
                                re.escape(public_owner.encode()) + rb"(?=/)", re.I)
    def check(data, name):
        lower = data.lower()
        # Decode both alignments; deleting NUL bytes would join unrelated C
        # strings and misclassify URI schemes as absolute Windows paths.
        views = [data] + [data[offset:].decode("utf-16le", errors="replace").encode("utf-8") for offset in (0, 1)]
        # The referenced GitHub namespace is public content. GCC may emit the
        # URL's string constructor as overlapping constant chunks, so the full
        # URL need not remain contiguous in the image. Other origins, logins
        # and private paths still inspect the original bytes/identity contexts.
        identity_views = [public_account.sub(b"", view) for view in views]
        if (any(needle in lower for needle in needles) or
                any(re.search(p, view, re.I) for p in patterns for view in views) or
                any(re.search(p, view, re.I) for p in identities for view in identity_views)):
            raise RuntimeError("Non-redistributable path or identity in " + name)
    for path in sorted(layout.rglob("*")):
        if not path.is_file():
            continue
        name = path.relative_to(layout).as_posix()
        check(name.encode("utf-8"), name)
        check(path.read_bytes(), name)
        if path.suffix in (".jar", ".zip"):
            with zipfile.ZipFile(path) as archive:
                for entry in archive.infolist():
                    check(entry.filename.encode("utf-8"), name + ":" + entry.filename)
                    check(archive.read(entry), name + ":" + entry.filename)


def remap_pe_source_names(path):
    """Remap prebuilt __FILE__ diagnostics without moving any bytes or suffixes.

    Our compiler prefix map cannot affect the prebuilt libc++abi archive. Its
    diagnostic literals live in .rdata. Preserve length, terminators, basename
    and every suffix offset (the linker may pool string suffixes).
    """
    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        return
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    headers = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    # LLVM strip removes DWARF/symbol tables, but preserves MSVC CodeView
    # records embedded in .rdata. Remove only those debug records; keep POGO,
    # reproducibility and extended DLL characteristics (including CET flags).
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    if optional_size:
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (112 if magic == 0x20b else 96)
        debug_rva, debug_size = struct.unpack_from("<II", data, directories + 6 * 8)
        for index in range(count):
            section = headers + index * 40
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, section + 8)
            if debug_rva and virtual_address <= debug_rva < virtual_address + raw_size:
                debug_offset = raw_offset + debug_rva - virtual_address
                for entry in range(debug_offset, debug_offset + debug_size, 28):
                    kind, size, rva, offset = struct.unpack_from("<IIII", data, entry + 12)
                    if kind == 2:
                        data[offset:offset + size] = b"\0" * size
                        data[entry:entry + 28] = b"\0" * 28
                break
    source = re.compile(rb"(?<![a-zA-Z0-9])(?:[a-zA-Z]:[/\\](?![/\\])|/(?:home|Users)/)[ -~]+?\.(?:cpp|cc|c|h|hpp)(?=\x00)")
    for index in range(count):
        header = headers + index * 40
        if data[header:header + 8].rstrip(b"\0") != b".rdata":
            continue
        size, offset = struct.unpack_from("<II", data, header + 16)
        for match in source.finditer(data[offset:offset + size]):
            value = match.group()
            prefix = max(value.rfind(b"/"), value.rfind(b"\\")) + 1
            # Relative diagnostic directory, with the original suffix intact.
            replacement = b"source/" + b"_" * (prefix - 8) + b"/"
            if prefix < 8:
                raise RuntimeError("Unexpected short source path")
            start = offset + match.start()
            data[start:start + prefix] = replacement
    path.write_bytes(data)


# DLLs that every supported Windows 10/11 x64 installation provides (Home,
# Pro, Enterprise, Education, LTSC and the N editions). Deliberately absent:
# MSVC runtimes (vcruntime*, msvcp*, concrt*, vccorlib*), debug CRTs, D3DX and
# d3dcompiler_*, Media Foundation / Windows Media (missing on N editions) and
# the WebView2 runtime. Those must ship in the release layout or be loaded
# dynamically with a fallback.
WINDOWS_SYSTEM_DLLS = frozenset("""
advapi32 authz avrt bcrypt bcryptprimitives cabinet cfgmgr32 combase comctl32
comdlg32 credui crypt32 cryptbase cryptnet d2d1 d3d11 d3d12 d3d9 dbghelp dcomp
dhcpcsvc dinput8 dnsapi dsound dwmapi dwrite dxgi dxva2 gdi32 gdiplus glu32 hid
imagehlp imm32 iphlpapi kernel32 kernelbase ksuser mpr msimg32 msvcrt mswsock
ncrypt netapi32 normaliz ntdll ole32 oleacc oleaut32 opengl32 powrprof propsys
psapi rpcrt4 secur32 setupapi shcore shell32 shlwapi sspicli ucrtbase urlmon
user32 userenv usp10 uxtheme version winhttp wininet winmm winspool.drv
wintrust wldap32 ws2_32 wsock32 wtsapi32 xinput1_4 xinput9_1_0
""".split())
# Installed by every Vulkan-capable GPU driver; Venture requires one.
GPU_DRIVER_DLLS = frozenset({"vulkan-1"})
# Directories, relative to the layout, whose DLLs a module may resolve against
# besides its own directory (the JRE loads its DLLs from its bin directories).
LAYOUT_DLL_DIRS = ("", "java/jdk/bin", "java/jdk/bin/server")


def is_os_provided(name):
    lower = name.lower()
    if lower.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    stem = lower[:-4] if lower.endswith(".dll") else lower
    return stem in WINDOWS_SYSTEM_DLLS or stem in GPU_DRIVER_DLLS


def pe_imports(path, readobj):
    """Return the regular and delay-loaded DLL imports of one PE file."""
    result = subprocess.run([str(readobj), "--coff-imports", str(path)], check=True, capture_output=True, text=True)
    return re.findall(r"^  Name: (.+)$", result.stdout, re.M)


def import_report(layout, readobj):
    """Map each PE in the layout to (imports, imports neither OS-provided nor shipped)."""
    layout = Path(layout)
    # Windows resolves DLL names case-insensitively; this also runs on Linux.
    shipped = {}
    for path in layout.rglob("*"):
        if path.is_file():
            shipped.setdefault(path.parent, set()).add(path.name.lower())
    report = {}
    for path in sorted(layout.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".exe", ".dll"):
            continue
        directories = [path.parent] + [layout / d for d in LAYOUT_DLL_DIRS]
        imports = pe_imports(path, readobj)
        missing = [name for name in imports if not is_os_provided(name)
                   and not any(name.lower() in shipped.get(d, ()) for d in directories)]
        report[path.relative_to(layout).as_posix()] = (imports, missing)
    return report


def check_imports(layout, readobj):
    """Reject PE imports that are neither shipped nor guaranteed by Windows.

    Independent of the build machine: a DLL present in the packager's System32
    (an SDK, Visual Studio or driver install) is not evidence that end users
    have it.
    """
    problems = [name + " in " + module for module, (_, missing) in import_report(layout, readobj).items() for name in missing]
    if problems:
        raise RuntimeError("Unresolved import " + "; ".join(problems))


def configured_sources(build):
    """Honor the same source overrides that supplied the linked dependency."""
    cache = build / "CMakeCache.txt"
    result = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator and value and key.startswith("FETCHCONTENT_SOURCE_DIR_"):
                name = key.partition(":")[0].removeprefix("FETCHCONTENT_SOURCE_DIR_").lower()
                result[name] = Path(value)
            elif separator and value and key.partition(":")[0] == "FETCHCONTENT_BASE_DIR":
                result[None] = Path(value)
    return result


def headless_jre(build, jdk):
    """The runtime's JVM is always headless (-Djava.awt.headless=true). Drop the
    X11 and ALSA bridges (they link libX11/libasound, which a Wayland desktop
    need not have) and give it bundled fonts with a static font configuration,
    so text renders without fontconfig or installed fonts."""
    for name in ("libawt_xawt.so", "libjawt.so", "libsplashscreen.so", "libjsound.so"):
        (jdk / "lib" / name).unlink(missing_ok=True)
    fetched = configured_sources(build)
    fonts = fetched.get("dejavu_fonts", fetched.get(None, ROOT / ".deps/fetch") / "dejavu_fonts-src")
    for name in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf"):
        copy(fonts / "ttf" / name, jdk / "lib/fonts" / name)
    copy(ROOT / "tools/dex/fontconfig.properties", jdk / "lib/fontconfig.properties")
    copy(fonts / "LICENSE", jdk.parent.parent / "licenses/dejavu-fonts/LICENSE")


def static_runtime_notices(build, output):
    """Notices for system libraries linked statically by the Linux baseline
    toolchain (cmake/toolchains/linux-baseline.cmake): OpenSSL and libc++."""
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8") if (build / "CMakeCache.txt").is_file() else ""
    crypto = re.search(r"^OPENSSL_CRYPTO_LIBRARY:FILEPATH=(.*)$", cache, re.M)
    if crypto and crypto.group(1).endswith(".a"):
        candidates = [Path("/usr/share/doc") / name / "copyright" for name in ("libssl-dev", "libssl3", "openssl")]
        candidates.append(Path(crypto.group(1)).parent.parent / "share/doc/openssl/LICENSE.txt")
        source = next((p for p in candidates if p.is_file()), None)
        if source is None:
            raise RuntimeError("OpenSSL is linked statically but its license was not found")
        copy(source, output / "licenses/openssl" / ("LICENSE.txt" if source.suffix == ".txt" else "copyright"))
    if "-static-libstdc++" in cache and "-stdlib=libc++" in cache:
        llvm = sorted(Path("/usr/share/doc").glob("libc++*-dev/copyright"))
        if not llvm:
            raise RuntimeError("libc++ is linked statically but its license was not found")
        copy(llvm[-1], output / "licenses/llvm-runtime/copyright")


def package(build, output, angle, strip, readobj):
    if output.exists():
        for current, dirs, files in os.walk(output, followlinks=False):
            for name in dirs + files:
                path = Path(current) / name
                if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
                    raise RuntimeError("Output must not contain links")
        # Never recursively delete or traverse a junction. Only our manifest may
        # authorize replacement of individual files in an existing layout.
        manifest = output / "release-files.json"
        if not manifest.is_file():
            raise RuntimeError("Output exists without a release manifest; choose a fresh directory")
        previous = json.loads(manifest.read_text())
        actual = {p.relative_to(output).as_posix() for p in output.rglob("*") if p.is_file()}
        if actual != set(previous["files"]) | {manifest.name}:
            raise RuntimeError("Output contains untracked files; choose a fresh directory")
        for name in previous["files"]:
            path = output / name
            if path.is_symlink() or not path.resolve().is_relative_to(output.resolve()):
                raise RuntimeError("Output must not contain links")
            path.unlink()
        manifest.unlink()
        for directory in sorted((p for p in output.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
            directory.rmdir()
    output.mkdir(parents=True, exist_ok=True)
    bin_dir = build / "bin"
    windows = os.name == "nt"
    suffix = ".exe" if windows else ""
    names = ["venture" + suffix, "venture-runtime" + suffix, "venture-framework.jar",
             "venture-java-bridge.jar"]
    names += ["WebView2Loader.dll", "libEGL.dll", "libGLESv2.dll"] if windows else ["libEGL.so", "libGLESv2.so"]
    names += ["avutil-60.dll", "avcodec-62.dll", "avformat-62.dll", "swscale-9.dll"] if windows else [
        "libavutil.so.60", "libavcodec.so.62", "libavformat.so.62", "libswscale.so.9"]
    if not windows:
        names.append("libsqlite3.so.0")
    for name in names:
        source = bin_dir / name
        if not windows and name in ("libEGL.so", "libGLESv2.so") and not source.is_file():
            source = angle / "bin" / name
        copy(source, output / name)
    if windows:
        copy(ROOT / "data/venture-passthrough.cmd", output / "venture-passthrough.cmd")
    for name in ("LGPL-2.1.txt", "ffmpeg-8.1.3.tar.xz", "ffmpeg-8.1.3.tar.xz.asc"):
        copy(bin_dir / "LICENSES" / name, output / "licenses/ffmpeg" / name)
    copy(bin_dir / "venture-ffmpeg.json", output / "licenses/ffmpeg/venture-ffmpeg.json")
    if windows:
        copy(bin_dir / "java/venture-java.exe", output / "java/venture-java.exe")
    agent = "java/agent/venture-jvmti-agent" + (".dll" if windows else ".so")
    copy(bin_dir / agent, output / agent)
    for name in ("dex/classes", "asm"):
        tree(bin_dir / "java" / name, output / "java" / name)
    # jdeps follows all compiled driver classes and recursively resolves the
    # reachable library classes. This excludes dex2jar's unused CLI/tool jars.
    jdeps = ROOT / ".deps/jdk/bin" / ("jdeps.exe" if windows else "jdeps")
    cp = os.pathsep.join([str(bin_dir / "java/asm/asm.jar"), str(bin_dir / "java/dex2jar/lib/*")])
    dependencies = subprocess.run([str(jdeps), "--multi-release", "21", "--recursive",
        "--class-path", cp, str(bin_dir / "java/dex/classes")],
        check=True, capture_output=True, text=True).stdout
    if " -> not found" in dependencies:
        raise RuntimeError("Converter has unresolved Java dependencies")
    jars = {Path(line.split(" -> ", 1)[1].strip()) for line in dependencies.splitlines()
            if " -> " in line and not line.startswith(" ") and line.rstrip().endswith(".jar")}
    if not jars:
        raise RuntimeError("jdeps found no converter dependencies")
    for jar in sorted(jars):
        if jar.parent == bin_dir / "java/dex2jar/lib":
            copy(jar, output / "java/dex2jar/lib" / jar.name)
    for name in ("LICENSE.txt", "NOTICE.txt"):
        copy(bin_dir / "java/dex2jar" / name, output / "java/dex2jar" / name)
    copy(bin_dir / "java/android-32.hierarchy.tsv", output / "java/android-32.hierarchy.tsv")
    subprocess.run([sys.executable, str(ROOT / "tools/dex/make_jre.py"), str(output / "java/jdk")], check=True)
    # jlink links each module's legal notices to java.base's copies on POSIX.
    # The layout must stay link-free (repackaging and runtime bundles reject
    # links), so materialize them as regular files.
    for path in sorted((output / "java/jdk").rglob("*")):
        if path.is_symlink():
            target = path.resolve()
            if not path.is_file() or not target.is_relative_to((output / "java/jdk").resolve()):
                raise RuntimeError(f"JDK link escapes the image: {path}")
            path.unlink()
            shutil.copy2(target, path)
    if not windows:
        headless_jre(build, output / "java/jdk")
    # jlink retains the JNI import archive, although embedding/launching the
    # JVM loads its DLL. Import archives and symbols are development files.
    for path in (output / "java/jdk").rglob("*"):
        if path.is_file() and path.suffix.lower() in (".lib", ".a", ".pdb"):
            path.unlink()
    # Native binaries are copied, then stripped; the build retains its symbols.
    for path in sorted(output.rglob("*")):
        if path.is_file() and (path.suffix in (".dll", ".exe") or (not windows and (".so" in path.name or path.name in ("venture", "venture-runtime", "java")))):
            subprocess.run([str(strip), "--strip-all", str(path)], check=True)
            if windows:
                remap_pe_source_names(path)
    copy(ROOT / "LICENSE", output / "LICENSE")
    copy(ROOT / "LICENSES/MIT.txt", output / "licenses/venture/MIT.txt")
    copy(ROOT / "LICENSES/0BSD.txt", output / "licenses/venture/0BSD.txt")
    copy(ROOT / "docs/THIRD_PARTY_NOTICES.md", output / "licenses/THIRD_PARTY_NOTICES.md")
    tree(ROOT / "tools/release-notices", output / "licenses")
    # The framework JAR already carries AOSP's NOTICE and Apache license.
    for relative, name in (("src/runtime/java/framework/org/kxml2/io/KXmlParser.java", "kxml2.txt"),
                           ("src/runtime/hle/libc/strings_errors.cpp", "bionic.txt"),
                           ("src/runtime/hle/libc/ctype_unicode.h", "unicode.txt")):
        source = (ROOT / relative).read_text()
        (output / "licenses" / name).write_text(source[:source.index("*/") + 2] + "\n")
    if windows:
        copy(ROOT / ".deps/llvm-mingw/LICENSE.TXT", output / "licenses/llvm-runtime/LICENSE.TXT")
    else:
        static_runtime_notices(build, output)
    dependency_sources = configured_sources(build)
    for component, files in NOTICES.items():
        fetched = dependency_sources.get(None, ROOT / ".deps/fetch") / (component + "-src")
        source_root = IN_TREE.get(component) or dependency_sources.get(component, fetched)
        for name in files:
            copy(source_root / name, output / "licenses" / component / name)
    for relative in ("src/bdf/bdflib.c", "src/pcf/pcfread.c", "src/base/fthash.c", "src/autofit/ft-hb-ft.c"):
        source = (dependency_sources.get("freetype", dependency_sources.get(None, ROOT / ".deps/fetch") / "freetype-src") / relative).read_text()
        notice = next(block for block in re.findall(r"/\*.*?\*/", source, re.S)
                      if "Copyright" in block and "Permission" in block)
        (output / "licenses/freetype" / (Path(relative).stem + ".txt")).write_text(notice + "\n")
    copy(ROOT / "src/runtime/hle/vulkan/shaders/LICENSE.gfxstream", output / "licenses/gfxstream/LICENSE")
    copy(angle / "LICENSE", output / "licenses/angle/LICENSE")
    index = json.loads((angle / "licenses/INDEX.json").read_text())
    for source, name in index.items():
        if source.startswith(ANGLE_NOTICES):
            copy(angle / "licenses" / name, output / "licenses/angle" / source)
    if windows:
        copy(dependency_sources.get(None, ROOT / ".deps/fetch") / "webview2-src/LICENSE.txt", output / "licenses/webview2/LICENSE.txt")
        copy(dependency_sources.get(None, ROOT / ".deps/fetch") / "webview2-src/NOTICE.txt", output / "licenses/webview2/NOTICE.txt")
        check_imports(output, readobj)
    forbidden = [str(ROOT), str(build), str(Path.home())]
    users, hosts = [os.environ.get("USERNAME", "")], [os.environ.get("COMPUTERNAME", "")]
    if os.name != "nt":
        users += [os.environ.get("USER", ""), os.environ.get("LOGNAME", "")]
        hosts += [os.uname().nodename] if os.uname().nodename != "localhost" else []
    scan(output, forbidden, users, hosts)
    files = {p.relative_to(output).as_posix(): {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in sorted(output.rglob("*")) if p.is_file()}
    (output / "release-files.json").write_text(json.dumps({"files": files}, indent=2) + "\n")
    print(f"Release layout: {len(files) + 1} files, {sum(v['bytes'] for v in files.values()) / 1048576:.1f} MiB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--angle", type=Path, required=True)
    parser.add_argument("--strip", type=Path, required=True)
    parser.add_argument("--readobj", type=Path)
    args = parser.parse_args()
    output = args.output or ROOT / "dist" / ("Venture-win64" if os.name == "nt" else "Venture-linux64")
    package(args.build.resolve(), output.resolve(), args.angle.resolve(), args.strip, args.readobj)


if __name__ == "__main__":
    main()
