# The Android SDK pieces the APK build needs, without Android Studio or the
# SDK manager: Google's build-tools (aapt2, d8, zipalign, apksigner) and one
# platform's android.jar. Each is a hash-pinned archive from Google's SDK
# repository (repository2-3.xml publishes the SHA-1), populated once per
# machine into the shared source cache like the NDK. User-level only.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/FetchCacheSource.cmake")

# Build tools 37.0.0 and the API 36 platform (compileSdk). Verified against
# https://dl.google.com/android/repository/repository2-3.xml on 2026-11-18.
set(VENTURE_ANDROID_COMPILE_SDK 36 CACHE STRING "The Android platform the APK compiles against")
set(VENTURE_ANDROID_TARGET_SDK 34 CACHE STRING "The Android API level the APK targets")
if(CMAKE_HOST_WIN32)
  set(_venture_bt_name "build-tools_r37_windows.zip")
  set(_venture_bt_hash "SHA1=e9c97e26b5b5678002e9d3fed632c841ae62d99f")
else()
  set(_venture_bt_name "build-tools_r37_linux.zip")
  set(_venture_bt_hash "SHA1=70954e99f4c3d9d46ee70fa32624672fe7cd6ebe")
endif()
set(_venture_platform_name "platform-36_r02.zip")
set(_venture_platform_hash "SHA1=2c1a80dd4d9f7d0e6dd336ec603d9b5c55a6f576")

get_property(_venture_root GLOBAL PROPERTY VENTURE_FETCH_ROOT)
if(NOT _venture_root)
  file(TO_CMAKE_PATH "${VENTURE_FETCH_CACHE_DIR}" _venture_root)
  set_property(GLOBAL PROPERTY VENTURE_FETCH_ROOT "${_venture_root}")
endif()
_venture_fetch_source(android-build-tools VENTURE_ANDROID_BUILD_TOOLS
  URL "https://dl.google.com/android/repository/${_venture_bt_name}" URL_HASH ${_venture_bt_hash})
_venture_fetch_source(android-platform _venture_platform
  URL "https://dl.google.com/android/repository/${_venture_platform_name}" URL_HASH ${_venture_platform_hash})
if(NOT VENTURE_ANDROID_BUILD_TOOLS OR NOT EXISTS "${_venture_platform}/android.jar")
  message(FATAL_ERROR "Could not provide the Android build tools and platform")
endif()
set(VENTURE_ANDROID_JAR "${_venture_platform}/android.jar")

# A JDK for javac, d8, apksigner and keytool: the pinned one bootstrap.py
# --jdk provides, else JAVA_HOME.
set(VENTURE_ANDROID_JAVA_HOME "" CACHE PATH "JDK used to build the APK (default: .deps/jdk, else JAVA_HOME)")
if(NOT VENTURE_ANDROID_JAVA_HOME)
  if(EXISTS "${PROJECT_SOURCE_DIR}/.deps/jdk/bin")
    set(VENTURE_ANDROID_JAVA_HOME "${PROJECT_SOURCE_DIR}/.deps/jdk")
  elseif(DEFINED ENV{JAVA_HOME})
    file(TO_CMAKE_PATH "$ENV{JAVA_HOME}" VENTURE_ANDROID_JAVA_HOME)
  else()
    message(FATAL_ERROR "The APK build needs a JDK: run tools/bootstrap.py --jdk or set JAVA_HOME")
  endif()
endif()
