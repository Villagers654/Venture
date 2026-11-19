# Android arm64 (the one Venture APK) with the pinned NDK r30, the same NDK
# tools/bootstrap.py pins. The NDK is populated once per machine into the
# shared source cache (FetchCacheSource.cmake) from the archive bootstrap.py
# keeps in Venture's dev-cache, or downloaded with the same hash.
cmake_minimum_required(VERSION 3.28)

set(VENTURE_ANDROID_API 29 CACHE STRING "Lowest Android API level the APK runs on")
# arm64-v8a: the APK. armeabi-v7a: the 32-bit game libraries it carries for
# ARM32 games on CPUs that run AArch32 (src/android_app, a sub-build).
# A dependency's checks that do not forward CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
# get it from the environment, as the NDK path below.
if(NOT VENTURE_ANDROID_ABI AND DEFINED ENV{VENTURE_ANDROID_ABI})
  set(VENTURE_ANDROID_ABI "$ENV{VENTURE_ANDROID_ABI}")
endif()
set(VENTURE_ANDROID_ABI arm64-v8a CACHE STRING "Android ABI of this tree")

# try_compile projects load this file again. They get the NDK path through
# CMAKE_TRY_COMPILE_PLATFORM_VARIABLES, and through the environment where a
# dependency's checks do not forward it.
if(NOT VENTURE_ANDROID_NDK AND DEFINED ENV{VENTURE_ANDROID_NDK})
  set(VENTURE_ANDROID_NDK "$ENV{VENTURE_ANDROID_NDK}")
endif()
if(NOT VENTURE_ANDROID_NDK)
  if(NOT VENTURE_FETCH_CACHE_DIR)
    message(FATAL_ERROR "android-arm64 needs VENTURE_FETCH_CACHE_DIR (the shared source cache)")
  endif()
  if(CMAKE_HOST_WIN32)
    set(_venture_ndk_name "android-ndk-r30-windows.zip")
    set(_venture_ndk_hash "SHA1=9bf167a1985fa7d4a036186b78f702eab9179408")
    set(_venture_data "$ENV{LOCALAPPDATA}/Venture")
  else()
    set(_venture_ndk_name "android-ndk-r30-linux.zip")
    set(_venture_ndk_hash "SHA1=5107f898313790e449e87eee2183d9a20602dee9")
    if(DEFINED ENV{XDG_DATA_HOME} AND NOT "$ENV{XDG_DATA_HOME}" STREQUAL "")
      set(_venture_data "$ENV{XDG_DATA_HOME}/venture")
    else()
      set(_venture_data "$ENV{HOME}/.local/share/venture")
    endif()
  endif()
  if(DEFINED ENV{VENTURE_DATA_DIR} AND NOT "$ENV{VENTURE_DATA_DIR}" STREQUAL "")
    set(_venture_data "$ENV{VENTURE_DATA_DIR}")
  endif()
  file(TO_CMAKE_PATH "${VENTURE_FETCH_CACHE_DIR}" _venture_root)
  set_property(GLOBAL PROPERTY VENTURE_FETCH_ROOT "${_venture_root}")
  include("${CMAKE_CURRENT_LIST_DIR}/../FetchCacheSource.cmake")
  set(_venture_urls "https://dl.google.com/android/repository/${_venture_ndk_name}")
  file(TO_CMAKE_PATH "${_venture_data}/dev-cache/${_venture_ndk_name}" _venture_local)
  if(EXISTS "${_venture_local}")
    list(PREPEND _venture_urls "${_venture_local}")
  endif()
  _venture_fetch_source(android-ndk _venture_ndk URL ${_venture_urls} URL_HASH ${_venture_ndk_hash})
  if(NOT _venture_ndk)
    message(FATAL_ERROR "Could not provide the Android NDK")
  endif()
  set(VENTURE_ANDROID_NDK "${_venture_ndk}" CACHE PATH "The pinned Android NDK (shared source cache)" FORCE)
endif()

set(ENV{VENTURE_ANDROID_NDK} "${VENTURE_ANDROID_NDK}")
set(ENV{VENTURE_ANDROID_ABI} "${VENTURE_ANDROID_ABI}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VENTURE_ANDROID_NDK VENTURE_ANDROID_API VENTURE_ANDROID_ABI)
set(ANDROID_ABI ${VENTURE_ANDROID_ABI})
set(ANDROID_PLATFORM "android-${VENTURE_ANDROID_API}")
set(ANDROID_STL c++_static)
# 16 KiB-page devices need 16 KiB-aligned LOAD segments.
set(ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON)
include("${VENTURE_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
