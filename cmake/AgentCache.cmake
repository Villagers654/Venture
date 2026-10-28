# Opt-in: product presets retain ordinary compiler execution.
option(VENTURE_AGENT_CACHE "Use shared ccache for host compilation" OFF)
if(NOT VENTURE_AGENT_CACHE)
  return()
endif()
if(WIN32)
  file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Venture/dev-cache" _venture_dev_cache)
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(_venture_dev_cache "$ENV{XDG_CACHE_HOME}/Venture/dev-cache")
else()
  set(_venture_dev_cache "$ENV{HOME}/.cache/Venture/dev-cache")
endif()
set(VENTURE_AGENT_CACHE_ROOT "${_venture_dev_cache}" CACHE PATH "Shared agent build caches")
set(VENTURE_CCACHE_MAX_SIZE "20GiB" CACHE STRING "Maximum shared compiler-cache size")
find_program(VENTURE_CCACHE_PROGRAM NAMES ccache
  HINTS "${CMAKE_SOURCE_DIR}/.deps/ccache" "${CMAKE_SOURCE_DIR}/.deps/ccache/bin" REQUIRED)
set(_venture_launcher "${VENTURE_CCACHE_PROGRAM}"
  "cache_dir=${VENTURE_AGENT_CACHE_ROOT}/ccache"
  "base_dir=${CMAKE_SOURCE_DIR}" "hash_dir=false"
  "max_size=${VENTURE_CCACHE_MAX_SIZE}" "compiler_check=content" "direct_mode=true")
set(CMAKE_C_COMPILER_LAUNCHER "${_venture_launcher}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${_venture_launcher}")
# Keep the compilation-directory field relocatable before disabling cwd hashing.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${VENTURE_AGENT_CACHE_ROOT}=dev-cache>")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=.>")
endif()
