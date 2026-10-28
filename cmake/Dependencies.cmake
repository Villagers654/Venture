# Pinned third-party dependencies. Sources are cached under .deps/fetch (see
# CMakePresets.json) so separate build trees share one download.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/archive/55f93686c01528224f448c19128836e7df245f72.tar.gz # v3.12.0
  URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef
  SYSTEM)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/2d0a9359a60c51affe2a9bebb1be1dca47868151.tar.gz # v2.5.3
  URL_HASH SHA256=e64542c4ea68e9f381ccf6eae924cfdd652567c87c142d76fe92644fb4608149
  SYSTEM)
set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/b48d1afbe8ee8b238e2961dc363a949dd7304e23.tar.gz # v1.92.9b-docking
  URL_HASH SHA256=747c5a465a05c4ef5b4a09e2e0e3d0ce2bc76b31420f0f5af33df470907cf893
  SYSTEM)

FetchContent_Declare(openxr_sdk
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz # release-1.1.63
  URL_HASH SHA256=1681e0f6c9633c73117c2fc376d1fe71cbd6fdd2aeb896ed5854ff022aa82eaa
  SYSTEM)

FetchContent_MakeAvailable(nlohmann_json doctest)
if(NOT WIN32)
  # Venture owns SIGSEGV/SIGBUS (JIT fastmem, guarded guest access) and chains
  # to earlier handlers. doctest's per-test-case handlers would sit in front of
  # it; Windows' doctest uses a last-chance filter that never preempts the JIT.
  target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_NO_POSIX_SIGNALS)
endif()

# Launcher networking is shared with the runtime, but no UI dependency leaks
# into venture_common. Only HTTP(S), static libcurl, native Windows certificate store.
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_WEBSOCKETS ON CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(curl
  URL https://github.com/curl/curl/archive/fdb8a789d2b446b77bd7cdd2eff95f6cbc814cf4.tar.gz # curl-8_14_1
  URL_HASH SHA256=64205466b2b9c1fd6fc44a8b765842edd67ed1a3a7df554d68efcf117eca8925
  SYSTEM)
FetchContent_MakeAvailable(curl)

# SDL3 serves the launcher everywhere and the runtime's audio outside Windows.
if(VENTURE_BUILD_LAUNCHER OR NOT WIN32)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  # Wayland only on Linux: no X11 video backend (Xlib, xcb, XCursor, XInput,
  # XRandR...) is compiled in or loaded at run time. No libdecor either: its
  # cairo/GTK plugins load libX11 and libxcb; the launcher draws its own
  # window chrome instead (launcher/ui/chrome.h).
  set(SDL_X11 OFF CACHE BOOL "" FORCE)
  set(SDL_WAYLAND_LIBDECOR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/7f3ae3d57459e59943a4ecfefc8f6277ec6bf540.tar.gz # release-3.2.28
    URL_HASH SHA256=7a8347c770b90b33daac2352858ca03f9c9a2ccc8ce711054870361d1a6b32e5
    SYSTEM)
  FetchContent_MakeAvailable(SDL3)
endif()

if(VENTURE_BUILD_LAUNCHER)
  FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    SYSTEM)
  # FreeType rasterizes the launcher's text (hinting, color emoji) instead of stb_truetype.
  foreach(feature ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${feature} ON CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(freetype
    URL https://github.com/freetype/freetype/archive/0a0221a1347e2f1e07c395263540026e9a0aa7c7.tar.gz # VER-2-14-3
    URL_HASH SHA256=11cd478953fc1d382f20a233b8e2aed6c31b47cbf0568c4bdb3334bcc1550698
    SYSTEM EXCLUDE_FROM_ALL)
  # nanosvg rasterizes the launcher's SVG logo; headers only (src/ has no CMake project).
  FetchContent_Declare(nanosvg
    URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
    URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
    SOURCE_SUBDIR src SYSTEM)
  FetchContent_MakeAvailable(imgui stb freetype nanosvg)
  # Meta's public CDN now serves WebP even when Accept requests JPEG. stb still
  # handles JPEG/PNG/GIF; use the small portable decoder for WebP covers.
  foreach(feature ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS LIBWEBPMUX)
    set(WEBP_BUILD_${feature} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(WEBP_BUILD_LIBWEBPDECODER ON CACHE BOOL "" FORCE)
  set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(webp
    URL https://github.com/webmproject/libwebp/archive/a4d7a715337ded4451fec90ff8ce79728e04126c.tar.gz # v1.5.0
    URL_HASH SHA256=e3a6189054f81e06a5fbadb0d5c5a4678018c6df6d37e17bd99a86f66c650a97
    SYSTEM EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(webp)
  if(WIN32)
    FetchContent_Declare(webview2
      URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3537.50/microsoft.web.webview2.1.0.3537.50.nupkg
      URL_HASH SHA256=5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(webview2)
  endif()
endif()

# Shared by package assets and the libz HLE. Static, pinned upstream release.
if(NOT TARGET zlibstatic)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib
    URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    SYSTEM)
  FetchContent_MakeAvailable(zlib)
  set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(VENTURE_BUILD_RUNTIME)
  # Dynarmic consumes Boost headers only. Keep their source immutable too.
  FetchContent_Declare(boost_headers
    URL https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    URL_HASH SHA256=f55c340aa49763b1925ccf02b2e83f35fdcf634c9d5164a2acb87540173c741d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(boost_headers)
  set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)
  set(BOOST_ROOT "${boost_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
  set(Boost_INCLUDE_DIR "${boost_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
  set(CMAKE_POLICY_DEFAULT_CMP0167 OLD)
  # Venture's JIT (src/jit) and the unmodified libraries it uses (xbyak, which
  # carries Venture's dual-mapping change, lives in src/jit/xbyak).
  # mcl and oaknut set CMAKE_DISABLE_SOURCE_CHANGES, so their build trees must
  # live in this build tree, not under the source-tree fetch cache.
  FetchContent_Declare(fmt
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/fmt-build"
    URL https://github.com/fmtlib/fmt/archive/7b273fbb541c7c9058800bc613f4291274b2d26e.tar.gz # 11.1.4-12
    URL_HASH SHA256=4d5465ace5ee6aed8183b0954f9f22fffa5ca7dd550fbd5763a54e58ab958bca
    SYSTEM)
  FetchContent_Declare(mcl
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/mcl-build"
    URL https://github.com/merryhime/mcl/archive/5fc4beaf331037649b10625736b41365defb4f50.tar.gz # 0.1.14
    URL_HASH SHA256=36a2200e7940838112c9967388685a5178c2e9a110233193b914f54b2e2f51fd
    SYSTEM)
  FetchContent_Declare(robin_map
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/robin_map-build"
    URL https://github.com/Tessil/robin-map/archive/054ec5ad67440fcd65e0497e5a27ef31f53fcc7f.tar.gz # v1.3.0-6
    URL_HASH SHA256=a73d3a6b98b7961244981bd89a42fcb4aef63a89d1ec135bbc90d3621b355d27
    SYSTEM)
  FetchContent_Declare(catch2
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/catch2-build"
    URL https://github.com/catchorg/Catch2/archive/76f70b1403dbc0781216f49e20e45b71f7eccdd8.tar.gz # v3.8.0-1
    URL_HASH SHA256=ff28f1ebaca35e5b1cf8494e9f5c6dd697a1d700b0df4124832a78781f5669b5
    SYSTEM)
  FetchContent_Declare(oaknut
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/oaknut-build"
    URL https://github.com/merryhime/oaknut/archive/94c726ce0338b054eb8cb5ea91de8fe6c19f4392.tar.gz # 2.0.2-5
    URL_HASH SHA256=c404db82002f9a851a412b3d55ac9be8a42a2e319fd2fc8a73e63e6ef5d4f97b
    SYSTEM)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/jit" "${CMAKE_BINARY_DIR}/jit")
  # Boost ICL insertions/queries can overlap several disjoint keys. libc++'s unique-key
  # lower/upper_bound optimization returns only one of them, leaving stale JIT
  # blocks after range invalidation. Use ICL's documented Boost.Container backend
  # (boost/icl/impl_config.hpp) consistently inside both Dynarmic frontends.
  target_compile_definitions(dynarmic PRIVATE ICL_USE_BOOST_MOVE_IMPLEMENTATION)
  # Pinned fmt predates libc++ removing the transitive malloc declaration.
  target_compile_options(fmt PUBLIC "SHELL:-include stdlib.h")
endif()

# miniz reads APKs and ZIP archives: the runtime's APK loader and the
# launcher's game import (common).
FetchContent_Declare(miniz
  BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/miniz-build"
  URL https://github.com/richgel999/miniz/archive/293d4db1b7d0ffee9756d035b9ac6f7431ef8492.tar.gz # 3.0.2
  URL_HASH SHA256=27d99216e9127d1678085639f3059f6907a02c044e8a38392a39f96163ddbbf0
  SYSTEM)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(miniz)

# miniaudio 0.11.23, MIT-0 / public domain. SOURCE_SUBDIR avoids its example
# targets; Venture builds one statically linked, device-only implementation.
FetchContent_Declare(miniaudio
  URL https://codeload.github.com/mackron/miniaudio/tar.gz/refs/tags/0.11.23
  URL_HASH SHA256=1062155cc5882b55c48cb37f57a4dc783669e83ae0838535c62b206eeb1587a6
  SOURCE_SUBDIR venture-no-upstream-targets
  SYSTEM)
if(VENTURE_BUILD_RUNTIME OR VENTURE_BUILD_TESTS)
  FetchContent_MakeAvailable(miniaudio)
endif()

# SQLite for the runtime's android.database.sqlite bridge. Windows uses the
# system winsqlite3.dll; Linux ships its own libsqlite3.so.0 beside the runtime
# (loaded before any system copy), so no distribution package is required.
if(VENTURE_BUILD_RUNTIME AND NOT WIN32)
  FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip # 3.53.4
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sqlite)
  add_library(venture_sqlite3 SHARED "${sqlite_SOURCE_DIR}/sqlite3.c")
  target_compile_definitions(venture_sqlite3 PRIVATE SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS4 SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1 SQLITE_ENABLE_RTREE
    SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_UNLOCK_NOTIFY SQLITE_SECURE_DELETE)
  target_link_libraries(venture_sqlite3 PRIVATE ${CMAKE_DL_LIBS} m)
  set_target_properties(venture_sqlite3 PROPERTIES OUTPUT_NAME sqlite3 SOVERSION 0 C_VISIBILITY_PRESET default)
endif()

# DejaVu Sans for the bundled headless JRE (java/jdk/lib/fonts with a static
# tools/dex/fontconfig.properties) and the launcher's fallback font, so text
# renders without fontconfig or installed fonts. Files only, no build.
if(NOT WIN32)
  FetchContent_Declare(dejavu_fonts
    URL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
    URL_HASH SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
    SOURCE_SUBDIR venture-no-upstream-targets
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(dejavu_fonts)
endif()
