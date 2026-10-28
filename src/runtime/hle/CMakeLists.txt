include(FetchContent)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mimalloc
  URL https://github.com/microsoft/mimalloc/archive/fbd8b99c2b828428947d70fdc046bb55609be93e.tar.gz # v2.2.4
  URL_HASH SHA256=083ea3a867ce85d0f552b2333f3629c10f86e29edfdaa944c35e3e0051375b99
  SYSTEM)
FetchContent_MakeAvailable(mimalloc)
# zlib comes from cmake/Dependencies.cmake.
target_compile_definitions(zlibstatic PUBLIC _FILE_OFFSET_BITS=64 _LARGEFILE64_SOURCE=1)
if(WIN32)
  # zconf.h otherwise uses 32-bit MinGW off_t for its nominal 64-bit API.
  target_compile_definitions(zlibstatic PUBLIC z_off_t=__INT64_TYPE__ Z_LARGE64=1)
endif()
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(utf8proc
  URL https://github.com/JuliaStrings/utf8proc/archive/a1b99daa2a3393884220264c927a48ba1251a9c6.tar.gz # v2.10.0
  URL_HASH SHA256=43f09c9d0a2d3fcbb4eba77729dd603b0ac470ccace59a2961f82a8b6323445f
  SYSTEM)
FetchContent_MakeAvailable(utf8proc)

# Both libc parts and future HLE libraries are discovered without editing this list.
file(GLOB VENTURE_HLE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/libc/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/libm/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/liblog/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/libz/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/libstdcpp/*.cpp")
list(APPEND VENTURE_HLE_SOURCES registry.cpp ossdk.cpp)
add_library(venture_hle STATIC ${VENTURE_HLE_SOURCES})
target_compile_options(venture_hle PRIVATE -Wall -Wextra -Wpedantic)
if(WIN32)
  target_sources(venture_hle PRIVATE ../platform/win32/libc_host.cpp ../platform/win32/libc_files.cpp ../platform/win32/libc_net.cpp)
  target_link_libraries(venture_hle PRIVATE bcrypt ws2_32 iphlpapi)
else()
  target_sources(venture_hle PRIVATE ../platform/posix/libc_host.cpp ../platform/posix/libc_files.cpp ../platform/posix/libc_net.cpp)
endif()
target_include_directories(venture_hle PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_include_directories(venture_hle PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(venture_hle PUBLIC venture_common venture_runtime PRIVATE mimalloc-static zlibstatic utf8proc)
