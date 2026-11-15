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

# Sets <out> to the cached source directory of a hash-pinned archive, populating
# it first when needed; empty when the declaration is not a plain archive.
function(_venture_fetch_source name out)
  set(${out} "" PARENT_SCOPE)
  set(urls "")
  set(hash "")
  set(download_dir "")
  set(download_name "")
  set(keep_timestamps FALSE)
  set(key "")
  set(value_of "")
  foreach(arg IN LISTS ARGN)
    if(NOT value_of STREQUAL "")
      # The value of a single-value keyword, whatever it looks like (TRUE).
      set(${value_of} "${arg}")
      set(value_of "")
    elseif(arg MATCHES "^(GIT|SVN|HG|CVS)_|^(DOWNLOAD|UPDATE|PATCH|CONFIGURE|BUILD|INSTALL|TEST)_COMMAND$|^(DOWNLOAD_NO_EXTRACT|FIND_PACKAGE_ARGS|OVERRIDE_FIND_PACKAGE)$")
      return()
    elseif(arg STREQUAL "URL_HASH")
      set(value_of hash)
    elseif(arg STREQUAL "DOWNLOAD_DIR")
      set(value_of download_dir)
    elseif(arg STREQUAL "DOWNLOAD_NAME")
      set(value_of download_name)
    elseif(arg STREQUAL "DOWNLOAD_EXTRACT_TIMESTAMP")
      set(value_of keep_timestamps)
    elseif(arg MATCHES "^[A-Z][A-Z0-9_]*$")
      set(key "${arg}")
    elseif(key STREQUAL "URL")
      list(APPEND urls "${arg}")
    endif()
  endforeach()
  if(urls STREQUAL "" OR NOT hash MATCHES "^([A-Za-z0-9_]+)=([0-9A-Fa-f]+)$")
    return()
  endif()
  string(TOUPPER "${CMAKE_MATCH_1}" algorithm)
  string(TOLOWER "${CMAKE_MATCH_2}" digest)
  string(TOLOWER "${name}" lower)
  string(SUBSTRING "${digest}" 0 16 short)
  get_property(root GLOBAL PROPERTY VENTURE_FETCH_ROOT)
  set(final "${root}/${lower}-${short}")
  # Written after the rename, under the lock: a directory without it is not
  # complete (an interrupted population or a partial deletion).
  set(complete "${root}/${lower}-${short}.ok")
  if(EXISTS "${complete}" AND IS_DIRECTORY "${final}")
    set(${out} "${final}" PARENT_SCOPE)
    return()
  endif()

  file(MAKE_DIRECTORY "${root}")
  file(LOCK "${root}/${lower}-${short}.lock" GUARD FUNCTION TIMEOUT 7200 RESULT_VARIABLE locked)
  if(NOT locked EQUAL 0)
    message(FATAL_ERROR "Could not lock the shared source of ${name}: ${locked}")
  endif()
  if(EXISTS "${complete}" AND IS_DIRECTORY "${final}")
    set(${out} "${final}" PARENT_SCOPE)
    return()
  endif()
  # Holding this archive's lock: anything left for it is from an interrupted run.
  file(GLOB stale LIST_DIRECTORIES true "${root}/.ex-${lower}-${short}-*")
  if(EXISTS "${final}")
    list(APPEND stale "${final}")
  endif()
  foreach(directory IN LISTS stale)
    file(REMOVE_RECURSE "${directory}")
  endforeach()

  # The archive: a local file, or a download into the shared download directory
  # FetchContent used before (reusing an archive whose hash matches).
  list(GET urls 0 first)
  if(NOT first MATCHES "^[A-Za-z][A-Za-z0-9+.-]*://" AND EXISTS "${first}")
    set(archive "${first}")
    file(${algorithm} "${archive}" actual)
    if(NOT actual STREQUAL digest)
      message(FATAL_ERROR "${name}: ${archive} has ${algorithm} ${actual}, expected ${digest}")
    endif()
  else()
    if(download_dir STREQUAL "")
      set(download_dir "${root}/${lower}-tmp")
    endif()
    if(download_name STREQUAL "")
      # The file name ExternalProject derives, so earlier downloads are reused.
      if(first MATCHES [[([^/\?#]+(\.|=)(7z|tar|tar\.bz2|tar\.gz|tar\.xz|tbz2|tgz|txz|zip))([/?#].*)?$]])
        set(download_name "${CMAKE_MATCH_1}")
      else()
        set(download_name "archive.tar")
      endif()
    endif()
    set(archive "${download_dir}/${download_name}")
    set(actual "")
    if(EXISTS "${archive}")
      file(${algorithm} "${archive}" actual)
    endif()
    if(NOT actual STREQUAL digest)
      file(MAKE_DIRECTORY "${download_dir}")
      string(RANDOM LENGTH 8 tag)
      set(errors "")
      foreach(url IN LISTS urls)
        message(STATUS "Downloading ${name} from ${url}")
        file(DOWNLOAD "${url}" "${archive}.part-${tag}" TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        set(actual "")
        if(code EQUAL 0)
          file(${algorithm} "${archive}.part-${tag}" actual)
        endif()
        if(actual STREQUAL digest)
          file(RENAME "${archive}.part-${tag}" "${archive}")
          break()
        endif()
        file(REMOVE "${archive}.part-${tag}")
        list(APPEND errors "${url}: ${status} ${actual}")
      endforeach()
      if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "Could not download ${name} with ${algorithm} ${digest}: ${errors}")
      endif()
    endif()
  endif()

  message(STATUS "Extracting ${name} into the shared source cache")
  string(RANDOM LENGTH 8 tag)
  set(work "${root}/.ex-${lower}-${short}-${tag}")
  file(MAKE_DIRECTORY "${work}")
  if(keep_timestamps)
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${work}")
  else()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${work}" TOUCH)
  endif()
  # As ExternalProject: a single top-level directory is the source root.
  file(GLOB contents LIST_DIRECTORIES true "${work}/*")
  list(REMOVE_ITEM contents "${work}/.DS_Store")
  list(LENGTH contents count)
  set(top "${work}")
  if(count EQUAL 1 AND IS_DIRECTORY "${contents}")
    set(top "${contents}")
  endif()
  # Antivirus scanners briefly hold freshly written files open on Windows.
  foreach(attempt RANGE 30)
    file(RENAME "${top}" "${final}" RESULT renamed)
    if(renamed EQUAL 0)
      break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
  endforeach()
  if(NOT renamed EQUAL 0)
    message(FATAL_ERROR "Could not publish ${name} into ${final}: ${renamed}")
  endif()
  file(WRITE "${complete}" "${algorithm}=${digest}\n")
  if(NOT top STREQUAL work)
    file(REMOVE_RECURSE "${work}")
  endif()
  set(${out} "${final}" PARENT_SCOPE)
endfunction()

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
