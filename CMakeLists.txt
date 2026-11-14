cmake_minimum_required(VERSION 3.28)
# Populate pinned sources directly rather than configuring a sub-build per pin.
if(POLICY CMP0168)
  if(NOT DEFINED CMAKE_POLICY_DEFAULT_CMP0168)
    set(CMAKE_POLICY_DEFAULT_CMP0168 NEW)
  endif()
  cmake_policy(SET CMP0168 "${CMAKE_POLICY_DEFAULT_CMP0168}")
endif()
project(Venture VERSION 0.2.3.7 LANGUAGES C CXX)
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
if(VENTURE_BUILD_RUNTIME OR VENTURE_BUILD_TESTS)
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

# Warnings for Venture's own code (src, tests, tools). Applied per target once
# configuration is done, so dependencies fetched from inside those directories,
# and the JIT (src/jit, Dynarmic and xbyak), keep their own flags.
function(venture_enable_warnings directory)
  cmake_path(RELATIVE_PATH directory BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE relative)
  if(NOT relative MATCHES "^(src|tests|tools)(/|$)" OR relative MATCHES "^src/jit(/|$)")
    return()
  endif()
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
      # -Wmissing-field-initializers is off: OpenXR, Vulkan and Win32 structs are
      # written {XR_TYPE_...} / {.sType = ...} on purpose, the rest zero-filled.
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wno-missing-field-initializers>")
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    venture_enable_warnings("${child}")
  endforeach()
endfunction()
function(venture_enable_project_warnings)
  get_property(children DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    venture_enable_warnings("${child}")
  endforeach()
endfunction()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL venture_enable_project_warnings)
endif()
