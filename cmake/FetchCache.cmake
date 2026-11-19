# Shared dependency sources for every build tree, set up through
# CMAKE_PROJECT_TOP_LEVEL_INCLUDES (CMakePresets.json).
#
# Stock FetchContent records "already extracted" in each build tree
# (CMakeFiles/fc-stamp) but extracts into FETCHCONTENT_BASE_DIR. With one base
# directory shared by all build trees and worktrees (.deps/fetch), every fresh
# build tree deleted and re-extracted every <name>-src while other trees were
# compiling from it, concurrent configures extracted the same archive at once,
# and all trees built dependencies into the same <name>-build directories.
#
# Here each hash-pinned archive gets an immutable source directory named by its
# hash under VENTURE_FETCH_CACHE_DIR. It is populated once per machine under a
# file lock, published by an atomic rename and never modified afterwards.
# Dependencies are built inside their own build tree (FETCHCONTENT_BASE_DIR is
# <build>/_deps). Declarations that are not plain hash-pinned archives keep
# stock FetchContent behavior, which is safe because the base is per tree.
include_guard(GLOBAL)

set(VENTURE_FETCH_CACHE_DIR "" CACHE PATH "Shared, immutable dependency source cache (empty: stock FetchContent)")
if(VENTURE_FETCH_CACHE_DIR STREQUAL "")
  return()
endif()
file(TO_CMAKE_PATH "${VENTURE_FETCH_CACHE_DIR}" _venture_fetch_root)
set_property(GLOBAL PROPERTY VENTURE_FETCH_ROOT "${_venture_fetch_root}")
unset(_venture_fetch_root)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory under which to collect all populated content" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/FetchCacheSource.cmake")

# A macro, so the dependency is added in the scope of the FetchContent_MakeAvailable
# call, exactly as stock FetchContent adds it.
macro(venture_fetch_provide method name)
  _venture_fetch_source("${name}" _venture_fetch_dir ${ARGN})
  if(NOT _venture_fetch_dir STREQUAL "")
    string(TOUPPER "${name}" _venture_fetch_upper)
    # Keep the cached user override empty: only this call sees the shared path,
    # so a later dependency bump cannot reuse a stale directory.
    if(NOT DEFINED CACHE{FETCHCONTENT_SOURCE_DIR_${_venture_fetch_upper}})
      set(FETCHCONTENT_SOURCE_DIR_${_venture_fetch_upper} "" CACHE PATH
        "When not empty, overrides where to find pre-populated content for ${name}")
    endif()
    # For tools that read a build tree's sources (tools/package_release.py).
    set(VENTURE_FETCH_SOURCE_DIR_${_venture_fetch_upper} "${_venture_fetch_dir}" CACHE INTERNAL
      "Shared source directory of ${name}")
    set(FETCHCONTENT_SOURCE_DIR_${_venture_fetch_upper} "${_venture_fetch_dir}")
    FetchContent_MakeAvailable(${name})
    # The nested call ends by unsetting the provider name the outer call reads
    # for its remaining names (CMake 4.4), which then skipped this provider for
    # every name after the first one in FetchContent_MakeAvailable(a b c).
    get_property(__cmake_providerCommand GLOBAL PROPERTY __FETCHCONTENT_MAKEAVAILABLE_SERIAL_PROVIDER)
    unset(FETCHCONTENT_SOURCE_DIR_${_venture_fetch_upper})
    unset(_venture_fetch_upper)
  endif()
  unset(_venture_fetch_dir)
endmacro()

cmake_language(SET_DEPENDENCY_PROVIDER venture_fetch_provide
  SUPPORTED_METHODS FETCHCONTENT_MAKEAVAILABLE_SERIAL)
