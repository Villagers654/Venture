# Venture's JIT: A32/A64 frontends, IR passes and the x64 backend.
# Its libraries are pinned in cmake/Dependencies.cmake; xbyak (with Venture's
# dual-mapped code buffer support) lives in src/jit/xbyak.
# Boost comes from the pinned header checkout (cmake/Dependencies.cmake), found
# through the FindBoost module.
cmake_policy(SET CMP0167 OLD)
find_package(Boost 1.87 REQUIRED)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64|x64")
  message(FATAL_ERROR "Venture's JIT currently targets x64 hosts")
endif()

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(TSL_ROBIN_MAP_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt mcl robin_map)
add_library(venture_jit_xbyak INTERFACE)
target_include_directories(venture_jit_xbyak SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(xbyak::xbyak ALIAS venture_jit_xbyak)

# The platform exception wrapper selects its corresponding x64 file.
file(GLOB_RECURSE portable_sources CONFIGURE_DEPENDS
  dynarmic/common/*.cpp dynarmic/frontend/*.cpp dynarmic/ir/*.cpp)
list(FILTER portable_sources EXCLUDE REGEX "spin_lock_(arm64|riscv64|loongarch64)\\.cpp$")
# The A64 translator includes its implementation files into one translation unit.
list(FILTER portable_sources EXCLUDE REGEX "/frontend/A64/translate/impl/.*\\.cpp$")
file(GLOB x64_sources CONFIGURE_DEPENDS dynarmic/backend/x64/*.cpp)
list(FILTER x64_sources EXCLUDE REGEX "exception_handler_windows\\.cpp$")
add_library(dynarmic STATIC ${portable_sources} ${x64_sources}
  dynarmic/backend/block_range_information.cpp)
if(WIN32)
  target_sources(dynarmic PRIVATE dynarmic/backend/exception_handler_windows.cpp)
else()
  target_sources(dynarmic PRIVATE dynarmic/backend/exception_handler_posix.cpp)
endif()
target_include_directories(dynarmic SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(dynarmic PUBLIC fmt::fmt Boost::boost xbyak::xbyak venture_common PRIVATE merry::mcl)
target_compile_features(dynarmic PUBLIC cxx_std_23)
target_compile_definitions(dynarmic PRIVATE
  ARCHITECTURE_x86_64=1 DYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1
  DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=1 FMT_USE_USER_DEFINED_LITERALS=1
  XBYAK_STRICT_CHECK_MEM_REG_SIZE=0 XBYAK_OLD_DISP_CHECK=1)
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
  add_executable(venture_dynarmic_tests ${fp_tests}
    tests/decoder_tests.cpp tests/x64_cpu_info.cpp
    tests/A32/test_arm_instructions.cpp tests/A32/test_thumb_instructions.cpp
    tests/A32/test_coprocessor.cpp tests/A32/test_svc.cpp
    tests/A64/a64.cpp tests/A64/fibonacci.cpp tests/A64/fp_min_max.cpp
    tests/A64/misaligned_page_table.cpp tests/A64/test_invalidation.cpp
    tests/A64/real_world.cpp tests/native/preserve_xmm.cpp)
  target_link_libraries(venture_dynarmic_tests PRIVATE dynarmic Catch2::Catch2WithMain merry::oaknut merry::mcl)
  if(WIN32)
    target_link_options(venture_dynarmic_tests PRIVATE -municode)
  endif()

endif()
