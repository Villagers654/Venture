# Linux arm64 (the Steam Frame and any later arm64 Linux headset), cross-built
# from a Windows or x64 Linux host. The target is the Linux release baseline's
# (linux-baseline.cmake): glibc 2.35, static libc++/libc++abi/libgcc/libatomic,
# archive symbols kept local. The sysroot (tools/linux_arm64/sysroot.py) is
# pinned Ubuntu 22.04 arm64 packages plus libc++ from LLVM's arm64 release.
# Native arm64 hosts (the CI job) use the linux-baseline preset instead.
cmake_minimum_required(VERSION 3.28)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(VENTURE_LINUX_ARM64_TRIPLE aarch64-linux-gnu)

if(NOT VENTURE_LINUX_ARM64_SYSROOT AND DEFINED ENV{VENTURE_LINUX_ARM64_SYSROOT})
  set(VENTURE_LINUX_ARM64_SYSROOT "$ENV{VENTURE_LINUX_ARM64_SYSROOT}")
endif()
if(NOT VENTURE_LINUX_ARM64_SYSROOT)
  get_filename_component(_venture_source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  set(VENTURE_LINUX_ARM64_SYSROOT "${_venture_source}/.deps/linux-arm64-sysroot")
endif()
if(NOT EXISTS "${VENTURE_LINUX_ARM64_SYSROOT}/.venture-sysroot.ok")
  message(FATAL_ERROR "Missing the Linux arm64 sysroot: run py -3 tools/linux_arm64/sysroot.py, then configure again.")
endif()
# try_compile projects load this file again with the same sysroot.
set(ENV{VENTURE_LINUX_ARM64_SYSROOT} "${VENTURE_LINUX_ARM64_SYSROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VENTURE_LINUX_ARM64_SYSROOT)

set(CMAKE_SYSROOT "${VENTURE_LINUX_ARM64_SYSROOT}")
set(CMAKE_C_COMPILER_TARGET ${VENTURE_LINUX_ARM64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${VENTURE_LINUX_ARM64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${VENTURE_LINUX_ARM64_TRIPLE})
if(CMAKE_HOST_WIN32)
  # The pinned llvm-mingw clang targets AArch64 Linux too; ld.lld sits beside it.
  get_filename_component(_venture_llvm "${CMAKE_CURRENT_LIST_DIR}/../../.deps/llvm-mingw/bin" ABSOLUTE)
  set(CMAKE_C_COMPILER "${_venture_llvm}/clang.exe")
  set(CMAKE_CXX_COMPILER "${_venture_llvm}/clang++.exe")
  set(CMAKE_ASM_COMPILER "${_venture_llvm}/clang.exe")
  set(CMAKE_AR "${_venture_llvm}/llvm-ar.exe" CACHE FILEPATH "")
  set(CMAKE_RANLIB "${_venture_llvm}/llvm-ranlib.exe" CACHE FILEPATH "")
  set(CMAKE_NM "${_venture_llvm}/llvm-nm.exe" CACHE FILEPATH "")
  set(CMAKE_OBJCOPY "${_venture_llvm}/llvm-objcopy.exe" CACHE FILEPATH "")
  set(CMAKE_STRIP "${_venture_llvm}/llvm-strip.exe" CACHE FILEPATH "")
  set(CMAKE_READELF "${_venture_llvm}/llvm-readelf.exe" CACHE FILEPATH "")
  # Build-time tools (glslang) run on the host.
  set(VENTURE_HOST_C_COMPILER "${_venture_llvm}/clang.exe")
  set(VENTURE_HOST_CXX_COMPILER "${_venture_llvm}/clang++.exe")
  # SDL's Wayland protocol code: the host wayland-scanner sysroot.py builds.
  get_filename_component(_venture_scanner "${CMAKE_CURRENT_LIST_DIR}/../../.deps/linux-arm64-host-tools/wayland-scanner.exe" ABSOLUTE)
  if(NOT EXISTS "${_venture_scanner}")
    message(FATAL_ERROR "No host wayland-scanner: run py -3 tools/linux_arm64/sysroot.py")
  endif()
  set(WAYLAND_SCANNER "${_venture_scanner}" CACHE FILEPATH "")
  # pkgconf for the sysroot's .pc files. On Windows it would relocate each
  # prefix to the .pc file's directory by default; the sysroot prefix is enough.
  set(PKG_CONFIG_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/../../.deps/linux-arm64-host-tools/pkgconf.exe" CACHE FILEPATH "")
  set(PKG_CONFIG_ARGN "--dont-define-prefix" CACHE STRING "")
  set(_venture_pkg_separator ";")
else()
  set(CMAKE_C_COMPILER clang)
  set(CMAKE_CXX_COMPILER clang++)
  set(VENTURE_HOST_C_COMPILER clang)
  set(VENTURE_HOST_CXX_COMPILER clang++)
  set(_venture_pkg_separator ":")
endif()

# libc++ headers: the sysroot's (LLVM's arm64 release), never the host compiler's.
set(_venture_libcxx "${VENTURE_LINUX_ARM64_SYSROOT}/usr/lib/${VENTURE_LINUX_ARM64_TRIPLE}/venture-libcxx")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${VENTURE_LINUX_ARM64_SYSROOT}/usr/include/${VENTURE_LINUX_ARM64_TRIPLE}/c++/v1 -isystem ${VENTURE_LINUX_ARM64_SYSROOT}/usr/include/c++/v1")
set(venture_baseline_link "-stdlib=libc++ -static-libstdc++ -static-libgcc -fuse-ld=lld -Wl,--exclude-libs,ALL -Wl,--as-needed -L${_venture_libcxx} -L${VENTURE_LINUX_ARM64_SYSROOT}/usr/lib/${VENTURE_LINUX_ARM64_TRIPLE}/venture-static")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${venture_baseline_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${venture_baseline_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${venture_baseline_link}")
set(OPENSSL_USE_STATIC_LIBS TRUE)

set(CMAKE_FIND_ROOT_PATH "${VENTURE_LINUX_ARM64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${VENTURE_LINUX_ARM64_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${VENTURE_LINUX_ARM64_SYSROOT}/usr/lib/${VENTURE_LINUX_ARM64_TRIPLE}/pkgconfig${_venture_pkg_separator}${VENTURE_LINUX_ARM64_SYSROOT}/usr/share/pkgconfig")
# Cross-built test binaries run under an AArch64 Linux machine (tools/linux_arm64/vm.py).
if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(CMAKE_CROSSCOMPILING_EMULATOR "" CACHE STRING "")
endif()
