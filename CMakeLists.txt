cmake_minimum_required(VERSION 3.28)
# Populate pinned sources directly rather than configuring a sub-build per pin.
if(POLICY CMP0168)
  if(NOT DEFINED CMAKE_POLICY_DEFAULT_CMP0168)
    set(CMAKE_POLICY_DEFAULT_CMP0168 NEW)
  endif()
  cmake_policy(SET CMP0168 "${CMAKE_POLICY_DEFAULT_CMP0168}")
endif()
project(Venture VERSION 0.1.1.1 LANGUAGES C CXX)
# All owned generators share this interpreter and its imported target.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

option(VENTURE_BUILD_LAUNCHER "Build the native launcher" ON)
option(VENTURE_BUILD_RUNTIME "Build the game runtime" ON)
option(VENTURE_BUILD_TESTS "Build tests" ON)
option(VENTURE_INSTALLED_GAME_TESTS "Register optional tests that scan installed games" OFF)
option(VENTURE_BUILD_ANGLE_SMOKE "Build the native ANGLE Vulkan callback/render smoke test" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  enable_language(RC)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  # Ship self-contained executables: libc++/libunwind linked statically, UCRT from the OS.
  add_link_options(-static)
else()
  # A32 identity guests reserve low 4 GiB, so all host code/data must be PIE.
  # Set before dependency targets so their static archives are PIC as well.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  # CMP0083: executables link as PIE only after this check (tests included).
  include(CheckPIESupported)
  check_pie_supported(LANGUAGES C CXX)
endif()

# Keep compiler-generated file names relocatable, including linked dependencies.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,ASM>:-ffile-prefix-map=${PROJECT_SOURCE_DIR}=.>"
    "$<$<COMPILE_LANGUAGE:C,CXX,ASM>:-ffile-prefix-map=${PROJECT_SOURCE_DIR}/.deps=deps>")
endif()
include(cmake/AgentCache.cmake)
include(cmake/HostLinkPool.cmake)
if(VENTURE_BUILD_RUNTIME)
  include(cmake/FFmpegDependency.cmake)
endif()
include(cmake/ProbeCache.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(src/common)
if(VENTURE_BUILD_LAUNCHER OR VENTURE_BUILD_RUNTIME OR VENTURE_BUILD_TESTS)
  add_subdirectory(src/runtime/hle/ovrplatform)
endif()
if(VENTURE_BUILD_RUNTIME)
  add_subdirectory(src/runtime)
endif()
if(VENTURE_BUILD_LAUNCHER)
  add_subdirectory(src/launcher)
endif()
if(VENTURE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(VENTURE_BUILD_ANGLE_SMOKE)
  add_subdirectory(tools/angle)
endif()

include(cmake/ReleaseLayout.cmake)
