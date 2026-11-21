# Android Studio's layoutlib (Apache-2.0, from AOSP): the Android framework's
# View, widget, text and graphics classes with their host-built native runtime
# (Skia, HWUI, Minikin, libandroidfw). Venture runs Android 2D windows on it on
# desktop hosts (docs/design/layoutlib.md). Never part of the Android APK: an
# Android headset has the real framework.
#
# Google publishes layoutlib on its Maven repository. Each artifact is a JAR
# (a zip archive), hash-pinned and extracted once per machine into the shared
# source cache like every other pinned archive (cmake/FetchCacheSource.cmake).
# 17.0.4 (2026-10-29) is the newest stable release older than two weeks on
# 2026-11-20. The runtime JARs' SHA-256 values match Google's Gradle module
# metadata (layoutlib-runtime-17.0.4.module).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/FetchCacheSource.cmake")

set(VENTURE_LAYOUTLIB_VERSION 17.0.4)
set(_venture_layoutlib_maven "https://dl.google.com/android/maven2/com/android/tools/layoutlib")
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(_venture_layoutlib_os win)
  set(_venture_layoutlib_runtime_hash "SHA256=cebd81e1e03877dc9194320beee43b99b5a44d26ba66b14d532afb2eb3951347")
  set(VENTURE_LAYOUTLIB_NATIVES layoutlib_jni.dll libandroid_runtime.dll)
else()
  set(_venture_layoutlib_os linux)
  set(_venture_layoutlib_runtime_hash "SHA256=0baec3380b7f1f29bcebeb4e5ba17bcde0259cf75e923913bf3ac492edf328c7")
  set(VENTURE_LAYOUTLIB_NATIVES layoutlib_jni.so libandroid_runtime.so)
endif()

get_property(_venture_root GLOBAL PROPERTY VENTURE_FETCH_ROOT)
if(NOT _venture_root)
  file(TO_CMAKE_PATH "${VENTURE_FETCH_CACHE_DIR}" _venture_root)
  set_property(GLOBAL PROPERTY VENTURE_FETCH_ROOT "${_venture_root}")
endif()
_venture_fetch_source(layoutlib VENTURE_LAYOUTLIB_CLASSES
  URL "${_venture_layoutlib_maven}/layoutlib/${VENTURE_LAYOUTLIB_VERSION}/layoutlib-${VENTURE_LAYOUTLIB_VERSION}.jar"
  URL_HASH SHA256=ea661abe6ce643f5b6c7cb467675821cfc5deefad6f2885e6893909d7434d5aa
  DOWNLOAD_NAME layoutlib-${VENTURE_LAYOUTLIB_VERSION}.zip)
_venture_fetch_source(layoutlib-runtime-${_venture_layoutlib_os} VENTURE_LAYOUTLIB_RUNTIME
  URL "${_venture_layoutlib_maven}/layoutlib-runtime/${VENTURE_LAYOUTLIB_VERSION}/layoutlib-runtime-${VENTURE_LAYOUTLIB_VERSION}-${_venture_layoutlib_os}.jar"
  URL_HASH ${_venture_layoutlib_runtime_hash}
  DOWNLOAD_NAME layoutlib-runtime-${VENTURE_LAYOUTLIB_VERSION}-${_venture_layoutlib_os}.zip)
if(NOT EXISTS "${VENTURE_LAYOUTLIB_CLASSES}/android/view/View.class"
   OR NOT EXISTS "${VENTURE_LAYOUTLIB_RUNTIME}/data/${_venture_layoutlib_os}/lib64")
  message(FATAL_ERROR "Could not provide layoutlib ${VENTURE_LAYOUTLIB_VERSION}")
endif()
set(VENTURE_LAYOUTLIB_NATIVE_DIR "${VENTURE_LAYOUTLIB_RUNTIME}/data/${_venture_layoutlib_os}/lib64")
