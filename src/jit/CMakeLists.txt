# Venture's JIT: A32/A64 frontends, IR passes, and the backend of the host
# architecture: x64 (Windows and Linux x64) or arm64 (Linux arm64, where the
# arm64 backend translates A32 guests and is the A64 fallback).
# Its libraries are pinned in cmake/Dependencies.cmake; xbyak (with Venture's
# dual-mapped code buffer support) lives in src/jit/xbyak.
# Boost comes from the pinned header checkout (cmake/Dependencies.cmake), found
# through the FindBoost module.
cmake_policy(SET CMP0167 OLD)
find_package(Boost 1.87 REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64|x64")
  set(VENTURE_JIT_ARCH x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(VENTURE_JIT_ARCH arm64)
else()
  message(FATAL_ERROR "Venture's JIT targets x64 and arm64 hosts")
endif()
set(VENTURE_JIT_ARCH ${VENTURE_JIT_ARCH} PARENT_SCOPE)

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(TSL_ROBIN_MAP_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt mcl robin_map)
add_library(venture_jit_xbyak INTERFACE)
target_include_directories(venture_jit_xbyak SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(xbyak::xbyak ALIAS venture_jit_xbyak)

# The platform exception wrapper selects its corresponding x64 file.
file(GLOB_RECURSE portable_sources CONFIGURE_DEPENDS
  dynarmic/common/*.cpp dynarmic/frontend/*.cpp dynarmic/ir/*.cpp)
# Each backend brings its own spin lock (below).
list(FILTER portable_sources EXCLUDE REGEX "spin_lock_(x64|arm64|riscv64|loongarch64)\\.cpp$")
# The A64 translator includes its implementation files into one translation unit.
list(FILTER portable_sources EXCLUDE REGEX "/frontend/A64/translate/impl/.*\\.cpp$")
if(VENTURE_JIT_ARCH STREQUAL "x64")
  file(GLOB backend_sources CONFIGURE_DEPENDS dynarmic/backend/x64/*.cpp)
  list(FILTER backend_sources EXCLUDE REGEX "exception_handler_windows\\.cpp$")
  list(APPEND backend_sources dynarmic/common/spin_lock_x64.cpp)
else()
  FetchContent_MakeAvailable(oaknut)
  file(GLOB backend_sources CONFIGURE_DEPENDS dynarmic/backend/arm64/*.cpp)
  list(APPEND backend_sources dynarmic/common/spin_lock_arm64.cpp)
endif()
add_library(dynarmic STATIC ${portable_sources} ${backend_sources}
  dynarmic/backend/block_range_information.cpp dynarmic/backend/perf_map.cpp
  dynarmic/backend/code_stats.cpp compat/common/assert.cpp)
if(WIN32)
  target_sources(dynarmic PRIVATE dynarmic/backend/exception_handler_windows.cpp)
else()
  target_sources(dynarmic PRIVATE dynarmic/backend/exception_handler_posix.cpp)
endif()
target_include_directories(dynarmic SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(dynarmic PUBLIC fmt::fmt Boost::boost venture_common PRIVATE merry::mcl)
target_compile_features(dynarmic PUBLIC cxx_std_23)
target_compile_definitions(dynarmic PRIVATE
  DYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1
  DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=1 FMT_USE_USER_DEFINED_LITERALS=1)
if(VENTURE_JIT_ARCH STREQUAL "x64")
  target_link_libraries(dynarmic PUBLIC xbyak::xbyak)
  target_compile_definitions(dynarmic PRIVATE ARCHITECTURE_x86_64=1
    XBYAK_STRICT_CHECK_MEM_REG_SIZE=0 XBYAK_OLD_DISP_CHECK=1)
else()
  target_link_libraries(dynarmic PUBLIC merry::oaknut)
  target_compile_definitions(dynarmic PRIVATE ARCHITECTURE_arm64=1)
endif()
if(WIN32)
  target_compile_definitions(dynarmic PRIVATE FMT_USE_WINDOWS_H=0)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(dynarmic PRIVATE -fbracket-depth=1024)
endif()

if(VENTURE_BUILD_TESTS)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  # As for doctest (cmake/Dependencies.cmake): never preempt the fastmem handler.
  set(CATCH_CONFIG_NO_POSIX_SIGNALS ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(catch2 oaknut)
  file(GLOB fp_tests CONFIGURE_DEPENDS tests/fp/*.cpp)
  # x64-only tests: the x64 backend's CPU features, portable blocks, code
  # allocation failures and XMM preservation.
  if(VENTURE_JIT_ARCH STREQUAL "x64")
    set(backend_tests tests/x64_cpu_info.cpp tests/A64/portable_blocks.cpp
      tests/A64/code_allocation.cpp tests/native/preserve_xmm.cpp)
  else()
    # arm64 backend: LSE atomics, fast SVCs, host FP environment, precise
    # aborts, exclusives (adapted from the Joseph-DiGiovanni/Venture fork).
    set(backend_tests tests/A64/arm64_backend.cpp)
  endif()
  add_executable(venture_dynarmic_tests ${fp_tests}
    tests/decoder_tests.cpp
    tests/A32/test_arm_instructions.cpp tests/A32/test_thumb_instructions.cpp
    tests/A32/test_coprocessor.cpp tests/A32/test_svc.cpp
    tests/A64/a64.cpp tests/A64/fibonacci.cpp tests/A64/fp_min_max.cpp
    tests/A64/misaligned_page_table.cpp tests/A64/test_invalidation.cpp
    tests/A64/real_world.cpp tests/A64/translate_any.cpp ${backend_tests})
  target_link_libraries(venture_dynarmic_tests PRIVATE dynarmic Catch2::Catch2WithMain merry::oaknut merry::mcl)
  if(WIN32)
    target_link_options(venture_dynarmic_tests PRIVATE -municode)
  endif()

endif()
